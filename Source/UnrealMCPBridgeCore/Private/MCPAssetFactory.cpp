// Copyright FatumGame. All Rights Reserved.

#include "MCPAssetFactory.h"

#include "MCPToolHelpers.h"  // kMCPErrorInternal / kMCPErrorInvalidParams (inline constexpr in header)
#include "MCPTypes.h"
#include "Utils/MCPAssetPathUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace FMCPAssetFactory
{
	FMCPAssetCreateResult Create(const FString& InAssetPath, UClass* Class, EObjectFlags ObjectFlags)
	{
		check(IsInGameThread());

		FMCPAssetCreateResult R;

		// Defensive: Class can't be null. asserts in Debug; returns error in Release.
		if (!Class)
		{
			R.ErrorCode    = kMCPErrorInternal;
			R.ErrorMessage = TEXT("FMCPAssetFactory::Create: Class is null");
			return R;
		}

		// 1. Path normalize + mount validation. Matches the prior per-surface pattern:
		//    FMCPAssetPathUtils::Normalize → IsValidGameOrPlugin guard.
		const FString DestPathNorm = FMCPAssetPathUtils::Normalize(InAssetPath);
		if (DestPathNorm.IsEmpty() || !FMCPAssetPathUtils::IsValidGameOrPlugin(DestPathNorm))
		{
			R.ErrorCode    = kMCPErrorInvalidPath;
			R.ErrorMessage = FString::Printf(TEXT("path '%s' malformed or unknown mount"), *InAssetPath);
			return R;
		}

		const FString AssetName = FPaths::GetBaseFilename(DestPathNorm);

		// 2. Existence check — package on disk OR loaded UObject with the asset name.
		//    Matches the prior duplicated guard exactly so callers see the same -32014 PathInUse.
		if (FPackageName::DoesPackageExist(DestPathNorm) ||
			FindObject<UObject>(nullptr, *(DestPathNorm + TEXT(".") + AssetName)) != nullptr)
		{
			R.ErrorCode    = kMCPErrorPathInUse;
			R.ErrorMessage = FString::Printf(TEXT("path '%s' already exists"), *DestPathNorm);
			return R;
		}

		// 3. CreatePackage + FullyLoad. CreatePackage returning null is exceptionally rare (OOM
		//    or path-resolver internal failure); we still surface it cleanly rather than crashing.
		UPackage* Package = CreatePackage(*DestPathNorm);
		if (!Package)
		{
			R.ErrorCode    = kMCPErrorInternal;
			R.ErrorMessage = FString::Printf(TEXT("CreatePackage returned null for '%s'"), *DestPathNorm);
			return R;
		}
		Package->FullyLoad();

		// 4. NewObject. Standard "saveable + transactable" object-flag set unless caller overrode.
		UObject* NewAsset = NewObject<UObject>(Package, Class, *AssetName, ObjectFlags);
		if (!NewAsset)
		{
			R.ErrorCode    = kMCPErrorInternal;
			R.ErrorMessage = FString::Printf(
				TEXT("NewObject<%s> returned null for '%s' (abstract class or sandbox restriction?)"),
				*Class->GetName(), *DestPathNorm);
			return R;
		}

		// 5. Asset registry notification. Caller MUST call Scope.DirtyPackage(Result.Package)
		//    AFTER any per-type configure step so the package is marked dirty once with the
		//    fully-populated asset (avoids editor seeing a half-constructed asset during save).
		FAssetRegistryModule::AssetCreated(NewAsset);

		R.Asset     = NewAsset;
		R.Package   = Package;
		R.AssetPath = NewAsset->GetPathName();
		return R;
	}
}
