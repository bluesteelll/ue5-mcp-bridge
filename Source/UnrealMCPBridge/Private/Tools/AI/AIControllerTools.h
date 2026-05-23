// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Wave J Surface 3 — AAIController enumeration + introspection surface (ai.controller.*).
 * 3 user-visible tools, all Lane A.
 *
 * Tool roster:
 *   ai.controller.list                — enumerate AAIController actors in the current editor world.
 *                                       Optional ``class_filter`` (full /Script/... or /Game/... path)
 *                                       narrows to subclasses of the resolved UClass. Per entry:
 *                                       { actor_path, class_path, possessed_pawn?, has_blackboard,
 *                                         has_active_bt }. Runtime — no PIE guard, so it transparently
 *                                       sees AI controllers in either the editor world OR the PIE
 *                                       world when PIE is active (the call simply enumerates whichever
 *                                       world GEditor->GetEditorWorldContext().World() resolves to).
 *   ai.controller.get_state           — detailed runtime info for one controller. Returns
 *                                       { class_path, possessed_pawn?, active_behavior_tree?,
 *                                         blackboard_asset?, perception_components,
 *                                         navmesh_volume? }. Read-only.
 *   ai.controller.respawn_blackboard  — replace the blackboard asset on a controller. Calls
 *                                       UseBlackboard (creates the BlackboardComponent if missing)
 *                                       OR InitializeBlackboard on the existing component. Runtime
 *                                       mutator — NO PIE guard (the brief explicitly excludes one;
 *                                       reseating a blackboard during PIE is a legitimate AI-author
 *                                       workflow). Returns { replaced, prior_blackboard? }.
 *
 * **No PIE guard.** Per the Wave J S3 brief these tools target runtime AI state — both
 * enumeration AND blackboard reseat are valid during PIE. The flip side: ``actor_path`` strings
 * returned by ``list`` during PIE address the PIE-world transient duplicate, NOT the editor-world
 * source actor. Callers persisting AI controller references should call ``list`` AFTER PIE has
 * stopped if they want editor-world-stable paths.
 *
 * **Class filter semantics.** ``ai.controller.list`` ``class_filter`` accepts any UClass path
 * resolvable via ``StaticLoadClass`` / ``FindObject<UClass>``. If the path doesn't resolve OR
 * doesn't derive from AAIController, the response is -32011 WrongClass. Omitting the field
 * defaults to AAIController itself (matches all subclasses).
 *
 * **Possessed-pawn shape.** The ``possessed_pawn`` field is an actor path string (canonical
 * BuildActorPath form) when the controller currently possesses a pawn; the field is OMITTED
 * (not null) when no pawn is possessed. Same convention applies to the optional fields
 * ``active_behavior_tree``, ``blackboard_asset``, ``navmesh_volume``, ``prior_blackboard`` —
 * omitted when absent, never set to JSON null.
 *
 * **Perception components.** ``get_state`` returns the count of UAIPerceptionComponent
 * instances on the controller (typically 0 or 1; engine convention is a single perception
 * component but custom AI subclasses occasionally attach more). The count exposes the
 * structure without requiring a per-component drill-down on this surface.
 *
 * **navmesh_volume.** ``get_state`` resolves the controlled pawn's location against every
 * ANavMeshBoundsVolume in the world via ``EncompassesPoint``. Returns the actor path of the
 * first containing volume, or omits the field when the pawn is outside every volume (or when
 * the controller has no possessed pawn). This is a best-effort spatial classification — it
 * does NOT correspond to which navdata the controller's pathfinder is currently using; the
 * NavigationSystem assigns navdata per-agent-class, not per-volume.
 *
 * **Blackboard reseat semantics.** ``respawn_blackboard`` accepts a ``blackboard_asset_path``
 * resolvable via ``LoadObject<UBlackboardData>``. When the controller already has a
 * UBlackboardComponent, the prior asset is captured into ``prior_blackboard`` (so callers
 * can roll back) BEFORE the new asset is installed via ``UBlackboardComponent::InitializeBlackboard``.
 * When the controller has NO BlackboardComponent yet (uncommon but possible for controllers
 * spawned without one), ``AAIController::UseBlackboard`` is called instead — this both creates
 * the component AND initialises it; ``prior_blackboard`` is omitted in that path.
 *
 * **Lane A justification.** AActor / UWorld / UObject pointer traversal is GAME-THREAD-ONLY.
 * ``AAIController::UseBlackboard`` mutates the actor's component graph (adds a UActorComponent
 * via NewObject), which races UObject GC + Slate widget rebuilds off-GT.
 *
 * **Error codes (all reused — no new codes added in MCPTypes.h):**
 *   - -32004 ObjectNotFound      controller / pawn / blackboard asset doesn't resolve
 *   - -32010 InvalidPath         malformed controller_path / blackboard_asset_path
 *   - -32011 WrongClass          actor isn't AAIController; asset isn't UBlackboardData;
 *                                class_filter doesn't derive from AAIController
 *   - -32602 InvalidParams       missing required string field
 *   - -32603 Internal            no GEditor / no editor world
 *
 * Threading: GT only (``check(IsInGameThread())`` enforced in each tool).
 */
namespace FAIControllerTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	UNREALMCPBRIDGE_API FMCPResponse Tool_List(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_GetState(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_RespawnBlackboard(const FMCPRequest& Request);
}
