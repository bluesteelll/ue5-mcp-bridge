// Copyright FatumGame. All Rights Reserved.

#include "LevelStreamingTools.h"

#include "FMCPDispatchQueue.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPAssetPathUtils.h"
#include "Utils/MCPWorldContext.h"

#include "Editor.h"
#include "EditorLevelUtils.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/LevelStreaming.h"
#include "Engine/LevelStreamingDynamic.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// LS_ prefix per surface convention.
	constexpr int32 kLSErrorInvalidParams = -32602;
	constexpr int32 kLSErrorInternal      = -32603;

	void LS_StampIds(const FMCPRequest& Request, FMCPResponse& Response)
	{
		Response.RequestId = Request.RequestId;
		Response.OriginalIdString = Request.OriginalIdString;
	}

	FMCPResponse LS_MakeError(const FMCPRequest& Request, int32 Code, const FString& Message)
	{
		FMCPResponse R;
		LS_StampIds(Request, R);
		R.bIsError = true; R.ErrorCode = Code; R.ErrorMessage = Message;
		return R;
	}

	FMCPResponse LS_MakeSuccessObj(const FMCPRequest& Request, TSharedPtr<FJsonObject> Result)
	{
		FMCPResponse R;
		LS_StampIds(Request, R);
		R.bIsError = false;
		R.Result = MakeShared<FJsonValueObject>(MoveTemp(Result));
		return R;
	}

	/**
	 * Resolve a persistent UWorld by path, OR return the current editor world when ``Path`` is
	 * empty. Mirrors WorldPartitionTools' ``WP_LoadWorldByPath`` but lets the caller fall through
	 * to the editor world for the default case.
	 *
	 * Returns null + populates ``Out*`` on failure (-32004 ObjectNotFound or -32011 WrongClass or
	 * -32010 InvalidPath / -32603 internal).
	 */
	UWorld* LS_ResolvePersistentWorld(const FString& Path, int32& OutErrorCode, FString& OutError)
	{
		if (Path.IsEmpty())
		{
			UWorld* W = FMCPWorldContext::GetEditorWorld();
			if (!W)
			{
				OutErrorCode = kLSErrorInternal;
				OutError = TEXT("no editor world available (GEditor missing)");
				return nullptr;
			}
			return W;
		}

		const FString Normalised = FMCPAssetPathUtils::Normalize(Path);
		if (Normalised.IsEmpty() || !FMCPAssetPathUtils::IsValidGameOrPlugin(Normalised))
		{
			OutErrorCode = kMCPErrorInvalidPath;
			OutError = FString::Printf(TEXT("path '%s' malformed or unknown mount"), *Path);
			return nullptr;
		}
		UObject* Loaded = LoadObject<UObject>(nullptr, *Normalised);
		if (!Loaded)
		{
			const FString ObjPath = FMCPAssetPathUtils::ToObjectPath(Normalised);
			if (!ObjPath.IsEmpty() && ObjPath != Normalised) { Loaded = LoadObject<UObject>(nullptr, *ObjPath); }
		}
		if (!Loaded)
		{
			OutErrorCode = kMCPErrorObjectNotFound;
			OutError = FString::Printf(TEXT("'%s' not loadable"), *Path);
			return nullptr;
		}
		UWorld* World = Cast<UWorld>(Loaded);
		if (!World)
		{
			OutErrorCode = kMCPErrorWrongClass;
			OutError = FString::Printf(TEXT("'%s' is class '%s'; expected UWorld"),
				*Path, *Loaded->GetClass()->GetPathName());
			return nullptr;
		}
		return World;
	}

	/**
	 * PIE-first / editor-fallback world resolution for ``set_loaded`` — mirrors PhysicsTools'
	 * ``PHY_ResolveTraceWorld`` convention. Returns null only when GEditor itself is missing.
	 */
	UWorld* LS_ResolveActiveWorld()
	{
		check(IsInGameThread());
		if (GEditor && GEditor->PlayWorld)
		{
			return GEditor->PlayWorld;
		}
		return FMCPWorldContext::GetEditorWorld();
	}

	/**
	 * Find the matching ULevelStreaming entry whose target package equals ``SublevelPath``.
	 * Returns null when no entry matches (caller surfaces -32004 ObjectNotFound).
	 */
	ULevelStreaming* LS_FindStreamingEntry(UWorld* World, const FString& SublevelPath)
	{
		if (!World) { return nullptr; }
		const FString Norm = FMCPWorldContext::NormaliseMapPath(SublevelPath);
		if (Norm.IsEmpty()) { return nullptr; }
		const FName Target(*Norm);
		for (ULevelStreaming* Streaming : World->GetStreamingLevels())
		{
			if (!Streaming) { continue; }
			if (Streaming->GetWorldAssetPackageFName() == Target)
			{
				return Streaming;
			}
		}
		return nullptr;
	}

	TSharedRef<FJsonObject> LS_BuildStreamingSummary(ULevelStreaming* Streaming)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (!Streaming)
		{
			Obj->SetField(TEXT("sublevel_path"), MakeShared<FJsonValueNull>());
			return Obj;
		}
		Obj->SetStringField(TEXT("sublevel_path"),    Streaming->GetWorldAssetPackageName());
		Obj->SetStringField(TEXT("package_name"),     Streaming->GetWorldAssetPackageName());
		Obj->SetStringField(TEXT("level_class"),      Streaming->GetClass()->GetPathName());
		Obj->SetBoolField  (TEXT("is_loaded"),        Streaming->HasLoadedLevel());
		Obj->SetBoolField  (TEXT("is_visible"),       Streaming->IsLevelVisible());
		Obj->SetBoolField  (TEXT("should_be_loaded"), Streaming->ShouldBeLoaded());
		Obj->SetBoolField  (TEXT("should_be_visible"),Streaming->ShouldBeVisible());
		return Obj;
	}

	bool LS_ReadVectorArray(
		const TSharedPtr<FJsonObject>& Args,
		const TCHAR* FieldName,
		const FVector& DefaultValue,
		FVector& OutV,
		FString& OutError)
	{
		// Not present → default. Present-but-malformed → error.
		if (!Args.IsValid() || !Args->HasField(FieldName))
		{
			OutV = DefaultValue;
			return true;
		}
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Args->TryGetArrayField(FieldName, Arr) || !Arr)
		{
			OutError = FString::Printf(TEXT("'%s' must be a 3-element [x,y,z] array"), FieldName);
			return false;
		}
		if (Arr->Num() != 3)
		{
			OutError = FString::Printf(
				TEXT("'%s' must be [x,y,z] (3 numbers); got %d entries"), FieldName, Arr->Num());
			return false;
		}
		double X = 0.0, Y = 0.0, Z = 0.0;
		if (!(*Arr)[0]->TryGetNumber(X)
			|| !(*Arr)[1]->TryGetNumber(Y)
			|| !(*Arr)[2]->TryGetNumber(Z))
		{
			OutError = FString::Printf(TEXT("'%s' entries must all be numbers"), FieldName);
			return false;
		}
		OutV = FVector(X, Y, Z);
		return true;
	}

	/** Parse optional ``transform`` object {location:[x,y,z], rotation:[p,y,r], scale:[x,y,z]}. */
	bool LS_ReadTransformObj(
		const TSharedPtr<FJsonObject>& Args,
		FTransform& OutT,
		FString& OutError)
	{
		OutT = FTransform::Identity;
		if (!Args.IsValid() || !Args->HasField(TEXT("transform")))
		{
			return true; // optional — Identity default
		}
		const TSharedPtr<FJsonObject>* TObj = nullptr;
		if (!Args->TryGetObjectField(TEXT("transform"), TObj) || !TObj || !TObj->IsValid())
		{
			OutError = TEXT("'transform' must be an object {location, rotation, scale}");
			return false;
		}
		FVector Location(0, 0, 0);
		FVector RotationVec(0, 0, 0);  // pitch, yaw, roll (degrees)
		FVector Scale(1, 1, 1);
		if (!LS_ReadVectorArray(*TObj, TEXT("location"), FVector::ZeroVector, Location, OutError)) { return false; }
		if (!LS_ReadVectorArray(*TObj, TEXT("rotation"), FVector::ZeroVector, RotationVec, OutError)) { return false; }
		if (!LS_ReadVectorArray(*TObj, TEXT("scale"),    FVector::OneVector,  Scale, OutError)) { return false; }
		const FRotator Rotation(RotationVec.X, RotationVec.Y, RotationVec.Z); // pitch, yaw, roll
		OutT = FTransform(Rotation.Quaternion(), Location, Scale);
		return true;
	}
} // namespace

namespace FLevelStreamingTools
{

// ─── level_streaming.list ─────────────────────────────────────────────────────────────────────
//
// Args:    { persistent_level_path?: string (defaults to current editor world) }
// Result:  { persistent: string, streaming: [{ sublevel_path, level_class, package_name,
//            is_loaded, is_visible, should_be_loaded, should_be_visible }] }
FMCPResponse Tool_List(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString PersistentPath;
	if (Request.Args.IsValid())
	{
		Request.Args->TryGetStringField(TEXT("persistent_level_path"), PersistentPath);
	}

	int32 LoadErrCode = 0;
	FString LoadErrMsg;
	UWorld* World = LS_ResolvePersistentWorld(PersistentPath, LoadErrCode, LoadErrMsg);
	if (!World) { return LS_MakeError(Request, LoadErrCode, LoadErrMsg); }

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("persistent"),
		World->PersistentLevel && World->PersistentLevel->GetOutermost()
			? World->PersistentLevel->GetOutermost()->GetName()
			: World->GetPathName());

	const TArray<ULevelStreaming*>& Streaming = World->GetStreamingLevels();
	TArray<TSharedPtr<FJsonValue>> Arr;
	Arr.Reserve(Streaming.Num());
	for (ULevelStreaming* SL : Streaming)
	{
		if (!SL) { continue; }
		Arr.Add(MakeShared<FJsonValueObject>(LS_BuildStreamingSummary(SL)));
	}
	Out->SetArrayField(TEXT("streaming"), Arr);
	Out->SetNumberField(TEXT("count"), Arr.Num());
	return LS_MakeSuccessObj(Request, Out);
}

// ─── level_streaming.add (mutator — PIE-guarded) ─────────────────────────────────────────────
//
// Args:    { persistent_level_path: string,
//            sublevel_asset_path:   string,
//            transform?: { location:[x,y,z], rotation:[p,y,r], scale:[x,y,z] } }
// Result:  { added: bool, sublevel_path: string, package_name: string }
//
// Internally uses ``UEditorLevelUtils::AddLevelToWorld(World*, TCHAR*, TSubclassOf<ULevelStreaming>,
// FTransform)``. Defaults streaming class to ULevelStreamingDynamic (matches editor's outliner
// drag-drop convention). Wrapped in FScopedTransaction; persistent level package marked dirty.
FMCPResponse Tool_Add(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (FMCPWorldContext::IsPIEActive())
	{
		return LS_MakeError(Request, kMCPErrorPIEActive, kMCPMessagePIEActive);
	}

	if (!Request.Args.IsValid())
	{
		return LS_MakeError(Request, kLSErrorInvalidParams, TEXT("missing args object"));
	}

	FString PersistentPath, SublevelPath;
	if (!Request.Args->TryGetStringField(TEXT("persistent_level_path"), PersistentPath)
		|| PersistentPath.IsEmpty())
	{
		return LS_MakeError(Request, kLSErrorInvalidParams,
			TEXT("missing required string field 'persistent_level_path'"));
	}
	if (!Request.Args->TryGetStringField(TEXT("sublevel_asset_path"), SublevelPath)
		|| SublevelPath.IsEmpty())
	{
		return LS_MakeError(Request, kLSErrorInvalidParams,
			TEXT("missing required string field 'sublevel_asset_path'"));
	}

	FTransform Transform;
	FString TransformErr;
	if (!LS_ReadTransformObj(Request.Args, Transform, TransformErr))
	{
		return LS_MakeError(Request, kLSErrorInvalidParams, TransformErr);
	}

	int32 LoadErrCode = 0;
	FString LoadErrMsg;
	UWorld* PersistentWorld = LS_ResolvePersistentWorld(PersistentPath, LoadErrCode, LoadErrMsg);
	if (!PersistentWorld) { return LS_MakeError(Request, LoadErrCode, LoadErrMsg); }

	// Normalise the sublevel path to package-name form expected by AddLevelToWorld.
	const FString SublevelNorm = FMCPAssetPathUtils::Normalize(SublevelPath);
	if (SublevelNorm.IsEmpty() || !FMCPAssetPathUtils::IsValidGameOrPlugin(SublevelNorm))
	{
		return LS_MakeError(Request, kMCPErrorInvalidPath,
			FString::Printf(TEXT("sublevel_asset_path '%s' malformed or unknown mount"), *SublevelPath));
	}

	// Verify the asset actually exists on disk before AddLevelToWorld silently no-ops on a typo.
	if (!FPackageName::DoesPackageExist(SublevelNorm))
	{
		return LS_MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("sublevel package '%s' does not exist on disk"), *SublevelNorm));
	}

	// Refuse if the sublevel is already in the streaming list (matches UEditorLevelUtils' "does
	// nothing if the level already exists" docs — we surface that as -32014 PathInUse so callers
	// can detect the no-op deterministically).
	if (LS_FindStreamingEntry(PersistentWorld, SublevelNorm))
	{
		return LS_MakeError(Request, kMCPErrorPathInUse,
			FString::Printf(TEXT("sublevel '%s' is already in the streaming list of '%s'"),
				*SublevelNorm, *PersistentWorld->GetPathName()));
	}

	const FScopedTransaction Transaction(LOCTEXT("LevelStreamingAdd", "MCP: add streaming level"));
	ULevelStreaming* NewStreaming = UEditorLevelUtils::AddLevelToWorld(
		PersistentWorld,
		*SublevelNorm,
		ULevelStreamingDynamic::StaticClass(),
		Transform);

	if (!NewStreaming)
	{
		return LS_MakeError(Request, kLSErrorInternal,
			FString::Printf(TEXT("UEditorLevelUtils::AddLevelToWorld returned null for sublevel '%s'"),
				*SublevelNorm));
	}

	if (UPackage* Pkg = PersistentWorld->PersistentLevel
		? PersistentWorld->PersistentLevel->GetOutermost()
		: PersistentWorld->GetOutermost())
	{
		Pkg->MarkPackageDirty();
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField  (TEXT("added"),         true);
	Out->SetStringField(TEXT("sublevel_path"), NewStreaming->GetWorldAssetPackageName());
	Out->SetStringField(TEXT("package_name"),  NewStreaming->GetWorldAssetPackageName());
	Out->SetStringField(TEXT("level_class"),   NewStreaming->GetClass()->GetPathName());
	return LS_MakeSuccessObj(Request, Out);
}

// ─── level_streaming.remove (mutator — PIE-guarded) ──────────────────────────────────────────
//
// Args:    { persistent_level_path: string, sublevel_asset_path: string }
// Result:  { removed: bool, sublevel_path: string }
//
// Forwards to ``UEditorLevelUtils::RemoveLevelFromWorld(ULevel*)``. The sublevel MUST be in the
// streaming list — otherwise -32004 ObjectNotFound.
FMCPResponse Tool_Remove(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (FMCPWorldContext::IsPIEActive())
	{
		return LS_MakeError(Request, kMCPErrorPIEActive, kMCPMessagePIEActive);
	}

	if (!Request.Args.IsValid())
	{
		return LS_MakeError(Request, kLSErrorInvalidParams, TEXT("missing args object"));
	}

	FString PersistentPath, SublevelPath;
	if (!Request.Args->TryGetStringField(TEXT("persistent_level_path"), PersistentPath)
		|| PersistentPath.IsEmpty())
	{
		return LS_MakeError(Request, kLSErrorInvalidParams,
			TEXT("missing required string field 'persistent_level_path'"));
	}
	if (!Request.Args->TryGetStringField(TEXT("sublevel_asset_path"), SublevelPath)
		|| SublevelPath.IsEmpty())
	{
		return LS_MakeError(Request, kLSErrorInvalidParams,
			TEXT("missing required string field 'sublevel_asset_path'"));
	}

	int32 LoadErrCode = 0;
	FString LoadErrMsg;
	UWorld* PersistentWorld = LS_ResolvePersistentWorld(PersistentPath, LoadErrCode, LoadErrMsg);
	if (!PersistentWorld) { return LS_MakeError(Request, LoadErrCode, LoadErrMsg); }

	ULevelStreaming* Streaming = LS_FindStreamingEntry(PersistentWorld, SublevelPath);
	if (!Streaming)
	{
		return LS_MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("sublevel '%s' is not in the streaming list of '%s'"),
				*SublevelPath, *PersistentWorld->GetPathName()));
	}

	ULevel* Level = Streaming->GetLoadedLevel();
	const FString ResolvedPath = Streaming->GetWorldAssetPackageName();

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("sublevel_path"), ResolvedPath);

	if (!Level)
	{
		// Sublevel entry exists but its ULevel isn't loaded — RemoveInvalidLevelFromWorld is the
		// right path. Treat as removable for caller convenience.
		const FScopedTransaction Transaction(LOCTEXT("LevelStreamingRemoveInvalid", "MCP: remove invalid streaming level"));
		const bool bOk = UEditorLevelUtils::RemoveInvalidLevelFromWorld(Streaming);
		Out->SetBoolField(TEXT("removed"), bOk);
		if (!bOk)
		{
			return LS_MakeError(Request, kLSErrorInternal,
				FString::Printf(
					TEXT("RemoveInvalidLevelFromWorld returned false for sublevel '%s'"),
					*ResolvedPath));
		}
	}
	else
	{
		const FScopedTransaction Transaction(LOCTEXT("LevelStreamingRemove", "MCP: remove streaming level"));
		const bool bOk = UEditorLevelUtils::RemoveLevelFromWorld(Level);
		Out->SetBoolField(TEXT("removed"), bOk);
		if (!bOk)
		{
			return LS_MakeError(Request, kLSErrorInternal,
				FString::Printf(TEXT("RemoveLevelFromWorld returned false for sublevel '%s'"),
					*ResolvedPath));
		}
	}

	if (UPackage* Pkg = PersistentWorld->PersistentLevel
		? PersistentWorld->PersistentLevel->GetOutermost()
		: PersistentWorld->GetOutermost())
	{
		Pkg->MarkPackageDirty();
	}

	return LS_MakeSuccessObj(Request, Out);
}

// ─── level_streaming.set_loaded (PIE-safe — no guard) ────────────────────────────────────────
//
// Args:    { sublevel_path: string, loaded: bool, visible?: bool (defaults to ``loaded``) }
// Result:  { sublevel_path, prior: {loaded, visible}, new: {loaded, visible} }
//
// PIE-first / editor-fallback world resolution. ``FlushLevelStreaming`` only when NOT in PIE —
// in PIE the world's runtime streaming tick drains the flag change naturally; forcing a synchronous
// flush mid-PIE is potentially hazardous.
FMCPResponse Tool_SetLoaded(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!Request.Args.IsValid())
	{
		return LS_MakeError(Request, kLSErrorInvalidParams, TEXT("missing args object"));
	}

	FString SublevelPath;
	if (!Request.Args->TryGetStringField(TEXT("sublevel_path"), SublevelPath) || SublevelPath.IsEmpty())
	{
		return LS_MakeError(Request, kLSErrorInvalidParams,
			TEXT("missing required string field 'sublevel_path'"));
	}
	bool bLoaded = false;
	if (!Request.Args->TryGetBoolField(TEXT("loaded"), bLoaded))
	{
		return LS_MakeError(Request, kLSErrorInvalidParams,
			TEXT("missing required bool field 'loaded'"));
	}
	bool bVisible = bLoaded;  // default: track loaded
	Request.Args->TryGetBoolField(TEXT("visible"), bVisible);

	UWorld* World = LS_ResolveActiveWorld();
	if (!World)
	{
		return LS_MakeError(Request, kLSErrorInternal,
			TEXT("no world available (GEditor missing)"));
	}

	ULevelStreaming* Streaming = LS_FindStreamingEntry(World, SublevelPath);
	if (!Streaming)
	{
		return LS_MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("sublevel '%s' is not in the streaming list of world '%s'"),
				*SublevelPath, *World->GetPathName()));
	}

	const bool bPriorLoaded  = Streaming->ShouldBeLoaded();
	const bool bPriorVisible = Streaming->ShouldBeVisible();

	const bool bInPIE = FMCPWorldContext::IsPIEActive();
	// Transactions are an editor authoring concept — only meaningful outside PIE. In PIE the
	// streaming flags are just runtime state.
	if (!bInPIE)
	{
		const FScopedTransaction Transaction(LOCTEXT("LevelStreamingSetLoaded", "MCP: set streaming loaded"));
		Streaming->SetShouldBeLoaded(bLoaded);
		Streaming->SetShouldBeVisible(bVisible);
		if (UPackage* Pkg = World->PersistentLevel
			? World->PersistentLevel->GetOutermost()
			: World->GetOutermost())
		{
			Pkg->MarkPackageDirty();
		}
		World->FlushLevelStreaming();
	}
	else
	{
		Streaming->SetShouldBeLoaded(bLoaded);
		Streaming->SetShouldBeVisible(bVisible);
		// No flush in PIE — runtime streaming tick drains.
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("sublevel_path"), Streaming->GetWorldAssetPackageName());

	TSharedRef<FJsonObject> Prior = MakeShared<FJsonObject>();
	Prior->SetBoolField(TEXT("loaded"),  bPriorLoaded);
	Prior->SetBoolField(TEXT("visible"), bPriorVisible);
	Out->SetObjectField(TEXT("prior"), Prior);

	TSharedRef<FJsonObject> New = MakeShared<FJsonObject>();
	New->SetBoolField(TEXT("loaded"),  bLoaded);
	New->SetBoolField(TEXT("visible"), bVisible);
	Out->SetObjectField(TEXT("new"), New);

	Out->SetBoolField(TEXT("flushed"), !bInPIE);
	return LS_MakeSuccessObj(Request, Out);
}

void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	RegisterTool(TEXT("level_streaming.list"),       &Tool_List,      /*Lane A*/ false);
	RegisterTool(TEXT("level_streaming.add"),        &Tool_Add,       /*Lane A*/ false);
	RegisterTool(TEXT("level_streaming.remove"),     &Tool_Remove,    /*Lane A*/ false);
	RegisterTool(TEXT("level_streaming.set_loaded"), &Tool_SetLoaded, /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("LevelStreaming surface registered: 4 level_streaming.* tools "
			 "(list + add + remove + set_loaded), all Lane A"));
}

} // namespace FLevelStreamingTools

#undef LOCTEXT_NAMESPACE
