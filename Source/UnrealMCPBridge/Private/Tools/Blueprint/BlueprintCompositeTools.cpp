// Copyright FatumGame. All Rights Reserved.

#include "BlueprintCompositeTools.h"

#include "MCPSurfaceRegistry.h"

#include "FMCPDispatchQueue.h"
#include "FMCPJobRegistry.h"
#include "MCPJsonBuilder.h"
#include "MCPToolHelpers.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPWorldContext.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/ARFilter.h"
#include "Engine/Blueprint.h"
#include "HAL/PlatformTime.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Logging/TokenizedMessage.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	// BPC_ prefix per the unity-build symbol-collision pattern. Per-surface error constants kept;
	// XX_StampIds/MakeError/MakeSuccessObj removed in Phase 3 — use FMCPToolHelpers::Xxx
	// from MCPToolHelpers.h.
	constexpr int32 kBPCErrorInvalidParams = -32602;
	constexpr int32 kBPCErrorInternal      = -32603;

	// Cooperative-cancel cadence: BP compile is heavy (~50ms-2s each); poll every 16 BPs instead of
	// the Phase 3 default 256 used for cheap per-actor mutators. Progress updates every 8 BPs.
	constexpr int32 kBPCCancelCheckEvery   = 16;
	constexpr int32 kBPCProgressUpdateEvery = 8;

	/**
	 * Tokenise a per-BP compile FCompilerResultsLog into the errors[] array of a failure entry.
	 * Warnings are intentionally dropped from the failure entry — they're cosmetic and the
	 * inner result's ``failed[]`` array is for hard-fail diagnostics only.
	 */
	TArray<TSharedPtr<FJsonValue>> BPC_CompileErrorsToJsonArray(const FCompilerResultsLog& Log)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Reserve(Log.NumErrors);
		for (const TSharedRef<FTokenizedMessage>& Msg : Log.Messages)
		{
			if (Msg->GetSeverity() == EMessageSeverity::Error)
			{
				Out.Add(MakeShared<FJsonValueString>(Msg->ToText().ToString()));
			}
		}
		return Out;
	}

	/** Build a per-BP failure entry: { path, errors[] }. */
	TSharedRef<FJsonObject> BPC_MakeFailureEntry(
		const FString& AssetPath,
		const TArray<TSharedPtr<FJsonValue>>& Errors)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("path"), AssetPath);
		Obj->SetArrayField(TEXT("errors"), Errors);
		return Obj;
	}
} // namespace

namespace FBlueprintCompositeTools
{

// ─── bp._compile_all_dirty_internal (Lane B → SubmitJob → game-thread body) ──────────────────
//
// Walks the AssetRegistry filtered by scope_paths + UBlueprint class → loads each → compiles
// each. Per-BP failure does NOT abort the batch (D1 — continue + aggregate).
//
// Args:
//   - scope_paths: [string]  (required, non-empty) — AR PackagePaths filter (recursive).
//                                                    e.g. ["/Game"], ["/Game/MCPTest/Phase4"].
//   - fail_fast?: bool=false  (currently advisory only — body always continues)
//
// Inner result (after job.result on Succeeded):
//   {
//     "compiled":    int,      // total BPs processed (loaded + attempted compile)
//     "succeeded":   int,      // count with Blueprint->Status ∈ {BS_UpToDate, BS_UpToDateWithWarnings}
//     "failed":      [{path, errors[]}],
//     "duration_ms": float
//   }
//
// Error codes (Lane B sync surface):
//   -32602 InvalidParams              — missing/empty scope_paths array (Python wrapper raises
//                                       ValueError which auto-translates to -32602; this is the
//                                       belt-and-braces direct-handler guard).
//   -32016 JobSubmitFailed            — registry initialization failed.
//
// PIE-guard is INSIDE the body: PIE state can transition between submit and execution.
FMCPResponse Tool_CompileAllDirtyInternal(const FMCPRequest& Request)
{
	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kBPCErrorInvalidParams, TEXT("missing args object"));
	}

	const TArray<TSharedPtr<FJsonValue>>* ScopesArr = nullptr;
	if (!Request.Args->TryGetArrayField(TEXT("scope_paths"), ScopesArr) || !ScopesArr || ScopesArr->Num() == 0)
	{
		return FMCPToolHelpers::MakeError(Request, kBPCErrorInvalidParams,
			TEXT("missing or empty required array field 'scope_paths' (e.g. ['/Game'])"));
	}

	// Copy out as plain FStrings — listener-thread-safe.
	TArray<FString> Scopes;
	Scopes.Reserve(ScopesArr->Num());
	for (const TSharedPtr<FJsonValue>& V : *ScopesArr)
	{
		FString S;
		if (V.IsValid() && V->TryGetString(S) && !S.IsEmpty())
		{
			Scopes.Add(S);
		}
	}
	if (Scopes.Num() == 0)
	{
		return FMCPToolHelpers::MakeError(Request, kBPCErrorInvalidParams,
			TEXT("scope_paths must contain at least one non-empty string"));
	}

	bool bFailFast = false;
	Request.Args->TryGetBoolField(TEXT("fail_fast"), bFailFast);

	const FGuid JobIdGuid = FMCPJobRegistry::Get().SubmitJob(
		TEXT("bp._compile_all_dirty_internal"),
		[ScopesCap = MoveTemp(Scopes), bFailFastCap = bFailFast](FMCPJob& Job) -> TSharedPtr<FJsonValue>
		{
			// PIE-guard inside body — PIE state can transition between listener-thread submit and
			// game-thread execution.
			if (FMCPWorldContext::IsPIEActive())
			{
				Job.ErrorMessage = kMCPMessagePIEActive;
				return nullptr;
			}

			const double StartTime = FPlatformTime::Seconds();

			// AR query: PackagePaths ∈ scope, recursive, class = UBlueprint. ClassPaths uses the
			// modern FTopLevelAssetPath form (UE 5.1+); the legacy ClassNames API would surface
			// deprecation warnings.
			IAssetRegistry& IAR = FAssetRegistryModule::GetRegistry();
			FARFilter Filter;
			Filter.bRecursivePaths = true;
			for (const FString& S : ScopesCap)
			{
				Filter.PackagePaths.Add(FName(*S));
			}
			Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());

			TArray<FAssetData> BlueprintAssets;
			IAR.GetAssets(Filter, BlueprintAssets);

			const int32 Total = BlueprintAssets.Num();
			Job.Description = FString::Printf(TEXT("Compile-all-dirty: %d candidate BPs"), Total);

			int32 ProcessedCount = 0;
			int32 SucceededCount = 0;
			TArray<TSharedPtr<FJsonValue>> FailedEntries;
			FailedEntries.Reserve(8);

			for (int32 i = 0; i < Total; ++i)
			{
				// Cooperative cancel — heavier cadence than Phase 3 default (16 vs 256) because
				// each compile is ~50ms-2s instead of a trivial property mutation.
				if ((i % kBPCCancelCheckEvery) == 0)
				{
					if (Job.bCancelRequested.load(std::memory_order_acquire))
					{
						Job.ErrorMessage = TEXT("cancelled");
						return nullptr;
					}
				}
				if ((i % kBPCProgressUpdateEvery) == 0)
				{
					Job.Progress.store(static_cast<float>(i) / FMath::Max(1, Total),
						std::memory_order_release);
					Job.Description = FString::Printf(TEXT("Compile %d/%d"), i + 1, Total);
				}

				const FAssetData& AssetData = BlueprintAssets[i];

				// LoadObject — may trigger BP load chain if not yet in memory. Safe inside GT body.
				UObject* Loaded = AssetData.GetAsset();
				UBlueprint* Blueprint = Cast<UBlueprint>(Loaded);
				if (!Blueprint)
				{
					FailedEntries.Add(MakeShared<FJsonValueObject>(BPC_MakeFailureEntry(
						AssetData.GetObjectPathString(),
						{ MakeShared<FJsonValueString>(TEXT("LoadObject returned null or non-UBlueprint")) })));
					++ProcessedCount;
					if (bFailFastCap) { break; }
					continue;
				}

				++ProcessedCount;

				FCompilerResultsLog ResultsLog;
				ResultsLog.bSilentMode = true;
				ResultsLog.SetSourcePath(AssetData.GetObjectPathString());

				FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &ResultsLog);

				const EBlueprintStatus Status = Blueprint->Status;
				const bool bOk = (Status == BS_UpToDate || Status == BS_UpToDateWithWarnings);
				if (bOk)
				{
					++SucceededCount;
				}
				else
				{
					FailedEntries.Add(MakeShared<FJsonValueObject>(BPC_MakeFailureEntry(
						AssetData.GetObjectPathString(),
						BPC_CompileErrorsToJsonArray(ResultsLog))));
					if (bFailFastCap) { break; }
				}
			}

			Job.Progress.store(1.0f, std::memory_order_release);

			const double DurationMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetNumberField(TEXT("compiled"),    static_cast<double>(ProcessedCount));
			Out->SetNumberField(TEXT("succeeded"),   static_cast<double>(SucceededCount));
			Out->SetArrayField(TEXT("failed"),       FailedEntries);
			Out->SetNumberField(TEXT("duration_ms"), DurationMs);
			return MakeShared<FJsonValueObject>(Out);
		},
		/*bGameThreadRequired*/ true);

	if (!JobIdGuid.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorJobSubmitFailed,
			TEXT("FMCPJobRegistry::SubmitJob refused (shutdown?)"));
	}

	return FMCPJsonBuilder()
		.Str(TEXT("job_id"), JobIdGuid.ToString(EGuidFormats::DigitsWithHyphens))
		.BuildSuccess(Request);
}

// ─── Registration ────────────────────────────────────────────────────────────────────────────
//
// MUST be Lane B — the Python composite calls this via dispatch_internal (TCP loopback) from
// inside FMCPPythonEval::CallPythonTool on the game thread. Lane A would queue back to the same
// game thread that's blocked on socket.recv() → 60s deadlock until socket timeout. Same rationale
// as the Phase 2/3 internal composite handlers.
void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	// Day 10: bp.compile_all_dirty backing internal (async composite).
	RegisterTool(TEXT("bp._compile_all_dirty_internal"), &Tool_CompileAllDirtyInternal, /*Lane B*/ true);

	UE_LOG(LogMCP, Log,
		TEXT("Phase 4 Day 10: registered 1 internal composite handler (bp._compile_all_dirty_internal, Lane B); ")
		TEXT("Python wrapper bp.compile_all_dirty in blueprint_composites.py"));
}

} // namespace FBlueprintCompositeTools

MCP_REGISTER_SURFACE(BlueprintCompositeTools, &FBlueprintCompositeTools::Register)
