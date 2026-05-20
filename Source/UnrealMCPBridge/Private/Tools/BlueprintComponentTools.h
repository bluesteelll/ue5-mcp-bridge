// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Wave F Surface 3 — Blueprint SimpleConstructionScript (SCS) component surface. 4 user-visible
 * tools, all Lane A.
 *
 * Tool roster:
 *   bp.add_component          → instantiate a UActorComponent subclass as a USCS_Node on the
 *                               Blueprint's SimpleConstructionScript, optionally re-parented to
 *                               an existing SCS node (default: RootComponent / DefaultSceneRoot)
 *   bp.remove_component       → locate a USCS_Node by VariableName and remove it from the SCS;
 *                               its direct children are re-parented onto the removed node's parent
 *                               so the scene-graph stays connected (matches the BP editor "Delete"
 *                               which promotes children up one level)
 *   bp.list_components        → walk SCS->GetAllNodes (recursive=true, default) returning
 *                               { variable_name, component_class, parent_variable_name?, attach_socket? }
 *                               per node — plus the root SCS_Node name (typically "DefaultSceneRoot"
 *                               on bare Actor BPs, or the user's first scene component if it was
 *                               promoted to root via Blueprint editor)
 *   bp.set_component_default  → write a UPROPERTY on the SCS_Node's ComponentTemplate (the per-BP
 *                               "CDO" surrogate for the instantiated component) by name + type-
 *                               marshalled JSON. Reuses FMCPReflection::WritePropertyValueAt +
 *                               FMCPWritePropertyScope for round-trip parity with bp.set_node_property
 *                               and component.set_property.
 *
 * **All Lane A.** USimpleConstructionScript / USCS_Node / Blueprint compile path all assume game-
 * thread execution under the editor's transaction system. SCS structural mutations (Add/Remove) must
 * call ``FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified`` so the next compile pass
 * regenerates the BPGC's component layout; default-value writes only need MarkBlueprintAsModified.
 *
 * **PIE-guarded.** add/remove/set_component_default refuse during PIE with -32027 PIEActive — SCS
 * mutation during PIE corrupts the BP asset (PIE world uses a cloned actor tree but the BP asset's
 * SCS is shared). list is read-only and is PIE-safe.
 *
 * **FScopedTransaction.** Every mutator wraps the SCS-touching block in FScopedTransaction so
 * Ctrl-Z in the editor reverts the change. The transaction also batches Modify() calls on the
 * SCS + each affected USCS_Node.
 *
 * **Parent resolution semantics (add_component).** ``parent_component`` (optional) names an
 * existing SCS_Node by ``VariableName`` and adds the new node as that node's child via
 * ``USCS_Node::AddChildNode``. If omitted OR resolved to ``"DefaultSceneRoot"`` we use the
 * SCS->GetDefaultSceneRootNode() return when present, else the first root in
 * SCS->GetRootNodes(). When the new component is non-scene (eg UMovementComponent) the
 * ``parent_component`` argument is ignored — non-scene SCS_Nodes live as roots in the SCS
 * (matches BP editor behaviour where non-scene components don't appear under any scene parent).
 *
 * **Variable-name collision.** Pre-checked via SCS->FindSCSNode(FName) and surfaces -32014
 * PathInUse rather than allowing UE to auto-suffix the requested name (matches the bp.add_variable
 * collision contract — caller picks a unique name).
 *
 * **Class resolution (add_component).** Resolves ``component_class_path`` via LoadObject<UClass>
 * with the standard ``_C`` retry suffix for Blueprint component classes. Enforces:
 *   - -32023 InvalidClassPath  — empty / no leading slash / contains backslash
 *   - -32020 ClassNotFound     — LoadObject returned null after _C retry
 *   - -32021 ClassAbstract     — CLASS_Abstract flag set
 *   - -32011 WrongClass        — not a UActorComponent subclass
 *
 * **Removal contract.** ``bp.remove_component`` is fail-fast: missing variable name → -32004
 * ObjectNotFound. The removed node's ``GetChildNodes()`` array is captured BEFORE removal and
 * each child is reattached to the removed node's parent (via either the SCS_Node's parent's
 * AddChildNode or via SCS->AddNode if the removed node was itself a root). Returns
 * ``reparented_children_count`` so the caller can confirm the topology preserved.
 *
 * **set_component_default contract.** Writes go to ``SCS_Node->ComponentTemplate`` which is the
 * editor-time per-BP component archetype (Blueprint's "default value" for the runtime instance).
 * The 4-step PreEditChange/Modify/write/PostEditChange contract is enforced via
 * FMCPWritePropertyScope (same as bp.set_node_property and component.set_property). The edit-const
 * gate is INTENTIONALLY OMITTED here — same rationale as bp.add_variable's default_value path:
 * SCS component templates ARE the authoring surface (this is what the Blueprint editor's Details
 * panel writes to when you tune values on a placed-in-SCS component), so CPF_DisableEditOnInstance
 * is expected and shouldn't block.
 */
namespace FBlueprintComponentTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	// ─── Wave F Surface 3 — Blueprint SCS component CRUD (4 tools) ───────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_AddComponent(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_RemoveComponent(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_ListComponents(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_SetComponentDefault(const FMCPRequest& Request);
}
