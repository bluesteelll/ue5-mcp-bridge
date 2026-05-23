// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Phase 3 — Category D (Composites). 5 internal C++ Lane-B handlers backing the 5 Python composite
 * tools defined in ``Content/Python/MCPTools/tools/level_composites.py``.
 *
 * Lifecycle (per Phase 3 plan §day-by-day Days 11-14):
 *   Day 11-12: ``level._full_actor_dump_internal``, ``level._find_actors_with_class_internal``
 *   Day 13:    ``actor._batch_spawn_internal``, ``actor._batch_destroy_internal``
 *   Day 14:    ``actor._batch_set_property_internal``
 *
 * **All 5 internals are Lane B** (``bThreadSafe=true``) per the Phase 2 Hotfix-3 pattern:
 *   - Sync handler runs on the listener thread, ONLY does string/JSON arg parsing + path validation
 *     + ``FMCPJobRegistry::SubmitJob`` with ``bGameThreadRequired=true``. Returns ``{job_id}`` to
 *     the Python composite.
 *   - The actual work — UWorld traversal, actor spawn/destroy, property mutation — runs in the job
 *     body lambda on the game thread.
 *   - The Python composite returns the ``{job_id}`` envelope to the AI client; the client then
 *     polls ``job.status`` / ``job.result`` from off-game-thread (external TCP socket on its own
 *     thread).
 *
 * **Composite Python wrappers are async-only** — they never poll their own job. This matches the
 * post-Hotfix-3 contract for all asset composites (see AssetCompositeTools.h doc).
 *
 * **Internal handler filter convention.** Names use the ``_internal`` suffix (e.g.
 * ``level._full_actor_dump_internal``) so callers can distinguish them from user-visible tools.
 * The leading underscore on the second segment matches the asset-composite convention
 * (``asset._find_unused_internal``). Both internal and public handlers appear in
 * ``tools.list.cpp_handlers``; the Python tool registry (``MCPTools.registry``) is the source of
 * truth for what AI clients see.
 *
 * **Cooperative cancellation.** All 5 job bodies poll ``Job.bCancelRequested`` at the top of the
 * per-item loop and set ``Job.ErrorMessage = TEXT("cancelled"); return nullptr;`` to terminate
 * cleanly. The registry transitions the job to ``Cancelled`` state when the body returns null
 * after cancel has been requested.
 *
 * **Progress cadence.** Per the v3 plan critic NIT, all 5 job bodies update progress every 256
 * iterations (``(i & 0xFF) == 0``) to avoid atomic-store traffic dominating the per-item cost. The
 * cap-rejection path emits a final progress write of 1.0 before returning.
 *
 * **PIE-guard placement.** ``actor._batch_spawn_internal`` / ``actor._batch_destroy_internal`` /
 * ``actor._batch_set_property_internal`` check ``FMCPWorldContext::IsPIEActive`` INSIDE the job
 * body (NOT at submit time) because PIE state can transition between the listener-thread submit
 * and the game-thread body execution. Failure surface is identical to the per-tool mutators:
 * ``Job.ErrorMessage = kMCPMessagePIEActive; return nullptr;``.
 *
 * **Caps (per plan v3 D3/C4/C5).**
 *   - ``MAX_ACTORS_PER_DUMP = 5000`` for ``level._full_actor_dump_internal`` and
 *     ``level._find_actors_with_class_internal``. Detected inside the job body (not at submit time
 *     — would require game-thread UWorld access). AI client sees ``Failed`` after first poll.
 *   - ``MAX_BATCH_ITEMS = 1000`` for ``actor._batch_spawn_internal`` / ``actor._batch_destroy_internal``
 *     / ``actor._batch_set_property_internal``. Detected at submit time (pure ``Args.GetArray.Num()``
 *     check — Lane B-safe). Surfaced as ``kMCPErrorInputTooLarge`` (-32017).
 *
 * **Per-item transactions (per plan v3 D3 Lesson 4 / C5).** Batch handlers wrap EACH item in its
 * own ``FScopedTransaction`` so the user can undo individual operations from a batch. The
 * ~500µs/transaction overhead dominates the per-item cost for trivial spawns; that's acceptable
 * because batch composites are AI-orchestration tools, not hot-path mutators.
 */
namespace FLevelCompositeTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	// ─── Day 11-12: level dump + class-filtered enumeration ────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_FullActorDumpInternal(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_FindActorsWithClassInternal(const FMCPRequest& Request);

	// ─── Day 13: batch spawn + destroy ─────────────────────────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_BatchSpawnInternal(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_BatchDestroyInternal(const FMCPRequest& Request);

	// ─── Day 14: batch property mutation ───────────────────────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_BatchSetPropertyInternal(const FMCPRequest& Request);
}
