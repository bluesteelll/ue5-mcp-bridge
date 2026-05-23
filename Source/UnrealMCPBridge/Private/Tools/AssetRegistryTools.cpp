// Copyright FatumGame. All Rights Reserved.

#include "AssetRegistryTools.h"

#include "MCPSurfaceRegistry.h"

#include "FMCPDispatchQueue.h"
#include "MCPJsonBuilder.h"
#include "MCPMutatorScope.h"
#include "MCPToolHelpers.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPARFilterParser.h"
#include "Utils/MCPAssetPathUtils.h"
#include "Utils/MCPPageCursor.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetIdentifier.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "Editor.h"
#include "Engine/DataAsset.h"
#include "Factories/DataAssetFactory.h"
#include "Factories/Factory.h"
#include "IAssetTools.h"
#include "ImageUtils.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "UObject/UObjectIterator.h"
#include "Misc/AssetRegistryInterface.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/ObjectThumbnail.h"
#include "ObjectTools.h"
#include "ThumbnailRendering/ThumbnailManager.h"
#include "ThumbnailRendering/ThumbnailRenderer.h"
#include "UObject/Class.h"
#include "Utils/MCPPathSandbox.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/TopLevelAssetPath.h"

namespace
{
	// AR_ prefix per the unity-build symbol-collision pattern. XX_StampIds / XX_MakeError /
	// XX_MakeSuccessObj removed in Phase 3 — use FMCPToolHelpers::Xxx from MCPToolHelpers.h.
	// kMCPErrorInvalidParams replaced by canonical kMCPErrorInvalidParams.

	/** Resolve ``args.path`` → normalised string; emit INVALID_PATH (-32010) on failure. */
	bool AR_RequirePath(const FMCPRequest& Request, FString& OutNormalized, FMCPResponse& OutError)
	{
		FString Raw;
		if (!FMCPToolHelpers::RequireStringField(Request, TEXT("path"), Raw, OutError)) { return false; }
		const FString Normalized = FMCPAssetPathUtils::Normalize(Raw);
		if (Normalized.IsEmpty())
		{
			OutError = FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidPath,
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

	/** Build a JSON object describing one asset for asset.list / asset.search_by_* responses. */
	TSharedPtr<FJsonObject> AR_BuildAssetSummary(const FAssetData& Data, bool bIncludeTags)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("asset_path"),   Data.GetObjectPathString());
		Obj->SetStringField(TEXT("package_path"), Data.PackagePath.ToString());
		Obj->SetStringField(TEXT("class"),        Data.AssetClassPath.ToString());
		if (bIncludeTags)
		{
			AR_AppendTags(Data, Obj);
		}
		return Obj;
	}

	/**
	 * D11 overly-broad guard: ``package_paths == ["/Game"] && recursive_paths == true && no
	 * class filter && _unsafe_full_scan == false`` is rejected. Callers can either narrow
	 * (add ClassPaths or a deeper PackagePaths) or opt in via the ``_unsafe_full_scan`` arg.
	 */
	bool AR_IsOverlyBroad(const FARFilter& Filter)
	{
		if (Filter.ClassPaths.Num() > 0) { return false; }
		if (!Filter.bRecursivePaths)     { return false; }
		if (Filter.PackagePaths.Num() != 1) { return false; }
		return Filter.PackagePaths[0] == FName(TEXT("/Game"));
	}

	/**
	 * Lexicographically sort an FAssetData array by ObjectPath (the pagination sort key per D2).
	 * Stable sort so equal keys preserve relative order.
	 */
	void AR_SortByObjectPath(TArray<FAssetData>& Data)
	{
		Data.StableSort([](const FAssetData& A, const FAssetData& B)
		{
			return A.GetObjectPathString().Compare(B.GetObjectPathString(), ESearchCase::IgnoreCase) < 0;
		});
	}

	/**
	 * Slice a sorted FAssetData array by ``last_asset_path`` sentinel + ``page_size``. ``OutAssets``
	 * receives the new page; ``OutNextSentinel`` receives the last entry's ObjectPath (empty if
	 * the page is the last one — caller emits next_page_token=null).
	 *
	 * Skip-until-past-sentinel is O(N) per call, which matches the AR query cost anyway. For very
	 * large result sets (N > 100k) consider a binary search — out of scope for Phase 2.
	 */
	void AR_SlicePage(
		const TArray<FAssetData>& SortedAll,
		const FString& LastAssetPath,
		int32 PageSize,
		TArray<FAssetData>& OutPageAssets,
		FString& OutNextSentinel)
	{
		OutPageAssets.Reset();
		OutNextSentinel.Reset();

		// Find first index strictly > LastAssetPath.
		int32 StartIdx = 0;
		if (!LastAssetPath.IsEmpty())
		{
			while (StartIdx < SortedAll.Num())
			{
				const FString Cur = SortedAll[StartIdx].GetObjectPathString();
				if (Cur.Compare(LastAssetPath, ESearchCase::IgnoreCase) > 0)
				{
					break;
				}
				++StartIdx;
			}
		}

		const int32 EndIdxExcl = FMath::Min(SortedAll.Num(), StartIdx + PageSize);
		OutPageAssets.Reserve(EndIdxExcl - StartIdx);
		for (int32 i = StartIdx; i < EndIdxExcl; ++i)
		{
			OutPageAssets.Add(SortedAll[i]);
		}

		// More pages remaining? Only emit sentinel if there's at least one entry past the slice.
		if (EndIdxExcl < SortedAll.Num() && OutPageAssets.Num() > 0)
		{
			OutNextSentinel = OutPageAssets.Last().GetObjectPathString();
		}
	}

	/**
	 * Common return path for asset.list / asset.search_by_* (the four paginated tools). Wraps the
	 * sliced array + cursor into a uniform JSON response envelope.
	 *
	 * ``ItemsFieldName`` is the response array name — "assets" for asset.list, "matches" for the
	 * three search tools.
	 */
	FMCPResponse AR_BuildPaginatedResponse(
		const FMCPRequest& Request,
		const TCHAR* ItemsFieldName,
		const TArray<FAssetData>& PageAssets,
		int32 TotalKnown,
		const FString& NextSentinel,
		uint64 FilterHash,
		bool bIncludeTags,
		bool bIncludeColdCacheFlag)
	{
		TArray<TSharedPtr<FJsonValue>> Items;
		Items.Reserve(PageAssets.Num());
		for (const FAssetData& Data : PageAssets)
		{
			Items.Add(MakeShared<FJsonValueObject>(AR_BuildAssetSummary(Data, bIncludeTags)));
		}
		return FMCPJsonBuilder()
			.Arr(ItemsFieldName, MoveTemp(Items))
			.Int(TEXT("total_known"), TotalKnown)
			.If(NextSentinel.IsEmpty(), [](FMCPJsonBuilder& B) { B.Null(TEXT("next_page_token")); })
			.If(!NextSentinel.IsEmpty(), [&](FMCPJsonBuilder& B)
			{
				FMCPPageCursor Cursor;
				Cursor.FilterHash = FilterHash;
				Cursor.LastAssetPath = NextSentinel;
				Cursor.TotalKnownSnapshot = TotalKnown;
				B.Str(TEXT("next_page_token"), FMCPPageCursorUtils::Encode(Cursor));
			})
			.If(bIncludeColdCacheFlag, [](FMCPJsonBuilder& B)
			{
				B.Bool(TEXT("is_cold_cache"), FAssetRegistryModule::GetRegistry().IsLoadingAssets());
			})
			.BuildSuccess(Request);
	}

	/**
	 * Decode + validate a page_token wire value against ``ExpectedFilterHash``. Populates
	 * ``OutCursor`` on success; populates ``OutError`` and returns false on decode or hash
	 * mismatch (caller surfaces the response).
	 */
	bool AR_DecodeCursor(
		const FMCPRequest& Request,
		const FString& PageTokenWire,
		uint64 ExpectedFilterHash,
		FMCPPageCursor& OutCursor,
		FMCPResponse& OutError)
	{
		FString DecodeErr;
		if (!FMCPPageCursorUtils::Decode(PageTokenWire, OutCursor, DecodeErr))
		{
			OutError = FMCPToolHelpers::MakeError(Request, kMCPErrorStaleCursor,
				FString::Printf(TEXT("page_token decode failed: %s"), *DecodeErr));
			return false;
		}
		if (!FMCPPageCursorUtils::ValidateAgainstFilter(OutCursor, ExpectedFilterHash))
		{
			OutError = FMCPToolHelpers::MakeError(Request, kMCPErrorStaleCursor,
				TEXT("page_token filter_hash mismatch — caller mutated filter between pages; "
					 "restart pagination with page_token=null"));
			return false;
		}
		return true;
	}

	/** Validate page_size; clamps to [1, 1000] per the inline schema. */
	int32 AR_ClampPageSize(const TSharedPtr<FJsonObject>& Args, const TCHAR* FieldName, int32 Default)
	{
		int32 Out = Default;
		if (Args.IsValid())
		{
			Args->TryGetNumberField(FieldName, Out);
		}
		return FMath::Clamp(Out, 1, 1000);
	}

	/**
	 * Build the EDependencyCategory + FDependencyQuery from the include_hard / include_soft /
	 * include_searchable_names boolean trio used by find_references / find_dependents.
	 *
	 * Returns the category mask (Package OR Package|SearchableName); query expresses the hard/soft
	 * split inside the Package category.
	 */
	void AR_BuildDependencyQuery(
		bool bIncludeHard, bool bIncludeSoft, bool bIncludeSearchableNames,
		UE::AssetRegistry::EDependencyCategory& OutCategory,
		UE::AssetRegistry::FDependencyQuery& OutQuery)
	{
		OutCategory = UE::AssetRegistry::EDependencyCategory::Package;
		if (bIncludeSearchableNames)
		{
			OutCategory = OutCategory | UE::AssetRegistry::EDependencyCategory::SearchableName;
		}

		// Default = unfiltered Package query (both hard and soft come back). If only one of the
		// two flags is true, ask the registry to filter for us via Hard/NotHard.
		OutQuery = UE::AssetRegistry::FDependencyQuery();
		if (bIncludeHard && !bIncludeSoft)
		{
			OutQuery = UE::AssetRegistry::FDependencyQuery(UE::AssetRegistry::EDependencyQuery::Hard);
		}
		else if (!bIncludeHard && bIncludeSoft)
		{
			OutQuery = UE::AssetRegistry::FDependencyQuery(UE::AssetRegistry::EDependencyQuery::NotHard);
		}
		// If both true (the typical case) leave default. If both false the caller wants no
		// results — we still issue the query and let the returned set be empty.
	}

	/**
	 * Classify a returned ``FAssetDependency`` as ``"Hard"`` / ``"Soft"`` / ``"SearchableName"``.
	 * Used for the per-entry ``dependency_kind`` field on find_references / find_dependents.
	 */
	const TCHAR* AR_ClassifyDependency(const FAssetDependency& Dep)
	{
		if (Dep.Category == UE::AssetRegistry::EDependencyCategory::SearchableName)
		{
			return TEXT("SearchableName");
		}
		return EnumHasAnyFlags(Dep.Properties, UE::AssetRegistry::EDependencyProperty::Hard)
			? TEXT("Hard") : TEXT("Soft");
	}

	/**
	 * BFS over GetReferencers / GetDependencies starting at ``RootPackage`` (the asset's
	 * package-name). Visited-set capped at 10k entries per D11; exceeding the cap returns false
	 * with OutError populated — caller surfaces as OVERLY_BROAD_QUERY.
	 *
	 * Returns the accumulated dependency list flat (not the BFS tree). Output is unsorted; caller
	 * sorts via the same lex-by-PackageName scheme used elsewhere for cursor stability.
	 *
	 * The ``bReferencers`` flag selects direction: true = walk who-points-to-me;
	 * false = walk who-do-I-point-to.
	 */
	bool AR_WalkDependencyGraph(
		IAssetRegistry& IAR,
		const FName& RootPackage,
		bool bReferencers,
		UE::AssetRegistry::EDependencyCategory Category,
		const UE::AssetRegistry::FDependencyQuery& Query,
		bool bRecursive,
		int32 MaxVisited,
		TArray<FAssetDependency>& OutAll,
		FString& OutError)
	{
		TSet<FName> Visited;
		Visited.Reserve(64);
		TQueue<FName> Frontier;
		Visited.Add(RootPackage);
		Frontier.Enqueue(RootPackage);

		while (!Frontier.IsEmpty())
		{
			FName Cur;
			Frontier.Dequeue(Cur);

			TArray<FAssetDependency> ThisHop;
			const FAssetIdentifier Id(Cur);
			if (bReferencers)
			{
				IAR.GetReferencers(Id, ThisHop, Category, Query);
			}
			else
			{
				IAR.GetDependencies(Id, ThisHop, Category, Query);
			}

			for (const FAssetDependency& Dep : ThisHop)
			{
				const FName Next = Dep.AssetId.PackageName;
				if (Next.IsNone()) { continue; }

				// Per-hop output is the FAssetDependency itself (preserves kind). Append
				// unconditionally so caller sees direct + indirect links separately.
				OutAll.Add(Dep);

				if (bRecursive && !Visited.Contains(Next))
				{
					Visited.Add(Next);
					if (Visited.Num() > MaxVisited)
					{
						OutError = FString::Printf(
							TEXT("graph walk visited %d entries, exceeding the %d cap; "
								 "non-recursive query or narrower root recommended"),
							Visited.Num(), MaxVisited);
						return false;
					}
					Frontier.Enqueue(Next);
				}
			}

			if (!bRecursive)
			{
				// Single-hop request: drain the frontier without enqueuing children. We already
				// processed the root; bail.
				break;
			}
		}
		return true;
	}

	/**
	 * Sort FAssetDependency array lex by PackageName for cursor stability + slice by sentinel.
	 * Mirrors AR_SortByObjectPath + AR_SlicePage but for the dependency-list shape.
	 */
	void AR_SortDependenciesByPackageName(TArray<FAssetDependency>& Deps)
	{
		Deps.StableSort([](const FAssetDependency& A, const FAssetDependency& B)
		{
			return A.AssetId.PackageName.ToString().Compare(
				B.AssetId.PackageName.ToString(), ESearchCase::IgnoreCase) < 0;
		});
	}

	void AR_SliceDependencyPage(
		const TArray<FAssetDependency>& Sorted,
		const FString& LastKey,
		int32 PageSize,
		TArray<FAssetDependency>& OutPage,
		FString& OutNextSentinel)
	{
		OutPage.Reset();
		OutNextSentinel.Reset();

		int32 StartIdx = 0;
		if (!LastKey.IsEmpty())
		{
			while (StartIdx < Sorted.Num())
			{
				const FString Cur = Sorted[StartIdx].AssetId.PackageName.ToString();
				if (Cur.Compare(LastKey, ESearchCase::IgnoreCase) > 0)
				{
					break;
				}
				++StartIdx;
			}
		}
		const int32 EndIdxExcl = FMath::Min(Sorted.Num(), StartIdx + PageSize);
		OutPage.Reserve(EndIdxExcl - StartIdx);
		for (int32 i = StartIdx; i < EndIdxExcl; ++i)
		{
			OutPage.Add(Sorted[i]);
		}
		if (EndIdxExcl < Sorted.Num() && OutPage.Num() > 0)
		{
			OutNextSentinel = OutPage.Last().AssetId.PackageName.ToString();
		}
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

	return FMCPJsonBuilder()
		.Bool(TEXT("exists"), Data.IsValid())
		.Str(TEXT("asset_path_canonical"), NormalizedPath)
		.BuildSuccess(Request);
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
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
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
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
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
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("no AssetRegistry entry for '%s'"), *NormalizedPath));
	}

	// Lane B contract: NO UObject access. FindPackage walks the global package hash and is
	// game-thread-only — concurrent invocation during GC or async load → use-after-free. The
	// `package_flags` field is dropped from the response (was documented as "0 if not loaded",
	// utility for AI workflows was marginal). asset.is_dirty (Lane A) remains for callers that
	// need live UPackage state.
	const FString PackageNameStr = Data.PackageName.ToString();
	const bool bOnDisk = FPackageName::DoesPackageExist(PackageNameStr);

	return FMCPJsonBuilder()
		.Str(TEXT("package_path"), PackageNameStr)
		.Bool(TEXT("on_disk"), bOnDisk)
		.BuildSuccess(Request);
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
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("no AssetRegistry entry for '%s'"), *NormalizedPath));
	}

	const FString PackageNameStr = Data.PackageName.ToString();
	UPackage* P = FindPackage(nullptr, *PackageNameStr);
	// Deliberate: we do NOT autoload — that would mutate in-memory state silently. Contrast
	// with cb.delete force=false which MAY autoload during its reference walk.
	return FMCPJsonBuilder()
		.Bool(TEXT("dirty"), P ? P->IsDirty() : false)
		.Bool(TEXT("in_memory"), P != nullptr)
		.BuildSuccess(Request);
}

// ─── asset.list (paginated, FARFilter) ───────────────────────────────────────────────────────
FMCPResponse Tool_AssetList(const FMCPRequest& Request)
{
	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams, TEXT("missing args object"));
	}

	// Parse FARFilter from args.filter (object). Missing/empty → default FARFilter (matches all).
	const TSharedPtr<FJsonObject>* FilterObjPtr = nullptr;
	Request.Args->TryGetObjectField(TEXT("filter"), FilterObjPtr);
	TSharedPtr<FJsonObject> FilterObj = (FilterObjPtr && FilterObjPtr->IsValid()) ? *FilterObjPtr : TSharedPtr<FJsonObject>();

	FARFilter Filter;
	FString ParseErr;
	if (!FMCPARFilterParser::Parse(FilterObj, Filter, ParseErr))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			FString::Printf(TEXT("filter parse error: %s"), *ParseErr));
	}

	// Overly-broad guard (D11) — opt-out via args._unsafe_full_scan.
	bool bUnsafeFullScan = false;
	Request.Args->TryGetBoolField(TEXT("_unsafe_full_scan"), bUnsafeFullScan);
	if (!bUnsafeFullScan && AR_IsOverlyBroad(Filter))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorOverlyBroadQuery,
			TEXT("filter targets entire /Game recursively with no class narrowing. "
				 "Add class_paths or set _unsafe_full_scan=true"));
	}

	const int32 PageSize = AR_ClampPageSize(Request.Args, TEXT("page_size"), 100);

	// page_token (optional). filter_hash validation = STALE_CURSOR if mismatched.
	const uint64 FilterHash = FMCPARFilterParser::ComputeFilterHash(Filter);
	FString PageTokenWire;
	Request.Args->TryGetStringField(TEXT("page_token"), PageTokenWire);
	FMCPPageCursor Cursor;
	FMCPResponse CursorErr;
	if (!AR_DecodeCursor(Request, PageTokenWire, FilterHash, Cursor, CursorErr))
	{
		return CursorErr;
	}

	// Full re-query + sort + slice. AR query is documented thread-safe.
	IAssetRegistry& IAR = FAssetRegistryModule::GetRegistry();
	TArray<FAssetData> All;
	IAR.GetAssets(Filter, All);
	AR_SortByObjectPath(All);

	TArray<FAssetData> Page;
	FString NextSentinel;
	AR_SlicePage(All, Cursor.LastAssetPath, PageSize, Page, NextSentinel);

	return AR_BuildPaginatedResponse(Request, TEXT("assets"), Page, All.Num(), NextSentinel,
		FilterHash, /*bIncludeTags*/ true, /*bIncludeColdCacheFlag*/ true);
}
// ─── asset.find_references / asset.find_dependents — shared body ─────────────────────────────
namespace
{
	FMCPResponse AR_RunDependencyQuery(const FMCPRequest& Request, bool bReferencers, const TCHAR* ItemsFieldName)
	{
		FString NormalizedPath;
		FMCPResponse Err;
		if (!AR_RequirePath(Request, NormalizedPath, Err)) { return Err; }

		FAssetData RootData;
		if (!FMCPAssetPathUtils::ResolveAssetData(NormalizedPath, RootData))
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
				FString::Printf(TEXT("no AssetRegistry entry for '%s'"), *NormalizedPath));
		}

		bool bRecursive   = false;
		bool bIncludeSoft = true;
		bool bIncludeHard = true;
		bool bIncludeSN   = false;
		Request.Args->TryGetBoolField(TEXT("recursive"), bRecursive);
		Request.Args->TryGetBoolField(TEXT("include_soft"), bIncludeSoft);
		Request.Args->TryGetBoolField(TEXT("include_hard"), bIncludeHard);
		Request.Args->TryGetBoolField(TEXT("include_searchable_names"), bIncludeSN);

		UE::AssetRegistry::EDependencyCategory Category;
		UE::AssetRegistry::FDependencyQuery Query;
		AR_BuildDependencyQuery(bIncludeHard, bIncludeSoft, bIncludeSN, Category, Query);

		IAssetRegistry& IAR = FAssetRegistryModule::GetRegistry();
		TArray<FAssetDependency> All;
		FString WalkErr;
		if (!AR_WalkDependencyGraph(IAR, RootData.PackageName, bReferencers, Category, Query,
			bRecursive, /*MaxVisited*/ 10000, All, WalkErr))
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorOverlyBroadQuery, WalkErr);
		}

		AR_SortDependenciesByPackageName(All);

		// Filter-hash: incorporate root + flags so changing any of them invalidates the cursor.
		FARFilter HashFilter;
		HashFilter.PackageNames.Add(RootData.PackageName);
		HashFilter.TagsAndValues.Add(FName(TEXT("__direction__")),
			TOptional<FString>(bReferencers ? TEXT("ref") : TEXT("dep")));
		HashFilter.TagsAndValues.Add(FName(TEXT("__recursive__")),
			TOptional<FString>(bRecursive ? TEXT("1") : TEXT("0")));
		HashFilter.TagsAndValues.Add(FName(TEXT("__hard__")),
			TOptional<FString>(bIncludeHard ? TEXT("1") : TEXT("0")));
		HashFilter.TagsAndValues.Add(FName(TEXT("__soft__")),
			TOptional<FString>(bIncludeSoft ? TEXT("1") : TEXT("0")));
		HashFilter.TagsAndValues.Add(FName(TEXT("__sn__")),
			TOptional<FString>(bIncludeSN ? TEXT("1") : TEXT("0")));
		const uint64 FilterHash = FMCPARFilterParser::ComputeFilterHash(HashFilter);

		const int32 PageSize = AR_ClampPageSize(Request.Args, TEXT("page_size"), 200);
		FString PageTokenWire;
		Request.Args->TryGetStringField(TEXT("page_token"), PageTokenWire);
		FMCPPageCursor Cursor;
		FMCPResponse CursorErr;
		if (!AR_DecodeCursor(Request, PageTokenWire, FilterHash, Cursor, CursorErr))
		{
			return CursorErr;
		}

		TArray<FAssetDependency> Page;
		FString NextSentinel;
		AR_SliceDependencyPage(All, Cursor.LastAssetPath, PageSize, Page, NextSentinel);

		TArray<TSharedPtr<FJsonValue>> Items;
		Items.Reserve(Page.Num());
		for (const FAssetDependency& Dep : Page)
		{
			TSharedPtr<FJsonObject> ItemObj = MakeShared<FJsonObject>();
			ItemObj->SetStringField(TEXT("package_path"), Dep.AssetId.PackageName.ToString());
			ItemObj->SetStringField(TEXT("dependency_kind"), AR_ClassifyDependency(Dep));
			Items.Add(MakeShared<FJsonValueObject>(ItemObj));
		}
		return FMCPJsonBuilder()
			.Arr(ItemsFieldName, MoveTemp(Items))
			.Int(TEXT("total_known"), All.Num())
			.If(NextSentinel.IsEmpty(), [](FMCPJsonBuilder& B) { B.Null(TEXT("next_page_token")); })
			.If(!NextSentinel.IsEmpty(), [&](FMCPJsonBuilder& B)
			{
				FMCPPageCursor NewCursor;
				NewCursor.FilterHash = FilterHash;
				NewCursor.LastAssetPath = NextSentinel;
				NewCursor.TotalKnownSnapshot = All.Num();
				B.Str(TEXT("next_page_token"), FMCPPageCursorUtils::Encode(NewCursor));
			})
			.BuildSuccess(Request);
	}
}

FMCPResponse Tool_AssetFindReferences(const FMCPRequest& Request)
{
	return AR_RunDependencyQuery(Request, /*bReferencers*/ true, TEXT("referencers"));
}

FMCPResponse Tool_AssetFindDependents(const FMCPRequest& Request)
{
	return AR_RunDependencyQuery(Request, /*bReferencers*/ false, TEXT("dependents"));
}

// ─── asset.dependency_graph (Wave M M2 — BFS over Package deps + referencers) ────────────────
//
// Args:
//   asset_path   : string (required)         — package path or object path of the root asset
//   direction?   : "dependencies" (default) | "referencers" | "both"
//   max_depth?   : int (default 3, clamp [1, 10])
//   type_filter? : string[]                  — class short-names whitelist (e.g. ["Material","Texture2D"])
//                                              applied to nodes at depth >= 1 (root always included)
//   include_engine?: bool (default false)    — when false, skip /Engine/, /Script/, /Game/Developers/
//   max_nodes?   : int (default 500, clamp [10, 5000])
//
// Result:
//   {
//     root: string,
//     direction: "dependencies"|"referencers"|"both",
//     max_depth: int,
//     nodes: [{ path: string, depth: int, class: string, package_size_bytes: int }],
//     edges: [{ from: string, to: string, direction: "dependency"|"referencer" }],
//     total_nodes: int,
//     total_edges: int,
//     truncated_by_depth: bool,
//     truncated_by_max_nodes: bool
//   }
//
// Errors:
//   -32004 ObjectNotFound  root asset_path doesn't resolve
//   -32602 InvalidParams   missing/invalid args
//
// Algorithm: BFS from root. Per critique C4, explicitly pass UE::AssetRegistry::EDependencyCategory::Package.
// Per critique Q3 decision, edges carry an explicit ``direction`` field. ``package_size_bytes`` is
// best-effort on-disk size (-1 / null when package is in-memory-only, per critique N2).
//
// Lane A initially (per critique Q4); reviewer audits Lane B promotion in followup. Auto-register
// already covers this surface — no change to MCP_REGISTER_SURFACE line needed.
FMCPResponse Tool_AssetDependencyGraph(const FMCPRequest& Request)
{
	FString NormalizedPath;
	FMCPResponse Err;
	if (!AR_RequirePath(Request, NormalizedPath, Err)) { return Err; }

	FAssetData RootData;
	if (!FMCPAssetPathUtils::ResolveAssetData(NormalizedPath, RootData))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("no AssetRegistry entry for '%s'"), *NormalizedPath));
	}

	// ── Parse direction. ──────────────────────────────────────────────────────────────────────
	enum class EDir : uint8 { Dependencies, Referencers, Both };
	EDir Direction = EDir::Dependencies;
	if (Request.Args.IsValid())
	{
		FString DirStr;
		if (Request.Args->TryGetStringField(TEXT("direction"), DirStr) && !DirStr.IsEmpty())
		{
			if (DirStr.Equals(TEXT("dependencies"), ESearchCase::IgnoreCase))
			{
				Direction = EDir::Dependencies;
			}
			else if (DirStr.Equals(TEXT("referencers"), ESearchCase::IgnoreCase))
			{
				Direction = EDir::Referencers;
			}
			else if (DirStr.Equals(TEXT("both"), ESearchCase::IgnoreCase))
			{
				Direction = EDir::Both;
			}
			else
			{
				return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
					FString::Printf(TEXT("'direction' must be one of: dependencies, referencers, both (got '%s')"), *DirStr));
			}
		}
	}

	// ── Parse caps. ───────────────────────────────────────────────────────────────────────────
	int32 MaxDepth = 3;
	int32 MaxNodes = 500;
	bool bIncludeEngine = false;
	TSet<FString> TypeFilter;     // empty = no filter
	if (Request.Args.IsValid())
	{
		int32 RawDepth = MaxDepth;
		if (Request.Args->TryGetNumberField(TEXT("max_depth"), RawDepth))
		{
			MaxDepth = FMath::Clamp(RawDepth, 1, 10);
		}
		int32 RawNodes = MaxNodes;
		if (Request.Args->TryGetNumberField(TEXT("max_nodes"), RawNodes))
		{
			MaxNodes = FMath::Clamp(RawNodes, 10, 5000);
		}
		Request.Args->TryGetBoolField(TEXT("include_engine"), bIncludeEngine);

		const TArray<TSharedPtr<FJsonValue>>* TypeArr = nullptr;
		if (Request.Args->TryGetArrayField(TEXT("type_filter"), TypeArr) && TypeArr)
		{
			for (const TSharedPtr<FJsonValue>& V : *TypeArr)
			{
				FString Cls;
				if (V.IsValid() && V->TryGetString(Cls) && !Cls.IsEmpty())
				{
					TypeFilter.Add(Cls);
				}
			}
		}
	}

	IAssetRegistry& IAR = FAssetRegistryModule::GetRegistry();
	const UE::AssetRegistry::EDependencyCategory Category = UE::AssetRegistry::EDependencyCategory::Package;
	const UE::AssetRegistry::FDependencyQuery Query = UE::AssetRegistry::FDependencyQuery(); // unfiltered Package

	// ── BFS. Nodes keyed by PackageName (FName). ──────────────────────────────────────────────
	struct FNodeInfo
	{
		FName PackageName;
		int32 Depth;
		FAssetData Data;          // may be invalid (.IsValid()==false) if AR has no entry — still report the package
		bool bHasData;
	};
	struct FEdgeInfo
	{
		FName From;
		FName To;
		bool bIsDependency;       // true → "dependency" direction; false → "referencer"
	};

	auto ShouldSkipForEngineFilter = [bIncludeEngine](const FString& PackageNameStr) -> bool
	{
		if (bIncludeEngine) { return false; }
		return PackageNameStr.StartsWith(TEXT("/Engine/"))
			|| PackageNameStr.StartsWith(TEXT("/Script/"))
			|| PackageNameStr.StartsWith(TEXT("/Game/Developers/"));
	};

	auto MatchesTypeFilter = [&TypeFilter](const FAssetData& Data) -> bool
	{
		if (TypeFilter.Num() == 0) { return true; }
		if (!Data.IsValid()) { return false; }
		// AssetClassPath::GetAssetName returns FName like "Material" / "Texture2D".
		return TypeFilter.Contains(Data.AssetClassPath.GetAssetName().ToString());
	};

	TMap<FName, FNodeInfo> Nodes;
	TArray<FEdgeInfo> Edges;
	TQueue<FName> Frontier;
	bool bTruncatedByDepth = false;
	bool bTruncatedByMaxNodes = false;

	// Seed with root (always included regardless of include_engine / type_filter — caller asked
	// for this asset specifically).
	const FName RootPackage = RootData.PackageName;
	{
		FNodeInfo RootInfo;
		RootInfo.PackageName = RootPackage;
		RootInfo.Depth = 0;
		RootInfo.Data = RootData;
		RootInfo.bHasData = true;
		Nodes.Add(RootPackage, RootInfo);
		Frontier.Enqueue(RootPackage);
	}

	while (!Frontier.IsEmpty())
	{
		FName Current;
		Frontier.Dequeue(Current);
		const int32 CurDepth = Nodes[Current].Depth;

		if (CurDepth >= MaxDepth)
		{
			bTruncatedByDepth = true;
			continue;
		}

		auto ProcessHop = [&](bool bDirIsDependencies)
		{
			TArray<FAssetDependency> Hop;
			const FAssetIdentifier Id(Current);
			if (bDirIsDependencies)
			{
				IAR.GetDependencies(Id, Hop, Category, Query);
			}
			else
			{
				IAR.GetReferencers(Id, Hop, Category, Query);
			}
			for (const FAssetDependency& Dep : Hop)
			{
				const FName Next = Dep.AssetId.PackageName;
				if (Next.IsNone()) { continue; }

				// Filter check on the NEXT node (depth >= 1 always). Root already added above
				// unconditionally; this code only sees children/parents.
				const FString NextStr = Next.ToString();
				if (ShouldSkipForEngineFilter(NextStr)) { continue; }

				// Look up AR data for the next package (may be invalid for stale-/script-deps).
				FAssetData NextData;
				bool bHasData = false;
				{
					const FString ObjectPath = FString::Printf(TEXT("%s.%s"),
						*NextStr, *FPackageName::GetShortName(NextStr));
					NextData = IAR.GetAssetByObjectPath(FSoftObjectPath(ObjectPath),
						/*bIncludeOnlyOnDiskAssets*/ false);
					bHasData = NextData.IsValid();
				}

				// Type-filter applies to depth >= 1 nodes only (root always present).
				if (!MatchesTypeFilter(NextData)) { continue; }

				// Add the node if not already visited.
				FNodeInfo* Existing = Nodes.Find(Next);
				if (!Existing)
				{
					if (Nodes.Num() >= MaxNodes)
					{
						bTruncatedByMaxNodes = true;
						// Do NOT add the node, do NOT emit the edge — we'd produce an edge with no
						// matching node which makes the graph malformed.
						continue;
					}
					FNodeInfo NewInfo;
					NewInfo.PackageName = Next;
					NewInfo.Depth = CurDepth + 1;
					NewInfo.Data = NextData;
					NewInfo.bHasData = bHasData;
					Nodes.Add(Next, NewInfo);
					Frontier.Enqueue(Next);
				}

				// Emit edge (always — duplicates possible across direction passes in "both" mode,
				// but distinct direction fields prevent collapse).
				FEdgeInfo E;
				if (bDirIsDependencies)
				{
					E.From = Current;
					E.To   = Next;
				}
				else
				{
					E.From = Next;       // referencer points TO current
					E.To   = Current;
				}
				E.bIsDependency = bDirIsDependencies;
				Edges.Add(E);
			}
		};

		if (Direction == EDir::Dependencies || Direction == EDir::Both)
		{
			ProcessHop(/*bDirIsDependencies*/ true);
		}
		if (Direction == EDir::Referencers || Direction == EDir::Both)
		{
			ProcessHop(/*bDirIsDependencies*/ false);
		}
	}

	// ── Build response. Sort nodes by depth then path for stable output. ──────────────────────
	TArray<FNodeInfo> SortedNodes;
	SortedNodes.Reserve(Nodes.Num());
	for (const TPair<FName, FNodeInfo>& Pair : Nodes)
	{
		SortedNodes.Add(Pair.Value);
	}
	SortedNodes.Sort([](const FNodeInfo& A, const FNodeInfo& B)
	{
		if (A.Depth != B.Depth) return A.Depth < B.Depth;
		return A.PackageName.LexicalLess(B.PackageName);
	});

	FMCPJsonArrayBuilder NodesArr;
	for (const FNodeInfo& Info : SortedNodes)
	{
		const FString PkgStr = Info.PackageName.ToString();
		const FString ClassStr = Info.bHasData ? Info.Data.AssetClassPath.ToString() : FString();

		// Best-effort on-disk package size (-1 if not on disk / in-memory only).
		int64 PackageBytes = -1;
		{
			FString Filename;
			if (FPackageName::DoesPackageExist(PkgStr, &Filename))
			{
				const int64 Sz = IFileManager::Get().FileSize(*Filename);
				if (Sz >= 0) { PackageBytes = Sz; }
			}
		}

		TSharedRef<FJsonObject> NodeObj = MakeShared<FJsonObject>();
		NodeObj->SetStringField(TEXT("path"), PkgStr);
		NodeObj->SetNumberField(TEXT("depth"), Info.Depth);
		NodeObj->SetStringField(TEXT("class"), ClassStr);
		if (PackageBytes >= 0)
		{
			NodeObj->SetNumberField(TEXT("package_size_bytes"), static_cast<double>(PackageBytes));
		}
		else
		{
			NodeObj->SetField(TEXT("package_size_bytes"), MakeShared<FJsonValueNull>());
		}
		NodesArr.AddValue(MakeShared<FJsonValueObject>(NodeObj));
	}

	FMCPJsonArrayBuilder EdgesArr;
	for (const FEdgeInfo& E : Edges)
	{
		TSharedRef<FJsonObject> EdgeObj = MakeShared<FJsonObject>();
		EdgeObj->SetStringField(TEXT("from"), E.From.ToString());
		EdgeObj->SetStringField(TEXT("to"), E.To.ToString());
		EdgeObj->SetStringField(TEXT("direction"),
			E.bIsDependency ? TEXT("dependency") : TEXT("referencer"));
		EdgesArr.AddValue(MakeShared<FJsonValueObject>(EdgeObj));
	}

	const TCHAR* DirStr =
		(Direction == EDir::Dependencies) ? TEXT("dependencies") :
		(Direction == EDir::Referencers)  ? TEXT("referencers")  : TEXT("both");

	return FMCPJsonBuilder()
		.Str(TEXT("root"), RootData.PackageName.ToString())
		.Str(TEXT("direction"), DirStr)
		.Int(TEXT("max_depth"), MaxDepth)
		.Arr(TEXT("nodes"), NodesArr.ToValueArray())
		.Arr(TEXT("edges"), EdgesArr.ToValueArray())
		.Int(TEXT("total_nodes"), Nodes.Num())
		.Int(TEXT("total_edges"), Edges.Num())
		.Bool(TEXT("truncated_by_depth"), bTruncatedByDepth)
		.Bool(TEXT("truncated_by_max_nodes"), bTruncatedByMaxNodes)
		.BuildSuccess(Request);
}

// ─── asset.search_by_class ───────────────────────────────────────────────────────────────────
FMCPResponse Tool_AssetSearchByClass(const FMCPRequest& Request)
{
	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams, TEXT("missing args object"));
	}

	FString ClassPath;
	if (!Request.Args->TryGetStringField(TEXT("class_path"), ClassPath) || ClassPath.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			TEXT("missing required string field 'class_path'"));
	}

	// Allow either /Script/Engine.StaticMesh or /Script/Engine/StaticMesh form (FTopLevelAssetPath
	// requires the dot).
	FString ClassPathNormalized = ClassPath;
	if (!ClassPathNormalized.Contains(TEXT(".")))
	{
		int32 LastSlash = INDEX_NONE;
		if (ClassPathNormalized.FindLastChar(TEXT('/'), LastSlash))
		{
			ClassPathNormalized[LastSlash] = TEXT('.');
		}
	}
	FTopLevelAssetPath Top;
	if (!Top.TrySetPath(ClassPathNormalized))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorWrongClass,
			FString::Printf(TEXT("class_path '%s' does not resolve to FTopLevelAssetPath"), *ClassPath));
	}

	// Build the equivalent FARFilter then delegate to the same pagination path as asset.list.
	FARFilter Filter;
	Filter.ClassPaths.Add(Top);

	const TArray<TSharedPtr<FJsonValue>>* PackagePathsPtr = nullptr;
	if (Request.Args->TryGetArrayField(TEXT("package_paths"), PackagePathsPtr))
	{
		for (const TSharedPtr<FJsonValue>& V : *PackagePathsPtr)
		{
			FString S;
			if (!V.IsValid() || !V->TryGetString(S))
			{
				return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
					TEXT("package_paths: expected array of strings"));
			}
			const FString Norm = FMCPAssetPathUtils::Normalize(S);
			if (Norm.IsEmpty())
			{
				return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidPath,
					FString::Printf(TEXT("package_paths entry '%s' is malformed"), *S));
			}
			Filter.PackagePaths.Add(FName(*Norm));
		}
	}
	else
	{
		// Default to /Game per the inline schema.
		Filter.PackagePaths.Add(FName(TEXT("/Game")));
	}

	bool bRecursivePaths = true;  // default per schema
	Request.Args->TryGetBoolField(TEXT("recursive_paths"), bRecursivePaths);
	Filter.bRecursivePaths = bRecursivePaths;

	bool bSearchSubclasses = false;
	Request.Args->TryGetBoolField(TEXT("search_subclasses"), bSearchSubclasses);
	Filter.bRecursiveClasses = bSearchSubclasses;

	// Same overly-broad guard so "search_by_class with bare /Game + recursive + no narrowing"
	// still trips — except in this tool we ALWAYS have a class filter so the guard never fires.
	// Kept here defensively in case the schema changes.
	bool bUnsafeFullScan = false;
	Request.Args->TryGetBoolField(TEXT("_unsafe_full_scan"), bUnsafeFullScan);
	if (!bUnsafeFullScan && AR_IsOverlyBroad(Filter))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorOverlyBroadQuery,
			TEXT("search_by_class scope too broad — narrow package_paths"));
	}

	const int32 PageSize = AR_ClampPageSize(Request.Args, TEXT("page_size"), 100);
	const uint64 FilterHash = FMCPARFilterParser::ComputeFilterHash(Filter);
	FString PageTokenWire;
	Request.Args->TryGetStringField(TEXT("page_token"), PageTokenWire);
	FMCPPageCursor Cursor;
	FMCPResponse CursorErr;
	if (!AR_DecodeCursor(Request, PageTokenWire, FilterHash, Cursor, CursorErr))
	{
		return CursorErr;
	}

	IAssetRegistry& IAR = FAssetRegistryModule::GetRegistry();
	TArray<FAssetData> All;
	IAR.GetAssets(Filter, All);
	AR_SortByObjectPath(All);

	TArray<FAssetData> Page;
	FString NextSentinel;
	AR_SlicePage(All, Cursor.LastAssetPath, PageSize, Page, NextSentinel);

	return AR_BuildPaginatedResponse(Request, TEXT("matches"), Page, All.Num(), NextSentinel,
		FilterHash, /*bIncludeTags*/ false, /*bIncludeColdCacheFlag*/ true);
}

// ─── asset.search_by_tag ─────────────────────────────────────────────────────────────────────
FMCPResponse Tool_AssetSearchByTag(const FMCPRequest& Request)
{
	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams, TEXT("missing args object"));
	}

	FString TagName;
	if (!Request.Args->TryGetStringField(TEXT("tag_name"), TagName) || TagName.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			TEXT("missing required string field 'tag_name'"));
	}

	// tag_value: nullable string. Empty / missing / explicit null = any-value match.
	FString TagValue;
	const bool bHasValue = Request.Args->TryGetStringField(TEXT("tag_value"), TagValue) && !TagValue.IsEmpty();

	// Optional package_paths post-filter (defaults to ["/Game"]).
	TArray<FString> ScopePaths;
	const TArray<TSharedPtr<FJsonValue>>* ScopePtr = nullptr;
	if (Request.Args->TryGetArrayField(TEXT("package_paths"), ScopePtr))
	{
		for (const TSharedPtr<FJsonValue>& V : *ScopePtr)
		{
			FString S;
			if (!V.IsValid() || !V->TryGetString(S))
			{
				return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
					TEXT("package_paths: expected array of strings"));
			}
			const FString Norm = FMCPAssetPathUtils::Normalize(S);
			if (Norm.IsEmpty())
			{
				return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidPath,
					FString::Printf(TEXT("package_paths entry '%s' is malformed"), *S));
			}
			ScopePaths.Add(Norm);
		}
	}
	else
	{
		ScopePaths.Add(TEXT("/Game"));
	}

	const int32 PageSize = AR_ClampPageSize(Request.Args, TEXT("page_size"), 100);

	IAssetRegistry& IAR = FAssetRegistryModule::GetRegistry();
	TArray<FAssetData> Raw;
	if (bHasValue)
	{
		TMultiMap<FName, FString> Map;
		Map.Add(FName(*TagName), TagValue);
		IAR.GetAssetsByTagValues(Map, Raw);
	}
	else
	{
		TArray<FName> Tags;
		Tags.Add(FName(*TagName));
		IAR.GetAssetsByTags(Tags, Raw);
	}

	// Post-filter by package_paths prefix (case-insensitive). GetAssetsByTags doesn't accept a path
	// scope; we do it in user space.
	TArray<FAssetData> Filtered;
	Filtered.Reserve(Raw.Num());
	for (const FAssetData& Data : Raw)
	{
		const FString PkgPath = Data.PackagePath.ToString();
		for (const FString& Scope : ScopePaths)
		{
			if (PkgPath.StartsWith(Scope, ESearchCase::IgnoreCase))
			{
				Filtered.Add(Data);
				break;
			}
		}
	}
	AR_SortByObjectPath(Filtered);

	// Build a stable filter-hash for pagination — synthesise an FARFilter with the same
	// human-meaningful fields so the canonical-form hash distinguishes different invocations.
	FARFilter HashFilter;
	for (const FString& Scope : ScopePaths) { HashFilter.PackagePaths.Add(FName(*Scope)); }
	TOptional<FString> ValOpt;
	if (bHasValue) { ValOpt = TagValue; }
	HashFilter.TagsAndValues.Add(FName(*TagName), ValOpt);
	const uint64 FilterHash = FMCPARFilterParser::ComputeFilterHash(HashFilter);

	FString PageTokenWire;
	Request.Args->TryGetStringField(TEXT("page_token"), PageTokenWire);
	FMCPPageCursor Cursor;
	FMCPResponse CursorErr;
	if (!AR_DecodeCursor(Request, PageTokenWire, FilterHash, Cursor, CursorErr))
	{
		return CursorErr;
	}

	TArray<FAssetData> Page;
	FString NextSentinel;
	AR_SlicePage(Filtered, Cursor.LastAssetPath, PageSize, Page, NextSentinel);

	// Custom response shape — includes per-item ``tag_value`` field, so we can't reuse the
	// generic paginated builder verbatim.
	TArray<TSharedPtr<FJsonValue>> Items;
	Items.Reserve(Page.Num());
	for (const FAssetData& Data : Page)
	{
		TSharedPtr<FJsonObject> ItemObj = AR_BuildAssetSummary(Data, /*bIncludeTags*/ false);
		const FAssetTagValueRef ValRef = Data.TagsAndValues.FindTag(FName(*TagName));
		ItemObj->SetStringField(TEXT("tag_value"), ValRef.IsSet() ? ValRef.AsString() : FString());
		Items.Add(MakeShared<FJsonValueObject>(ItemObj));
	}
	return FMCPJsonBuilder()
		.Arr(TEXT("matches"), MoveTemp(Items))
		.Int(TEXT("total_known"), Filtered.Num())
		.If(NextSentinel.IsEmpty(), [](FMCPJsonBuilder& B) { B.Null(TEXT("next_page_token")); })
		.If(!NextSentinel.IsEmpty(), [&](FMCPJsonBuilder& B)
		{
			FMCPPageCursor NewCursor;
			NewCursor.FilterHash = FilterHash;
			NewCursor.LastAssetPath = NextSentinel;
			NewCursor.TotalKnownSnapshot = Filtered.Num();
			B.Str(TEXT("next_page_token"), FMCPPageCursorUtils::Encode(NewCursor));
		})
		.BuildSuccess(Request);
}

// ─── asset.search_by_name ────────────────────────────────────────────────────────────────────
FMCPResponse Tool_AssetSearchByName(const FMCPRequest& Request)
{
	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams, TEXT("missing args object"));
	}

	FString Pattern;
	if (!Request.Args->TryGetStringField(TEXT("name_pattern"), Pattern) || Pattern.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			TEXT("missing required string field 'name_pattern'"));
	}
	bool bCaseSensitive = false;
	Request.Args->TryGetBoolField(TEXT("case_sensitive"), bCaseSensitive);

	// package_paths scope (default ["/Game"], recursive). We let the AR filter handle the
	// folder scope so the candidate-set is bounded.
	FARFilter Filter;
	Filter.bRecursivePaths = true;
	const TArray<TSharedPtr<FJsonValue>>* ScopePtr = nullptr;
	if (Request.Args->TryGetArrayField(TEXT("package_paths"), ScopePtr))
	{
		for (const TSharedPtr<FJsonValue>& V : *ScopePtr)
		{
			FString S;
			if (!V.IsValid() || !V->TryGetString(S))
			{
				return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
					TEXT("package_paths: expected array of strings"));
			}
			const FString Norm = FMCPAssetPathUtils::Normalize(S);
			if (Norm.IsEmpty())
			{
				return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidPath,
					FString::Printf(TEXT("package_paths entry '%s' is malformed"), *S));
			}
			Filter.PackagePaths.Add(FName(*Norm));
		}
	}
	else
	{
		Filter.PackagePaths.Add(FName(TEXT("/Game")));
	}

	const int32 PageSize = AR_ClampPageSize(Request.Args, TEXT("page_size"), 100);

	IAssetRegistry& IAR = FAssetRegistryModule::GetRegistry();
	TArray<FAssetData> Raw;
	IAR.GetAssets(Filter, Raw);

	const ESearchCase::Type SearchMode = bCaseSensitive ? ESearchCase::CaseSensitive : ESearchCase::IgnoreCase;
	TArray<FAssetData> Matches;
	Matches.Reserve(Raw.Num());
	for (const FAssetData& Data : Raw)
	{
		if (Data.AssetName.ToString().Contains(Pattern, SearchMode))
		{
			Matches.Add(Data);
		}
	}
	AR_SortByObjectPath(Matches);

	// Filter-hash mixes Pattern + case flag + scope. We encode Pattern into a synthesised tag
	// entry so the canonical-form hash distinguishes "search Cat case-sensitive" from "search
	// Cat case-insensitive".
	FARFilter HashFilter = Filter;
	HashFilter.TagsAndValues.Add(FName(TEXT("__name_pattern__")), TOptional<FString>(Pattern));
	HashFilter.TagsAndValues.Add(FName(TEXT("__case_sensitive__")),
		TOptional<FString>(bCaseSensitive ? TEXT("1") : TEXT("0")));
	const uint64 FilterHash = FMCPARFilterParser::ComputeFilterHash(HashFilter);

	FString PageTokenWire;
	Request.Args->TryGetStringField(TEXT("page_token"), PageTokenWire);
	FMCPPageCursor Cursor;
	FMCPResponse CursorErr;
	if (!AR_DecodeCursor(Request, PageTokenWire, FilterHash, Cursor, CursorErr))
	{
		return CursorErr;
	}

	TArray<FAssetData> Page;
	FString NextSentinel;
	AR_SlicePage(Matches, Cursor.LastAssetPath, PageSize, Page, NextSentinel);

	return AR_BuildPaginatedResponse(Request, TEXT("matches"), Page, Matches.Num(), NextSentinel,
		FilterHash, /*bIncludeTags*/ false, /*bIncludeColdCacheFlag*/ false);
}
// ─── Thumbnail rendering helpers (shared by both tools) ──────────────────────────────────────
namespace
{
	/**
	 * Render the thumbnail for ``Path`` at the requested size; return the raw RGBA8 image plus
	 * dimensions plus an "is_class_generic" hint. On total failure (no asset, no class-generic
	 * fallback) returns false with OutError populated.
	 *
	 * MUST RUN ON GAME THREAD — ThumbnailTools::RenderThumbnail enqueues to the render thread.
	 *
	 * @param OutImageRGBA8     Raw RGBA8 byte array (Width*Height*4 bytes).
	 * @param OutWidth/OutHeight Image dimensions (typically == requested Size, may be smaller).
	 * @param bOutIsClassGeneric True if the result is the class-default icon rather than an
	 *                           asset-specific render.
	 */
	bool AR_RenderAssetThumbnail(
		const FString& NormalizedPath, int32 Size,
		TArray<uint8>& OutImageRGBA8, int32& OutWidth, int32& OutHeight,
		bool& bOutIsClassGeneric, FString& OutError)
	{
		OutError.Reset();
		OutImageRGBA8.Reset();
		OutWidth = 0;
		OutHeight = 0;
		bOutIsClassGeneric = false;

		// Resolve to UObject*. Thumbnail rendering needs the actual instance. This DOES load the
		// asset — by design (a thumbnail of an unloaded asset would have no content to render).
		const FString ObjectPath = FMCPAssetPathUtils::ToObjectPath(NormalizedPath);
		UObject* Asset = nullptr;
		// First try FindObject for already-loaded assets (cheap). If miss, fall back to LoadObject.
		Asset = FindObject<UObject>(nullptr, *ObjectPath);
		if (Asset == nullptr)
		{
			Asset = LoadObject<UObject>(nullptr, *ObjectPath);
		}
		if (Asset == nullptr)
		{
			OutError = FString::Printf(TEXT("could not load object '%s'"), *ObjectPath);
			return false;
		}

		FObjectThumbnail RenderedThumbnail;
		ThumbnailTools::RenderThumbnail(Asset, Size, Size,
			ThumbnailTools::EThumbnailTextureFlushMode::AlwaysFlush,
			/*InRenderTargetResource*/ nullptr,
			&RenderedThumbnail);

		if (RenderedThumbnail.IsEmpty() || !RenderedThumbnail.HasValidImageData())
		{
			// No specific renderer + no class-generic fallback obtainable.
			OutError = FString::Printf(TEXT("RenderThumbnail returned empty result for '%s'"), *ObjectPath);
			return false;
		}

		// Class-generic detection via the thumbnail manager's renderer registry. When a class has
		// no specific renderer registered, UThumbnailManager::GetRenderingInfo returns nullptr
		// (the implementation maps the "&NotSupported" sentinel to nullptr at the end). A non-null
		// Renderer means the class has a dedicated thumbnail renderer (e.g. StaticMesh, Texture2D);
		// nullptr means RenderThumbnail produced the class-generic icon fallback instead.
		FThumbnailRenderingInfo* RenderInfo = UThumbnailManager::Get().GetRenderingInfo(Asset);
		bOutIsClassGeneric = (RenderInfo == nullptr || RenderInfo->Renderer == nullptr);

		OutImageRGBA8 = RenderedThumbnail.GetUncompressedImageData();
		OutWidth = RenderedThumbnail.GetImageWidth();
		OutHeight = RenderedThumbnail.GetImageHeight();
		return true;
	}

	/**
	 * Convert raw RGBA8 bytes to a PNG byte array via FImageUtils::PNGCompressImageArray.
	 * Input MUST be Width*Height*4 bytes (BGRA8 actually — UE stores thumbnails in B,G,R,A order
	 * but the helper treats them as FColor which is uint32 in the same order).
	 */
	bool AR_EncodePNG(const TArray<uint8>& RGBA8, int32 Width, int32 Height, TArray64<uint8>& OutPNGBytes)
	{
		if (RGBA8.Num() != Width * Height * 4) { return false; }
		const TArrayView64<const FColor> ColorView(
			reinterpret_cast<const FColor*>(RGBA8.GetData()),
			static_cast<int64>(Width) * static_cast<int64>(Height));
		FImageUtils::PNGCompressImageArray(Width, Height, ColorView, OutPNGBytes);
		return OutPNGBytes.Num() > 0;
	}
}

// ─── asset.get_thumbnail (in-memory base64, capped at 256×256) ──────────────────────────────
FMCPResponse Tool_AssetGetThumbnail(const FMCPRequest& Request)
{
	FString NormalizedPath;
	FMCPResponse Err;
	if (!AR_RequirePath(Request, NormalizedPath, Err)) { return Err; }

	FAssetData Data;
	if (!FMCPAssetPathUtils::ResolveAssetData(NormalizedPath, Data))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("no AssetRegistry entry for '%s'"), *NormalizedPath));
	}

	int32 Size = 128;
	Request.Args->TryGetNumberField(TEXT("size"), Size);
	if (Size < 16 || Size > 256)
	{
		// Cap is 256 per D6 — larger requests get the disk-tier tool.
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			FString::Printf(TEXT("size %d outside [16, 256] — use asset.get_thumbnail_to_disk "
				"for larger sizes (max 2048)"), Size));
	}

	TArray<uint8> RGBA8;
	int32 Width = 0, Height = 0;
	bool bIsClassGeneric = false;
	FString RenderErr;
	if (!AR_RenderAssetThumbnail(NormalizedPath, Size, RGBA8, Width, Height, bIsClassGeneric, RenderErr))
	{
		// Hard failure per the spec — no asset-specific bitmap AND no class-generic fallback.
		return FMCPToolHelpers::MakeError(Request, kMCPErrorThumbnailRenderFailed,
			FString::Printf(TEXT("thumbnail render failed: %s"), *RenderErr));
	}

	TArray64<uint8> PNGBytes;
	if (!AR_EncodePNG(RGBA8, Width, Height, PNGBytes))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorThumbnailRenderFailed,
			TEXT("PNG encode failed (unexpected RGBA8 size mismatch)"));
	}

	const FString Base64 = FBase64::Encode(reinterpret_cast<const uint8*>(PNGBytes.GetData()), PNGBytes.Num());

	return FMCPJsonBuilder()
		.Str(TEXT("base64"), Base64)
		.Str(TEXT("mime"), TEXT("image/png"))
		.Int(TEXT("width"),  Width)
		.Int(TEXT("height"), Height)
		.Bool(TEXT("is_class_generic"), bIsClassGeneric)
		.BuildSuccess(Request);
}

// ─── asset.get_thumbnail_to_disk (file output, sandboxed, max 2048) ─────────────────────────
FMCPResponse Tool_AssetGetThumbnailToDisk(const FMCPRequest& Request)
{
	FString NormalizedPath;
	FMCPResponse Err;
	if (!AR_RequirePath(Request, NormalizedPath, Err)) { return Err; }

	FAssetData Data;
	if (!FMCPAssetPathUtils::ResolveAssetData(NormalizedPath, Data))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("no AssetRegistry entry for '%s'"), *NormalizedPath));
	}

	int32 Size = 256;
	Request.Args->TryGetNumberField(TEXT("size"), Size);
	if (Size < 16 || Size > 2048)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			FString::Printf(TEXT("size %d outside [16, 2048]"), Size));
	}

	// Format: png (default) or jpg.
	FString Format = TEXT("png");
	Request.Args->TryGetStringField(TEXT("format"), Format);
	const bool bJpg = Format.Equals(TEXT("jpg"), ESearchCase::IgnoreCase) ||
		Format.Equals(TEXT("jpeg"), ESearchCase::IgnoreCase);

	// Resolve output_path: default = <Saved>/UnrealMCP/thumbnails/<uuid>.<ext>.
	// FPaths::ProjectSavedDir() returns a UE-style RELATIVE path (e.g. ../../../FatumGame/Saved/)
	// — must canonicalise to absolute BEFORE handing to FMCPPathSandbox::Resolve, which (correctly)
	// refuses any `..` segments as a defense-in-depth measure to prevent silent sandbox escape.
	FString OutputPathRaw;
	Request.Args->TryGetStringField(TEXT("output_path"), OutputPathRaw);
	if (OutputPathRaw.IsEmpty())
	{
		const FString Ext = bJpg ? TEXT("jpg") : TEXT("png");
		const FGuid Guid = FGuid::NewGuid();
		const FString SavedAbs = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir());
		OutputPathRaw = FPaths::Combine(SavedAbs, TEXT("UnrealMCP"),
			TEXT("thumbnails"), FString::Printf(TEXT("%s.%s"), *Guid.ToString(EGuidFormats::Digits), *Ext));
	}

	// Sandbox resolve (PATH_ESCAPE on whitelist miss).
	FString AbsPath;
	FString SandboxErr;
	if (!FMCPPathSandbox::Resolve(OutputPathRaw, AbsPath, SandboxErr))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPathEscape, SandboxErr);
	}

	TArray<uint8> RGBA8;
	int32 Width = 0, Height = 0;
	bool bIsClassGeneric = false;
	FString RenderErr;
	if (!AR_RenderAssetThumbnail(NormalizedPath, Size, RGBA8, Width, Height, bIsClassGeneric, RenderErr))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorThumbnailRenderFailed,
			FString::Printf(TEXT("thumbnail render failed: %s"), *RenderErr));
	}

	TArray64<uint8> EncodedBytes;
	if (bJpg)
	{
		// JPG via FImageUtils::CompressImage + FImageView (zero-copy wrapper around RGBA8 bytes).
		// Quality 85 = good visual / size tradeoff for thumbnails.
		const FImageView Src(
			reinterpret_cast<const FColor*>(RGBA8.GetData()), Width, Height, EGammaSpace::sRGB);
		if (!FImageUtils::CompressImage(EncodedBytes, TEXT("jpg"), Src, /*Quality*/ 85))
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorThumbnailRenderFailed, TEXT("JPG encode failed"));
		}
	}
	else
	{
		if (!AR_EncodePNG(RGBA8, Width, Height, EncodedBytes))
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorThumbnailRenderFailed,
				TEXT("PNG encode failed (unexpected RGBA8 size mismatch)"));
		}
	}

	// Ensure parent directory exists; SaveArrayToFile will silently fail otherwise.
	const FString ParentDir = FPaths::GetPath(AbsPath);
	IFileManager::Get().MakeDirectory(*ParentDir, /*Tree*/ true);

	// SaveArrayToFile takes TArray<uint8> not TArray64; convert if it fits in int32 (PNG of
	// 2048x2048 is ~10 MiB — comfortably under 2 GiB).
	if (EncodedBytes.Num() > INT32_MAX)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorThumbnailRenderFailed,
			TEXT("encoded thumbnail exceeds 2 GiB — refusing to write"));
	}
	TArray<uint8> WriteBuf;
	WriteBuf.Append(EncodedBytes.GetData(), static_cast<int32>(EncodedBytes.Num()));
	if (!FFileHelper::SaveArrayToFile(WriteBuf, *AbsPath))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorThumbnailRenderFailed,
			FString::Printf(TEXT("could not write thumbnail to '%s'"), *AbsPath));
	}

	return FMCPJsonBuilder()
		.Str(TEXT("path"), AbsPath)
		.Int(TEXT("bytes"), WriteBuf.Num())
		.Int(TEXT("width"),  Width)
		.Int(TEXT("height"), Height)
		.BuildSuccess(Request);
}
// ─── asset.get_class_hierarchy ───────────────────────────────────────────────────────────────
FMCPResponse Tool_AssetGetClassHierarchy(const FMCPRequest& Request)
{
	FString NormalizedPath;
	FMCPResponse Err;
	if (!AR_RequirePath(Request, NormalizedPath, Err)) { return Err; }

	FAssetData Data;
	if (!FMCPAssetPathUtils::ResolveAssetData(NormalizedPath, Data))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("no AssetRegistry entry for '%s'"), *NormalizedPath));
	}

	// Lane B contract: NO UObject access. Walk ancestors via the AR's documented thread-safe
	// GetAncestorClassNames path — pure string-keyed lookup against the AR's class hierarchy
	// cache. Native/Blueprint detection by package-name heuristic:
	//   /Script/<Module>.<Class>  → native (the canonical native class path form)
	//   /Game/... or /<MountedContent>/...  → Blueprint-generated class
	auto MakeChainEntry = [](const FTopLevelAssetPath& ClassPath) -> TSharedPtr<FJsonValue>
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("class_path"), ClassPath.ToString());
		Entry->SetStringField(TEXT("class_name"), ClassPath.GetAssetName().ToString());
		const bool bNative = ClassPath.GetPackageName().ToString().StartsWith(TEXT("/Script/"));
		Entry->SetBoolField(TEXT("is_native"), bNative);
		Entry->SetBoolField(TEXT("is_blueprint"), !bNative);
		return MakeShared<FJsonValueObject>(Entry);
	};

	TArray<TSharedPtr<FJsonValue>> Chain;
	if (Data.AssetClassPath.IsNull())
	{
		// AR entry exists but its class path is unknown — cannot build a hierarchy at all.
		return FMCPJsonBuilder()
			.Arr(TEXT("chain"), MoveTemp(Chain))
			.BuildSuccess(Request);
	}

	// Leaf entry (the asset's own class) is always included — even if GetAncestorClassNames
	// returns false for unloaded class trees, the caller still gets the leaf identity.
	Chain.Add(MakeChainEntry(Data.AssetClassPath));

	IAssetRegistry& IAR = FAssetRegistryModule::GetRegistry();
	TArray<FTopLevelAssetPath> AncestorPaths;
	IAR.GetAncestorClassNames(Data.AssetClassPath, AncestorPaths);
	for (const FTopLevelAssetPath& AncestorPath : AncestorPaths)
	{
		Chain.Add(MakeChainEntry(AncestorPath));
	}

	return FMCPJsonBuilder()
		.Arr(TEXT("chain"), MoveTemp(Chain))
		.BuildSuccess(Request);
}

// ─── asset.create_data_asset — create a new UDataAsset of given class at given path ─────────
//
// Args:
//   - class_path: string (required)   /Script/<Module>.<Class>  OR  /Game/.../BP.BP_C
//   - dest_path:  string (required)   /Game/.../DA_NewAsset  (extension omitted — added by factory)
//   - save:       bool   (optional)   default false. true → calls UEditorAssetSubsystem::SaveLoadedAsset
//                                       on the created asset before returning.
//
// Result:
//   - created:     bool
//   - asset_path:  string  (e.g. "/Game/MCPTest/DA_NewItem.DA_NewItem")
//   - class_path:  string  (echo of resolved class)
//   - saved:       bool    (true if save=true and SaveLoadedAsset succeeded)
//
// Error codes (subset; reused from Phase 2/3):
//   -32010 InvalidPath        dest_path malformed (no /Game/... mount, has '\' or '..')
//   -32014 PathInUse          dest_path already exists on disk
//   -32020 ClassNotFound      class_path didn't resolve via LoadObject (with _C autoload fallback)
//   -32021 ClassAbstract      class is CLASS_Abstract — cannot be instantiated
//   -32022 WrongClassFamily   class resolves but isn't UDataAsset subclass
//   -32023 InvalidClassPath   class_path syntactically malformed
//   -32027 PIEActive          GEditor->PlayWorld != nullptr — refused
//   -32603 InternalError      IAssetTools::CreateAsset returned null (factory rejected / package conflict)
//
// Lane A (game thread only — UDataAssetFactory + IAssetTools require GT). PIE-guarded.
FMCPResponse Tool_AssetCreateDataAsset(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (GEditor && GEditor->PlayWorld != nullptr)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPIEActive, kMCPMessagePIEActive);
	}

	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request,-32602,
			TEXT("asset.create_data_asset requires args.class_path + args.dest_path"));
	}

	// ─── Parse + validate class_path ───────────────────────────────────────────────────────────
	FString ClassPath;
	if (!Request.Args->TryGetStringField(TEXT("class_path"), ClassPath) || ClassPath.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request,-32602, TEXT("missing required string field 'class_path'"));
	}
	if (!ClassPath.StartsWith(TEXT("/")) || ClassPath.Contains(TEXT("\\")))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidClassPath,
			FString::Printf(TEXT("class_path '%s' must start with '/' and use forward slashes only "
				"(form: /Script/<Module>.<Class>  OR  /Game/.../BP.BP_C)"), *ClassPath));
	}

	UClass* TargetClass = LoadClass<UObject>(nullptr, *ClassPath);
	if (!TargetClass)
	{
		// Try BP autoload — Blueprint generated classes need _C suffix
		FString WithC = ClassPath.EndsWith(TEXT("_C")) ? ClassPath : (ClassPath + TEXT("_C"));
		TargetClass = LoadClass<UObject>(nullptr, *WithC);
	}
	if (!TargetClass)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorClassNotFound,
			FString::Printf(TEXT("could not LoadClass '%s' (also tried _C suffix)"), *ClassPath));
	}
	if (TargetClass->HasAnyClassFlags(CLASS_Abstract))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorClassAbstract,
			FString::Printf(TEXT("class '%s' is abstract — cannot instantiate"),
				*TargetClass->GetPathName()));
	}
	if (!TargetClass->IsChildOf(UDataAsset::StaticClass()))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorWrongClassFamily,
			FString::Printf(TEXT("class '%s' is not a UDataAsset subclass (parent chain: %s ...)"),
				*TargetClass->GetPathName(),
				TargetClass->GetSuperClass() ? *TargetClass->GetSuperClass()->GetPathName() : TEXT("?")));
	}

	// ─── Parse + validate dest_path ────────────────────────────────────────────────────────────
	FString DestPathRaw;
	if (!Request.Args->TryGetStringField(TEXT("dest_path"), DestPathRaw) || DestPathRaw.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request,-32602, TEXT("missing required string field 'dest_path'"));
	}
	const FString DestPathNorm = FMCPAssetPathUtils::Normalize(DestPathRaw);
	if (DestPathNorm.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidPath,
			FString::Printf(TEXT("dest_path '%s' is not a valid mount-prefixed asset path "
				"(expected /Game/... or /<Plugin>/...)"), *DestPathRaw));
	}
	if (FPackageName::DoesPackageExist(DestPathNorm))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPathInUse,
			FString::Printf(TEXT("dest_path '%s' already exists on disk — use cb.delete first or pick a different name"),
				*DestPathNorm));
	}

	// ─── Create via UDataAssetFactory + IAssetTools::CreateAsset ───────────────────────────────
	const FString PackagePath = FPaths::GetPath(DestPathNorm);
	const FString AssetName   = FPaths::GetBaseFilename(DestPathNorm);

	UDataAssetFactory* Factory = NewObject<UDataAssetFactory>();
	Factory->DataAssetClass = TargetClass;

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	UObject* NewAsset = AssetTools.CreateAsset(AssetName, PackagePath, TargetClass, Factory);
	if (!NewAsset)
	{
		return FMCPToolHelpers::MakeError(Request,-32603,
			FString::Printf(TEXT("IAssetTools::CreateAsset returned null for class=%s name=%s path=%s "
				"(factory may have rejected; check editor log)"),
				*TargetClass->GetPathName(), *AssetName, *PackagePath));
	}

	// Mark dirty so cb.save / cb.save_all_dirty picks it up.
	if (UPackage* Pkg = NewAsset->GetOutermost()) { Pkg->MarkPackageDirty(); }

	bool bSaveRequested = false;
	bool bSavedOk       = false;
	Request.Args->TryGetBoolField(TEXT("save"), bSaveRequested);
	if (bSaveRequested)
	{
		if (UEditorAssetSubsystem* EAS = GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr)
		{
			bSavedOk = EAS->SaveLoadedAsset(NewAsset, /*bOnlyIfIsDirty*/ true);
		}
	}

	return FMCPJsonBuilder()
		.Bool(TEXT("created"), true)
		.Str(TEXT("asset_path"), NewAsset->GetPathName())
		.Str(TEXT("class_path"), TargetClass->GetPathName())
		.Bool(TEXT("saved"), bSavedOk)
		.BuildSuccess(Request);
}

// ─── asset.create — generic creator for ANY UObject subclass (no source file) ───────────────
//
// Wider than asset.create_data_asset: covers UNiagaraSystem, UAnimSequence, UAnimMontage,
// UMaterial (empty graph), UStaticMesh (empty), UDataTable, UCurveTable, USoundCue, etc.
//
// Discovery strategy:
//   1. Try to find a UFactoryNew subclass with SupportedClass == TargetClass. If found, use
//      IAssetTools::CreateAsset(name, path, class, factory) — same path as material.create_instance.
//   2. Fallback: direct NewObject<TargetClass>(Package) + AssetRegistry::AssetCreated +
//      MarkPackageDirty. Works for most simple classes (data tables need a row struct → fails
//      this path; caller should use specialized tooling for those).
//
// Args:
//   - class_path:   string (required)  /Script/Module.Class  OR  /Game/.../BP.BP_C
//   - dest_path:    string (required)  /Game/.../NewAsset
//   - save:         bool   (optional)  default false
//   - allow_fallback_newobject: bool (optional) default true — allow NewObject fallback if no factory found
//
// Result:
//   - created:     bool
//   - asset_path:  string
//   - class_path:  string  echo
//   - used_factory:string  factory class path used (empty if NewObject fallback)
//   - saved:       bool
//
// Lane A. PIE-guarded.
FMCPResponse Tool_AssetCreate(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (GEditor && GEditor->PlayWorld != nullptr)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPIEActive, kMCPMessagePIEActive);
	}

	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request,-32602,
			TEXT("asset.create requires args.class_path + args.dest_path"));
	}

	FString ClassPath;
	FString DestPathRaw;
	if (!Request.Args->TryGetStringField(TEXT("class_path"), ClassPath) || ClassPath.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request,-32602, TEXT("missing required string field 'class_path'"));
	}
	if (!Request.Args->TryGetStringField(TEXT("dest_path"), DestPathRaw) || DestPathRaw.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request,-32602, TEXT("missing required string field 'dest_path'"));
	}
	if (!ClassPath.StartsWith(TEXT("/")) || ClassPath.Contains(TEXT("\\")))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidClassPath,
			FString::Printf(TEXT("class_path '%s' must start with '/' (form: /Script/<Module>.<Class>  OR  /Game/.../BP.BP_C)"),
				*ClassPath));
	}

	// Resolve class (with _C autoload fallback).
	UClass* TargetClass = LoadClass<UObject>(nullptr, *ClassPath);
	if (!TargetClass)
	{
		const FString WithC = ClassPath.EndsWith(TEXT("_C")) ? ClassPath : (ClassPath + TEXT("_C"));
		TargetClass = LoadClass<UObject>(nullptr, *WithC);
	}
	if (!TargetClass)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorClassNotFound,
			FString::Printf(TEXT("could not LoadClass '%s' (also tried _C suffix)"), *ClassPath));
	}
	if (TargetClass->HasAnyClassFlags(CLASS_Abstract))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorClassAbstract,
			FString::Printf(TEXT("class '%s' is abstract — cannot instantiate"), *TargetClass->GetPathName()));
	}

	// Validate dest path.
	const FString DestPathNorm = FMCPAssetPathUtils::Normalize(DestPathRaw);
	if (DestPathNorm.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidPath,
			FString::Printf(TEXT("dest_path '%s' is not a valid mount-prefixed asset path"), *DestPathRaw));
	}
	if (FPackageName::DoesPackageExist(DestPathNorm))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPathInUse,
			FString::Printf(TEXT("dest_path '%s' already exists on disk"), *DestPathNorm));
	}

	const FString PackagePath = FPaths::GetPath(DestPathNorm);
	const FString AssetName   = FPaths::GetBaseFilename(DestPathNorm);

	// ─── Strategy 1: find UFactoryNew with SupportedClass == TargetClass ───────────────────────
	UFactory* SelectedFactory = nullptr;
	UClass*   SelectedFactoryClass = nullptr;
	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* FC = *It;
		if (!FC || !FC->IsChildOf(UFactory::StaticClass())) { continue; }
		if (FC->HasAnyClassFlags(CLASS_Abstract | CLASS_NewerVersionExists)) { continue; }
		UFactory* CDO = FC->GetDefaultObject<UFactory>();
		if (!CDO) { continue; }
		// Prefer factories that explicitly support "new asset" creation (not import factories).
		if (!CDO->ShouldShowInNewMenu()) { continue; }
		if (CDO->GetSupportedClass() != TargetClass) { continue; }
		SelectedFactory      = NewObject<UFactory>(GetTransientPackage(), FC);
		SelectedFactoryClass = FC;
		break;
	}

	UObject* NewAsset = nullptr;
	FString  UsedFactory;
	if (SelectedFactory)
	{
		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		NewAsset = AssetTools.CreateAsset(AssetName, PackagePath, TargetClass, SelectedFactory);
		if (NewAsset && SelectedFactoryClass)
		{
			UsedFactory = SelectedFactoryClass->GetPathName();
		}
	}

	// ─── Strategy 2: NewObject fallback (only if requested + no factory worked) ────────────────
	bool bAllowFallback = true;
	Request.Args->TryGetBoolField(TEXT("allow_fallback_newobject"), bAllowFallback);

	if (!NewAsset && bAllowFallback)
	{
		const FString PackageNameFull = PackagePath / AssetName;
		UPackage* Pkg = CreatePackage(*PackageNameFull);
		if (Pkg)
		{
			Pkg->FullyLoad();
			NewAsset = NewObject<UObject>(Pkg, TargetClass, FName(*AssetName), RF_Public | RF_Standalone);
			if (NewAsset)
			{
				FAssetRegistryModule::AssetCreated(NewAsset);
				Pkg->MarkPackageDirty();
				UsedFactory = TEXT("(NewObject fallback)");
			}
		}
	}

	if (!NewAsset)
	{
		return FMCPToolHelpers::MakeError(Request,-32603,
			FString::Printf(TEXT("could not create asset of class '%s' at '%s' "
				"(no UFactoryNew matched%s)"),
				*TargetClass->GetPathName(), *DestPathNorm,
				bAllowFallback ? TEXT(" and NewObject fallback failed") : TEXT(" and allow_fallback_newobject=false")));
	}

	if (UPackage* Pkg = NewAsset->GetOutermost()) { Pkg->MarkPackageDirty(); }

	bool bSaveRequested = false;
	bool bSavedOk = false;
	Request.Args->TryGetBoolField(TEXT("save"), bSaveRequested);
	if (bSaveRequested)
	{
		if (UEditorAssetSubsystem* EAS = GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr)
		{
			bSavedOk = EAS->SaveLoadedAsset(NewAsset, /*bOnlyIfIsDirty*/ true);
		}
	}

	return FMCPJsonBuilder()
		.Bool(TEXT("created"), true)
		.Str(TEXT("asset_path"), NewAsset->GetPathName())
		.Str(TEXT("class_path"), TargetClass->GetPathName())
		.Str(TEXT("used_factory"), UsedFactory)
		.Bool(TEXT("saved"), bSavedOk)
		.BuildSuccess(Request);
}

// ─── asset.list_data_asset_classes — discover UDataAsset subclasses available for create ────
//
// Args:
//   - prefix_filter:    string (optional)  case-insensitive class-name prefix filter
//   - include_abstract: bool   (optional)  default false — abstract classes can't be created;
//                                            include them only for discovery/UI purposes
//   - page_token:       string (optional)  FMCPPageCursor; filter_hash binds prefix_filter +
//                                            include_abstract together
//   - page_size:        int    (optional)  default 100, max 1000
//
// Result:
//   - classes[{class_path, class_name, parent_class_path, is_abstract, is_blueprint_generated,
//              source_module, description}]
//   - next_page_token (string|null)
//   - total_known     (int)
//
// Only NATIVE UClasses are enumerated (TObjectIterator<UClass>). Blueprint-based DA classes
// require an AssetRegistry walk + ParentClass-tag lookup — deferred to a future variant
// (`asset.list_data_asset_blueprints`) to keep this tool fast and predictable.
//
// Lane A (TObjectIterator is GT-only).
FMCPResponse Tool_AssetListDataAssetClasses(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString PrefixFilter;
	bool bIncludeAbstract = false;
	int32 PageSize = 100;
	FString PageTokenWire;
	if (Request.Args.IsValid())
	{
		Request.Args->TryGetStringField(TEXT("prefix_filter"), PrefixFilter);
		Request.Args->TryGetBoolField(TEXT("include_abstract"), bIncludeAbstract);
		Request.Args->TryGetStringField(TEXT("page_token"), PageTokenWire);
		int32 RawPageSize = 0;
		if (Request.Args->TryGetNumberField(TEXT("page_size"), RawPageSize))
		{
			PageSize = FMath::Clamp(RawPageSize, 1, 1000);
		}
	}

	// Filter hash binds discovery args so mid-pagination changes → -32015 StaleCursor.
	const uint64 FilterHash = static_cast<uint64>(GetTypeHash(PrefixFilter)) ^
		(static_cast<uint64>(bIncludeAbstract ? 1 : 0) << 32) ^ 0xDA7AA55E70000001ULL;

	FMCPPageCursor Cursor;
	if (!PageTokenWire.IsEmpty())
	{
		FString DecodeErr;
		if (!FMCPPageCursorUtils::Decode(PageTokenWire, Cursor, DecodeErr))
		{
			return FMCPToolHelpers::MakeError(Request,-32602,
				FString::Printf(TEXT("page_token decode failed: %s"), *DecodeErr));
		}
		if (Cursor.FilterHash != FilterHash)
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorStaleCursor,
				TEXT("page_token's filter_hash doesn't match current call's filter — "
					 "caller changed prefix_filter or include_abstract mid-pagination"));
		}
	}

	// Walk all native UClasses; filter to UDataAsset subclasses; sort by class path.
	TArray<UClass*> AllMatching;
	AllMatching.Reserve(64);
	const UClass* BaseClass = UDataAsset::StaticClass();
	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* C = *It;
		if (!C || !C->IsChildOf(BaseClass) || C == BaseClass)
		{
			continue;
		}
		if (!bIncludeAbstract && C->HasAnyClassFlags(CLASS_Abstract))
		{
			continue;
		}
		// Skip skeleton/REINST classes that aren't "real".
		if (C->HasAnyClassFlags(CLASS_NewerVersionExists | CLASS_Deprecated | CLASS_Hidden))
		{
			continue;
		}
		// Apply prefix filter (case-insensitive on class name).
		if (!PrefixFilter.IsEmpty())
		{
			if (!C->GetName().StartsWith(PrefixFilter, ESearchCase::IgnoreCase))
			{
				continue;
			}
		}
		AllMatching.Add(C);
	}
	AllMatching.StableSort([](const UClass& A, const UClass& B)
	{
		return A.GetPathName().Compare(B.GetPathName(), ESearchCase::IgnoreCase) < 0;
	});

	const int32 TotalKnown = AllMatching.Num();

	// Skip-until-past-sentinel.
	int32 StartIdx = 0;
	if (!Cursor.LastAssetPath.IsEmpty())
	{
		while (StartIdx < TotalKnown)
		{
			if (AllMatching[StartIdx]->GetPathName().Compare(Cursor.LastAssetPath, ESearchCase::IgnoreCase) > 0)
			{
				break;
			}
			++StartIdx;
		}
	}
	const int32 EndExcl = FMath::Min(TotalKnown, StartIdx + PageSize);

	TArray<TSharedPtr<FJsonValue>> Out;
	Out.Reserve(EndExcl - StartIdx);
	for (int32 i = StartIdx; i < EndExcl; ++i)
	{
		UClass* C = AllMatching[i];
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("class_path"), C->GetPathName());
		Obj->SetStringField(TEXT("class_name"), C->GetName());
		Obj->SetStringField(TEXT("parent_class_path"),
			C->GetSuperClass() ? C->GetSuperClass()->GetPathName() : FString());
		Obj->SetBoolField(TEXT("is_abstract"), C->HasAnyClassFlags(CLASS_Abstract));
		Obj->SetBoolField(TEXT("is_blueprint_generated"), false);  // native-only this tool
		if (UPackage* Pkg = C->GetOutermost())
		{
			Obj->SetStringField(TEXT("source_module"), Pkg->GetName());
		}
#if WITH_EDITORONLY_DATA
		// Class display tooltip if available (metadata-driven; safe in editor).
		const FText Tooltip = C->GetToolTipText(/*bShortTooltip*/ true);
		if (!Tooltip.IsEmpty())
		{
			Obj->SetStringField(TEXT("description"), Tooltip.ToString());
		}
#endif
		Out.Add(MakeShared<FJsonValueObject>(Obj));
	}

	const bool bHasNext = (EndExcl < TotalKnown && Out.Num() > 0);
	return FMCPJsonBuilder()
		.Arr(TEXT("classes"), MoveTemp(Out))
		.Int(TEXT("total_known"), TotalKnown)
		.If(bHasNext, [&](FMCPJsonBuilder& B)
		{
			FMCPPageCursor NewCursor;
			NewCursor.FilterHash = FilterHash;
			NewCursor.LastAssetPath = AllMatching[EndExcl - 1]->GetPathName();
			NewCursor.TotalKnownSnapshot = TotalKnown;
			B.Str(TEXT("next_page_token"), FMCPPageCursorUtils::Encode(NewCursor));
		})
		.If(!bHasNext, [](FMCPJsonBuilder& B) { B.Null(TEXT("next_page_token")); })
		.BuildSuccess(Request);
}

// ─── Registration ────────────────────────────────────────────────────────────────────────────
void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	// HOTFIX 2026-05 (Plan R11 systemic-unsafe contingency): ALL AR read tools demoted to Lane A.
	// Discovered via autonomous test crash: IAR.GetAssets() asserts when enumerating in-memory
	// assets off the game thread because UE 5.7's GetAssetRegistryTags() is not fully thread-safe
	// ("Enumerating in-memory assets can only be done on the game thread or in the loader, there
	// are too many GetAssetRegistryTags() still not thread-safe" — AssetRegistry.cpp:2906). The
	// AR read API was documented thread-safe since UE 5.0 but the in-memory-enumeration path
	// regressed. Lane B router (FMCPDispatchQueue::DispatchInline + IsThreadSafe + FMCPConnection
	// short-circuit) remains for Phase 3+ — only the per-tool flag changes here. Possible Phase 3+
	// workaround: pass Filter.bIncludeOnlyOnDiskAssets=true to skip the unsafe enumeration path.
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	// Day 2: first 4 tools (all Lane A post-hotfix).
	RegisterTool(TEXT("asset.exists"),                &Tool_AssetExists,                /*Lane A (was B)*/ false);
	RegisterTool(TEXT("asset.metadata"),              &Tool_AssetMetadata,              /*Lane A (was B)*/ false);
	RegisterTool(TEXT("asset.get_outermost_package"), &Tool_AssetGetOutermostPackage,   /*Lane A (was B)*/ false);
	RegisterTool(TEXT("asset.is_dirty"),              &Tool_AssetIsDirty,               /*Lane A*/          false);

	// Day 3+ AR query tools — all Lane A post-hotfix. Thumbnail tools were already Lane A
	// (need game-thread for RT enqueue + FObjectThumbnail cache) and are unchanged.
	RegisterTool(TEXT("asset.list"),                  &Tool_AssetList,                  /*Lane A (was B)*/ false);
	RegisterTool(TEXT("asset.find_references"),       &Tool_AssetFindReferences,        /*Lane A (was B)*/ false);
	RegisterTool(TEXT("asset.find_dependents"),       &Tool_AssetFindDependents,        /*Lane A (was B)*/ false);
	RegisterTool(TEXT("asset.search_by_class"),       &Tool_AssetSearchByClass,         /*Lane A (was B)*/ false);
	RegisterTool(TEXT("asset.search_by_tag"),         &Tool_AssetSearchByTag,           /*Lane A (was B)*/ false);
	RegisterTool(TEXT("asset.search_by_name"),        &Tool_AssetSearchByName,          /*Lane A (was B)*/ false);
	RegisterTool(TEXT("asset.get_thumbnail"),         &Tool_AssetGetThumbnail,          /*Lane A*/          false);
	RegisterTool(TEXT("asset.get_thumbnail_to_disk"), &Tool_AssetGetThumbnailToDisk,    /*Lane A*/          false);
	RegisterTool(TEXT("asset.get_class_hierarchy"),   &Tool_AssetGetClassHierarchy,     /*Lane A (was B)*/ false);

	// Data Asset creation surface (2026-05): create + discover DA classes.
	RegisterTool(TEXT("asset.create_data_asset"),       &Tool_AssetCreateDataAsset,       /*Lane A*/          false);
	RegisterTool(TEXT("asset.list_data_asset_classes"), &Tool_AssetListDataAssetClasses,  /*Lane A*/          false);
	// Generic asset creation for any UClass (UFactoryNew + NewObject fallback).
	RegisterTool(TEXT("asset.create"),                  &Tool_AssetCreate,                /*Lane A*/          false);

	// Wave M (M2) — recursive dep/referencer graph (BFS). All Lane A per critique Q4 ("ship working
	// tools first, optimise threading separately"); reviewer audits Lane B promotion in followup.
	RegisterTool(TEXT("asset.dependency_graph"),        &Tool_AssetDependencyGraph,       /*Lane A*/          false);

	UE_LOG(LogMCP, Log, TEXT("Phase 2 hotfix: registered 14 asset.* handlers (all Lane A — UE 5.7 AR not thread-safe; Wave M added dependency_graph)"));
}

} // namespace FAssetRegistryTools

MCP_REGISTER_SURFACE(AssetRegistryTools, &FAssetRegistryTools::Register)
