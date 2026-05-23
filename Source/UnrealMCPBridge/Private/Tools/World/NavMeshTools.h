// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Wave G Surface 3 — Navigation system query surface (navmesh.*). 4 user-visible tools, all Lane A.
 *
 * Tool roster:
 *   navmesh.list               → enumerate ARecastNavMesh actors with key configuration fields
 *   navmesh.rebuild            → UNavigationSystemV1::Build() OR per-NavData RebuildAll
 *   navmesh.find_path          → FindPathSync between two world points (returns waypoint list)
 *   navmesh.project_to_navmesh → ProjectPointToNavigation (find nearest reachable point)
 *
 * **Lane A justification.** All four tools touch GAME-THREAD-ONLY state:
 *   - ``UNavigationSystemV1::GetCurrent(World)`` reads ``UWorld::NavigationSystem`` which is a UObject
 *     pointer; UObject pointer reads from non-GT are unsafe (GC may rewrite during a sweep).
 *   - ``ARecastNavMesh`` field reads (``AgentRadius`` / ``AgentHeight`` / ``CellSize`` / ``TileSizeUU``)
 *     come from a live AActor — same UObject GT constraint.
 *   - ``FindPathSync`` walks the recast tree (NavData->FindPath internally) under the navmesh's own
 *     critical section; calling from non-GT can deadlock with the navigation task graph.
 *   - ``Build()`` schedules build tasks via the navigation thread pool; submitting from non-GT races
 *     the task scheduler.
 *   - ``ProjectPointToNavigation`` is a synchronous octree query; same GT-only convention as
 *     FindPathSync.
 *
 * **PIE guard policy.**
 *   - ``navmesh.list`` / ``navmesh.find_path`` / ``navmesh.project_to_navmesh`` — NOT guarded. Reads
 *     work on whichever world resolves (PIE > editor) so callers can introspect the runtime navmesh
 *     during gameplay.
 *   - ``navmesh.rebuild`` — PIE-guarded with -32027 + frozen ``kMCPMessagePIEActive`` text. Build is
 *     an editor-time operation; runtime rebuilds would have to go through dynamic-nav-mesh APIs that
 *     are out of scope for this surface.
 *
 * **No transactions / no MarkPackageDirty.** Navmesh data is generated into editor-world ARecastNavMesh
 * actors but the BUILD operation is itself the "edit" — the actor is saved separately via standard
 * level-save flows. We intentionally do NOT wrap Build() in FScopedTransaction because (a) the build
 * task is asynchronous (Build kicks off, returns; tiles populate over the next several frames) so a
 * single-call transaction would close before any tiles materialise, and (b) Ctrl-Z on navmesh tiles
 * is a known footgun even in the native editor UI.
 *
 * **World selection.** Mirrors PhysicsTools / DebugTools: PIE world when ``GEditor->PlayWorld`` is
 * non-null; otherwise the editor world from ``FMCPWorldContext::GetEditorWorld()``. Response carries
 * the world-kind string (``"editor"`` | ``"pie"``) so callers can confirm.
 *
 * **FatumGame caveat.** This is the UE NavigationSystem (recast), NOT the Jolt/Barrage navmesh
 * (Barrage doesn't ship one — characters use raycast-based locomotion). These tools surface UE's
 * navigation queries for AI/agent prototyping; they have no awareness of Flecs entities, ISM-rendered
 * items, or Jolt static bodies (recast bakes against UE collision meshes only).
 *
 * **Error codes (all reused — no new codes added in MCPTypes.h):**
 *   - -32004 ObjectNotFound      navmesh_actor_path supplied but doesn't resolve to an ARecastNavMesh
 *   - -32027 PIEActive           navmesh.rebuild attempted during PIE
 *   - -32602 InvalidParams       missing/malformed coords on find_path / project_to_navmesh
 *   - -32603 Internal            no world available OR no UNavigationSystemV1 on the resolved world
 *
 * Threading: GT only (``check(IsInGameThread())`` enforced).
 */
namespace FNavMeshTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	UNREALMCPBRIDGE_API FMCPResponse Tool_List(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_Rebuild(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_FindPath(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_ProjectToNavMesh(const FMCPRequest& Request);
}
