// Copyright FatumGame. All Rights Reserved.

#include "ContentBrowserTools.h"

#include "MCPSurfaceRegistry.h"

#include "FMCPDispatchQueue.h"
#include "FMCPJobRegistry.h"
#include "MCPJsonBuilder.h"
#include "MCPMutatorScope.h"
#include "MCPToolHelpers.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPAssetPathUtils.h"
#include "Utils/MCPPathSandbox.h"

#include "AssetExportTask.h"
#include "AssetImportTask.h"
#include "AssetRegistry/ARFilter.h"
#include "EditorReimportHandler.h"
#include "EditorFramework/AssetImportData.h"
#include "Exporters/Exporter.h"
#include "Factories/Factory.h"
#include "UObject/UObjectIterator.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "IAssetTools.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "ScopedTransaction.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "UObject/Class.h"
#include "UObject/Object.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/TopLevelAssetPath.h"

#define LOCTEXT_NAMESPACE "MCPBridge_CB"

namespace
{
	// CB_ prefix retained for any helper unique to this surface.
	constexpr int32 kCBErrorInvalidParams = kMCPErrorInvalidParams;
	constexpr int32 kCBErrorInternal      = kMCPErrorInternal;

	/**
	 * Resolve a path arg with INVALID_PATH on malformed. Mirrors AR_RequirePath but lives here
	 * to avoid cross-TU helper sharing.
	 */
	bool CB_RequirePath(const FMCPRequest& Request, const TCHAR* FieldName,
		FString& OutNormalized, FMCPResponse& OutError)
	{
		FString Raw;
		if (!FMCPToolHelpers::RequireStringField(Request, FieldName, Raw, OutError))
		{
			return false;
		}
		const FString Normalized = FMCPAssetPathUtils::Normalize(Raw);
		if (Normalized.IsEmpty())
		{
			OutError = FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidPath,
				FString::Printf(TEXT("invalid path '%s' on field '%s'"), *Raw, FieldName));
			return false;
		}
		OutNormalized = Normalized;
		return true;
	}

	/** Convenience: get the EditorAssetSubsystem singleton (Lane A always — GEditor required). */
	UEditorAssetSubsystem* CB_GetSubsystem()
	{
		if (GEditor == nullptr) { return nullptr; }
		return GEditor->GetEditorSubsystem<UEditorAssetSubsystem>();
	}

	/** Helper: extract the leaf name from a normalised package path (`/Game/Foo/Bar` → `Bar`). */
	FString CB_LeafFromPath(const FString& NormalizedPath)
	{
		int32 LastSlash = INDEX_NONE;
		if (NormalizedPath.FindLastChar(TEXT('/'), LastSlash))
		{
			return NormalizedPath.Mid(LastSlash + 1);
		}
		return NormalizedPath;
	}

	/**
	 * True if `NormalizedPath` is a depth-2 /Game asset path (i.e. `/Game/<segment>/<leaf>`).
	 * Used by cb.delete force=true logging to surface "likely-mistake" deletions at Warning level.
	 *
	 * Examples: `/Game/Player/Foo` -> true; `/Game/Foo/Bar/Baz` -> false; `/Game/Foo` -> false.
	 */
	bool CB_IsDepth2GamePath(const FString& NormalizedPath)
	{
		if (!NormalizedPath.StartsWith(TEXT("/Game/"))) { return false; }
		// Count slashes — depth 2 means exactly 3 slashes total ("/Game/X/Y").
		int32 SlashCount = 0;
		for (TCHAR Ch : NormalizedPath)
		{
			if (Ch == TEXT('/')) { ++SlashCount; }
		}
		return SlashCount == 3;
	}

	/**
	 * Build the {source, asset_path} JSON entry used by cb.bulk_import.imported and the
	 * {path, error} JSON entry used by *.failed arrays. Single helper for serialiser code reuse.
	 */
	TSharedRef<FJsonObject> CB_MakeFailureEntry(const FString& Path, const FString& Error)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("path"), Path);
		Obj->SetStringField(TEXT("error"), Error);
		return Obj;
	}

	/**
	 * Render the standard job-result envelope: {ok: true, succeeded[], failed[], duration_ms}.
	 * Used by the inline lambdas for cb.save_all_dirty and cb.bulk_import so the wire shape is
	 * uniform across async tools.
	 */
	TSharedPtr<FJsonValue> CB_MakeJobResultObject(
		const TArray<TSharedPtr<FJsonValue>>& Succeeded,
		const TArray<TSharedPtr<FJsonValue>>& Failed,
		const TCHAR* SucceededFieldName,
		double DurationMs)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetArrayField(SucceededFieldName, Succeeded);
		Obj->SetArrayField(TEXT("failed"), Failed);
		Obj->SetNumberField(TEXT("duration_ms"), DurationMs);
		return MakeShared<FJsonValueObject>(Obj);
	}
}

namespace FContentBrowserTools
{

// ─── cb.create_folder ────────────────────────────────────────────────────────────────────────
FMCPResponse Tool_CreateFolder(const FMCPRequest& Request)
{
	FString NormalizedPath;
	FMCPResponse Err;
	if (!CB_RequirePath(Request, TEXT("path"), NormalizedPath, Err)) { return Err; }

	UEditorAssetSubsystem* Subsys = CB_GetSubsystem();
	if (Subsys == nullptr)
	{
		return FMCPToolHelpers::MakeError(Request, kCBErrorInternal, TEXT("EditorAssetSubsystem unavailable (no GEditor)"));
	}

	// Idempotent: if the directory already exists we return created=false (not an error).
	// UEditorAssetSubsystem::DoesDirectoryExist tells us up front.
	const bool bAlreadyExists = Subsys->DoesDirectoryExist(NormalizedPath);

	bool bCreated = false;
	if (!bAlreadyExists)
	{
		FScopedTransaction Transaction(LOCTEXT("MCPCreateFolder", "MCP Create Folder"));
		bCreated = Subsys->MakeDirectory(NormalizedPath);
		if (!bCreated)
		{
			// Disk-write failure (permissions / disk-full / parent path doesn't exist for
			// non-recursive creates). We've already issued the transaction; nothing rolled back
			// since no asset was modified.
			return FMCPToolHelpers::MakeError(Request, -32000,
				FString::Printf(TEXT("MakeDirectory failed for '%s' (permissions or disk-full?)"),
					*NormalizedPath));
		}
	}

	return FMCPJsonBuilder()
		.Bool(TEXT("created"), bCreated)
		.Str(TEXT("normalized_path"), NormalizedPath)
		.BuildSuccess(Request);
}

// ─── cb.rename ───────────────────────────────────────────────────────────────────────────────
FMCPResponse Tool_Rename(const FMCPRequest& Request)
{
	FString OldNormalized, NewNormalized;
	FMCPResponse Err;
	if (!CB_RequirePath(Request, TEXT("old_path"), OldNormalized, Err)) { return Err; }
	if (!CB_RequirePath(Request, TEXT("new_path"), NewNormalized, Err)) { return Err; }

	UEditorAssetSubsystem* Subsys = CB_GetSubsystem();
	if (Subsys == nullptr)
	{
		return FMCPToolHelpers::MakeError(Request, kCBErrorInternal, TEXT("EditorAssetSubsystem unavailable (no GEditor)"));
	}

	if (!Subsys->DoesAssetExist(OldNormalized))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("source asset '%s' does not exist"), *OldNormalized));
	}
	if (Subsys->DoesAssetExist(NewNormalized))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPathInUse,
			FString::Printf(TEXT("destination '%s' already exists"), *NewNormalized));
	}

	bool bSuccess = false;
	{
		FScopedTransaction Transaction(LOCTEXT("MCPRenameAsset", "MCP Rename Asset"));
		bSuccess = Subsys->RenameAsset(OldNormalized, NewNormalized);
		if (!bSuccess)
		{
			// EditorAssetSubsystem returned false — could be SC reject, readonly file, or a
			// race where the asset got modified by another tool. Transaction auto-rollbacks
			// when bSuccess=false (no Modify() call landed).
			return FMCPToolHelpers::MakeError(Request, -32000,
				FString::Printf(TEXT("RenameAsset returned false for '%s' -> '%s' "
					"(SC reject / readonly file / cross-cook collision?)"),
					*OldNormalized, *NewNormalized));
		}
	}

	return FMCPJsonBuilder()
		.Bool(TEXT("success"), true)
		.Str(TEXT("canonical_new_path"), NewNormalized)
		.BuildSuccess(Request);
}

// ─── cb.save ─────────────────────────────────────────────────────────────────────────────────
FMCPResponse Tool_Save(const FMCPRequest& Request)
{
	FString NormalizedPath;
	FMCPResponse Err;
	if (!CB_RequirePath(Request, TEXT("path"), NormalizedPath, Err)) { return Err; }

	bool bOnlyIfDirty = true;
	Request.Args->TryGetBoolField(TEXT("only_if_dirty"), bOnlyIfDirty);

	UEditorAssetSubsystem* Subsys = CB_GetSubsystem();
	if (Subsys == nullptr)
	{
		return FMCPToolHelpers::MakeError(Request, kCBErrorInternal, TEXT("EditorAssetSubsystem unavailable (no GEditor)"));
	}

	if (!Subsys->DoesAssetExist(NormalizedPath))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("asset '%s' does not exist"), *NormalizedPath));
	}

	// Saves don't participate in undo, so no FScopedTransaction.
	const bool bSaved = Subsys->SaveAsset(NormalizedPath, bOnlyIfDirty);

	return FMCPJsonBuilder()
		.Bool(TEXT("saved"), bSaved)
		.BuildSuccess(Request);
}

// ─── cb.move ─────────────────────────────────────────────────────────────────────────────────
FMCPResponse Tool_Move(const FMCPRequest& Request)
{
	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kCBErrorInvalidParams, TEXT("missing args object"));
	}

	// source_paths: required, non-empty array.
	const TArray<TSharedPtr<FJsonValue>>* SourcesPtr = nullptr;
	if (!Request.Args->TryGetArrayField(TEXT("source_paths"), SourcesPtr)
		|| SourcesPtr == nullptr || SourcesPtr->Num() == 0)
	{
		return FMCPToolHelpers::MakeError(Request, kCBErrorInvalidParams,
			TEXT("missing or empty required array field 'source_paths'"));
	}

	// dest_folder: required string.
	FString DestFolderRaw;
	if (!Request.Args->TryGetStringField(TEXT("dest_folder"), DestFolderRaw) || DestFolderRaw.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kCBErrorInvalidParams,
			TEXT("missing required string field 'dest_folder'"));
	}
	const FString DestFolder = FMCPAssetPathUtils::Normalize(DestFolderRaw);
	if (DestFolder.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidPath,
			FString::Printf(TEXT("invalid dest_folder '%s'"), *DestFolderRaw));
	}

	UEditorAssetSubsystem* Subsys = CB_GetSubsystem();
	if (Subsys == nullptr)
	{
		return FMCPToolHelpers::MakeError(Request, kCBErrorInternal, TEXT("EditorAssetSubsystem unavailable (no GEditor)"));
	}

	if (!Subsys->DoesDirectoryExist(DestFolder))
	{
		// FOLDER_NOT_FOUND is not a distinct code in MCPTypes — surface as ObjectNotFound for
		// consistency with rename/delete (the destination concept is "the folder asset").
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("destination folder '%s' does not exist — call cb.create_folder first"),
				*DestFolder));
	}

	// Pre-validate every source path BEFORE doing any work. Surface INVALID_PATH for malformed
	// entries — per the inline schema, invalid paths are an upfront error, not per-item aggregate.
	TArray<FString> NormalizedSources;
	NormalizedSources.Reserve(SourcesPtr->Num());
	for (const TSharedPtr<FJsonValue>& V : *SourcesPtr)
	{
		FString S;
		if (!V.IsValid() || !V->TryGetString(S))
		{
			return FMCPToolHelpers::MakeError(Request, kCBErrorInvalidParams,
				TEXT("source_paths: expected array of strings"));
		}
		const FString Norm = FMCPAssetPathUtils::Normalize(S);
		if (Norm.IsEmpty())
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidPath,
				FString::Printf(TEXT("source_paths entry '%s' is malformed"), *S));
		}
		NormalizedSources.Add(Norm);
	}

	// Per-asset transaction (D4): each move opens one FScopedTransaction so the user can
	// Ctrl+Z one item at a time. Aggregated into moved[] / failed[].
	TArray<TSharedPtr<FJsonValue>> Moved, Failed;
	Moved.Reserve(NormalizedSources.Num());
	Failed.Reserve(NormalizedSources.Num());

	for (const FString& Src : NormalizedSources)
	{
		const FString Leaf = CB_LeafFromPath(Src);
		const FString Dest = DestFolder + TEXT("/") + Leaf;

		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("from"), Src);

		if (!Subsys->DoesAssetExist(Src))
		{
			Entry->SetStringField(TEXT("error"),
				FString::Printf(TEXT("source asset does not exist: %s"), *Src));
			Failed.Add(MakeShared<FJsonValueObject>(Entry));
			continue;
		}
		if (Subsys->DoesAssetExist(Dest))
		{
			Entry->SetStringField(TEXT("error"),
				FString::Printf(TEXT("destination already exists: %s"), *Dest));
			Failed.Add(MakeShared<FJsonValueObject>(Entry));
			continue;
		}

		bool bOk = false;
		{
			FScopedTransaction Transaction(LOCTEXT("MCPMoveAsset", "MCP Move Asset"));
			bOk = Subsys->RenameAsset(Src, Dest);
		}

		if (bOk)
		{
			Entry->SetStringField(TEXT("to"), Dest);
			Moved.Add(MakeShared<FJsonValueObject>(Entry));
		}
		else
		{
			Entry->SetStringField(TEXT("error"),
				TEXT("RenameAsset returned false (SC reject / readonly / cross-cook?)"));
			Failed.Add(MakeShared<FJsonValueObject>(Entry));
		}
	}

	return FMCPJsonBuilder()
		.Arr(TEXT("moved"), MoveTemp(Moved))
		.Arr(TEXT("failed"), MoveTemp(Failed))
		.BuildSuccess(Request);
}

// ─── cb.duplicate ────────────────────────────────────────────────────────────────────────────
FMCPResponse Tool_Duplicate(const FMCPRequest& Request)
{
	FString Src, Dest;
	FMCPResponse Err;
	if (!CB_RequirePath(Request, TEXT("source_path"), Src,  Err)) { return Err; }
	if (!CB_RequirePath(Request, TEXT("dest_path"),   Dest, Err)) { return Err; }

	UEditorAssetSubsystem* Subsys = CB_GetSubsystem();
	if (Subsys == nullptr)
	{
		return FMCPToolHelpers::MakeError(Request, kCBErrorInternal, TEXT("EditorAssetSubsystem unavailable (no GEditor)"));
	}

	if (!Subsys->DoesAssetExist(Src))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("source asset '%s' does not exist"), *Src));
	}
	if (Subsys->DoesAssetExist(Dest))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPathInUse,
			FString::Printf(TEXT("destination '%s' already exists"), *Dest));
	}

	UObject* NewObj = nullptr;
	{
		FScopedTransaction Transaction(LOCTEXT("MCPDuplicateAsset", "MCP Duplicate Asset"));
		NewObj = Subsys->DuplicateAsset(Src, Dest);
		if (NewObj == nullptr)
		{
			return FMCPToolHelpers::MakeError(Request, -32000,
				FString::Printf(TEXT("DuplicateAsset returned null for '%s' -> '%s' (locked / cross-package?)"),
					*Src, *Dest));
		}
	}

	return FMCPJsonBuilder()
		.Str(TEXT("new_path"), Dest)
		.BuildSuccess(Request);
}

// ─── cb.delete ───────────────────────────────────────────────────────────────────────────────
FMCPResponse Tool_Delete(const FMCPRequest& Request)
{
	FString NormalizedPath;
	FMCPResponse Err;
	if (!CB_RequirePath(Request, TEXT("path"), NormalizedPath, Err)) { return Err; }

	bool bForce = false;
	Request.Args->TryGetBoolField(TEXT("force"), bForce);

	UEditorAssetSubsystem* Subsys = CB_GetSubsystem();
	if (Subsys == nullptr)
	{
		return FMCPToolHelpers::MakeError(Request, kCBErrorInternal, TEXT("EditorAssetSubsystem unavailable (no GEditor)"));
	}

	// Asset-vs-folder guard: cb.delete is per-spec single-asset only. A folder path resolves
	// to no UPackage on disk, so DoesPackageExist returns false even though DoesDirectoryExist
	// might say true. Treat folder paths as INVALID_PATH per M9.
	const bool bDiskExists = FPackageName::DoesPackageExist(NormalizedPath);

	// Wave S fix (2026-05-24): in-memory unsaved assets are valid delete targets too. Discovered
	// via stress test T10 (cleanup phase): assets created in current editor session via
	// bp.create_blueprint / ai.bt.create_asset / etc. live in-memory until SavePackage runs.
	// Prior cb.delete impl required disk presence, so test-artifact cleanup leaked.
	// Fix: detect in-memory presence via FindObject; if present, force-delete it via the
	// ForceDelete path (since there's nothing on disk to leave dangling, force semantics are
	// strictly safer for in-memory cleanup).
	bool bMemoryOnly = false;
	if (!bDiskExists)
	{
		// Distinguish "folder" from "missing asset" — only fail INVALID if it IS a known folder.
		if (Subsys->DoesDirectoryExist(NormalizedPath))
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidPath,
				FString::Printf(TEXT("path '%s' is a folder, not an asset — cb.delete is single-asset only"),
					*NormalizedPath));
		}

		// Check in-memory presence before erroring out.
		const FString ObjectPathForFind = FMCPAssetPathUtils::ToObjectPath(NormalizedPath);
		if (FindObject<UObject>(nullptr, *ObjectPathForFind) != nullptr)
		{
			// In-memory unsaved asset — auto-promote to force-delete since there's nothing on
			// disk to redirector-leave behind.
			bMemoryOnly = true;
			bForce = true;
			UE_LOG(LogMCP, Verbose, TEXT("cb.delete: in-memory only asset '%s' — auto force-delete"),
				*NormalizedPath);
		}
		else
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
				FString::Printf(TEXT("no package on disk OR in memory for '%s'"), *NormalizedPath));
		}
	}

	// DoesAssetExist queries the asset registry which may not have an entry for in-memory unsaved
	// assets (depending on whether FAssetRegistryModule::AssetCreated was called by the creator).
	// For bMemoryOnly path we skip this check and rely on FindObject in the ForceDelete branch.
	if (!bMemoryOnly && !Subsys->DoesAssetExist(NormalizedPath))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("no AssetRegistry entry for '%s'"), *NormalizedPath));
	}

	// R3/M9 logging: every force=true call goes to Display; depth-2 paths additionally to Warning.
	if (bForce)
	{
		UE_LOG(LogMCP, Display, TEXT("MCP cb.delete force=true: %s"), *NormalizedPath);
		if (CB_IsDepth2GamePath(NormalizedPath))
		{
			UE_LOG(LogMCP, Warning,
				TEXT("MCP cb.delete force=true on depth-2 path (likely-mistake guard): %s"),
				*NormalizedPath);
		}
	}

	// Resolve to FAssetData (force=false path) or UObject* (force=true path).
	// bMemoryOnly skips FAssetData resolution — the ForceDelete branch uses FindObject directly.
	FAssetData Data;
	if (!bMemoryOnly && !FMCPAssetPathUtils::ResolveAssetData(NormalizedPath, Data))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("could not resolve FAssetData for '%s'"), *NormalizedPath));
	}

	int32 DeletedCount = 0;
	bool bRedirectorLeft = false;
	{
		FScopedTransaction Transaction(LOCTEXT("MCPDeleteAsset", "MCP Delete Asset"));

		if (bForce)
		{
			// Force path: load + ForceDeleteObjects. We MUST load (FindObject returns null for
			// not-yet-loaded asset). LoadObject may itself dirty the package — acceptable for
			// force-delete since we're about to destroy it.
			const FString ObjectPath = FMCPAssetPathUtils::ToObjectPath(NormalizedPath);
			UObject* Obj = FindObject<UObject>(nullptr, *ObjectPath);
			if (Obj == nullptr)
			{
				Obj = LoadObject<UObject>(nullptr, *ObjectPath);
			}
			if (Obj == nullptr)
			{
				return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
					FString::Printf(TEXT("could not load object '%s' for force-delete"), *ObjectPath));
			}
			DeletedCount = ObjectTools::ForceDeleteObjects({ Obj }, /*bShowConfirmation*/ false);
		}
		else
		{
			// Soft path: DeleteAssets does the reference check and (per D12) MAY autoload the
			// package to walk its referencers. Pass bShowConfirmation=false to suppress the
			// "Confirm Delete" dialog in non-headless mode.
			DeletedCount = ObjectTools::DeleteAssets({ Data }, /*bShowConfirmation*/ false);
		}
	}

	const bool bDeleted = DeletedCount > 0;

	// Inspect AR post-delete for a redirector at the path. force=true never leaves one (object
	// is force-removed); force=false MAY leave one when the asset was referenced.
	if (bDeleted)
	{
		FAssetData Post;
		if (FMCPAssetPathUtils::ResolveAssetData(NormalizedPath, Post))
		{
			static const FTopLevelAssetPath RedirectorClassPath(TEXT("/Script/CoreUObject"), TEXT("ObjectRedirector"));
			bRedirectorLeft = Post.AssetClassPath == RedirectorClassPath;
		}
	}

	if (!bDeleted && !bForce)
	{
		// Most likely cause for soft-delete failure: asset has referencers.
		return FMCPToolHelpers::MakeError(Request, -32000,
			FString::Printf(TEXT("delete failed for '%s' (likely referenced — retry with force=true)"),
				*NormalizedPath));
	}

	return FMCPJsonBuilder()
		.Bool(TEXT("deleted"), bDeleted)
		.Bool(TEXT("redirector_left"), bRedirectorLeft)
		.BuildSuccess(Request);
}

// ─── cb.fix_redirectors ──────────────────────────────────────────────────────────────────────
FMCPResponse Tool_FixRedirectors(const FMCPRequest& Request)
{
	FString NormalizedPath;
	FMCPResponse Err;
	if (!CB_RequirePath(Request, TEXT("path"), NormalizedPath, Err)) { return Err; }

	bool bRecursive = true;
	Request.Args->TryGetBoolField(TEXT("recursive"), bRecursive);

	// Build AR filter scoped to ObjectRedirector under `path`. AR query is Lane-B-safe.
	IAssetRegistry& IAR = FAssetRegistryModule::GetRegistry();
	FARFilter Filter;
	Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/CoreUObject"), TEXT("ObjectRedirector")));
	Filter.PackagePaths.Add(FName(*NormalizedPath));
	Filter.bRecursivePaths = bRecursive;

	TArray<FAssetData> RedirectorData;
	IAR.GetAssets(Filter, RedirectorData);

	// Hard cap per M10 — refuse rather than commit to a multi-minute operation. The cap is
	// pre-load so we never even touch the redirector objects when over-budget.
	constexpr int32 kRedirectorCap = 500;
	if (RedirectorData.Num() > kRedirectorCap)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorOverlyBroadQuery,
			FString::Printf(TEXT("path '%s' contains %d redirectors (cap=%d) — narrow `path` to a deeper subfolder"),
				*NormalizedPath, RedirectorData.Num(), kRedirectorCap));
	}

	if (RedirectorData.Num() == 0)
	{
		// Idempotent: zero work to do is success, not error.
		return FMCPJsonBuilder()
			.Num(TEXT("fixed_count"), 0)
			.Num(TEXT("removed_count"), 0)
			.BuildSuccess(Request);
	}

	// FixupReferencers needs the redirector objects loaded. Loading is per-redirector and
	// must happen on game thread (Lane A).
	TArray<UObjectRedirector*> Redirectors;
	Redirectors.Reserve(RedirectorData.Num());
	for (const FAssetData& Data : RedirectorData)
	{
		const FString ObjectPath = Data.GetObjectPathString();
		UObject* Loaded = FindObject<UObject>(nullptr, *ObjectPath);
		if (Loaded == nullptr)
		{
			Loaded = LoadObject<UObject>(nullptr, *ObjectPath);
		}
		if (UObjectRedirector* Redir = Cast<UObjectRedirector>(Loaded))
		{
			Redirectors.Add(Redir);
		}
	}

	const int32 PreCount = Redirectors.Num();

	{
		FScopedTransaction Transaction(LOCTEXT("MCPFixRedirectors", "MCP Fix Redirectors"));
		FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
		AssetToolsModule.Get().FixupReferencers(
			Redirectors,
			/*bCheckoutDialogPrompt*/ false,
			ERedirectFixupMode::DeleteFixedUpRedirectors);
	}

	// Post-scan: how many redirectors remain? `removed_count` = pre - post.
	TArray<FAssetData> PostData;
	IAR.GetAssets(Filter, PostData);
	const int32 RemovedCount = FMath::Max(0, PreCount - PostData.Num());

	// `fixed_count` is referencers patched — FixupReferencers doesn't return a number directly,
	// so we use redirectors-removed as the closest signal. Both fields in the schema are present;
	// removed_count is authoritative, fixed_count is informational.
	return FMCPJsonBuilder()
		.Num(TEXT("fixed_count"), RemovedCount)
		.Num(TEXT("removed_count"), RemovedCount)
		.BuildSuccess(Request);
}

// ─── cb.list_folders (Lane A post-hotfix — body remains Lane-B-safe for Phase 3+ revival) ──
FMCPResponse Tool_ListFolders(const FMCPRequest& Request)
{
	FString ParentPath;
	FMCPResponse Err;
	if (!CB_RequirePath(Request, TEXT("parent_path"), ParentPath, Err)) { return Err; }

	bool bRecursive = false;
	if (Request.Args.IsValid())
	{
		Request.Args->TryGetBoolField(TEXT("recursive"), bRecursive);
	}

	IAssetRegistry& IAR = FAssetRegistryModule::GetRegistry();

	TArray<FString> SubPaths;
	IAR.GetSubPaths(ParentPath, SubPaths, bRecursive);

	constexpr int32 kFolderCap = 10000;
	if (SubPaths.Num() > kFolderCap)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorOverlyBroadQuery,
			FString::Printf(TEXT("parent_path '%s' enumerates %d folders (cap=%d) — narrow scope"),
				*ParentPath, SubPaths.Num(), kFolderCap));
	}

	// Stable sort for deterministic output (helps clients diffing folder sets across calls).
	SubPaths.Sort();

	TArray<TSharedPtr<FJsonValue>> Items;
	Items.Reserve(SubPaths.Num());
	for (const FString& Folder : SubPaths)
	{
		// Non-recursive asset count per folder. GetAssetsByPath is Lane-B-safe.
		TArray<FAssetData> InFolder;
		IAR.GetAssetsByPath(FName(*Folder), InFolder, /*bRecursive*/ false);

		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("path"), Folder);
		Obj->SetStringField(TEXT("name"), CB_LeafFromPath(Folder));
		Obj->SetNumberField(TEXT("asset_count"), InFolder.Num());
		Items.Add(MakeShared<FJsonValueObject>(Obj));
	}

	return FMCPJsonBuilder()
		.Arr(TEXT("folders"), MoveTemp(Items))
		.BuildSuccess(Request);
}

// ─── cb.import ───────────────────────────────────────────────────────────────────────────────
FMCPResponse Tool_Import(const FMCPRequest& Request)
{
	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kCBErrorInvalidParams, TEXT("missing args object"));
	}

	FString SourceRaw;
	if (!Request.Args->TryGetStringField(TEXT("source_file"), SourceRaw) || SourceRaw.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kCBErrorInvalidParams,
			TEXT("missing required string field 'source_file'"));
	}

	FString DestPath;
	FMCPResponse Err;
	if (!CB_RequirePath(Request, TEXT("dest_path"), DestPath, Err)) { return Err; }

	// Sandbox-check source file (PATH_ESCAPE on whitelist miss).
	FString AbsSource;
	FString SandboxErr;
	if (!FMCPPathSandbox::Resolve(SourceRaw, AbsSource, SandboxErr))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPathEscape, SandboxErr);
	}

	if (!IFileManager::Get().FileExists(*AbsSource))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("source file not found: %s"), *AbsSource));
	}

	bool bReplaceExisting = false;
	Request.Args->TryGetBoolField(TEXT("replace_existing"), bReplaceExisting);

	UEditorAssetSubsystem* Subsys = CB_GetSubsystem();
	if (Subsys != nullptr && Subsys->DoesAssetExist(DestPath) && !bReplaceExisting)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPathInUse,
			FString::Printf(TEXT("dest_path '%s' already exists; pass replace_existing=true to overwrite"),
				*DestPath));
	}

	// Optional explicit factory override. Null = auto-resolve via IAssetTools by extension.
	UFactory* ExplicitFactory = nullptr;
	FString FactoryClassPath;
	if (Request.Args->TryGetStringField(TEXT("factory_class"), FactoryClassPath) && !FactoryClassPath.IsEmpty())
	{
		UClass* FactoryCls = LoadClass<UFactory>(nullptr, *FactoryClassPath);
		if (FactoryCls == nullptr)
		{
			return FMCPToolHelpers::MakeError(Request, kCBErrorInvalidParams,
				FString::Printf(TEXT("factory_class '%s' does not resolve to a UFactory class"),
					*FactoryClassPath));
		}
		ExplicitFactory = NewObject<UFactory>(GetTransientPackage(), FactoryCls);
	}

	// Split dest_path into destination folder + name. AssetImportTask wants them separate.
	const FString DestFolder = FPaths::GetPath(DestPath);
	const FString DestName   = FPaths::GetBaseFilename(DestPath); // last segment, no extension

	UAssetImportTask* Task = NewObject<UAssetImportTask>();
	Task->Filename         = AbsSource;
	Task->DestinationPath  = DestFolder;
	Task->DestinationName  = DestName;
	Task->bAutomated       = true;            // suppress dialogs (FBX modal caveat per M12 doc'd)
	Task->bReplaceExisting = bReplaceExisting;
	Task->bSave            = false;
	Task->Factory          = ExplicitFactory; // null OK — auto-resolve

	FString ImportedPath;
	{
		FScopedTransaction Transaction(LOCTEXT("MCPImport", "MCP Import Asset"));
		FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
		AssetToolsModule.Get().ImportAssetTasks({ Task });

		if (Task->ImportedObjectPaths.Num() == 0)
		{
			return FMCPToolHelpers::MakeError(Request, -32000,
				FString::Printf(TEXT("ImportAssetTasks produced no output for '%s' (factory unresolved / source corrupt?)"),
					*AbsSource));
		}
		ImportedPath = Task->ImportedObjectPaths[0];
	}

	return FMCPJsonBuilder()
		.Str(TEXT("asset_path"), ImportedPath)
		.BuildSuccess(Request);
}

// ─── cb.export ───────────────────────────────────────────────────────────────────────────────
FMCPResponse Tool_Export(const FMCPRequest& Request)
{
	FString NormalizedPath;
	FMCPResponse Err;
	if (!CB_RequirePath(Request, TEXT("path"), NormalizedPath, Err)) { return Err; }

	FString DestFileRaw;
	if (!Request.Args->TryGetStringField(TEXT("dest_file"), DestFileRaw) || DestFileRaw.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kCBErrorInvalidParams,
			TEXT("missing required string field 'dest_file'"));
	}

	// Sandbox-check dest path. Caller-provided path may or may not exist yet.
	FString AbsDest;
	FString SandboxErr;
	if (!FMCPPathSandbox::Resolve(DestFileRaw, AbsDest, SandboxErr))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPathEscape, SandboxErr);
	}

	FAssetData Data;
	if (!FMCPAssetPathUtils::ResolveAssetData(NormalizedPath, Data))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("no AssetRegistry entry for '%s'"), *NormalizedPath));
	}

	// Ensure parent dir exists or ExportAssets silently no-ops.
	const FString ParentDir = FPaths::GetPath(AbsDest);
	IFileManager::Get().MakeDirectory(*ParentDir, /*Tree*/ true);

	// UE 5.0+ canonical export: UAssetExportTask + UExporter::RunAssetExportTask. Handles
	// exporter discovery internally (was broken with IAssetTools::ExportAssets for Texture2D
	// in UE 5.7 since UTextureExporter* classes weren't being auto-instantiated by the older
	// path).
	UObject* AssetToExport = Data.GetAsset();
	if (!AssetToExport)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("asset '%s' could not be loaded"), *NormalizedPath));
	}

	// Find a compatible exporter for the requested file extension. UExporter::FindExporter
	// iterates all loaded UExporter subclasses and matches by SupportedClass + FormatExtension.
	const FString DestExt = FPaths::GetExtension(AbsDest, /*bIncludeDot*/ false).ToLower();
	UExporter* Exporter = nullptr;
	if (!DestExt.IsEmpty())
	{
		Exporter = UExporter::FindExporter(AssetToExport, *DestExt);
	}
	if (!Exporter)
	{
		// Last-ditch: any exporter that supports the asset's class. UE will pick its preferred
		// extension (e.g. .uasset.copy or .fbx for static meshes) but we still write to AbsDest.
		Exporter = UExporter::FindExporter(AssetToExport, TEXT(""));
	}
	if (!Exporter)
	{
		return FMCPToolHelpers::MakeError(Request, -32000,
			FString::Printf(TEXT("no compatible UExporter for class %s + ext '%s' "
								"(try .png/.jpg for textures, .fbx for meshes, .t3d for levels)"),
				*Data.AssetClassPath.ToString(), *DestExt));
	}

	// If AbsDest already exists, delete it so the export doesn't accumulate or fail.
	if (IFileManager::Get().FileExists(*AbsDest))
	{
		IFileManager::Get().Delete(*AbsDest);
	}

	UAssetExportTask* Task = NewObject<UAssetExportTask>();
	Task->Object             = AssetToExport;
	Task->Exporter           = Exporter;
	Task->Filename           = AbsDest;
	Task->bSelected          = false;
	Task->bReplaceIdentical  = true;
	Task->bPrompt            = false;
	Task->bUseFileArchive    = AssetToExport->IsA(UPackage::StaticClass());  // binary archive for packages
	Task->bWriteEmptyFiles   = false;
	Task->bAutomated         = true;
	const bool bExported     = UExporter::RunAssetExportTask(Task);

	if (!bExported || !IFileManager::Get().FileExists(*AbsDest))
	{
		return FMCPToolHelpers::MakeError(Request, -32000,
			FString::Printf(TEXT("UExporter::RunAssetExportTask failed for '%s' (Exporter=%s, dest='%s')"),
				*NormalizedPath, *Exporter->GetClass()->GetName(), *AbsDest));
	}

	const int64 Bytes = IFileManager::Get().FileSize(*AbsDest);

	return FMCPJsonBuilder()
		.Bool(TEXT("exported"), true)
		.Num(TEXT("bytes"), static_cast<double>(Bytes > 0 ? Bytes : 0))
		.BuildSuccess(Request);
}

// ─── cb.save_all_dirty (async, inline lambda body per D5/D10) ───────────────────────────────
FMCPResponse Tool_SaveAllDirty(const FMCPRequest& Request)
{
	bool bIncludeMaps    = true;
	bool bIncludeContent = true;
	if (Request.Args.IsValid())
	{
		Request.Args->TryGetBoolField(TEXT("include_maps"),    bIncludeMaps);
		Request.Args->TryGetBoolField(TEXT("include_content"), bIncludeContent);
	}

	const FGuid JobId = FMCPJobRegistry::Get().SubmitJob(
		TEXT("cb.save_all_dirty"),
		[bIncludeMaps, bIncludeContent](FMCPJob& Job) -> TSharedPtr<FJsonValue>
		{
			const double StartTime = FPlatformTime::Seconds();

			TArray<UPackage*> Dirty;
			if (bIncludeContent) { UEditorLoadingAndSavingUtils::GetDirtyContentPackages(Dirty); }
			if (bIncludeMaps)    { UEditorLoadingAndSavingUtils::GetDirtyMapPackages(Dirty); }

			TArray<TSharedPtr<FJsonValue>> Saved, Failed;
			Saved.Reserve(Dirty.Num());
			Failed.Reserve(Dirty.Num());

			const int32 Total = Dirty.Num();
			for (int32 i = 0; i < Total; ++i)
			{
				// Cooperative cancellation between packages — body has no other safe checkpoint.
				if (Job.bCancelRequested.load(std::memory_order_acquire))
				{
					Job.ErrorMessage = TEXT("cancelled");
					return nullptr; // registry transitions to Cancelled (null + flag set)
				}

				UPackage* Pkg = Dirty[i];
				if (Pkg == nullptr) { continue; }

				Job.Progress.store(static_cast<float>(i) / FMath::Max(1, Total),
					std::memory_order_release);
				Job.Description = FString::Printf(TEXT("Saving %d/%d: %s"),
					i + 1, Total, *Pkg->GetName());

				const bool bOk = UEditorLoadingAndSavingUtils::SavePackages({ Pkg }, /*bOnlyDirty*/ false);
				if (bOk)
				{
					Saved.Add(MakeShared<FJsonValueString>(Pkg->GetName()));
				}
				else
				{
					Failed.Add(MakeShared<FJsonValueObject>(
						CB_MakeFailureEntry(Pkg->GetName(), TEXT("SavePackages returned false"))));
				}
			}

			Job.Progress.store(1.0f, std::memory_order_release);

			const double DurationMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;
			return CB_MakeJobResultObject(Saved, Failed, TEXT("saved"), DurationMs);
		},
		/*bGameThreadRequired*/ true);

	if (!JobId.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorJobSubmitFailed,
			TEXT("FMCPJobRegistry::SubmitJob refused (shutdown?)"));
	}

	return FMCPJsonBuilder()
		.Str(TEXT("job_id"), JobId.ToString(EGuidFormats::DigitsWithHyphens))
		.BuildSuccess(Request);
}

// ─── cb.reimport (refresh asset from source) ─────────────────────────────────────────────────
//
// Args:
//   - asset_path:    string (required)  /Game/.../Asset
//   - source_file:   string (optional)  Override the source file. Default = use the source the
//                                          asset was originally imported from (stored in
//                                          AssetImportData).
//
// Result:
//   - reimported:    bool
//   - source_file:   string  the file actually used
//   - asset_path:    string  echo
//
// PIE-guarded. Lane A (FReimportManager + UObject access).
FMCPResponse Tool_Reimport(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("CBReimport", "MCP: reimport asset"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString NormalizedPath;
	FMCPResponse Err;
	if (!CB_RequirePath(Request, TEXT("asset_path"), NormalizedPath, Err)) { return Err; }

	UObject* AssetObj = StaticLoadObject(UObject::StaticClass(), nullptr, *NormalizedPath);
	if (!AssetObj)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("could not load asset '%s' for reimport"), *NormalizedPath));
	}

	// Optional source-file override — push into AssetImportData->SetSourceFiles before reimport.
	FString SourceFileOverride;
	if (Request.Args.IsValid())
	{
		Request.Args->TryGetStringField(TEXT("source_file"), SourceFileOverride);
	}
	if (!SourceFileOverride.IsEmpty())
	{
		FString AbsSrc;
		FString SandboxErr;
		if (!FMCPPathSandbox::Resolve(SourceFileOverride, AbsSrc, SandboxErr))
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorPathEscape, SandboxErr);
		}
		if (!IFileManager::Get().FileExists(*AbsSrc))
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
				FString::Printf(TEXT("source_file '%s' does not exist on disk"), *AbsSrc));
		}

		// Try to find the asset's UAssetImportData via reflection. Common location:
		// UTexture::AssetImportData, UStaticMesh::AssetImportData, etc.
		FProperty* AssetImportProp = AssetObj->GetClass()->FindPropertyByName(FName(TEXT("AssetImportData")));
		if (AssetImportProp)
		{
			FObjectProperty* ObjProp = CastField<FObjectProperty>(AssetImportProp);
			if (ObjProp)
			{
				UObject* ImportData = ObjProp->GetObjectPropertyValue_InContainer(AssetObj);
				if (UAssetImportData* AID = Cast<UAssetImportData>(ImportData))
				{
					AID->Update(AbsSrc);
				}
			}
		}
		SourceFileOverride = AbsSrc;  // for response echo
	}

	const bool bReimported = FReimportManager::Instance()->Reimport(
		AssetObj,
		/*bAskForNewFileIfMissing*/ false,
		/*bShowNotification*/ false,
		/*PreferredReimportFile*/ SourceFileOverride);

	if (!bReimported)
	{
		return FMCPToolHelpers::MakeError(Request, -32000,
			FString::Printf(TEXT("FReimportManager::Reimport returned false for '%s' "
				"(no factory accepted the reimport; check editor log)"), *NormalizedPath));
	}

	return FMCPJsonBuilder()
		.Bool(TEXT("reimported"), true)
		.Str(TEXT("asset_path"), NormalizedPath)
		.Str(TEXT("source_file"), SourceFileOverride)
		.BuildSuccess(Request);
}

// ─── cb.list_supported_formats (discover factories + supported file extensions) ─────────────
//
// Args:
//   - extension_filter: string (optional)  case-insensitive — only emit factories that support
//                                            this extension (e.g. "fbx" → just FBX factories)
//   - include_class_path: bool (optional)   default true — include supported_class path
//
// Result:
//   - factories[{factory_class_path, supported_class_path, extensions[string], description,
//                supports_new_menu, supports_drag_drop}]
//   - total_known
//
// Lane A (TObjectIterator + UFactory::GetSupportedFileExtensions).
FMCPResponse Tool_ListSupportedFormats(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString ExtensionFilter;
	if (Request.Args.IsValid())
	{
		Request.Args->TryGetStringField(TEXT("extension_filter"), ExtensionFilter);
		ExtensionFilter = ExtensionFilter.ToLower();
		if (ExtensionFilter.StartsWith(TEXT(".")))
		{
			ExtensionFilter = ExtensionFilter.RightChop(1);
		}
	}

	TArray<TSharedPtr<FJsonValue>> Out;
	int32 TotalKnown = 0;

	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* C = *It;
		if (!C || !C->IsChildOf(UFactory::StaticClass()) || C == UFactory::StaticClass())
		{
			continue;
		}
		if (C->HasAnyClassFlags(CLASS_Abstract | CLASS_NewerVersionExists | CLASS_Deprecated))
		{
			continue;
		}

		UFactory* CDO = C->GetDefaultObject<UFactory>();
		if (!CDO) { continue; }

		TArray<FString> Extensions;
		CDO->GetSupportedFileExtensions(Extensions);

		// Filter by extension if requested.
		if (!ExtensionFilter.IsEmpty())
		{
			bool bMatchesExt = false;
			for (const FString& E : Extensions)
			{
				if (E.Equals(ExtensionFilter, ESearchCase::IgnoreCase))
				{
					bMatchesExt = true;
					break;
				}
			}
			if (!bMatchesExt) { continue; }
		}

		++TotalKnown;

		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("factory_class_path"), C->GetPathName());
		Obj->SetStringField(TEXT("supported_class_path"),
			CDO->GetSupportedClass() ? CDO->GetSupportedClass()->GetPathName() : FString());

		TArray<TSharedPtr<FJsonValue>> ExtArr;
		for (const FString& E : Extensions)
		{
			ExtArr.Add(MakeShared<FJsonValueString>(E));
		}
		Obj->SetArrayField(TEXT("extensions"), ExtArr);
		Obj->SetBoolField(TEXT("supports_new_menu"), CDO->ShouldShowInNewMenu());
		Obj->SetBoolField(TEXT("supports_drag_drop"), CDO->FactoryCanImport(TEXT("")));
		Obj->SetStringField(TEXT("description"), CDO->GetDisplayName().ToString());

		Out.Add(MakeShared<FJsonValueObject>(Obj));
	}

	return FMCPJsonBuilder()
		.Arr(TEXT("factories"), MoveTemp(Out))
		.Num(TEXT("total_known"), static_cast<double>(TotalKnown))
		.BuildSuccess(Request);
}

// ─── cb.bulk_import (async, inline lambda body per D5/D10) ──────────────────────────────────
FMCPResponse Tool_BulkImport(const FMCPRequest& Request)
{
	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kCBErrorInvalidParams, TEXT("missing args object"));
	}

	const TArray<TSharedPtr<FJsonValue>>* FilesPtr = nullptr;
	if (!Request.Args->TryGetArrayField(TEXT("source_files"), FilesPtr)
		|| FilesPtr == nullptr || FilesPtr->Num() == 0)
	{
		return FMCPToolHelpers::MakeError(Request, kCBErrorInvalidParams,
			TEXT("missing or empty required array field 'source_files'"));
	}

	FString DestFolderRaw;
	if (!Request.Args->TryGetStringField(TEXT("dest_folder"), DestFolderRaw) || DestFolderRaw.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kCBErrorInvalidParams,
			TEXT("missing required string field 'dest_folder'"));
	}
	const FString DestFolder = FMCPAssetPathUtils::Normalize(DestFolderRaw);
	if (DestFolder.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidPath,
			FString::Printf(TEXT("invalid dest_folder '%s'"), *DestFolderRaw));
	}

	bool bReplaceExisting = false;
	Request.Args->TryGetBoolField(TEXT("replace_existing"), bReplaceExisting);

	// Pre-sandbox every source file. Surface PATH_ESCAPE as a non-aggregate error — the whole
	// batch is rejected if any file is outside the sandbox (don't import half a list).
	TArray<FString> AbsFiles;
	AbsFiles.Reserve(FilesPtr->Num());
	for (const TSharedPtr<FJsonValue>& V : *FilesPtr)
	{
		FString S;
		if (!V.IsValid() || !V->TryGetString(S))
		{
			return FMCPToolHelpers::MakeError(Request, kCBErrorInvalidParams,
				TEXT("source_files: expected array of strings"));
		}
		FString Abs;
		FString SandboxErr;
		if (!FMCPPathSandbox::Resolve(S, Abs, SandboxErr))
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorPathEscape, SandboxErr);
		}
		AbsFiles.Add(Abs);
	}

	const FGuid JobId = FMCPJobRegistry::Get().SubmitJob(
		TEXT("cb.bulk_import"),
		[Files = MoveTemp(AbsFiles), DestFolder, bReplaceExisting](FMCPJob& Job) -> TSharedPtr<FJsonValue>
		{
			const double StartTime = FPlatformTime::Seconds();
			const int32 Total = Files.Num();

			TArray<TSharedPtr<FJsonValue>> Imported, Failed;
			Imported.Reserve(Total);
			Failed.Reserve(Total);

			FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));

			// Batch-build all tasks then submit ONCE to ImportAssetTasks. UE 5.7's AssetTools
			// internally dispatches via Interchange which uses TaskGraph for parallel work; calling
			// ImportAssetTasks per-file in a tight loop tripped TaskGraph's RecursionGuard at
			// Async/TaskGraph.cpp:689 (Interchange's intermediate tasks hadn't drained between
			// iterations). Single batched call lets Interchange manage its own internal pipelining.
			TArray<UAssetImportTask*> Tasks;
			Tasks.Reserve(Total);
			TArray<int32> TaskFileIndex;  // map Tasks[k] back to Files[TaskFileIndex[k]] for reporting
			TaskFileIndex.Reserve(Total);

			for (int32 i = 0; i < Total; ++i)
			{
				if (Job.bCancelRequested.load(std::memory_order_acquire))
				{
					Job.ErrorMessage = TEXT("cancelled");
					return nullptr;
				}

				const FString& File = Files[i];
				if (!IFileManager::Get().FileExists(*File))
				{
					Failed.Add(MakeShared<FJsonValueObject>(
						CB_MakeFailureEntry(File, TEXT("source file not found on disk"))));
					continue;
				}

				UAssetImportTask* Task = NewObject<UAssetImportTask>();
				Task->Filename         = File;
				Task->DestinationPath  = DestFolder;
				Task->DestinationName  = FPaths::GetBaseFilename(File);
				Task->bAutomated       = true;
				Task->bReplaceExisting = bReplaceExisting;
				Task->bSave            = false;
				Tasks.Add(Task);
				TaskFileIndex.Add(i);
			}

			Job.Description = FString::Printf(TEXT("ImportAssetTasks: %d task(s) batched"), Tasks.Num());
			Job.Progress.store(0.5f, std::memory_order_release);

			if (Tasks.Num() > 0)
			{
				AssetToolsModule.Get().ImportAssetTasks(Tasks);
			}

			for (int32 k = 0; k < Tasks.Num(); ++k)
			{
				UAssetImportTask* Task = Tasks[k];
				const FString& File = Files[TaskFileIndex[k]];
				if (Task && Task->ImportedObjectPaths.Num() > 0)
				{
					TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
					Entry->SetStringField(TEXT("source"), File);
					Entry->SetStringField(TEXT("asset_path"), Task->ImportedObjectPaths[0]);
					Imported.Add(MakeShared<FJsonValueObject>(Entry));
				}
				else
				{
					Failed.Add(MakeShared<FJsonValueObject>(
						CB_MakeFailureEntry(File, TEXT("ImportAssetTasks produced no output"))));
				}
			}

			Job.Progress.store(1.0f, std::memory_order_release);

			const double DurationMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;

			// Use a distinct field name "imported" per the spec.
			return MakeShared<FJsonValueObject>(FMCPJsonBuilder()
				.Arr(TEXT("imported"), MoveTemp(Imported))
				.Arr(TEXT("failed"), MoveTemp(Failed))
				.Num(TEXT("duration_ms"), DurationMs)
				.ToShared());
		},
		/*bGameThreadRequired*/ true);

	if (!JobId.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorJobSubmitFailed,
			TEXT("FMCPJobRegistry::SubmitJob refused (shutdown?)"));
	}

	return FMCPJsonBuilder()
		.Str(TEXT("job_id"), JobId.ToString(EGuidFormats::DigitsWithHyphens))
		.BuildSuccess(Request);
}

// ─── Registration ────────────────────────────────────────────────────────────────────────────
void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	// Day 6: creation / metadata.
	RegisterTool(TEXT("cb.create_folder"), &Tool_CreateFolder, /*Lane A*/ false);
	RegisterTool(TEXT("cb.rename"),        &Tool_Rename,       /*Lane A*/ false);
	RegisterTool(TEXT("cb.save"),          &Tool_Save,         /*Lane A*/ false);

	// Day 7: bulk mutations.
	RegisterTool(TEXT("cb.move"),            &Tool_Move,            /*Lane A*/ false);
	RegisterTool(TEXT("cb.duplicate"),       &Tool_Duplicate,       /*Lane A*/ false);
	RegisterTool(TEXT("cb.delete"),          &Tool_Delete,          /*Lane A*/ false);

	// Day 8: redirector + folder enumeration.
	// HOTFIX 2026-05 (Plan R11): cb.list_folders demoted to Lane A. IAR.GetSubPaths shares the
	// AssetRegistry enumeration path with the AR query tools that crash on listener thread in
	// UE 5.7 (GetAssetRegistryTags not fully thread-safe). See AssetRegistryTools.cpp registration
	// block for the full assertion text. Lane B router infrastructure is preserved for Phase 3+.
	RegisterTool(TEXT("cb.fix_redirectors"), &Tool_FixRedirectors,  /*Lane A*/          false);
	RegisterTool(TEXT("cb.list_folders"),    &Tool_ListFolders,     /*Lane A (was B)*/ false);

	// Day 9: import / export.
	RegisterTool(TEXT("cb.import"),          &Tool_Import,          /*Lane A*/ false);
	RegisterTool(TEXT("cb.export"),          &Tool_Export,          /*Lane A*/ false);

	// Day 10: async jobs.
	RegisterTool(TEXT("cb.save_all_dirty"),  &Tool_SaveAllDirty,    /*Lane A*/ false);
	RegisterTool(TEXT("cb.bulk_import"),     &Tool_BulkImport,      /*Lane A*/ false);

	// Asset-import surface additions (2026-05): reimport from source + factory discovery.
	RegisterTool(TEXT("cb.reimport"),               &Tool_Reimport,              /*Lane A*/ false);
	RegisterTool(TEXT("cb.list_supported_formats"), &Tool_ListSupportedFormats,  /*Lane A*/ false);

	UE_LOG(LogMCP, Log, TEXT("Phase 2 hotfix: registered 12 cb.* handlers (all Lane A — UE 5.7 AR not thread-safe)"));
}

} // namespace FContentBrowserTools

#undef LOCTEXT_NAMESPACE

MCP_REGISTER_SURFACE(ContentBrowserTools, &FContentBrowserTools::Register)
