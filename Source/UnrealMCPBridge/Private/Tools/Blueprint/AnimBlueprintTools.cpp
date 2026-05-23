// Copyright FatumGame. All Rights Reserved.

#include "AnimBlueprintTools.h"

#include "MCPSurfaceRegistry.h"

#include "FMCPDispatchQueue.h"
#include "MCPAssetLoader.h"
#include "MCPJsonBuilder.h"
#include "MCPMutatorScope.h"
#include "MCPToolHelpers.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPBlueprintUtils.h"

#include "AnimationGraph.h"
#include "AnimationGraphSchema.h"
#include "AnimationStateGraph.h"
#include "AnimationStateMachineGraph.h"
#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimGraphNode_StateResult.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimSequence.h"
#include "AnimStateEntryNode.h"
#include "AnimStateNode.h"
#include "AnimStateTransitionNode.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/Package.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// ABP_ prefix per the unity-build symbol-collision convention. The four shared helpers
	// (StampIds / MakeError / MakeSuccessObj / RequireStringField) live in FMCPToolHelpers — see
	// Phase 1 helper extraction (commit b2fd19d). UAnimSequence loader migrated to
	// FMCPAssetLoader::Load<T>; UAnimBlueprint loader stays here because it layers a typed
	// class-narrow on top of FMCPBlueprintUtils::LoadBlueprintByPath.
	constexpr int32 kABPErrorInvalidParams   = -32602;
	constexpr int32 kABPErrorInternal        = -32603;
	constexpr int32 kABPErrorObjectNotFound  = kMCPErrorObjectNotFound;       // -32004
	constexpr int32 kABPErrorInvalidPath     = kMCPErrorInvalidPath;          // -32010
	constexpr int32 kABPErrorWrongClass      = kMCPErrorWrongClass;           // -32011
	constexpr int32 kABPErrorPathInUse       = kMCPErrorPathInUse;            // -32014
	constexpr int32 kABPErrorPIEActive       = kMCPErrorPIEActive;            // -32027
	constexpr int32 kABPErrorBlueprintTypeMismatch = kMCPErrorBlueprintTypeMismatch; // -32031

	// ─── UAnimBlueprint load + class-narrow ─────────────────────────────────────────────────────────

	/**
	 * Load a UAnimBlueprint by path. Uses FMCPBlueprintUtils::LoadBlueprintByPath under the hood
	 * (UAnimBlueprint extends UBlueprint, so the generic loader resolves it), then narrows the cast.
	 *
	 * Returns null + populates OutErrorCode / OutErrorMsg on failure. Distinguishes:
	 *   - Generic load failure (path bad / asset missing / wrong class for UBlueprint) — codes
	 *     propagated verbatim from MCPBlueprintUtils (-32004 / -32010 / -32031).
	 *   - "Found a UBlueprint but not a UAnimBlueprint" — -32011 WrongClass with the actual class.
	 */
	UAnimBlueprint* ABP_LoadAnimBlueprintByPath(const FString& Path, int32& OutErrorCode, FString& OutErrorMsg)
	{
		UBlueprint* BP = FMCPBlueprintUtils::LoadBlueprintByPath(Path, OutErrorCode, OutErrorMsg);
		if (!BP)
		{
			return nullptr; // error already populated
		}
		UAnimBlueprint* ABP = Cast<UAnimBlueprint>(BP);
		if (!ABP)
		{
			OutErrorCode = kABPErrorWrongClass;
			OutErrorMsg = FString::Printf(
				TEXT("'%s' is a UBlueprint of class '%s'; expected UAnimBlueprint"),
				*Path, *BP->GetClass()->GetPathName());
			return nullptr;
		}
		return ABP;
	}

	// ─── Anim BP graph enumeration ──────────────────────────────────────────────────────────────────

	/**
	 * Is this UEdGraph an anim graph? Anim graphs use UAnimationGraphSchema (or a subclass like
	 * UAnimationStateGraphSchema for state-internal anim graphs). We restrict to AnimationGraphSchema
	 * here — state-internal graphs aren't enumerated at the top level (they live under state nodes).
	 */
	bool ABP_IsTopLevelAnimGraph(const UEdGraph* Graph)
	{
		if (!Graph) { return false; }
		const UEdGraphSchema* Schema = Graph->GetSchema();
		if (!Schema) { return false; }
		// Top-level anim graphs use UAnimationGraphSchema EXACTLY (subclasses are for state-internal
		// / transition-internal graphs which aren't enumerated here).
		return Schema->GetClass() == UAnimationGraphSchema::StaticClass();
	}

	/**
	 * Find a UAnimGraphNode_StateMachine inside an Anim Blueprint by EditorStateMachineGraph's FName.
	 * Returns null if no match. Walks every anim graph in FunctionGraphs.
	 */
	UAnimGraphNode_StateMachine* ABP_FindStateMachineNodeByName(UAnimBlueprint* ABP, const FString& SMName)
	{
		if (!ABP) { return nullptr; }
		const FName Target(*SMName);
		for (UEdGraph* Graph : ABP->FunctionGraphs)
		{
			if (!ABP_IsTopLevelAnimGraph(Graph)) { continue; }
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				UAnimGraphNode_StateMachine* SMNode = Cast<UAnimGraphNode_StateMachine>(Node);
				if (!SMNode) { continue; }
				if (SMNode->EditorStateMachineGraph &&
				    SMNode->EditorStateMachineGraph->GetFName() == Target)
				{
					return SMNode;
				}
			}
		}
		return nullptr;
	}

	/** Find a UAnimStateNode by name (== BoundGraph name == GetStateName()) inside a state machine graph. */
	UAnimStateNode* ABP_FindStateNodeByName(UAnimationStateMachineGraph* SMGraph, const FString& StateName)
	{
		if (!SMGraph) { return nullptr; }
		for (UEdGraphNode* Node : SMGraph->Nodes)
		{
			UAnimStateNode* State = Cast<UAnimStateNode>(Node);
			if (!State) { continue; }
			if (State->GetStateName() == StateName)
			{
				return State;
			}
		}
		return nullptr;
	}

	/** Find the entry node's connected output state (the SM's "start" state) — null when no entry link. */
	UAnimStateNode* ABP_FindEntryStateNode(const UAnimationStateMachineGraph* SMGraph)
	{
		if (!SMGraph || !SMGraph->EntryNode) { return nullptr; }
		UEdGraphPin* EntryOut = SMGraph->EntryNode->GetOutputPin();
		if (!EntryOut || EntryOut->LinkedTo.Num() == 0) { return nullptr; }
		// Walk the first link — entry node's output should be single-link by schema. The linked pin's
		// owning node should be a UAnimStateNode (or UAnimStateConduitNode / UAnimStateAliasNode — we
		// only flag UAnimStateNode as "entry state" since this surface doesn't enumerate conduits).
		for (UEdGraphPin* LinkedPin : EntryOut->LinkedTo)
		{
			if (!LinkedPin || !LinkedPin->GetOwningNode()) { continue; }
			if (UAnimStateNode* State = Cast<UAnimStateNode>(LinkedPin->GetOwningNode()))
			{
				return State;
			}
		}
		return nullptr;
	}

	/**
	 * Count UAnimStateNode instances inside a state machine graph (excludes entry / transitions /
	 * conduits / aliases). Returns 0 for null graph.
	 */
	int32 ABP_CountStates(const UAnimationStateMachineGraph* SMGraph)
	{
		if (!SMGraph) { return 0; }
		int32 Count = 0;
		for (const UEdGraphNode* Node : SMGraph->Nodes)
		{
			if (Cast<const UAnimStateNode>(Node)) { ++Count; }
		}
		return Count;
	}

	// ─── Inner-graph sequence player wiring (for add_state) ────────────────────────────────────────

	/**
	 * Spawn a UAnimGraphNode_SequencePlayer inside a state's anim graph + connect its pose output to
	 * the state result node's input. Returns true on full success; on partial success (node placed
	 * but pose connection failed) returns false but the node is still in the graph — caller surfaces
	 * ``sequence_wired: false``.
	 */
	bool ABP_WireSequencePlayerInState(
		UAnimStateNode* StateNode,
		UAnimSequence* SequenceAsset,
		FString& OutNotice)
	{
		check(StateNode);
		check(SequenceAsset);

		UAnimationStateGraph* InnerStateGraph = Cast<UAnimationStateGraph>(StateNode->BoundGraph);
		if (!InnerStateGraph)
		{
			OutNotice = TEXT("state's BoundGraph is not a UAnimationStateGraph — cannot wire sequence");
			return false;
		}
		UAnimGraphNode_StateResult* ResultNode = InnerStateGraph->GetResultNode();
		if (!ResultNode)
		{
			OutNotice = TEXT("state's anim graph has no StateResult node — cannot wire sequence");
			return false;
		}

		InnerStateGraph->Modify();

		UAnimGraphNode_SequencePlayer* SeqPlayer = NewObject<UAnimGraphNode_SequencePlayer>(
			InnerStateGraph, NAME_None, RF_Transactional);
		if (!SeqPlayer)
		{
			OutNotice = TEXT("NewObject<UAnimGraphNode_SequencePlayer> returned null");
			return false;
		}

		// Place left of the result node so the inner graph reads naturally.
		SeqPlayer->NodePosX = ResultNode->NodePosX - 300;
		SeqPlayer->NodePosY = ResultNode->NodePosY;
		SeqPlayer->CreateNewGuid();

		InnerStateGraph->AddNode(SeqPlayer, /*bUserAction*/ false, /*bSelectNewNode*/ false);
		SeqPlayer->PostPlacedNewNode();
		SeqPlayer->AllocateDefaultPins();

		// Set the asset via the canonical API (handles AssetPlayerBase bookkeeping + reconstruct).
		SeqPlayer->SetAnimationAsset(SequenceAsset);
		SeqPlayer->ReconstructNode();

		// Find SeqPlayer's pose output pin and the ResultNode's pose input pin and connect them.
		UEdGraphPin* PoseOut = nullptr;
		for (UEdGraphPin* Pin : SeqPlayer->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Output)
			{
				// Anim graph schema uses "Pose" as the canonical pose pin category, but pin NAME
				// varies. Match by category instead of name so future engine renames don't break us.
				if (UAnimationGraphSchema::IsLocalSpacePosePin(Pin->PinType) ||
				    UAnimationGraphSchema::IsComponentSpacePosePin(Pin->PinType))
				{
					PoseOut = Pin;
					break;
				}
			}
		}

		UEdGraphPin* ResultIn = nullptr;
		for (UEdGraphPin* Pin : ResultNode->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Input)
			{
				if (UAnimationGraphSchema::IsLocalSpacePosePin(Pin->PinType) ||
				    UAnimationGraphSchema::IsComponentSpacePosePin(Pin->PinType))
				{
					ResultIn = Pin;
					break;
				}
			}
		}

		if (!PoseOut || !ResultIn)
		{
			OutNotice = TEXT("could not locate pose pins on sequence player / state result");
			return false;
		}

		const UEdGraphSchema* Schema = InnerStateGraph->GetSchema();
		if (!Schema)
		{
			OutNotice = TEXT("state anim graph has null schema");
			return false;
		}
		const bool bConnected = Schema->TryCreateConnection(PoseOut, ResultIn);
		if (!bConnected)
		{
			OutNotice = TEXT("schema rejected pose-output → state-result connection");
			return false;
		}
		return true;
	}

	// ─── JSON helpers ───────────────────────────────────────────────────────────────────────────────

	TArray<TSharedPtr<FJsonValue>> ABP_IntPointToJsonArray(int32 X, int32 Y)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Add(MakeShared<FJsonValueNumber>(X));
		Arr.Add(MakeShared<FJsonValueNumber>(Y));
		return Arr;
	}

	/** Build per-state-node summary JSON object. */
	TSharedRef<FJsonObject> ABP_BuildStateJson(
		const UAnimStateNode* State,
		bool bIsEntry)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), State->GetStateName());
		Obj->SetArrayField(TEXT("position"),
			ABP_IntPointToJsonArray(State->NodePosX, State->NodePosY));
		Obj->SetBoolField(TEXT("is_entry"), bIsEntry);
		Obj->SetStringField(TEXT("state_node_guid"), State->NodeGuid.ToString(EGuidFormats::Digits));

		// Optional: detect a single SequencePlayer in the BoundGraph and surface its asset path.
		// This is a courtesy summary — callers that need full graph introspection should use
		// marshall.* on the BoundGraph itself.
		if (const UAnimationStateGraph* InnerGraph = Cast<UAnimationStateGraph>(State->BoundGraph))
		{
			for (const UEdGraphNode* Node : InnerGraph->Nodes)
			{
				const UAnimGraphNode_SequencePlayer* SeqPlayer = Cast<const UAnimGraphNode_SequencePlayer>(Node);
				if (!SeqPlayer) { continue; }
				if (UAnimationAsset* Asset = SeqPlayer->GetAnimationAsset())
				{
					Obj->SetStringField(TEXT("anim_sequence_path"), Asset->GetPathName());
				}
				break; // surface the first one only — multi-sequence states use blend graphs
			}
		}
		return Obj;
	}
} // namespace

namespace FAnimBlueprintTools
{

// ─── animbp.list_state_machines ────────────────────────────────────────────────────────────────
//
// Args:    { anim_blueprint_path: string }
// Result:  { state_machines: [{ name, graph_name, state_count }] }
//
// Walks UAnimBlueprint->FunctionGraphs filtered to UAnimationGraphSchema, collects every
// UAnimGraphNode_StateMachine. ``name`` and ``graph_name`` are the same value today — the SM's
// editor graph FName matches the node title — but the field is duplicated for forward-compat
// with potential future divergence.
//
// Empty result is valid: an Anim BP can have zero state machines (just an AnimGraph with a single
// pose source / blend graph).
//
// Errors: standard load errors (-32004 / -32010 / -32011 / -32031 from FMCPBlueprintUtils).
FMCPResponse Tool_ListStateMachines(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString AnimBPPath;
	FMCPResponse Err;
	// Accept both ``anim_blueprint_path`` (brief default) and ``anim_bp_path`` (matches the other
	// 3 tools in this surface for symmetry).
	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kABPErrorInvalidParams, TEXT("missing args object"));
	}
	if (!Request.Args->TryGetStringField(TEXT("anim_blueprint_path"), AnimBPPath) || AnimBPPath.IsEmpty())
	{
		if (!Request.Args->TryGetStringField(TEXT("anim_bp_path"), AnimBPPath) || AnimBPPath.IsEmpty())
		{
			return FMCPToolHelpers::MakeError(Request, kABPErrorInvalidParams,
				TEXT("missing required string field 'anim_blueprint_path' (or 'anim_bp_path')"));
		}
	}

	int32 LoadErrCode = 0;
	FString LoadErrMsg;
	UAnimBlueprint* ABP = ABP_LoadAnimBlueprintByPath(AnimBPPath, LoadErrCode, LoadErrMsg);
	if (!ABP) { return FMCPToolHelpers::MakeError(Request, LoadErrCode, LoadErrMsg); }

	TArray<TSharedPtr<FJsonValue>> SMArr;
	for (UEdGraph* Graph : ABP->FunctionGraphs)
	{
		if (!ABP_IsTopLevelAnimGraph(Graph)) { continue; }
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UAnimGraphNode_StateMachine* SMNode = Cast<UAnimGraphNode_StateMachine>(Node);
			if (!SMNode) { continue; }
			UAnimationStateMachineGraph* SMGraph = SMNode->EditorStateMachineGraph;
			if (!SMGraph) { continue; }

			TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
			const FString SMName = SMGraph->GetFName().ToString();
			Obj->SetStringField(TEXT("name"), SMName);
			Obj->SetStringField(TEXT("graph_name"), SMName);
			Obj->SetStringField(TEXT("parent_graph_name"), Graph->GetFName().ToString());
			Obj->SetNumberField(TEXT("state_count"), ABP_CountStates(SMGraph));
			Obj->SetStringField(TEXT("sm_node_guid"), SMNode->NodeGuid.ToString(EGuidFormats::Digits));
			SMArr.Add(MakeShared<FJsonValueObject>(Obj));
		}
	}

	return FMCPJsonBuilder()
		.Arr(TEXT("state_machines"), MoveTemp(SMArr))
		.Str(TEXT("anim_blueprint_path"), ABP->GetPathName())
		.BuildSuccess(Request);
}

// ─── animbp.get_states ─────────────────────────────────────────────────────────────────────────
//
// Args:    { anim_bp_path: string, state_machine_name: string }
// Result:  { states: [{ name, state_node_guid, position: [x,y], is_entry: bool, anim_sequence_path? }] }
//
// Walks the named state machine's UAnimationStateMachineGraph and reports every UAnimStateNode.
// The entry-state determination follows the EntryNode's output-pin link (so the SM's "starting"
// state is flagged regardless of node ordering). Optional ``anim_sequence_path`` surfaces the
// first UAnimGraphNode_SequencePlayer found in the state's BoundGraph — useful for the common
// "single anim per state" pattern.
//
// Errors: standard load errors + -32004 ObjectNotFound when state_machine_name doesn't resolve.
FMCPResponse Tool_GetStates(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString AnimBPPath, SMName;
	FMCPResponse Err;
	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kABPErrorInvalidParams, TEXT("missing args object"));
	}
	if (!Request.Args->TryGetStringField(TEXT("anim_bp_path"), AnimBPPath) || AnimBPPath.IsEmpty())
	{
		// Tolerate ``anim_blueprint_path`` for symmetry with list_state_machines.
		if (!Request.Args->TryGetStringField(TEXT("anim_blueprint_path"), AnimBPPath) || AnimBPPath.IsEmpty())
		{
			return FMCPToolHelpers::MakeError(Request, kABPErrorInvalidParams,
				TEXT("missing required string field 'anim_bp_path' (or 'anim_blueprint_path')"));
		}
	}
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("state_machine_name"), SMName, Err)) { return Err; }

	int32 LoadErrCode = 0;
	FString LoadErrMsg;
	UAnimBlueprint* ABP = ABP_LoadAnimBlueprintByPath(AnimBPPath, LoadErrCode, LoadErrMsg);
	if (!ABP) { return FMCPToolHelpers::MakeError(Request, LoadErrCode, LoadErrMsg); }

	UAnimGraphNode_StateMachine* SMNode = ABP_FindStateMachineNodeByName(ABP, SMName);
	if (!SMNode || !SMNode->EditorStateMachineGraph)
	{
		return FMCPToolHelpers::MakeError(Request, kABPErrorObjectNotFound,
			FString::Printf(TEXT("state machine '%s' not found in anim blueprint '%s'"),
				*SMName, *AnimBPPath));
	}
	UAnimationStateMachineGraph* SMGraph = SMNode->EditorStateMachineGraph;
	const UAnimStateNode* EntryState = ABP_FindEntryStateNode(SMGraph);

	TArray<TSharedPtr<FJsonValue>> StatesArr;
	for (UEdGraphNode* Node : SMGraph->Nodes)
	{
		UAnimStateNode* State = Cast<UAnimStateNode>(Node);
		if (!State) { continue; }
		const bool bIsEntry = (State == EntryState);
		StatesArr.Add(MakeShared<FJsonValueObject>(ABP_BuildStateJson(State, bIsEntry)));
	}

	return FMCPJsonBuilder()
		.Arr(TEXT("states"), MoveTemp(StatesArr))
		.Str(TEXT("state_machine_name"), SMName)
		.BuildSuccess(Request);
}

// ─── animbp.add_state ──────────────────────────────────────────────────────────────────────────
//
// Args:    { anim_bp_path: string, state_machine_name: string, state_name: string,
//            position?: [x, y] (default [0, 0]), anim_sequence_path?: string }
// Result:  { state_node_guid, state_name, sequence_wired?: bool }
//
// Steps:
//   1. PIE guard.
//   2. Resolve anim BP + state machine graph (-32004 / -32011 as appropriate).
//   3. Verify state name uniqueness — -32014 PathInUse on collision (rejects engine's silent rename).
//   4. FMCPMutatorScope holds FScopedTransaction; SMGraph->Modify().
//   5. NewObject<UAnimStateNode> + CreateNewGuid + place + PostPlacedNewNode + AllocateDefaultPins.
//      PostPlacedNewNode creates the inner UAnimationStateGraph + auto-creates the State Result node.
//   6. Rename the inner BoundGraph to match the requested state_name (since AnimStateNode::GetStateName
//      returns BoundGraph->GetName() — so renaming the inner graph IS the state name).
//   7. If anim_sequence_path supplied → ABP_WireSequencePlayerInState attempts the sequence player +
//      pose connection. Returns sequence_wired in response (true / false) so caller knows.
//   8. FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(ABP).
//
// Errors: PIE, load, -32014 PathInUse, -32602 InvalidParams (bad position array),
//         -32011 WrongClass (anim_sequence_path resolves to non-UAnimSequence).
FMCPResponse Tool_AddState(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_AddAnimState", "Add Anim State"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString AnimBPPath, SMName, StateName;
	FMCPResponse Err;
	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kABPErrorInvalidParams, TEXT("missing args object"));
	}
	if (!Request.Args->TryGetStringField(TEXT("anim_bp_path"), AnimBPPath) || AnimBPPath.IsEmpty())
	{
		if (!Request.Args->TryGetStringField(TEXT("anim_blueprint_path"), AnimBPPath) || AnimBPPath.IsEmpty())
		{
			return FMCPToolHelpers::MakeError(Request, kABPErrorInvalidParams,
				TEXT("missing required string field 'anim_bp_path' (or 'anim_blueprint_path')"));
		}
	}
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("state_machine_name"), SMName,    Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("state_name"),         StateName, Err)) { return Err; }

	int32 PosX = 0, PosY = 0;
	const TArray<TSharedPtr<FJsonValue>>* PositionArr = nullptr;
	if (Request.Args->TryGetArrayField(TEXT("position"), PositionArr) && PositionArr)
	{
		if (PositionArr->Num() != 2)
		{
			return FMCPToolHelpers::MakeError(Request, kABPErrorInvalidParams,
				FString::Printf(TEXT("'position' must be [x, y] (2 numbers); got %d entries"),
					PositionArr->Num()));
		}
		double X = 0.0, Y = 0.0;
		if (!(*PositionArr)[0]->TryGetNumber(X) || !(*PositionArr)[1]->TryGetNumber(Y))
		{
			return FMCPToolHelpers::MakeError(Request, kABPErrorInvalidParams,
				TEXT("'position' entries must be numbers"));
		}
		PosX = static_cast<int32>(X);
		PosY = static_cast<int32>(Y);
	}

	FString SequencePath;
	const bool bHasSequence = Request.Args->TryGetStringField(TEXT("anim_sequence_path"), SequencePath)
		&& !SequencePath.IsEmpty();

	// ─── Resolve anim BP + state machine ────────────────────────────────────────────────────────
	int32 LoadErrCode = 0;
	FString LoadErrMsg;
	UAnimBlueprint* ABP = ABP_LoadAnimBlueprintByPath(AnimBPPath, LoadErrCode, LoadErrMsg);
	if (!ABP) { return FMCPToolHelpers::MakeError(Request, LoadErrCode, LoadErrMsg); }

	UAnimGraphNode_StateMachine* SMNode = ABP_FindStateMachineNodeByName(ABP, SMName);
	if (!SMNode || !SMNode->EditorStateMachineGraph)
	{
		return FMCPToolHelpers::MakeError(Request, kABPErrorObjectNotFound,
			FString::Printf(TEXT("state machine '%s' not found in anim blueprint '%s'"),
				*SMName, *AnimBPPath));
	}
	UAnimationStateMachineGraph* SMGraph = SMNode->EditorStateMachineGraph;

	// ─── State name uniqueness check (pre-mutation, no transaction overhead on bad input) ──────
	if (ABP_FindStateNodeByName(SMGraph, StateName) != nullptr)
	{
		return FMCPToolHelpers::MakeError(Request, kABPErrorPathInUse,
			FString::Printf(TEXT("state '%s' already exists in state machine '%s'"),
				*StateName, *SMName));
	}

	// ─── Resolve optional sequence asset BEFORE mutating graph state ────────────────────────────
	UAnimSequence* SequenceAsset = nullptr;
	if (bHasSequence)
	{
		int32 SeqErrCode = 0;
		FString SeqErrMsg;
		SequenceAsset = FMCPAssetLoader::Load<UAnimSequence>(SequencePath, SeqErrCode, SeqErrMsg);
		if (!SequenceAsset)
		{
			return FMCPToolHelpers::MakeError(Request, SeqErrCode, SeqErrMsg);
		}
	}

	// ─── Mutate graph under transaction (held by FMCPMutatorScope) ──────────────────────────────
	ABP->Modify();
	SMGraph->Modify();

	UAnimStateNode* NewState = NewObject<UAnimStateNode>(
		SMGraph, NAME_None, RF_Transactional);
	if (!NewState)
	{
		Scope.Abort();
		return FMCPToolHelpers::MakeError(Request, kABPErrorInternal,
			TEXT("NewObject<UAnimStateNode> returned null"));
	}
	NewState->NodePosX = PosX;
	NewState->NodePosY = PosY;
	NewState->CreateNewGuid();

	// AddNode BEFORE PostPlacedNewNode — PostPlacedNewNode walks GetGraph() to add the inner state
	// graph to ParentGraph->SubGraphs. AddNode sets Node->Outer == Graph implicitly via SMGraph being
	// the construction outer.
	SMGraph->AddNode(NewState, /*bUserAction*/ false, /*bSelectNewNode*/ false);

	// PostPlacedNewNode creates BoundGraph (UAnimationStateGraph with auto-named "State" suffix) +
	// the default UAnimGraphNode_StateResult node inside it. After this call the state IS placeable
	// — we then rename the BoundGraph to make GetStateName() return the user's requested name.
	NewState->PostPlacedNewNode();
	NewState->AllocateDefaultPins();

	if (!NewState->BoundGraph)
	{
		Scope.Abort();
		return FMCPToolHelpers::MakeError(Request, kABPErrorInternal,
			TEXT("PostPlacedNewNode did not create BoundGraph on UAnimStateNode"));
	}

	// Rename the inner BoundGraph to user-requested name — this is what GetStateName() returns.
	// FBlueprintEditorUtils::RenameGraph handles the package + outer rename + observer notification.
	FBlueprintEditorUtils::RenameGraph(NewState->BoundGraph, StateName);

	// Optional sequence player wiring (inside the state's inner anim graph).
	bool bSequenceWired = false;
	FString SequenceWireNotice;
	if (SequenceAsset)
	{
		bSequenceWired = ABP_WireSequencePlayerInState(NewState, SequenceAsset, SequenceWireNotice);
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(ABP);

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("state_node_guid"), NewState->NodeGuid.ToString(EGuidFormats::Digits));
	Out->SetStringField(TEXT("state_name"), NewState->GetStateName());
	Out->SetArrayField(TEXT("position"), ABP_IntPointToJsonArray(NewState->NodePosX, NewState->NodePosY));
	if (bHasSequence)
	{
		Out->SetBoolField(TEXT("sequence_wired"), bSequenceWired);
		if (!bSequenceWired && !SequenceWireNotice.IsEmpty())
		{
			Out->SetStringField(TEXT("sequence_notice"), SequenceWireNotice);
		}
	}
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── animbp.add_transition ─────────────────────────────────────────────────────────────────────
//
// Args:    { anim_bp_path: string, state_machine_name: string, from_state: string, to_state: string }
// Result:  { transition_node_guid, from_state, to_state }
//
// Wires an unconditional transition between two existing states using the canonical pattern from
// the engine source (UAnimStateTransitionNode::CreateConnections, AnimStateTransitionNode.cpp:340):
//   - pins[0] (input "In") gets MakeLinkTo from FromState->GetOutputPin()
//   - pins[1] (output "Out") gets MakeLinkTo from ToState->GetInputPin()
// PostPlacedNewNode creates the boolean-rule UAnimationTransitionGraph that the transition uses for
// its predicate (default: always-true → transition always available). Callers wanting non-trivial
// rules edit that graph via bp.add_node / bp.connect_pins (the transition's BoundGraph FName is
// derivable from the transition node Guid).
//
// Errors: PIE, load, -32004 ObjectNotFound (state_machine_name / from_state / to_state not found),
//         -32602 InvalidParams (from == to is rejected — state machines support self-transitions but
//         require the special "Create Self Transition" path which lives in AnimGraphCommands, not
//         this surface).
FMCPResponse Tool_AddTransition(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_AddAnimStateTransition", "Add Anim State Transition"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString AnimBPPath, SMName, FromStateName, ToStateName;
	FMCPResponse Err;
	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kABPErrorInvalidParams, TEXT("missing args object"));
	}
	if (!Request.Args->TryGetStringField(TEXT("anim_bp_path"), AnimBPPath) || AnimBPPath.IsEmpty())
	{
		if (!Request.Args->TryGetStringField(TEXT("anim_blueprint_path"), AnimBPPath) || AnimBPPath.IsEmpty())
		{
			return FMCPToolHelpers::MakeError(Request, kABPErrorInvalidParams,
				TEXT("missing required string field 'anim_bp_path' (or 'anim_blueprint_path')"));
		}
	}
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("state_machine_name"), SMName,        Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("from_state"),         FromStateName, Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("to_state"),           ToStateName,   Err)) { return Err; }

	if (FromStateName == ToStateName)
	{
		return FMCPToolHelpers::MakeError(Request, kABPErrorInvalidParams,
			FString::Printf(TEXT("from_state == to_state ('%s'); self-transitions require the "
				"engine's CreateSelfTransition path which is outside this surface"),
				*FromStateName));
	}

	int32 LoadErrCode = 0;
	FString LoadErrMsg;
	UAnimBlueprint* ABP = ABP_LoadAnimBlueprintByPath(AnimBPPath, LoadErrCode, LoadErrMsg);
	if (!ABP) { return FMCPToolHelpers::MakeError(Request, LoadErrCode, LoadErrMsg); }

	UAnimGraphNode_StateMachine* SMNode = ABP_FindStateMachineNodeByName(ABP, SMName);
	if (!SMNode || !SMNode->EditorStateMachineGraph)
	{
		return FMCPToolHelpers::MakeError(Request, kABPErrorObjectNotFound,
			FString::Printf(TEXT("state machine '%s' not found in anim blueprint '%s'"),
				*SMName, *AnimBPPath));
	}
	UAnimationStateMachineGraph* SMGraph = SMNode->EditorStateMachineGraph;

	UAnimStateNode* FromState = ABP_FindStateNodeByName(SMGraph, FromStateName);
	if (!FromState)
	{
		return FMCPToolHelpers::MakeError(Request, kABPErrorObjectNotFound,
			FString::Printf(TEXT("from_state '%s' not found in state machine '%s'"),
				*FromStateName, *SMName));
	}
	UAnimStateNode* ToState = ABP_FindStateNodeByName(SMGraph, ToStateName);
	if (!ToState)
	{
		return FMCPToolHelpers::MakeError(Request, kABPErrorObjectNotFound,
			FString::Printf(TEXT("to_state '%s' not found in state machine '%s'"),
				*ToStateName, *SMName));
	}

	// ─── Mutate graph under transaction (held by FMCPMutatorScope) ──────────────────────────────
	ABP->Modify();
	SMGraph->Modify();
	FromState->Modify();
	ToState->Modify();

	UAnimStateTransitionNode* TransNode = NewObject<UAnimStateTransitionNode>(
		SMGraph, NAME_None, RF_Transactional);
	if (!TransNode)
	{
		Scope.Abort();
		return FMCPToolHelpers::MakeError(Request, kABPErrorInternal,
			TEXT("NewObject<UAnimStateTransitionNode> returned null"));
	}

	// Mid-point position between source and destination for cosmetic placement.
	TransNode->NodePosX = (FromState->NodePosX + ToState->NodePosX) / 2;
	TransNode->NodePosY = (FromState->NodePosY + ToState->NodePosY) / 2;
	TransNode->CreateNewGuid();

	SMGraph->AddNode(TransNode, /*bUserAction*/ false, /*bSelectNewNode*/ false);

	// PostPlacedNewNode internally calls CreateBoundGraph which spawns the predicate rule graph
	// (UAnimationTransitionGraph) with the default "Return true" Result node. The transition is
	// available immediately — no user-supplied rule expression yet.
	TransNode->PostPlacedNewNode();
	TransNode->AllocateDefaultPins();

	// Wire pins using the canonical engine pattern (CreateConnections, AnimStateTransitionNode.cpp:340).
	// CreateConnections does Modify() on both pin endpoints + LinkedTo.Empty + MakeLinkTo.
	TransNode->CreateConnections(FromState, ToState);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(ABP);

	return FMCPJsonBuilder()
		.Str(TEXT("transition_node_guid"), TransNode->NodeGuid.ToString(EGuidFormats::Digits))
		.Str(TEXT("from_state"), FromStateName)
		.Str(TEXT("to_state"),   ToStateName)
		.Arr(TEXT("position"), ABP_IntPointToJsonArray(TransNode->NodePosX, TransNode->NodePosY))
		.BuildSuccess(Request);
}

// ─── Registration ──────────────────────────────────────────────────────────────────────────────
void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	RegisterTool(TEXT("animbp.list_state_machines"), &Tool_ListStateMachines, /*Lane A*/ false);
	RegisterTool(TEXT("animbp.get_states"),          &Tool_GetStates,         /*Lane A*/ false);
	RegisterTool(TEXT("animbp.add_state"),           &Tool_AddState,          /*Lane A*/ false);
	RegisterTool(TEXT("animbp.add_transition"),      &Tool_AddTransition,     /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("AnimBlueprint surface registered: 4 animbp.* tools "
			 "(list_state_machines + get_states + add_state + add_transition), all Lane A"));
}

} // namespace FAnimBlueprintTools

#undef LOCTEXT_NAMESPACE

MCP_REGISTER_SURFACE(AnimBlueprintTools, &FAnimBlueprintTools::Register)
