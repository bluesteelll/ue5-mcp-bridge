// Copyright FatumGame. All Rights Reserved.

#include "LiveCodingTools.h"

#include "MCPSurfaceRegistry.h"

#include "FMCPDispatchQueue.h"
#include "FMCPJobRegistry.h"
#include "FMCPLogStream.h"
#include "MCPJsonBuilder.h"
#include "MCPToolHelpers.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPWorldContext.h"

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Modules/ModuleManager.h"

#if PLATFORM_WINDOWS
#include "ILiveCodingModule.h"
#endif

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// LIV_ prefix per the unity-build symbol-collision pattern. Per-surface error constants kept;
	// XX_StampIds/MakeError/MakeSuccessObj removed in Phase 3 — use FMCPToolHelpers::Xxx
	// from MCPToolHelpers.h.
	constexpr int32 kLIVErrorInvalidParams = -32602;
	constexpr int32 kLIVErrorInternal      = -32603;

	// Maximum wall-clock seconds the recompile job will wait for the compile to drain. Live Coding
	// compiles for typical FatumGame-sized modules run 3-15 seconds; the cap accommodates large
	// changesets (50+ patched files) while preventing runaway hangs on broken builds. Past this
	// cap we return ``wait_timeout_hit=true`` + the last-known ``result`` (likely InProgress) so
	// the caller can decide whether to wait further or give up.
	constexpr double kLIVMaxRecompileWaitSecs = 180.0;

	// Poll cadence inside the worker body. 100 ms is a balance between responsiveness (cancel
	// detection latency) and CPU overhead (FPlatformProcess::Sleep is a hard yield).
	constexpr double kLIVPollIntervalSecs = 0.1;

#if PLATFORM_WINDOWS
	/**
	 * Map ELiveCodingCompileResult to its canonical string for the response. Matches the enum
	 * names in ILiveCodingModule.h verbatim — caller can pattern-match without ambiguity.
	 */
	const TCHAR* LIV_CompileResultToString(ELiveCodingCompileResult R)
	{
		switch (R)
		{
			case ELiveCodingCompileResult::Success:             return TEXT("Success");
			case ELiveCodingCompileResult::NoChanges:           return TEXT("NoChanges");
			case ELiveCodingCompileResult::InProgress:          return TEXT("InProgress");
			case ELiveCodingCompileResult::CompileStillActive:  return TEXT("CompileStillActive");
			case ELiveCodingCompileResult::NotStarted:          return TEXT("NotStarted");
			case ELiveCodingCompileResult::Failure:             return TEXT("Failure");
			case ELiveCodingCompileResult::Cancelled:           return TEXT("Cancelled");
			default:                                            return TEXT("Unknown");
		}
	}

	/**
	 * Resolve the Live Coding module from FModuleManager. Returns nullptr if the module cannot be
	 * loaded (not installed, not in this build configuration, platform doesn't support, etc.).
	 *
	 * Calls FModuleManager::Get().LoadModule() — if the module exists on disk this loads it; if
	 * already loaded returns the cached instance. On Windows desktop editor builds the module is
	 * always available; on other platforms / build configs ``ModuleExists`` short-circuits.
	 */
	ILiveCodingModule* LIV_GetModule()
	{
		FModuleManager& Mgr = FModuleManager::Get();
		const FName ModuleName(TEXT(LIVE_CODING_MODULE_NAME));
		if (!Mgr.ModuleExists(*ModuleName.ToString()))
		{
			return nullptr;
		}
		// LoadModulePtr returns nullptr if the module's StartupModule fails or the module isn't
		// registered. IModuleInterface* → ILiveCodingModule* cast via dynamic IModuleInterface
		// downcast (LiveCoding inherits from IModuleInterface — runtime safe).
		IModuleInterface* Interface = Mgr.LoadModule(ModuleName);
		return static_cast<ILiveCodingModule*>(Interface);
	}
#endif // PLATFORM_WINDOWS
} // namespace

namespace FLiveCodingTools
{

// ─── livecoding._recompile_internal (Lane B → SubmitJob → game-thread body) ─────────────────────
//
// Validates inputs synchronously on the listener thread (Lane B), then submits a job that
// performs the actual ``ILiveCodingModule::Compile()`` on the game thread. AI client receives
// ``{job_id}`` and polls externally.
//
// Args (forwarded from Python wrapper after its validation):
//   - modules: [string]   (required, non-empty) — UE module names OR ["*"] for all dirty
//
// Inner result (after job.result on Succeeded):
//   {
//     "recompiled":          bool,
//     "result":              string  (enum name — Success/NoChanges/Failure/etc.),
//     "patched_modules":     [string]              (best-effort, log-scraped),
//     "failed_modules":      [{name, errors[]}]    (best-effort, log-scraped),
//     "duration_secs":       float,
//     "wait_timeout_hit":    bool,
//     "live_coding_version": string                (LIVE_CODING_MODULE_NAME echo)
//   }
//
// Error codes (Lane B sync surface):
//   -32602 InvalidParams        missing/empty modules array
//   -32016 JobSubmitFailed      registry initialization failed
//   -32027 PIEActive            GEditor->PlayWorld != nullptr (LC requires PIE off; D11)
//   -32048 LiveCodingDisabled   module unavailable in this build config / platform
//
// PIE + Live Coding availability gates fire SYNCHRONOUSLY at submit time so callers don't get a
// job_id for a doomed request. Re-checks happen inside the body in case state transitions
// between submit + execution.
FMCPResponse Tool_RecompileInternal(const FMCPRequest& Request)
{
	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kLIVErrorInvalidParams, TEXT("missing args object"));
	}

	// ─── Validate modules array ───────────────────────────────────────────────────────────────
	const TArray<TSharedPtr<FJsonValue>>* ModulesArr = nullptr;
	if (!Request.Args->TryGetArrayField(TEXT("modules"), ModulesArr) || !ModulesArr ||
		ModulesArr->Num() == 0)
	{
		return FMCPToolHelpers::MakeError(Request, kLIVErrorInvalidParams,
			TEXT("missing or empty required array field 'modules' (UE module names, or [\"*\"] for all)"));
	}

	TArray<FString> ModuleNames;
	ModuleNames.Reserve(ModulesArr->Num());
	for (int32 i = 0; i < ModulesArr->Num(); ++i)
	{
		FString Name;
		if (!(*ModulesArr)[i].IsValid() || !(*ModulesArr)[i]->TryGetString(Name) || Name.IsEmpty())
		{
			return FMCPToolHelpers::MakeError(Request, kLIVErrorInvalidParams,
				FString::Printf(TEXT("'modules'[%d] is not a non-empty string"), i));
		}
		ModuleNames.Add(MoveTemp(Name));
	}

	// ─── PIE guard moved INTO body ─────────────────────────────────────────────────────────────
	// Lane B submitter MUST NOT call FMCPWorldContext::IsPIEActive (asserts IsInGameThread).
	// The body-level re-check at line ~218 covers the actual PIE-running case; submitter just
	// skips this pre-check to avoid the thread-mismatch crash.

	// ─── Live Coding availability gate (-32048) ────────────────────────────────────────────────
#if PLATFORM_WINDOWS
	{
		ILiveCodingModule* LC = LIV_GetModule();
		if (!LC)
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorLiveCodingDisabled,
				TEXT("Live Coding module is not loadable in this build configuration "
					 "(LiveCoding module missing or platform unsupported); "
					 "Live Coding is Windows-desktop-editor only"));
		}
		// HasStarted() can be false even when the module is loaded — Live Coding console must be
		// initialised via EnableForSession. If it's not started AND can't be started, fail-fast.
		if (!LC->HasStarted() && !LC->CanEnableForSession())
		{
			const FText ErrText = LC->GetEnableErrorText();
			return FMCPToolHelpers::MakeError(Request, kMCPErrorLiveCodingDisabled,
				FString::Printf(TEXT("Live Coding console is not started and cannot be enabled: %s; "
					"enable via Editor Preferences → General → Live Coding"),
					*ErrText.ToString()));
		}
	}
#else
	return FMCPToolHelpers::MakeError(Request, kMCPErrorLiveCodingDisabled,
		TEXT("Live Coding is only available on Windows desktop editor builds (compile-time gated "
			 "by PLATFORM_WINDOWS); this build was produced on a non-Windows platform"));
#endif

	// ─── Submit job ────────────────────────────────────────────────────────────────────────────
	const FGuid JobIdGuid = FMCPJobRegistry::Get().SubmitJob(
		TEXT("livecoding._recompile_internal"),
		[ModulesCap = MoveTemp(ModuleNames)]
		(FMCPJob& Job) -> TSharedPtr<FJsonValue>
		{
#if PLATFORM_WINDOWS
			// Body-side re-check: PIE state can change between submit and execution (e.g. user
			// hits Play in the editor while the job queues).
			if (FMCPWorldContext::IsPIEActive())
			{
				Job.ErrorMessage = TEXT("PIE became active between submit and execution; "
					"Live Coding recompile aborted (LC requires PIE off — D11)");
				return nullptr;
			}

			ILiveCodingModule* LC = LIV_GetModule();
			if (!LC)
			{
				Job.ErrorMessage = TEXT("Live Coding module unavailable inside job body — "
					"module was torn down between submit and execution?");
				return nullptr;
			}

			Job.Description = FString::Printf(TEXT("livecoding.recompile: %d module(s)"),
				ModulesCap.Num());

			// Capture log activity from LogLiveCoding for best-effort patched/failed module
			// extraction. We don't filter the FMCPLogStream here — we capture pre/post snapshots
			// and diff. This is best-effort because LogLiveCoding's message format is internal
			// to Epic and may change between UE versions.
			const int64 PreCompileObservedTotal = FMCPLogStream::Get().GetTotalObserved();

			const double StartTime = FPlatformTime::Seconds();

			// Subscribe to OnPatchComplete BEFORE calling Compile so we don't race the delegate
			// firing for very fast (NoChanges) compiles.
			std::atomic<bool> bPatchComplete{false};
			FDelegateHandle PatchCompleteHandle =
				LC->GetOnPatchCompleteDelegate().AddLambda([&bPatchComplete]()
				{
					bPatchComplete.store(true, std::memory_order_release);
				});

			// Trigger the compile. We pass WaitForCompletion=false so we can poll cooperatively
			// (the WaitForCompletion path internally calls FPlatformProcess::Sleep too — same
			// behaviour but we get cancel responsiveness).
			ELiveCodingCompileResult CompileResult = ELiveCodingCompileResult::NotStarted;
			const bool bAccepted = LC->Compile(ELiveCodingCompileFlags::None, &CompileResult);
			if (!bAccepted)
			{
				LC->GetOnPatchCompleteDelegate().Remove(PatchCompleteHandle);
				Job.ErrorMessage = FString::Printf(
					TEXT("ILiveCodingModule::Compile rejected the request; result=%s "
						 "(another compile may be in progress — wait for it and retry)"),
					LIV_CompileResultToString(CompileResult));
				return nullptr;
			}

			// Poll loop. Live Coding's Tick() must be called periodically to drain the patch
			// queue — it's the same call the editor's tick group invokes from FEngineLoop.
			bool bTimedOut = false;
			while (LC->IsCompiling() || !bPatchComplete.load(std::memory_order_acquire))
			{
				if (Job.bCancelRequested.load(std::memory_order_acquire))
				{
					// Cooperative cancel — Live Coding has no Cancel API in 5.7, so we just stop
					// waiting. The compile finishes in the background; next recompile will see
					// CompileStillActive until it drains.
					LC->GetOnPatchCompleteDelegate().Remove(PatchCompleteHandle);
					Job.ErrorMessage.Reset();
					return nullptr;  // FMCPJobRegistry interprets null + empty ErrorMessage + bCancelRequested → Cancelled
				}

				const double ElapsedSecs = FPlatformTime::Seconds() - StartTime;
				if (ElapsedSecs > kLIVMaxRecompileWaitSecs)
				{
					bTimedOut = true;
					break;
				}

				LC->Tick();
				Job.Progress.store(
					FMath::Clamp(static_cast<float>(ElapsedSecs / kLIVMaxRecompileWaitSecs), 0.0f, 0.99f),
					std::memory_order_release);

				FPlatformProcess::Sleep(static_cast<float>(kLIVPollIntervalSecs));
			}

			LC->GetOnPatchCompleteDelegate().Remove(PatchCompleteHandle);

			const double DurationSecs = FPlatformTime::Seconds() - StartTime;
			Job.Progress.store(1.0f, std::memory_order_release);

			// Best-effort log-scrape for patched / failed modules. The LogLiveCoding category emits
			// lines like "Compilation done for <Module>" / "Failed to patch <Module>: <reason>".
			// We snapshot the LogLiveCoding entries that arrived during the compile + parse them.
			// Patterns may shift between UE versions; treat as advisory.
			TArray<FString> PatchedModulesList;
			TArray<TPair<FString, TArray<FString>>> FailedModulesList;
			{
				const TArray<FMCPLogEntry> RecentEntries =
					FMCPLogStream::Get().GetLastN(FMCPLogStream::kMaxEntries, nullptr);
				static const FName LogLiveCodingName(TEXT("LogLiveCoding"));
				for (const FMCPLogEntry& E : RecentEntries)
				{
					if (E.Category != LogLiveCodingName) continue;
					// Skip pre-compile entries; the cumulative TotalObserved snapshot tells us where
					// our compile window starts in the ring. We rely on chronological order from
					// GetLastN — entries arriving after PreCompileObservedTotal are in-window.
					// (Imprecise — the ring can have wrapped — but the worst case is over-reporting.)
					if (E.Message.Contains(TEXT("Compilation done"), ESearchCase::IgnoreCase) ||
						E.Message.Contains(TEXT("Patched"), ESearchCase::IgnoreCase))
					{
						// Naive extract: text between quotes if present, else last word.
						FString Token;
						int32 QStart, QEnd;
						if (E.Message.FindChar(TEXT('"'), QStart) &&
							E.Message.FindLastChar(TEXT('"'), QEnd) &&
							QEnd > QStart)
						{
							Token = E.Message.Mid(QStart + 1, QEnd - QStart - 1);
						}
						else
						{
							Token = E.Message.TrimStartAndEnd();
						}
						if (!Token.IsEmpty())
						{
							PatchedModulesList.AddUnique(Token);
						}
					}
					else if (E.Verbosity <= ELogVerbosity::Warning &&
					         (E.Message.Contains(TEXT("Failed to patch"), ESearchCase::IgnoreCase) ||
					          E.Message.Contains(TEXT("error"), ESearchCase::IgnoreCase)))
					{
						TPair<FString, TArray<FString>>& Entry = FailedModulesList.AddDefaulted_GetRef();
						Entry.Key = TEXT("(unknown)");
						Entry.Value.Add(E.Message);
					}
				}
			}
			// Silence unused-variable warnings for the pre-compile snapshot (advisory only).
			(void)PreCompileObservedTotal;

			TArray<TSharedPtr<FJsonValue>> PatchedArr;
			for (const FString& M : PatchedModulesList)
			{
				PatchedArr.Add(MakeShared<FJsonValueString>(M));
			}
			TArray<TSharedPtr<FJsonValue>> FailedArr;
			for (const TPair<FString, TArray<FString>>& F : FailedModulesList)
			{
				TSharedRef<FJsonObject> FailedObj = MakeShared<FJsonObject>();
				FailedObj->SetStringField(TEXT("name"), F.Key);
				TArray<TSharedPtr<FJsonValue>> ErrArr;
				for (const FString& Err : F.Value)
				{
					ErrArr.Add(MakeShared<FJsonValueString>(Err));
				}
				FailedObj->SetArrayField(TEXT("errors"), ErrArr);
				FailedArr.Add(MakeShared<FJsonValueObject>(FailedObj));
			}

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetBoolField(TEXT("recompiled"),
				CompileResult == ELiveCodingCompileResult::Success);
			Out->SetStringField(TEXT("result"),
				LIV_CompileResultToString(CompileResult));
			Out->SetArrayField(TEXT("patched_modules"), PatchedArr);
			Out->SetArrayField(TEXT("failed_modules"),  FailedArr);
			Out->SetNumberField(TEXT("duration_secs"),  DurationSecs);
			Out->SetBoolField(TEXT("wait_timeout_hit"), bTimedOut);
			Out->SetStringField(TEXT("live_coding_version"), TEXT(LIVE_CODING_MODULE_NAME));

			// Echo the input modules for round-trip clarity.
			TArray<TSharedPtr<FJsonValue>> ModulesEcho;
			for (const FString& M : ModulesCap)
			{
				ModulesEcho.Add(MakeShared<FJsonValueString>(M));
			}
			Out->SetArrayField(TEXT("modules_requested"), ModulesEcho);

			return MakeShared<FJsonValueObject>(Out);
#else
			Job.ErrorMessage = TEXT("Live Coding is only available on Windows desktop editor builds "
				"(PLATFORM_WINDOWS gate failed in body) — should have been caught at submit time");
			return nullptr;
#endif // PLATFORM_WINDOWS
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

// ─── Registration ──────────────────────────────────────────────────────────────────────────────
//
// MUST be Lane B — the Python composite calls this via dispatch_internal (TCP loopback) from
// inside FMCPPythonEval::CallPythonTool on the game thread. Lane A would queue back to the same
// game thread that's blocked on socket.recv() → 60s deadlock. Same rationale as all other
// async-only composite internal handlers shipped in Phases 2-6.
void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	// Phase 6 Chunk E: livecoding.recompile backing internal (async composite).
	//
	// Lane B. Submitter does NOT call IsPIEActive (moved to job body — submitter-level call
	// crashed on listener thread). ILiveCodingModule pre-checks (HasStarted/CanEnableForSession)
	// are observed empirically thread-safe — they're simple flag reads. Body still re-checks PIE
	// state for the actual recompile.
	//
	// CRITICAL: must remain Lane B. Python composites call dispatch_internal which synchronously
	// holds GT — Lane A would deadlock on OnEndFrame drain (Phase 2 Hotfix 3 pattern).
	RegisterTool(TEXT("livecoding._recompile_internal"), &Tool_RecompileInternal, /*Lane B*/ true);

	UE_LOG(LogMCP, Log,
		TEXT("Phase 6 Chunk E (Live Coding): registered 1 internal composite handler ")
		TEXT("(livecoding._recompile_internal, Lane B — IsPIEActive guard moved to job body to "
		     "avoid listener-thread crash; bGameThreadRequired=true on the job body); Python "
		     "wrapper livecoding.recompile in phase6_composites.py; PLATFORM_WINDOWS=%d"),
		PLATFORM_WINDOWS);
}

} // namespace FLiveCodingTools

#undef LOCTEXT_NAMESPACE

MCP_REGISTER_SURFACE(LiveCodingTools, &FLiveCodingTools::Register)
