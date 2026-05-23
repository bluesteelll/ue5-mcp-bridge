// Copyright FatumGame. All Rights Reserved.

#include "AssetCompositeTools.h"

#include "MCPSurfaceRegistry.h"

#include "FMCPDispatchQueue.h"
#include "FMCPJobRegistry.h"
#include "MCPJsonBuilder.h"
#include "MCPToolHelpers.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPAssetPathUtils.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetIdentifier.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/PackageName.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/TopLevelAssetPath.h"

namespace
{
	// COMP_ prefix per the unity-build symbol-collision pattern. Per-surface error constants kept;
	// XX_StampIds/MakeError/MakeSuccessObj removed in Phase 3 — use FMCPToolHelpers::Xxx from
	// MCPToolHelpers.h.
	constexpr int32 kCOMPErrorInvalidParams = -32602;
	constexpr int32 kCOMPErrorInternal      = -32603;

	/**
	 * Parse the ``package_paths`` array argument: required, minItems=1, every entry normalised.
	 * Returns false + populates OutError on any malformed input. Used by find_unused / size_report
	 * internals (both take the same shape).
	 */
	bool COMP_ParsePackagePaths(const FMCPRequest& Request, TArray<FString>& OutNormalized,
		FMCPResponse& OutError)
	{
		if (!Request.Args.IsValid())
		{
			OutError = FMCPToolHelpers::MakeError(Request, kCOMPErrorInvalidParams, TEXT("missing args object"));
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Request.Args->TryGetArrayField(TEXT("package_paths"), Arr) || Arr == nullptr || Arr->Num() == 0)
		{
			OutError = FMCPToolHelpers::MakeError(Request, kCOMPErrorInvalidParams,
				TEXT("missing or empty required array field 'package_paths'"));
			return false;
		}
		OutNormalized.Reserve(Arr->Num());
		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			FString S;
			if (!V.IsValid() || !V->TryGetString(S))
			{
				OutError = FMCPToolHelpers::MakeError(Request, kCOMPErrorInvalidParams,
					TEXT("package_paths: expected array of strings"));
				return false;
			}
			const FString Norm = FMCPAssetPathUtils::Normalize(S);
			if (Norm.IsEmpty())
			{
				OutError = FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidPath,
					FString::Printf(TEXT("package_paths entry '%s' is malformed"), *S));
				return false;
			}
			OutNormalized.Add(Norm);
		}
		return true;
	}

	/** Build the {asset_path, package_path, class, tags} object used by batch_metadata. */
	TSharedRef<FJsonObject> COMP_BuildMetadataEntry(const FAssetData& Data)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("asset_path"),   Data.GetObjectPathString());
		Obj->SetStringField(TEXT("package_path"), Data.PackagePath.ToString());
		Obj->SetStringField(TEXT("class"),        Data.AssetClassPath.ToString());
		TSharedRef<FJsonObject> TagsObj = MakeShared<FJsonObject>();
		Data.TagsAndValues.ForEach([&TagsObj](const TPair<FName, FAssetTagValueRef>& Pair)
		{
			TagsObj->SetStringField(Pair.Key.ToString(), Pair.Value.AsString());
		});
		Obj->SetObjectField(TEXT("tags"), TagsObj);
		return Obj;
	}
}

namespace FAssetCompositeTools
{

// ─── asset._find_unused_internal (HOTFIX 2 — async job pattern; sync handler is Lane B) ─────
//
// Why async?
//
//   Hotfix 1 demoted both internals to Lane A so the AR enumeration would run on the game thread
//   (UE 5.7 asserts on non-game-thread enumeration — see AssetRegistry.cpp:2906). But that broke
//   the Python composite path: asset.find_unused / asset.size_report run inside CallPythonTool
//   on the game thread, then call dispatch_internal('asset._find_unused_internal', ...) which
//   opens a TCP loopback and blocks on sock.recv(). The Lane-A handler would queue back to the
//   SAME game thread that's blocked on the socket → 60-second deadlock until TCP timeout.
//
//   The fix: the sync handler runs Lane B (validate args → submit job → return {job_id}) — never
//   touches AR on the listener thread. The actual enumeration runs in the job body via
//   bGameThreadRequired=true (the worker thread blocks on AsyncTask(GameThread, …) → satisfies
//   the UE 5.7 game-thread requirement for AR enumeration). The Python composite then polls
//   job.result (also promoted to Lane B by this hotfix) which returns immediately on the listener
//   thread without touching the game thread, so the game-thread Tick can drain the AsyncTask
//   pending the job body. Loop completes, composite returns the inner result. No deadlock.
//
//   See FMCPDay7Handlers.cpp JobStatus/JobResult promotion notes for the dual-Lane-B contract.
FMCPResponse Tool_FindUnusedInternal(const FMCPRequest& Request)
{
	TArray<FString> Scopes;
	FMCPResponse Err;
	if (!COMP_ParsePackagePaths(Request, Scopes, Err)) { return Err; }

	// exclude_class_paths: array of class paths whose assets are skipped even if unused.
	// Default set per the inline schema in the plan — World, GameMode, SaveGame, etc.
	TSet<FString> ExcludeClasses;
	const TArray<TSharedPtr<FJsonValue>>* ExclPtr = nullptr;
	if (Request.Args->TryGetArrayField(TEXT("exclude_class_paths"), ExclPtr) && ExclPtr != nullptr)
	{
		for (const TSharedPtr<FJsonValue>& V : *ExclPtr)
		{
			FString S;
			if (V.IsValid() && V->TryGetString(S) && !S.IsEmpty())
			{
				ExcludeClasses.Add(S);
			}
		}
	}

	// Submit a game-thread-required job. The body runs on the game thread (via the registry's
	// AsyncTask coordinator) so IAR.GetAssets enumeration is safe per the UE 5.7 contract.
	const FGuid JobId = FMCPJobRegistry::Get().SubmitJob(
		TEXT("asset._find_unused_internal"),
		[ScopesCap = MoveTemp(Scopes), ExcludeClassesCap = MoveTemp(ExcludeClasses)]
		(FMCPJob& Job) -> TSharedPtr<FJsonValue>
		{
			// Build a single AR query spanning all scopes (recursive).
			IAssetRegistry& IAR = FAssetRegistryModule::GetRegistry();
			FARFilter Filter;
			Filter.bRecursivePaths = true;
			for (const FString& S : ScopesCap) { Filter.PackagePaths.Add(FName(*S)); }

			TArray<FAssetData> Candidates;
			IAR.GetAssets(Filter, Candidates);

			// Hard cap per the plan — 5000 entries. Surfaced as ErrorMessage → job ends Failed,
			// composite re-raises as RuntimeError with the cap message.
			constexpr int32 kFindUnusedCap = 5000;
			if (Candidates.Num() > kFindUnusedCap)
			{
				Job.ErrorMessage = FString::Printf(
					TEXT("OVERLY_BROAD_QUERY: scope contains %d candidates (cap=%d) — narrow package_paths"),
					Candidates.Num(), kFindUnusedCap);
				return nullptr; // → Failed
			}

			TArray<TSharedPtr<FJsonValue>> Unused;
			Unused.Reserve(Candidates.Num());

			const int32 Total = Candidates.Num();
			for (int32 i = 0; i < Total; ++i)
			{
				if (Job.bCancelRequested.load(std::memory_order_acquire))
				{
					Job.ErrorMessage = TEXT("cancelled");
					return nullptr;
				}
				// Cheap progress update — Total may be small but the bridge still benefits from
				// a non-zero progress reading for the polling client.
				Job.Progress.store(static_cast<float>(i) / FMath::Max(1, Total),
					std::memory_order_release);

				const FAssetData& Data = Candidates[i];

				// Skip excluded class kinds. Compare against the AssetClassPath string for portability
				// (the user passes "/Script/Engine.World" etc.).
				if (ExcludeClassesCap.Contains(Data.AssetClassPath.ToString()))
				{
					continue;
				}

				// Look up referencers via package-level GetReferencers. Empty result = unused per
				// static analysis. The well-known caveat about runtime LoadClass / construction
				// script refs being invisible to AR is documented in the tool description.
				TArray<FName> Referencers;
				IAR.GetReferencers(Data.PackageName, Referencers, UE::AssetRegistry::EDependencyCategory::Package);
				if (Referencers.Num() == 0)
				{
					TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
					Obj->SetStringField(TEXT("asset_path"), Data.GetObjectPathString());
					Obj->SetStringField(TEXT("class"),      Data.AssetClassPath.ToString());
					Unused.Add(MakeShared<FJsonValueObject>(Obj));
				}
			}

			Job.Progress.store(1.0f, std::memory_order_release);

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetArrayField(TEXT("unused"), Unused);
			Out->SetNumberField(TEXT("scanned_count"), Candidates.Num());
			return MakeShared<FJsonValueObject>(Out);
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

// ─── asset._size_report_internal (HOTFIX 2 — async job pattern; sync handler is Lane B) ─────
//
// Identical deadlock-avoidance rationale as Tool_FindUnusedInternal above.
FMCPResponse Tool_SizeReportInternal(const FMCPRequest& Request)
{
	TArray<FString> Scopes;
	FMCPResponse Err;
	if (!COMP_ParsePackagePaths(Request, Scopes, Err)) { return Err; }

	int32 TopN = 50;
	Request.Args->TryGetNumberField(TEXT("top_n"), TopN);
	TopN = FMath::Clamp(TopN, 1, 1000);

	const FGuid JobId = FMCPJobRegistry::Get().SubmitJob(
		TEXT("asset._size_report_internal"),
		[ScopesCap = MoveTemp(Scopes), TopNCap = TopN]
		(FMCPJob& Job) -> TSharedPtr<FJsonValue>
		{
			IAssetRegistry& IAR = FAssetRegistryModule::GetRegistry();
			FARFilter Filter;
			Filter.bRecursivePaths = true;
			for (const FString& S : ScopesCap) { Filter.PackagePaths.Add(FName(*S)); }

			TArray<FAssetData> All;
			IAR.GetAssets(Filter, All);

			// Internal cap: 50,000 assets per call. Larger projects can still be analysed via
			// per-folder subqueries; the cap is a defence against a runaway full-/Game/ scan.
			constexpr int32 kSizeReportCap = 50000;
			if (All.Num() > kSizeReportCap)
			{
				UE_LOG(LogMCP, Warning,
					TEXT("asset._size_report_internal: scope contains %d assets, truncating to %d"),
					All.Num(), kSizeReportCap);
				All.SetNum(kSizeReportCap, EAllowShrinking::No);
			}

			// Pair: (size, idx-into-All). Sort descending by size, keep top N.
			struct FEntry
			{
				int64 Bytes = 0;
				int32 Index = INDEX_NONE;
			};
			TArray<FEntry> Entries;
			Entries.Reserve(All.Num());

			int64 TotalBytes = 0;
			const int32 Total = All.Num();
			for (int32 i = 0; i < Total; ++i)
			{
				if (Job.bCancelRequested.load(std::memory_order_acquire))
				{
					Job.ErrorMessage = TEXT("cancelled");
					return nullptr;
				}
				// Progress updates each 256 entries — file-size lookup is the slow part.
				if ((i & 0xFF) == 0)
				{
					Job.Progress.store(static_cast<float>(i) / FMath::Max(1, Total),
						std::memory_order_release);
				}

				FString PackageFilename;
				if (!FPackageName::DoesPackageExist(All[i].PackageName.ToString(), &PackageFilename))
				{
					continue; // in-memory only
				}
				const int64 Size = IFileManager::Get().FileSize(*PackageFilename);
				if (Size <= 0) { continue; }
				TotalBytes += Size;
				Entries.Add({ Size, i });
			}

			Entries.Sort([](const FEntry& A, const FEntry& B) { return A.Bytes > B.Bytes; });

			TArray<TSharedPtr<FJsonValue>> Top;
			const int32 Limit = FMath::Min(TopNCap, Entries.Num());
			Top.Reserve(Limit);
			for (int32 i = 0; i < Limit; ++i)
			{
				const FAssetData& Data = All[Entries[i].Index];
				TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
				Obj->SetStringField(TEXT("asset_path"), Data.GetObjectPathString());
				Obj->SetStringField(TEXT("class"),      Data.AssetClassPath.ToString());
				Obj->SetNumberField(TEXT("bytes"),      static_cast<double>(Entries[i].Bytes));
				Top.Add(MakeShared<FJsonValueObject>(Obj));
			}

			Job.Progress.store(1.0f, std::memory_order_release);

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetArrayField(TEXT("top"), Top);
			Out->SetNumberField(TEXT("total_bytes"), static_cast<double>(TotalBytes));
			return MakeShared<FJsonValueObject>(Out);
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

// ─── asset._batch_metadata_internal (Lane B — sync only validates strings + submits job) ───
FMCPResponse Tool_BatchMetadataInternal(const FMCPRequest& Request)
{
	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kCOMPErrorInvalidParams, TEXT("missing args object"));
	}

	const TArray<TSharedPtr<FJsonValue>>* PathsPtr = nullptr;
	if (!Request.Args->TryGetArrayField(TEXT("paths"), PathsPtr) || PathsPtr == nullptr || PathsPtr->Num() == 0)
	{
		return FMCPToolHelpers::MakeError(Request, kCOMPErrorInvalidParams,
			TEXT("missing or empty required array field 'paths'"));
	}

	// Validate every path upfront (INVALID_PATH per-call, not per-entry — caller must clean input).
	TArray<FString> NormalizedPaths;
	NormalizedPaths.Reserve(PathsPtr->Num());
	for (const TSharedPtr<FJsonValue>& V : *PathsPtr)
	{
		FString S;
		if (!V.IsValid() || !V->TryGetString(S))
		{
			return FMCPToolHelpers::MakeError(Request, kCOMPErrorInvalidParams,
				TEXT("paths: expected array of strings"));
		}
		const FString Norm = FMCPAssetPathUtils::Normalize(S);
		if (Norm.IsEmpty())
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidPath,
				FString::Printf(TEXT("paths entry '%s' is malformed"), *S));
		}
		NormalizedPaths.Add(Norm);
	}

	// Submit a Lane-B-safe worker-thread job. The body never touches a UObject — AR lookups are
	// thread-safe per the documented contract.
	const FGuid JobId = FMCPJobRegistry::Get().SubmitJob(
		TEXT("asset._batch_metadata_internal"),
		[Paths = MoveTemp(NormalizedPaths)](FMCPJob& Job) -> TSharedPtr<FJsonValue>
		{
			const double StartTime = FPlatformTime::Seconds();
			const int32 Total = Paths.Num();

			IAssetRegistry& IAR = FAssetRegistryModule::GetRegistry();

			TArray<TSharedPtr<FJsonValue>> Assets, Failed;
			Assets.Reserve(Total);
			Failed.Reserve(Total);

			for (int32 i = 0; i < Total; ++i)
			{
				if (Job.bCancelRequested.load(std::memory_order_acquire))
				{
					Job.ErrorMessage = TEXT("cancelled");
					return nullptr;
				}

				Job.Progress.store(static_cast<float>(i) / FMath::Max(1, Total),
					std::memory_order_release);
				Job.Description = FString::Printf(TEXT("Metadata %d/%d"), i + 1, Total);

				const FString& P = Paths[i];
				const FString ObjectPath = FMCPAssetPathUtils::ToObjectPath(P);
				const FSoftObjectPath Soft(ObjectPath);
				const FAssetData Data = IAR.GetAssetByObjectPath(Soft, /*bIncludeOnlyOnDiskAssets*/ false);

				if (!Data.IsValid())
				{
					TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
					Entry->SetStringField(TEXT("path"), P);
					Entry->SetStringField(TEXT("error"), TEXT("no AssetRegistry entry"));
					Failed.Add(MakeShared<FJsonValueObject>(Entry));
					continue;
				}

				Assets.Add(MakeShared<FJsonValueObject>(COMP_BuildMetadataEntry(Data)));
			}

			Job.Progress.store(1.0f, std::memory_order_release);

			const double DurationMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;
			TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetArrayField(TEXT("assets"), Assets);
			Obj->SetArrayField(TEXT("failed"), Failed);
			Obj->SetNumberField(TEXT("duration_ms"), DurationMs);
			return MakeShared<FJsonValueObject>(Obj);
		},
		/*bGameThreadRequired*/ false);

	if (!JobId.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorJobSubmitFailed,
			TEXT("FMCPJobRegistry::SubmitJob refused (shutdown?)"));
	}

	return FMCPJsonBuilder()
		.Str(TEXT("job_id"), JobId.ToString(EGuidFormats::DigitsWithHyphens))
		.BuildSuccess(Request);
}

// ─── asset._find_broken_references_internal (Hotfix 3 — Lane B, async-job pattern) ───────────
//
// Walks the AR for every asset in scope, collects hard dependencies via GetDependencies, then
// checks each dep package against the AR (GetAssetsByPackageName non-empty). Accumulates broken
// refs in a {asset_path, missing_paths[]} list. Game-thread-required because IAR.GetAssets
// + IAR.GetDependencies + IAR.GetAssetsByPackageName all touch the AR in-memory tables that
// UE 5.7 asserts on off-GT.
//
// Hotfix 3 rationale: the previous Python implementation looped via TCP loopback (asset.list →
// asset.find_dependents → asset.exists per asset × N), which deadlocked the same way as
// find_unused did before hotfix 2 — composite runs on GT, every loopback call queues back to GT
// behind composite's blocked socket. C++ promotion executes the whole walk inside ONE job body.
FMCPResponse Tool_FindBrokenReferencesInternal(const FMCPRequest& Request)
{
	TArray<FString> Scopes;
	FMCPResponse Err;
	if (!COMP_ParsePackagePaths(Request, Scopes, Err)) { return Err; }

	const FGuid JobId = FMCPJobRegistry::Get().SubmitJob(
		TEXT("asset._find_broken_references_internal"),
		[ScopesCap = MoveTemp(Scopes)]
		(FMCPJob& Job) -> TSharedPtr<FJsonValue>
		{
			IAssetRegistry& IAR = FAssetRegistryModule::GetRegistry();

			// Enumerate every asset across all scopes recursively (single AR call — same pattern
			// as Tool_FindUnusedInternal).
			FARFilter Filter;
			Filter.bRecursivePaths = true;
			for (const FString& S : ScopesCap) { Filter.PackagePaths.Add(FName(*S)); }

			TArray<FAssetData> All;
			IAR.GetAssets(Filter, All);

			constexpr int32 kFindBrokenCap = 5000;
			if (All.Num() > kFindBrokenCap)
			{
				Job.ErrorMessage = FString::Printf(
					TEXT("OVERLY_BROAD_QUERY: scope contains %d candidates (cap=%d) — narrow package_paths"),
					All.Num(), kFindBrokenCap);
				return nullptr;
			}

			// Hard-only dependency query (mirrors the Python fallback flags: include_hard=true,
			// include_soft=false). Soft refs by definition may legitimately be missing at editor time.
			UE::AssetRegistry::EDependencyCategory Category = UE::AssetRegistry::EDependencyCategory::Package;
			UE::AssetRegistry::FDependencyQuery Query(UE::AssetRegistry::EDependencyQuery::Hard);

			TArray<TSharedPtr<FJsonValue>> Broken;
			const int32 Total = All.Num();
			for (int32 i = 0; i < Total; ++i)
			{
				if (Job.bCancelRequested.load(std::memory_order_acquire))
				{
					Job.ErrorMessage = TEXT("cancelled");
					return nullptr;
				}
				Job.Progress.store(static_cast<float>(i) / FMath::Max(1, Total),
					std::memory_order_release);

				const FAssetData& Data = All[i];
				const FAssetIdentifier RootId(Data.PackageName);

				TArray<FAssetDependency> Deps;
				IAR.GetDependencies(RootId, Deps, Category, Query);

				TArray<TSharedPtr<FJsonValue>> MissingPaths;
				for (const FAssetDependency& Dep : Deps)
				{
					const FName DepPackage = Dep.AssetId.PackageName;
					if (DepPackage.IsNone()) { continue; }
					const FString DepPackageStr = DepPackage.ToString();
					// /Script/ deps are native classes — always present, never broken.
					if (DepPackageStr.StartsWith(TEXT("/Script/"))) { continue; }

					// Existence check via package-name lookup. GetAssetsByPackageName returns true
					// iff the registry has ANY asset entry for that package name (in-memory OR disk).
					TArray<FAssetData> DepAssets;
					IAR.GetAssetsByPackageName(DepPackage, DepAssets, /*bIncludeOnlyOnDiskAssets*/ false);
					if (DepAssets.Num() == 0)
					{
						MissingPaths.Add(MakeShared<FJsonValueString>(DepPackageStr));
					}
				}

				if (MissingPaths.Num() > 0)
				{
					TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
					Obj->SetStringField(TEXT("asset_path"), Data.GetObjectPathString());
					Obj->SetArrayField(TEXT("missing_paths"), MissingPaths);
					Broken.Add(MakeShared<FJsonValueObject>(Obj));
				}
			}

			Job.Progress.store(1.0f, std::memory_order_release);

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetArrayField(TEXT("broken"), Broken);
			Out->SetNumberField(TEXT("scanned_count"), All.Num());
			return MakeShared<FJsonValueObject>(Out);
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

// ─── asset._find_duplicates_by_name_internal (Hotfix 3 — Lane B, async-job pattern) ──────────
//
// Walks the AR for every asset in scope, groups by short basename (last path segment, stripped of
// class suffix), emits groups with count>1. Game-thread-required because IAR.GetAssets asserts
// off-GT in UE 5.7.
//
// Hotfix 3 rationale: same as Tool_FindBrokenReferencesInternal — Python loopback to asset.list
// per scope deadlocked the composite. Promoted to a single C++ job body.
FMCPResponse Tool_FindDuplicatesByNameInternal(const FMCPRequest& Request)
{
	TArray<FString> Scopes;
	FMCPResponse Err;
	if (!COMP_ParsePackagePaths(Request, Scopes, Err)) { return Err; }

	bool bIgnoreClass = true;
	Request.Args->TryGetBoolField(TEXT("ignore_class"), bIgnoreClass);

	const FGuid JobId = FMCPJobRegistry::Get().SubmitJob(
		TEXT("asset._find_duplicates_by_name_internal"),
		[ScopesCap = MoveTemp(Scopes), bIgnoreClassCap = bIgnoreClass]
		(FMCPJob& Job) -> TSharedPtr<FJsonValue>
		{
			IAssetRegistry& IAR = FAssetRegistryModule::GetRegistry();

			FARFilter Filter;
			Filter.bRecursivePaths = true;
			for (const FString& S : ScopesCap) { Filter.PackagePaths.Add(FName(*S)); }

			TArray<FAssetData> All;
			IAR.GetAssets(Filter, All);

			// Cap matches find_unused — keeps memory and lock-hold time bounded.
			constexpr int32 kFindDupCap = 5000;
			if (All.Num() > kFindDupCap)
			{
				Job.ErrorMessage = FString::Printf(
					TEXT("OVERLY_BROAD_QUERY: scope contains %d candidates (cap=%d) — narrow package_paths"),
					All.Num(), kFindDupCap);
				return nullptr;
			}

			// Group by basename (with optional class suffix). Stores parallel arrays so we can emit
			// stable {name, paths[]} groups after the walk.
			struct FGroupEntry
			{
				FString AssetPath;
				FString ClassPath;
			};
			TMap<FString, TArray<FGroupEntry>> Groups;
			Groups.Reserve(All.Num());

			const int32 Total = All.Num();
			for (int32 i = 0; i < Total; ++i)
			{
				if (Job.bCancelRequested.load(std::memory_order_acquire))
				{
					Job.ErrorMessage = TEXT("cancelled");
					return nullptr;
				}
				if ((i & 0xFF) == 0)
				{
					Job.Progress.store(static_cast<float>(i) / FMath::Max(1, Total),
						std::memory_order_release);
				}

				const FAssetData& Data = All[i];
				const FString AssetPath = Data.GetObjectPathString();
				const FString ClassPath = Data.AssetClassPath.ToString();

				// Basename = leaf after last '/', strip optional '.SubObject' suffix.
				FString Leaf = AssetPath;
				int32 LastSlash = INDEX_NONE;
				if (AssetPath.FindLastChar(TEXT('/'), LastSlash))
				{
					Leaf = AssetPath.Mid(LastSlash + 1);
				}
				int32 LastDot = INDEX_NONE;
				if (Leaf.FindLastChar(TEXT('.'), LastDot))
				{
					Leaf = Leaf.Left(LastDot);
				}

				const FString Key = bIgnoreClassCap
					? Leaf
					: FString::Printf(TEXT("%s::%s"), *Leaf, *ClassPath);

				Groups.FindOrAdd(Key).Add({ AssetPath, ClassPath });
			}

			// Emit groups with >1 entry. Strip the class suffix back off the wire name when present.
			TArray<TSharedPtr<FJsonValue>> Duplicates;
			for (const TPair<FString, TArray<FGroupEntry>>& Pair : Groups)
			{
				if (Pair.Value.Num() <= 1) { continue; }

				FString Name = Pair.Key;
				int32 Sep = INDEX_NONE;
				if (Name.FindChar(TEXT(':'), Sep))
				{
					Name = Name.Left(Sep);
				}

				TArray<TSharedPtr<FJsonValue>> Paths;
				Paths.Reserve(Pair.Value.Num());
				for (const FGroupEntry& E : Pair.Value)
				{
					TSharedRef<FJsonObject> EntryObj = MakeShared<FJsonObject>();
					EntryObj->SetStringField(TEXT("asset_path"), E.AssetPath);
					EntryObj->SetStringField(TEXT("class"),      E.ClassPath);
					Paths.Add(MakeShared<FJsonValueObject>(EntryObj));
				}

				TSharedRef<FJsonObject> GroupObj = MakeShared<FJsonObject>();
				GroupObj->SetStringField(TEXT("name"), Name);
				GroupObj->SetArrayField(TEXT("paths"), Paths);
				Duplicates.Add(MakeShared<FJsonValueObject>(GroupObj));
			}

			Job.Progress.store(1.0f, std::memory_order_release);

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetArrayField(TEXT("duplicates"), Duplicates);
			Out->SetNumberField(TEXT("scanned_count"), All.Num());
			return MakeShared<FJsonValueObject>(Out);
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

	// HOTFIX 3 (2026-05): added 2 more internals (_find_broken_references, _find_duplicates_by_name),
	// converted ALL Python composites to async-only (return {job_id}, no in-composite polling).
	// Previous hotfix-2 design still deadlocked because the composite kept ownership of the game
	// thread while polling its own job.result — even with both endpoints Lane B, the game-thread-
	// required job body couldn't drain. Resolution: composites NEVER poll; AI client polls externally
	// via TCP from off-game-thread. The composite path is now identical to asset.batch_metadata_async
	// which has always worked correctly.
	//
	// All 5 sync handlers are Lane B because the body only does string parsing + a thread-safe
	// registry call; the actual AR work runs in the job body lambda. Job bodies require
	// bGameThreadRequired=true when they call IAR.GetAssets enumeration (find_unused, size_report,
	// find_broken_references, find_duplicates_by_name) — UE 5.7 asserts off-GT. batch_metadata
	// uses bGameThreadRequired=false because it calls only GetAssetByObjectPath (single-point query,
	// thread-safe).
	//
	// MUST be Lane B — the Python composites call these via dispatch_internal (TCP loopback)
	// from inside FMCPPythonEval::CallPythonTool on the game thread. Lane A would queue back to
	// the same game thread that's blocked on socket.recv() → 60s deadlock until socket timeout.
	RegisterTool(TEXT("asset._find_unused_internal"),            &Tool_FindUnusedInternal,            /*Lane B*/ true);
	RegisterTool(TEXT("asset._size_report_internal"),            &Tool_SizeReportInternal,            /*Lane B*/ true);
	RegisterTool(TEXT("asset._batch_metadata_internal"),         &Tool_BatchMetadataInternal,         /*Lane B*/ true);
	RegisterTool(TEXT("asset._find_broken_references_internal"), &Tool_FindBrokenReferencesInternal,  /*Lane B*/ true);
	RegisterTool(TEXT("asset._find_duplicates_by_name_internal"),&Tool_FindDuplicatesByNameInternal,  /*Lane B*/ true);

	UE_LOG(LogMCP, Log,
		TEXT("Phase 2 hotfix 3: registered 5 internal asset.* handlers (all Lane B, all async-job pattern; composites are async-only, return {job_id})"));
}

} // namespace FAssetCompositeTools

MCP_REGISTER_SURFACE(AssetCompositeTools, &FAssetCompositeTools::Register)
