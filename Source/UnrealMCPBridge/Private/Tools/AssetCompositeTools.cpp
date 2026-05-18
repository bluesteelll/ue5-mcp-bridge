// Copyright FatumGame. All Rights Reserved.

#include "AssetCompositeTools.h"

#include "FMCPDispatchQueue.h"
#include "FMCPJobRegistry.h"
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
	// COMP_ prefix per the unity-build symbol-collision pattern.
	constexpr int32 kCOMPErrorInvalidParams = -32602;
	constexpr int32 kCOMPErrorInternal      = -32603;

	void COMP_StampIds(const FMCPRequest& Request, FMCPResponse& Response)
	{
		Response.RequestId = Request.RequestId;
		Response.OriginalIdString = Request.OriginalIdString;
	}

	FMCPResponse COMP_MakeError(const FMCPRequest& Request, int32 Code, const FString& Message)
	{
		FMCPResponse R;
		COMP_StampIds(Request, R);
		R.bIsError = true;
		R.ErrorCode = Code;
		R.ErrorMessage = Message;
		return R;
	}

	FMCPResponse COMP_MakeSuccessObj(const FMCPRequest& Request, TSharedPtr<FJsonObject> Result)
	{
		FMCPResponse R;
		COMP_StampIds(Request, R);
		R.bIsError = false;
		R.Result = MakeShared<FJsonValueObject>(MoveTemp(Result));
		return R;
	}

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
			OutError = COMP_MakeError(Request, kCOMPErrorInvalidParams, TEXT("missing args object"));
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Request.Args->TryGetArrayField(TEXT("package_paths"), Arr) || Arr == nullptr || Arr->Num() == 0)
		{
			OutError = COMP_MakeError(Request, kCOMPErrorInvalidParams,
				TEXT("missing or empty required array field 'package_paths'"));
			return false;
		}
		OutNormalized.Reserve(Arr->Num());
		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			FString S;
			if (!V.IsValid() || !V->TryGetString(S))
			{
				OutError = COMP_MakeError(Request, kCOMPErrorInvalidParams,
					TEXT("package_paths: expected array of strings"));
				return false;
			}
			const FString Norm = FMCPAssetPathUtils::Normalize(S);
			if (Norm.IsEmpty())
			{
				OutError = COMP_MakeError(Request, kMCPErrorInvalidPath,
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

// ─── asset._find_unused_internal (Lane B) ────────────────────────────────────────────────────
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

	// Build a single AR query spanning all scopes (recursive).
	IAssetRegistry& IAR = FAssetRegistryModule::GetRegistry();
	FARFilter Filter;
	Filter.bRecursivePaths = true;
	for (const FString& S : Scopes) { Filter.PackagePaths.Add(FName(*S)); }

	TArray<FAssetData> Candidates;
	IAR.GetAssets(Filter, Candidates);

	// Hard cap per the plan — 5000 entries.
	constexpr int32 kFindUnusedCap = 5000;
	if (Candidates.Num() > kFindUnusedCap)
	{
		return COMP_MakeError(Request, kMCPErrorOverlyBroadQuery,
			FString::Printf(TEXT("scope contains %d candidates (cap=%d) — narrow package_paths"),
				Candidates.Num(), kFindUnusedCap));
	}

	TArray<TSharedPtr<FJsonValue>> Unused;
	Unused.Reserve(Candidates.Num());

	for (const FAssetData& Data : Candidates)
	{
		// Skip excluded class kinds. Compare against the AssetClassPath string for portability
		// (the user passes "/Script/Engine.World" etc.).
		if (ExcludeClasses.Contains(Data.AssetClassPath.ToString()))
		{
			continue;
		}

		// Look up referencers via package-level GetReferencers (Lane B-safe). Empty result =
		// unused per static analysis. The well-known caveat about runtime LoadClass / construction
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

	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetArrayField(TEXT("unused"), Unused);
	Out->SetNumberField(TEXT("scanned_count"), Candidates.Num());
	return COMP_MakeSuccessObj(Request, Out);
}

// ─── asset._size_report_internal (Lane B) ────────────────────────────────────────────────────
FMCPResponse Tool_SizeReportInternal(const FMCPRequest& Request)
{
	TArray<FString> Scopes;
	FMCPResponse Err;
	if (!COMP_ParsePackagePaths(Request, Scopes, Err)) { return Err; }

	int32 TopN = 50;
	Request.Args->TryGetNumberField(TEXT("top_n"), TopN);
	TopN = FMath::Clamp(TopN, 1, 1000);

	IAssetRegistry& IAR = FAssetRegistryModule::GetRegistry();
	FARFilter Filter;
	Filter.bRecursivePaths = true;
	for (const FString& S : Scopes) { Filter.PackagePaths.Add(FName(*S)); }

	TArray<FAssetData> All;
	IAR.GetAssets(Filter, All);

	// Internal cap: 50,000 assets per call. Larger projects can still be analysed via per-folder
	// subqueries; the cap is a defence against a runaway full-/Game/ scan.
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
	for (int32 i = 0; i < All.Num(); ++i)
	{
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
	const int32 Limit = FMath::Min(TopN, Entries.Num());
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

	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetArrayField(TEXT("top"), Top);
	Out->SetNumberField(TEXT("total_bytes"), static_cast<double>(TotalBytes));
	return COMP_MakeSuccessObj(Request, Out);
}

// ─── asset._batch_metadata_internal (Lane A submit — body runs Lane B-safe on worker) ──────
FMCPResponse Tool_BatchMetadataInternal(const FMCPRequest& Request)
{
	if (!Request.Args.IsValid())
	{
		return COMP_MakeError(Request, kCOMPErrorInvalidParams, TEXT("missing args object"));
	}

	const TArray<TSharedPtr<FJsonValue>>* PathsPtr = nullptr;
	if (!Request.Args->TryGetArrayField(TEXT("paths"), PathsPtr) || PathsPtr == nullptr || PathsPtr->Num() == 0)
	{
		return COMP_MakeError(Request, kCOMPErrorInvalidParams,
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
			return COMP_MakeError(Request, kCOMPErrorInvalidParams,
				TEXT("paths: expected array of strings"));
		}
		const FString Norm = FMCPAssetPathUtils::Normalize(S);
		if (Norm.IsEmpty())
		{
			return COMP_MakeError(Request, kMCPErrorInvalidPath,
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
		return COMP_MakeError(Request, kMCPErrorJobSubmitFailed,
			TEXT("FMCPJobRegistry::SubmitJob refused (shutdown?)"));
	}

	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("job_id"), JobId.ToString(EGuidFormats::DigitsWithHyphens));
	return COMP_MakeSuccessObj(Request, Out);
}

// ─── Registration ────────────────────────────────────────────────────────────────────────────
void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	// _find_unused_internal: pure AR walk + GetReferencers — Lane B-safe.
	RegisterTool(TEXT("asset._find_unused_internal"),  &Tool_FindUnusedInternal,    /*Lane B*/ true);
	// _size_report_internal: AR walk + FileSize — Lane B-safe.
	RegisterTool(TEXT("asset._size_report_internal"),  &Tool_SizeReportInternal,    /*Lane B*/ true);
	// _batch_metadata_internal: submits an async job, returns {job_id}.
	// MUST be Lane B — the Python composite asset.batch_metadata_async calls this via
	// dispatch_internal (TCP loopback), which itself runs inside FMCPPythonEval::CallPythonTool
	// on the game thread. If this handler were Lane A it would queue back to the same game
	// thread that's blocked on socket.recv() → 60s deadlock until socket timeout.
	// Lane B safety: handler body only does (a) AR string ops to validate paths and
	// (b) FMCPJobRegistry::SubmitJob, which is thread-safe (FScopeLock(JobsLock), no game-thread
	// assert). The submitted lambda runs on the worker pool — never on this handler thread.
	RegisterTool(TEXT("asset._batch_metadata_internal"), &Tool_BatchMetadataInternal, /*Lane B*/ true);

	UE_LOG(LogMCP, Log,
		TEXT("Phase 2 Day 11: registered 3 internal asset.* handlers (hidden from tools.list)"));
}

} // namespace FAssetCompositeTools
