// Copyright FatumGame. All Rights Reserved.

#include "AssetRegistryTools.h"

#include "FMCPDispatchQueue.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPARFilterParser.h"
#include "Utils/MCPAssetPathUtils.h"
#include "Utils/MCPPageCursor.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetIdentifier.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "ImageUtils.h"
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
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Items;
		Items.Reserve(PageAssets.Num());
		for (const FAssetData& Data : PageAssets)
		{
			Items.Add(MakeShared<FJsonValueObject>(AR_BuildAssetSummary(Data, bIncludeTags)));
		}
		Out->SetArrayField(ItemsFieldName, Items);
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

		if (bIncludeColdCacheFlag)
		{
			Out->SetBoolField(TEXT("is_cold_cache"),
				FAssetRegistryModule::GetRegistry().IsLoadingAssets());
		}
		return AR_MakeSuccessObj(Request, Out);
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
			OutError = AR_MakeError(Request, kMCPErrorStaleCursor,
				FString::Printf(TEXT("page_token decode failed: %s"), *DecodeErr));
			return false;
		}
		if (!FMCPPageCursorUtils::ValidateAgainstFilter(OutCursor, ExpectedFilterHash))
		{
			OutError = AR_MakeError(Request, kMCPErrorStaleCursor,
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

	// Lane B contract: NO UObject access. FindPackage walks the global package hash and is
	// game-thread-only — concurrent invocation during GC or async load → use-after-free. The
	// `package_flags` field is dropped from the response (was documented as "0 if not loaded",
	// utility for AI workflows was marginal). asset.is_dirty (Lane A) remains for callers that
	// need live UPackage state.
	const FString PackageNameStr = Data.PackageName.ToString();
	const bool bOnDisk = FPackageName::DoesPackageExist(PackageNameStr);

	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("package_path"), PackageNameStr);
	Out->SetBoolField(TEXT("on_disk"), bOnDisk);
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

// ─── asset.list (paginated, FARFilter) ───────────────────────────────────────────────────────
FMCPResponse Tool_AssetList(const FMCPRequest& Request)
{
	if (!Request.Args.IsValid())
	{
		return AR_MakeError(Request, kARErrorInvalidParams, TEXT("missing args object"));
	}

	// Parse FARFilter from args.filter (object). Missing/empty → default FARFilter (matches all).
	const TSharedPtr<FJsonObject>* FilterObjPtr = nullptr;
	Request.Args->TryGetObjectField(TEXT("filter"), FilterObjPtr);
	TSharedPtr<FJsonObject> FilterObj = (FilterObjPtr && FilterObjPtr->IsValid()) ? *FilterObjPtr : TSharedPtr<FJsonObject>();

	FARFilter Filter;
	FString ParseErr;
	if (!FMCPARFilterParser::Parse(FilterObj, Filter, ParseErr))
	{
		return AR_MakeError(Request, kARErrorInvalidParams,
			FString::Printf(TEXT("filter parse error: %s"), *ParseErr));
	}

	// Overly-broad guard (D11) — opt-out via args._unsafe_full_scan.
	bool bUnsafeFullScan = false;
	Request.Args->TryGetBoolField(TEXT("_unsafe_full_scan"), bUnsafeFullScan);
	if (!bUnsafeFullScan && AR_IsOverlyBroad(Filter))
	{
		return AR_MakeError(Request, kMCPErrorOverlyBroadQuery,
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
			return AR_MakeError(Request, kMCPErrorObjectNotFound,
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
			return AR_MakeError(Request, kMCPErrorOverlyBroadQuery, WalkErr);
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

		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Items;
		Items.Reserve(Page.Num());
		for (const FAssetDependency& Dep : Page)
		{
			TSharedPtr<FJsonObject> ItemObj = MakeShared<FJsonObject>();
			ItemObj->SetStringField(TEXT("package_path"), Dep.AssetId.PackageName.ToString());
			ItemObj->SetStringField(TEXT("dependency_kind"), AR_ClassifyDependency(Dep));
			Items.Add(MakeShared<FJsonValueObject>(ItemObj));
		}
		Out->SetArrayField(ItemsFieldName, Items);
		Out->SetNumberField(TEXT("total_known"), static_cast<double>(All.Num()));
		if (NextSentinel.IsEmpty())
		{
			Out->SetField(TEXT("next_page_token"), MakeShared<FJsonValueNull>());
		}
		else
		{
			FMCPPageCursor NewCursor;
			NewCursor.FilterHash = FilterHash;
			NewCursor.LastAssetPath = NextSentinel;
			NewCursor.TotalKnownSnapshot = All.Num();
			Out->SetStringField(TEXT("next_page_token"), FMCPPageCursorUtils::Encode(NewCursor));
		}
		return AR_MakeSuccessObj(Request, Out);
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
// ─── asset.search_by_class ───────────────────────────────────────────────────────────────────
FMCPResponse Tool_AssetSearchByClass(const FMCPRequest& Request)
{
	if (!Request.Args.IsValid())
	{
		return AR_MakeError(Request, kARErrorInvalidParams, TEXT("missing args object"));
	}

	FString ClassPath;
	if (!Request.Args->TryGetStringField(TEXT("class_path"), ClassPath) || ClassPath.IsEmpty())
	{
		return AR_MakeError(Request, kARErrorInvalidParams,
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
		return AR_MakeError(Request, kMCPErrorWrongClass,
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
				return AR_MakeError(Request, kARErrorInvalidParams,
					TEXT("package_paths: expected array of strings"));
			}
			const FString Norm = FMCPAssetPathUtils::Normalize(S);
			if (Norm.IsEmpty())
			{
				return AR_MakeError(Request, kMCPErrorInvalidPath,
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
		return AR_MakeError(Request, kMCPErrorOverlyBroadQuery,
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
		return AR_MakeError(Request, kARErrorInvalidParams, TEXT("missing args object"));
	}

	FString TagName;
	if (!Request.Args->TryGetStringField(TEXT("tag_name"), TagName) || TagName.IsEmpty())
	{
		return AR_MakeError(Request, kARErrorInvalidParams,
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
				return AR_MakeError(Request, kARErrorInvalidParams,
					TEXT("package_paths: expected array of strings"));
			}
			const FString Norm = FMCPAssetPathUtils::Normalize(S);
			if (Norm.IsEmpty())
			{
				return AR_MakeError(Request, kMCPErrorInvalidPath,
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
	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Items;
	Items.Reserve(Page.Num());
	for (const FAssetData& Data : Page)
	{
		TSharedPtr<FJsonObject> ItemObj = AR_BuildAssetSummary(Data, /*bIncludeTags*/ false);
		const FAssetTagValueRef ValRef = Data.TagsAndValues.FindTag(FName(*TagName));
		ItemObj->SetStringField(TEXT("tag_value"), ValRef.IsSet() ? ValRef.AsString() : FString());
		Items.Add(MakeShared<FJsonValueObject>(ItemObj));
	}
	Out->SetArrayField(TEXT("matches"), Items);
	Out->SetNumberField(TEXT("total_known"), static_cast<double>(Filtered.Num()));
	if (NextSentinel.IsEmpty())
	{
		Out->SetField(TEXT("next_page_token"), MakeShared<FJsonValueNull>());
	}
	else
	{
		FMCPPageCursor NewCursor;
		NewCursor.FilterHash = FilterHash;
		NewCursor.LastAssetPath = NextSentinel;
		NewCursor.TotalKnownSnapshot = Filtered.Num();
		Out->SetStringField(TEXT("next_page_token"), FMCPPageCursorUtils::Encode(NewCursor));
	}
	return AR_MakeSuccessObj(Request, Out);
}

// ─── asset.search_by_name ────────────────────────────────────────────────────────────────────
FMCPResponse Tool_AssetSearchByName(const FMCPRequest& Request)
{
	if (!Request.Args.IsValid())
	{
		return AR_MakeError(Request, kARErrorInvalidParams, TEXT("missing args object"));
	}

	FString Pattern;
	if (!Request.Args->TryGetStringField(TEXT("name_pattern"), Pattern) || Pattern.IsEmpty())
	{
		return AR_MakeError(Request, kARErrorInvalidParams,
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
				return AR_MakeError(Request, kARErrorInvalidParams,
					TEXT("package_paths: expected array of strings"));
			}
			const FString Norm = FMCPAssetPathUtils::Normalize(S);
			if (Norm.IsEmpty())
			{
				return AR_MakeError(Request, kMCPErrorInvalidPath,
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
		return AR_MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("no AssetRegistry entry for '%s'"), *NormalizedPath));
	}

	int32 Size = 128;
	Request.Args->TryGetNumberField(TEXT("size"), Size);
	if (Size < 16 || Size > 256)
	{
		// Cap is 256 per D6 — larger requests get the disk-tier tool.
		return AR_MakeError(Request, kARErrorInvalidParams,
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
		return AR_MakeError(Request, kMCPErrorThumbnailRenderFailed,
			FString::Printf(TEXT("thumbnail render failed: %s"), *RenderErr));
	}

	TArray64<uint8> PNGBytes;
	if (!AR_EncodePNG(RGBA8, Width, Height, PNGBytes))
	{
		return AR_MakeError(Request, kMCPErrorThumbnailRenderFailed,
			TEXT("PNG encode failed (unexpected RGBA8 size mismatch)"));
	}

	const FString Base64 = FBase64::Encode(reinterpret_cast<const uint8*>(PNGBytes.GetData()), PNGBytes.Num());

	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("base64"), Base64);
	Out->SetStringField(TEXT("mime"), TEXT("image/png"));
	Out->SetNumberField(TEXT("width"),  Width);
	Out->SetNumberField(TEXT("height"), Height);
	Out->SetBoolField(TEXT("is_class_generic"), bIsClassGeneric);
	return AR_MakeSuccessObj(Request, Out);
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
		return AR_MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("no AssetRegistry entry for '%s'"), *NormalizedPath));
	}

	int32 Size = 256;
	Request.Args->TryGetNumberField(TEXT("size"), Size);
	if (Size < 16 || Size > 2048)
	{
		return AR_MakeError(Request, kARErrorInvalidParams,
			FString::Printf(TEXT("size %d outside [16, 2048]"), Size));
	}

	// Format: png (default) or jpg.
	FString Format = TEXT("png");
	Request.Args->TryGetStringField(TEXT("format"), Format);
	const bool bJpg = Format.Equals(TEXT("jpg"), ESearchCase::IgnoreCase) ||
		Format.Equals(TEXT("jpeg"), ESearchCase::IgnoreCase);

	// Resolve output_path: default = <Saved>/UnrealMCP/thumbnails/<uuid>.<ext>.
	FString OutputPathRaw;
	Request.Args->TryGetStringField(TEXT("output_path"), OutputPathRaw);
	if (OutputPathRaw.IsEmpty())
	{
		const FString Ext = bJpg ? TEXT("jpg") : TEXT("png");
		const FGuid Guid = FGuid::NewGuid();
		OutputPathRaw = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealMCP"),
			TEXT("thumbnails"), FString::Printf(TEXT("%s.%s"), *Guid.ToString(EGuidFormats::Digits), *Ext));
	}

	// Sandbox resolve (PATH_ESCAPE on whitelist miss).
	FString AbsPath;
	FString SandboxErr;
	if (!FMCPPathSandbox::Resolve(OutputPathRaw, AbsPath, SandboxErr))
	{
		return AR_MakeError(Request, kMCPErrorPathEscape, SandboxErr);
	}

	TArray<uint8> RGBA8;
	int32 Width = 0, Height = 0;
	bool bIsClassGeneric = false;
	FString RenderErr;
	if (!AR_RenderAssetThumbnail(NormalizedPath, Size, RGBA8, Width, Height, bIsClassGeneric, RenderErr))
	{
		return AR_MakeError(Request, kMCPErrorThumbnailRenderFailed,
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
			return AR_MakeError(Request, kMCPErrorThumbnailRenderFailed, TEXT("JPG encode failed"));
		}
	}
	else
	{
		if (!AR_EncodePNG(RGBA8, Width, Height, EncodedBytes))
		{
			return AR_MakeError(Request, kMCPErrorThumbnailRenderFailed,
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
		return AR_MakeError(Request, kMCPErrorThumbnailRenderFailed,
			TEXT("encoded thumbnail exceeds 2 GiB — refusing to write"));
	}
	TArray<uint8> WriteBuf;
	WriteBuf.Append(EncodedBytes.GetData(), static_cast<int32>(EncodedBytes.Num()));
	if (!FFileHelper::SaveArrayToFile(WriteBuf, *AbsPath))
	{
		return AR_MakeError(Request, kMCPErrorThumbnailRenderFailed,
			FString::Printf(TEXT("could not write thumbnail to '%s'"), *AbsPath));
	}

	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("path"), AbsPath);
	Out->SetNumberField(TEXT("bytes"), static_cast<double>(WriteBuf.Num()));
	Out->SetNumberField(TEXT("width"),  Width);
	Out->SetNumberField(TEXT("height"), Height);
	return AR_MakeSuccessObj(Request, Out);
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
		return AR_MakeError(Request, kMCPErrorObjectNotFound,
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
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetArrayField(TEXT("chain"), Chain);
		return AR_MakeSuccessObj(Request, Out);
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

	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetArrayField(TEXT("chain"), Chain);
	return AR_MakeSuccessObj(Request, Out);
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

	UE_LOG(LogMCP, Log, TEXT("Phase 2 hotfix: registered 13 asset.* handlers (all Lane A — UE 5.7 AR not thread-safe)"));
}

} // namespace FAssetRegistryTools
