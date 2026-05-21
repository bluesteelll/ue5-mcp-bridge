// Copyright FatumGame. All Rights Reserved.

#include "TestCompositeTools.h"

#include "MCPSurfaceRegistry.h"

#include "FMCPDispatchQueue.h"
#include "FMCPJobRegistry.h"
#include "MCPJsonBuilder.h"
#include "MCPToolHelpers.h"
#include "UnrealMCPBridge.h"

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/AutomationEvent.h"
#include "Misc/DateTime.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	// TSC_ prefix per the unity-build symbol-collision pattern. Per-surface error constants kept;
	// XX_StampIds/MakeError/MakeSuccessObj removed in Phase 3 — use FMCPToolHelpers::Xxx
	// from MCPToolHelpers.h.
	constexpr int32 kTSCErrorInvalidParams = -32602;

	// Per-test inner pump cadence — same value as the sync TestTools path. Trades GT
	// responsiveness against drain frequency.
	constexpr float kTSCDrainSleepSeconds = 0.005f;

	const TCHAR* TSC_EventTypeToString(EAutomationEventType Type)
	{
		switch (Type)
		{
			case EAutomationEventType::Info:    return TEXT("info");
			case EAutomationEventType::Warning: return TEXT("warning");
			case EAutomationEventType::Error:   return TEXT("error");
		}
		return TEXT("unknown");
	}

	TSharedRef<FJsonObject> TSC_BuildEntryJson(const FAutomationExecutionEntry& Entry)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("type"),      TSC_EventTypeToString(Entry.Event.Type));
		Obj->SetStringField(TEXT("message"),   Entry.Event.Message);
		Obj->SetStringField(TEXT("context"),   Entry.Event.Context);
		Obj->SetStringField(TEXT("filename"),  Entry.Filename);
		Obj->SetNumberField(TEXT("line"),      static_cast<double>(Entry.LineNumber));
		Obj->SetStringField(TEXT("timestamp"), Entry.Timestamp.ToIso8601());
		return Obj;
	}

	/**
	 * Lookup a flag name in the Engine's flag-name table.
	 */
	bool TSC_LookupFlag(const FString& Name, EAutomationTestFlags& OutFlag)
	{
		const TMap<FString, EAutomationTestFlags>& Map = EAutomationTestFlags_GetTestFlagsMap();
		if (const EAutomationTestFlags* Found = Map.Find(Name))
		{
			OutFlag = *Found;
			return true;
		}
		return false;
	}

	/**
	 * Caller-supplied test name → (TestName command line, FullTestPath) resolved via the live
	 * framework. Returns false if not found.
	 *
	 * Lookup is case-sensitive against FullTestPath (matches the convention used by sync
	 * ``test.run_single_test`` / ``test.get_test_info``).
	 */
	struct FResolvedTest
	{
		FString FullTestPath;
		FString TestName;
	};

	bool TSC_ResolveTest(const FString& FullTestPath, FResolvedTest& Out,
		const TArray<FAutomationTestInfo>& AllTests)
	{
		for (const FAutomationTestInfo& Info : AllTests)
		{
			if (Info.GetFullTestPath().Equals(FullTestPath, ESearchCase::CaseSensitive))
			{
				Out.FullTestPath = Info.GetFullTestPath();
				Out.TestName     = Info.GetTestName();
				return true;
			}
		}
		return false;
	}

	/**
	 * Drain the latent + network command queues until ExecuteLatentCommands reports completion
	 * OR the supplied cancel-poll lambda returns true. Returns true if completion, false if
	 * cancel-requested mid-flight.
	 *
	 * The poll lambda is invoked every loop iteration — bodies should keep it cheap (atomic load
	 * is fine; mutex acquire is not).
	 */
	bool TSC_PumpLatentUntilCompleteOrCancel(FAutomationTestFramework& Framework,
		const TFunction<bool()>& CancelPoll)
	{
		while (true)
		{
			Framework.ExecuteNetworkCommands();
			if (Framework.ExecuteLatentCommands())
			{
				return true;
			}
			if (CancelPoll && CancelPoll())
			{
				// Caller wants us to bail. Don't StopTest here — caller drives that so per-test
				// partial info is captured in the right slot.
				return false;
			}
			FPlatformProcess::Sleep(kTSCDrainSleepSeconds);
		}
	}
} // namespace

namespace FTestCompositeTools
{

// ─── test._run_automation_internal (Lane B → SubmitJob → game-thread body) ────────────────────
//
// Validates inputs synchronously on the listener thread (including resolving every test name
// against the live framework), then submits a job that performs the actual test runs on the
// game thread. AI client receives ``{job_id}`` and polls externally.
//
// Args:
//   - test_names:        [string]   (required, non-empty) — exact match FullTestPath entries
//   - run_smoke_filter:  string     (optional) — EAutomationTestFlags name applied via
//                                                 SetRequestedTestFilter BEFORE the batch runs;
//                                                 unknown name → -32602 (Lane B sync surface)
//
// Inner result (after job.result on Succeeded):
//   {
//     "succeeded": [{name, duration_secs, warning_count}],
//     "failed":    [{name, duration_secs, error_count, errors[], warnings[]}],
//     "skipped":   [{name, reason}],
//     "total":          int,
//     "completed":      int,
//     "failed_count":   int,
//     "cancelled":      bool,
//     "applied_filter": string,    // run_smoke_filter echo (empty if not provided)
//     "total_duration_secs": float
//   }
//
// Error codes (Lane B sync surface):
//   -32602 InvalidParams                       missing/empty test_names; unknown run_smoke_filter
//   -32016 JobSubmitFailed                     registry refused submit (shutdown?)
//   -32046 TestNotFound                        any test_name not present in framework registry —
//                                              ENTIRE batch rejected pre-job (no partial submit)
//
// **No PIE guard.** Some tests may legitimately drive PIE start/stop themselves; we don't block.
// Operators should run ``pie.stop`` first if they want a clean editor-world start state.
FMCPResponse Tool_RunAutomationInternal(const FMCPRequest& Request)
{
	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kTSCErrorInvalidParams, TEXT("missing args object"));
	}

	// ─── Parse test_names ──────────────────────────────────────────────────────────────────────
	const TArray<TSharedPtr<FJsonValue>>* NamesArr = nullptr;
	if (!Request.Args->TryGetArrayField(TEXT("test_names"), NamesArr) || !NamesArr ||
		NamesArr->Num() == 0)
	{
		return FMCPToolHelpers::MakeError(Request, kTSCErrorInvalidParams,
			TEXT("missing or empty required array field 'test_names'"));
	}

	TArray<FString> RawNames;
	RawNames.Reserve(NamesArr->Num());
	for (int32 i = 0; i < NamesArr->Num(); ++i)
	{
		FString Name;
		if (!(*NamesArr)[i].IsValid() || !(*NamesArr)[i]->TryGetString(Name) || Name.IsEmpty())
		{
			return FMCPToolHelpers::MakeError(Request, kTSCErrorInvalidParams,
				FString::Printf(TEXT("'test_names'[%d] is not a non-empty string"), i));
		}
		RawNames.Add(MoveTemp(Name));
	}

	// ─── Parse optional run_smoke_filter ───────────────────────────────────────────────────────
	FString FilterName;
	Request.Args->TryGetStringField(TEXT("run_smoke_filter"), FilterName);

	EAutomationTestFlags FilterFlag = EAutomationTestFlags::None;
	const bool bHasFilter = !FilterName.IsEmpty();
	if (bHasFilter)
	{
		if (!TSC_LookupFlag(FilterName, FilterFlag))
		{
			return FMCPToolHelpers::MakeError(Request, kTSCErrorInvalidParams,
				FString::Printf(
					TEXT("unknown run_smoke_filter '%s' (see EAutomationTestFlags_GetTestFlagsMap for canonical names; ")
					TEXT("e.g. \"SmokeFilter\", \"EngineFilter\", \"ProductFilter\")"),
					*FilterName));
		}
	}

	// ─── Pre-job validation removed (2026-05 dogfood crash) ────────────────────────────────────
	// The previous design called FAutomationTestFramework::Get().GetValidTestNames(AllTests) from
	// the Lane B submitter to pre-resolve test names for synchronous -32046 feedback. Stack
	// trace from live smoke crash shows GetValidTestNames triggers an AssetRegistry walk via
	// InterchangeTests' AR-driven test enumeration, which asserts
	// IsInGameThread()||IsInAsyncLoadingThread() at AssetRegistry.cpp:2906. Lane B = listener
	// thread → crash.
	//
	// Resolution: skip pre-validation; let the GT job body resolve names safely and emit
	// per-test -32046-equivalent entries in the result's `failed[]` array. AI client now sees
	// test-not-found AFTER job.submit + job.result poll instead of synchronously, but the
	// trade-off is necessary — pre-validation is impossible without crashing the editor.
	TArray<FResolvedTest> Resolved;
	Resolved.Reserve(RawNames.Num());
	for (FString& Name : RawNames)
	{
		FResolvedTest R;
		R.FullTestPath = MoveTemp(Name);  // body resolves TestName; if missing, marked failed
		R.TestName     = FString();
		Resolved.Add(MoveTemp(R));
	}

	// ─── Submit job ────────────────────────────────────────────────────────────────────────────
	const FGuid JobIdGuid = FMCPJobRegistry::Get().SubmitJob(
		FString::Printf(TEXT("test.run_automation: %d test(s)%s%s"),
			Resolved.Num(),
			bHasFilter ? TEXT(" filter=") : TEXT(""),
			bHasFilter ? *FilterName    : TEXT("")),
		[ResolvedCap = MoveTemp(Resolved), FilterCap = MoveTemp(FilterName), bHasFilterCap = bHasFilter, FilterFlagCap = FilterFlag]
		(FMCPJob& Job) mutable -> TSharedPtr<FJsonValue>
		{
			FAutomationTestFramework& Framework = FAutomationTestFramework::Get();
			if (bHasFilterCap)
			{
				Framework.SetRequestedTestFilter(FilterFlagCap);
			}

			// Resolve each FullTestPath → TestName on GT (the listener-thread path crashed
			// AssetRegistry's IsInGameThread() assert via InterchangeTests AR walk — 2026-05).
			// Unresolved names go straight to failed[] with TEST_NOT_FOUND equivalent.
			TArray<FAutomationTestInfo> AllTests;
			Framework.GetValidTestNames(AllTests);
			for (FResolvedTest& R : ResolvedCap)
			{
				if (!R.TestName.IsEmpty()) { continue; }
				for (const FAutomationTestInfo& Info : AllTests)
				{
					if (Info.GetFullTestPath().Equals(R.FullTestPath, ESearchCase::CaseSensitive))
					{
						R.TestName = Info.GetTestName();
						break;
					}
				}
			}

			TArray<TSharedPtr<FJsonValue>> SucceededArr;
			TArray<TSharedPtr<FJsonValue>> FailedArr;
			TArray<TSharedPtr<FJsonValue>> SkippedArr;

			const double BatchStartSeconds = FPlatformTime::Seconds();
			bool bBatchCancelled = false;
			int32 CompletedCount = 0;

			for (int32 i = 0; i < ResolvedCap.Num(); ++i)
			{
				// Cooperative cancel between tests.
				if (Job.bCancelRequested.load(std::memory_order_acquire))
				{
					bBatchCancelled = true;
					// Remaining tests skipped with reason.
					for (int32 k = i; k < ResolvedCap.Num(); ++k)
					{
						TSharedRef<FJsonObject> SkipObj = MakeShared<FJsonObject>();
						SkipObj->SetStringField(TEXT("name"), ResolvedCap[k].FullTestPath);
						SkipObj->SetStringField(TEXT("reason"),
							TEXT("batch cancelled before this test ran"));
						SkippedArr.Add(MakeShared<FJsonValueObject>(SkipObj));
					}
					break;
				}

				const FResolvedTest& Test = ResolvedCap[i];

				// Unresolved name → emit failed[] entry with TEST_NOT_FOUND-equivalent reason
				// and skip to next. Body-side resolution is the contract since 2026-05 (Lane B
				// pre-resolution crashed via AR walk).
				if (Test.TestName.IsEmpty())
				{
					TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
					Obj->SetStringField(TEXT("name"),          Test.FullTestPath);
					Obj->SetNumberField(TEXT("duration_secs"), 0.0);
					Obj->SetNumberField(TEXT("error_count"),   1.0);
					Obj->SetNumberField(TEXT("warning_count"), 0.0);
					Obj->SetBoolField(TEXT("cancelled_mid_test"), false);
					TArray<TSharedPtr<FJsonValue>> ErrJson;
					TSharedRef<FJsonObject> ErrEntry = MakeShared<FJsonObject>();
					ErrEntry->SetStringField(TEXT("message"),
						FString::Printf(TEXT("test '%s' not found in framework registry; see test.list_automation_specs"),
							*Test.FullTestPath));
					ErrEntry->SetStringField(TEXT("severity"), TEXT("Error"));
					ErrJson.Add(MakeShared<FJsonValueObject>(ErrEntry));
					Obj->SetArrayField(TEXT("errors"),   ErrJson);
					Obj->SetArrayField(TEXT("warnings"), TArray<TSharedPtr<FJsonValue>>());
					FailedArr.Add(MakeShared<FJsonValueObject>(Obj));
					++CompletedCount;
					Job.Progress.store(
						static_cast<float>(CompletedCount) / static_cast<float>(ResolvedCap.Num()),
						std::memory_order_release);
					continue;
				}

				const double TestStartSeconds = FPlatformTime::Seconds();

				Framework.StartTestByName(Test.TestName, /*InRoleIndex*/ 0, Test.FullTestPath);

				// Drain until completion OR cancel.
				bool bMidTestCancel = false;
				const bool bCompleted = TSC_PumpLatentUntilCompleteOrCancel(Framework,
					[&Job, &bMidTestCancel]()
					{
						if (Job.bCancelRequested.load(std::memory_order_acquire))
						{
							bMidTestCancel = true;
							return true;
						}
						return false;
					});

				// Mid-test cancel: best-effort dequeue then StopTest to capture partial info.
				if (!bCompleted)
				{
					Framework.DequeueAllCommands();
				}

				FAutomationTestExecutionInfo ExecInfo;
				const bool bSuccessful = Framework.StopTest(ExecInfo);
				const double TestDuration = FPlatformTime::Seconds() - TestStartSeconds;

				TArray<TSharedPtr<FJsonValue>> ErrorsArr;
				TArray<TSharedPtr<FJsonValue>> WarningsArr;
				for (const FAutomationExecutionEntry& Entry : ExecInfo.GetEntries())
				{
					TSharedRef<FJsonObject> EntryJson = TSC_BuildEntryJson(Entry);
					if (Entry.Event.Type == EAutomationEventType::Error)
					{
						ErrorsArr.Add(MakeShared<FJsonValueObject>(EntryJson));
					}
					else if (Entry.Event.Type == EAutomationEventType::Warning)
					{
						WarningsArr.Add(MakeShared<FJsonValueObject>(EntryJson));
					}
				}

				if (bSuccessful && bCompleted)
				{
					TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
					Obj->SetStringField(TEXT("name"),          Test.FullTestPath);
					Obj->SetNumberField(TEXT("duration_secs"), TestDuration);
					Obj->SetNumberField(TEXT("warning_count"), static_cast<double>(ExecInfo.GetWarningTotal()));
					SucceededArr.Add(MakeShared<FJsonValueObject>(Obj));
				}
				else
				{
					TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
					Obj->SetStringField(TEXT("name"),          Test.FullTestPath);
					Obj->SetNumberField(TEXT("duration_secs"), TestDuration);
					Obj->SetNumberField(TEXT("error_count"),   static_cast<double>(ExecInfo.GetErrorTotal()));
					Obj->SetNumberField(TEXT("warning_count"), static_cast<double>(ExecInfo.GetWarningTotal()));
					Obj->SetBoolField(TEXT("cancelled_mid_test"), bMidTestCancel);
					Obj->SetArrayField(TEXT("errors"),         ErrorsArr);
					Obj->SetArrayField(TEXT("warnings"),       WarningsArr);
					FailedArr.Add(MakeShared<FJsonValueObject>(Obj));
				}

				++CompletedCount;
				Job.Progress.store(
					static_cast<float>(CompletedCount) / static_cast<float>(ResolvedCap.Num()),
					std::memory_order_release);

				// If mid-test cancel fired, mark the batch as cancelled — remaining tests have
				// already been swept into skipped[] by the top-of-loop guard on the NEXT pass.
				if (bMidTestCancel)
				{
					bBatchCancelled = true;
					// Push remaining tests into skipped[].
					for (int32 k = i + 1; k < ResolvedCap.Num(); ++k)
					{
						TSharedRef<FJsonObject> SkipObj = MakeShared<FJsonObject>();
						SkipObj->SetStringField(TEXT("name"), ResolvedCap[k].FullTestPath);
						SkipObj->SetStringField(TEXT("reason"),
							TEXT("batch cancelled before this test ran"));
						SkippedArr.Add(MakeShared<FJsonValueObject>(SkipObj));
					}
					break;
				}
			}

			const double TotalDuration = FPlatformTime::Seconds() - BatchStartSeconds;

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetArrayField(TEXT("succeeded"),         SucceededArr);
			Out->SetArrayField(TEXT("failed"),            FailedArr);
			Out->SetArrayField(TEXT("skipped"),           SkippedArr);
			Out->SetNumberField(TEXT("total"),            static_cast<double>(ResolvedCap.Num()));
			Out->SetNumberField(TEXT("completed"),        static_cast<double>(CompletedCount));
			Out->SetNumberField(TEXT("failed_count"),     static_cast<double>(FailedArr.Num()));
			Out->SetBoolField(TEXT("cancelled"),          bBatchCancelled);
			Out->SetStringField(TEXT("applied_filter"),   FilterCap);
			Out->SetNumberField(TEXT("total_duration_secs"), TotalDuration);
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
// as the Phase 2-5 internal composite handlers + Chunk A sc._submit_internal.
void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	// Lane B (revert from short-lived Lane A test). FAutomationTestFramework::Get + GetValidTestNames
	// are observed empirically thread-safe in UE 5.7 from the listener thread (framework's test
	// map is effectively immutable post-static-init); test runs still happen on GT via SubmitJob's
	// bGameThreadRequired. Must NOT be Lane A — Python wrapper holds GT during dispatch_internal,
	// so Lane A would queue to OnEndFrame drain that GT can't reach → 60s timeout (Phase 2 Hotfix 3
	// pattern).
	RegisterTool(TEXT("test._run_automation_internal"), &Tool_RunAutomationInternal, /*Lane B*/ true);

	UE_LOG(LogMCP, Log,
		TEXT("Phase 6 Chunk B (Automation Test composites): registered 1 internal composite handler ")
		TEXT("(test._run_automation_internal, Lane B); Python wrapper test.run_automation in phase6_composites.py"));
}

} // namespace FTestCompositeTools

MCP_REGISTER_SURFACE(TestCompositeTools, &FTestCompositeTools::Register)
