// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Wave G Surface 4 — Anim Blueprint state machine surface (animbp.*). 4 user-visible tools, all Lane A.
 *
 * Tool roster:
 *   animbp.list_state_machines  → walk UAnimBlueprint anim graphs for UAnimGraphNode_StateMachine instances
 *   animbp.get_states           → enumerate UAnimStateNode + UAnimStateEntryNode inside a state machine
 *   animbp.add_state            → create a UAnimStateNode + optional UAnimGraphNode_SequencePlayer hookup
 *   animbp.add_transition       → wire UAnimStateTransitionNode between two existing states
 *
 * **Lane A justification.** All four touch UObject state (UEdGraph mutation, UAnimBlueprint asset
 * pointer, FBlueprintEditorUtils::CreateNewGraph during PostPlacedNewNode of state/transition nodes).
 * UObject pointer reads from non-GT race the GC sweeper; CreateNewGraph internally hits
 * GUObjectArray + UPackage::Modify which require GT serialisation.
 *
 * **PIE guard policy.**
 *   - list_state_machines / get_states — NOT guarded. Both are pure reads against the asset graph.
 *     During PIE the asset is shared but no compile is triggered by these walks, so PIE-safe.
 *   - add_state / add_transition — PIE-guarded with -32027 + frozen kMCPMessagePIEActive text. PIE
 *     keeps the generated UAnimBlueprintGeneratedClass live for the running instance; mutating the
 *     editor BP graph + triggering MarkBlueprintAsStructurallyModified would force a recompile that
 *     invalidates the live class. Matches the Phase 4 bp.* mutator convention.
 *
 * **Transactions + structural mod.** Mutators wrap their body in FScopedTransaction (so Ctrl-Z
 * reverts the entire add operation) and call FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified
 * AFTER the graph mutation succeeds. PostPlacedNewNode on UAnimStateNode / UAnimStateTransitionNode
 * triggers FBlueprintEditorUtils::CreateNewGraph internally (UAnimationStateGraph for state nodes,
 * UAnimationTransitionGraph for transition nodes) — those graphs are owned by the parent state
 * machine graph via Modify+SubGraphs.Add.
 *
 * **Anim Blueprint graph layout.**
 *   UAnimBlueprint  (extends UBlueprint)
 *   ├── FunctionGraphs[]                 — includes the anim graphs (one or more per BP). Each anim
 *   │   ├── UAnimationGraph (schema)       graph's Schema == UAnimationGraphSchema::StaticClass()
 *   │   │   └── Nodes[]                    — anim-graph nodes (UAnimGraphNode_Base subclasses)
 *   │   │       └── UAnimGraphNode_StateMachine (extends StateMachineBase)
 *   │   │           └── EditorStateMachineGraph: UAnimationStateMachineGraph*
 *   │   │               └── Nodes[]
 *   │   │                   ├── UAnimStateEntryNode  (one, marks start state via output pin link)
 *   │   │                   ├── UAnimStateNode       (one per state; has BoundGraph for state's anim)
 *   │   │                   └── UAnimStateTransitionNode  (one per transition; pins[0]=from, pins[1]=to)
 *   └── UbergraphPages[]                 — event graphs (NotifyPlayerOwner, NativeUpdateAnimation, etc.)
 *
 * **State name uniqueness.** UAnimStateNode::GetStateName() returns the BoundGraph's name (the
 * inner anim graph for that state). So state names ARE the inner graph names, renamed via
 * FBlueprintEditorUtils::RenameGraphWithSuggestion when AnimStateNode::PostPlacedNewNode runs. We
 * verify uniqueness before NewObject — duplicate name → -32014 PathInUse so callers don't fall
 * into the engine's auto-rename behaviour (which silently appends a suffix).
 *
 * **Sequence player hookup.** When ``anim_sequence_path`` is supplied to add_state, after the state
 * node is placed (BoundGraph allocated by PostPlacedNewNode) we:
 *   1. Load the UAnimSequence by path (reuses asset-path canonicalisation rules from AnimTools).
 *   2. Find the state's UAnimationStateGraph::MyResultNode (auto-created by PostPlacedNewNode).
 *   3. NewObject<UAnimGraphNode_SequencePlayer> inside the state's BoundGraph + AllocateDefaultPins.
 *   4. Set the sequence asset via SetAnimationAsset(UAnimSequence*).
 *   5. Connect the sequence player's "Pose" output to the result node's "Result" input.
 * If the connection fails (schema mismatch / no compatible pose pin), we leave the node placed but
 * unwired and surface ``sequence_wired: false`` in the response — caller decides whether to retry.
 *
 * **Transition wiring.** add_transition uses the canonical UAnimStateTransitionNode::CreateConnections
 * pattern (see UE source: AnimStateTransitionNode.cpp::CreateConnections at line 340):
 *   - pins[0] (input "In") is linked to FromState->GetOutputPin()
 *   - pins[1] (output "Out") is linked to ToState->GetInputPin()
 * The transition node is auto-bound to its own UAnimationTransitionGraph (boolean rule graph) by
 * PostPlacedNewNode → CreateBoundGraph.
 *
 * **Error codes (all reused — no new codes added in MCPTypes.h):**
 *   - -32004 ObjectNotFound        anim_blueprint_path, state_machine_name, or from/to_state missing
 *   - -32010 InvalidPath           anim_blueprint_path malformed / unknown mount
 *   - -32011 WrongClass            asset isn't a UAnimBlueprint OR anim_sequence_path isn't a UAnimSequence
 *   - -32014 PathInUse             add_state: state_name collides with an existing state in the SM
 *   - -32027 PIEActive             add_state / add_transition attempted during PIE
 *   - -32602 InvalidParams         missing required string arg / malformed position array
 *   - -32603 Internal              unexpected null after NewObject / PostPlacedNewNode
 *
 * Threading: GT only (``check(IsInGameThread())`` enforced).
 */
namespace FAnimBlueprintTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	UNREALMCPBRIDGE_API FMCPResponse Tool_ListStateMachines(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_GetStates(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_AddState(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_AddTransition(const FMCPRequest& Request);
}
