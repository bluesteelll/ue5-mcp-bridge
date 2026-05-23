// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Phase 6 — Chunk B (Automation Test). 7 user-visible synchronous tools, all Lane A.
 *
 * Tool roster (per Phase 6 plan §Category-B):
 *   test.list_automation_specs  → paginated enum of registered tests (FMCPPageCursor)
 *   test.run_single_test        → sync convenience: start + wait for one test (must be fast)
 *   test.get_last_results       → snapshot of FAutomationTestFramework::GetExecutionInfo
 *   test.cancel_current         → best-effort StopTest + DequeueAllCommands
 *   test.list_categories        → derive distinct top-level path segments (System / Project / ...)
 *   test.get_test_info          → single test detail (path, flags, source file, etc.)
 *   test.set_filter_flags       → map ["smoke","perf"...] → EAutomationTestFlags via the
 *                                 Engine-provided EAutomationTestFlags_GetTestFlagsMap() lookup
 *
 * The 8th tool ``test.run_automation`` is an ASYNC composite — its internal Lane-B submitter
 * lives in ``Tools/TestCompositeTools.{h,cpp}`` and its Python wrapper in
 * ``Content/Python/MCPTools/tools/phase6_composites.py``.
 *
 * **All 7 sync tools are Lane A** (``bThreadSafe=false``). Reasons:
 *   - ``FAutomationTestFramework`` is a global singleton with mutable internal state (test
 *     registry, latent command queue, current-running pointer). Reads + writes are
 *     game-thread-only by Epic design — the framework drives `ExecuteLatentCommands` on the
 *     game thread, and tests touch UObject / GWorld state.
 *   - ``GetValidTestNames`` walks the registered-tests map; ``ContainsTest`` queries it.
 *     Without a documented thread guarantee we conservatively pin Lane A (Phase 1 lesson —
 *     "uncertain → Lane A").
 *
 * **No PIE guard.** Test runs may themselves drive PIE start/stop via latent commands; we
 * surface this in the response (``warning`` field on ``run_single_test`` when PIE is detected
 * mid-flight) but don't block. The plan §risk-10 documents this: "test.run_automation may
 * interact with current editor PIE state; recommend pie.stop before running."
 *
 * **test.run_single_test is sync but bounded.** We `StartTestByName`, then drive
 * `ExecuteLatentCommands` in a loop with a wall-clock cap of ``kTSTSingleTestMaxSeconds``
 * (default 30s — most smoke tests are sub-second; product/perf tests should go through
 * ``test.run_automation`` instead). On timeout we issue ``StopTest`` + return -32603 with a
 * clear "exceeded sync cap" message and the captured partial results so the caller can decide.
 *
 * **Pagination scheme.** ``test.list_automation_specs`` uses ``FMCPPageCursor`` keyed on the
 * (FullTestPath) string. The filter-hash is computed from the optional ``filter`` substring;
 * caller-supplied ``filter`` mutation between pages → -32015 StaleCursor. Page size defaults
 * to 100, clamped [1, 1000].
 *
 * **Categories.** ``test.list_categories`` walks the registered tests once, extracts the first
 * dot-separated segment of FullTestPath (e.g. ``"System.Engine.Maps.Foo"`` → ``"System"``),
 * deduplicates into a TSet, and returns sorted. This mirrors what the editor's Session Frontend
 * exposes as the top-level "category" header.
 *
 * **test.set_filter_flags** delegates flag-name lookup to Epic's own ``EAutomationTestFlags_GetTestFlagsMap()``
 * — the canonical name table. Unknown flag names go into ``rejected[]`` with the per-name
 * reason; the union of known flags is applied via ``SetRequestedTestFilter``. Empty flags
 * array OR all-unknown → no-op (still returns 200 with empty ``applied[]``).
 *
 * **Build.cs.** No additional module dep needed — ``FAutomationTestFramework``, ``EAutomationTestFlags``,
 * ``FAutomationTestInfo`` and ``FAutomationTestExecutionInfo`` all live in ``Core/Public/Misc/AutomationTest.h``,
 * already a transitive dep of the bridge module. (The Phase 6 plan tentatively listed
 * ``AutomationTest`` + ``AutomationController`` — those modules ship higher-level UI-side
 * runner support and are NOT required for the bridge's framework-API usage.)
 */
namespace FTestTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	// ─── Category B: Automation Test sync tools ─────────────────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_ListAutomationSpecs(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_RunSingleTest(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_GetLastResults(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_CancelCurrent(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_ListCategories(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_GetTestInfo(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_SetFilterFlags(const FMCPRequest& Request);
}
