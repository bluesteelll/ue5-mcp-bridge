// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Wave J Surface 1 — AI Behavior Tree introspection + runtime control surface (ai.bt.*).
 * 4 user-visible tools, all Lane A.
 *
 * Tool roster:
 *   ai.bt.list_assets     → paginated enumeration of UBehaviorTree assets via IAssetRegistry.
 *                           Returns { behavior_trees: [{ asset_path, blackboard_asset_path?,
 *                           root_node_class? }], total_known, next_page_token? }.
 *                           Read-only — no PIE guard. Mirrors data_table.list pagination pattern
 *                           (keyset-pagination by ObjectPath, base64+JSON cursor with filter
 *                           hash for staleness detection).
 *   ai.bt.get_nodes       → read full tree structure for one UBehaviorTree. Recursive walk of
 *                           RootNode (UBTCompositeNode) → Children[].ChildComposite or
 *                           Children[].ChildTask, plus Decorators[] + Services[] arrays on
 *                           composite nodes. Returns { root: { node_class, node_name,
 *                           execution_index, children: [...], decorators: [...], services: [...] } }.
 *                           Editor-only API but no PIE guard — read-only inspection.
 *   ai.bt.start_on_actor  → resolve an actor, find its AAIController, call RunBehaviorTree(BT).
 *                           Returns { started: bool, controller_path: string }. NOT PIE-guarded
 *                           — these ARE the runtime-testing tools (you'd invoke them during PIE
 *                           to validate BT changes against a live pawn). Not a bridge "mutator"
 *                           in the editor-asset sense (no FScopedTransaction, no
 *                           MarkPackageDirty).
 *   ai.bt.stop_on_actor   → resolve actor → AAIController → UBehaviorTreeComponent → StopTree().
 *                           Returns { stopped: bool, prior_active_bt?: string }. Same PIE policy
 *                           as start_on_actor.
 *
 * **Lane A justification.** Every tool touches UObject state:
 *   - ``IAssetRegistry::GetAssets`` + per-entry ``LoadObject<UBehaviorTree>`` for inspect → GT-only
 *     (Wave A Lane B audit: AR enumeration triggers GT-asserting code paths).
 *   - ``UBTNode::GetNodeName`` / ``UBTCompositeNode::Children`` UObject pointer walks → GT-only
 *     under GC sweep.
 *   - ``AAIController::RunBehaviorTree`` schedules StartTree on UBehaviorTreeComponent which
 *     allocates instance memory + runs InitializeFromAsset on every node → game-thread only.
 *   - ``UBehaviorTreeComponent::StopTree`` mutates the BT's instance stack + may emit AIMessage
 *     bus events → GT-only.
 *
 * **PIE guard policy.** NONE of the four tools is PIE-guarded:
 *   - list_assets / get_nodes are read-only inspection — safe in any world state.
 *   - start_on_actor / stop_on_actor are the runtime-testing tools — they're EXPLICITLY designed
 *     to be invoked during PIE on PIE pawns to validate BT iteration. Editor-world AAIController
 *     instances are rare in normal workflows (pawn AAIController is typically spawned at
 *     runtime), so the editor-world case is effectively "actor not found" / "no controller" →
 *     -32011 WrongClass surface.
 *
 * **No transactions.** ``RunBehaviorTree`` / ``StopTree`` mutate per-component runtime state, not
 * editor assets. Wrapping them in FScopedTransaction would leak transient instance memory into
 * the undo stack — meaningless and potentially crash-prone on PIE teardown.
 *
 * **Error codes (all reused from existing range — no new codes added in MCPTypes.h):**
 *   - -32004 ObjectNotFound       bt_path / actor_path / controller not loadable/resolvable
 *   - -32010 InvalidPath          malformed bt_path / actor_path
 *   - -32011 WrongClass           asset is not UBehaviorTree; actor has no AAIController
 *   - -32602 InvalidParams        missing required args
 *   - -32603 InternalError        AR module load failure / unexpected null in tree structure
 *
 * Threading: GT only (``check(IsInGameThread())`` enforced at top of each tool body).
 */
namespace FAIBehaviorTreeTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	UNREALMCPBRIDGE_API FMCPResponse Tool_ListAssets(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_GetNodes(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_StartOnActor(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_StopOnActor(const FMCPRequest& Request);
}
