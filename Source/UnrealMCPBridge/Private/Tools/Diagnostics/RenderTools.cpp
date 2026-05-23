// Copyright FatumGame. All Rights Reserved.

#include "RenderTools.h"

#include "MCPSurfaceRegistry.h"

#include "FMCPDispatchQueue.h"
#include "MCPJsonBuilder.h"
#include "MCPMutatorScope.h"
#include "MCPToolHelpers.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPActorPathUtils.h"
#include "Utils/MCPReflection.h"
#include "Utils/MCPWorldContext.h"

#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/Scene.h"
#include "GameFramework/Actor.h"
#include "LevelEditorViewport.h"
#include "ShowFlags.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// RND_ prefix per the unity-build symbol-collision convention. The four shared helpers
	// (StampIds / MakeError / MakeSuccessObj / RequireStringField) live in FMCPToolHelpers — see
	// Phase 1 helper extraction (commit b2fd19d). PIE-guard now lives in FMCPMutatorScope.
	constexpr int32 kRNDErrorInvalidParams = -32602;
	constexpr int32 kRNDErrorInternal      = -32603;

	// ─── Viewport resolution (mirror of ViewportTools VPT_ResolveViewport semantics) ────────────────

	/**
	 * Resolve a viewport client by index against ``GEditor->GetLevelViewportClients()``. Returns
	 * null + populates ``OutErr`` on failure. Default index is 0 when caller omits the field.
	 *
	 * Surfaces:
	 *   -32603 Internal             GEditor missing
	 *   -32004 ObjectNotFound       editor has no level viewport clients yet
	 *   -32026 PropertyIndexOOB     index out of [0, Count)
	 */
	FLevelEditorViewportClient* RND_ResolveViewport(const FMCPRequest& Request, FMCPResponse& OutErr)
	{
		check(IsInGameThread());

		if (!GEditor)
		{
			OutErr = FMCPToolHelpers::MakeError(Request, kRNDErrorInternal,
				TEXT("GEditor unavailable (commandlet?)"));
			return nullptr;
		}

		const TArray<FLevelEditorViewportClient*>& Clients = GEditor->GetLevelViewportClients();
		if (Clients.Num() == 0)
		{
			OutErr = FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
				TEXT("no level viewports available (open a level editor tab first)"));
			return nullptr;
		}

		int32 Index = 0;
		if (Request.Args.IsValid())
		{
			double Raw = 0.0;
			if (Request.Args->TryGetNumberField(TEXT("viewport_index"), Raw))
			{
				Index = static_cast<int32>(Raw);
			}
		}
		if (Index < 0 || Index >= Clients.Num())
		{
			OutErr = FMCPToolHelpers::MakeError(Request, kMCPErrorPropertyIndexOOB,
				FString::Printf(
					TEXT("viewport_index %d out of range [0, %d)"),
					Index, Clients.Num()));
			return nullptr;
		}

		FLevelEditorViewportClient* VC = Clients[Index];
		if (!VC)
		{
			OutErr = FMCPToolHelpers::MakeError(Request, kRNDErrorInternal,
				FString::Printf(TEXT("level viewport client at index %d is null"), Index));
			return nullptr;
		}
		return VC;
	}

	// ─── Show-flag enumeration sink ────────────────────────────────────────────────────────────────

	/**
	 * FEngineShowFlags::IterateAllFlags expects a sink class with
	 *   ``bool OnEngineShowFlag(uint32 InIndex, const FString& InName)``  // continue iteration if true
	 *   ``bool OnCustomShowFlag(uint32 InIndex, const FString& InName)``  // ditto
	 *
	 * We push (index, name) pairs into a flat array; the caller then queries
	 * ``Flags.GetSingleFlag(Index)`` to assemble each ``enabled`` value. Both engine-side and
	 * custom flags are kept so AI callers see the same surface the editor's show-flag menu exposes.
	 *
	 * IMPORTANT: ``IterateCustomFlags`` (called internally by ``IterateAllFlags``) already passes
	 * ``Idx + SF_FirstCustom`` as the index — see ShowFlags.cpp:1099. Do NOT add SF_FirstCustom
	 * again here, or the index lands past the registered CustomShowFlags array bounds and
	 * ``GetSingleFlag`` hits ``checkNoEntry()`` at ShowFlags.cpp:162.
	 */
	struct FRNDShowFlagSink
	{
		struct FEntry { uint32 Index; FString Name; };
		TArray<FEntry> Entries;

		bool OnEngineShowFlag(uint32 InIndex, const FString& InName)
		{
			Entries.Add({ InIndex, InName });
			return true;
		}

		bool OnCustomShowFlag(uint32 InIndex, const FString& InName)
		{
			Entries.Add({ InIndex, InName });
			return true;
		}
	};

	// ─── Post-process property write helper ────────────────────────────────────────────────────────

	/**
	 * Strip the optional ``Settings.`` prefix so callers can pass either ``Settings.AutoExposureBias``
	 * or just ``AutoExposureBias`` and the lookup hits the same FPostProcessSettings field.
	 */
	FString RND_NormalisePostProcessFieldName(const FString& Input)
	{
		const FString Prefix = TEXT("Settings.");
		if (Input.StartsWith(Prefix, ESearchCase::IgnoreCase))
		{
			return Input.RightChop(Prefix.Len());
		}
		return Input;
	}

	/** Snapshot a JSON value of the named PP property (for prior/new diff in the response). */
	TSharedPtr<FJsonValue> RND_ReadPostProcessField(
		FPostProcessSettings& Settings,
		FProperty* Prop)
	{
		void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(&Settings);
		return FMCPReflection::ReadPropertyValueAt(Prop, ValuePtr);
	}
} // namespace

namespace FRenderTools
{

// ─── render.list_show_flags ────────────────────────────────────────────────────────────────────
//
// Args: { viewport_index?: int (default 0) }
// Result: {
//   viewport_index, count: int,
//   flags: [ { name, enabled: bool }, ... ]
// }
FMCPResponse Tool_ListShowFlags(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPResponse Err;
	FLevelEditorViewportClient* VC = RND_ResolveViewport(Request, Err);
	if (!VC) { return Err; }

	FRNDShowFlagSink Sink;
	FEngineShowFlags::IterateAllFlags(Sink);

	const FEngineShowFlags& Flags = VC->EngineShowFlags;
	TArray<TSharedPtr<FJsonValue>> Arr;
	Arr.Reserve(Sink.Entries.Num());
	for (const FRNDShowFlagSink::FEntry& Entry : Sink.Entries)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Entry.Name);
		Obj->SetBoolField(TEXT("enabled"), Flags.GetSingleFlag(Entry.Index));
		Arr.Add(MakeShared<FJsonValueObject>(Obj));
	}

	const int32 VPIdx = GEditor ? GEditor->GetLevelViewportClients().IndexOfByKey(VC) : INDEX_NONE;

	const int32 ArrNum = Arr.Num();
	return FMCPJsonBuilder()
		.Num(TEXT("viewport_index"), VPIdx)
		.Num(TEXT("count"), ArrNum)
		.Arr(TEXT("flags"), MoveTemp(Arr))
		.BuildSuccess(Request);
}

// ─── render.set_show_flag ──────────────────────────────────────────────────────────────────────
//
// Args: { flag_name: string, enabled: bool, viewport_index?: int (default 0) }
// Result: { viewport_index, flag_name, prior_enabled: bool, new_enabled: bool }
//
// Uses ``FEngineShowFlags::FindIndexByName`` for case-insensitive name lookup. Unknown name → -32004.
// After mutation we call ``Invalidate()`` so the viewport redraws with the new flag state immediately.
FMCPResponse Tool_SetShowFlag(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kRNDErrorInvalidParams, TEXT("missing args object"));
	}

	FString FlagName;
	if (!Request.Args->TryGetStringField(TEXT("flag_name"), FlagName) || FlagName.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kRNDErrorInvalidParams,
			TEXT("missing required string field 'flag_name'"));
	}

	bool bEnabled = false;
	if (!Request.Args->TryGetBoolField(TEXT("enabled"), bEnabled))
	{
		return FMCPToolHelpers::MakeError(Request, kRNDErrorInvalidParams,
			TEXT("missing required bool field 'enabled'"));
	}

	FMCPResponse Err;
	FLevelEditorViewportClient* VC = RND_ResolveViewport(Request, Err);
	if (!VC) { return Err; }

	FEngineShowFlags& Flags = VC->EngineShowFlags;
	const int32 FlagIdx = FEngineShowFlags::FindIndexByName(*FlagName);
	if (FlagIdx == INDEX_NONE)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("show flag '%s' not found; use render.list_show_flags to enumerate"),
				*FlagName));
	}

	const bool bPrior = Flags.GetSingleFlag(static_cast<uint32>(FlagIdx));
	Flags.SetSingleFlag(static_cast<uint32>(FlagIdx), bEnabled);
	VC->Invalidate();
	const bool bNew = Flags.GetSingleFlag(static_cast<uint32>(FlagIdx));

	const int32 VPIdx = GEditor->GetLevelViewportClients().IndexOfByKey(VC);

	return FMCPJsonBuilder()
		.Num(TEXT("viewport_index"), VPIdx)
		.Str(TEXT("flag_name"), FlagName)
		.Bool(TEXT("prior_enabled"), bPrior)
		.Bool(TEXT("new_enabled"), bNew)
		.BuildSuccess(Request);
}

// ─── render.set_engine_stat ────────────────────────────────────────────────────────────────────
//
// Args: { stat_name: string, enabled: bool, viewport_index?: int (default 0) }
// Result: { stat_name, enabled, world_kind: "PIE"|"Editor"|... }
//
// Wraps ``GEngine->SetEngineStat(World, ViewportClient, *Name, bShow)``. UE doesn't expose a
// "is this stat registered" introspection API — unknown names are silently ignored at the GEngine
// layer (same behaviour as the ``stat <name>`` console command). We therefore do not surface
// -32004 for unrecognised stats; callers verify visually or via subsequent ``stat`` console queries.
//
// Picks PIE-world-first / editor-world-fallback for the World argument so stats targeting the
// running game world resolve correctly during PIE (matches PhysicsTools' trace-world convention).
FMCPResponse Tool_SetEngineStat(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kRNDErrorInvalidParams, TEXT("missing args object"));
	}

	FString StatName;
	if (!Request.Args->TryGetStringField(TEXT("stat_name"), StatName) || StatName.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kRNDErrorInvalidParams,
			TEXT("missing required string field 'stat_name'"));
	}

	bool bEnabled = false;
	if (!Request.Args->TryGetBoolField(TEXT("enabled"), bEnabled))
	{
		return FMCPToolHelpers::MakeError(Request, kRNDErrorInvalidParams,
			TEXT("missing required bool field 'enabled'"));
	}

	if (!GEngine)
	{
		return FMCPToolHelpers::MakeError(Request, kRNDErrorInternal, TEXT("GEngine unavailable"));
	}

	FMCPResponse Err;
	FLevelEditorViewportClient* VC = RND_ResolveViewport(Request, Err);
	if (!VC) { return Err; }

	// PIE-first / editor-fallback world. Same selection rule as PhysicsTools/DebugTools — keeps
	// the surface coherent for callers who toggle stats during PIE. Explicit branches avoid the
	// TObjectPtr<UWorld> vs UWorld* ambiguity that the conditional operator triggers in UE 5.7.
	UWorld* World = FMCPWorldContext::GetEditorWorld();
	if (GEditor && GEditor->PlayWorld)
	{
		World = GEditor->PlayWorld;
	}
	if (!World)
	{
		return FMCPToolHelpers::MakeError(Request, kRNDErrorInternal,
			TEXT("no world available for SetEngineStat (no editor world)"));
	}

	// FLevelEditorViewportClient publicly inherits FCommonViewportClient via FEditorViewportClient —
	// the SetEngineStat signature accepts the base class.
	FCommonViewportClient* VCBase = static_cast<FCommonViewportClient*>(VC);
	GEngine->SetEngineStat(World, VCBase, *StatName, bEnabled);
	VC->Invalidate();

	const TCHAR* WorldKind = TEXT("Other");
	switch (World->WorldType)
	{
	case EWorldType::Editor: WorldKind = TEXT("Editor"); break;
	case EWorldType::PIE:    WorldKind = TEXT("PIE");    break;
	case EWorldType::Game:   WorldKind = TEXT("Game");   break;
	default: break;
	}

	return FMCPJsonBuilder()
		.Str(TEXT("stat_name"), StatName)
		.Bool(TEXT("enabled"), bEnabled)
		.Str(TEXT("world_kind"), WorldKind)
		.BuildSuccess(Request);
}

// ─── render.set_post_process_volume_property ───────────────────────────────────────────────────
//
// Args: {
//   volume_actor_path: string,
//   property_path: string (accepts "AutoExposureBias" or "Settings.AutoExposureBias"),
//   value: <typed JSON>  (number for floats, bool, FVector/FLinearColor _kind dict for vectors, etc.)
// }
// Result: {
//   volume_actor_path, property_path,
//   prior_value: <JSON>, new_value: <JSON>,
//   bOverride_set: bool   (always true on success — companion bOverride_X is forced true so the
//                         volume actually applies the value)
// }
//
// PIE-guarded. ``FMCPMutatorScope`` wraps the write so Ctrl-Z reverts it. ``MarkPackageDirty``
// fires on the actor's external package (WorldPartition one-file-per-actor) with GetOutermost
// fallback.
//
// The companion ``bOverride_<FieldName>`` bool is automatically forced to true — this is the
// canonical pattern that the editor's Details panel uses (the field is gated by ``editcondition =
// "bOverride_<X>"`` so writing it without flipping the override leaves the volume rendering
// unchanged). If no companion override property exists (some fields like ``bEnabled`` are not
// gated), ``bOverride_set`` returns false but the write still succeeds.
FMCPResponse Tool_SetPostProcessVolumeProperty(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_PPV_SetProperty", "MCP: set post-process property"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kRNDErrorInvalidParams, TEXT("missing args object"));
	}

	FString VolumePath;
	if (!Request.Args->TryGetStringField(TEXT("volume_actor_path"), VolumePath) || VolumePath.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kRNDErrorInvalidParams,
			TEXT("missing required string field 'volume_actor_path'"));
	}

	FString PropertyPath;
	if (!Request.Args->TryGetStringField(TEXT("property_path"), PropertyPath) || PropertyPath.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kRNDErrorInvalidParams,
			TEXT("missing required string field 'property_path'"));
	}

	const TSharedPtr<FJsonValue> ValueField = Request.Args->TryGetField(TEXT("value"));
	if (!ValueField.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kRNDErrorInvalidParams,
			TEXT("missing required field 'value'"));
	}

	// Resolve actor. ``bRejectPIE=true`` mirrors the PIE-guard above — defence-in-depth so the
	// path resolver can't accidentally pick a PIE-cloned actor when the editor world is intended.
	bool bAmbiguous = false;
	FString AmbiguityHint, ResolveErr;
	AActor* Actor = FMCPActorPathUtils::ResolveActor(
		VolumePath, /*bRejectPIE*/ true, bAmbiguous, AmbiguityHint, ResolveErr);
	if (!Actor)
	{
		const FString Msg = bAmbiguous
			? FString::Printf(TEXT("actor '%s' ambiguous: %s"), *VolumePath, *AmbiguityHint)
			: FString::Printf(TEXT("actor '%s' not found: %s"), *VolumePath, *ResolveErr);
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound, Msg);
	}

	APostProcessVolume* PPV = Cast<APostProcessVolume>(Actor);
	if (!PPV)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorWrongClass,
			FString::Printf(TEXT("actor '%s' is not an APostProcessVolume (got %s)"),
				*VolumePath, *Actor->GetClass()->GetName()));
	}

	// Field lookup. FPostProcessSettings::StaticStruct() returns the UScriptStruct we walk.
	const FString FieldName = RND_NormalisePostProcessFieldName(PropertyPath);
	UScriptStruct* PPStruct = FPostProcessSettings::StaticStruct();
	check(PPStruct != nullptr);
	FProperty* Prop = PPStruct->FindPropertyByName(FName(*FieldName));
	if (!Prop)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPropertyNotFound,
			FString::Printf(
				TEXT("property '%s' not found on FPostProcessSettings (accepted with or without "
					 "'Settings.' prefix); use marshall.describe_struct on "
					 "/Script/Engine.PostProcessSettings to enumerate"),
				*FieldName));
	}

	// Companion bOverride_<FieldName> lookup. Some fields (rare) have no override — we still
	// write the value but skip flipping the (missing) bool.
	const FString OverrideName = FString::Printf(TEXT("bOverride_%s"), *FieldName);
	FProperty* OverrideProp = PPStruct->FindPropertyByName(FName(*OverrideName));
	FBoolProperty* OverrideBool = OverrideProp ? CastField<FBoolProperty>(OverrideProp) : nullptr;
	const bool bHasOverrideCompanion = (OverrideBool != nullptr);

	// Snapshot prior value BEFORE the write so the response can carry the diff.
	TSharedPtr<FJsonValue> PriorValue = RND_ReadPostProcessField(PPV->Settings, Prop);

	// Mutation — PreEditChange / Modify, then write, then PostEditChange. The surrounding
	// FMCPMutatorScope holds the FScopedTransaction so undo/redo captures this entire block.
	PPV->Modify();
	PPV->PreEditChange(Prop);

	// Force companion bOverride_<X> = true so the volume actually applies the override. Without
	// this the FPostProcessSettings field is ignored at evaluation time.
	if (OverrideBool)
	{
		OverrideBool->SetPropertyValue_InContainer(&PPV->Settings, true);
	}

	// Write the actual value via the universal reflection helper. ``OwnerObject = PPV`` is
	// forwarded to ImportText for text-fallback path (some inner FStructProperty unmarshallers
	// need an outer to resolve relative refs).
	void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(&PPV->Settings);
	FString WriteError;
	const bool bWriteOk = FMCPReflection::WritePropertyValueAt(Prop, ValuePtr, ValueField, PPV, WriteError);

	// PostEditChangeProperty MUST fire whether or not the write succeeded — Pre/Post pair is
	// non-negotiable per the FProperty edit contract. The PropertyChangedEvent carries the
	// inner Settings field so any Edit-time delegates (PostProcessComponent rebuild, etc.) get
	// notified.
	FPropertyChangedEvent ChangeEvent(Prop);
	PPV->PostEditChangeProperty(ChangeEvent);

	if (!bWriteOk)
	{
		Scope.Abort();
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPropertyTypeMismatch,
			FString::Printf(TEXT("write rejected for '%s': %s"), *FieldName, *WriteError));
	}

	// Snapshot new value AFTER the write — round-trips through the same reader so the response
	// schema is stable.
	TSharedPtr<FJsonValue> NewValue = RND_ReadPostProcessField(PPV->Settings, Prop);

	// Dirty marking — prefer external package (WorldPartition OFPA) when present, else outermost.
	UPackage* DirtyPkg = PPV->GetExternalPackage();
	if (!DirtyPkg)
	{
		DirtyPkg = PPV->GetOutermost();
	}
	Scope.DirtyPackage(DirtyPkg);

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("volume_actor_path"), PPV->GetPathName());
	Out->SetStringField(TEXT("property_path"), FieldName);
	Out->SetField(TEXT("prior_value"), PriorValue);
	Out->SetField(TEXT("new_value"), NewValue);
	Out->SetBoolField(TEXT("bOverride_set"), bHasOverrideCompanion);
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	RegisterTool(TEXT("render.list_show_flags"),                 &Tool_ListShowFlags,                /*Lane A*/ false);
	RegisterTool(TEXT("render.set_show_flag"),                   &Tool_SetShowFlag,                  /*Lane A*/ false);
	RegisterTool(TEXT("render.set_engine_stat"),                 &Tool_SetEngineStat,                /*Lane A*/ false);
	RegisterTool(TEXT("render.set_post_process_volume_property"),&Tool_SetPostProcessVolumeProperty, /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("Render surface registered: 4 tools "
			 "(list_show_flags + set_show_flag + set_engine_stat + set_post_process_volume_property), "
			 "all Lane A"));
}

} // namespace FRenderTools

#undef LOCTEXT_NAMESPACE

MCP_REGISTER_SURFACE(RenderTools, &FRenderTools::Register)
