// Copyright FatumGame. All Rights Reserved.

#include "PackageTools.h"

#include "FMCPDispatchQueue.h"
#include "MCPJsonBuilder.h"
#include "MCPToolHelpers.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPAssetPathUtils.h"
#include "Utils/MCPWorldContext.h"

#include "AssetRegistry/AssetIdentifier.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// PKG_ prefix per the unity-build symbol-collision convention. The plugin uses unity builds so
	// anonymous-namespace helpers MUST be uniquely prefixed across every Tools/*.cpp — see the
	// BlueprintComponentTools rename note in MEMORY.md (Wave F4) for the failure mode.
	//
	// XX_StampIds / XX_MakeError / XX_MakeSuccessObj / XX_RequireStringField removed in Phase 2 —
	// use FMCPToolHelpers::Xxx from MCPToolHelpers.h. Per-surface error constants kept for now
	// (Phase 4 sweep target).
	constexpr int32 kPKGErrorInternal = -32603;

	/**
	 * Normalise the caller's package_path arg and return the canonical package-name form
	 * (``/Game/Foo/Bar``, no leaf-suffix, no trailing slash). Emits -32010 on malformed input
	 * (backslash, ``..``, unknown mount, empty).
	 */
	bool PKG_RequirePackagePath(const FMCPRequest& Request, FString& OutPackageName,
		FMCPResponse& OutError)
	{
		FString Raw;
		if (!FMCPToolHelpers::RequireStringField(Request, TEXT("package_path"), Raw, OutError)) { return false; }

		const FString Normalised = FMCPAssetPathUtils::Normalize(Raw);
		if (Normalised.IsEmpty() || !FMCPAssetPathUtils::IsValidGameOrPlugin(Normalised))
		{
			OutError = FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidPath,
				FString::Printf(TEXT("package_path '%s' malformed or unknown mount"), *Raw));
			return false;
		}
		// Normalize already strips ``.LeafName``; this is the canonical package-name form.
		OutPackageName = FMCPAssetPathUtils::ToPackageName(Normalised);
		return true;
	}

	/**
	 * Derive the on-disk filename for ``Pkg``. Uses the engine's canonical extension picker —
	 * maps get ``.umap``, everything else gets ``.uasset``. Returns empty string when
	 * LongPackageNameToFilename rejects the input (which should not happen for a loaded package
	 * — we still guard against it via a check at the call site).
	 */
	FString PKG_DeriveFilename(const UPackage* Pkg)
	{
		check(Pkg);
		const FString& Extension = Pkg->ContainsMap()
			? FPackageName::GetMapPackageExtension()
			: FPackageName::GetAssetPackageExtension();
		return FPackageName::LongPackageNameToFilename(Pkg->GetName(), Extension);
	}

	/**
	 * Count assets (UObjects with the package as outermost) for the list_dirty response. Walks
	 * the loaded-object graph via ForEachObjectWithOuter — bounded by the package's own outer chain.
	 * Used purely for the diagnostic ``asset_count`` field; callers don't pivot logic on it.
	 */
	int32 PKG_CountAssetsInPackage(UPackage* Pkg)
	{
		check(Pkg);
		int32 Count = 0;
		ForEachObjectWithOuter(Pkg, [&Count](UObject* /*Inner*/)
		{
			++Count;
		}, /*bIncludeNestedObjects*/ false);
		return Count;
	}

	/** True if Pkg is one of the "skip this in bulk save / never let caller save" packages —
	 * transient, /Engine/Transient, /Temp/, or /Script/* (native module packages — saving these
	 * is undefined and historically crashes the editor in 5.7).
	 */
	bool PKG_IsTransientPackage(const UPackage* Pkg)
	{
		check(Pkg);
		if (Pkg->HasAnyFlags(RF_Transient)) { return true; }
		const FString Name = Pkg->GetName();
		// /Engine/Transient: anonymous-package mount for throwaway temp objects.
		// /Temp/: secondary throwaway mount used by some editor commands.
		// /Script/*: native module packages (one per UCLASS-bearing C++ module). Saving these
		// is meaningless (their data is the compiled .dll) AND triggers a hard crash in UE 5.7's
		// SavePackage path when the package's loader is the native-class loader. Discovered
		// during Wave I S1 testing — the editor crashed when package.save("/Script/Engine") was
		// invoked.
		return Name.StartsWith(TEXT("/Engine/Transient"), ESearchCase::CaseSensitive)
			|| Name.StartsWith(TEXT("/Temp/"), ESearchCase::CaseSensitive)
			|| Name.StartsWith(TEXT("/Script/"), ESearchCase::CaseSensitive);
	}

	/**
	 * Build the EDependencyCategory + FDependencyQuery from the include_hard / include_soft
	 * boolean pair. Returns the category mask (Package only — SearchableName not exposed at
	 * this surface) + Hard/Soft filter inside the Package category.
	 *
	 * Mirrors ``AR_BuildDependencyQuery`` in AssetRegistryTools.cpp — kept self-contained here
	 * so PackageTools doesn't reach across into AssetRegistryTools' private helpers.
	 */
	void PKG_BuildDependencyQuery(bool bIncludeHard, bool bIncludeSoft,
		UE::AssetRegistry::EDependencyCategory& OutCategory,
		UE::AssetRegistry::FDependencyQuery& OutQuery)
	{
		OutCategory = UE::AssetRegistry::EDependencyCategory::Package;

		// Default = unfiltered Package query (both hard and soft come back). If only one flag is
		// true, ask the registry to filter for us via Hard/NotHard. The "both flags false" case is
		// handled at the call site as an early empty-result short-circuit — at this layer we leave
		// OutQuery in the unfiltered state (would otherwise return all results, which violates the
		// caller's intent).
		OutQuery = UE::AssetRegistry::FDependencyQuery();
		if (bIncludeHard && !bIncludeSoft)
		{
			OutQuery = UE::AssetRegistry::FDependencyQuery(UE::AssetRegistry::EDependencyQuery::Hard);
		}
		else if (!bIncludeHard && bIncludeSoft)
		{
			OutQuery = UE::AssetRegistry::FDependencyQuery(UE::AssetRegistry::EDependencyQuery::NotHard);
		}
	}

	/**
	 * Classify a returned ``FAssetDependency`` as ``"Hard"`` / ``"Soft"`` / ``"SearchableName"``.
	 * Mirrors ``AR_ClassifyDependency`` in AssetRegistryTools.cpp.
	 */
	const TCHAR* PKG_ClassifyDependency(const FAssetDependency& Dep)
	{
		if (Dep.Category == UE::AssetRegistry::EDependencyCategory::SearchableName)
		{
			return TEXT("SearchableName");
		}
		return EnumHasAnyFlags(Dep.Properties, UE::AssetRegistry::EDependencyProperty::Hard)
			? TEXT("Hard") : TEXT("Soft");
	}

	/**
	 * BFS over GetDependencies starting at ``Root``. Visited-set capped at 10k entries (mirrors
	 * the AssetRegistryTools cap). Returns false + populates OutError when the cap trips.
	 *
	 * Used by Tool_GetDependencies in recursive mode. get_referencers does NOT use BFS — the
	 * referencer graph for engine assets fans out catastrophically.
	 */
	bool PKG_WalkDependenciesBFS(IAssetRegistry& IAR, const FName& Root,
		UE::AssetRegistry::EDependencyCategory Category,
		const UE::AssetRegistry::FDependencyQuery& Query,
		int32 MaxVisited,
		TArray<FAssetDependency>& OutAll,
		FString& OutError)
	{
		TSet<FName> Visited;
		Visited.Reserve(64);
		Visited.Add(Root);

		TQueue<FName> Frontier;
		Frontier.Enqueue(Root);

		while (!Frontier.IsEmpty())
		{
			FName Cur;
			Frontier.Dequeue(Cur);

			TArray<FAssetDependency> ThisHop;
			IAR.GetDependencies(FAssetIdentifier(Cur), ThisHop, Category, Query);

			for (const FAssetDependency& Dep : ThisHop)
			{
				const FName Next = Dep.AssetId.PackageName;
				if (Next.IsNone()) { continue; }

				OutAll.Add(Dep);

				if (!Visited.Contains(Next))
				{
					Visited.Add(Next);
					if (Visited.Num() > MaxVisited)
					{
						OutError = FString::Printf(
							TEXT("dependency walk visited %d entries, exceeding the %d cap; "
								 "set recursive=false or narrow the root package"),
							Visited.Num(), MaxVisited);
						return false;
					}
					Frontier.Enqueue(Next);
				}
			}
		}
		return true;
	}
} // namespace

namespace FPackageTools
{

// ─── package.save ─────────────────────────────────────────────────────────────────────────────
//
// Args:    { package_path: string }
// Result:  { saved: bool, was_dirty: bool, file_path: string }
//
// PIE-guarded mutator. Refuses with -32004 ObjectNotFound when the package is not loaded —
// saving an unloaded package is a no-op by definition (there is no in-memory state to flush),
// surfacing this as a hard error matches the contract of every other write-side tool.
//
// The was_dirty flag reports the PRE-save state so callers can detect "no actual change written"
// (was_dirty=false → file was rewritten with byte-identical content — the operation still
// returns saved=true because UPackage::SavePackage succeeded).
//
// Filename derivation uses ContainsMap() to pick .umap vs .uasset. This matches Unreal's
// internal save-path convention.
FMCPResponse Tool_Save(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (FMCPWorldContext::IsPIEActive())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPIEActive, kMCPMessagePIEActive);
	}

	FString PackageName;
	FMCPResponse Err;
	if (!PKG_RequirePackagePath(Request, PackageName, Err)) { return Err; }

	// FindPackage (NOT LoadPackage) — saving an unloaded package is a no-op by definition, and
	// LoadPackage would have the side-effect of pulling the asset into memory just to write it
	// straight back to disk. If the caller wants to save an unloaded package, they should load
	// it explicitly first (e.g. via asset.load_object or by referencing it from some other tool).
	UPackage* Pkg = FindPackage(nullptr, *PackageName);
	if (!Pkg)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("package '%s' is not loaded — nothing to save"), *PackageName));
	}

	// Refuse to save script/transient packages — saving /Script/* crashes UE 5.7's SavePackage
	// path (the native-loader-backed package has no on-disk asset file). Discovered during
	// Wave I S1 testing — editor crash with exit code 3 on package.save("/Script/Engine").
	if (PKG_IsTransientPackage(Pkg))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidPath,
			FString::Printf(TEXT("package '%s' is transient/script (cannot be saved to disk)"),
				*PackageName));
	}

	const bool bWasDirty = Pkg->IsDirty();

	const FString Filename = PKG_DeriveFilename(Pkg);
	if (Filename.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kPKGErrorInternal,
			FString::Printf(TEXT("could not derive on-disk filename for package '%s'"), *PackageName));
	}

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Standalone;
	SaveArgs.SaveFlags = SAVE_NoError;
	const bool bSaved = UPackage::SavePackage(Pkg, /*InAsset*/ nullptr, *Filename, SaveArgs);

	if (!bSaved)
	{
		// No dedicated "save failed" code exists in the legacy range — use InternalError. The
		// brief named -32015 OperationFailed but the actual MCPTypes.h defines that slot as
		// StaleCursor (paginator codepath). InternalError + descriptive message is the closer
		// semantic match for "UE engine API said no" with no further info available.
		return FMCPToolHelpers::MakeError(Request, kPKGErrorInternal,
			FString::Printf(TEXT("UPackage::SavePackage returned false for '%s' (filename '%s')"),
				*PackageName, *Filename));
	}

	return FMCPJsonBuilder()
		.Bool(TEXT("saved"), true)
		.Bool(TEXT("was_dirty"), bWasDirty)
		.Str(TEXT("file_path"), Filename)
		.Str(TEXT("package_path"), PackageName)
		.BuildSuccess(Request);
}

// ─── package.save_all ─────────────────────────────────────────────────────────────────────────
//
// Args:    { only_dirty?: bool=true, max_packages?: int=500 }
// Result:  { saved: int, failed: int, failures: [{ package_path, reason }] }
//
// PIE-guarded mutator. Iterates every loaded UPackage via TObjectIterator<UPackage>; filters
// out RF_Transient and /Engine/Transient packages; respects only_dirty (default true) and
// caps the total to max_packages (default 500, soft guardrail — caller can raise it).
//
// Per-package failures are collected into the failures[] array — partial successes are NOT
// rolled back. The synchronous nature of this tool (no job submission) makes it suitable for
// modest batches (< 500 packages); for large bulk-saves prefer the job-backed cb.save_all_dirty
// or level.save_all_dirty tools.
FMCPResponse Tool_SaveAll(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (FMCPWorldContext::IsPIEActive())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPIEActive, kMCPMessagePIEActive);
	}

	bool bOnlyDirty = true;
	int32 MaxPackages = 500;
	if (Request.Args.IsValid())
	{
		Request.Args->TryGetBoolField(TEXT("only_dirty"), bOnlyDirty);
		Request.Args->TryGetNumberField(TEXT("max_packages"), MaxPackages);
	}
	MaxPackages = FMath::Clamp(MaxPackages, 1, 10000);

	TArray<UPackage*> Candidates;
	Candidates.Reserve(256);
	for (TObjectIterator<UPackage> It; It; ++It)
	{
		UPackage* P = *It;
		if (!P) { continue; }
		if (PKG_IsTransientPackage(P)) { continue; }
		if (bOnlyDirty && !P->IsDirty()) { continue; }
		Candidates.Add(P);
		if (Candidates.Num() >= MaxPackages) { break; }
	}

	int32 SavedCount = 0;
	TArray<TSharedPtr<FJsonValue>> Failures;

	for (UPackage* P : Candidates)
	{
		check(P);
		const FString PkgName = P->GetName();
		const FString Filename = PKG_DeriveFilename(P);
		if (Filename.IsEmpty())
		{
			TSharedRef<FJsonObject> FailObj = MakeShared<FJsonObject>();
			FailObj->SetStringField(TEXT("package_path"), PkgName);
			FailObj->SetStringField(TEXT("reason"), TEXT("could not derive on-disk filename"));
			Failures.Add(MakeShared<FJsonValueObject>(FailObj));
			continue;
		}

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Standalone | RF_Public;
		SaveArgs.SaveFlags = SAVE_NoError;
		const bool bSaved = UPackage::SavePackage(P, /*InAsset*/ nullptr, *Filename, SaveArgs);
		if (bSaved)
		{
			++SavedCount;
		}
		else
		{
			TSharedRef<FJsonObject> FailObj = MakeShared<FJsonObject>();
			FailObj->SetStringField(TEXT("package_path"), PkgName);
			FailObj->SetStringField(TEXT("reason"), TEXT("UPackage::SavePackage returned false"));
			Failures.Add(MakeShared<FJsonValueObject>(FailObj));
		}
	}

	const int32 FailureCount = Failures.Num();
	return FMCPJsonBuilder()
		.Num(TEXT("saved"), SavedCount)
		.Num(TEXT("failed"), FailureCount)
		.Arr(TEXT("failures"), MoveTemp(Failures))
		.Num(TEXT("total_candidates"), Candidates.Num())
		.Bool(TEXT("only_dirty"), bOnlyDirty)
		.Num(TEXT("max_packages"), MaxPackages)
		.BuildSuccess(Request);
}

// ─── package.list_dirty ───────────────────────────────────────────────────────────────────────
//
// Args:    (none)
// Result:  { dirty_packages: [{ path, asset_count, transient }], total_dirty }
//
// Read-only — no PIE guard. Iterates every loaded UPackage via TObjectIterator and reports the
// dirty ones. asset_count is the count of top-level objects whose outer is the package (cheap
// to compute — bounded by the in-memory size of each package).
//
// transient flag tells the caller whether this dirty package would be skipped by a subsequent
// save_all call — transient packages are always RF_Transient OR /Engine/Transient + /Temp/ mounted.
FMCPResponse Tool_ListDirty(const FMCPRequest& Request)
{
	check(IsInGameThread());

	TArray<TSharedPtr<FJsonValue>> DirtyArr;
	int32 TotalDirty = 0;

	for (TObjectIterator<UPackage> It; It; ++It)
	{
		UPackage* P = *It;
		if (!P) { continue; }
		if (!P->IsDirty()) { continue; }
		++TotalDirty;

		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("path"), P->GetName());
		Obj->SetNumberField(TEXT("asset_count"), PKG_CountAssetsInPackage(P));
		Obj->SetBoolField(TEXT("transient"), PKG_IsTransientPackage(P));
		DirtyArr.Add(MakeShared<FJsonValueObject>(Obj));
	}

	return FMCPJsonBuilder()
		.Arr(TEXT("dirty_packages"), MoveTemp(DirtyArr))
		.Num(TEXT("total_dirty"), TotalDirty)
		.BuildSuccess(Request);
}

// ─── package.get_dependencies ─────────────────────────────────────────────────────────────────
//
// Args:    { package_path: string, recursive?: bool=false, include_hard?: bool=true,
//            include_soft?: bool=true }
// Result:  { dependencies: [{ path, dep_type }], total }
//
// Read-only — no PIE guard. dep_type is "Hard" | "Soft" | "SearchableName".
//
// Recursive mode performs a BFS walk with a 10k visited-set cap (matches AssetRegistryTools
// pattern). The cap protects against pathological graphs like default materials referencing
// thousands of texture assets transitively.
//
// Asset-registry-only path — does NOT load the target package. The registry knows the dependency
// graph from disk metadata without needing to touch UObjects. This means dependencies of newly-
// created (in-memory only) packages won't surface until the package is saved (and the registry
// re-scans).
FMCPResponse Tool_GetDependencies(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString PackageName;
	FMCPResponse Err;
	if (!PKG_RequirePackagePath(Request, PackageName, Err)) { return Err; }

	bool bRecursive   = false;
	bool bIncludeHard = true;
	bool bIncludeSoft = true;
	if (Request.Args.IsValid())
	{
		Request.Args->TryGetBoolField(TEXT("recursive"), bRecursive);
		Request.Args->TryGetBoolField(TEXT("include_hard"), bIncludeHard);
		Request.Args->TryGetBoolField(TEXT("include_soft"), bIncludeSoft);
	}

	UE::AssetRegistry::EDependencyCategory Category;
	UE::AssetRegistry::FDependencyQuery Query;
	PKG_BuildDependencyQuery(bIncludeHard, bIncludeSoft, Category, Query);

	IAssetRegistry& IAR = FAssetRegistryModule::GetRegistry();
	const FName RootName(*PackageName);

	TArray<FAssetDependency> All;
	// Wave I bug-fix: when caller asks to include NEITHER hard NOR soft deps, the underlying
	// FDependencyQuery has no usable filter (NotHard would still include soft, Hard would
	// include hard) so the registry would return the full result set. Short-circuit to an
	// empty result instead — that matches the caller's intent ("show me no dep types").
	if (!bIncludeHard && !bIncludeSoft)
	{
		// All stays empty; downstream sort+serialize is a no-op for an empty array.
	}
	else if (bRecursive)
	{
		FString WalkErr;
		if (!PKG_WalkDependenciesBFS(IAR, RootName, Category, Query, /*MaxVisited*/ 10000, All, WalkErr))
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorOverlyBroadQuery, WalkErr);
		}
	}
	else
	{
		IAR.GetDependencies(FAssetIdentifier(RootName), All, Category, Query);
	}

	// Stable sort by PackageName lex order for deterministic output across calls.
	All.StableSort([](const FAssetDependency& A, const FAssetDependency& B)
	{
		return A.AssetId.PackageName.ToString().Compare(
			B.AssetId.PackageName.ToString(), ESearchCase::IgnoreCase) < 0;
	});

	TArray<TSharedPtr<FJsonValue>> Items;
	Items.Reserve(All.Num());
	for (const FAssetDependency& Dep : All)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("path"), Dep.AssetId.PackageName.ToString());
		Obj->SetStringField(TEXT("dep_type"), PKG_ClassifyDependency(Dep));
		Items.Add(MakeShared<FJsonValueObject>(Obj));
	}

	const int32 TotalDeps = All.Num();
	return FMCPJsonBuilder()
		.Arr(TEXT("dependencies"), MoveTemp(Items))
		.Num(TEXT("total"), TotalDeps)
		.Str(TEXT("package_path"), PackageName)
		.Bool(TEXT("recursive"), bRecursive)
		.BuildSuccess(Request);
}

// ─── package.get_referencers ──────────────────────────────────────────────────────────────────
//
// Args:    { package_path: string, include_hard?: bool=true, include_soft?: bool=true }
// Result:  { referencers: [{ path, dep_type }], total }
//
// Read-only — no PIE guard. Single-hop only (no recursive flag) because the referencer graph
// for common engine assets (e.g. base materials, default textures) can fan out catastrophically;
// a recursive walk would return tens of thousands of packages. Callers that need a transitive
// dependent-set should use the job-backed asset.find_references tool instead.
//
// Same dep_type classification as get_dependencies — "Hard" | "Soft" | "SearchableName".
FMCPResponse Tool_GetReferencers(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString PackageName;
	FMCPResponse Err;
	if (!PKG_RequirePackagePath(Request, PackageName, Err)) { return Err; }

	bool bIncludeHard = true;
	bool bIncludeSoft = true;
	if (Request.Args.IsValid())
	{
		Request.Args->TryGetBoolField(TEXT("include_hard"), bIncludeHard);
		Request.Args->TryGetBoolField(TEXT("include_soft"), bIncludeSoft);
	}

	UE::AssetRegistry::EDependencyCategory Category;
	UE::AssetRegistry::FDependencyQuery Query;
	PKG_BuildDependencyQuery(bIncludeHard, bIncludeSoft, Category, Query);

	IAssetRegistry& IAR = FAssetRegistryModule::GetRegistry();
	const FName RootName(*PackageName);

	TArray<FAssetDependency> All;
	// Wave I bug-fix: short-circuit empty when neither filter flag is on (see get_dependencies).
	if (bIncludeHard || bIncludeSoft)
	{
		IAR.GetReferencers(FAssetIdentifier(RootName), All, Category, Query);
	}

	All.StableSort([](const FAssetDependency& A, const FAssetDependency& B)
	{
		return A.AssetId.PackageName.ToString().Compare(
			B.AssetId.PackageName.ToString(), ESearchCase::IgnoreCase) < 0;
	});

	TArray<TSharedPtr<FJsonValue>> Items;
	Items.Reserve(All.Num());
	for (const FAssetDependency& Dep : All)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("path"), Dep.AssetId.PackageName.ToString());
		Obj->SetStringField(TEXT("dep_type"), PKG_ClassifyDependency(Dep));
		Items.Add(MakeShared<FJsonValueObject>(Obj));
	}

	const int32 TotalRefs = All.Num();
	return FMCPJsonBuilder()
		.Arr(TEXT("referencers"), MoveTemp(Items))
		.Num(TEXT("total"), TotalRefs)
		.Str(TEXT("package_path"), PackageName)
		.BuildSuccess(Request);
}

// ─── Registration ─────────────────────────────────────────────────────────────────────────────
void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	RegisterTool(TEXT("package.save"),             &Tool_Save,            /*Lane A*/ false);
	RegisterTool(TEXT("package.save_all"),         &Tool_SaveAll,         /*Lane A*/ false);
	RegisterTool(TEXT("package.list_dirty"),       &Tool_ListDirty,       /*Lane A*/ false);
	RegisterTool(TEXT("package.get_dependencies"), &Tool_GetDependencies, /*Lane A*/ false);
	RegisterTool(TEXT("package.get_referencers"),  &Tool_GetReferencers,  /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("Package surface registered: 5 package.* tools "
			 "(save + save_all + list_dirty + get_dependencies + get_referencers), all Lane A"));
}

} // namespace FPackageTools

// Wave I refactor 2026-05: auto-registration via FMCPSurfaceRegistry replaces the
// manual include + Register call in UnrealMCPBridge.cpp.
#include "MCPSurfaceRegistry.h"
MCP_REGISTER_SURFACE(PackageTools, &FPackageTools::Register)

#undef LOCTEXT_NAMESPACE
