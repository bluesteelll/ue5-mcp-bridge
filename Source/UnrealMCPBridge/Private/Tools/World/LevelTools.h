// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Phase 3 — Category A (Level operations). 12 user-visible tools, all Lane A.
 *
 * Lifecycle by day (per Phase 3 plan §day-by-day):
 *   Day 1: ``level.list_loaded``, ``level.current_map``
 *   Day 2: ``level.load``, ``level.save``, ``level.create``, ``level.unload``,
 *          ``level.set_streaming_state``
 *   Day 3: ``level.get_world_settings``, ``level.set_world_settings``,
 *          ``level.get_persistent_level_actors``, ``level.save_all_dirty``, ``level.duplicate``
 *
 * **All 12 tools are Lane A** (``bThreadSafe=false``). Reasons:
 *   - Editor world traversal requires GAME THREAD (GEditor / UWorld / ULevel APIs not thread-safe).
 *   - Mutators wrap in FScopedTransaction (game-thread only).
 *   - Save/load operations call FEditorFileUtils / UEditorAssetSubsystem which assert IsInGameThread().
 *
 * **Mutator PIE-guard.** Every write-side handler (load, save, create, unload, set_streaming_state,
 * set_world_settings, save_all_dirty, duplicate) checks ``FMCPWorldContext::IsPIEActive`` first
 * and refuses with ``kMCPErrorPIEActive`` (-32027) + frozen message. Read-only handlers
 * (list_loaded, current_map, get_world_settings, get_persistent_level_actors) work transparently
 * during PIE — they see ``GEditor->PlayWorld`` when present, ``GetEditorWorld`` otherwise.
 *
 * **Async exception.** ``level.save_all_dirty`` is Lane A but its body submits a job via the
 * inline-SubmitJob pattern (per plan D3 N7 decision tree: single AI-facing async tool, no Python
 * orchestration needed). Returns ``{job_id}`` immediately; AI polls via ``job.status`` /
 * ``job.result`` from off-game-thread.
 *
 * **Lane B sanity probe.** ``_phase3_lane_b_sanity`` is a throwaway handler registered as Lane B
 * to verify the listener-thread router still works after the Phase 2 Lane-A demotion of every AR
 * tool. Returns ``{echo, thread_id}`` where ``thread_id`` is the FPlatformTLS::GetCurrentThreadId()
 * — a non-game-thread id confirms the request bypassed the Drain queue.
 */
namespace FLevelTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	// ─── Lane B sanity (per critic N1) ────────────────────────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_Phase3LaneBSanity(const FMCPRequest& Request);

	// ─── Day 1 ────────────────────────────────────────────────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_ListLoaded(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_CurrentMap(const FMCPRequest& Request);

	// ─── Day 2 ────────────────────────────────────────────────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_Load(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_Save(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_Create(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_Unload(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_SetStreamingState(const FMCPRequest& Request);

	// ─── Day 3 ────────────────────────────────────────────────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_GetWorldSettings(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_SetWorldSettings(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_GetPersistentLevelActors(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_SaveAllDirty(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_Duplicate(const FMCPRequest& Request);
}
