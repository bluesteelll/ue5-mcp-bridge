// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Phase 6 — Chunk B (Automation Test composites). ONE internal Lane-B handler backing the
 * single Python composite tool ``test.run_automation`` defined in
 * ``Content/Python/MCPTools/tools/phase6_composites.py``.
 *
 * **Async-only composite pattern (same rationale as ``sc.submit`` / Phase 3-5 composites).** The
 * Python wrapper validates ``args.test_names`` (non-empty list of strings) and raises ValueError
 * on bad input — auto-translated to JSON-RPC -32602 by the dispatcher. The internal handler is
 * Lane B: it parses args + validates pre-job (every name must exist in the framework) + submits
 * a job (``bGameThreadRequired=true``) + returns ``{job_id}``. Actual work — driving
 * ``StartTestByName`` + the latent-command loop for every test in sequence — runs on the game
 * thread inside the job body. AI client polls ``job.status`` / ``job.result`` externally.
 *
 * **Why async?** Per Phase 6 plan §D2: "UE automation tests can take 10s-15min each." A batch of
 * 5 tests could legitimately run for an hour. Sync would block GT + listener thread; async is
 * the only viable shape.
 *
 * **Why pre-job validation?** We resolve every test name BEFORE creating the job so the caller
 * gets a fast -32046 TestNotFound on typo (rather than waiting through job.submit + job.result
 * to discover the test didn't exist). The validation is O(N tests × M registered tests) — for
 * tens of caller names and thousands of registered tests, a handful of milliseconds. Acceptable.
 *
 * **Result shape.** Per Phase 6 plan §Category-B:
 *   {
 *     "succeeded":      [{name, duration_secs, warning_count}],
 *     "failed":         [{name, duration_secs, error_count, errors[], warnings[]}],
 *     "skipped":        [{name, reason}],   // future: tests excluded via excludelist or filter
 *     "total_duration": float,              // wall-clock for the batch
 *     "completed_count": int,
 *     "failed_count":    int
 *   }
 *
 * **Sequential execution.** Tests run one at a time within the batch — the framework's
 * StartTestByName + ExecuteLatentCommands loop is fundamentally a single-test driver (only one
 * test can be "current" at a time). Running tests in parallel would require multiple
 * FAutomationTestFramework instances which Epic doesn't support. The job iterates through
 * test_names, runs each one to completion, then proceeds to the next. Per-test progress is
 * exposed via ``FMCPJob::Progress`` (0..1 fraction).
 *
 * **Cooperative cancel.** The job body checks ``Job.bCancelRequested.load()`` between tests AND
 * inside the latent-command pump loop. On cancel detected mid-test, we ``DequeueAllCommands``
 * + ``StopTest`` the current test (capturing its partial info), and skip the remaining tests in
 * the batch (added to ``skipped[]`` with reason="batch cancelled before this test ran").
 *
 * **No per-test cap.** Unlike ``test.run_single_test`` (which has kTSTSingleTestMaxSeconds), the
 * batch path has no wall-clock cap per test — operators submit ``test.run_automation`` precisely
 * because they expect long runs. To bound total runtime, callers issue ``job.cancel`` from the
 * polling client.
 *
 * **Run-smoke-filter pass-through.** The optional ``run_smoke_filter`` arg, when present as a
 * non-empty string, is treated as an EAutomationTestFlags filter name (per
 * ``EAutomationTestFlags_GetTestFlagsMap``) and applied via ``SetRequestedTestFilter`` BEFORE
 * the batch runs. Common values: ``"SmokeFilter"``, ``"EngineFilter"``, ``"ProductFilter"``.
 * If supplied but unknown → -32602 InvalidParams (Python-side validation rejects). The filter
 * persists post-batch — callers can re-issue ``test.set_filter_flags`` to reset if needed.
 */
namespace FTestCompositeTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	UNREALMCPBRIDGE_API FMCPResponse Tool_RunAutomationInternal(const FMCPRequest& Request);
}
