// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Phase 6 — Chunk A (Source Control composites). ONE internal Lane-B handler backing the single
 * Python composite tool ``sc.submit`` defined in
 * ``Content/Python/MCPTools/tools/phase6_composites.py``.
 *
 * Lifecycle (per Phase 6 plan §day-by-day Day 3):
 *   Day 3: ``sc._submit_internal`` (Lane B submitter + GT body)
 *
 * **Async-only composite pattern (same rationale as Phase 3-5 composites).** The Python wrapper
 * validates ``args.file_paths`` (non-empty list of path strings) + ``args.description`` (non-empty
 * string) and raises ``ValueError`` on bad input — auto-translated to JSON-RPC -32602 by the
 * dispatcher (Phase 3 polish #12). The internal handler is Lane B: it parses args + validates +
 * submits a job (with ``bGameThreadRequired=true``) + returns ``{job_id}``. Actual work — running
 * ``Provider.Execute(FCheckIn)`` which performs the Perforce/Git RPC — runs on the game thread
 * inside the job body. AI client polls ``job.status`` / ``job.result`` externally.
 *
 * **Why async?** Per Phase 6 plan §D1: "Underlying Perforce/Git submit RPC can take 5-60s on
 * large changelists. Sync would block GT + listener thread." Same rationale as
 * ``bp.compile_all_dirty`` + ``cb.bulk_import`` + Phase 5 ``editor.cook_*``.
 *
 * **Failure aggregation.** The body assembles per-file outcome from post-submit state into
 * ``{submitted: bool, changelist: string, conflicts: [string], duration_ms, error?}``.
 * On batch-level failure, ``submitted=false`` + ``error`` describes the provider rejection;
 * ``conflicts[]`` lists files in conflict state (caller resolves them and retries).
 *
 * **Path resolution.** Identical to the sync sc.* tools: caller passes disk paths or
 * ``/Game/...`` package paths, sandboxed via ``FMCPPathSandbox``, package paths translated to
 * ``.uasset`` / ``.umap`` filenames via ``FPackageName``. Per-file path failures are surfaced
 * at submit time (sync error before job creation) so the caller doesn't get a job_id for an
 * invalid batch.
 *
 * **Provider check inside body.** Even though we validate provider availability at submit time
 * for fail-fast, the body re-checks because provider state can transition (e.g. network glitch
 * disconnecting Perforce between submit and execution). Failure → ``Job.ErrorMessage =
 * <provider message>; return nullptr;`` — registry surfaces Failed; AI client sees
 * ``-32045`` SourceControlProviderUnavailable-style message on first poll.
 *
 * **Changelist field is wire-string.** Per Phase 6 plan risk #7: "Perforce changelist may be
 * string vs int across providers". We always return as JSON string; caller parses as int if
 * known-Perforce. Empty string = no changelist created (e.g. Git auto-commit).
 */
namespace FSourceControlCompositeTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	UNREALMCPBRIDGE_API FMCPResponse Tool_SubmitInternal(const FMCPRequest& Request);
}
