// Copyright FatumGame. All Rights Reserved.

#include "ActorTools.h"

#include "FMCPDispatchQueue.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPActorPathUtils.h"
#include "Utils/MCPPageCursor.h"
#include "Utils/MCPReflection.h"
#include "Utils/MCPWorldContext.h"

#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Editor/UnrealEdEngine.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Selection.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Math/Quat.h"
#include "Math/Rotator.h"
#include "Math/Transform.h"
#include "Math/Vector.h"
#include "ScopedTransaction.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// ACT_ prefix per the unity-build symbol-collision pattern (MakeError/MakeSuccess clash with
	// UE's global ValueOrError templates).
	constexpr int32 kACTErrorInvalidParams = -32602;
	constexpr int32 kACTErrorInternal      = -32603;

	void ACT_StampIds(const FMCPRequest& Request, FMCPResponse& Response)
	{
		Response.RequestId = Request.RequestId;
		Response.OriginalIdString = Request.OriginalIdString;
	}

	FMCPResponse ACT_MakeError(const FMCPRequest& Request, int32 Code, const FString& Message)
	{
		FMCPResponse R;
		ACT_StampIds(Request, R);
		R.bIsError = true;
		R.ErrorCode = Code;
		R.ErrorMessage = Message;
		return R;
	}

	FMCPResponse ACT_MakeSuccessObj(const FMCPRequest& Request, TSharedPtr<FJsonObject> Result)
	{
		FMCPResponse R;
		ACT_StampIds(Request, R);
		R.bIsError = false;
		R.Result = MakeShared<FJsonValueObject>(MoveTemp(Result));
		return R;
	}

	/** Frozen PIE-mutator refusal (per D10 — smoke asserts both "Phase 5" and "pie." substrings). */
	FMCPResponse ACT_MakePIEError(const FMCPRequest& Request)
	{
		return ACT_MakeError(Request, kMCPErrorPIEActive, kMCPMessagePIEActive);
	}

	// ─── arg parsing helpers ─────────────────────────────────────────────────────────────────────

	/**
	 * Read a Vector value from a JSON object — accepts either ``{x,y,z}`` shorthand OR the canonical
	 * marshalling ``{_kind:"Vector",x,y,z}`` form. Returns true on success; on false, ``OutV`` is
	 * left untouched and the caller must keep its pre-existing default.
	 *
	 * Kept as bool-out-param (NOT TOptional) for consistency with sibling helpers
	 * ``ACT_ReadJsonRotator`` / ``ACT_ReadJsonString``. ``[[nodiscard]]`` forces deliberate
	 * handling — fire-and-forget callers must explicitly ``(void)`` the result.
	 */
	[[nodiscard]] bool ACT_ReadJsonVector(const TSharedPtr<FJsonObject>& Obj, FVector& OutV)
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

	/**
	 * Read a Rotator value from a JSON object — accepts ``{pitch,yaw,roll}`` shorthand OR
	 * ``{_kind:"Rotator",pitch,yaw,roll}``. Returns true on success.
	 */
	bool ACT_ReadJsonRotator(const TSharedPtr<FJsonObject>& Obj, FRotator& OutR)
	{
		if (!Obj.IsValid())
		{
			return false;
		}
		double Pitch = 0.0, Yaw = 0.0, Roll = 0.0;
		const bool bHasP = Obj->TryGetNumberField(TEXT("pitch"), Pitch);
		const bool bHasY = Obj->TryGetNumberField(TEXT("yaw"), Yaw);
		const bool bHasR = Obj->TryGetNumberField(TEXT("roll"), Roll);
		if (!bHasP && !bHasY && !bHasR)
		{
			return false;
		}
		OutR = FRotator(Pitch, Yaw, Roll);
		return true;
	}

	/** Read ``args.actor_path`` field; emit kACTErrorInvalidParams on missing/empty. */
	bool ACT_RequireActorPath(const FMCPRequest& Request, FString& OutPath, FMCPResponse& OutError,
		const TCHAR* FieldName = TEXT("actor_path"))
	{
		if (!Request.Args.IsValid())
		{
			OutError = ACT_MakeError(Request, kACTErrorInvalidParams, TEXT("missing args object"));
			return false;
		}
		if (!Request.Args->TryGetStringField(FieldName, OutPath) || OutPath.IsEmpty())
		{
			OutError = ACT_MakeError(Request, kACTErrorInvalidParams,
				FString::Printf(TEXT("missing required string field '%s'"), FieldName));
			return false;
		}
		return true;
	}

	/**
	 * Resolve actor by path + emit appropriate error. ``bRejectPIE`` is the caller's choice — mutators
	 * pass true (refuse to mutate PIE-world actors); read-only callers pass false. The function
	 * additionally checks D18 sublevel-visibility when ``bCheckSublevelVisible=true``.
	 *
	 * Returns nullptr + populates ``OutError`` on failure. On success returns the actor and leaves
	 * OutError untouched.
	 */
	AActor* ACT_ResolveActorOrError(
		const FMCPRequest& Request,
		const FString& Path,
		bool bRejectPIE,
		bool bCheckSublevelVisible,
		FMCPResponse& OutError)
	{
		bool bAmbig = false;
		FString AmbigHint;
		FString ResolveErr;
		AActor* Actor = FMCPActorPathUtils::ResolveActor(Path, bRejectPIE, bAmbig, AmbigHint, ResolveErr);
		if (!Actor)
		{
			OutError = ACT_MakeError(Request, kMCPErrorObjectNotFound, ResolveErr);
			return nullptr;
		}
		if (bCheckSublevelVisible)
		{
			const ULevel* Level = Actor->GetLevel();
			if (Level && !Level->bIsVisible)
			{
				OutError = ACT_MakeError(Request, kMCPErrorLevelNotFound,
					FString::Printf(
						TEXT("actor '%s' owning sublevel '%s' is loaded but not visible/loaded; cannot mutate"),
						*Actor->GetName(),
						Level->GetOutermost() ? *Level->GetOutermost()->GetName() : TEXT("?")));
				return nullptr;
			}
		}
		return Actor;
	}

	// ─── JSON serialisers ────────────────────────────────────────────────────────────────────────

	TSharedRef<FJsonObject> ACT_VectorToJson(const FVector& V)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("_kind"), TEXT("Vector"));
		Obj->SetNumberField(TEXT("x"), V.X);
		Obj->SetNumberField(TEXT("y"), V.Y);
		Obj->SetNumberField(TEXT("z"), V.Z);
		return Obj;
	}

	TSharedRef<FJsonObject> ACT_RotatorToJson(const FRotator& R)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("_kind"), TEXT("Rotator"));
		Obj->SetNumberField(TEXT("pitch"), R.Pitch);
		Obj->SetNumberField(TEXT("yaw"),   R.Yaw);
		Obj->SetNumberField(TEXT("roll"),  R.Roll);
		return Obj;
	}

	TSharedRef<FJsonObject> ACT_TransformToJson(const FTransform& T)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("_kind"), TEXT("Transform"));
		Obj->SetObjectField(TEXT("translation"), ACT_VectorToJson(T.GetLocation()));
		Obj->SetObjectField(TEXT("rotation"),    ACT_RotatorToJson(T.GetRotation().Rotator()));
		Obj->SetObjectField(TEXT("scale"),       ACT_VectorToJson(T.GetScale3D()));
		return Obj;
	}

	/**
	 * Build a compact component summary for actor.get + actor.list_components responses.
	 *
	 * Shape:
	 *   {
	 *     "name":                "FName string",
	 *     "class":               "/Script/Engine.StaticMeshComponent",
	 *     "is_scene":            bool,
	 *     "parent_component_name": "FName string" | null,
	 *     "relative_location":   Vector | null (scene-only),
	 *     "relative_rotation":   Rotator | null (scene-only),
	 *     "relative_scale":      Vector | null (scene-only)
	 *   }
	 */
	TSharedRef<FJsonObject> ACT_BuildComponentSummary(const UActorComponent* Comp)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (!Comp)
		{
			Obj->SetField(TEXT("name"), MakeShared<FJsonValueNull>());
			return Obj;
		}
		Obj->SetStringField(TEXT("name"), Comp->GetFName().ToString());
		Obj->SetStringField(TEXT("class"), Comp->GetClass()->GetPathName());
		const USceneComponent* Scene = Cast<const USceneComponent>(Comp);
		const bool bIsScene = (Scene != nullptr);
		Obj->SetBoolField(TEXT("is_scene"), bIsScene);

		if (bIsScene)
		{
			if (const USceneComponent* Parent = Scene->GetAttachParent())
			{
				Obj->SetStringField(TEXT("parent_component_name"), Parent->GetFName().ToString());
			}
			else
			{
				Obj->SetField(TEXT("parent_component_name"), MakeShared<FJsonValueNull>());
			}
			Obj->SetObjectField(TEXT("relative_location"), ACT_VectorToJson(Scene->GetRelativeLocation()));
			Obj->SetObjectField(TEXT("relative_rotation"), ACT_RotatorToJson(Scene->GetRelativeRotation()));
			Obj->SetObjectField(TEXT("relative_scale"),    ACT_VectorToJson(Scene->GetRelativeScale3D()));
		}
		else
		{
			Obj->SetField(TEXT("parent_component_name"), MakeShared<FJsonValueNull>());
			Obj->SetField(TEXT("relative_location"),     MakeShared<FJsonValueNull>());
			Obj->SetField(TEXT("relative_rotation"),     MakeShared<FJsonValueNull>());
			Obj->SetField(TEXT("relative_scale"),        MakeShared<FJsonValueNull>());
		}
		return Obj;
	}

	/**
	 * Build the full actor snapshot returned by actor.get.
	 *
	 * Shape:
	 *   {
	 *     "actor_path":  "/Game/...:PersistentLevel.MyActor_5",
	 *     "class":       "/Script/Engine.StaticMeshActor",
	 *     "label":       "MyStaticMesh",
	 *     "name":        "MyActor_5",
	 *     "map_path":    "/Game/Maps/MyMap",
	 *     "folder_path": "Folder/Subfolder" | "",
	 *     "owner_path":  "<actor_path>" | null,
	 *     "is_hidden":   bool,
	 *     "transform":   { _kind:Transform, translation, rotation, scale },
	 *     "tags":        ["foo", "bar"],
	 *     "components":  [ component-summary, ... ]
	 *   }
	 */
	TSharedRef<FJsonObject> ACT_BuildActorSnapshot(const AActor* Actor)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (!Actor)
		{
			return Obj;
		}
		Obj->SetStringField(TEXT("actor_path"), FMCPActorPathUtils::BuildActorPath(Actor));
		Obj->SetStringField(TEXT("class"),      Actor->GetClass()->GetPathName());
		Obj->SetStringField(TEXT("label"),      Actor->GetActorNameOrLabel());
		Obj->SetStringField(TEXT("name"),       Actor->GetFName().ToString());
		if (const ULevel* Level = Actor->GetLevel())
		{
			if (const UPackage* Pkg = Level->GetOutermost())
			{
				Obj->SetStringField(TEXT("map_path"), Pkg->GetName());
			}
		}
		Obj->SetStringField(TEXT("folder_path"), Actor->GetFolderPath().ToString());
		if (AActor* OwnerActor = Actor->GetOwner())
		{
			Obj->SetStringField(TEXT("owner_path"), FMCPActorPathUtils::BuildActorPath(OwnerActor));
		}
		else
		{
			Obj->SetField(TEXT("owner_path"), MakeShared<FJsonValueNull>());
		}
		Obj->SetBoolField(TEXT("is_hidden"), Actor->IsHidden());
		Obj->SetObjectField(TEXT("transform"), ACT_TransformToJson(Actor->GetActorTransform()));

		TArray<TSharedPtr<FJsonValue>> TagArr;
		TagArr.Reserve(Actor->Tags.Num());
		for (const FName& T : Actor->Tags)
		{
			TagArr.Add(MakeShared<FJsonValueString>(T.ToString()));
		}
		Obj->SetArrayField(TEXT("tags"), TagArr);

		TArray<UActorComponent*> Comps;
		Actor->GetComponents(Comps);
		TArray<TSharedPtr<FJsonValue>> CompArr;
		CompArr.Reserve(Comps.Num());
		for (UActorComponent* C : Comps)
		{
			CompArr.Add(MakeShared<FJsonValueObject>(ACT_BuildComponentSummary(C)));
		}
		Obj->SetArrayField(TEXT("components"), CompArr);

		return Obj;
	}

	/**
	 * Build a compact actor summary for find_by_* responses.
	 *
	 * Shape: { actor_path, class, label, name, map_path, location }
	 */
	TSharedRef<FJsonObject> ACT_BuildActorMatchSummary(const AActor* Actor)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (!Actor)
		{
			return Obj;
		}
		Obj->SetStringField(TEXT("actor_path"), FMCPActorPathUtils::BuildActorPath(Actor));
		Obj->SetStringField(TEXT("class"),      Actor->GetClass()->GetPathName());
		Obj->SetStringField(TEXT("label"),      Actor->GetActorNameOrLabel());
		Obj->SetStringField(TEXT("name"),       Actor->GetFName().ToString());
		if (const ULevel* Level = Actor->GetLevel())
		{
			if (const UPackage* Pkg = Level->GetOutermost())
			{
				Obj->SetStringField(TEXT("map_path"), Pkg->GetName());
			}
		}
		Obj->SetObjectField(TEXT("location"), ACT_VectorToJson(Actor->GetActorLocation()));
		return Obj;
	}

	// ─── class resolution (actor.spawn) ──────────────────────────────────────────────────────────

	/**
	 * Resolve ``class_path`` to a concrete UClass, surfacing the Phase 3 Day 4 error families:
	 *   -32023 InvalidClassPath  — syntactically bad (no leading /, backslash, etc.)
	 *   -32020 ClassNotFound     — well-formed path, no class loaded after best-effort autoload
	 *   -32021 ClassAbstract     — class has CLASS_Abstract
	 *   -32022 WrongClassFamily  — class is not an AActor subclass
	 *
	 * Returns the resolved UClass on success; populates ``OutError`` and returns nullptr otherwise.
	 */
	UClass* ACT_ResolveActorClass(const FMCPRequest& Request, const FString& ClassPath,
		FMCPResponse& OutError)
	{
		// Syntactic shape: must start with '/', no backslashes.
		if (ClassPath.IsEmpty() || ClassPath[0] != TEXT('/'))
		{
			OutError = ACT_MakeError(Request, kMCPErrorInvalidClassPath,
				FString::Printf(
					TEXT("class_path '%s' invalid — must start with '/' (e.g. '/Script/Engine.StaticMeshActor' or '/Game/Blueprints/BP_Foo.BP_Foo_C')"),
					*ClassPath));
			return nullptr;
		}
		if (ClassPath.Contains(TEXT("\\")))
		{
			OutError = ACT_MakeError(Request, kMCPErrorInvalidClassPath,
				FString::Printf(TEXT("class_path '%s' contains backslash"), *ClassPath));
			return nullptr;
		}

		// Best-effort autoload — LoadObject pulls Blueprint reference graphs when needed (10ms-15s).
		UClass* Class = LoadObject<UClass>(nullptr, *ClassPath);
		if (!Class)
		{
			// Heuristic: Blueprint paths normally need the trailing ``_C``. Probe once with the
			// suffix to give a friendlier error than "not found" when the user forgot it.
			if (!ClassPath.EndsWith(TEXT("_C")))
			{
				const FString Retry = ClassPath + TEXT("_C");
				Class = LoadObject<UClass>(nullptr, *Retry);
			}
		}
		if (!Class)
		{
			OutError = ACT_MakeError(Request, kMCPErrorClassNotFound,
				FString::Printf(
					TEXT("class_path '%s' could not be resolved to a UClass (LoadObject returned null); ")
					TEXT("for Blueprint paths try with trailing '_C', e.g. '/Game/BP_Foo.BP_Foo_C'"),
					*ClassPath));
			return nullptr;
		}
		if (Class->HasAnyClassFlags(CLASS_Abstract))
		{
			OutError = ACT_MakeError(Request, kMCPErrorClassAbstract,
				FString::Printf(
					TEXT("class '%s' is abstract — pick a concrete subclass"),
					*Class->GetPathName()));
			return nullptr;
		}
		if (!Class->IsChildOf(AActor::StaticClass()))
		{
			OutError = ACT_MakeError(Request, kMCPErrorWrongClassFamily,
				FString::Printf(
					TEXT("class '%s' is not an AActor subclass (got base '%s')"),
					*Class->GetPathName(),
					Class->GetSuperClass() ? *Class->GetSuperClass()->GetPathName() : TEXT("?")));
			return nullptr;
		}
		return Class;
	}

	// ─── pagination helpers (find_by_*) ──────────────────────────────────────────────────────────

	/** Stable lex sort by GetPathName — Phase 2 cursor scheme reused. */
	void ACT_SortByPathName(TArray<TWeakObjectPtr<AActor>>& Actors)
	{
		Actors.StableSort([](const TWeakObjectPtr<AActor>& A, const TWeakObjectPtr<AActor>& B)
		{
			const AActor* ARaw = A.Get();
			const AActor* BRaw = B.Get();
			if (!ARaw && !BRaw) { return false; }
			if (!ARaw) { return true; }
			if (!BRaw) { return false; }
			return ARaw->GetPathName().Compare(BRaw->GetPathName(), ESearchCase::IgnoreCase) < 0;
		});
	}

	/**
	 * Skip-until-past-sentinel slice for a sorted weak-actor array. Sets ``OutNextSentinel`` to the
	 * last entry's GetPathName when there's a next page; empty string when the slice is final.
	 */
	void ACT_SliceActorPage(
		const TArray<TWeakObjectPtr<AActor>>& SortedAll,
		const FString& LastKey,
		int32 PageSize,
		TArray<AActor*>& OutPage,
		FString& OutNextSentinel)
	{
		OutPage.Reset();
		OutNextSentinel.Reset();

		int32 StartIdx = 0;
		if (!LastKey.IsEmpty())
		{
			while (StartIdx < SortedAll.Num())
			{
				const AActor* Cur = SortedAll[StartIdx].Get();
				if (Cur && Cur->GetPathName().Compare(LastKey, ESearchCase::IgnoreCase) > 0)
				{
					break;
				}
				++StartIdx;
			}
		}
		const int32 EndIdxExcl = FMath::Min(SortedAll.Num(), StartIdx + PageSize);
		OutPage.Reserve(EndIdxExcl - StartIdx);
		for (int32 i = StartIdx; i < EndIdxExcl; ++i)
		{
			if (AActor* A = SortedAll[i].Get())
			{
				OutPage.Add(A);
			}
		}
		if (EndIdxExcl < SortedAll.Num() && OutPage.Num() > 0)
		{
			OutNextSentinel = OutPage.Last()->GetPathName();
		}
	}

	/**
	 * Filter-kind discriminator for ``ACT_HashFilter`` — replaces raw 0/1/2 magic ints with
	 * a type-safe enum so the compiler catches accidental int↔enum implicit conversions.
	 * Underlying values are part of the on-the-wire hash contract — DO NOT reorder existing
	 * entries (would invalidate every outstanding page_token).
	 */
	enum class EACTHashFilter : uint8
	{
		Class = 0,
		Label = 1,
		Tag   = 2,
	};

	/** Compute a stable hash for a filter string (class_path / label / tag). */
	uint64 ACT_HashFilter(const FString& A, const FString& B, bool bC, EACTHashFilter Kind)
	{
		const uint32 H1 = GetTypeHash(A);
		const uint32 H2 = GetTypeHash(B);
		const uint32 H3 = bC ? 0x9E3779B9u : 0x12345678u;
		const uint32 H4 = static_cast<uint32>(Kind);
		uint64 Out = (static_cast<uint64>(H1) << 32) ^ H2;
		Out ^= (static_cast<uint64>(H3) << 16);
		Out ^= H4;
		return Out;
	}

	int32 ACT_ClampPageSize(const TSharedPtr<FJsonObject>& Args, const TCHAR* FieldName, int32 Default)
	{
		int32 Out = Default;
		if (Args.IsValid())
		{
			Args->TryGetNumberField(FieldName, Out);
		}
		return FMath::Clamp(Out, 1, 1000);
	}

	bool ACT_DecodeCursor(
		const FMCPRequest& Request,
		const FString& TokenWire,
		uint64 ExpectedFilterHash,
		FMCPPageCursor& OutCursor,
		FMCPResponse& OutError)
	{
		FString DecodeErr;
		if (!FMCPPageCursorUtils::Decode(TokenWire, OutCursor, DecodeErr))
		{
			OutError = ACT_MakeError(Request, kMCPErrorStaleCursor,
				FString::Printf(TEXT("page_token decode failed: %s"), *DecodeErr));
			return false;
		}
		if (!FMCPPageCursorUtils::ValidateAgainstFilter(OutCursor, ExpectedFilterHash))
		{
			OutError = ACT_MakeError(Request, kMCPErrorStaleCursor,
				TEXT("page_token filter_hash mismatch — caller mutated filter between pages; "
					 "restart pagination with page_token=null"));
			return false;
		}
		return true;
	}

	/**
	 * Build a paginated find-result response. Shared by find_by_class / find_by_label / find_by_tag.
	 */
	FMCPResponse ACT_BuildPaginatedResponse(
		const FMCPRequest& Request,
		const TArray<AActor*>& PageActors,
		int32 TotalKnown,
		const FString& NextSentinel,
		uint64 FilterHash)
	{
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Items;
		Items.Reserve(PageActors.Num());
		for (const AActor* A : PageActors)
		{
			Items.Add(MakeShared<FJsonValueObject>(ACT_BuildActorMatchSummary(A)));
		}
		Out->SetArrayField(TEXT("actors"), Items);
		Out->SetNumberField(TEXT("total_known"), static_cast<double>(TotalKnown));

		if (NextSentinel.IsEmpty())
		{
			Out->SetField(TEXT("next_page_token"), MakeShared<FJsonValueNull>());
		}
		else
		{
			FMCPPageCursor Cursor;
			Cursor.FilterHash = FilterHash;
			Cursor.LastAssetPath = NextSentinel;
			Cursor.TotalKnownSnapshot = TotalKnown;
			Out->SetStringField(TEXT("next_page_token"), FMCPPageCursorUtils::Encode(Cursor));
		}
		return ACT_MakeSuccessObj(Request, Out);
	}

	// ─── cycle detection (attach) ────────────────────────────────────────────────────────────────

	/** Maximum attach-chain depth ``ACT_AttachWouldCycle`` will traverse before giving up. */
	constexpr int32 kACTMaxAttachDepth = 256;

	/** Tri-state result for the bounded cycle walk. */
	enum class EACTCycleCheckResult : uint8
	{
		Safe,         // walk completed, ``Child`` not found in ancestor chain
		WouldCycle,   // ``Child`` is already an ancestor of ``NewParent`` — attach forbidden
		DepthExceeded // hit ``kACTMaxAttachDepth`` without terminating — corrupted hierarchy
	};

	/**
	 * Walk parent chain from ``NewParent`` upward; returns ``WouldCycle`` if ``Child`` appears in
	 * the chain, ``DepthExceeded`` if the walk exceeds ``kACTMaxAttachDepth`` (indicating a likely
	 * pre-existing cycle in the actor graph — fail-fast rather than infinite-loop), else ``Safe``.
	 * Used by Tool_Attach to refuse parent reassignments that would create a loop.
	 */
	EACTCycleCheckResult ACT_AttachWouldCycle(const AActor* NewParent, const AActor* Child)
	{
		if (!NewParent || !Child) { return EACTCycleCheckResult::Safe; }
		if (NewParent == Child)   { return EACTCycleCheckResult::WouldCycle; }
		const AActor* Cur = NewParent;
		for (int32 i = 0; i < kACTMaxAttachDepth; ++i)
		{
			if (!Cur) { return EACTCycleCheckResult::Safe; }
			if (Cur == Child) { return EACTCycleCheckResult::WouldCycle; }
			Cur = Cur->GetAttachParentActor();
		}
		// Cap reached without termination — chain is either pathologically deep or already cyclic.
		return EACTCycleCheckResult::DepthExceeded;
	}

	/**
	 * Resolve target world for read operations — PIE world if active (so reads transparently see
	 * PIE actors), else editor world. Returns null on total failure.
	 */
	UWorld* ACT_ResolveReadWorld()
	{
		if (FMCPWorldContext::IsPIEActive())
		{
			return GEditor ? GEditor->PlayWorld : nullptr;
		}
		return FMCPWorldContext::GetEditorWorld();
	}
} // namespace

namespace FActorTools
{

// ─── actor.spawn (mutator — PIE-guarded) ─────────────────────────────────────────────────────
//
// Spawns a new actor in the editor world. Class is autoloaded via LoadObject — Blueprint paths
// drag in reference graphs so latency can hit several hundred ms (acceptable for editor tooling).
//
// Args:
//   class_path             (required) — /Script/<Module>.<ClassName> OR /Game/.../BP_X.BP_X_C
//   location               (optional Vector) — defaults to (0,0,0)
//   rotation               (optional Rotator) — defaults to (0,0,0)
//   scale                  (optional Vector) — defaults to (1,1,1) (only used if no template)
//   name                   (optional FName string) — uniqueness enforced by UE; collision → engine
//                          auto-renames
//   label                  (optional FString) — actor display label (editor outliner)
//   target_level           (optional /Game/Maps/X) — sublevel to spawn into; defaults to persistent
//   template_actor_path    (optional actor_path) — template for FActorSpawnParameters.Template;
//                          MUST live in the same world as the target sublevel (cross-world reject)
//   owner_path             (optional actor_path) — FActorSpawnParameters.Owner; cross-level reject
//   collision_handling     (optional enum string) — Default / AlwaysSpawn / AdjustIfPossibleButAlwaysSpawn
//                          / AdjustIfPossibleButDontSpawnIfColliding / DontSpawnIfColliding
//   spawn_in_folder        (optional outliner folder path)
//   tags                   (optional [string]) — appended to Actor->Tags
//
// Response: { actor_path, class, label, name, map_path, transform }
FMCPResponse Tool_Spawn(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (FMCPWorldContext::IsPIEActive())
	{
		return ACT_MakePIEError(Request);
	}

	if (!Request.Args.IsValid())
	{
		return ACT_MakeError(Request, kACTErrorInvalidParams, TEXT("missing args object"));
	}

	FString ClassPath;
	if (!Request.Args->TryGetStringField(TEXT("class_path"), ClassPath) || ClassPath.IsEmpty())
	{
		return ACT_MakeError(Request, kACTErrorInvalidParams,
			TEXT("missing required string field 'class_path'"));
	}

	FMCPResponse ClassErr;
	UClass* Class = ACT_ResolveActorClass(Request, ClassPath, ClassErr);
	if (!Class)
	{
		return ClassErr;
	}

	UWorld* World = FMCPWorldContext::GetEditorWorld();
	if (!World)
	{
		return ACT_MakeError(Request, kMCPErrorLevelNotFound,
			TEXT("no editor world available (GEditor missing)"));
	}

	// ── optional positional fields ──
	FVector Location(0, 0, 0);
	FRotator Rotation(0, 0, 0);
	FVector Scale(1, 1, 1);
	const TSharedPtr<FJsonObject>* LocObj = nullptr;
	if (Request.Args->TryGetObjectField(TEXT("location"), LocObj) && LocObj && LocObj->IsValid())
	{
		// Default Location remains (0,0,0) if read fails — intentional fall-through.
		(void)ACT_ReadJsonVector(*LocObj, Location);
	}
	const TSharedPtr<FJsonObject>* RotObj = nullptr;
	if (Request.Args->TryGetObjectField(TEXT("rotation"), RotObj) && RotObj && RotObj->IsValid())
	{
		ACT_ReadJsonRotator(*RotObj, Rotation);
	}
	const TSharedPtr<FJsonObject>* ScaleObj = nullptr;
	bool bHasScale = false;
	if (Request.Args->TryGetObjectField(TEXT("scale"), ScaleObj) && ScaleObj && ScaleObj->IsValid())
	{
		bHasScale = ACT_ReadJsonVector(*ScaleObj, Scale);
	}

	// ── spawn parameter assembly ──
	FActorSpawnParameters Params;

	FString DesiredName;
	if (Request.Args->TryGetStringField(TEXT("name"), DesiredName) && !DesiredName.IsEmpty())
	{
		Params.Name = FName(*DesiredName);
		Params.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Requested;
	}

	FString DesiredLabel;
#if WITH_EDITOR
	if (Request.Args->TryGetStringField(TEXT("label"), DesiredLabel) && !DesiredLabel.IsEmpty())
	{
		Params.InitialActorLabel = FStringView(DesiredLabel);
	}
#endif

	// target_level: route to a specific sublevel via OverrideLevel.
	FString TargetLevel;
	if (Request.Args->TryGetStringField(TEXT("target_level"), TargetLevel) && !TargetLevel.IsEmpty())
	{
		ULevel* Sublevel = FMCPWorldContext::ResolveLevelOrNull(World, TargetLevel);
		if (!Sublevel)
		{
			return ACT_MakeError(Request, kMCPErrorLevelNotFound,
				FString::Printf(TEXT("target_level '%s' is not a loaded sublevel"), *TargetLevel));
		}
		if (!Sublevel->bIsVisible)
		{
			return ACT_MakeError(Request, kMCPErrorLevelNotFound,
				FString::Printf(TEXT("target_level '%s' is loaded but not visible — cannot spawn into hidden sublevels"),
					*TargetLevel));
		}
		Params.OverrideLevel = Sublevel;
	}

	// template_actor_path: spawn-using-template (Template == existing actor). MUST live in the same
	// World (cross-world template would corrupt the new instance's outer).
	FString TemplateActorPath;
	if (Request.Args->TryGetStringField(TEXT("template_actor_path"), TemplateActorPath)
		&& !TemplateActorPath.IsEmpty())
	{
		FMCPResponse ResolveErr;
		AActor* TemplateActor = ACT_ResolveActorOrError(
			Request, TemplateActorPath, /*bRejectPIE*/ true, /*bCheckSublevelVisible*/ false, ResolveErr);
		if (!TemplateActor) { return ResolveErr; }
		if (TemplateActor->GetWorld() != World)
		{
			return ACT_MakeError(Request, kMCPErrorLevelNotStreamingEntry,
				FString::Printf(
					TEXT("template_actor_path '%s' lives in a different world than the spawn target — cross-world template rejected"),
					*TemplateActorPath));
		}
		if (!TemplateActor->IsA(Class))
		{
			return ACT_MakeError(Request, kMCPErrorWrongClassFamily,
				FString::Printf(
					TEXT("template_actor_path '%s' is a '%s' but class_path requested '%s' — template class must match"),
					*TemplateActorPath, *TemplateActor->GetClass()->GetPathName(), *Class->GetPathName()));
		}
		Params.Template = TemplateActor;
	}

	// owner_path: FActorSpawnParameters.Owner. Cross-level guard per M7.
	FString OwnerPath;
	if (Request.Args->TryGetStringField(TEXT("owner_path"), OwnerPath) && !OwnerPath.IsEmpty())
	{
		FMCPResponse ResolveErr;
		AActor* OwnerActor = ACT_ResolveActorOrError(
			Request, OwnerPath, /*bRejectPIE*/ true, /*bCheckSublevelVisible*/ false, ResolveErr);
		if (!OwnerActor) { return ResolveErr; }
		if (OwnerActor->GetWorld() != World)
		{
			return ACT_MakeError(Request, kMCPErrorLevelNotStreamingEntry,
				FString::Printf(
					TEXT("owner_path '%s' lives in a different world than the spawn target — cross-world owner rejected"),
					*OwnerPath));
		}
		Params.Owner = OwnerActor;
	}

	// collision_handling: enum-string parse.
	FString CollisionStr;
	if (Request.Args->TryGetStringField(TEXT("collision_handling"), CollisionStr) && !CollisionStr.IsEmpty())
	{
		if (CollisionStr.Equals(TEXT("Default"), ESearchCase::IgnoreCase) ||
			CollisionStr.Equals(TEXT("Undefined"), ESearchCase::IgnoreCase))
		{
			Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::Undefined;
		}
		else if (CollisionStr.Equals(TEXT("AlwaysSpawn"), ESearchCase::IgnoreCase))
		{
			Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		}
		else if (CollisionStr.Equals(TEXT("AdjustIfPossibleButAlwaysSpawn"), ESearchCase::IgnoreCase))
		{
			Params.SpawnCollisionHandlingOverride =
				ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
		}
		else if (CollisionStr.Equals(TEXT("AdjustIfPossibleButDontSpawnIfColliding"), ESearchCase::IgnoreCase))
		{
			Params.SpawnCollisionHandlingOverride =
				ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButDontSpawnIfColliding;
		}
		else if (CollisionStr.Equals(TEXT("DontSpawnIfColliding"), ESearchCase::IgnoreCase))
		{
			Params.SpawnCollisionHandlingOverride =
				ESpawnActorCollisionHandlingMethod::DontSpawnIfColliding;
		}
		else
		{
			return ACT_MakeError(Request, kACTErrorInvalidParams,
				FString::Printf(TEXT("collision_handling '%s' unknown — expected Default / AlwaysSpawn / AdjustIfPossibleButAlwaysSpawn / AdjustIfPossibleButDontSpawnIfColliding / DontSpawnIfColliding"),
					*CollisionStr));
		}
	}

	// Spawn under transaction so undo restores cleanly.
	const FScopedTransaction Transaction(LOCTEXT("ActorSpawn", "MCP: spawn actor"));

	const FTransform SpawnTransform(Rotation.Quaternion(), Location, bHasScale ? Scale : FVector::OneVector);
	AActor* NewActor = World->SpawnActor(Class, &SpawnTransform, Params);
	if (!NewActor)
	{
		return ACT_MakeError(Request, kACTErrorInternal,
			FString::Printf(
				TEXT("World->SpawnActor('%s') returned null — collision rejected? construction-script failed? see editor log"),
				*Class->GetPathName()));
	}

#if WITH_EDITOR
	// Label fallback for SpawnActor paths that don't pick up InitialActorLabel (some BP factories).
	if (!DesiredLabel.IsEmpty() && NewActor->GetActorLabel() != DesiredLabel)
	{
		NewActor->SetActorLabel(DesiredLabel);
	}
#endif

	// Folder: set after spawn (InitialActorLabel is in FActorSpawnParameters but folder is not).
	FString FolderPath;
	if (Request.Args->TryGetStringField(TEXT("spawn_in_folder"), FolderPath) && !FolderPath.IsEmpty())
	{
		NewActor->SetFolderPath(FName(*FolderPath));
	}

	// Tags: append (do not overwrite — engine may have populated some).
	const TArray<TSharedPtr<FJsonValue>>* TagsArr = nullptr;
	if (Request.Args->TryGetArrayField(TEXT("tags"), TagsArr) && TagsArr)
	{
		for (const TSharedPtr<FJsonValue>& V : *TagsArr)
		{
			if (V.IsValid() && V->Type == EJson::String)
			{
				NewActor->Tags.Add(FName(*V->AsString()));
			}
		}
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("actor_path"), FMCPActorPathUtils::BuildActorPath(NewActor));
	Out->SetStringField(TEXT("class"),      NewActor->GetClass()->GetPathName());
	Out->SetStringField(TEXT("label"),      NewActor->GetActorNameOrLabel());
	Out->SetStringField(TEXT("name"),       NewActor->GetFName().ToString());
	if (const ULevel* L = NewActor->GetLevel())
	{
		Out->SetStringField(TEXT("map_path"), L->GetOutermost() ? L->GetOutermost()->GetName() : FString());
	}
	Out->SetObjectField(TEXT("transform"), ACT_TransformToJson(NewActor->GetActorTransform()));
	return ACT_MakeSuccessObj(Request, Out);
}

// ─── actor.destroy (mutator — PIE-guarded) ───────────────────────────────────────────────────
//
// Removes an actor from the editor world. Wraps in transaction so undo restores it.
//
// Args: { actor_path: "..." }
// Response: { destroyed: bool, actor_path: "..." }
FMCPResponse Tool_Destroy(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (FMCPWorldContext::IsPIEActive())
	{
		return ACT_MakePIEError(Request);
	}

	FString Path;
	FMCPResponse PathErr;
	if (!ACT_RequireActorPath(Request, Path, PathErr)) { return PathErr; }

	FMCPResponse ResolveErr;
	AActor* Actor = ACT_ResolveActorOrError(
		Request, Path, /*bRejectPIE*/ true, /*bCheckSublevelVisible*/ true, ResolveErr);
	if (!Actor) { return ResolveErr; }

	const FString CanonicalPath = FMCPActorPathUtils::BuildActorPath(Actor);

	const FScopedTransaction Transaction(LOCTEXT("ActorDestroy", "MCP: destroy actor"));
	UWorld* World = Actor->GetWorld();
	check(World);

	const bool bOk = World->EditorDestroyActor(Actor, /*bShouldModifyLevel*/ true);

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("destroyed"), bOk);
	Out->SetStringField(TEXT("actor_path"), CanonicalPath);
	if (!bOk)
	{
		return ACT_MakeError(Request, kACTErrorInternal,
			FString::Printf(TEXT("EditorDestroyActor('%s') returned false"), *CanonicalPath));
	}
	return ACT_MakeSuccessObj(Request, Out);
}

// ─── actor.duplicate (mutator — PIE-guarded) ─────────────────────────────────────────────────
//
// Spawns a new actor using ``source_actor_path`` as Template. New actor inherits source class +
// component instance data. Optional ``offset_location`` displaces the new actor from the source.
//
// Args:
//   source_actor_path  (required actor_path)
//   label              (optional FString) — applied to the new actor
//   offset_location    (optional Vector) — added to source's world location; defaults to (0,0,0)
//
// Response: { actor_path, class, label, name, map_path, transform }
FMCPResponse Tool_Duplicate(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (FMCPWorldContext::IsPIEActive())
	{
		return ACT_MakePIEError(Request);
	}

	FString SourcePath;
	FMCPResponse PathErr;
	if (!ACT_RequireActorPath(Request, SourcePath, PathErr, TEXT("source_actor_path"))) { return PathErr; }

	FMCPResponse ResolveErr;
	AActor* Source = ACT_ResolveActorOrError(
		Request, SourcePath, /*bRejectPIE*/ true, /*bCheckSublevelVisible*/ true, ResolveErr);
	if (!Source) { return ResolveErr; }

	UWorld* World = Source->GetWorld();
	check(World);

	FVector OffsetLoc(0, 0, 0);
	const TSharedPtr<FJsonObject>* OffsetObj = nullptr;
	if (Request.Args->TryGetObjectField(TEXT("offset_location"), OffsetObj)
		&& OffsetObj && OffsetObj->IsValid())
	{
		// Default OffsetLoc remains (0,0,0) if read fails — intentional fall-through.
		(void)ACT_ReadJsonVector(*OffsetObj, OffsetLoc);
	}

	FActorSpawnParameters Params;
	Params.Template = Source;
	Params.OverrideLevel = Source->GetLevel();

	FString DesiredLabel;
#if WITH_EDITOR
	if (Request.Args->TryGetStringField(TEXT("label"), DesiredLabel) && !DesiredLabel.IsEmpty())
	{
		Params.InitialActorLabel = FStringView(DesiredLabel);
	}
#endif

	const FScopedTransaction Transaction(LOCTEXT("ActorDuplicate", "MCP: duplicate actor"));

	const FTransform NewTransform(
		Source->GetActorQuat(),
		Source->GetActorLocation() + OffsetLoc,
		Source->GetActorScale3D());

	AActor* NewActor = World->SpawnActor(Source->GetClass(), &NewTransform, Params);
	if (!NewActor)
	{
		return ACT_MakeError(Request, kACTErrorInternal,
			FString::Printf(TEXT("SpawnActor for duplicate of '%s' returned null"), *SourcePath));
	}

#if WITH_EDITOR
	if (!DesiredLabel.IsEmpty() && NewActor->GetActorLabel() != DesiredLabel)
	{
		NewActor->SetActorLabel(DesiredLabel);
	}
#endif

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("actor_path"), FMCPActorPathUtils::BuildActorPath(NewActor));
	Out->SetStringField(TEXT("class"),      NewActor->GetClass()->GetPathName());
	Out->SetStringField(TEXT("label"),      NewActor->GetActorNameOrLabel());
	Out->SetStringField(TEXT("name"),       NewActor->GetFName().ToString());
	if (const ULevel* L = NewActor->GetLevel())
	{
		Out->SetStringField(TEXT("map_path"), L->GetOutermost() ? L->GetOutermost()->GetName() : FString());
	}
	Out->SetObjectField(TEXT("transform"), ACT_TransformToJson(NewActor->GetActorTransform()));
	return ACT_MakeSuccessObj(Request, Out);
}

// ─── actor.get (read-only — works in PIE) ────────────────────────────────────────────────────
//
// Returns the full actor snapshot — class, label, name, transform, tags, folder, owner, components.
// Read-only, so works during PIE against the play world.
//
// Args: { actor_path: "..." }
// Response: see ACT_BuildActorSnapshot.
FMCPResponse Tool_Get(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString Path;
	FMCPResponse PathErr;
	if (!ACT_RequireActorPath(Request, Path, PathErr)) { return PathErr; }

	FMCPResponse ResolveErr;
	AActor* Actor = ACT_ResolveActorOrError(
		Request, Path, /*bRejectPIE*/ false, /*bCheckSublevelVisible*/ false, ResolveErr);
	if (!Actor) { return ResolveErr; }

	return ACT_MakeSuccessObj(Request, ACT_BuildActorSnapshot(Actor));
}

// ─── actor.set_transform (mutator — PIE-guarded) ─────────────────────────────────────────────
//
// Sets some or all of the actor's world transform components. Each field is optional — missing
// fields preserve the current value.
//
// Args: { actor_path, location?, rotation?, scale? }
// Response: { ok: true, actor_path, transform: <new full transform> }
FMCPResponse Tool_SetTransform(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (FMCPWorldContext::IsPIEActive())
	{
		return ACT_MakePIEError(Request);
	}

	FString Path;
	FMCPResponse PathErr;
	if (!ACT_RequireActorPath(Request, Path, PathErr)) { return PathErr; }

	FMCPResponse ResolveErr;
	AActor* Actor = ACT_ResolveActorOrError(
		Request, Path, /*bRejectPIE*/ true, /*bCheckSublevelVisible*/ true, ResolveErr);
	if (!Actor) { return ResolveErr; }

	FTransform NewT = Actor->GetActorTransform();
	bool bAnyField = false;

	const TSharedPtr<FJsonObject>* LocObj = nullptr;
	if (Request.Args->TryGetObjectField(TEXT("location"), LocObj) && LocObj && LocObj->IsValid())
	{
		FVector V;
		if (ACT_ReadJsonVector(*LocObj, V))
		{
			NewT.SetLocation(V);
			bAnyField = true;
		}
	}
	const TSharedPtr<FJsonObject>* RotObj = nullptr;
	if (Request.Args->TryGetObjectField(TEXT("rotation"), RotObj) && RotObj && RotObj->IsValid())
	{
		FRotator R;
		if (ACT_ReadJsonRotator(*RotObj, R))
		{
			NewT.SetRotation(R.Quaternion());
			bAnyField = true;
		}
	}
	const TSharedPtr<FJsonObject>* ScaleObj = nullptr;
	if (Request.Args->TryGetObjectField(TEXT("scale"), ScaleObj) && ScaleObj && ScaleObj->IsValid())
	{
		FVector V;
		if (ACT_ReadJsonVector(*ScaleObj, V))
		{
			NewT.SetScale3D(V);
			bAnyField = true;
		}
	}

	if (!bAnyField)
	{
		return ACT_MakeError(Request, kACTErrorInvalidParams,
			TEXT("actor.set_transform: at least one of location / rotation / scale must be provided"));
	}

	const FScopedTransaction Transaction(LOCTEXT("ActorSetTransform", "MCP: set actor transform"));
	Actor->Modify();
	Actor->SetActorTransform(NewT);

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("ok"), true);
	Out->SetStringField(TEXT("actor_path"), FMCPActorPathUtils::BuildActorPath(Actor));
	Out->SetObjectField(TEXT("transform"), ACT_TransformToJson(Actor->GetActorTransform()));
	return ACT_MakeSuccessObj(Request, Out);
}

// ─── actor.set_location (mutator — PIE-guarded) ──────────────────────────────────────────────
//
// Convenience wrapper for the location-only case. Wraps in its own transaction so undo is per-field.
//
// Args: { actor_path, location: Vector }
// Response: { ok: true, actor_path, location: Vector }
FMCPResponse Tool_SetLocation(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (FMCPWorldContext::IsPIEActive())
	{
		return ACT_MakePIEError(Request);
	}

	FString Path;
	FMCPResponse PathErr;
	if (!ACT_RequireActorPath(Request, Path, PathErr)) { return PathErr; }

	const TSharedPtr<FJsonObject>* LocObj = nullptr;
	if (!Request.Args->TryGetObjectField(TEXT("location"), LocObj) || !LocObj || !LocObj->IsValid())
	{
		return ACT_MakeError(Request, kACTErrorInvalidParams,
			TEXT("missing required object field 'location'"));
	}
	FVector NewLoc;
	if (!ACT_ReadJsonVector(*LocObj, NewLoc))
	{
		return ACT_MakeError(Request, kACTErrorInvalidParams,
			TEXT("'location' must contain at least one of x/y/z numeric fields"));
	}

	FMCPResponse ResolveErr;
	AActor* Actor = ACT_ResolveActorOrError(
		Request, Path, /*bRejectPIE*/ true, /*bCheckSublevelVisible*/ true, ResolveErr);
	if (!Actor) { return ResolveErr; }

	const FScopedTransaction Transaction(LOCTEXT("ActorSetLocation", "MCP: set actor location"));
	Actor->Modify();
	Actor->SetActorLocation(NewLoc);

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("ok"), true);
	Out->SetStringField(TEXT("actor_path"), FMCPActorPathUtils::BuildActorPath(Actor));
	Out->SetObjectField(TEXT("location"), ACT_VectorToJson(Actor->GetActorLocation()));
	return ACT_MakeSuccessObj(Request, Out);
}

// ─── actor.set_rotation (mutator — PIE-guarded) ──────────────────────────────────────────────
//
// Args: { actor_path, rotation: Rotator }
// Response: { ok: true, actor_path, rotation: Rotator }
FMCPResponse Tool_SetRotation(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (FMCPWorldContext::IsPIEActive())
	{
		return ACT_MakePIEError(Request);
	}

	FString Path;
	FMCPResponse PathErr;
	if (!ACT_RequireActorPath(Request, Path, PathErr)) { return PathErr; }

	const TSharedPtr<FJsonObject>* RotObj = nullptr;
	if (!Request.Args->TryGetObjectField(TEXT("rotation"), RotObj) || !RotObj || !RotObj->IsValid())
	{
		return ACT_MakeError(Request, kACTErrorInvalidParams,
			TEXT("missing required object field 'rotation'"));
	}
	FRotator NewRot;
	if (!ACT_ReadJsonRotator(*RotObj, NewRot))
	{
		return ACT_MakeError(Request, kACTErrorInvalidParams,
			TEXT("'rotation' must contain at least one of pitch/yaw/roll numeric fields"));
	}

	FMCPResponse ResolveErr;
	AActor* Actor = ACT_ResolveActorOrError(
		Request, Path, /*bRejectPIE*/ true, /*bCheckSublevelVisible*/ true, ResolveErr);
	if (!Actor) { return ResolveErr; }

	const FScopedTransaction Transaction(LOCTEXT("ActorSetRotation", "MCP: set actor rotation"));
	Actor->Modify();
	Actor->SetActorRotation(NewRot);

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("ok"), true);
	Out->SetStringField(TEXT("actor_path"), FMCPActorPathUtils::BuildActorPath(Actor));
	Out->SetObjectField(TEXT("rotation"), ACT_RotatorToJson(Actor->GetActorRotation()));
	return ACT_MakeSuccessObj(Request, Out);
}

// ─── actor.set_scale (mutator — PIE-guarded) ─────────────────────────────────────────────────
//
// Args: { actor_path, scale: Vector }
// Response: { ok: true, actor_path, scale: Vector }
FMCPResponse Tool_SetScale(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (FMCPWorldContext::IsPIEActive())
	{
		return ACT_MakePIEError(Request);
	}

	FString Path;
	FMCPResponse PathErr;
	if (!ACT_RequireActorPath(Request, Path, PathErr)) { return PathErr; }

	const TSharedPtr<FJsonObject>* ScaleObj = nullptr;
	if (!Request.Args->TryGetObjectField(TEXT("scale"), ScaleObj) || !ScaleObj || !ScaleObj->IsValid())
	{
		return ACT_MakeError(Request, kACTErrorInvalidParams,
			TEXT("missing required object field 'scale'"));
	}
	FVector NewScale;
	if (!ACT_ReadJsonVector(*ScaleObj, NewScale))
	{
		return ACT_MakeError(Request, kACTErrorInvalidParams,
			TEXT("'scale' must contain at least one of x/y/z numeric fields"));
	}

	FMCPResponse ResolveErr;
	AActor* Actor = ACT_ResolveActorOrError(
		Request, Path, /*bRejectPIE*/ true, /*bCheckSublevelVisible*/ true, ResolveErr);
	if (!Actor) { return ResolveErr; }

	const FScopedTransaction Transaction(LOCTEXT("ActorSetScale", "MCP: set actor scale"));
	Actor->Modify();
	Actor->SetActorScale3D(NewScale);

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("ok"), true);
	Out->SetStringField(TEXT("actor_path"), FMCPActorPathUtils::BuildActorPath(Actor));
	Out->SetObjectField(TEXT("scale"), ACT_VectorToJson(Actor->GetActorScale3D()));
	return ACT_MakeSuccessObj(Request, Out);
}

// ─── actor.set_label (mutator — PIE-guarded) ─────────────────────────────────────────────────
//
// Sets the actor's outliner display label. The internal FName is unaffected.
//
// Args: { actor_path, label: FString }
// Response: { ok: true, actor_path, label, previous_label }
FMCPResponse Tool_SetLabel(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (FMCPWorldContext::IsPIEActive())
	{
		return ACT_MakePIEError(Request);
	}

	FString Path;
	FMCPResponse PathErr;
	if (!ACT_RequireActorPath(Request, Path, PathErr)) { return PathErr; }

	FString NewLabel;
	if (!Request.Args->TryGetStringField(TEXT("label"), NewLabel))
	{
		return ACT_MakeError(Request, kACTErrorInvalidParams,
			TEXT("missing required string field 'label'"));
	}

	FMCPResponse ResolveErr;
	AActor* Actor = ACT_ResolveActorOrError(
		Request, Path, /*bRejectPIE*/ true, /*bCheckSublevelVisible*/ true, ResolveErr);
	if (!Actor) { return ResolveErr; }

	const FString PrevLabel = Actor->GetActorLabel();

#if WITH_EDITOR
	const FScopedTransaction Transaction(LOCTEXT("ActorSetLabel", "MCP: set actor label"));
	Actor->SetActorLabel(NewLabel);
#endif

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("ok"), true);
	Out->SetStringField(TEXT("actor_path"), FMCPActorPathUtils::BuildActorPath(Actor));
	Out->SetStringField(TEXT("label"), Actor->GetActorLabel());
	Out->SetStringField(TEXT("previous_label"), PrevLabel);
	return ACT_MakeSuccessObj(Request, Out);
}

// ─── actor.set_folder (mutator — PIE-guarded) ────────────────────────────────────────────────
//
// Sets the actor's outliner folder path.
//
// Args: { actor_path, folder_path: FString } (empty string clears the folder)
// Response: { ok: true, actor_path, folder_path, previous_folder_path }
FMCPResponse Tool_SetFolder(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (FMCPWorldContext::IsPIEActive())
	{
		return ACT_MakePIEError(Request);
	}

	FString Path;
	FMCPResponse PathErr;
	if (!ACT_RequireActorPath(Request, Path, PathErr)) { return PathErr; }

	FString FolderPath;
	if (!Request.Args->TryGetStringField(TEXT("folder_path"), FolderPath))
	{
		return ACT_MakeError(Request, kACTErrorInvalidParams,
			TEXT("missing required string field 'folder_path' (empty string OK to clear)"));
	}

	FMCPResponse ResolveErr;
	AActor* Actor = ACT_ResolveActorOrError(
		Request, Path, /*bRejectPIE*/ true, /*bCheckSublevelVisible*/ true, ResolveErr);
	if (!Actor) { return ResolveErr; }

	const FString PrevFolder = Actor->GetFolderPath().ToString();

	const FScopedTransaction Transaction(LOCTEXT("ActorSetFolder", "MCP: set actor folder"));
	Actor->SetFolderPath(FName(*FolderPath));

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("ok"), true);
	Out->SetStringField(TEXT("actor_path"), FMCPActorPathUtils::BuildActorPath(Actor));
	Out->SetStringField(TEXT("folder_path"), Actor->GetFolderPath().ToString());
	Out->SetStringField(TEXT("previous_folder_path"), PrevFolder);
	return ACT_MakeSuccessObj(Request, Out);
}

// ─── actor.attach (mutator — PIE-guarded) ────────────────────────────────────────────────────
//
// Attaches ``child_actor_path`` to ``parent_actor_path`` at optional ``socket_name``.
//
// Args:
//   child_actor_path   (required actor_path)
//   parent_actor_path  (required actor_path)
//   socket_name        (optional FName string)
//   attachment_rule    (optional enum string) — KeepRelative / KeepWorld / SnapToTarget
//                                              (defaults to KeepWorld; bWeldSimulatedBodies=false)
//
// Cross-sublevel attach is REFUSED per M7 — attached children dangle when their parent's sublevel
// unloads, which is a notorious source of editor crashes.
//
// Cycle detection: walks the parent chain to ensure new attachment doesn't create a loop.
//
// Response: { ok: true, child_actor_path, parent_actor_path, socket, rule, previous_parent_path? }
FMCPResponse Tool_Attach(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (FMCPWorldContext::IsPIEActive())
	{
		return ACT_MakePIEError(Request);
	}

	FString ChildPath, ParentPath;
	FMCPResponse PathErr;
	if (!ACT_RequireActorPath(Request, ChildPath, PathErr, TEXT("child_actor_path"))) { return PathErr; }
	if (!ACT_RequireActorPath(Request, ParentPath, PathErr, TEXT("parent_actor_path"))) { return PathErr; }

	FMCPResponse ChildErr;
	AActor* Child = ACT_ResolveActorOrError(
		Request, ChildPath, /*bRejectPIE*/ true, /*bCheckSublevelVisible*/ true, ChildErr);
	if (!Child) { return ChildErr; }

	FMCPResponse ParentErr;
	AActor* Parent = ACT_ResolveActorOrError(
		Request, ParentPath, /*bRejectPIE*/ true, /*bCheckSublevelVisible*/ true, ParentErr);
	if (!Parent) { return ParentErr; }

	if (Child == Parent)
	{
		return ACT_MakeError(Request, kACTErrorInvalidParams,
			TEXT("child and parent are the same actor"));
	}

	// Cross-sublevel guard (M7).
	if (Child->GetLevel() != Parent->GetLevel())
	{
		return ACT_MakeError(Request, kMCPErrorLevelNotStreamingEntry,
			FString::Printf(
				TEXT("cross-sublevel attach refused — child lives in '%s' but parent lives in '%s'; ")
				TEXT("re-parent within a single sublevel to avoid dangling attachments on streaming-out"),
				Child->GetLevel() && Child->GetLevel()->GetOutermost() ?
					*Child->GetLevel()->GetOutermost()->GetName() : TEXT("?"),
				Parent->GetLevel() && Parent->GetLevel()->GetOutermost() ?
					*Parent->GetLevel()->GetOutermost()->GetName() : TEXT("?")));
	}

	// Cycle detection — bounded walk, fail-fast on corrupted/infinite ancestor chain.
	switch (ACT_AttachWouldCycle(Parent, Child))
	{
		case EACTCycleCheckResult::WouldCycle:
			return ACT_MakeError(Request, kACTErrorInvalidParams,
				FString::Printf(
					TEXT("attach would create a cycle — '%s' (or its ancestor) is already attached under '%s'"),
					*Parent->GetName(), *Child->GetName()));
		case EACTCycleCheckResult::DepthExceeded:
			return ACT_MakeError(Request, kACTErrorInternal,
				FString::Printf(
					TEXT("attach hierarchy depth exceeded (%d ancestors walked from '%s'); possible cycle in existing parent chain"),
					kACTMaxAttachDepth, *Parent->GetName()));
		case EACTCycleCheckResult::Safe:
			break;
	}

	FName SocketName(NAME_None);
	FString SocketStr;
	if (Request.Args->TryGetStringField(TEXT("socket_name"), SocketStr) && !SocketStr.IsEmpty())
	{
		SocketName = FName(*SocketStr);
	}

	// Attachment rule — translate string to FAttachmentTransformRules with bWeldSimulatedBodies=false.
	FAttachmentTransformRules Rules = FAttachmentTransformRules::KeepWorldTransform;
	FString RuleStr;
	if (Request.Args->TryGetStringField(TEXT("attachment_rule"), RuleStr) && !RuleStr.IsEmpty())
	{
		if (RuleStr.Equals(TEXT("KeepRelative"), ESearchCase::IgnoreCase))
		{
			Rules = FAttachmentTransformRules::KeepRelativeTransform;
		}
		else if (RuleStr.Equals(TEXT("KeepWorld"), ESearchCase::IgnoreCase))
		{
			Rules = FAttachmentTransformRules::KeepWorldTransform;
		}
		else if (RuleStr.Equals(TEXT("SnapToTarget"), ESearchCase::IgnoreCase)
			|| RuleStr.Equals(TEXT("SnapToTargetNotIncludingScale"), ESearchCase::IgnoreCase))
		{
			Rules = FAttachmentTransformRules::SnapToTargetNotIncludingScale;
		}
		else if (RuleStr.Equals(TEXT("SnapToTargetIncludingScale"), ESearchCase::IgnoreCase))
		{
			Rules = FAttachmentTransformRules::SnapToTargetIncludingScale;
		}
		else
		{
			return ACT_MakeError(Request, kACTErrorInvalidParams,
				FString::Printf(TEXT("attachment_rule '%s' unknown — expected KeepRelative / KeepWorld / SnapToTarget / SnapToTargetIncludingScale"),
					*RuleStr));
		}
	}

	const AActor* PrevParent = Child->GetAttachParentActor();

	const FScopedTransaction Transaction(LOCTEXT("ActorAttach", "MCP: attach actor"));
	Child->Modify();
	Parent->Modify();
	const bool bOk = Child->AttachToActor(Parent, Rules, SocketName);

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("ok"), bOk);
	Out->SetStringField(TEXT("child_actor_path"), FMCPActorPathUtils::BuildActorPath(Child));
	Out->SetStringField(TEXT("parent_actor_path"), FMCPActorPathUtils::BuildActorPath(Parent));
	Out->SetStringField(TEXT("socket"), SocketName.ToString());
	Out->SetStringField(TEXT("rule"), RuleStr.IsEmpty() ? TEXT("KeepWorld") : RuleStr);
	if (PrevParent)
	{
		Out->SetStringField(TEXT("previous_parent_path"), FMCPActorPathUtils::BuildActorPath(PrevParent));
	}
	else
	{
		Out->SetField(TEXT("previous_parent_path"), MakeShared<FJsonValueNull>());
	}
	if (!bOk)
	{
		return ACT_MakeError(Request, kACTErrorInternal,
			TEXT("AttachToActor returned false (no RootComponent on child? socket name missing on parent mesh?)"));
	}
	return ACT_MakeSuccessObj(Request, Out);
}

// ─── actor.detach (mutator — PIE-guarded) ────────────────────────────────────────────────────
//
// Detaches ``child_actor_path`` from its current parent.
//
// Args:
//   child_actor_path   (required actor_path)
//   detachment_rule    (optional enum string) — KeepRelative / KeepWorld (default KeepWorld)
//
// Response: { ok, child_actor_path, previous_parent_path }
FMCPResponse Tool_Detach(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (FMCPWorldContext::IsPIEActive())
	{
		return ACT_MakePIEError(Request);
	}

	FString ChildPath;
	FMCPResponse PathErr;
	if (!ACT_RequireActorPath(Request, ChildPath, PathErr, TEXT("child_actor_path"))) { return PathErr; }

	FMCPResponse ChildErr;
	AActor* Child = ACT_ResolveActorOrError(
		Request, ChildPath, /*bRejectPIE*/ true, /*bCheckSublevelVisible*/ true, ChildErr);
	if (!Child) { return ChildErr; }

	FDetachmentTransformRules Rules = FDetachmentTransformRules::KeepWorldTransform;
	FString RuleStr;
	if (Request.Args->TryGetStringField(TEXT("detachment_rule"), RuleStr) && !RuleStr.IsEmpty())
	{
		if (RuleStr.Equals(TEXT("KeepRelative"), ESearchCase::IgnoreCase))
		{
			Rules = FDetachmentTransformRules::KeepRelativeTransform;
		}
		else if (RuleStr.Equals(TEXT("KeepWorld"), ESearchCase::IgnoreCase))
		{
			Rules = FDetachmentTransformRules::KeepWorldTransform;
		}
		else
		{
			return ACT_MakeError(Request, kACTErrorInvalidParams,
				FString::Printf(TEXT("detachment_rule '%s' unknown — expected KeepRelative / KeepWorld"),
					*RuleStr));
		}
	}

	const AActor* PrevParent = Child->GetAttachParentActor();

	const FScopedTransaction Transaction(LOCTEXT("ActorDetach", "MCP: detach actor"));
	Child->Modify();
	Child->DetachFromActor(Rules);

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("ok"), true);
	Out->SetStringField(TEXT("child_actor_path"), FMCPActorPathUtils::BuildActorPath(Child));
	if (PrevParent)
	{
		Out->SetStringField(TEXT("previous_parent_path"), FMCPActorPathUtils::BuildActorPath(PrevParent));
	}
	else
	{
		Out->SetField(TEXT("previous_parent_path"), MakeShared<FJsonValueNull>());
	}
	return ACT_MakeSuccessObj(Request, Out);
}

// ─── actor.get_property (read-only — works in PIE) ───────────────────────────────────────────
//
// Reads a single FProperty value off the actor by dotted path (e.g. RootComponent.RelativeLocation).
// Delegates to FMCPReflection::ResolvePropertyPath + ReadPropertyValueAt.
//
// Args: { actor_path, property_name: dotted path }
// Response: { actor_path, property_path, type, value: <jsonified> }
FMCPResponse Tool_GetProperty(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString Path;
	FMCPResponse PathErr;
	if (!ACT_RequireActorPath(Request, Path, PathErr)) { return PathErr; }

	FString PropertyName;
	if (!Request.Args->TryGetStringField(TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
	{
		return ACT_MakeError(Request, kACTErrorInvalidParams,
			TEXT("missing required string field 'property_name'"));
	}

	FMCPResponse ResolveErr;
	AActor* Actor = ACT_ResolveActorOrError(
		Request, Path, /*bRejectPIE*/ false, /*bCheckSublevelVisible*/ false, ResolveErr);
	if (!Actor) { return ResolveErr; }

	UObject* Container = nullptr;
	void* LeafValuePtr = nullptr;
	FProperty* LeafProp = nullptr;
	int32 ErrCode = 0;
	FString ErrMsg;
	if (!FMCPReflection::ResolvePropertyPath(Actor, PropertyName, Container, LeafValuePtr, LeafProp,
		ErrCode, ErrMsg))
	{
		return ACT_MakeError(Request, ErrCode, ErrMsg);
	}

	TSharedPtr<FJsonValue> Value = FMCPReflection::ReadPropertyValueAt(LeafProp, LeafValuePtr);

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("actor_path"),    FMCPActorPathUtils::BuildActorPath(Actor));
	Out->SetStringField(TEXT("property_path"), PropertyName);
	Out->SetStringField(TEXT("type"),          FMCPReflection::DescribePropertyType(LeafProp));
	Out->SetField(TEXT("value"), Value);
	return ACT_MakeSuccessObj(Request, Out);
}

// ─── actor.set_property (mutator — PIE-guarded) ──────────────────────────────────────────────
//
// Writes a single FProperty by dotted path. Implements the full D7 contract:
//   1. Edit-const gate FIRST (no transaction opened, no Pre/Post imbalance)
//   2. FMCPWritePropertyScope owns 4-step contract (Pre + Modify + transaction; Post on dtor)
//   3. WritePropertyValueAt handles the JSON → FProperty marshalling
//
// Args: { actor_path, property_name: dotted path, value: <jsonified> }
// Response: { ok: true, actor_path, property_path, value: <new value, re-read for round-trip> }
FMCPResponse Tool_SetProperty(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (FMCPWorldContext::IsPIEActive())
	{
		return ACT_MakePIEError(Request);
	}

	FString Path;
	FMCPResponse PathErr;
	if (!ACT_RequireActorPath(Request, Path, PathErr)) { return PathErr; }

	FString PropertyName;
	if (!Request.Args->TryGetStringField(TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
	{
		return ACT_MakeError(Request, kACTErrorInvalidParams,
			TEXT("missing required string field 'property_name'"));
	}
	const TSharedPtr<FJsonValue> ValueField = Request.Args->TryGetField(TEXT("value"));
	if (!ValueField.IsValid())
	{
		return ACT_MakeError(Request, kACTErrorInvalidParams,
			TEXT("missing required field 'value'"));
	}

	FMCPResponse ResolveErr;
	AActor* Actor = ACT_ResolveActorOrError(
		Request, Path, /*bRejectPIE*/ true, /*bCheckSublevelVisible*/ true, ResolveErr);
	if (!Actor) { return ResolveErr; }

	UObject* Container = nullptr;
	void* LeafValuePtr = nullptr;
	FProperty* LeafProp = nullptr;
	int32 ErrCode = 0;
	FString ErrMsg;
	if (!FMCPReflection::ResolvePropertyPath(Actor, PropertyName, Container, LeafValuePtr, LeafProp,
		ErrCode, ErrMsg))
	{
		return ACT_MakeError(Request, ErrCode, ErrMsg);
	}

	// Step 1: edit-const gate FIRST (early return, no transaction). Matches canonical 3-flag set
	// per MCPReflection.h:192 + LevelTools.cpp pattern. CPF_DisableEditOnInstance is critical for
	// actor.set_property because it operates on placed instances (not CDOs) — its omission lets
	// the bridge silently overwrite values the editor's own property browser refuses.
	const uint64 Flags = LeafProp->PropertyFlags;
	if (Flags & (CPF_BlueprintReadOnly | CPF_EditConst | CPF_DisableEditOnInstance))
	{
		return ACT_MakeError(Request, kMCPErrorPropertyAccessDenied,
			FString::Printf(
				TEXT("property '%s' is read-only (CPF flags=%llu)"),
				*LeafProp->GetName(), static_cast<unsigned long long>(Flags)));
	}

	// Step 2: RAII scope owns Pre/Modify/Transaction; dtor fires PostEditChangeProperty.
	FString WriteErr;
	bool bWriteOk = false;
	{
		FMCPWritePropertyScope Scope(Container, LeafProp,
			LOCTEXT("ActorSetProperty", "MCP: set actor property"));
		bWriteOk = FMCPReflection::WritePropertyValueAt(LeafProp, LeafValuePtr, ValueField, Container, WriteErr);
	}
	if (!bWriteOk)
	{
		return ACT_MakeError(Request, kMCPErrorPropertyTypeMismatch,
			FString::Printf(TEXT("write rejected: %s"), *WriteErr));
	}

	// Re-read post-write for round-trip echo.
	TSharedPtr<FJsonValue> EchoValue = FMCPReflection::ReadPropertyValueAt(LeafProp, LeafValuePtr);

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("ok"), true);
	Out->SetStringField(TEXT("actor_path"),    FMCPActorPathUtils::BuildActorPath(Actor));
	Out->SetStringField(TEXT("property_path"), PropertyName);
	Out->SetField(TEXT("value"), EchoValue);
	return ACT_MakeSuccessObj(Request, Out);
}

// ─── actor.exists (read-only — works in PIE) ─────────────────────────────────────────────────
//
// Boolean presence check. Resolves the actor path WITHOUT raising any error — returns
// {exists: bool, actor_path: <canonical or original>}.
//
// Args: { actor_path }
// Response: { exists: bool, actor_path }
FMCPResponse Tool_Exists(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString Path;
	FMCPResponse PathErr;
	if (!ACT_RequireActorPath(Request, Path, PathErr)) { return PathErr; }

	AActor* Actor = FMCPActorPathUtils::ResolveActorOrNull(Path, /*bRejectPIE*/ false);

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("exists"), Actor != nullptr);
	Out->SetStringField(TEXT("actor_path"),
		Actor ? FMCPActorPathUtils::BuildActorPath(Actor) : Path);
	return ACT_MakeSuccessObj(Request, Out);
}

// ─── actor.select_in_editor (works in PIE per D10 edge case) ─────────────────────────────────
//
// Selects the named actors in the editor outliner. Selection is editor-side state (USelection),
// independent of editor vs play world, so this works during PIE. Wraps in transaction per M9.
//
// Args: { actor_paths: [string], additive: bool (default false — clears prior selection) }
// Response: { selected: N, requested: M, missing: [paths that couldn't resolve] }
FMCPResponse Tool_SelectInEditor(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!Request.Args.IsValid())
	{
		return ACT_MakeError(Request, kACTErrorInvalidParams, TEXT("missing args object"));
	}
	const TArray<TSharedPtr<FJsonValue>>* PathsArr = nullptr;
	if (!Request.Args->TryGetArrayField(TEXT("actor_paths"), PathsArr) || !PathsArr)
	{
		return ACT_MakeError(Request, kACTErrorInvalidParams,
			TEXT("missing required array field 'actor_paths'"));
	}
	bool bAdditive = false;
	Request.Args->TryGetBoolField(TEXT("additive"), bAdditive);

	if (!GEditor)
	{
		return ACT_MakeError(Request, kACTErrorInternal, TEXT("GEditor is null"));
	}
	USelection* Selection = GEditor->GetSelectedActors();
	if (!Selection)
	{
		return ACT_MakeError(Request, kACTErrorInternal, TEXT("GEditor->GetSelectedActors returned null"));
	}

	TArray<AActor*> Resolved;
	TArray<FString> Missing;
	Resolved.Reserve(PathsArr->Num());
	for (const TSharedPtr<FJsonValue>& V : *PathsArr)
	{
		if (!V.IsValid() || V->Type != EJson::String)
		{
			continue;
		}
		const FString OnePath = V->AsString();
		AActor* A = FMCPActorPathUtils::ResolveActorOrNull(OnePath, /*bRejectPIE*/ false);
		if (A)
		{
			Resolved.Add(A);
		}
		else
		{
			Missing.Add(OnePath);
		}
	}

	const FScopedTransaction Transaction(FText::Format(LOCTEXT("ActorSelectN", "MCP: select {0} actor(s)"),
		FText::AsNumber(Resolved.Num())));
	Selection->Modify();

	Selection->BeginBatchSelectOperation();
	if (!bAdditive)
	{
		GEditor->SelectNone(/*bNoteSelectionChange*/ false, /*bDeselectBSPSurfs*/ true, /*WarnAboutManyActors*/ false);
	}
	for (AActor* A : Resolved)
	{
		GEditor->SelectActor(A, /*bInSelected*/ true, /*bNotify*/ false, /*bSelectEvenIfHidden*/ true);
	}
	Selection->EndBatchSelectOperation(/*bNotify*/ true);

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetNumberField(TEXT("selected"), static_cast<double>(Resolved.Num()));
	Out->SetNumberField(TEXT("requested"), static_cast<double>(PathsArr->Num()));
	Out->SetBoolField(TEXT("additive"), bAdditive);
	TArray<TSharedPtr<FJsonValue>> MissingJson;
	MissingJson.Reserve(Missing.Num());
	for (const FString& M : Missing)
	{
		MissingJson.Add(MakeShared<FJsonValueString>(M));
	}
	Out->SetArrayField(TEXT("missing"), MissingJson);
	return ACT_MakeSuccessObj(Request, Out);
}

// ─── actor.find_by_class (paginated read — works in PIE) ─────────────────────────────────────
//
// Enumerates actors whose class IsA(target_class) — by default walks all loaded levels. Pagination
// via Phase 2 FMCPPageCursor (sort by GetPathName, sentinel = last actor's path; filter hash
// validates caller didn't switch class mid-pagination).
//
// Args:
//   class_path     (required) — same resolution as actor.spawn
//   search_subclasses (optional bool, default true) — IsA semantics; false = exact class
//   page_size      (optional, default 100, clamped to [1, 1000]) — Phase 3 cap keeps
//                  responses comfortably under 256 KB; bump only if caller truly needs more
//   page_token     (optional opaque base64 from previous response)
//
// Response: { actors: [...], total_known, next_page_token: str | null }
FMCPResponse Tool_FindByClass(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!Request.Args.IsValid())
	{
		return ACT_MakeError(Request, kACTErrorInvalidParams, TEXT("missing args object"));
	}
	FString ClassPath;
	if (!Request.Args->TryGetStringField(TEXT("class_path"), ClassPath) || ClassPath.IsEmpty())
	{
		return ACT_MakeError(Request, kACTErrorInvalidParams,
			TEXT("missing required string field 'class_path'"));
	}

	// Resolve class — but for find we accept ABSTRACT (it's a filter, not a spawn target).
	// Reproduce the resolver logic without the abstract/family gates to keep the surface friendly.
	if (ClassPath[0] != TEXT('/'))
	{
		return ACT_MakeError(Request, kMCPErrorInvalidClassPath,
			FString::Printf(TEXT("class_path '%s' invalid — must start with '/'"), *ClassPath));
	}
	UClass* TargetClass = LoadObject<UClass>(nullptr, *ClassPath);
	if (!TargetClass && !ClassPath.EndsWith(TEXT("_C")))
	{
		const FString Retry = ClassPath + TEXT("_C");
		TargetClass = LoadObject<UClass>(nullptr, *Retry);
	}
	if (!TargetClass)
	{
		return ACT_MakeError(Request, kMCPErrorClassNotFound,
			FString::Printf(TEXT("class_path '%s' could not be resolved (LoadObject returned null)"), *ClassPath));
	}
	if (!TargetClass->IsChildOf(AActor::StaticClass()))
	{
		return ACT_MakeError(Request, kMCPErrorWrongClassFamily,
			FString::Printf(TEXT("class '%s' is not an AActor subclass"), *TargetClass->GetPathName()));
	}

	bool bSearchSubclasses = true;
	Request.Args->TryGetBoolField(TEXT("search_subclasses"), bSearchSubclasses);

	UWorld* World = ACT_ResolveReadWorld();
	if (!World)
	{
		return ACT_MakeError(Request, kMCPErrorLevelNotFound, TEXT("no world available"));
	}

	// Gather all matching actors.
	TArray<TWeakObjectPtr<AActor>> Matches;
	for (FActorIterator It(World); It; ++It)
	{
		AActor* A = *It;
		if (!A) { continue; }
		const bool bMatches = bSearchSubclasses
			? A->IsA(TargetClass)
			: (A->GetClass() == TargetClass);
		if (bMatches)
		{
			Matches.Add(A);
		}
	}
	ACT_SortByPathName(Matches);

	const uint64 FilterHash = ACT_HashFilter(TargetClass->GetPathName(), FString(), bSearchSubclasses, EACTHashFilter::Class);

	FString TokenWire;
	Request.Args->TryGetStringField(TEXT("page_token"), TokenWire);
	FMCPPageCursor Cursor;
	FMCPResponse CursorErr;
	if (!ACT_DecodeCursor(Request, TokenWire, FilterHash, Cursor, CursorErr))
	{
		return CursorErr;
	}

	const int32 PageSize = ACT_ClampPageSize(Request.Args, TEXT("page_size"), 100);

	TArray<AActor*> Page;
	FString NextSentinel;
	ACT_SliceActorPage(Matches, Cursor.LastAssetPath, PageSize, Page, NextSentinel);

	return ACT_BuildPaginatedResponse(Request, Page, Matches.Num(), NextSentinel, FilterHash);
}

// ─── actor.find_by_label (paginated read — works in PIE) ─────────────────────────────────────
//
// Finds actors whose display label contains ``label_substring`` (case-insensitive). With
// ``exact=true``, matches the full label string. Pagination same as find_by_class.
//
// Args: { label_substring (req), exact: bool (default false), page_size, page_token }
FMCPResponse Tool_FindByLabel(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!Request.Args.IsValid())
	{
		return ACT_MakeError(Request, kACTErrorInvalidParams, TEXT("missing args object"));
	}
	FString LabelSub;
	if (!Request.Args->TryGetStringField(TEXT("label_substring"), LabelSub))
	{
		return ACT_MakeError(Request, kACTErrorInvalidParams,
			TEXT("missing required string field 'label_substring'"));
	}
	if (LabelSub.IsEmpty())
	{
		return ACT_MakeError(Request, kACTErrorInvalidParams,
			TEXT("'label_substring' must be non-empty"));
	}
	bool bExact = false;
	Request.Args->TryGetBoolField(TEXT("exact"), bExact);

	UWorld* World = ACT_ResolveReadWorld();
	if (!World)
	{
		return ACT_MakeError(Request, kMCPErrorLevelNotFound, TEXT("no world available"));
	}

	TArray<TWeakObjectPtr<AActor>> Matches;
	for (FActorIterator It(World); It; ++It)
	{
		AActor* A = *It;
		if (!A) { continue; }
		const FString Label = A->GetActorLabel();
		bool bMatches;
		if (bExact)
		{
			bMatches = Label.Equals(LabelSub, ESearchCase::IgnoreCase);
		}
		else
		{
			bMatches = Label.Contains(LabelSub, ESearchCase::IgnoreCase);
		}
		if (bMatches)
		{
			Matches.Add(A);
		}
	}
	ACT_SortByPathName(Matches);

	const uint64 FilterHash = ACT_HashFilter(LabelSub, FString(), bExact, EACTHashFilter::Label);

	FString TokenWire;
	Request.Args->TryGetStringField(TEXT("page_token"), TokenWire);
	FMCPPageCursor Cursor;
	FMCPResponse CursorErr;
	if (!ACT_DecodeCursor(Request, TokenWire, FilterHash, Cursor, CursorErr))
	{
		return CursorErr;
	}

	const int32 PageSize = ACT_ClampPageSize(Request.Args, TEXT("page_size"), 100);

	TArray<AActor*> Page;
	FString NextSentinel;
	ACT_SliceActorPage(Matches, Cursor.LastAssetPath, PageSize, Page, NextSentinel);

	return ACT_BuildPaginatedResponse(Request, Page, Matches.Num(), NextSentinel, FilterHash);
}

// ─── actor.find_by_tag (paginated read — works in PIE) ───────────────────────────────────────
//
// Finds actors whose Tags array contains ``tag`` (exact FName match).
//
// Args: { tag (req string), page_size, page_token }
FMCPResponse Tool_FindByTag(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!Request.Args.IsValid())
	{
		return ACT_MakeError(Request, kACTErrorInvalidParams, TEXT("missing args object"));
	}
	FString Tag;
	if (!Request.Args->TryGetStringField(TEXT("tag"), Tag) || Tag.IsEmpty())
	{
		return ACT_MakeError(Request, kACTErrorInvalidParams,
			TEXT("missing required non-empty string field 'tag'"));
	}

	UWorld* World = ACT_ResolveReadWorld();
	if (!World)
	{
		return ACT_MakeError(Request, kMCPErrorLevelNotFound, TEXT("no world available"));
	}

	const FName TargetTag(*Tag);
	TArray<TWeakObjectPtr<AActor>> Matches;
	for (FActorIterator It(World); It; ++It)
	{
		AActor* A = *It;
		if (!A) { continue; }
		if (A->Tags.Contains(TargetTag))
		{
			Matches.Add(A);
		}
	}
	ACT_SortByPathName(Matches);

	const uint64 FilterHash = ACT_HashFilter(Tag, FString(), false, EACTHashFilter::Tag);

	FString TokenWire;
	Request.Args->TryGetStringField(TEXT("page_token"), TokenWire);
	FMCPPageCursor Cursor;
	FMCPResponse CursorErr;
	if (!ACT_DecodeCursor(Request, TokenWire, FilterHash, Cursor, CursorErr))
	{
		return CursorErr;
	}

	const int32 PageSize = ACT_ClampPageSize(Request.Args, TEXT("page_size"), 100);

	TArray<AActor*> Page;
	FString NextSentinel;
	ACT_SliceActorPage(Matches, Cursor.LastAssetPath, PageSize, Page, NextSentinel);

	return ACT_BuildPaginatedResponse(Request, Page, Matches.Num(), NextSentinel, FilterHash);
}

// ─── actor.list_components (read-only — works in PIE) ────────────────────────────────────────
//
// Returns the full component list for one actor. Same shape as actor.get's "components" field but
// without the surrounding actor metadata — useful when the AI already has the actor identity.
//
// Args: { actor_path }
// Response: { actor_path, components: [...], total: N }
FMCPResponse Tool_ListComponents(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString Path;
	FMCPResponse PathErr;
	if (!ACT_RequireActorPath(Request, Path, PathErr)) { return PathErr; }

	FMCPResponse ResolveErr;
	AActor* Actor = ACT_ResolveActorOrError(
		Request, Path, /*bRejectPIE*/ false, /*bCheckSublevelVisible*/ false, ResolveErr);
	if (!Actor) { return ResolveErr; }

	TArray<UActorComponent*> Comps;
	Actor->GetComponents(Comps);

	TArray<TSharedPtr<FJsonValue>> CompArr;
	CompArr.Reserve(Comps.Num());
	for (UActorComponent* C : Comps)
	{
		CompArr.Add(MakeShared<FJsonValueObject>(ACT_BuildComponentSummary(C)));
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("actor_path"), FMCPActorPathUtils::BuildActorPath(Actor));
	Out->SetArrayField(TEXT("components"), CompArr);
	Out->SetNumberField(TEXT("total"), static_cast<double>(Comps.Num()));
	return ACT_MakeSuccessObj(Request, Out);
}

// ─── Registration ────────────────────────────────────────────────────────────────────────────
void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	// Day 4: spawn / destroy / duplicate / get
	RegisterTool(TEXT("actor.spawn"),     &Tool_Spawn,     /*Lane A*/ false);
	RegisterTool(TEXT("actor.destroy"),   &Tool_Destroy,   /*Lane A*/ false);
	RegisterTool(TEXT("actor.duplicate"), &Tool_Duplicate, /*Lane A*/ false);
	RegisterTool(TEXT("actor.get"),       &Tool_Get,       /*Lane A*/ false);

	// Day 5: transform mutators
	RegisterTool(TEXT("actor.set_transform"), &Tool_SetTransform, /*Lane A*/ false);
	RegisterTool(TEXT("actor.set_location"),  &Tool_SetLocation,  /*Lane A*/ false);
	RegisterTool(TEXT("actor.set_rotation"),  &Tool_SetRotation,  /*Lane A*/ false);
	RegisterTool(TEXT("actor.set_scale"),     &Tool_SetScale,     /*Lane A*/ false);

	// Day 6: identity / outliner / attachment
	RegisterTool(TEXT("actor.set_label"),  &Tool_SetLabel,  /*Lane A*/ false);
	RegisterTool(TEXT("actor.set_folder"), &Tool_SetFolder, /*Lane A*/ false);
	RegisterTool(TEXT("actor.attach"),     &Tool_Attach,    /*Lane A*/ false);
	RegisterTool(TEXT("actor.detach"),     &Tool_Detach,    /*Lane A*/ false);

	// Day 7: property + presence + selection
	RegisterTool(TEXT("actor.get_property"),       &Tool_GetProperty,     /*Lane A*/ false);
	RegisterTool(TEXT("actor.set_property"),       &Tool_SetProperty,     /*Lane A*/ false);
	RegisterTool(TEXT("actor.exists"),             &Tool_Exists,          /*Lane A*/ false);
	RegisterTool(TEXT("actor.select_in_editor"),   &Tool_SelectInEditor,  /*Lane A*/ false);

	// Day 8: find / list_components
	RegisterTool(TEXT("actor.find_by_class"),  &Tool_FindByClass,     /*Lane A*/ false);
	RegisterTool(TEXT("actor.find_by_label"),  &Tool_FindByLabel,     /*Lane A*/ false);
	RegisterTool(TEXT("actor.find_by_tag"),    &Tool_FindByTag,       /*Lane A*/ false);
	RegisterTool(TEXT("actor.list_components"),&Tool_ListComponents,  /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("Phase 3 Days 4-8: registered 20 actor.* handlers (all Lane A)"));
}

} // namespace FActorTools

#undef LOCTEXT_NAMESPACE
