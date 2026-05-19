// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Phase 6 — Chunk E (Live Coding). ONE async composite tool — only async sub-surface in Chunk E.
 *
 * Tool roster (per Phase 6 plan §Category-E):
 *   livecoding.recompile (Python composite wrapper) → returns {job_id}; AI polls job.result.
 *   livecoding._recompile_internal (Lane B C++ submitter) → submits a GT job invoking
 *                                                            ILiveCodingModule::Compile().
 *
 * **Why async?** Per Phase 6 plan §D3: "Live Coding compile takes 5-30s. Sync would hang the GT."
 * Same rationale as ``bp.compile_all_dirty`` / ``sc.submit`` / ``test.run_automation``.
 *
 * **Internal handler is Lane B.** The Python composite ``livecoding.recompile`` (in
 * ``phase6_composites.py``) calls ``dispatch_internal("livecoding._recompile_internal", ...)``
 * from inside ``FMCPPythonEval::CallPythonTool`` on the game thread. Lane A would queue back to
 * the same game thread that's blocked on socket.recv() → 60s deadlock. The Lane B submitter
 * validates inputs + submits a job (with ``bGameThreadRequired=true`` — Live Coding mutations
 * require the GT for module init and FOnPatchCompleteDelegate fire) + returns ``{job_id}``.
 *
 * **PIE guard (D11).** ``livecoding.recompile`` refuses when GEditor->PlayWorld != nullptr.
 * Per Epic docs (LiveCoding/Source/.../LiveCoding.cpp): "Live Coding requires that PIE be
 * stopped before recompiling" — attempting recompile during PIE can leave the game in an
 * inconsistent state (module patch applies mid-frame; gameplay code mutates UClass*). We surface
 * -32027 PIEActive with the frozen kMCPMessagePIEActive text.
 *
 * **Build configuration availability.** Live Coding is platform-locked: only available on
 * Windows desktop editor builds (not Linux, not Mac, not console, not Shipping). The
 * ``LiveCoding`` module is a Windows-only Developer module. We check
 * ``FModuleManager::Get().ModuleExists(TEXT("LiveCoding"))`` + ``IsAvailable()`` AND
 * ``ILiveCodingModule::HasStarted()`` (which can be false even when the module is loaded — Live
 * Coding console must be initialised via the EnableForSession path). Either check failing →
 * -32048 LiveCodingDisabled with recovery hint pointing at Editor Preferences → Live Coding.
 *
 * **modules argument.** Accepts an array of UE module names (e.g. ``["FatumGame", "Barrage"]``)
 * OR the wildcard ``["*"]`` (compiles all dirty modules). Per Phase 6 plan: Python wrapper
 * validates non-empty array of strings. We do NOT validate that each named module exists in the
 * process (``FModuleManager::IsModuleLoaded``) BEFORE submitting — Live Coding's own compile path
 * gracefully handles unknown module names (logged as warnings, not failures). Caller can inspect
 * the recompile output via ``log.tail category="LogLiveCoding"`` for any per-module diagnostics.
 *
 * **Compile flow.** The body sets up a one-shot subscription to ``GetOnPatchCompleteDelegate()``
 * to capture the completion event, then calls ``Compile(WaitForCompletion=false, Result*)`` to
 * kick off the async compile. Loop on ``IsCompiling()`` + ``Tick()`` with a small ``FPlatformProcess::Sleep``
 * each iteration to yield the GT (sleep is on the WORKER thread for the duration the job runs;
 * GT continues drawing frames). On ``IsCompiling() == false`` we wait one more delegate fire to
 * capture the final ``ELiveCodingCompileResult`` and return.
 *
 * **Result schema** (returned by ``job.result`` once Succeeded)::
 *
 *     {
 *       "recompiled":         bool,                // true if at least one patch landed (Success)
 *       "result":             "Success" | "NoChanges" | "InProgress" | "CompileStillActive" |
 *                             "NotStarted" | "Failure" | "Cancelled",
 *       "patched_modules":    [string],            // best-effort list (parsed from LogLiveCoding entries)
 *       "failed_modules":     [{name, errors[]}],  // best-effort, populated from LogLiveCoding warnings
 *       "duration_secs":      float,               // wall-clock from compile start to delegate fire
 *       "wait_timeout_hit":   bool,                // true if we bailed after kMaxLiveCodingWaitSecs
 *       "live_coding_version": string              // ILiveCodingModule::GetModuleName() echo
 *     }
 *
 * The patched_modules / failed_modules arrays are best-effort: Live Coding does not expose a
 * structured per-module status through its public delegate. We snapshot LogLiveCoding entries
 * during the compile via FMCPLogStream and post-process them — caller has the raw log via
 * log.tail if they need authoritative diagnostics.
 *
 * **Cooperative cancel.** Worker polls ``Job.bCancelRequested`` every poll iteration. On cancel
 * we DON'T attempt to abort the running compile (Live Coding has no Cancel API in 5.7) — we
 * simply stop polling and return Cancelled, leaving the compile to finish in the background.
 * Subsequent recompile attempts will see ``CompileStillActive`` until the prior compile drains.
 *
 * **Build.cs.** Adds ``LiveCoding`` private dep — module header is in
 * ``Engine/Source/Developer/Windows/LiveCoding/Public/ILiveCodingModule.h``. The module is
 * Windows-only by Epic's design (LiveCoding.Build.cs restricts to ``UEBuildConfiguration.bWinXX``).
 * On non-Windows the include will fail at compile time — we wrap the entire LiveCoding usage in
 * ``#if PLATFORM_WINDOWS`` and surface -32048 LiveCodingDisabled on other platforms.
 */
namespace FLiveCodingTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	// ─── Category E: Live Coding async composite (Lane B submitter) ──────────────────────────────
	// Backing internal for the Python wrapper ``livecoding.recompile`` in phase6_composites.py.
	UNREALMCPBRIDGE_API FMCPResponse Tool_RecompileInternal(const FMCPRequest& Request);
}
