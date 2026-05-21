// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Wave H Surface 6 — Cooking automation surface. 3 user-visible tools that expose the editor's
 * cook pipeline to MCP callers (target-platform discovery, dry-run cookability validation, and
 * out-of-process cook kick-off via FMonitoredProcess).
 *
 * Tool roster:
 *   cook.list_platforms      — Enumerate ITargetPlatformManagerModule::GetTargetPlatforms()
 *                               (all registered platforms, not just the cooker-active subset).
 *                               Per-entry: { name, display_name, is_server, is_client, is_editor }
 *                               where ``name`` is the FString returned by
 *                               ``ITargetPlatform::PlatformName()`` (the canonical wire identifier
 *                               accepted by ``-TargetPlatform=<X>`` on UnrealEditor-Cmd.exe).
 *                               Lane A (TPM is game-thread by convention). NO PIE guard
 *                               (TPM state is independent of PIE).
 *   cook.validate_cookable   — Dry-run cookability check. Walks ``asset_paths`` (or all of /Game
 *                               when omitted) and classifies each package via the cheap
 *                               UPackage::HasAnyPackageFlags(PKG_EditorOnly) test plus the
 *                               target platform's ``AllowsEditorObjects()``/``IsServerOnly()`` gates
 *                               (which determine which packages a cook would emit). Returns
 *                               { cookable_count, uncookable_count, errors: [{ asset_path, reason }] }
 *                               where ``errors`` lists ONLY non-cookable assets (cookable ones are
 *                               summarised by count). Hard cap = ``max_assets`` (default 5000) to
 *                               avoid OOM on a project-wide walk. Lane A (LoadObject-equivalent
 *                               UPackage flag reads + AR walk). NO PIE guard.
 *   cook.start               — Kick off an out-of-process cook via FMonitoredProcess wrapping
 *                               UnrealEditor-Cmd.exe -run=Cook -TargetPlatform=<X>
 *                               -CookOutputDir=<output_directory>. Lane A entrypoint submits a
 *                               GT-NOT-required job (the body launches the process + polls in a
 *                               cooperative loop on the worker thread, no UObject access).
 *                               Returns { job_id, started_at }; caller polls via job.status /
 *                               job.result for { return_code, duration_secs, output_tail } once
 *                               the cook completes. Output captured into a bounded ring (last
 *                               1 MiB of stdout/stderr) so the worker doesn't OOM on a multi-hour
 *                               cook. Cooperative cancel calls FMonitoredProcess::Cancel(true)
 *                               to terminate the cooker subprocess tree on job.cancel.
 *
 * **Why no PIE guard.** Cooking operates on disk-resident package state — it can run during PIE
 * (the editor's own File → Cook menu doesn't gate on PIE either). The cook subprocess itself
 * spawns a SEPARATE UnrealEditor-Cmd.exe instance; the running editor's PIE has no effect on it.
 *
 * **Why Lane A for cook.start.** The submitter touches FMCPJobRegistry::SubmitJob which spins up
 * the worker pool on first call (game-thread for predictability), and resolves
 * FPaths::EngineDir() / FPaths::GetProjectFilePath() / FMCPPathSandbox (all game-thread-safe but
 * conventionally GT-called in this codebase). The JOB BODY itself runs on the worker thread and
 * does no UObject access — only FMonitoredProcess operations + FPlatformProcess::Sleep.
 *
 * **Error codes (all reused — no new codes introduced):**
 *   -32004 ObjectNotFound        platform_name not registered in TPM
 *   -32013 PathEscape            output_directory resolves outside the sandbox whitelist
 *   -32016 JobSubmitFailed       FMCPJobRegistry::SubmitJob refused (registry shutdown)
 *   -32030 OperationCancelled    (reserved — only surfaces in the job result via cancel)
 *   -32602 InvalidParams         missing args object, missing required field, max_assets out
 *                                of range
 *   -32603 InternalError         TPM module load failure, engine exe missing, project file
 *                                resolution failure
 */
namespace FCookTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	// ─── Wave H Surface 6: Cooking automation tools ─────────────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_CookListPlatforms(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_CookValidateCookable(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_CookStart(const FMCPRequest& Request);
}
