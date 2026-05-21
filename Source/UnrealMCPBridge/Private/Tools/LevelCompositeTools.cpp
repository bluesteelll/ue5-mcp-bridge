// Copyright FatumGame. All Rights Reserved.

#include "LevelCompositeTools.h"

#include "MCPSurfaceRegistry.h"

#include "FMCPDispatchQueue.h"
#include "FMCPJobRegistry.h"
#include "MCPToolHelpers.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPActorPathUtils.h"
#include "Utils/MCPReflection.h"
#include "Utils/MCPWorldContext.h"

#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Math/Rotator.h"
#include "Math/Transform.h"
#include "Math/Vector.h"
#include "ScopedTransaction.h"
#include "UObject/Class.h"
#include "UObject/Object.h"
#include "UObject/ObjectPtr.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// LCO_ prefix per the unity-build symbol-collision pattern. StampIds / MakeError / MakeSuccessObj
	// migrated to FMCPToolHelpers in Phase 3 (Group G3); only the surface-local error-code aliases
	// and domain caps live here.
	constexpr int32 kLCOErrorInvalidParams = kMCPErrorInvalidParams; // -32602
	constexpr int32 kLCOErrorInternal      = kMCPErrorInternal;      // -32603

	// Hard caps per plan v3 D3 / C4 / C5.
	constexpr int32 kLCOMaxActorsPerDump = 5000;
	constexpr int32 kLCOMaxBatchItems    = 1000;

	/**
	 * Build the compact ~200B/actor summary for ``level._full_actor_dump_internal`` and
	 * ``level._find_actors_with_class_internal``. Keeping the entry small lets the 5000-actor cap
	 * still fit comfortably under the 64 MiB frame limit (5000 × 200 ≈ 1 MiB).
	 *
	 * Shape:
	 *   {
	 *     "actor_path":      "/Game/Maps/X.X:PersistentLevel.MyActor_5",
	 *     "class":           "/Script/Engine.StaticMeshActor",
	 *     "label":           "MyStaticMesh",
	 *     "location":        {x,y,z},
	 *     "rotation":        {pitch,yaw,roll},
	 *     "tag_count":       N,
	 *     "component_count": N
	 *   }
	 */
	TSharedRef<FJsonObject> LCO_SerializeActorSummary(const AActor* Actor)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		// Caller filtered nulls; assert defensively.
		check(Actor);

		Obj->SetStringField(TEXT("actor_path"), FMCPActorPathUtils::BuildActorPath(Actor));
		Obj->SetStringField(TEXT("class"),      Actor->GetClass()->GetPathName());
		Obj->SetStringField(TEXT("label"),      Actor->GetActorNameOrLabel());

		const FVector Loc = Actor->GetActorLocation();
		TSharedRef<FJsonObject> LocObj = MakeShared<FJsonObject>();
		LocObj->SetNumberField(TEXT("x"), Loc.X);
		LocObj->SetNumberField(TEXT("y"), Loc.Y);
		LocObj->SetNumberField(TEXT("z"), Loc.Z);
		Obj->SetObjectField(TEXT("location"), LocObj);

		const FRotator Rot = Actor->GetActorRotation();
		TSharedRef<FJsonObject> RotObj = MakeShared<FJsonObject>();
		RotObj->SetNumberField(TEXT("pitch"), Rot.Pitch);
		RotObj->SetNumberField(TEXT("yaw"),   Rot.Yaw);
		RotObj->SetNumberField(TEXT("roll"),  Rot.Roll);
		Obj->SetObjectField(TEXT("rotation"), RotObj);

		Obj->SetNumberField(TEXT("tag_count"), static_cast<double>(Actor->Tags.Num()));

		// Cheap component count — owned (TArray) components only; doesn't traverse the scene graph.
		TArray<UActorComponent*> Comps;
		Actor->GetComponents(Comps);
		Obj->SetNumberField(TEXT("component_count"), static_cast<double>(Comps.Num()));

		return Obj;
	}

	/** Read optional ``map_path`` arg; empty string means "use editor world". */
	void LCO_GetOptionalMapPathArg(const FMCPRequest& Request, FString& OutMapPath)
	{
		OutMapPath.Reset();
		if (Request.Args.IsValid())
		{
			Request.Args->TryGetStringField(TEXT("map_path"), OutMapPath);
		}
	}

	/**
	 * Light helper for vector reads inside the batch_spawn body. Mirrors the ACT_ReadJsonVector
	 * helper in ActorTools.cpp — we don't share the symbol because ActorTools defines it inside its
	 * anonymous namespace.
	 */
	bool LCO_ReadJsonVector(const TSharedPtr<FJsonObject>& Obj, FVector& OutV)
	{
		if (!Obj.IsValid())
		{
			return false;
		}
		double X = 0.0, Y = 0.0, Z = 0.0;
		const bool bHasX = Obj->TryGetNumberField(TEXT("x"), X);
		const bool bHasY = Obj->TryGetNumberField(TEXT("y"), Y);
		const bool bHasZ = Obj->TryGetNumberField(TEXT("z"), Z);
		if (!bHasX && !bHasY && !bHasZ)
		{
			return false;
		}
		OutV = FVector(X, Y, Z);
		return true;
	}

	bool LCO_ReadJsonRotator(const TSharedPtr<FJsonObject>& Obj, FRotator& OutR)
	{
		if (!Obj.IsValid())
		{
			return false;
		}
		double Pitch = 0.0, Yaw = 0.0, Roll = 0.0;
		const bool bHasP = Obj->TryGetNumberField(TEXT("pitch"), Pitch);
		const bool bHasY = Obj->TryGetNumberField(TEXT("yaw"),   Yaw);
		const bool bHasR = Obj->TryGetNumberField(TEXT("roll"),  Roll);
		if (!bHasP && !bHasY && !bHasR)
		{
			return false;
		}
		OutR = FRotator(Pitch, Yaw, Roll);
		return true;
	}

	/**
	 * Resolve ``class_path`` to a concrete UClass, mirroring the actor.spawn resolver but emitting
	 * per-item error codes (NOT FMCPResponse) so the batch handler can collect failures inline.
	 * On failure populates ``OutErrCode`` + ``OutErrMsg`` and returns nullptr.
	 *
	 * Inside the job body (game thread) so LoadObject is safe.
	 */
	UClass* LCO_ResolveActorClass(const FString& ClassPath, int32& OutErrCode, FString& OutErrMsg)
	{
		if (ClassPath.IsEmpty() || ClassPath[0] != TEXT('/'))
		{
			OutErrCode = kMCPErrorInvalidClassPath;
			OutErrMsg = FString::Printf(
				TEXT("class_path '%s' invalid — must start with '/' (e.g. '/Script/Engine.StaticMeshActor')"),
				*ClassPath);
			return nullptr;
		}
		if (ClassPath.Contains(TEXT("\\")))
		{
			OutErrCode = kMCPErrorInvalidClassPath;
			OutErrMsg = FString::Printf(TEXT("class_path '%s' contains backslash"), *ClassPath);
			return nullptr;
		}
		UClass* Class = LoadObject<UClass>(nullptr, *ClassPath);
		if (!Class && !ClassPath.EndsWith(TEXT("_C")))
		{
			const FString Retry = ClassPath + TEXT("_C");
			Class = LoadObject<UClass>(nullptr, *Retry);
		}
		if (!Class)
		{
			OutErrCode = kMCPErrorClassNotFound;
			OutErrMsg = FString::Printf(
				TEXT("class_path '%s' could not be resolved (LoadObject returned null); ")
				TEXT("Blueprint paths usually need trailing '_C'"),
				*ClassPath);
			return nullptr;
		}
		if (Class->HasAnyClassFlags(CLASS_Abstract))
		{
			OutErrCode = kMCPErrorClassAbstract;
			OutErrMsg = FString::Printf(TEXT("class '%s' is abstract"), *Class->GetPathName());
			return nullptr;
		}
		if (!Class->IsChildOf(AActor::StaticClass()))
		{
			OutErrCode = kMCPErrorWrongClassFamily;
			OutErrMsg = FString::Printf(
				TEXT("class '%s' is not an AActor subclass"), *Class->GetPathName());
			return nullptr;
		}
		return Class;
	}

	/** Build a per-item failure entry: ``{index, error_code, error_message}``. */
	TSharedRef<FJsonObject> LCO_MakeFailureEntry(int32 Index, int32 Code, const FString& Message)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("index"),         static_cast<double>(Index));
		Obj->SetNumberField(TEXT("error_code"),    static_cast<double>(Code));
		Obj->SetStringField(TEXT("error_message"), Message);
		return Obj;
	}
} // namespace

namespace FLevelCompositeTools
{

// ─── level._full_actor_dump_internal (Lane B → SubmitJob → game-thread body) ─────────────────
//
// Lane B sync handler: validate optional ``map_path`` arg + submit job + return ``{job_id}``.
// Job body: enumerate ``Level->Actors`` for the resolved level (persistent if no map_path) and emit
// a 5000-cap-protected list of compact actor summaries.
//
// Args (all optional):
//   - map_path  string — sublevel package path; defaults to editor world's persistent level.
//
// Inner result (after job.result on Succeeded):
//   {
//     "map_path":  "/Game/Maps/X",
//     "actors":    [ {actor_path, class, label, location, rotation, tag_count, component_count}, ... ],
//     "total":     N
//   }
//
// Cap behaviour: when Level->Actors.Num() > 5000, the body sets ErrorMessage and returns null →
// registry transitions job to Failed → AI client sees the cap message on its first job.result poll.
FMCPResponse Tool_FullActorDumpInternal(const FMCPRequest& Request)
{
	// Lane B contract: pure args parsing only (no UObject access on listener thread).
	FString MapPath;
	LCO_GetOptionalMapPathArg(Request, MapPath);

	const FGuid JobIdGuid = FMCPJobRegistry::Get().SubmitJob(
		TEXT("level._full_actor_dump_internal"),
		[MapPathCap = MoveTemp(MapPath)](FMCPJob& Job) -> TSharedPtr<FJsonValue>
		{
			// Game-thread body. Free to touch GEditor / UWorld / actors.
			UWorld* World = FMCPWorldContext::GetEditorWorld();
			if (!World)
			{
				Job.ErrorMessage = TEXT("no editor world available (GEditor missing)");
				return nullptr;
			}

			// Resolve target level. Reject WP maps at the level-resolution layer.
			ULevel* Level = nullptr;
			if (MapPathCap.IsEmpty())
			{
				// Persistent level when no map_path supplied — but if the world is partitioned the
				// downstream UE 5.7 streaming model doesn't expose stable Actors[] for it.
				if (World->IsPartitionedWorld())
				{
					Job.ErrorMessage = FString::Printf(TEXT(
						"editor world is partitioned — Phase 5 will ship wp.* tools for this surface "
						"(no map_path supplied; world='%s')"),
						*FMCPWorldContext::GetWorldPackagePath(World));
					return nullptr;
				}
				Level = World->PersistentLevel;
			}
			else
			{
				bool bWPRejected = false;
				Level = FMCPWorldContext::ResolveLevelByMapPath(World, MapPathCap, /*bRejectPartitioned*/ true, bWPRejected);
				if (bWPRejected)
				{
					Job.ErrorMessage = FString::Printf(TEXT(
						"editor world is partitioned — Phase 5 will ship wp.* tools for this surface (map='%s')"),
						*MapPathCap);
					return nullptr;
				}
				if (!Level)
				{
					Job.ErrorMessage = FString::Printf(
						TEXT("no loaded sublevel matches map_path='%s'"), *MapPathCap);
					return nullptr;
				}
			}
			if (!Level)
			{
				Job.ErrorMessage = TEXT("no persistent level on editor world");
				return nullptr;
			}

			// ULevel::Actors is TArray<TObjectPtr<AActor>> in UE 5.7 — use .Get() per critic C2.
			const TArray<TObjectPtr<AActor>>& All = Level->Actors;
			const int32 Total = All.Num();

			// Cap rejection — surfaced inside body (would require GT to detect at submit time).
			if (Total > kLCOMaxActorsPerDump)
			{
				Job.ErrorMessage = FString::Printf(TEXT(
					"OVERLY_BROAD_QUERY: level has %d actors, exceeds MAX_ACTORS_PER_DUMP=%d; "
					"use level.get_persistent_level_actors with pagination instead"),
					Total, kLCOMaxActorsPerDump);
				return nullptr;
			}

			TArray<TSharedPtr<FJsonValue>> Actors;
			Actors.Reserve(Total);
			Job.Description = FString::Printf(TEXT("Dumping %d actors"), Total);

			for (int32 i = 0; i < Total; ++i)
			{
				// Cooperative cancel at top of loop.
				if (Job.bCancelRequested.load(std::memory_order_acquire))
				{
					Job.ErrorMessage = TEXT("cancelled");
					return nullptr;
				}
				// Progress + description cadence: every 256 entries.
				if ((i & 0xFF) == 0)
				{
					Job.Progress.store(static_cast<float>(i) / FMath::Max(1, Total),
						std::memory_order_release);
					Job.Description = FString::Printf(TEXT("Dumping %d/%d"), i + 1, Total);
				}

				AActor* A = All[i].Get();
				if (!A) { continue; } // sparse arrays contain pending-destroyed entries
				Actors.Add(MakeShared<FJsonValueObject>(LCO_SerializeActorSummary(A)));
			}

			Job.Progress.store(1.0f, std::memory_order_release);

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			if (UPackage* Pkg = Level->GetOutermost())
			{
				Out->SetStringField(TEXT("map_path"), Pkg->GetName());
			}
			else
			{
				Out->SetField(TEXT("map_path"), MakeShared<FJsonValueNull>());
			}
			Out->SetArrayField(TEXT("actors"), Actors);
			Out->SetNumberField(TEXT("total"), static_cast<double>(Actors.Num()));
			return MakeShared<FJsonValueObject>(Out);
		},
		/*bGameThreadRequired*/ true);

	if (!JobIdGuid.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorJobSubmitFailed,
			TEXT("FMCPJobRegistry::SubmitJob refused (shutdown?)"));
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("job_id"), JobIdGuid.ToString(EGuidFormats::DigitsWithHyphens));
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── level._find_actors_with_class_internal (Lane B → SubmitJob → game-thread body) ──────────
//
// Walks ALL loaded levels (or restricted set if level_paths supplied) for actors matching
// ``class_path``. With ``recursive_classes=true`` (default) uses IsA semantics; false = exact
// class. Per-actor summary uses LCO_SerializeActorSummary so the cap math is identical to
// ``_full_actor_dump_internal``.
//
// Args:
//   - class_path        string (required) — /Script/<Module>.<ClassName> OR /Game/.../BP_X.BP_X_C
//   - recursive_classes bool (optional, default true) — IsA semantics; false = exact class match
//   - level_paths       [string] (optional) — restrict search to these sublevel package paths;
//                       absent → walks every loaded level (FActorIterator default)
//
// Inner result:
//   {
//     "class_path":    "/Script/Engine.Light",
//     "actors":        [ ...summary, ... ],
//     "total":         N,
//     "scanned_count": M  // number of candidate actors examined (matches loaded-world size)
//   }
FMCPResponse Tool_FindActorsWithClassInternal(const FMCPRequest& Request)
{
	// Lane B contract: pure args parsing on listener thread; class resolution deferred to body
	// (LoadObject is game-thread-only).
	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kLCOErrorInvalidParams, TEXT("missing args object"));
	}
	FString ClassPath;
	if (!Request.Args->TryGetStringField(TEXT("class_path"), ClassPath) || ClassPath.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kLCOErrorInvalidParams,
			TEXT("missing required string field 'class_path'"));
	}
	bool bRecursive = true;
	Request.Args->TryGetBoolField(TEXT("recursive_classes"), bRecursive);

	// Optional level_paths array — captured for filtering inside body. Pure string copies, no
	// UObject access on listener thread.
	TArray<FString> LevelPaths;
	const TArray<TSharedPtr<FJsonValue>>* LevelArr = nullptr;
	if (Request.Args->TryGetArrayField(TEXT("level_paths"), LevelArr) && LevelArr)
	{
		LevelPaths.Reserve(LevelArr->Num());
		for (const TSharedPtr<FJsonValue>& V : *LevelArr)
		{
			FString S;
			if (V.IsValid() && V->TryGetString(S) && !S.IsEmpty())
			{
				LevelPaths.Add(FMCPWorldContext::NormaliseMapPath(S));
			}
		}
	}

	const FGuid JobIdGuid = FMCPJobRegistry::Get().SubmitJob(
		TEXT("level._find_actors_with_class_internal"),
		[ClassPathCap = MoveTemp(ClassPath), bRecursiveCap = bRecursive, LevelPathsCap = MoveTemp(LevelPaths)]
		(FMCPJob& Job) -> TSharedPtr<FJsonValue>
		{
			UWorld* World = FMCPWorldContext::GetEditorWorld();
			if (!World)
			{
				Job.ErrorMessage = TEXT("no editor world available (GEditor missing)");
				return nullptr;
			}

			// Resolve class. We accept abstract here (filter, not spawn target).
			if (ClassPathCap.IsEmpty() || ClassPathCap[0] != TEXT('/'))
			{
				Job.ErrorMessage = FString::Printf(
					TEXT("class_path '%s' invalid — must start with '/'"), *ClassPathCap);
				return nullptr;
			}
			UClass* TargetClass = LoadObject<UClass>(nullptr, *ClassPathCap);
			if (!TargetClass && !ClassPathCap.EndsWith(TEXT("_C")))
			{
				const FString Retry = ClassPathCap + TEXT("_C");
				TargetClass = LoadObject<UClass>(nullptr, *Retry);
			}
			if (!TargetClass)
			{
				Job.ErrorMessage = FString::Printf(
					TEXT("class_path '%s' could not be resolved (LoadObject returned null)"),
					*ClassPathCap);
				return nullptr;
			}
			if (!TargetClass->IsChildOf(AActor::StaticClass()))
			{
				Job.ErrorMessage = FString::Printf(
					TEXT("class '%s' is not an AActor subclass"), *TargetClass->GetPathName());
				return nullptr;
			}

			// Build the level-name filter set for fast membership checks (FName equality).
			TSet<FName> LevelFilter;
			if (LevelPathsCap.Num() > 0)
			{
				LevelFilter.Reserve(LevelPathsCap.Num());
				for (const FString& S : LevelPathsCap)
				{
					if (!S.IsEmpty())
					{
						LevelFilter.Add(FName(*S));
					}
				}
			}

			// Walk every loaded level in the world. ULevel::Actors is TArray<TObjectPtr<AActor>>.
			const TArray<ULevel*>& AllLevels = World->GetLevels();
			TArray<TSharedPtr<FJsonValue>> Matches;
			Matches.Reserve(64);

			int32 ScannedCount = 0;
			int32 IterCounter = 0; // for progress cadence across levels (FActorIterator-style)

			// First pass: count total candidates so we have a denominator for progress. Cheap —
			// scans Level->Actors lengths.
			int32 GlobalTotal = 0;
			for (ULevel* Level : AllLevels)
			{
				if (!Level) { continue; }
				if (LevelFilter.Num() > 0)
				{
					const FName LevelName = Level->GetOutermost() ? Level->GetOutermost()->GetFName() : NAME_None;
					if (!LevelFilter.Contains(LevelName)) { continue; }
				}
				GlobalTotal += Level->Actors.Num();
			}

			if (GlobalTotal > kLCOMaxActorsPerDump)
			{
				Job.ErrorMessage = FString::Printf(TEXT(
					"OVERLY_BROAD_QUERY: scope contains %d candidate actors, exceeds MAX_ACTORS_PER_DUMP=%d; "
					"use level_paths to narrow OR use actor.find_by_class with pagination"),
					GlobalTotal, kLCOMaxActorsPerDump);
				return nullptr;
			}

			Job.Description = FString::Printf(TEXT("Scanning %d actors for class '%s'"),
				GlobalTotal, *TargetClass->GetPathName());

			// Second pass: enumerate and filter.
			for (ULevel* Level : AllLevels)
			{
				if (!Level) { continue; }
				if (LevelFilter.Num() > 0)
				{
					const FName LevelName = Level->GetOutermost() ? Level->GetOutermost()->GetFName() : NAME_None;
					if (!LevelFilter.Contains(LevelName)) { continue; }
				}

				const TArray<TObjectPtr<AActor>>& LevelActors = Level->Actors;
				const int32 LevelTotal = LevelActors.Num();
				for (int32 i = 0; i < LevelTotal; ++i)
				{
					if (Job.bCancelRequested.load(std::memory_order_acquire))
					{
						Job.ErrorMessage = TEXT("cancelled");
						return nullptr;
					}
					if ((IterCounter & 0xFF) == 0)
					{
						Job.Progress.store(
							static_cast<float>(IterCounter) / FMath::Max(1, GlobalTotal),
							std::memory_order_release);
					}
					++IterCounter;

					AActor* A = LevelActors[i].Get();
					if (!A) { continue; }
					++ScannedCount;

					const bool bMatches = bRecursiveCap
						? A->IsA(TargetClass)
						: (A->GetClass() == TargetClass);
					if (bMatches)
					{
						Matches.Add(MakeShared<FJsonValueObject>(LCO_SerializeActorSummary(A)));
					}
				}
			}

			Job.Progress.store(1.0f, std::memory_order_release);

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetStringField(TEXT("class_path"),   TargetClass->GetPathName());
			Out->SetArrayField(TEXT("actors"),        Matches);
			Out->SetNumberField(TEXT("total"),        static_cast<double>(Matches.Num()));
			Out->SetNumberField(TEXT("scanned_count"),static_cast<double>(ScannedCount));
			return MakeShared<FJsonValueObject>(Out);
		},
		/*bGameThreadRequired*/ true);

	if (!JobIdGuid.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorJobSubmitFailed,
			TEXT("FMCPJobRegistry::SubmitJob refused (shutdown?)"));
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("job_id"), JobIdGuid.ToString(EGuidFormats::DigitsWithHyphens));
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── actor._batch_spawn_internal (Lane B → SubmitJob → game-thread body) ─────────────────────
//
// Spawn up to MAX_BATCH_ITEMS (1000) actors in a single async job. Per-item failure does NOT halt
// the batch — each spawn is independently transactioned and any failure surfaces in failed[].
//
// Args:
//   spawns: [ { class_path: str (required),
//               location?: Vector, rotation?: Rotator, scale?: Vector,
//               label?: str, name?: str }, ... ]
//
// Inner result:
//   {
//     "succeeded": [ {index, actor_path}, ... ],
//     "failed":    [ {index, error_code, error_message}, ... ],
//     "total":     N
//   }
//
// PIE-guard placement: inside body. State may transition between submit and execution.
// Per-item FScopedTransaction: yes (per plan v3 Lesson 4). ~500µs overhead × 1000 worst-case = 500ms.
FMCPResponse Tool_BatchSpawnInternal(const FMCPRequest& Request)
{
	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kLCOErrorInvalidParams, TEXT("missing args object"));
	}
	const TArray<TSharedPtr<FJsonValue>>* SpawnsArr = nullptr;
	if (!Request.Args->TryGetArrayField(TEXT("spawns"), SpawnsArr) || !SpawnsArr || SpawnsArr->Num() == 0)
	{
		return FMCPToolHelpers::MakeError(Request, kLCOErrorInvalidParams,
			TEXT("missing or empty required array field 'spawns'"));
	}
	if (SpawnsArr->Num() > kLCOMaxBatchItems)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInputTooLarge,
			FString::Printf(
				TEXT("spawns.length=%d exceeds MAX_BATCH_ITEMS=%d — split into smaller batches"),
				SpawnsArr->Num(), kLCOMaxBatchItems));
	}

	// Copy the array out so we own it through the job lifetime. Spawn-arg objects are TSharedPtr,
	// so capturing the array of TSharedPtr<FJsonValue> by MoveTemp is cheap and keeps the originals
	// alive for the body to inspect.
	TArray<TSharedPtr<FJsonValue>> SpawnsCopy = *SpawnsArr;

	const FGuid JobIdGuid = FMCPJobRegistry::Get().SubmitJob(
		TEXT("actor._batch_spawn_internal"),
		[Spawns = MoveTemp(SpawnsCopy)](FMCPJob& Job) -> TSharedPtr<FJsonValue>
		{
			// PIE-guard inside body — PIE state can transition between listener-side submit and
			// game-thread execution.
			if (FMCPWorldContext::IsPIEActive())
			{
				Job.ErrorMessage = kMCPMessagePIEActive;
				return nullptr;
			}

			UWorld* World = FMCPWorldContext::GetEditorWorld();
			if (!World)
			{
				Job.ErrorMessage = TEXT("no editor world available (GEditor missing)");
				return nullptr;
			}

			const int32 Total = Spawns.Num();
			Job.Description = FString::Printf(TEXT("Batch spawn 0/%d"), Total);

			TArray<TSharedPtr<FJsonValue>> Succeeded;
			TArray<TSharedPtr<FJsonValue>> Failed;
			Succeeded.Reserve(Total);
			Failed.Reserve(8);

			for (int32 i = 0; i < Total; ++i)
			{
				if (Job.bCancelRequested.load(std::memory_order_acquire))
				{
					Job.ErrorMessage = TEXT("cancelled");
					return nullptr;
				}
				if ((i & 0xFF) == 0)
				{
					Job.Progress.store(static_cast<float>(i) / FMath::Max(1, Total),
						std::memory_order_release);
					Job.Description = FString::Printf(TEXT("Batch spawn %d/%d"), i + 1, Total);
				}

				const TSharedPtr<FJsonValue>& V = Spawns[i];
				if (!V.IsValid() || V->Type != EJson::Object)
				{
					Failed.Add(MakeShared<FJsonValueObject>(LCO_MakeFailureEntry(
						i, kLCOErrorInvalidParams,
						TEXT("spawn entry is not a JSON object"))));
					continue;
				}
				const TSharedPtr<FJsonObject>& Item = V->AsObject();
				FString ClassPath;
				if (!Item->TryGetStringField(TEXT("class_path"), ClassPath) || ClassPath.IsEmpty())
				{
					Failed.Add(MakeShared<FJsonValueObject>(LCO_MakeFailureEntry(
						i, kLCOErrorInvalidParams,
						TEXT("spawn entry missing required string 'class_path'"))));
					continue;
				}

				int32 ErrCode = 0;
				FString ErrMsg;
				UClass* Class = LCO_ResolveActorClass(ClassPath, ErrCode, ErrMsg);
				if (!Class)
				{
					Failed.Add(MakeShared<FJsonValueObject>(LCO_MakeFailureEntry(i, ErrCode, ErrMsg)));
					continue;
				}

				// Optional transform fields.
				FVector Loc(0, 0, 0);
				FRotator Rot(0, 0, 0);
				FVector Scale(1, 1, 1);
				bool bHasScale = false;
				const TSharedPtr<FJsonObject>* LocObj = nullptr;
				if (Item->TryGetObjectField(TEXT("location"), LocObj) && LocObj && LocObj->IsValid())
				{
					LCO_ReadJsonVector(*LocObj, Loc);
				}
				const TSharedPtr<FJsonObject>* RotObj = nullptr;
				if (Item->TryGetObjectField(TEXT("rotation"), RotObj) && RotObj && RotObj->IsValid())
				{
					LCO_ReadJsonRotator(*RotObj, Rot);
				}
				const TSharedPtr<FJsonObject>* ScaleObj = nullptr;
				if (Item->TryGetObjectField(TEXT("scale"), ScaleObj) && ScaleObj && ScaleObj->IsValid())
				{
					bHasScale = LCO_ReadJsonVector(*ScaleObj, Scale);
				}

				FActorSpawnParameters Params;
				FString DesiredName;
				if (Item->TryGetStringField(TEXT("name"), DesiredName) && !DesiredName.IsEmpty())
				{
					Params.Name = FName(*DesiredName);
					Params.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Requested;
				}
				FString DesiredLabel;
#if WITH_EDITOR
				if (Item->TryGetStringField(TEXT("label"), DesiredLabel) && !DesiredLabel.IsEmpty())
				{
					Params.InitialActorLabel = FStringView(DesiredLabel);
				}
#endif

				// Per-item transaction per plan v3 Lesson 4 — allows undo of individual rows from a batch.
				const FScopedTransaction Transaction(LOCTEXT("BatchSpawnRow", "MCP: batch spawn actor"));

				const FTransform SpawnTransform(Rot.Quaternion(), Loc,
					bHasScale ? Scale : FVector::OneVector);
				AActor* NewActor = World->SpawnActor(Class, &SpawnTransform, Params);
				if (!NewActor)
				{
					Failed.Add(MakeShared<FJsonValueObject>(LCO_MakeFailureEntry(
						i, kLCOErrorInternal,
						FString::Printf(
							TEXT("SpawnActor('%s') returned null — collision rejected? construction-script failed?"),
							*Class->GetPathName()))));
					continue;
				}
#if WITH_EDITOR
				if (!DesiredLabel.IsEmpty() && NewActor->GetActorLabel() != DesiredLabel)
				{
					NewActor->SetActorLabel(DesiredLabel);
				}
#endif

				TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetNumberField(TEXT("index"),      static_cast<double>(i));
				Entry->SetStringField(TEXT("actor_path"), FMCPActorPathUtils::BuildActorPath(NewActor));
				Succeeded.Add(MakeShared<FJsonValueObject>(Entry));
			}

			Job.Progress.store(1.0f, std::memory_order_release);

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetArrayField(TEXT("succeeded"), Succeeded);
			Out->SetArrayField(TEXT("failed"),    Failed);
			Out->SetNumberField(TEXT("total"),    static_cast<double>(Total));
			return MakeShared<FJsonValueObject>(Out);
		},
		/*bGameThreadRequired*/ true);

	if (!JobIdGuid.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorJobSubmitFailed,
			TEXT("FMCPJobRegistry::SubmitJob refused (shutdown?)"));
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("job_id"), JobIdGuid.ToString(EGuidFormats::DigitsWithHyphens));
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── actor._batch_destroy_internal (Lane B → SubmitJob → game-thread body) ───────────────────
//
// Destroy up to MAX_BATCH_ITEMS (1000) actors. Idempotent per item — actor_paths that resolve to
// nothing (already destroyed) count as success.
//
// Args:
//   actor_paths: [string]  — required, non-empty, length <= 1000
//
// Inner result:
//   {
//     "succeeded": [ {index, actor_path}, ... ],
//     "failed":    [ {index, error_code, error_message}, ... ],
//     "total":     N
//   }
FMCPResponse Tool_BatchDestroyInternal(const FMCPRequest& Request)
{
	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kLCOErrorInvalidParams, TEXT("missing args object"));
	}
	const TArray<TSharedPtr<FJsonValue>>* PathsArr = nullptr;
	if (!Request.Args->TryGetArrayField(TEXT("actor_paths"), PathsArr) || !PathsArr || PathsArr->Num() == 0)
	{
		return FMCPToolHelpers::MakeError(Request, kLCOErrorInvalidParams,
			TEXT("missing or empty required array field 'actor_paths'"));
	}
	if (PathsArr->Num() > kLCOMaxBatchItems)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInputTooLarge,
			FString::Printf(
				TEXT("actor_paths.length=%d exceeds MAX_BATCH_ITEMS=%d — split into smaller batches"),
				PathsArr->Num(), kLCOMaxBatchItems));
	}

	// String-only copy on the listener thread; resolution deferred to body.
	TArray<FString> Paths;
	Paths.Reserve(PathsArr->Num());
	for (const TSharedPtr<FJsonValue>& V : *PathsArr)
	{
		FString S;
		if (!V.IsValid() || !V->TryGetString(S))
		{
			return FMCPToolHelpers::MakeError(Request, kLCOErrorInvalidParams,
				TEXT("actor_paths: expected array of strings"));
		}
		Paths.Add(S);
	}

	const FGuid JobIdGuid = FMCPJobRegistry::Get().SubmitJob(
		TEXT("actor._batch_destroy_internal"),
		[PathsCap = MoveTemp(Paths)](FMCPJob& Job) -> TSharedPtr<FJsonValue>
		{
			if (FMCPWorldContext::IsPIEActive())
			{
				Job.ErrorMessage = kMCPMessagePIEActive;
				return nullptr;
			}

			const int32 Total = PathsCap.Num();
			Job.Description = FString::Printf(TEXT("Batch destroy 0/%d"), Total);

			TArray<TSharedPtr<FJsonValue>> Succeeded;
			TArray<TSharedPtr<FJsonValue>> Failed;
			Succeeded.Reserve(Total);
			Failed.Reserve(8);

			for (int32 i = 0; i < Total; ++i)
			{
				if (Job.bCancelRequested.load(std::memory_order_acquire))
				{
					Job.ErrorMessage = TEXT("cancelled");
					return nullptr;
				}
				if ((i & 0xFF) == 0)
				{
					Job.Progress.store(static_cast<float>(i) / FMath::Max(1, Total),
						std::memory_order_release);
					Job.Description = FString::Printf(TEXT("Batch destroy %d/%d"), i + 1, Total);
				}

				const FString& Path = PathsCap[i];
				AActor* Actor = FMCPActorPathUtils::ResolveActorOrNull(Path, /*bRejectPIE*/ true);
				if (!Actor)
				{
					// Idempotent — already-gone counts as success. Echo the requested path so the
					// caller can correlate.
					TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
					Entry->SetNumberField(TEXT("index"),      static_cast<double>(i));
					Entry->SetStringField(TEXT("actor_path"), Path);
					Entry->SetBoolField(TEXT("was_already_gone"), true);
					Succeeded.Add(MakeShared<FJsonValueObject>(Entry));
					continue;
				}

				// D18 sublevel-visibility gate. Refuse to mutate hidden sublevels.
				const ULevel* Level = Actor->GetLevel();
				if (Level && !Level->bIsVisible)
				{
					Failed.Add(MakeShared<FJsonValueObject>(LCO_MakeFailureEntry(
						i, kMCPErrorLevelNotFound,
						FString::Printf(
							TEXT("actor '%s' owning sublevel '%s' is loaded but not visible — cannot mutate"),
							*Actor->GetName(),
							Level->GetOutermost() ? *Level->GetOutermost()->GetName() : TEXT("?")))));
					continue;
				}

				const FString CanonicalPath = FMCPActorPathUtils::BuildActorPath(Actor);

				const FScopedTransaction Transaction(LOCTEXT("BatchDestroyRow", "MCP: batch destroy actor"));
				UWorld* World = Actor->GetWorld();
				check(World);

				const bool bOk = World->EditorDestroyActor(Actor, /*bShouldModifyLevel*/ true);
				if (!bOk)
				{
					Failed.Add(MakeShared<FJsonValueObject>(LCO_MakeFailureEntry(
						i, kLCOErrorInternal,
						FString::Printf(TEXT("EditorDestroyActor('%s') returned false"),
							*CanonicalPath))));
					continue;
				}

				TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetNumberField(TEXT("index"),      static_cast<double>(i));
				Entry->SetStringField(TEXT("actor_path"), CanonicalPath);
				Entry->SetBoolField(TEXT("was_already_gone"), false);
				Succeeded.Add(MakeShared<FJsonValueObject>(Entry));
			}

			Job.Progress.store(1.0f, std::memory_order_release);

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetArrayField(TEXT("succeeded"), Succeeded);
			Out->SetArrayField(TEXT("failed"),    Failed);
			Out->SetNumberField(TEXT("total"),    static_cast<double>(Total));
			return MakeShared<FJsonValueObject>(Out);
		},
		/*bGameThreadRequired*/ true);

	if (!JobIdGuid.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorJobSubmitFailed,
			TEXT("FMCPJobRegistry::SubmitJob refused (shutdown?)"));
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("job_id"), JobIdGuid.ToString(EGuidFormats::DigitsWithHyphens));
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── actor._batch_set_property_internal (Lane B → SubmitJob → game-thread body) ──────────────
//
// Apply up to MAX_BATCH_ITEMS (1000) property mutations in a single async job. Each mutation is
// {actor_path, property_path, value} and goes through the canonical 3-flag edit-const gate
// (CPF_BlueprintReadOnly | CPF_EditConst | CPF_DisableEditOnInstance) per the Days 4-8 hotfix
// for actor.set_property, then through FMCPWritePropertyScope so PreEditChange / Modify /
// FScopedTransaction / PostEditChangeProperty fire in order.
//
// Args:
//   mutations: [ {actor_path: str, property_path: str, value: any}, ... ]  — length <= 1000
//
// Inner result:
//   {
//     "succeeded": [ {index, actor_path, property_path, value}, ... ],  // value = post-write echo
//     "failed":    [ {index, error_code, error_message}, ... ],
//     "total":     N
//   }
FMCPResponse Tool_BatchSetPropertyInternal(const FMCPRequest& Request)
{
	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kLCOErrorInvalidParams, TEXT("missing args object"));
	}
	const TArray<TSharedPtr<FJsonValue>>* MutArr = nullptr;
	if (!Request.Args->TryGetArrayField(TEXT("mutations"), MutArr) || !MutArr || MutArr->Num() == 0)
	{
		return FMCPToolHelpers::MakeError(Request, kLCOErrorInvalidParams,
			TEXT("missing or empty required array field 'mutations'"));
	}
	if (MutArr->Num() > kLCOMaxBatchItems)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInputTooLarge,
			FString::Printf(
				TEXT("mutations.length=%d exceeds MAX_BATCH_ITEMS=%d — split into smaller batches"),
				MutArr->Num(), kLCOMaxBatchItems));
	}

	// Capture the JSON array verbatim — value field is "any" so we MUST defer all interpretation
	// to the body where FMCPReflection can typecheck against the resolved FProperty.
	TArray<TSharedPtr<FJsonValue>> MutationsCopy = *MutArr;

	const FGuid JobIdGuid = FMCPJobRegistry::Get().SubmitJob(
		TEXT("actor._batch_set_property_internal"),
		[Mutations = MoveTemp(MutationsCopy)](FMCPJob& Job) -> TSharedPtr<FJsonValue>
		{
			if (FMCPWorldContext::IsPIEActive())
			{
				Job.ErrorMessage = kMCPMessagePIEActive;
				return nullptr;
			}

			const int32 Total = Mutations.Num();
			Job.Description = FString::Printf(TEXT("Batch set_property 0/%d"), Total);

			TArray<TSharedPtr<FJsonValue>> Succeeded;
			TArray<TSharedPtr<FJsonValue>> Failed;
			Succeeded.Reserve(Total);
			Failed.Reserve(8);

			for (int32 i = 0; i < Total; ++i)
			{
				if (Job.bCancelRequested.load(std::memory_order_acquire))
				{
					Job.ErrorMessage = TEXT("cancelled");
					return nullptr;
				}
				if ((i & 0xFF) == 0)
				{
					Job.Progress.store(static_cast<float>(i) / FMath::Max(1, Total),
						std::memory_order_release);
					Job.Description = FString::Printf(TEXT("Batch set_property %d/%d"), i + 1, Total);
				}

				const TSharedPtr<FJsonValue>& V = Mutations[i];
				if (!V.IsValid() || V->Type != EJson::Object)
				{
					Failed.Add(MakeShared<FJsonValueObject>(LCO_MakeFailureEntry(
						i, kLCOErrorInvalidParams,
						TEXT("mutation entry is not a JSON object"))));
					continue;
				}
				const TSharedPtr<FJsonObject>& Item = V->AsObject();

				FString ActorPath;
				if (!Item->TryGetStringField(TEXT("actor_path"), ActorPath) || ActorPath.IsEmpty())
				{
					Failed.Add(MakeShared<FJsonValueObject>(LCO_MakeFailureEntry(
						i, kLCOErrorInvalidParams,
						TEXT("mutation entry missing required string 'actor_path'"))));
					continue;
				}
				FString PropertyPath;
				if (!Item->TryGetStringField(TEXT("property_path"), PropertyPath) || PropertyPath.IsEmpty())
				{
					Failed.Add(MakeShared<FJsonValueObject>(LCO_MakeFailureEntry(
						i, kLCOErrorInvalidParams,
						TEXT("mutation entry missing required string 'property_path'"))));
					continue;
				}
				const TSharedPtr<FJsonValue> ValueField = Item->TryGetField(TEXT("value"));
				if (!ValueField.IsValid())
				{
					Failed.Add(MakeShared<FJsonValueObject>(LCO_MakeFailureEntry(
						i, kLCOErrorInvalidParams,
						TEXT("mutation entry missing required field 'value'"))));
					continue;
				}

				AActor* Actor = FMCPActorPathUtils::ResolveActorOrNull(ActorPath, /*bRejectPIE*/ true);
				if (!Actor)
				{
					Failed.Add(MakeShared<FJsonValueObject>(LCO_MakeFailureEntry(
						i, kMCPErrorObjectNotFound,
						FString::Printf(TEXT("actor '%s' could not be resolved"), *ActorPath))));
					continue;
				}

				// D18 sublevel-visibility gate.
				const ULevel* Level = Actor->GetLevel();
				if (Level && !Level->bIsVisible)
				{
					Failed.Add(MakeShared<FJsonValueObject>(LCO_MakeFailureEntry(
						i, kMCPErrorLevelNotFound,
						FString::Printf(
							TEXT("actor '%s' owning sublevel '%s' is loaded but not visible — cannot mutate"),
							*Actor->GetName(),
							Level->GetOutermost() ? *Level->GetOutermost()->GetName() : TEXT("?")))));
					continue;
				}

				UObject* Container = nullptr;
				void* LeafValuePtr = nullptr;
				FProperty* LeafProp = nullptr;
				int32 ErrCode = 0;
				FString ErrMsg;
				if (!FMCPReflection::ResolvePropertyPath(Actor, PropertyPath, Container, LeafValuePtr, LeafProp,
					ErrCode, ErrMsg))
				{
					Failed.Add(MakeShared<FJsonValueObject>(LCO_MakeFailureEntry(i, ErrCode, ErrMsg)));
					continue;
				}

				// Edit-const 2-flag gate (CPF_BlueprintReadOnly dropped 2026-05 — it's runtime BP
				// restriction, not editor-write restriction; editor's Details panel writes to those
				// fine). Per-mutation `bypass_readonly: true` field opts past the remaining
				// CPF_EditConst | CPF_DisableEditOnInstance check.
				bool bBypassReadOnlyPer = false;
				Item->TryGetBoolField(TEXT("bypass_readonly"), bBypassReadOnlyPer);
				const uint64 Flags = LeafProp->PropertyFlags;
				if (!bBypassReadOnlyPer && (Flags & (CPF_EditConst | CPF_DisableEditOnInstance)))
				{
					Failed.Add(MakeShared<FJsonValueObject>(LCO_MakeFailureEntry(
						i, kMCPErrorPropertyAccessDenied,
						FString::Printf(
							TEXT("property '%s' is read-only (CPF flags=%llu); pass mutation.bypass_readonly=true to override"),
							*LeafProp->GetName(),
							static_cast<unsigned long long>(Flags)))));
					continue;
				}

				// Per-item transaction + RAII Pre/Modify/Transaction/Post scope.
				FString WriteErr;
				bool bWriteOk = false;
				{
					FMCPWritePropertyScope Scope(Container, LeafProp,
						LOCTEXT("BatchSetPropertyRow", "MCP: batch set actor property"));
					bWriteOk = FMCPReflection::WritePropertyValueAt(
						LeafProp, LeafValuePtr, ValueField, Container, WriteErr);
				}
				if (!bWriteOk)
				{
					Failed.Add(MakeShared<FJsonValueObject>(LCO_MakeFailureEntry(
						i, kMCPErrorPropertyTypeMismatch,
						FString::Printf(TEXT("write rejected: %s"), *WriteErr))));
					continue;
				}

				// Re-read post-write for round-trip echo.
				TSharedPtr<FJsonValue> EchoValue = FMCPReflection::ReadPropertyValueAt(LeafProp, LeafValuePtr);

				TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetNumberField(TEXT("index"),          static_cast<double>(i));
				Entry->SetStringField(TEXT("actor_path"),     FMCPActorPathUtils::BuildActorPath(Actor));
				Entry->SetStringField(TEXT("property_path"),  PropertyPath);
				Entry->SetField(TEXT("value"), EchoValue);
				Succeeded.Add(MakeShared<FJsonValueObject>(Entry));
			}

			Job.Progress.store(1.0f, std::memory_order_release);

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetArrayField(TEXT("succeeded"), Succeeded);
			Out->SetArrayField(TEXT("failed"),    Failed);
			Out->SetNumberField(TEXT("total"),    static_cast<double>(Total));
			return MakeShared<FJsonValueObject>(Out);
		},
		/*bGameThreadRequired*/ true);

	if (!JobIdGuid.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorJobSubmitFailed,
			TEXT("FMCPJobRegistry::SubmitJob refused (shutdown?)"));
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("job_id"), JobIdGuid.ToString(EGuidFormats::DigitsWithHyphens));
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── Registration ────────────────────────────────────────────────────────────────────────────
//
// All 5 sync handlers are Lane B because the body only does string parsing + a thread-safe
// FMCPJobRegistry::SubmitJob call; the actual work runs in the job body lambda on the game thread.
//
// MUST be Lane B — the Python composites call these via dispatch_internal (TCP loopback) from
// inside FMCPPythonEval::CallPythonTool on the game thread. Lane A would queue back to the same
// game thread that's blocked on socket.recv() → 60s deadlock until socket timeout. Same rationale
// as the Phase 2 Hotfix-3 promotion for asset.* composites.
void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	// Day 11-12: level dump + class-filtered enumeration.
	RegisterTool(TEXT("level._full_actor_dump_internal"),        &Tool_FullActorDumpInternal,        /*Lane B*/ true);
	RegisterTool(TEXT("level._find_actors_with_class_internal"), &Tool_FindActorsWithClassInternal,  /*Lane B*/ true);

	// Day 13: batch spawn + destroy.
	RegisterTool(TEXT("actor._batch_spawn_internal"),            &Tool_BatchSpawnInternal,           /*Lane B*/ true);
	RegisterTool(TEXT("actor._batch_destroy_internal"),          &Tool_BatchDestroyInternal,         /*Lane B*/ true);

	// Day 14: batch property mutation.
	RegisterTool(TEXT("actor._batch_set_property_internal"),     &Tool_BatchSetPropertyInternal,     /*Lane B*/ true);

	UE_LOG(LogMCP, Log,
		TEXT("Phase 3 Days 11-14: registered 5 internal composite handlers (all Lane B, async-job pattern; composites are async-only, return {job_id})"));
}

} // namespace FLevelCompositeTools

#undef LOCTEXT_NAMESPACE

MCP_REGISTER_SURFACE(LevelCompositeTools, &FLevelCompositeTools::Register)
