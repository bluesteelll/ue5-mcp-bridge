// Copyright FatumGame. All Rights Reserved.

#include "FolderTools.h"

#include "FMCPDispatchQueue.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPActorPathUtils.h"
#include "Utils/MCPWorldContext.h"

#include "EditorActorFolders.h"
#include "Engine/World.h"
#include "EngineUtils.h"             // FActorIterator
#include "Folder.h"
#include "GameFramework/Actor.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// FLDR_ prefix per unity-build convention (avoids ODR collisions in shared compile units).
	constexpr int32 kFLDRErrorInvalidParams = -32602;
	constexpr int32 kFLDRErrorInternal      = -32603;

	void FLDR_StampIds(const FMCPRequest& Request, FMCPResponse& Response)
	{
		Response.RequestId = Request.RequestId;
		Response.OriginalIdString = Request.OriginalIdString;
	}

	FMCPResponse FLDR_MakeError(const FMCPRequest& Request, int32 Code, const FString& Message)
	{
		FMCPResponse R;
		FLDR_StampIds(Request, R);
		R.bIsError = true; R.ErrorCode = Code; R.ErrorMessage = Message;
		return R;
	}

	FMCPResponse FLDR_MakeSuccessObj(const FMCPRequest& Request, TSharedPtr<FJsonObject> Result)
	{
		FMCPResponse R;
		FLDR_StampIds(Request, R);
		R.bIsError = false;
		R.Result = MakeShared<FJsonValueObject>(MoveTemp(Result));
		return R;
	}

	bool FLDR_RequireString(const FMCPRequest& Request, const TCHAR* Field, FString& OutValue, FMCPResponse& OutErr)
	{
		if (!Request.Args.IsValid())
		{
			OutErr = FLDR_MakeError(Request, kFLDRErrorInvalidParams, TEXT("missing args object"));
			return false;
		}
		if (!Request.Args->TryGetStringField(Field, OutValue) || OutValue.IsEmpty())
		{
			OutErr = FLDR_MakeError(Request, kFLDRErrorInvalidParams,
				FString::Printf(TEXT("missing required string field '%s'"), Field));
			return false;
		}
		return true;
	}

	/**
	 * Resolve the editor world for folder ops. Returns nullptr + populates Out* on failure.
	 * Phase 3 convention: folder.* tools operate on the editor world only (PIE has its own
	 * outliner state per instance; managing those is out of scope).
	 */
	UWorld* FLDR_GetEditorWorldOrFail(int32& OutErrorCode, FString& OutError)
	{
		UWorld* World = FMCPWorldContext::GetEditorWorld();
		if (!World)
		{
			OutErrorCode = kFLDRErrorInternal;
			OutError = TEXT("no editor world (GEditor missing — commandlet context?)");
			return nullptr;
		}
		return World;
	}

	/**
	 * Build a FFolder for the world-root using the world's persistent-level root object — mirrors
	 * the engine's deprecated overloads (FActorFolders::CreateFolder(World, FName)) which use
	 * FActorFolders::GetWorldFolderRootObject internally.
	 */
	FFolder FLDR_MakeWorldFolder(UWorld* World, const FString& FolderPath)
	{
		check(World);
		const FFolder::FRootObject Root = FFolder::GetWorldRootFolder(World).GetRootObject();
		return FFolder(Root, FName(*FolderPath));
	}

	/** Returns the parent path of FolderPath (e.g. "A/B/C" -> "A/B"; "A" -> ""). */
	FString FLDR_GetParentPath(const FString& FolderPath)
	{
		int32 LastSlash = INDEX_NONE;
		if (FolderPath.FindLastChar(TEXT('/'), LastSlash))
		{
			return FolderPath.Left(LastSlash);
		}
		return FString();
	}
} // namespace

namespace FFolderTools
{

// ─── folder.list ──────────────────────────────────────────────────────────────────────────────
//
// Args:    (none — operates on editor world)
// Result:  { folders: [{ path: string, child_count: int }], total_known: int }
//
// Notes: child_count is "number of OTHER folders whose path begins with `<this>/`" — a quick
// proxy for "is this a leaf folder?" without inspecting actor membership. Sort is alphabetical.
FMCPResponse Tool_List(const FMCPRequest& Request)
{
	check(IsInGameThread());

	int32 ErrCode = 0;
	FString ErrMsg;
	UWorld* World = FLDR_GetEditorWorldOrFail(ErrCode, ErrMsg);
	if (!World) { return FLDR_MakeError(Request, ErrCode, ErrMsg); }

	TArray<FString> Paths;
	FActorFolders::Get().ForEachFolder(*World, [&Paths](const FFolder& Folder)
	{
		// Skip the invisible root folder (empty/none path).
		if (!Folder.IsNone())
		{
			Paths.Add(Folder.ToString());
		}
		return true;
	});
	Paths.Sort();

	TArray<TSharedPtr<FJsonValue>> Out;
	Out.Reserve(Paths.Num());
	for (const FString& P : Paths)
	{
		// Child count: any other folder whose path starts with "P/".
		const FString Prefix = P + TEXT("/");
		int32 ChildCount = 0;
		for (const FString& Other : Paths)
		{
			if (Other != P && Other.StartsWith(Prefix))
			{
				++ChildCount;
			}
		}
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("path"), P);
		Obj->SetNumberField(TEXT("child_count"), ChildCount);
		Out.Add(MakeShared<FJsonValueObject>(Obj));
	}

	TSharedRef<FJsonObject> Resp = MakeShared<FJsonObject>();
	Resp->SetArrayField(TEXT("folders"), Out);
	Resp->SetNumberField(TEXT("total_known"), Paths.Num());
	Resp->SetStringField(TEXT("world_path"), World->GetPathName());
	return FLDR_MakeSuccessObj(Request, Resp);
}

// ─── folder.create ────────────────────────────────────────────────────────────────────────────
//
// Args:    { folder_path: string }
// Result:  { created: bool, folder_path: string, world_path: string }
FMCPResponse Tool_Create(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (FMCPWorldContext::IsPIEActive())
	{
		return FLDR_MakeError(Request, kMCPErrorPIEActive, kMCPMessagePIEActive);
	}

	FString FolderPath;
	FMCPResponse Err;
	if (!FLDR_RequireString(Request, TEXT("folder_path"), FolderPath, Err)) { return Err; }

	int32 ErrCode = 0;
	FString ErrMsg;
	UWorld* World = FLDR_GetEditorWorldOrFail(ErrCode, ErrMsg);
	if (!World) { return FLDR_MakeError(Request, ErrCode, ErrMsg); }

	const FFolder Folder = FLDR_MakeWorldFolder(World, FolderPath);

	FScopedTransaction Transaction(LOCTEXT("MCP_CreateFolder", "Create Folder"));
	const bool bCreated = FActorFolders::Get().CreateFolder(*World, Folder);

	// World folders state isn't an asset, but mark the persistent level package so undo works
	// + the World can be re-saved with updated outliner state if the user wants persistence.
	if (UPackage* Pkg = World->PersistentLevel ? World->PersistentLevel->GetOutermost() : World->GetOutermost())
	{
		Pkg->MarkPackageDirty();
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("created"), bCreated);
	Out->SetStringField(TEXT("folder_path"), Folder.ToString());
	Out->SetStringField(TEXT("world_path"), World->GetPathName());
	return FLDR_MakeSuccessObj(Request, Out);
}

// ─── folder.delete ────────────────────────────────────────────────────────────────────────────
//
// Args:    { folder_path: string, move_children_to_parent?: bool (default true) }
// Result:  { deleted: bool, moved_children: int, folder_path: string, parent_path: string }
//
// When move_children_to_parent=true: walks every actor and any whose folder path begins with
// "<folder_path>/" gets re-parented to the deleted folder's parent (empty parent = world root).
// Actors directly inside the deleted folder (folder path == deleted folder) also get moved.
// Sub-folders are deleted by FActorFolders' own delete cascade.
FMCPResponse Tool_Delete(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (FMCPWorldContext::IsPIEActive())
	{
		return FLDR_MakeError(Request, kMCPErrorPIEActive, kMCPMessagePIEActive);
	}

	FString FolderPath;
	FMCPResponse Err;
	if (!FLDR_RequireString(Request, TEXT("folder_path"), FolderPath, Err)) { return Err; }

	bool bMoveChildrenToParent = true;
	if (Request.Args.IsValid())
	{
		Request.Args->TryGetBoolField(TEXT("move_children_to_parent"), bMoveChildrenToParent);
	}

	int32 ErrCode = 0;
	FString ErrMsg;
	UWorld* World = FLDR_GetEditorWorldOrFail(ErrCode, ErrMsg);
	if (!World) { return FLDR_MakeError(Request, ErrCode, ErrMsg); }

	const FFolder Folder = FLDR_MakeWorldFolder(World, FolderPath);

	if (!FActorFolders::Get().ContainsFolder(*World, Folder))
	{
		return FLDR_MakeError(Request, kMCPErrorFolderNotFound,
			FString::Printf(TEXT("folder '%s' does not exist in world '%s'"),
				*FolderPath, *World->GetPathName()));
	}

	FScopedTransaction Transaction(LOCTEXT("MCP_DeleteFolder", "Delete Folder"));

	int32 MovedChildren = 0;
	const FString ParentPath = FLDR_GetParentPath(FolderPath);
	const FName ParentFName = ParentPath.IsEmpty() ? NAME_None : FName(*ParentPath);

	if (bMoveChildrenToParent)
	{
		// Re-parent actors whose folder is either THIS folder OR a descendant (path starts with
		// "<folder>/"). We rewrite the actor's folder path: strip the deleted folder prefix and
		// concatenate with the new parent prefix. For direct children → parent path. For
		// descendants → parent path + remainder under the deleted folder.
		const FString FolderPrefix = FolderPath + TEXT("/");
		for (FActorIterator It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor) { continue; }

			const FName OldName = Actor->GetFolderPath();
			if (OldName.IsNone()) { continue; }
			const FString Old = OldName.ToString();

			FString New;
			if (Old == FolderPath)
			{
				// Direct child of the deleted folder → goes to parent.
				New = ParentPath;
			}
			else if (Old.StartsWith(FolderPrefix))
			{
				// Descendant — substitute the deleted prefix with the parent prefix.
				const FString Tail = Old.RightChop(FolderPrefix.Len());
				New = ParentPath.IsEmpty() ? Tail : (ParentPath + TEXT("/") + Tail);
			}
			else
			{
				continue;  // unrelated actor
			}

			Actor->Modify();
			Actor->SetFolderPath(New.IsEmpty() ? NAME_None : FName(*New));
			++MovedChildren;

			if (UPackage* ExternalPkg = Actor->GetExternalPackage())
			{
				ExternalPkg->MarkPackageDirty();
			}
		}
	}

	// Delete the folder. FActorFolders::DeleteFolder itself wraps in a FScopedTransaction but
	// nested transactions are safe (the inner one is a no-op when an outer is active).
	FActorFolders::Get().DeleteFolder(*World, Folder);

	const bool bStillExists = FActorFolders::Get().ContainsFolder(*World, Folder);

	if (UPackage* Pkg = World->PersistentLevel ? World->PersistentLevel->GetOutermost() : World->GetOutermost())
	{
		Pkg->MarkPackageDirty();
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("deleted"), !bStillExists);
	Out->SetNumberField(TEXT("moved_children"), MovedChildren);
	Out->SetStringField(TEXT("folder_path"), FolderPath);
	Out->SetStringField(TEXT("parent_path"), ParentPath);
	Out->SetStringField(TEXT("world_path"), World->GetPathName());
	return FLDR_MakeSuccessObj(Request, Out);
}

// ─── folder.set_actor ─────────────────────────────────────────────────────────────────────────
//
// Args:    { actor_path: string, folder_path: string (empty = root) }
// Result:  { actor_path, prior_folder, new_folder, world_path }
//
// Empty/missing folder_path = move to outliner root. Non-empty folder_path that doesn't exist
// is still accepted — UE creates the folder on demand (matches outliner drag-drop UX). We do
// NOT raise -32056 here; folder.set_actor is implicitly "create if missing".
FMCPResponse Tool_SetActor(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (FMCPWorldContext::IsPIEActive())
	{
		return FLDR_MakeError(Request, kMCPErrorPIEActive, kMCPMessagePIEActive);
	}

	FString ActorPath;
	FMCPResponse Err;
	if (!FLDR_RequireString(Request, TEXT("actor_path"), ActorPath, Err)) { return Err; }

	// folder_path is REQUIRED (caller must pass it explicitly — empty string = root, missing field
	// = invalid params, mirroring wp.set_actor_runtime_grid). Allow empty here.
	FString FolderPath;
	if (!Request.Args.IsValid() || !Request.Args->TryGetStringField(TEXT("folder_path"), FolderPath))
	{
		return FLDR_MakeError(Request, kFLDRErrorInvalidParams,
			TEXT("folder.set_actor requires args.folder_path (string; empty to move to root)"));
	}

	bool bAmbiguous = false;
	FString AmbiguityHint, ResolveErr;
	AActor* Actor = FMCPActorPathUtils::ResolveActor(ActorPath, /*bRejectPIE*/ true,
		bAmbiguous, AmbiguityHint, ResolveErr);
	if (!Actor)
	{
		return FLDR_MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("actor '%s' not found: %s"), *ActorPath, *ResolveErr));
	}

	const FName Prior = Actor->GetFolderPath();
	const FName Desired = FolderPath.IsEmpty() ? NAME_None : FName(*FolderPath);

	FScopedTransaction Transaction(LOCTEXT("MCP_SetActorFolder", "Set Actor Folder"));
	Actor->Modify();
	Actor->SetFolderPath(Desired);

	if (UPackage* ExternalPkg = Actor->GetExternalPackage())
	{
		ExternalPkg->MarkPackageDirty();
	}
	else if (UPackage* OuterPkg = Actor->GetOutermost())
	{
		OuterPkg->MarkPackageDirty();
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("actor_path"),   Actor->GetPathName());
	Out->SetStringField(TEXT("prior_folder"), Prior.ToString());
	Out->SetStringField(TEXT("new_folder"),   Desired.ToString());
	Out->SetStringField(TEXT("world_path"),   Actor->GetWorld() ? Actor->GetWorld()->GetPathName() : FString());
	return FLDR_MakeSuccessObj(Request, Out);
}

void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	RegisterTool(TEXT("folder.list"),      &Tool_List,     /*Lane A*/ false);
	RegisterTool(TEXT("folder.create"),    &Tool_Create,   /*Lane A*/ false);
	RegisterTool(TEXT("folder.delete"),    &Tool_Delete,   /*Lane A*/ false);
	RegisterTool(TEXT("folder.set_actor"), &Tool_SetActor, /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("Folder surface registered: 4 folder.* tools (list + create + delete + set_actor), all Lane A"));
}

} // namespace FFolderTools

#undef LOCTEXT_NAMESPACE
