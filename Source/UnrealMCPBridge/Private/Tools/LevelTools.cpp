// Copyright FatumGame. All Rights Reserved.

#include "LevelTools.h"

#include "FMCPDispatchQueue.h"
#include "FMCPJobRegistry.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPPageCursor.h"
#include "Utils/MCPReflection.h"
#include "Utils/MCPWorldContext.h"

#include "Editor.h"
#include "EditorLevelUtils.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/LevelStreaming.h"
#include "Engine/LevelStreamingDynamic.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "GameFramework/WorldSettings.h"
#include "LevelEditorSubsystem.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformTLS.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// LVL_ prefix per the unity-build symbol-collision pattern (MakeError/MakeSuccess clash with
	// UE's global ValueOrError templates).
	constexpr int32 kLVLErrorInvalidParams = -32602;
	constexpr int32 kLVLErrorInternal      = -32603;

	void LVL_StampIds(const FMCPRequest& Request, FMCPResponse& Response)
	{
		Response.RequestId = Request.RequestId;
		Response.OriginalIdString = Request.OriginalIdString;
	}

	FMCPResponse LVL_MakeError(const FMCPRequest& Request, int32 Code, const FString& Message)
	{
		FMCPResponse R;
		LVL_StampIds(Request, R);
		R.bIsError = true;
		R.ErrorCode = Code;
		R.ErrorMessage = Message;
		return R;
	}

	FMCPResponse LVL_MakeSuccessObj(const FMCPRequest& Request, TSharedPtr<FJsonObject> Result)
	{
		FMCPResponse R;
		LVL_StampIds(Request, R);
		R.bIsError = false;
		R.Result = MakeShared<FJsonValueObject>(MoveTemp(Result));
		return R;
	}

	/** Frozen PIE-mutator refusal (per D10 — smoke asserts both "Phase 5" and "pie." substrings). */
	FMCPResponse LVL_MakePIEError(const FMCPRequest& Request)
	{
		return LVL_MakeError(Request, kMCPErrorPIEActive, kMCPMessagePIEActive);
	}

	/** Read optional ``map_path`` arg; empty string means "use editor world". */
	void LVL_GetOptionalMapPathArg(const FMCPRequest& Request, FString& OutMapPath)
	{
		OutMapPath.Reset();
		if (Request.Args.IsValid())
		{
			Request.Args->TryGetStringField(TEXT("map_path"), OutMapPath);
		}
	}

	/** Resolve target world: PIE world if active + read-only, else editor world. Returns null on failure. */
	UWorld* LVL_ResolveReadWorld()
	{
		if (FMCPWorldContext::IsPIEActive())
		{
			return GEditor->PlayWorld;
		}
		return FMCPWorldContext::GetEditorWorld();
	}

	/**
	 * Build a JSON summary of one ULevel — used by ``level.list_loaded``.
	 *
	 * Shape:
	 *   {
	 *     "map_path": "/Game/Maps/X",
	 *     "is_persistent": bool,
	 *     "is_visible": bool,
	 *     "actor_count": N,
	 *     "is_dirty": bool
	 *   }
	 */
	TSharedRef<FJsonObject> LVL_BuildLevelSummary(ULevel* Level, UWorld* OwningWorld)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (!Level)
		{
			Obj->SetField(TEXT("map_path"), MakeShared<FJsonValueNull>());
			return Obj;
		}
		const FString PackageName = Level->GetOutermost()->GetName();
		Obj->SetStringField(TEXT("map_path"), PackageName);
		Obj->SetBoolField(TEXT("is_persistent"), OwningWorld && OwningWorld->PersistentLevel == Level);
		Obj->SetBoolField(TEXT("is_visible"), Level->bIsVisible);
		Obj->SetNumberField(TEXT("actor_count"), static_cast<double>(Level->Actors.Num()));
		const UPackage* Pkg = Level->GetOutermost();
		Obj->SetBoolField(TEXT("is_dirty"), Pkg && Pkg->IsDirty());
		return Obj;
	}

	/**
	 * Build a JSON summary of one actor for ``level.get_persistent_level_actors``.
	 *
	 * Shape:
	 *   {
	 *     "name": "BP_Foo_C_5",
	 *     "label": "BP_Foo (display label)",
	 *     "class": "/Game/.../BP_Foo.BP_Foo_C",
	 *     "location": {x,y,z},
	 *     "is_hidden": bool
	 *   }
	 */
	TSharedRef<FJsonObject> LVL_BuildActorSummary(const AActor* Actor)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Actor->GetFName().ToString());
		Obj->SetStringField(TEXT("label"), Actor->GetActorNameOrLabel());
		Obj->SetStringField(TEXT("class"), Actor->GetClass()->GetPathName());

		const FVector Loc = Actor->GetActorLocation();
		TSharedRef<FJsonObject> LocObj = MakeShared<FJsonObject>();
		LocObj->SetNumberField(TEXT("x"), Loc.X);
		LocObj->SetNumberField(TEXT("y"), Loc.Y);
		LocObj->SetNumberField(TEXT("z"), Loc.Z);
		Obj->SetObjectField(TEXT("location"), LocObj);

		Obj->SetBoolField(TEXT("is_hidden"), Actor->IsHidden());
		return Obj;
	}

	/**
	 * Resolve ``args.map_path`` to a sublevel within the editor world, surfacing WP/notfound errors
	 * cleanly. Returns null on failure with OutError populated for the caller to wrap in a
	 * response.
	 */
	ULevel* LVL_ResolveSubLevelArg(
		const FMCPRequest& Request,
		bool bRejectPartitioned,
		FMCPResponse& OutError)
	{
		FString MapPath;
		if (!Request.Args.IsValid()
			|| !Request.Args->TryGetStringField(TEXT("map_path"), MapPath)
			|| MapPath.IsEmpty())
		{
			OutError = LVL_MakeError(Request, kLVLErrorInvalidParams,
				TEXT("missing required string field 'map_path'"));
			return nullptr;
		}
		UWorld* World = FMCPWorldContext::GetEditorWorld();
		if (!World)
		{
			OutError = LVL_MakeError(Request, kMCPErrorLevelNotFound,
				TEXT("no editor world available (GEditor missing)"));
			return nullptr;
		}
		bool bWPRejected = false;
		ULevel* Level = FMCPWorldContext::ResolveLevelByMapPath(World, MapPath, bRejectPartitioned, bWPRejected);
		if (bWPRejected)
		{
			OutError = LVL_MakeError(Request, kMCPErrorWorldPartitionNotSupported,
				FString::Printf(
					TEXT("editor world is partitioned — Phase 5 will ship wp.* tools for this surface (map='%s')"),
					*MapPath));
			return nullptr;
		}
		if (!Level)
		{
			OutError = LVL_MakeError(Request, kMCPErrorLevelNotFound,
				FString::Printf(TEXT("no loaded sublevel matches map_path='%s'"), *MapPath));
			return nullptr;
		}
		return Level;
	}

	/**
	 * Find the ``ULevelStreaming`` entry in ``World->GetStreamingLevels()`` whose target package
	 * matches ``MapPath``. Returns null if no entry (the level may be loaded but not via the
	 * streaming-levels list — e.g. it IS the persistent level).
	 */
	ULevelStreaming* LVL_FindStreamingEntry(UWorld* World, const FString& MapPath)
	{
		if (!World)
		{
			return nullptr;
		}
		const FString Norm = FMCPWorldContext::NormaliseMapPath(MapPath);
		if (Norm.IsEmpty())
		{
			return nullptr;
		}
		const FName Target(*Norm);
		for (ULevelStreaming* Streaming : World->GetStreamingLevels())
		{
			if (!Streaming)
			{
				continue;
			}
			if (Streaming->GetWorldAssetPackageFName() == Target)
			{
				return Streaming;
			}
		}
		return nullptr;
	}

	// ─── Sentinel-cursor pagination helpers (mirrors AssetRegistryTools / ActorTools pattern) ───
	//
	// Mirrors the Phase 2 cursor contract: filter hash binds the result-set identity (mutation
	// between pages → kMCPErrorStaleCursor); LastAssetPath sentinel is the last-emitted entry's
	// stable sort key (Actor::GetPathName here, matching ACT_SliceActorPage). Keyset pagination
	// survives mid-pagination inserts/deletes — new actors land in their sorted slot naturally,
	// deleted ones leave a gap the next-greater key fills.

	/** Clamp ``page_size`` to [1, 1000]. */
	int32 LVL_ClampPageSize(const TSharedPtr<FJsonObject>& Args, const TCHAR* FieldName, int32 Default)
	{
		int32 Out = Default;
		if (Args.IsValid())
		{
			Args->TryGetNumberField(FieldName, Out);
		}
		return FMath::Clamp(Out, 1, 1000);
	}

	/**
	 * Stable hash over every arg that affects result-set identity. ``page_size`` is intentionally
	 * NOT mixed — callers may resize between pages without invalidating the cursor.
	 *
	 * For level.get_persistent_level_actors the only such input is the resolved level's package
	 * name (the underlying ``Level->Actors`` source). Computing the hash over the RESOLVED level
	 * package — not the raw ``map_path`` arg — means an empty ``map_path`` (=persistent level) and
	 * an explicit map_path pointing at that same persistent level produce identical hashes; the
	 * caller can switch arg form without a stale cursor.
	 */
	uint64 LVL_HashLevelFilter(const FString& ResolvedLevelPackageName)
	{
		const uint32 H1 = GetTypeHash(ResolvedLevelPackageName);
		// Magic constants discriminate this tool's filter shape from other future LVL_* paginators
		// that may hash the same string but mean something different.
		constexpr uint64 ToolDiscriminant = 0xA1B2C3D400000001ull; // level.get_persistent_level_actors == 1
		return (static_cast<uint64>(H1) << 32) ^ ToolDiscriminant;
	}

	/**
	 * Decode + filter-hash-validate a page_token. Populates ``OutCursor`` on success; on decode or
	 * hash mismatch populates ``OutError`` (caller returns it directly) and returns false. Empty
	 * token = first-page request (always succeeds).
	 */
	bool LVL_DecodeCursor(
		const FMCPRequest& Request,
		const FString& TokenWire,
		uint64 ExpectedFilterHash,
		FMCPPageCursor& OutCursor,
		FMCPResponse& OutError)
	{
		FString DecodeErr;
		if (!FMCPPageCursorUtils::Decode(TokenWire, OutCursor, DecodeErr))
		{
			// Fail-fast on malformed token: -32602 (invalid params) per the CLAUDE.md contract, not
			// STALE_CURSOR (which is reserved for "valid encoding, wrong filter").
			OutError = LVL_MakeError(Request, kLVLErrorInvalidParams,
				FString::Printf(TEXT("page_token decode failed: %s"), *DecodeErr));
			return false;
		}
		if (!FMCPPageCursorUtils::ValidateAgainstFilter(OutCursor, ExpectedFilterHash))
		{
			OutError = LVL_MakeError(Request, kMCPErrorStaleCursor,
				TEXT("page_token filter_hash mismatch — caller mutated filter between pages; "
					 "restart pagination with page_token=null"));
			return false;
		}
		return true;
	}
} // namespace

namespace FLevelTools
{

// ─── Lane B sanity probe (per critic N1) ──────────────────────────────────────────────────────
//
// Purpose: prove the Lane B router (FMCPDispatchQueue::IsThreadSafe + DispatchInline +
// FMCPConnection short-circuit) is still alive after the Phase 2 hotfix demoted every AR/CB tool
// to Lane A. The handler does NO UObject access — pure string/JSON manipulation — so it satisfies
// the Lane B contract (no GWorld, no LoadObject, no NewObject, no GC interaction).
//
// Response shape:
//   { "echo": <args verbatim>, "thread_id": "139987..." }
//
// A non-game-thread thread_id (compared against the main thread's id observed in Phase 1
// initialisation logs) confirms inline dispatch. Phase 3 smoke spike-calls this 100× and asserts
// no crashes / no asserts.
FMCPResponse Tool_Phase3LaneBSanity(const FMCPRequest& Request)
{
	// Lane B handlers MUST NOT touch UObjects. Build a pure-string response and echo args.
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();

	// Echo args. If null (no payload), emit empty object so AI clients see the field consistently.
	TSharedRef<FJsonObject> EchoObj = Request.Args.IsValid()
		? Request.Args.ToSharedRef()
		: MakeShared<FJsonObject>();
	Out->SetObjectField(TEXT("echo"), EchoObj);

	// UE's platform thread id is a uint32 — stable for the lifetime of the OS thread. The smoke
	// spike compares this against the game-thread id captured at module init and asserts the
	// Lane B response came from a DIFFERENT thread.
	const uint32 TID = FPlatformTLS::GetCurrentThreadId();
	Out->SetStringField(TEXT("thread_id"), FString::FromInt(static_cast<int32>(TID)));

	return LVL_MakeSuccessObj(Request, Out);
}

// ─── level.list_loaded (read-only — works in PIE) ────────────────────────────────────────────
//
// Returns all currently loaded ULevels in either the editor world (no PIE) or the play world
// (PIE active). For each level emits {map_path, is_persistent, is_visible, actor_count, is_dirty}.
//
// Response: { "world_kind": "Editor"|"PIE", "world_map_path": "...", "levels": [...], "total": N }
FMCPResponse Tool_ListLoaded(const FMCPRequest& Request)
{
	check(IsInGameThread());

	UWorld* World = LVL_ResolveReadWorld();
	if (!World)
	{
		return LVL_MakeError(Request, kMCPErrorLevelNotFound,
			TEXT("no world available (GEditor missing and no PIE world)"));
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("world_kind"), FMCPWorldContext::IsPIEActive() ? TEXT("PIE") : TEXT("Editor"));
	Out->SetStringField(TEXT("world_map_path"), FMCPWorldContext::GetWorldPackagePath(World));

	TArray<TSharedPtr<FJsonValue>> Levels;
	const TArray<ULevel*>& AllLevels = World->GetLevels();
	Levels.Reserve(AllLevels.Num());
	for (ULevel* Level : AllLevels)
	{
		if (!Level)
		{
			continue;
		}
		Levels.Add(MakeShared<FJsonValueObject>(LVL_BuildLevelSummary(Level, World)));
	}
	Out->SetArrayField(TEXT("levels"), Levels);
	Out->SetNumberField(TEXT("total"), static_cast<double>(Levels.Num()));
	return LVL_MakeSuccessObj(Request, Out);
}

// ─── level.current_map (read-only — works in PIE) ────────────────────────────────────────────
//
// Returns the persistent-level package path of the editor world (or PIE world during PIE).
//
// Response: { "map_path": "/Game/Maps/X", "world_kind": "Editor"|"PIE", "is_dirty": bool }
FMCPResponse Tool_CurrentMap(const FMCPRequest& Request)
{
	check(IsInGameThread());

	UWorld* World = LVL_ResolveReadWorld();
	if (!World)
	{
		return LVL_MakeError(Request, kMCPErrorLevelNotFound,
			TEXT("no world available (GEditor missing and no PIE world)"));
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("map_path"), FMCPWorldContext::GetWorldPackagePath(World));
	Out->SetStringField(TEXT("world_kind"), FMCPWorldContext::IsPIEActive() ? TEXT("PIE") : TEXT("Editor"));

	const UPackage* Pkg = World->GetOutermost();
	Out->SetBoolField(TEXT("is_dirty"), Pkg && Pkg->IsDirty());
	return LVL_MakeSuccessObj(Request, Out);
}

// ─── level.load (mutator — PIE-guarded) ──────────────────────────────────────────────────────
//
// Loads ``map_path`` as the new persistent level (closes current). Forwards to
// ULevelEditorSubsystem::LoadLevel.
//
// Args: { "map_path": "/Game/Maps/X", "force": bool (currently advisory — Engine always prompts
// for unsaved dirty packages if force=false; force=true is treated as best-effort and may still
// require user interaction depending on Engine config). }
//
// Response: { "loaded": bool, "map_path": "..." }
FMCPResponse Tool_Load(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (FMCPWorldContext::IsPIEActive())
	{
		return LVL_MakePIEError(Request);
	}

	if (!Request.Args.IsValid())
	{
		return LVL_MakeError(Request, kLVLErrorInvalidParams, TEXT("missing args object"));
	}
	FString MapPath;
	if (!Request.Args->TryGetStringField(TEXT("map_path"), MapPath) || MapPath.IsEmpty())
	{
		return LVL_MakeError(Request, kLVLErrorInvalidParams,
			TEXT("missing required string field 'map_path'"));
	}
	const FString Norm = FMCPWorldContext::NormaliseMapPath(MapPath);
	if (Norm.IsEmpty())
	{
		return LVL_MakeError(Request, kMCPErrorInvalidPath,
			FString::Printf(TEXT("malformed map_path '%s'"), *MapPath));
	}

	// LoadLevel asserts the map exists on disk — pre-flight via FPackageName.
	if (!FPackageName::DoesPackageExist(Norm))
	{
		return LVL_MakeError(Request, kMCPErrorLevelNotFound,
			FString::Printf(TEXT("map_path '%s' does not exist on disk"), *Norm));
	}

	ULevelEditorSubsystem* LES = GEditor ? GEditor->GetEditorSubsystem<ULevelEditorSubsystem>() : nullptr;
	if (!LES)
	{
		return LVL_MakeError(Request, kLVLErrorInternal,
			TEXT("ULevelEditorSubsystem unavailable"));
	}

	const FScopedTransaction Transaction(LOCTEXT("LevelLoad", "MCP: load level"));
	const bool bOk = LES->LoadLevel(Norm);

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("loaded"), bOk);
	Out->SetStringField(TEXT("map_path"), Norm);
	if (!bOk)
	{
		return LVL_MakeError(Request, kLVLErrorInternal,
			FString::Printf(TEXT("LoadLevel('%s') returned false (see editor log)"), *Norm));
	}
	return LVL_MakeSuccessObj(Request, Out);
}

// ─── level.save (mutator — PIE-guarded) ──────────────────────────────────────────────────────
//
// Saves the current persistent level OR a specified sublevel if ``map_path`` is provided.
// Forwards to ULevelEditorSubsystem::SaveCurrentLevel for persistent, or UEditorAssetSubsystem
// for sublevels.
//
// Args: { "map_path": "/Game/Maps/X" (optional — defaults to current persistent) }
//
// Response: { "saved": bool, "map_path": "..." }
FMCPResponse Tool_Save(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (FMCPWorldContext::IsPIEActive())
	{
		return LVL_MakePIEError(Request);
	}

	FString MapPath;
	LVL_GetOptionalMapPathArg(Request, MapPath);

	const FScopedTransaction Transaction(LOCTEXT("LevelSave", "MCP: save level"));

	if (MapPath.IsEmpty())
	{
		// Save current persistent level via LevelEditor subsystem.
		ULevelEditorSubsystem* LES = GEditor ? GEditor->GetEditorSubsystem<ULevelEditorSubsystem>() : nullptr;
		if (!LES)
		{
			return LVL_MakeError(Request, kLVLErrorInternal,
				TEXT("ULevelEditorSubsystem unavailable"));
		}
		const bool bOk = LES->SaveCurrentLevel();
		UWorld* World = FMCPWorldContext::GetEditorWorld();
		const FString CurrentMap = World ? FMCPWorldContext::GetWorldPackagePath(World) : FString();

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("saved"), bOk);
		Out->SetStringField(TEXT("map_path"), CurrentMap);
		if (!bOk)
		{
			return LVL_MakeError(Request, kLVLErrorInternal,
				FString::Printf(TEXT("SaveCurrentLevel('%s') returned false"), *CurrentMap));
		}
		return LVL_MakeSuccessObj(Request, Out);
	}

	// Sublevel path — resolve, save the underlying package via UEditorAssetSubsystem::SaveAsset.
	FMCPResponse ResolveErr;
	ULevel* Level = LVL_ResolveSubLevelArg(Request, /*bRejectPartitioned*/ true, ResolveErr);
	if (!Level)
	{
		return ResolveErr;
	}

	UEditorAssetSubsystem* EAS = GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr;
	if (!EAS)
	{
		return LVL_MakeError(Request, kLVLErrorInternal,
			TEXT("UEditorAssetSubsystem unavailable"));
	}
	const FString Norm = FMCPWorldContext::NormaliseMapPath(MapPath);
	const bool bOk = EAS->SaveAsset(Norm, /*bOnlyIfIsDirty*/ false);

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("saved"), bOk);
	Out->SetStringField(TEXT("map_path"), Norm);
	if (!bOk)
	{
		return LVL_MakeError(Request, kLVLErrorInternal,
			FString::Printf(TEXT("SaveAsset('%s') returned false"), *Norm));
	}
	return LVL_MakeSuccessObj(Request, Out);
}

// ─── level.create (mutator — PIE-guarded) ────────────────────────────────────────────────────
//
// Creates a new persistent level at ``map_path``. Optional ``template`` arg specifies an existing
// level to clone (forwards to ULevelEditorSubsystem::NewLevelFromTemplate); else creates a blank
// level (NewLevel). NEVER passes bIsPartitionedWorld=true — Phase 3 does not yet support WP maps.
//
// Args: { "map_path": "/Game/Maps/NewMap", "template": "/Game/Maps/X" (optional) }
//
// Response: { "created": bool, "map_path": "..." }
FMCPResponse Tool_Create(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (FMCPWorldContext::IsPIEActive())
	{
		return LVL_MakePIEError(Request);
	}

	if (!Request.Args.IsValid())
	{
		return LVL_MakeError(Request, kLVLErrorInvalidParams, TEXT("missing args object"));
	}
	FString MapPath;
	if (!Request.Args->TryGetStringField(TEXT("map_path"), MapPath) || MapPath.IsEmpty())
	{
		return LVL_MakeError(Request, kLVLErrorInvalidParams,
			TEXT("missing required string field 'map_path'"));
	}
	const FString Norm = FMCPWorldContext::NormaliseMapPath(MapPath);
	if (Norm.IsEmpty())
	{
		return LVL_MakeError(Request, kMCPErrorInvalidPath,
			FString::Printf(TEXT("malformed map_path '%s'"), *MapPath));
	}

	FString TemplatePath;
	Request.Args->TryGetStringField(TEXT("template"), TemplatePath);
	FString TemplateNorm;
	if (!TemplatePath.IsEmpty())
	{
		TemplateNorm = FMCPWorldContext::NormaliseMapPath(TemplatePath);
		if (TemplateNorm.IsEmpty())
		{
			return LVL_MakeError(Request, kMCPErrorInvalidPath,
				FString::Printf(TEXT("malformed template path '%s'"), *TemplatePath));
		}
		if (!FPackageName::DoesPackageExist(TemplateNorm))
		{
			return LVL_MakeError(Request, kMCPErrorLevelNotFound,
				FString::Printf(TEXT("template '%s' does not exist on disk"), *TemplateNorm));
		}
	}

	ULevelEditorSubsystem* LES = GEditor ? GEditor->GetEditorSubsystem<ULevelEditorSubsystem>() : nullptr;
	if (!LES)
	{
		return LVL_MakeError(Request, kLVLErrorInternal,
			TEXT("ULevelEditorSubsystem unavailable"));
	}

	const FScopedTransaction Transaction(LOCTEXT("LevelCreate", "MCP: create level"));
	const bool bOk = TemplateNorm.IsEmpty()
		? LES->NewLevel(Norm, /*bIsPartitionedWorld*/ false)
		: LES->NewLevelFromTemplate(Norm, TemplateNorm);

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("created"), bOk);
	Out->SetStringField(TEXT("map_path"), Norm);
	if (!bOk)
	{
		return LVL_MakeError(Request, kLVLErrorInternal,
			FString::Printf(TEXT("NewLevel('%s') returned false (path already exists? template missing?)"), *Norm));
	}
	return LVL_MakeSuccessObj(Request, Out);
}

// ─── level.unload (mutator — PIE-guarded) ────────────────────────────────────────────────────
//
// Removes a sublevel from the world. Forwards to UEditorLevelUtils::RemoveLevelFromWorld.
// CANNOT unload the persistent level — refuses with kMCPErrorLevelNotStreamingEntry.
//
// Args: { "map_path": "/Game/Maps/SubX" }
//
// Response: { "unloaded": bool, "map_path": "..." }
FMCPResponse Tool_Unload(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (FMCPWorldContext::IsPIEActive())
	{
		return LVL_MakePIEError(Request);
	}

	FMCPResponse ResolveErr;
	ULevel* Level = LVL_ResolveSubLevelArg(Request, /*bRejectPartitioned*/ true, ResolveErr);
	if (!Level)
	{
		return ResolveErr;
	}

	UWorld* World = FMCPWorldContext::GetEditorWorld();
	check(World);
	if (Level == World->PersistentLevel)
	{
		return LVL_MakeError(Request, kMCPErrorLevelNotStreamingEntry,
			TEXT("cannot unload the persistent level — use level.load to switch maps instead"));
	}

	const FScopedTransaction Transaction(LOCTEXT("LevelUnload", "MCP: unload level"));
	const bool bOk = UEditorLevelUtils::RemoveLevelFromWorld(Level);

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("unloaded"), bOk);
	if (UPackage* Pkg = Level->GetOutermost())
	{
		Out->SetStringField(TEXT("map_path"), Pkg->GetName());
	}
	if (!bOk)
	{
		return LVL_MakeError(Request, kLVLErrorInternal,
			TEXT("RemoveLevelFromWorld returned false"));
	}
	return LVL_MakeSuccessObj(Request, Out);
}

// ─── level.set_streaming_state (mutator — PIE-guarded) ───────────────────────────────────────
//
// Sets the loaded + visible bits on a streaming-level entry. The target MUST already be in
// World->GetStreamingLevels() — refuses with kMCPErrorLevelNotStreamingEntry (-32028) otherwise.
//
// Args: { "level_path": "/Game/Maps/SubX", "loaded": bool, "visible": bool }
//
// Response: { "ok": true, "level_path": "...", "loaded": bool, "visible": bool }
FMCPResponse Tool_SetStreamingState(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (FMCPWorldContext::IsPIEActive())
	{
		return LVL_MakePIEError(Request);
	}

	if (!Request.Args.IsValid())
	{
		return LVL_MakeError(Request, kLVLErrorInvalidParams, TEXT("missing args object"));
	}
	FString LevelPath;
	if (!Request.Args->TryGetStringField(TEXT("level_path"), LevelPath) || LevelPath.IsEmpty())
	{
		return LVL_MakeError(Request, kLVLErrorInvalidParams,
			TEXT("missing required string field 'level_path'"));
	}
	bool bLoaded = true;
	bool bVisible = true;
	Request.Args->TryGetBoolField(TEXT("loaded"), bLoaded);
	Request.Args->TryGetBoolField(TEXT("visible"), bVisible);

	UWorld* World = FMCPWorldContext::GetEditorWorld();
	if (!World)
	{
		return LVL_MakeError(Request, kMCPErrorLevelNotFound,
			TEXT("no editor world available"));
	}
	if (World->IsPartitionedWorld())
	{
		return LVL_MakeError(Request, kMCPErrorWorldPartitionNotSupported,
			TEXT("editor world is partitioned — Phase 5 wp.* tools required for streaming state"));
	}
	ULevelStreaming* Streaming = LVL_FindStreamingEntry(World, LevelPath);
	if (!Streaming)
	{
		return LVL_MakeError(Request, kMCPErrorLevelNotStreamingEntry,
			FString::Printf(
				TEXT("level '%s' is not in World->GetStreamingLevels() — only sublevels added via the streaming list can have their state toggled"),
				*LevelPath));
	}

	const FScopedTransaction Transaction(LOCTEXT("LevelSetStreamingState", "MCP: set streaming state"));
	Streaming->SetShouldBeLoaded(bLoaded);
	Streaming->SetShouldBeVisible(bVisible);

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("ok"), true);
	Out->SetStringField(TEXT("level_path"), FMCPWorldContext::NormaliseMapPath(LevelPath));
	Out->SetBoolField(TEXT("loaded"), bLoaded);
	Out->SetBoolField(TEXT("visible"), bVisible);
	return LVL_MakeSuccessObj(Request, Out);
}

// ─── level.get_world_settings (read-only — works in PIE) ─────────────────────────────────────
//
// Returns the writable subset of WorldSettings (per D7 canonical list) for the chosen world.
// Read-only: PIE-safe.
//
// Args: { "map_path": "..." (optional — currently MUST be empty or equal to current world path;
//   future enhancement: per-sublevel WorldSettings query). }
//
// Response: { "map_path": "...", "properties": { KillZ, WorldGravityZ, ..., TimeDilation } }
FMCPResponse Tool_GetWorldSettings(const FMCPRequest& Request)
{
	check(IsInGameThread());

	UWorld* World = LVL_ResolveReadWorld();
	if (!World)
	{
		return LVL_MakeError(Request, kMCPErrorLevelNotFound,
			TEXT("no world available"));
	}

	// Optional map_path: refuse if it doesn't match the current world (Phase 3 doesn't yet route
	// to per-sublevel WorldSettings — every sublevel can have its own AWorldSettings but the
	// resolution is non-trivial; defer to a later phase if needed).
	FString MapPath;
	LVL_GetOptionalMapPathArg(Request, MapPath);
	if (!MapPath.IsEmpty())
	{
		const FString Norm = FMCPWorldContext::NormaliseMapPath(MapPath);
		const FString CurMap = FMCPWorldContext::GetWorldPackagePath(World);
		if (!Norm.Equals(CurMap, ESearchCase::IgnoreCase))
		{
			return LVL_MakeError(Request, kMCPErrorLevelNotFound,
				FString::Printf(
					TEXT("map_path '%s' does not match current world '%s' — per-sublevel WorldSettings query not yet implemented"),
					*Norm, *CurMap));
		}
	}

	AWorldSettings* Settings = World->GetWorldSettings(/*bCheckStreamingPersistent*/ false, /*bChecked*/ false);
	if (!Settings)
	{
		return LVL_MakeError(Request, kLVLErrorInternal,
			TEXT("World->GetWorldSettings returned null"));
	}

	// Canonical writable subset per D7. Emit ALL of these as JSON via the marshalling reflection
	// helper so the wire shape is symmetric with marshall.read_property + set_world_settings.
	static const TCHAR* const kWritableFields[] = {
		TEXT("KillZ"),
		TEXT("WorldGravityZ"),
		TEXT("bGlobalGravitySet"),
		TEXT("DefaultGameMode"),
		TEXT("TimeDilation"),
		TEXT("bEnableWorldComposition"),
		TEXT("DefaultColorScale"),
	};

	TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
	UClass* SettingsClass = Settings->GetClass();
	for (const TCHAR* FieldName : kWritableFields)
	{
		FProperty* Prop = SettingsClass->FindPropertyByName(FName(FieldName));
		if (!Prop)
		{
			// Property may be cooked-out / version-skewed; emit null entry so the caller can see
			// the field was probed.
			Properties->SetField(FieldName, MakeShared<FJsonValueNull>());
			continue;
		}
		TSharedPtr<FJsonValue> Value = FMCPReflection::ReadPropertyValue(Settings, Prop);
		Properties->SetField(FieldName, Value.IsValid() ? Value : MakeShared<FJsonValueNull>());
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("map_path"), FMCPWorldContext::GetWorldPackagePath(World));
	Out->SetObjectField(TEXT("properties"), Properties);
	return LVL_MakeSuccessObj(Request, Out);
}

// ─── level.set_world_settings (mutator — PIE-guarded) ────────────────────────────────────────
//
// Writes one or more fields from the canonical writable subset. Fields outside the subset are
// rejected with kMCPErrorWorldSettingsReadonly (deferred from Day 4 — Phase 3 declared but not
// yet wired; for now use a generic internal-error code with descriptive message).
//
// Args: { "map_path": "..." (optional), "properties": { KillZ: 100.0, TimeDilation: 0.5, ... } }
//
// Response: { "ok": true, "map_path": "...", "applied": ["KillZ", "TimeDilation"], "rejected": [...] }
FMCPResponse Tool_SetWorldSettings(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (FMCPWorldContext::IsPIEActive())
	{
		return LVL_MakePIEError(Request);
	}

	if (!Request.Args.IsValid())
	{
		return LVL_MakeError(Request, kLVLErrorInvalidParams, TEXT("missing args object"));
	}

	const TSharedPtr<FJsonObject>* PropsPtr = nullptr;
	if (!Request.Args->TryGetObjectField(TEXT("properties"), PropsPtr) || !PropsPtr->IsValid())
	{
		return LVL_MakeError(Request, kLVLErrorInvalidParams,
			TEXT("missing required object field 'properties'"));
	}

	UWorld* World = FMCPWorldContext::GetEditorWorld();
	if (!World)
	{
		return LVL_MakeError(Request, kMCPErrorLevelNotFound,
			TEXT("no editor world available"));
	}

	AWorldSettings* Settings = World->GetWorldSettings(/*bCheckStreamingPersistent*/ false, /*bChecked*/ false);
	if (!Settings)
	{
		return LVL_MakeError(Request, kLVLErrorInternal,
			TEXT("World->GetWorldSettings returned null"));
	}

	// Canonical writable subset gate. Anything outside this set is hard-rejected (it goes into
	// the rejected[] array with reason="not in writable subset"). Per D7.
	static const TSet<FString> kWritableSet({
		TEXT("KillZ"),
		TEXT("WorldGravityZ"),
		TEXT("bGlobalGravitySet"),
		TEXT("DefaultGameMode"),
		TEXT("TimeDilation"),
		TEXT("bEnableWorldComposition"),
		TEXT("DefaultColorScale"),
	});

	UClass* SettingsClass = Settings->GetClass();
	TArray<TSharedPtr<FJsonValue>> Applied;
	TArray<TSharedPtr<FJsonValue>> Rejected;

	auto MakeRejected = [](const FString& Name, const FString& Reason) -> TSharedPtr<FJsonValue>
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Name);
		Obj->SetStringField(TEXT("reason"), Reason);
		return MakeShared<FJsonValueObject>(Obj);
	};

	for (const auto& Pair : (*PropsPtr)->Values)
	{
		const FString& Name = Pair.Key;
		if (!kWritableSet.Contains(Name))
		{
			Rejected.Add(MakeRejected(Name, TEXT("not in writable subset")));
			continue;
		}
		FProperty* Prop = SettingsClass->FindPropertyByName(FName(*Name));
		if (!Prop)
		{
			Rejected.Add(MakeRejected(Name, TEXT("FindPropertyByName returned null")));
			continue;
		}
		// Edit-const guard (mirror Day 0 reflection contract).
		if (Prop->HasAnyPropertyFlags(CPF_EditConst | CPF_BlueprintReadOnly | CPF_DisableEditOnInstance))
		{
			Rejected.Add(MakeRejected(Name, TEXT("property is edit-const")));
			continue;
		}

		// Scope owns Pre/Modify/Transaction; dtor fires PostEditChangeProperty.
		FString WriteErr;
		{
			FMCPWritePropertyScope Scope(Settings, Prop, LOCTEXT("SetWorldSetting", "MCP: set world setting"));
			if (!FMCPReflection::WritePropertyValue(Settings, Prop, Pair.Value, WriteErr))
			{
				Rejected.Add(MakeRejected(Name, WriteErr));
				continue;
			}
		}
		Applied.Add(MakeShared<FJsonValueString>(Name));
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("ok"), true);
	Out->SetStringField(TEXT("map_path"), FMCPWorldContext::GetWorldPackagePath(World));
	Out->SetArrayField(TEXT("applied"), Applied);
	Out->SetArrayField(TEXT("rejected"), Rejected);
	return LVL_MakeSuccessObj(Request, Out);
}

// ─── level.get_persistent_level_actors (read-only, paginated — works in PIE) ─────────────────
//
// Keyset-paginated iteration over the persistent level's Actors[] array (or the sublevel resolved
// via map_path). Cursor = opaque FMCPPageCursor (base64 JSON: filter_hash + last actor PathName +
// total_known snapshot). Survives mid-pagination level mutations: new actors land in their sorted
// slot, deleted actors leave a gap the next-greater key fills naturally.
//
// Filter-hash inputs: the resolved level's package name. ``page_size`` may change between pages.
// Switching ``map_path`` → -32015 STALE_CURSOR.
//
// Args: { "map_path": "..." (optional, defaults to persistent level), "page_size": int (default 200),
//   "page_token": "..." (opaque base64; null/empty → first page). }
//
// Response: { "map_path": "...", "actors": [...], "next_page_token": str|null, "total_known": N }
FMCPResponse Tool_GetPersistentLevelActors(const FMCPRequest& Request)
{
	check(IsInGameThread());

	UWorld* World = LVL_ResolveReadWorld();
	if (!World)
	{
		return LVL_MakeError(Request, kMCPErrorLevelNotFound,
			TEXT("no world available"));
	}

	ULevel* Level = World->PersistentLevel;
	FString MapPath;
	LVL_GetOptionalMapPathArg(Request, MapPath);
	if (!MapPath.IsEmpty())
	{
		Level = FMCPWorldContext::ResolveLevelOrNull(World, MapPath);
		if (!Level)
		{
			return LVL_MakeError(Request, kMCPErrorLevelNotFound,
				FString::Printf(TEXT("map_path '%s' does not resolve to a loaded level"), *MapPath));
		}
	}
	if (!Level)
	{
		return LVL_MakeError(Request, kMCPErrorLevelNotFound, TEXT("no persistent level on world"));
	}

	const int32 PageSize = LVL_ClampPageSize(Request.Args, TEXT("page_size"), 200);

	// Filter hash binds the result-set identity to the resolved level package name. Caller can
	// omit map_path (defaults to persistent) on page 1 and pass the persistent's path explicitly
	// on page 2 — both resolve to the same package, same hash, cursor stays valid.
	const FString ResolvedLevelPkg = Level->GetOutermost()->GetName();
	const uint64 FilterHash = LVL_HashLevelFilter(ResolvedLevelPkg);

	FString PageTokenWire;
	if (Request.Args.IsValid())
	{
		Request.Args->TryGetStringField(TEXT("page_token"), PageTokenWire);
	}
	FMCPPageCursor Cursor;
	FMCPResponse CursorErr;
	if (!LVL_DecodeCursor(Request, PageTokenWire, FilterHash, Cursor, CursorErr))
	{
		return CursorErr;
	}

	// Build a stable sorted view. ``Level->Actors`` may contain null entries (post-destroy gaps);
	// drop them up front so the sort key & sentinel comparison are well-defined.
	const TArray<AActor*>& AllActors = Level->Actors;
	TArray<AActor*> Sorted;
	Sorted.Reserve(AllActors.Num());
	for (AActor* A : AllActors)
	{
		if (A)
		{
			Sorted.Add(A);
		}
	}
	Sorted.StableSort([](const AActor& A, const AActor& B)
	{
		return A.GetPathName().Compare(B.GetPathName(), ESearchCase::IgnoreCase) < 0;
	});

	const int32 TotalKnown = Sorted.Num();

	// Skip-until-past-sentinel. O(N) linear scan — matches the AR pattern; for typical level sizes
	// (<10k actors) this is negligible vs the JSON marshalling cost.
	int32 StartIdx = 0;
	if (!Cursor.LastAssetPath.IsEmpty())
	{
		while (StartIdx < TotalKnown)
		{
			if (Sorted[StartIdx]->GetPathName().Compare(Cursor.LastAssetPath, ESearchCase::IgnoreCase) > 0)
			{
				break;
			}
			++StartIdx;
		}
	}
	const int32 EndExcl = FMath::Min(TotalKnown, StartIdx + PageSize);

	TArray<TSharedPtr<FJsonValue>> Out;
	Out.Reserve(EndExcl - StartIdx);
	FString NextSentinel;
	for (int32 i = StartIdx; i < EndExcl; ++i)
	{
		Out.Add(MakeShared<FJsonValueObject>(LVL_BuildActorSummary(Sorted[i])));
	}
	if (EndExcl < TotalKnown && Out.Num() > 0)
	{
		NextSentinel = Sorted[EndExcl - 1]->GetPathName();
	}

	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("map_path"), ResolvedLevelPkg);
	Obj->SetArrayField(TEXT("actors"), Out);
	Obj->SetNumberField(TEXT("total_known"), static_cast<double>(TotalKnown));
	if (NextSentinel.IsEmpty())
	{
		Obj->SetField(TEXT("next_page_token"), MakeShared<FJsonValueNull>());
	}
	else
	{
		FMCPPageCursor NewCursor;
		NewCursor.FilterHash = FilterHash;
		NewCursor.LastAssetPath = NextSentinel;
		NewCursor.TotalKnownSnapshot = TotalKnown;
		Obj->SetStringField(TEXT("next_page_token"), FMCPPageCursorUtils::Encode(NewCursor));
	}
	return LVL_MakeSuccessObj(Request, Obj);
}

// ─── level.save_all_dirty (Lane A + inline SubmitJob — per plan D3 N7 pattern) ───────────────
//
// Returns { job_id } immediately. Job body iterates dirty *map* packages (NOT content packages —
// this is the level-tool variant, distinct from Phase 2's cb.save_all_dirty). Cooperative cancel
// via FMCPJob::bCancelRequested; progress updates every 256 iterations.
//
// PIE-guarded — the underlying SavePackages call would fail anyway, but we surface the frozen
// message rather than wait for the implicit failure.
FMCPResponse Tool_SaveAllDirty(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (FMCPWorldContext::IsPIEActive())
	{
		return LVL_MakePIEError(Request);
	}

	const FGuid JobId = FMCPJobRegistry::Get().SubmitJob(
		TEXT("level.save_all_dirty"),
		[](FMCPJob& Job) -> TSharedPtr<FJsonValue>
		{
			TArray<UPackage*> Dirty;
			UEditorLoadingAndSavingUtils::GetDirtyMapPackages(Dirty);

			TArray<TSharedPtr<FJsonValue>> Saved;
			TArray<TSharedPtr<FJsonValue>> Failed;
			Saved.Reserve(Dirty.Num());
			Failed.Reserve(Dirty.Num());

			const int32 Total = Dirty.Num();
			for (int32 i = 0; i < Total; ++i)
			{
				// Cooperative cancellation between packages.
				if (Job.bCancelRequested.load(std::memory_order_acquire))
				{
					Job.ErrorMessage = TEXT("cancelled");
					return nullptr;
				}

				// Progress cadence: every 256 packages (in practice projects rarely have that many
				// dirty maps, but cheap enough to emit unconditionally).
				if ((i & 0xFF) == 0)
				{
					Job.Progress.store(static_cast<float>(i) / FMath::Max(1, Total),
						std::memory_order_release);
				}

				UPackage* Pkg = Dirty[i];
				if (!Pkg)
				{
					continue;
				}
				const bool bOk = UEditorLoadingAndSavingUtils::SavePackages({ Pkg }, /*bOnlyDirty*/ false);
				if (bOk)
				{
					Saved.Add(MakeShared<FJsonValueString>(Pkg->GetName()));
				}
				else
				{
					TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
					Obj->SetStringField(TEXT("package"), Pkg->GetName());
					Obj->SetStringField(TEXT("error"), TEXT("SavePackages returned false"));
					Failed.Add(MakeShared<FJsonValueObject>(Obj));
				}
			}

			Job.Progress.store(1.0f, std::memory_order_release);

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetArrayField(TEXT("saved"), Saved);
			Out->SetArrayField(TEXT("failed"), Failed);
			Out->SetNumberField(TEXT("total_dirty"), static_cast<double>(Total));
			return MakeShared<FJsonValueObject>(Out);
		},
		/*bGameThreadRequired*/ true);

	if (!JobId.IsValid())
	{
		return LVL_MakeError(Request, kMCPErrorJobSubmitFailed,
			TEXT("FMCPJobRegistry::SubmitJob refused (shutdown?)"));
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("job_id"), JobId.ToString(EGuidFormats::DigitsWithHyphens));
	return LVL_MakeSuccessObj(Request, Out);
}

// ─── level.duplicate (mutator — PIE-guarded) ─────────────────────────────────────────────────
//
// Duplicates a map to a new asset path via UEditorAssetSubsystem::DuplicateAsset. The source
// must exist on disk; the destination must NOT exist (otherwise kMCPErrorPathInUse).
//
// Args: { "source_map": "/Game/Maps/Src", "dest_map": "/Game/Maps/Dst" }
//
// Response: { "duplicated": bool, "source_map": "...", "dest_map": "..." }
FMCPResponse Tool_Duplicate(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (FMCPWorldContext::IsPIEActive())
	{
		return LVL_MakePIEError(Request);
	}

	if (!Request.Args.IsValid())
	{
		return LVL_MakeError(Request, kLVLErrorInvalidParams, TEXT("missing args object"));
	}
	FString SourceMap, DestMap;
	if (!Request.Args->TryGetStringField(TEXT("source_map"), SourceMap) || SourceMap.IsEmpty())
	{
		return LVL_MakeError(Request, kLVLErrorInvalidParams,
			TEXT("missing required string field 'source_map'"));
	}
	if (!Request.Args->TryGetStringField(TEXT("dest_map"), DestMap) || DestMap.IsEmpty())
	{
		return LVL_MakeError(Request, kLVLErrorInvalidParams,
			TEXT("missing required string field 'dest_map'"));
	}
	const FString SrcNorm = FMCPWorldContext::NormaliseMapPath(SourceMap);
	const FString DstNorm = FMCPWorldContext::NormaliseMapPath(DestMap);
	if (SrcNorm.IsEmpty() || DstNorm.IsEmpty())
	{
		return LVL_MakeError(Request, kMCPErrorInvalidPath,
			TEXT("source_map and dest_map must be valid /Game/... paths"));
	}
	if (SrcNorm.Equals(DstNorm, ESearchCase::IgnoreCase))
	{
		return LVL_MakeError(Request, kLVLErrorInvalidParams,
			TEXT("source_map and dest_map must differ"));
	}
	if (!FPackageName::DoesPackageExist(SrcNorm))
	{
		return LVL_MakeError(Request, kMCPErrorLevelNotFound,
			FString::Printf(TEXT("source_map '%s' does not exist on disk"), *SrcNorm));
	}
	if (FPackageName::DoesPackageExist(DstNorm))
	{
		return LVL_MakeError(Request, kMCPErrorPathInUse,
			FString::Printf(TEXT("dest_map '%s' already exists on disk"), *DstNorm));
	}

	UEditorAssetSubsystem* EAS = GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr;
	if (!EAS)
	{
		return LVL_MakeError(Request, kLVLErrorInternal,
			TEXT("UEditorAssetSubsystem unavailable"));
	}

	const FScopedTransaction Transaction(LOCTEXT("LevelDuplicate", "MCP: duplicate level"));
	UObject* Result = EAS->DuplicateAsset(SrcNorm, DstNorm);
	const bool bOk = Result != nullptr;

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("duplicated"), bOk);
	Out->SetStringField(TEXT("source_map"), SrcNorm);
	Out->SetStringField(TEXT("dest_map"), DstNorm);
	if (!bOk)
	{
		return LVL_MakeError(Request, kLVLErrorInternal,
			FString::Printf(TEXT("DuplicateAsset('%s' → '%s') returned null"), *SrcNorm, *DstNorm));
	}
	return LVL_MakeSuccessObj(Request, Out);
}

// ─── Registration ────────────────────────────────────────────────────────────────────────────
void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	// Lane B sanity (kept around as dev utility — leading underscore convention keeps it out of
	// tools.list per existing Phase 2 hotfix-3 filter).
	RegisterTool(TEXT("_phase3_lane_b_sanity"), &Tool_Phase3LaneBSanity, /*Lane B*/ true);

	// Day 1: read-only enumeration.
	RegisterTool(TEXT("level.list_loaded"), &Tool_ListLoaded, /*Lane A*/ false);
	RegisterTool(TEXT("level.current_map"), &Tool_CurrentMap, /*Lane A*/ false);

	// Day 2: lifecycle mutators + streaming.
	RegisterTool(TEXT("level.load"),                &Tool_Load,               /*Lane A*/ false);
	RegisterTool(TEXT("level.save"),                &Tool_Save,               /*Lane A*/ false);
	RegisterTool(TEXT("level.create"),              &Tool_Create,             /*Lane A*/ false);
	RegisterTool(TEXT("level.unload"),              &Tool_Unload,             /*Lane A*/ false);
	RegisterTool(TEXT("level.set_streaming_state"), &Tool_SetStreamingState,  /*Lane A*/ false);

	// Day 3: settings + bulk operations.
	RegisterTool(TEXT("level.get_world_settings"),          &Tool_GetWorldSettings,         /*Lane A*/ false);
	RegisterTool(TEXT("level.set_world_settings"),          &Tool_SetWorldSettings,         /*Lane A*/ false);
	RegisterTool(TEXT("level.get_persistent_level_actors"), &Tool_GetPersistentLevelActors, /*Lane A*/ false);
	RegisterTool(TEXT("level.save_all_dirty"),              &Tool_SaveAllDirty,             /*Lane A*/ false);
	RegisterTool(TEXT("level.duplicate"),                   &Tool_Duplicate,                /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("Phase 3 Days 1-3: registered 12 level.* handlers (all Lane A) + 1 sanity probe (_phase3_lane_b_sanity, Lane B)"));
}

} // namespace FLevelTools

#undef LOCTEXT_NAMESPACE
