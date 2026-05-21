// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Wave J Surface 6 — UCrowdManager / detour-crowd RVO settings surface (ai.crowd.*). 3 tools, all Lane A.
 *
 * Runtime introspection of the detour-crowd RVO (Reciprocal Velocity Obstacles) avoidance system
 * that ships with UE's NavigationSystem. Crowd-managed pawns drive their movement through
 * UCrowdFollowingComponent (a PathFollowingComponent subclass that ALSO implements
 * ICrowdAgentInterface for cross-agent avoidance). The UCrowdManager itself lives as a singleton on
 * the active UNavigationSystemV1 and steers detour-crowd's per-tick RVO solver.
 *
 * Tool roster:
 *   ai.crowd.get_settings           Read top-level UCrowdManager UPROPERTY config (MaxAgents,
 *                                   MaxAgentRadius, MaxAvoidedAgents, MaxAvoidedWalls, etc.). The
 *                                   brief's literal {anticipation_time, max_avoid_velocity,
 *                                   num_velocity_samples} fields don't exist directly on
 *                                   UCrowdManager — those settings are per-config-slot inside
 *                                   ``FCrowdAvoidanceConfig[]`` and require a future
 *                                   ``ai.crowd.get_avoidance_config(index)`` to read. This tool
 *                                   exposes what's actually a top-level singleton property — see
 *                                   the response-shape docs on Tool_GetSettings. Returns -32603
 *                                   when no UCrowdManager is registered on the resolved world.
 *   ai.crowd.list_agents            Enumerate every UCrowdFollowingComponent in the resolved world
 *                                   (PIE-first, editor-fallback per NavMeshTools / PhysicsTools
 *                                   convention). Returns owner actor path, current crowd-agent
 *                                   velocity, current avoidance quality, and the agent's cylinder
 *                                   radius (from ICrowdAgentInterface::GetCrowdAgentCollisions).
 *                                   Empty array is a legitimate state (no AI pawns spawned, or
 *                                   pawns using vanilla UPathFollowingComponent without crowd).
 *   ai.crowd.set_avoidance_quality  Set ``AvoidanceQuality`` on one UCrowdFollowingComponent
 *                                   resolved by actor_path. Returns the prior quality so callers
 *                                   can round-trip the mutation if needed. The underlying
 *                                   ``SetCrowdAvoidanceQuality`` API also pushes the change into
 *                                   detour-crowd's per-agent params via ``UpdateCrowdAgentParams``
 *                                   when ``bUpdateAgent=true`` (we always pass true).
 *
 * **PIE stance**: NO PIE guard on any of these tools. ai.crowd.* is a runtime gameplay-dev surface
 * mirroring physics.* / debug.* / navmesh.* — the crowd manager exists only when pawns are alive
 * (typically only in PIE), so refusing the editor world for set_avoidance_quality would be
 * counter-productive. The set IS a write but it targets a transient runtime component, not an
 * editor asset, so the PIE editor-world-mutator guard doesn't apply.
 *
 * **No FScopedTransaction**: same rationale — set_avoidance_quality writes a transient runtime
 * field, not a persisted UPROPERTY on an editor asset. Undo/redo against PIE state is meaningless.
 *
 * **Avoidance quality enum**: detour's ECrowdAvoidanceQuality has 4 values (Low / Medium / Good /
 * High) — the brief only mentions Low / Medium / High. We accept the brief's 3 strings and map
 * High → ECrowdAvoidanceQuality::High (skipping Good entirely). Callers wanting Good can target
 * it through marshall.set_property on the underlying byte UPROPERTY.
 *
 * **Error codes (all reused — no new codes added)**:
 *   -32004 ObjectNotFound       actor_path doesn't resolve OR no UCrowdFollowingComponent on actor
 *   -32010 InvalidPath          malformed actor_path
 *   -32011 WrongClass           (RESERVED — currently not raised; ambiguity surfaces as -32004)
 *   -32602 InvalidParams        missing required args OR bad quality string
 *   -32603 InternalError        no world OR no UNavigationSystemV1 OR no UCrowdManager
 */
namespace FAICrowdTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	// ─── Wave J Surface 6: AI crowd tools ───────────────────────────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_GetSettings(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_ListAgents(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_SetAvoidanceQuality(const FMCPRequest& Request);
}
