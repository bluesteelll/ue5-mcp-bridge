// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Phase 2 — Category D internal C++ Lane handlers backing the Python composite tools.
 *
 * These 5 methods are registered with the dispatch queue but FILTERED out of ``tools.list``
 * (they have the leading-underscore convention `asset._..._internal`). Python composites
 * (`asset_composites.py`) call them by exact name as one-round-trip helpers, avoiding the
 * per-asset Python ferry catastrophe that would otherwise occur for find_unused / size_report /
 * find_broken_references / find_duplicates_by_name.
 *
 * Routing (post-hotfix 3 2026-05): ALL five internals use the async-job pattern. The sync
 * handler body is Lane B (validate args + FMCPJobRegistry::SubmitJob — no AR work, no UObject
 * access); the heavy enumeration runs inside the submitted job body lambda.
 *
 *   - `_find_unused_internal`              → Lane B, bGameThreadRequired=true (IAR.GetAssets
 *     asserts off-GT in UE 5.7 — registry dispatches via AsyncTask(GT, …))
 *   - `_size_report_internal`              → Lane B, bGameThreadRequired=true (same reason)
 *   - `_batch_metadata_internal`           → Lane B, bGameThreadRequired=false (uses only
 *     IAR.GetAssetByObjectPath, a thread-safe single-point query)
 *   - `_find_broken_references_internal`   → Lane B, bGameThreadRequired=true (IAR.GetAssets +
 *     GetDependencies traversal — same UE 5.7 GT-assert reason)
 *   - `_find_duplicates_by_name_internal`  → Lane B, bGameThreadRequired=true (IAR.GetAssets)
 *
 * Why all Lane B + async-job? Hotfix 3 finding: when a Python composite blocks on its OWN job
 * (via wait_for_job_and_return_result polling job.result), the game thread can't be both
 * "composite caller" AND "job body executor" simultaneously — even if job.result is Lane B,
 * the composite still owns the game thread while sleeping between polls. Resolution: composites
 * NEVER poll; they return {job_id} to the AI client which polls externally (off-game-thread TCP).
 * This matches the pattern already used by ``asset.batch_metadata_async`` (which always worked).
 * See ``Content/Python/MCPTools/tools/asset_composites.py`` for the "Why composites are async"
 * docstring on each composite function.
 */
namespace FAssetCompositeTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	// Day 11 / Hotfix 3: internal Lane-B handlers (NOT in tools.list — leading-underscore convention).
	UNREALMCPBRIDGE_API FMCPResponse Tool_FindUnusedInternal(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_SizeReportInternal(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_BatchMetadataInternal(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_FindBrokenReferencesInternal(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_FindDuplicatesByNameInternal(const FMCPRequest& Request);
}
