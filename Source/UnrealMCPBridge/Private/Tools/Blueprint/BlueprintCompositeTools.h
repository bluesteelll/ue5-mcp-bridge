// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Phase 4 — Category C (Blueprint composites). ONE internal Lane-B handler backing the single
 * Python composite tool ``bp.compile_all_dirty`` defined in
 * ``Content/Python/MCPTools/tools/blueprint_composites.py``.
 *
 * Lifecycle (per Phase 4 plan §day-by-day Day 10):
 *   Day 10: ``bp._compile_all_dirty_internal`` (Lane B submitter + GT body)
 *
 * **Async-only composite pattern (same rationale as Phase 3 LevelCompositeTools).** The Python
 * wrapper validates ``args.scope_paths`` (non-empty list of ``/Game/...`` paths) and raises
 * ``ValueError`` on bad input — auto-translated to JSON-RPC -32602 by the dispatcher (Phase 3
 * polish #12). The internal handler is Lane B: it parses args + submits a job (with
 * ``bGameThreadRequired=true``) + returns ``{job_id}``. Actual work — AR walk, BP loading, per-BP
 * compile — runs on the game thread inside the job body. AI client polls ``job.status`` /
 * ``job.result`` externally.
 *
 * **Failure aggregation (D1).** The body does NOT abort on first failure. It accumulates per-BP
 * status: ``{compiled, succeeded, failed[{path, errors[]}], duration_ms}``. AI workflow gets the
 * full failure surface in one round-trip — can fix N BPs across multiple files in one edit.
 *
 * **Cooperative cancel cadence (Day 10 calibration — Phase 4 plan §critic Q5).** Phase 3 default
 * was every 256 iterations because Phase 3 ops are cheap mutator/property writes. BP compile is
 * orders of magnitude heavier (~50ms-2s each); we drop the cadence to every 16 BPs. Progress
 * updates every 8 BPs.
 *
 * **PIE guard placement.** Inside the job body. PIE state can transition between listener-thread
 * submit and game-thread execution. Body emits ``Job.ErrorMessage = kMCPMessagePIEActive`` and
 * returns null on PIE detection — registry surfaces Failed; AI client sees -32027-style message
 * on first poll.
 *
 * **scope_paths default + validation (D6).** Required arg. Empty array → Python wrapper raises
 * ValueError → -32602. Default usage: ``["/Game"]``. Engine/plugin/Script paths excluded by the
 * AR filter convention (the filter just ANDs the scopes — caller can include those by explicit
 * scope listing if they have a non-shippable reason).
 */
namespace FBlueprintCompositeTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	UNREALMCPBRIDGE_API FMCPResponse Tool_CompileAllDirtyInternal(const FMCPRequest& Request);
}
