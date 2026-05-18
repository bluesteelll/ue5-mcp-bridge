// Copyright FatumGame. All Rights Reserved.

#include "AssetRegistryTools.h"

#include "FMCPDispatchQueue.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPAssetPathUtils.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

namespace
{
	// AR_ prefix per the unity-build symbol-collision pattern documented in FMCPMarshalling.cpp /
	// FMCPDay7Handlers.cpp — `MakeError` / `MakeSuccess` collide with UE global templates.
	constexpr int32 kARErrorInvalidParams = -32602;

	void AR_StampIds(const FMCPRequest& Request, FMCPResponse& Response)
	{
		Response.RequestId = Request.RequestId;
		Response.OriginalIdString = Request.OriginalIdString;
	}

	FMCPResponse AR_MakeError(const FMCPRequest& Request, int32 Code, const FString& Message)
	{
		FMCPResponse R;
		AR_StampIds(Request, R);
		R.bIsError = true;
		R.ErrorCode = Code;
		R.ErrorMessage = Message;
		return R;
	}

	FMCPResponse AR_MakeSuccessObj(const FMCPRequest& Request, TSharedPtr<FJsonObject> Result)
	{
		FMCPResponse R;
		AR_StampIds(Request, R);
		R.bIsError = false;
		R.Result = MakeShared<FJsonValueObject>(MoveTemp(Result));
		return R;
	}

	/** Resolve ``args.path`` → normalised string; emit INVALID_PATH (-32010) on failure. */
	bool AR_RequirePath(const FMCPRequest& Request, FString& OutNormalized, FMCPResponse& OutError)
	{
		if (!Request.Args.IsValid())
		{
			OutError = AR_MakeError(Request, kARErrorInvalidParams, TEXT("missing args object"));
			return false;
		}
		FString Raw;
		if (!Request.Args->TryGetStringField(TEXT("path"), Raw) || Raw.IsEmpty())
		{
			OutError = AR_MakeError(Request, kARErrorInvalidParams,
				TEXT("missing required string field 'path'"));
			return false;
		}
		const FString Normalized = FMCPAssetPathUtils::Normalize(Raw);
		if (Normalized.IsEmpty())
		{
			OutError = AR_MakeError(Request, kMCPErrorInvalidPath,
				FString::Printf(TEXT("invalid path '%s' — must be /Game/.../Engine/.../Plugin form, "
					"no backslashes, no '..'"), *Raw));
			return false;
		}
		OutNormalized = Normalized;
		return true;
	}

	/**
	 * Append the raw FAssetData tag set to a JSON object as ``tags: { key: value }``. Used by
	 * asset.metadata, asset.list, asset.search_by_* response shapes.
	 *
	 * Walks via ForEach (no full CopyMap allocation). Values are stringified via .AsString().
	 */
	void AR_AppendTags(const FAssetData& Data, const TSharedPtr<FJsonObject>& OutObj)
	{
		TSharedPtr<FJsonObject> TagsObj = MakeShared<FJsonObject>();
		Data.TagsAndValues.ForEach([&TagsObj](const TPair<FName, FAssetTagValueRef>& Pair)
		{
			TagsObj->SetStringField(Pair.Key.ToString(), Pair.Value.AsString());
		});
		OutObj->SetObjectField(TEXT("tags"), TagsObj);
	}

	/** Best-effort on-disk size in bytes (0 if not present). */
	int64 AR_GetAssetSizeOnDisk(const FAssetData& Data)
	{
		FString PackageFilename;
		if (!FPackageName::DoesPackageExist(Data.PackageName.ToString(), &PackageFilename))
		{
			return 0;
		}
		const int64 Size = IFileManager::Get().FileSize(*PackageFilename);
		return Size > 0 ? Size : 0;
	}
}

namespace FAssetRegistryTools
{

// ─── asset.exists ────────────────────────────────────────────────────────────────────────────
FMCPResponse Tool_AssetExists(const FMCPRequest& Request)
{
	FString NormalizedPath;
	FMCPResponse Err;
	if (!AR_RequirePath(Request, NormalizedPath, Err)) { return Err; }

	// Lane B: read-only AR query. Use a tiny FARFilter with PackageNames so we exactly probe
	// for the single asset rather than walking PackagePaths.
	IAssetRegistry& IAR = FAssetRegistryModule::GetRegistry();
	const FString ObjectPath = FMCPAssetPathUtils::ToObjectPath(NormalizedPath);
	const FSoftObjectPath Soft(ObjectPath);
	const FAssetData Data = IAR.GetAssetByObjectPath(Soft, /*bIncludeOnlyOnDiskAssets*/ false);

	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("exists"), Data.IsValid());
	Out->SetStringField(TEXT("asset_path_canonical"), NormalizedPath);
	return AR_MakeSuccessObj(Request, Out);
}

// ─── asset.metadata ──────────────────────────────────────────────────────────────────────────
FMCPResponse Tool_AssetMetadata(const FMCPRequest& Request)
{
	FString NormalizedPath;
	FMCPResponse Err;
	if (!AR_RequirePath(Request, NormalizedPath, Err)) { return Err; }

	FAssetData Data;
	if (!FMCPAssetPathUtils::ResolveAssetData(NormalizedPath, Data))
	{
		return AR_MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("no AssetRegistry entry for '%s'"), *NormalizedPath));
	}

	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("asset_path"), Data.GetObjectPathString());
	Out->SetStringField(TEXT("package_path"), Data.PackagePath.ToString());
	Out->SetStringField(TEXT("class"), Data.AssetClassPath.ToString());
	Out->SetStringField(TEXT("package"), Data.PackageName.ToString());

	const int64 Size = AR_GetAssetSizeOnDisk(Data);
	if (Size > 0)
	{
		Out->SetNumberField(TEXT("size_disk"), static_cast<double>(Size));
	}
	else
	{
		Out->SetField(TEXT("size_disk"), MakeShared<FJsonValueNull>());
	}

	AR_AppendTags(Data, Out);
	return AR_MakeSuccessObj(Request, Out);
}

// ─── asset.get_outermost_package ─────────────────────────────────────────────────────────────
FMCPResponse Tool_AssetGetOutermostPackage(const FMCPRequest& Request)
{
	FString NormalizedPath;
	FMCPResponse Err;
	if (!AR_RequirePath(Request, NormalizedPath, Err)) { return Err; }

	FAssetData Data;
	if (!FMCPAssetPathUtils::ResolveAssetData(NormalizedPath, Data))
	{
		return AR_MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("no AssetRegistry entry for '%s'"), *NormalizedPath));
	}

	const FString PackageNameStr = Data.PackageName.ToString();
	const bool bOnDisk = FPackageName::DoesPackageExist(PackageNameStr);

	// package_flags: read live UPackage if loaded; else 0 (we deliberately don't load — that
	// would mutate in-memory state on a Lane B query).
	uint32 Flags = 0;
	if (UPackage* P = FindPackage(nullptr, *PackageNameStr))
	{
		Flags = P->GetPackageFlags();
	}

	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("package_path"), PackageNameStr);
	Out->SetBoolField(TEXT("on_disk"), bOnDisk);
	Out->SetNumberField(TEXT("package_flags"), static_cast<double>(Flags));
	return AR_MakeSuccessObj(Request, Out);
}

// ─── asset.is_dirty (Lane A — touches loaded-package map) ────────────────────────────────────
FMCPResponse Tool_AssetIsDirty(const FMCPRequest& Request)
{
	FString NormalizedPath;
	FMCPResponse Err;
	if (!AR_RequirePath(Request, NormalizedPath, Err)) { return Err; }

	// We require the asset's AR entry exists — symmetry with the other tools' NOT_FOUND surface.
	FAssetData Data;
	if (!FMCPAssetPathUtils::ResolveAssetData(NormalizedPath, Data))
	{
		return AR_MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("no AssetRegistry entry for '%s'"), *NormalizedPath));
	}

	const FString PackageNameStr = Data.PackageName.ToString();
	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	if (UPackage* P = FindPackage(nullptr, *PackageNameStr))
	{
		Out->SetBoolField(TEXT("dirty"), P->IsDirty());
		Out->SetBoolField(TEXT("in_memory"), true);
	}
	else
	{
		// Deliberate: we do NOT autoload — that would mutate in-memory state silently. Contrast
		// with cb.delete force=false which MAY autoload during its reference walk.
		Out->SetBoolField(TEXT("dirty"), false);
		Out->SetBoolField(TEXT("in_memory"), false);
	}
	return AR_MakeSuccessObj(Request, Out);
}

// ─── Day 3+ tools: stubs to be implemented in subsequent commits ─────────────────────────────
FMCPResponse Tool_AssetList(const FMCPRequest& Request)
{
	return AR_MakeError(Request, -32601, TEXT("asset.list — Day 3 stub (not yet implemented)"));
}
FMCPResponse Tool_AssetFindReferences(const FMCPRequest& Request)
{
	return AR_MakeError(Request, -32601, TEXT("asset.find_references — Day 4 stub"));
}
FMCPResponse Tool_AssetFindDependents(const FMCPRequest& Request)
{
	return AR_MakeError(Request, -32601, TEXT("asset.find_dependents — Day 4 stub"));
}
FMCPResponse Tool_AssetSearchByClass(const FMCPRequest& Request)
{
	return AR_MakeError(Request, -32601, TEXT("asset.search_by_class — Day 3 stub"));
}
FMCPResponse Tool_AssetSearchByTag(const FMCPRequest& Request)
{
	return AR_MakeError(Request, -32601, TEXT("asset.search_by_tag — Day 3 stub"));
}
FMCPResponse Tool_AssetSearchByName(const FMCPRequest& Request)
{
	return AR_MakeError(Request, -32601, TEXT("asset.search_by_name — Day 3 stub"));
}
FMCPResponse Tool_AssetGetThumbnail(const FMCPRequest& Request)
{
	return AR_MakeError(Request, -32601, TEXT("asset.get_thumbnail — Day 5 stub"));
}
FMCPResponse Tool_AssetGetThumbnailToDisk(const FMCPRequest& Request)
{
	return AR_MakeError(Request, -32601, TEXT("asset.get_thumbnail_to_disk — Day 5 stub"));
}
FMCPResponse Tool_AssetGetClassHierarchy(const FMCPRequest& Request)
{
	return AR_MakeError(Request, -32601, TEXT("asset.get_class_hierarchy — Day 4 stub"));
}

// ─── Registration ────────────────────────────────────────────────────────────────────────────
void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	// Per the plan baseline 11 of 12 register with bThreadSafe=true (Lane B). Audit demotion list
	// from Tests/lane_b_audit_results.md applies after Day 0b spike runs — until then we ship the
	// optimistic baseline since the AR read API is documented thread-safe since UE 5.0.
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	// Day 2: first 4 tools (3 Lane B + 1 Lane A).
	RegisterTool(TEXT("asset.exists"),                &Tool_AssetExists,                /*Lane B*/ true);
	RegisterTool(TEXT("asset.metadata"),              &Tool_AssetMetadata,              /*Lane B*/ true);
	RegisterTool(TEXT("asset.get_outermost_package"), &Tool_AssetGetOutermostPackage,   /*Lane B*/ true);
	RegisterTool(TEXT("asset.is_dirty"),              &Tool_AssetIsDirty,               /*Lane A*/ false);

	// Day 3+ stubs — register so the Day 0b audit harness has something to probe; the bodies
	// return method-not-found until the implementation day. Each is wired with the eventual
	// Lane B flag so once the body lands the routing is already correct.
	RegisterTool(TEXT("asset.list"),                  &Tool_AssetList,                  /*Lane B*/ true);
	RegisterTool(TEXT("asset.find_references"),       &Tool_AssetFindReferences,        /*Lane B*/ true);
	RegisterTool(TEXT("asset.find_dependents"),       &Tool_AssetFindDependents,        /*Lane B*/ true);
	RegisterTool(TEXT("asset.search_by_class"),       &Tool_AssetSearchByClass,         /*Lane B*/ true);
	RegisterTool(TEXT("asset.search_by_tag"),         &Tool_AssetSearchByTag,           /*Lane B*/ true);
	RegisterTool(TEXT("asset.search_by_name"),        &Tool_AssetSearchByName,          /*Lane B*/ true);
	RegisterTool(TEXT("asset.get_thumbnail"),         &Tool_AssetGetThumbnail,          /*Lane A*/ false);
	RegisterTool(TEXT("asset.get_thumbnail_to_disk"), &Tool_AssetGetThumbnailToDisk,    /*Lane A*/ false);
	RegisterTool(TEXT("asset.get_class_hierarchy"),   &Tool_AssetGetClassHierarchy,     /*Lane B*/ true);

	UE_LOG(LogMCP, Log, TEXT("Phase 2 Day 1-2: registered asset.* handlers (4 active + 9 stubs)"));
}

} // namespace FAssetRegistryTools
