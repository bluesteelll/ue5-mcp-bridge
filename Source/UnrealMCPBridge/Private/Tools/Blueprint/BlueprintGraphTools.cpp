// Copyright FatumGame. All Rights Reserved.

#include "BlueprintGraphTools.h"

#include "MCPSurfaceRegistry.h"

#include "FMCPDispatchQueue.h"
#include "MCPJsonBuilder.h"
#include "MCPMutatorScope.h"
#include "MCPToolHelpers.h"
#include "MCPClassResolver.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPBlueprintUtils.h"
#include "Utils/MCPPinTypeUtils.h"
#include "Utils/MCPReflection.h"
#include "Utils/MCPWorldContext.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "K2Node.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_CallFunction.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "ScopedTransaction.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// BGT_ prefix retained for any helper unique to this surface.
	constexpr int32 kBGTErrorInvalidParams = kMCPErrorInvalidParams;
	constexpr int32 kBGTErrorInternal      = kMCPErrorInternal;

	/**
	 * Find a graph by name across UbergraphPages + FunctionGraphs + MacroGraphs.
	 * Returns nullptr if not found.
	 */
	UEdGraph* BGT_FindGraphByName(UBlueprint* Blueprint, const FString& GraphName)
	{
		if (!Blueprint) { return nullptr; }
		const FName Target(*GraphName);

		// 1. Event graphs (UbergraphPages — canonical default is "EventGraph").
		for (UEdGraph* G : Blueprint->UbergraphPages)
		{
			if (G && G->GetFName() == Target) { return G; }
		}
		// 2. User function graphs + construction script.
		for (UEdGraph* G : Blueprint->FunctionGraphs)
		{
			if (G && G->GetFName() == Target) { return G; }
		}
		// 3. Macro graphs.
		for (UEdGraph* G : Blueprint->MacroGraphs)
		{
			if (G && G->GetFName() == Target) { return G; }
		}
		return nullptr;
	}

	/** Find a node in a graph by Guid string. Returns nullptr if not found. */
	UEdGraphNode* BGT_FindNodeByGuid(UEdGraph* Graph, const FString& GuidString)
	{
		if (!Graph) { return nullptr; }
		FGuid Guid;
		if (!FGuid::Parse(GuidString, Guid)) { return nullptr; }
		for (UEdGraphNode* N : Graph->Nodes)
		{
			if (N && N->NodeGuid == Guid) { return N; }
		}
		return nullptr;
	}

	/**
	 * Snapshot a pin's current default state as a JSON object — used by ``bp.set_pin_default`` to
	 * report ``prior_default`` and ``new_default``. Surface BOTH ``default_value`` (the string the
	 * schema produces for primitives / enums / structs) AND ``default_object`` (the path of a hard
	 * UObject default), since callers can't always predict which slot the schema chose. ``null``
	 * fields encode "not set" — a clean default for an unconnected primitive pin emits
	 * ``{default_value:"", default_object:null, default_text:""}`` whereas an object pin holding a
	 * mesh reference emits ``{default_value:"", default_object:"/Game/...", default_text:""}``.
	 */
	TSharedRef<FJsonObject> BGT_BuildPinDefaultSnapshot(const UEdGraphPin* Pin)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (!Pin) { return Obj; }
		Obj->SetStringField(TEXT("default_value"), Pin->DefaultValue);
		if (Pin->DefaultObject)
		{
			Obj->SetStringField(TEXT("default_object"), Pin->DefaultObject->GetPathName());
		}
		else
		{
			Obj->SetField(TEXT("default_object"), MakeShared<FJsonValueNull>());
		}
		Obj->SetStringField(TEXT("default_text"), Pin->DefaultTextValue.ToString());
		return Obj;
	}

	/** Build JSON {name, direction, pin_type} for a single UEdGraphPin (no LinkedTo for brevity). */
	TSharedRef<FJsonObject> BGT_BuildPinSummary(const UEdGraphPin* Pin)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (!Pin) { return Obj; }
		Obj->SetStringField(TEXT("name"), Pin->PinName.ToString());
		Obj->SetStringField(TEXT("direction"),
			Pin->Direction == EGPD_Input ? TEXT("input") :
			Pin->Direction == EGPD_Output ? TEXT("output") : TEXT("unknown"));
		Obj->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());
		if (Pin->PinType.PinSubCategoryObject.IsValid())
		{
			Obj->SetStringField(TEXT("subcategory_object"),
				Pin->PinType.PinSubCategoryObject->GetPathName());
		}
		Obj->SetStringField(TEXT("container"),
			Pin->PinType.ContainerType == EPinContainerType::Array ? TEXT("array") :
			Pin->PinType.ContainerType == EPinContainerType::Set   ? TEXT("set") :
			Pin->PinType.ContainerType == EPinContainerType::Map   ? TEXT("map") : TEXT("none"));
		Obj->SetBoolField(TEXT("is_reference"), Pin->PinType.bIsReference);
		return Obj;
	}

	// ─── Wave O helpers ────────────────────────────────────────────────────────────────────────────
	//
	// Shared infrastructure for the 7 Wave-O tools (5 specialised node spawners + 1 component-bound
	// event + 1 full pin enumeration). All Lane A — game-thread UObject mutation.

	/**
	 * Resolve a class path (``/Script/...`` or ``/Game/...``) to a UClass, and verify it is a
	 * subclass of ``BaseClass`` (typically ``UObject::StaticClass()`` for cast targets, or
	 * ``AActor::StaticClass()`` for actor bound events).
	 *
	 * Returns the resolved class on success. On failure populates ``OutError`` with a human-readable
	 * description and returns nullptr.
	 *
	 * Distinct from ``BP_ResolveClassOrError`` in BlueprintTools.cpp — this is a thin helper that
	 * caller wraps in its own ``MakeError`` call with the appropriate code (the caller knows whether
	 * the failure should map to -32020 ClassNotFound or -32011 WrongClass).
	 */
	// BPG_ResolveClass removed; replaced by FMCPClassResolver::Resolve (Wave Q2).
	// Lenient options: bRejectAbstract=false (cast TargetType / generic ClassRef pins accept
	// abstract bases like AActor); bRequirePathPrefix=false (BPG was tolerant of unprefixed
	// names historically; the shared helper's default is stricter, so we opt out here).

	/**
	 * Common K2-node post-construction: AddNode + CreateNewGuid + PostPlacedNewNode +
	 * AllocateDefaultPins. Mirrors the pattern in ``Tool_AddNode``. ``ReconstructNode`` is NOT called
	 * here — callers that set type-binding state via dedicated setters (SetMacroGraph, TargetType,
	 * StructType, InitializeComponentBoundEventParams, InputAction) may need to call it themselves
	 * after AllocateDefaultPins so pin layout reflects the binding.
	 *
	 * ``Node->NodePosX/Y`` are set HERE (after CreateNewGuid) — guid generation does not depend on
	 * position and we want position set before pin layout in case a subclass's AllocateDefaultPins
	 * reads NodePosX/Y (none in stock UE do, but defensive).
	 */
	void BPG_FinalizeNewNode(UK2Node* Node, UEdGraph* Graph, int32 X, int32 Y)
	{
		check(Node);
		check(Graph);
		Node->NodePosX = X;
		Node->NodePosY = Y;
		Node->CreateNewGuid();
		Graph->AddNode(Node, /*bUserAction*/ false, /*bSelectNewNode*/ false);
		Node->PostPlacedNewNode();
		Node->AllocateDefaultPins();
	}

	/** ``FBlueprintEditorUtils::MarkBlueprintAsModified(BP)`` wrapper for symmetry with other helpers. */
	void BPG_MarkModified(UBlueprint* BP)
	{
		check(BP);
		FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
	}

	/**
	 * Full pin summary for ``bp.list_node_pins`` AND post-construction response arrays for the
	 * specialised add_* tools. Compared to ``BGT_BuildPinSummary`` (the brief 5-field summary used
	 * by ``bp.add_node`` / ``bp.connect_pins``), this emits:
	 *   - pin_id (Guid stringified)
	 *   - sub_category (PinType.PinSubCategory.ToString())
	 *   - is_connected (LinkedTo.Num() > 0)
	 *   - default_value / default_object / default_text
	 *   - linked_pins [{node_guid, pin_id}]
	 *   - hidden (only emitted when true, per Risk R7)
	 *   - advanced_view (only when true)
	 *
	 * Reuses BGT_BuildPinSummary's category/subcategory_object/container shape so callers can
	 * compose both shapes from the same pin without divergent field naming.
	 */
	TSharedRef<FJsonObject> BPG_BuildPinSummary(const UEdGraphPin* Pin)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (!Pin) { return Obj; }

		Obj->SetStringField(TEXT("name"), Pin->PinName.ToString());
		Obj->SetStringField(TEXT("direction"),
			Pin->Direction == EGPD_Input ? TEXT("input") :
			Pin->Direction == EGPD_Output ? TEXT("output") : TEXT("unknown"));
		Obj->SetStringField(TEXT("pin_id"), Pin->PinId.ToString(EGuidFormats::DigitsWithHyphens));
		Obj->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());
		Obj->SetStringField(TEXT("sub_category"), Pin->PinType.PinSubCategory.ToString());
		if (Pin->PinType.PinSubCategoryObject.IsValid())
		{
			Obj->SetStringField(TEXT("sub_category_object"),
				Pin->PinType.PinSubCategoryObject->GetPathName());
		}
		Obj->SetStringField(TEXT("container_type"),
			Pin->PinType.ContainerType == EPinContainerType::Array ? TEXT("array") :
			Pin->PinType.ContainerType == EPinContainerType::Set   ? TEXT("set") :
			Pin->PinType.ContainerType == EPinContainerType::Map   ? TEXT("map") : TEXT("none"));
		Obj->SetBoolField(TEXT("is_reference"), Pin->PinType.bIsReference);
		Obj->SetBoolField(TEXT("is_connected"), Pin->LinkedTo.Num() > 0);
		Obj->SetStringField(TEXT("default_value"), Pin->DefaultValue);
		if (Pin->DefaultObject)
		{
			Obj->SetStringField(TEXT("default_object"), Pin->DefaultObject->GetPathName());
		}
		else
		{
			Obj->SetField(TEXT("default_object"), MakeShared<FJsonValueNull>());
		}
		Obj->SetStringField(TEXT("default_text"), Pin->DefaultTextValue.ToString());

		// LinkedTo: one entry per connected pin. Skip dangling pointers (corrupt BP) by guarding
		// the owning node via GetOwningNodeUnchecked + IsValid.
		TArray<TSharedPtr<FJsonValue>> Linked;
		Linked.Reserve(Pin->LinkedTo.Num());
		for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
		{
			if (!LinkedPin) { continue; }
			const UEdGraphNode* OwningNode = LinkedPin->GetOwningNodeUnchecked();
			if (!OwningNode || !IsValid(OwningNode)) { continue; }
			TSharedRef<FJsonObject> Edge = MakeShared<FJsonObject>();
			Edge->SetStringField(TEXT("node_guid"),
				OwningNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
			Edge->SetStringField(TEXT("pin_id"),
				LinkedPin->PinId.ToString(EGuidFormats::DigitsWithHyphens));
			Linked.Add(MakeShared<FJsonValueObject>(Edge));
		}
		Obj->SetArrayField(TEXT("linked_pins"), Linked);

		// Optional flags — only emit when true to keep the common case compact.
		if (Pin->bHidden) { Obj->SetBoolField(TEXT("hidden"), true); }
		if (Pin->bAdvancedView) { Obj->SetBoolField(TEXT("advanced_view"), true); }

		return Obj;
	}

	/**
	 * Build the standard ``pins: [...]`` array for a freshly-spawned node, reusing
	 * BPG_BuildPinSummary so the add_* tools and list_node_pins emit IDENTICAL pin shapes (caller
	 * code that consumes one can consume the other).
	 */
	TArray<TSharedPtr<FJsonValue>> BPG_PinsArray(const UK2Node* Node)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		if (!Node) { return Out; }
		Out.Reserve(Node->Pins.Num());
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin) { Out.Add(MakeShared<FJsonValueObject>(BPG_BuildPinSummary(Pin))); }
		}
		return Out;
	}
} // namespace

namespace FBlueprintGraphTools
{

// ─── bp.add_node ───────────────────────────────────────────────────────────────────────────────
//
// Args:    { blueprint_path: string,
//            node_class:     string  (e.g. "/Script/BlueprintGraph.K2Node_VariableGet"),
//            graph_name?:    string  (default "EventGraph"),
//            position?:      [x, y]  (default [0, 0]),
//            variable_name?: string  (K2Node_Variable* — sets VariableReference SelfMember),
//            function_name?: string  (K2Node_CallFunction — sets FunctionReference Name),
//            function_class?: string (K2Node_CallFunction — owning class path; self if omitted),
//            event_name?:    string  (K2Node_CustomEvent — CustomFunctionName) }
// Result:  { node_guid, node_class, position: [x, y], pins: [{name, direction, category, ...}] }
//
// Errors: standard kMCPError* + -32050 GraphNotFound.
FMCPResponse Tool_AddNode(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_AddBPNode", "Add Blueprint Node"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString BlueprintPath;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("blueprint_path"), BlueprintPath, Err)) { return Err; }

	FString NodeClassPath;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("node_class"), NodeClassPath, Err)) { return Err; }

	FString GraphName = TEXT("EventGraph");
	Request.Args->TryGetStringField(TEXT("graph_name"), GraphName);

	int32 PosX = 0, PosY = 0;
	const TArray<TSharedPtr<FJsonValue>>* PositionArr = nullptr;
	if (Request.Args->TryGetArrayField(TEXT("position"), PositionArr) && PositionArr && PositionArr->Num() == 2)
	{
		PosX = static_cast<int32>((*PositionArr)[0]->AsNumber());
		PosY = static_cast<int32>((*PositionArr)[1]->AsNumber());
	}

	// ─── Resolve blueprint + graph ──────────────────────────────────────────────────────────────
	int32 LoadErrCode = 0;
	FString LoadErrMsg;
	UBlueprint* Blueprint = FMCPBlueprintUtils::LoadBlueprintByPath(BlueprintPath, LoadErrCode, LoadErrMsg);
	if (!Blueprint) { return FMCPToolHelpers::MakeError(Request, LoadErrCode, LoadErrMsg); }

	UEdGraph* Graph = BGT_FindGraphByName(Blueprint, GraphName);
	if (!Graph)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorGraphNotFound,
			FString::Printf(
				TEXT("graph '%s' not found on blueprint '%s' (searched UbergraphPages/FunctionGraphs/MacroGraphs)"),
				*GraphName, *BlueprintPath));
	}

	// ─── Resolve node class ─────────────────────────────────────────────────────────────────────
	UClass* NodeClass = LoadObject<UClass>(nullptr, *NodeClassPath);
	if (!NodeClass)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorClassNotFound,
			FString::Printf(TEXT("node_class '%s' could not be loaded — expected e.g. "
				"'/Script/BlueprintGraph.K2Node_VariableGet'"), *NodeClassPath));
	}
	if (!NodeClass->IsChildOf(UK2Node::StaticClass()))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorWrongClass,
			FString::Printf(TEXT("node_class '%s' is not a UK2Node subclass (class hierarchy: %s)"),
				*NodeClassPath, *NodeClass->GetSuperClass()->GetPathName()));
	}
	if (NodeClass->HasAnyClassFlags(CLASS_Abstract))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorClassAbstract,
			FString::Printf(TEXT("node_class '%s' is abstract — cannot instantiate"), *NodeClassPath));
	}

	// ─── Construct + configure + place ──────────────────────────────────────────────────────────
	Blueprint->Modify();
	Graph->Modify();
	Scope.DirtyPackage(Blueprint->GetPackage());

	UK2Node* NewNode = NewObject<UK2Node>(Graph, NodeClass, NAME_None, RF_Transactional);

	// Type-specific config — apply BEFORE AllocateDefaultPins so pin layout reflects the binding.
	FString VariableName, FunctionName, FunctionClassPath, EventName;
	Request.Args->TryGetStringField(TEXT("variable_name"), VariableName);
	Request.Args->TryGetStringField(TEXT("function_name"), FunctionName);
	Request.Args->TryGetStringField(TEXT("function_class"), FunctionClassPath);
	Request.Args->TryGetStringField(TEXT("event_name"), EventName);

	if (UK2Node_VariableGet* VarGet = Cast<UK2Node_VariableGet>(NewNode))
	{
		if (!VariableName.IsEmpty())
		{
			VarGet->VariableReference.SetSelfMember(FName(*VariableName));
		}
	}
	else if (UK2Node_VariableSet* VarSet = Cast<UK2Node_VariableSet>(NewNode))
	{
		if (!VariableName.IsEmpty())
		{
			VarSet->VariableReference.SetSelfMember(FName(*VariableName));
		}
	}
	else if (UK2Node_CallFunction* CallFn = Cast<UK2Node_CallFunction>(NewNode))
	{
		if (!FunctionName.IsEmpty())
		{
			UClass* OwnerClass = Blueprint->ParentClass; // default to self
			if (!FunctionClassPath.IsEmpty())
			{
				if (UClass* Resolved = LoadObject<UClass>(nullptr, *FunctionClassPath))
				{
					OwnerClass = Resolved;
				}
			}
			if (OwnerClass)
			{
				CallFn->FunctionReference.SetExternalMember(FName(*FunctionName), OwnerClass);
			}
			else
			{
				CallFn->FunctionReference.SetSelfMember(FName(*FunctionName));
			}
		}
	}
	else if (UK2Node_CustomEvent* CustomEvent = Cast<UK2Node_CustomEvent>(NewNode))
	{
		if (!EventName.IsEmpty())
		{
			CustomEvent->CustomFunctionName = FName(*EventName);
		}
	}
	// Other K2Node subclasses: caller wires details via subsequent marshall.write_property calls.

	NewNode->NodePosX = PosX;
	NewNode->NodePosY = PosY;
	NewNode->CreateNewGuid();

	Graph->AddNode(NewNode, /*bUserAction*/ false, /*bSelectNewNode*/ false);
	NewNode->PostPlacedNewNode();
	NewNode->AllocateDefaultPins();

	// For function-call style nodes the pin set depends on the resolved UFunction signature;
	// ReconstructNode() rebuilds pins to match. Cheap no-op for nodes that already have correct pins.
	NewNode->ReconstructNode();

	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	// ─── Build response ─────────────────────────────────────────────────────────────────────────
	TArray<TSharedPtr<FJsonValue>> PositionResp;
	PositionResp.Add(MakeShared<FJsonValueNumber>(NewNode->NodePosX));
	PositionResp.Add(MakeShared<FJsonValueNumber>(NewNode->NodePosY));

	TArray<TSharedPtr<FJsonValue>> PinArr;
	PinArr.Reserve(NewNode->Pins.Num());
	for (const UEdGraphPin* Pin : NewNode->Pins)
	{
		if (Pin)
		{
			PinArr.Add(MakeShared<FJsonValueObject>(BGT_BuildPinSummary(Pin)));
		}
	}

	return FMCPJsonBuilder()
		.Str(TEXT("node_guid"), NewNode->NodeGuid.ToString(EGuidFormats::Digits))
		.Str(TEXT("node_class"), NodeClass->GetPathName())
		.Str(TEXT("title"), NewNode->GetNodeTitle(ENodeTitleType::ListView).ToString())
		.Arr(TEXT("position"), MoveTemp(PositionResp))
		.Arr(TEXT("pins"), MoveTemp(PinArr))
		.BuildSuccess(Request);
}

// ─── bp.connect_pins ───────────────────────────────────────────────────────────────────────────
//
// Args:    { blueprint_path, graph_name?, from_node, from_pin, to_node, to_pin }
// Result:  { connected: bool, broke_existing_count: int, response: string }
//
// Errors: -32050 GraphNotFound, -32051 NodeNotFound, -32052 PinNotFound, -32053 PinConnectionRefused.
FMCPResponse Tool_ConnectPins(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_ConnectPins", "Connect Blueprint Pins"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString BlueprintPath, FromNodeGuid, FromPinName, ToNodeGuid, ToPinName;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("blueprint_path"), BlueprintPath, Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("from_node"),      FromNodeGuid,  Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("from_pin"),       FromPinName,   Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("to_node"),        ToNodeGuid,    Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("to_pin"),         ToPinName,     Err)) { return Err; }

	FString GraphName = TEXT("EventGraph");
	Request.Args->TryGetStringField(TEXT("graph_name"), GraphName);

	int32 LoadErrCode = 0;
	FString LoadErrMsg;
	UBlueprint* Blueprint = FMCPBlueprintUtils::LoadBlueprintByPath(BlueprintPath, LoadErrCode, LoadErrMsg);
	if (!Blueprint) { return FMCPToolHelpers::MakeError(Request, LoadErrCode, LoadErrMsg); }

	UEdGraph* Graph = BGT_FindGraphByName(Blueprint, GraphName);
	if (!Graph)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorGraphNotFound,
			FString::Printf(TEXT("graph '%s' not found on blueprint '%s'"), *GraphName, *BlueprintPath));
	}

	UEdGraphNode* FromNode = BGT_FindNodeByGuid(Graph, FromNodeGuid);
	if (!FromNode)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorNodeNotFound,
			FString::Printf(TEXT("from_node '%s' not found in graph '%s'"), *FromNodeGuid, *GraphName));
	}
	UEdGraphNode* ToNode = BGT_FindNodeByGuid(Graph, ToNodeGuid);
	if (!ToNode)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorNodeNotFound,
			FString::Printf(TEXT("to_node '%s' not found in graph '%s'"), *ToNodeGuid, *GraphName));
	}

	UEdGraphPin* FromPin = FromNode->FindPin(FName(*FromPinName));
	if (!FromPin)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPinNotFound,
			FString::Printf(TEXT("from_pin '%s' not found on node '%s'"),
				*FromPinName, *FromNode->GetNodeTitle(ENodeTitleType::ListView).ToString()));
	}
	UEdGraphPin* ToPin = ToNode->FindPin(FName(*ToPinName));
	if (!ToPin)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPinNotFound,
			FString::Printf(TEXT("to_pin '%s' not found on node '%s'"),
				*ToPinName, *ToNode->GetNodeTitle(ENodeTitleType::ListView).ToString()));
	}

	const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Graph->GetSchema());
	if (!Schema)
	{
		return FMCPToolHelpers::MakeError(Request, kBGTErrorInternal,
			FString::Printf(TEXT("graph '%s' schema is not UEdGraphSchema_K2 (class=%s)"),
				*GraphName, *Graph->GetSchema()->GetClass()->GetPathName()));
	}

	// CanCreateConnection reports the schema's verdict + reason BEFORE we modify state, so we can
	// surface a clean PinConnectionRefused error.
	const FPinConnectionResponse CanConnect = Schema->CanCreateConnection(FromPin, ToPin);
	if (CanConnect.Response == CONNECT_RESPONSE_DISALLOW)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPinConnectionRefused,
			FString::Printf(TEXT("schema rejected connection '%s.%s' → '%s.%s': %s"),
				*FromNode->GetNodeTitle(ENodeTitleType::ListView).ToString(), *FromPinName,
				*ToNode->GetNodeTitle(ENodeTitleType::ListView).ToString(), *ToPinName,
				*CanConnect.Message.ToString()));
	}

	Blueprint->Modify();
	Graph->Modify();
	FromNode->Modify();
	ToNode->Modify();
	Scope.DirtyPackage(Blueprint->GetPackage());

	// Count link counts BEFORE so we can report break-existing semantics from
	// CONNECT_RESPONSE_BREAK_OTHERS_A/B/AB.
	const int32 PriorFromLinks = FromPin->LinkedTo.Num();
	const int32 PriorToLinks   = ToPin->LinkedTo.Num();

	const bool bConnected = Schema->TryCreateConnection(FromPin, ToPin);

	const int32 PostFromLinks = FromPin->LinkedTo.Num();
	const int32 PostToLinks   = ToPin->LinkedTo.Num();
	// Break delta: if a side was made unique it now holds exactly 1 link (the new one); prior link
	// count minus current new-link contribution = number broken. We compute (Prior + 1 - Post) per
	// side and sum; clamps to >= 0.
	const int32 BrokeFrom = FMath::Max(0, (PriorFromLinks + 1) - PostFromLinks);
	const int32 BrokeTo   = FMath::Max(0, (PriorToLinks   + 1) - PostToLinks);
	const int32 BrokeTotal = BrokeFrom + BrokeTo;

	if (bConnected)
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	}

	return FMCPJsonBuilder()
		.Bool(TEXT("connected"), bConnected)
		.Num(TEXT("broke_existing_count"), static_cast<double>(BrokeTotal))
		.Str(TEXT("response"), CanConnect.Message.ToString())
		.BuildSuccess(Request);
}

// ─── bp.set_node_property ──────────────────────────────────────────────────────────────────────
//
// Args:    { blueprint_path, graph_name?, node_guid, property_name, value: <JSON> }
// Result:  { node_guid, property_name, prior_value, new_value }
//
// Reuses the Phase-2 ``FMCPReflection::WritePropertyValueAt`` pipeline so JSON-typed values for
// vectors / enums / object refs / structs round-trip identically to ``marshall.write_property``.
// Wrapped in ``FMCPWritePropertyScope`` for the 4-step (PreEditChange → Modify → write →
// PostEditChangeProperty) contract. ``Node->ReconstructNode`` follows the write so pin layout
// reflects property-driven pin changes (e.g. ``bIsPureFunc`` toggling exec pins).
//
// Errors: -32050 GraphNotFound, -32051 NodeNotFound, -32005 PropertyNotFound,
//         -32006 PropertyTypeMismatch, -32027 PIEActive.
FMCPResponse Tool_SetNodeProperty(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (FMCPWorldContext::IsPIEActive())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPIEActive, kMCPMessagePIEActive);
	}

	FString BlueprintPath, NodeGuidStr, PropertyName;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("blueprint_path"),  BlueprintPath, Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("node_guid"),       NodeGuidStr,   Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("property_name"),   PropertyName,  Err)) { return Err; }

	const TSharedPtr<FJsonValue> ValueField = Request.Args->TryGetField(TEXT("value"));
	if (!ValueField.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kBGTErrorInvalidParams,
			TEXT("missing required field 'value' (any JSON value)"));
	}

	FString GraphName = TEXT("EventGraph");
	Request.Args->TryGetStringField(TEXT("graph_name"), GraphName);

	int32 LoadErrCode = 0;
	FString LoadErrMsg;
	UBlueprint* Blueprint = FMCPBlueprintUtils::LoadBlueprintByPath(BlueprintPath, LoadErrCode, LoadErrMsg);
	if (!Blueprint) { return FMCPToolHelpers::MakeError(Request, LoadErrCode, LoadErrMsg); }

	UEdGraph* Graph = BGT_FindGraphByName(Blueprint, GraphName);
	if (!Graph)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorGraphNotFound,
			FString::Printf(TEXT("graph '%s' not found on blueprint '%s'"), *GraphName, *BlueprintPath));
	}

	UEdGraphNode* Node = BGT_FindNodeByGuid(Graph, NodeGuidStr);
	if (!Node)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorNodeNotFound,
			FString::Printf(TEXT("node '%s' not found in graph '%s'"), *NodeGuidStr, *GraphName));
	}

	FProperty* Prop = Node->GetClass()->FindPropertyByName(FName(*PropertyName));
	if (!Prop)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPropertyNotFound,
			FString::Printf(TEXT("property '%s' not found on node class '%s'"),
				*PropertyName, *Node->GetClass()->GetPathName()));
	}

	void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Node);

	// Snapshot prior value via the same reader marshall.read_property uses.
	TSharedPtr<FJsonValue> PriorValue = FMCPReflection::ReadPropertyValueAt(Prop, ValuePtr);

	// Write through the scoped 4-step contract. FScopedTransaction inside the scope participates
	// in editor Undo. ``Node`` is both the container (FProperty offset target) AND the owner
	// (ImportText_Direct outer for text-fallback path resolution).
	FString WriteError;
	bool bWriteOk = false;
	{
		FMCPWritePropertyScope Scope(Node, Prop, LOCTEXT("MCP_SetNodeProperty", "MCP: bp.set_node_property"));
		bWriteOk = FMCPReflection::WritePropertyValueAt(Prop, ValuePtr, ValueField, Node, WriteError);
	}
	// PostEditChangeProperty has fired on Scope destructor by this point.

	if (!bWriteOk)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPropertyTypeMismatch,
			FString::Printf(TEXT("write rejected on '%s.%s': %s"),
				*Node->GetClass()->GetName(), *PropertyName, *WriteError));
	}

	// Re-read AFTER the write so ``new_value`` reflects any schema-side normalisation that
	// ImportText_Direct may have applied (enum case-folding, numeric clamps, etc.).
	TSharedPtr<FJsonValue> NewValue = FMCPReflection::ReadPropertyValueAt(Prop, ValuePtr);

	// Refresh pin layout — some K2Node properties (bIsPureFunc, bIsConst, FunctionReference)
	// change which pins are visible. Cheap no-op for nodes whose pins don't depend on the property.
	Node->ReconstructNode();

	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	const TSharedRef<FJsonValue> PriorValueOut = PriorValue.IsValid()
		? PriorValue.ToSharedRef()
		: TSharedRef<FJsonValue>(MakeShared<FJsonValueNull>());
	const TSharedRef<FJsonValue> NewValueOut = NewValue.IsValid()
		? NewValue.ToSharedRef()
		: TSharedRef<FJsonValue>(MakeShared<FJsonValueNull>());

	return FMCPJsonBuilder()
		.Str(TEXT("node_guid"),     Node->NodeGuid.ToString(EGuidFormats::Digits))
		.Str(TEXT("node_class"),    Node->GetClass()->GetPathName())
		.Str(TEXT("property_name"), PropertyName)
		.Field(TEXT("prior_value"), PriorValueOut)
		.Field(TEXT("new_value"),   NewValueOut)
		.BuildSuccess(Request);
}

// ─── bp.set_pin_default ────────────────────────────────────────────────────────────────────────
//
// Args:    { blueprint_path, graph_name?, node_guid, pin_name, value: <JSON> }
// Result:  { node_guid, pin_name, prior_default, new_default }
//
// Refuses pins with LinkedTo.Num() > 0 (a connected pin uses the linked value, not the default).
// Object pins (PC_Object/PC_Class) with a string-shaped ``value`` route through
// ``UEdGraphSchema_K2::TrySetDefaultObject`` (LoadObject path). All other pin shapes route through
// ``UEdGraphSchema_K2::TrySetDefaultValue`` — the schema's string-parsing entry point that handles
// primitives, enums, structs, soft refs, and emits an FFormatNamedArguments diagnostic on failure
// via ``IsPinDefaultValid``.
//
// Numeric / boolean JSON values are stringified for the schema (it parses everything from FString).
//
// Errors: -32050 GraphNotFound, -32051 NodeNotFound, -32052 PinNotFound,
//         -32602 InvalidParams (pin is connected), -32006 PropertyTypeMismatch (schema rejected),
//         -32027 PIEActive.
FMCPResponse Tool_SetPinDefault(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_SetPinDefault", "MCP: bp.set_pin_default"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString BlueprintPath, NodeGuidStr, PinName;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("blueprint_path"), BlueprintPath, Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("node_guid"),      NodeGuidStr,   Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("pin_name"),       PinName,       Err)) { return Err; }

	const TSharedPtr<FJsonValue> ValueField = Request.Args->TryGetField(TEXT("value"));
	if (!ValueField.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kBGTErrorInvalidParams,
			TEXT("missing required field 'value' (string / number / bool / null for default reset)"));
	}

	FString GraphName = TEXT("EventGraph");
	Request.Args->TryGetStringField(TEXT("graph_name"), GraphName);

	int32 LoadErrCode = 0;
	FString LoadErrMsg;
	UBlueprint* Blueprint = FMCPBlueprintUtils::LoadBlueprintByPath(BlueprintPath, LoadErrCode, LoadErrMsg);
	if (!Blueprint) { return FMCPToolHelpers::MakeError(Request, LoadErrCode, LoadErrMsg); }

	UEdGraph* Graph = BGT_FindGraphByName(Blueprint, GraphName);
	if (!Graph)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorGraphNotFound,
			FString::Printf(TEXT("graph '%s' not found on blueprint '%s'"), *GraphName, *BlueprintPath));
	}

	UEdGraphNode* Node = BGT_FindNodeByGuid(Graph, NodeGuidStr);
	if (!Node)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorNodeNotFound,
			FString::Printf(TEXT("node '%s' not found in graph '%s'"), *NodeGuidStr, *GraphName));
	}

	UEdGraphPin* Pin = Node->FindPin(FName(*PinName));
	if (!Pin)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPinNotFound,
			FString::Printf(TEXT("pin '%s' not found on node '%s' (class %s)"),
				*PinName, *Node->GetNodeTitle(ENodeTitleType::ListView).ToString(),
				*Node->GetClass()->GetName()));
	}

	if (Pin->LinkedTo.Num() > 0)
	{
		return FMCPToolHelpers::MakeError(Request, kBGTErrorInvalidParams,
			FString::Printf(TEXT("pin '%s' is connected to %d link(s); disconnect first via "
				"bp.disconnect_pin or bp.connect_pins (break-others response)"),
				*PinName, Pin->LinkedTo.Num()));
	}

	const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Pin->GetSchema());
	if (!Schema)
	{
		return FMCPToolHelpers::MakeError(Request, kBGTErrorInternal,
			FString::Printf(TEXT("pin '%s' schema is not UEdGraphSchema_K2 (class=%s)"),
				*PinName, *Pin->GetSchema()->GetClass()->GetPathName()));
	}

	// Snapshot prior default for the response BEFORE schema modifies it.
	TSharedRef<FJsonObject> PriorDefault = BGT_BuildPinDefaultSnapshot(Pin);

	Blueprint->Modify();
	Graph->Modify();
	Node->Modify();
	Scope.DirtyPackage(Blueprint->GetPackage());

	// Coerce JSON value to FString for the schema. Distinguish object-ref shape (PC_Object/PC_Class
	// + string-shaped value) so we can use TrySetDefaultObject (LoadObject). Otherwise route through
	// TrySetDefaultValue — the schema's universal string parser.
	const bool bIsHardObjectCategory =
		Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Object ||
		Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Class;
	FString ValueAsString;
	bool bWroteAsObject = false;

	// Null value → reset to autogenerated default.
	if (ValueField->Type == EJson::Null)
	{
		ValueAsString = Pin->AutogeneratedDefaultValue;
	}
	else if (ValueField->Type == EJson::String)
	{
		ValueAsString = ValueField->AsString();
	}
	else if (ValueField->Type == EJson::Number)
	{
		ValueAsString = FString::SanitizeFloat(ValueField->AsNumber());
	}
	else if (ValueField->Type == EJson::Boolean)
	{
		ValueAsString = ValueField->AsBool() ? TEXT("true") : TEXT("false");
	}
	else
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPropertyTypeMismatch,
			FString::Printf(TEXT("unsupported JSON value type for pin '%s' default — "
				"expected string/number/bool/null, got JSON type %d"), *PinName,
				static_cast<int32>(ValueField->Type)));
	}

	// Hard object pin path: load the UObject by string path then bind via TrySetDefaultObject.
	// TrySetDefaultObject does NOT also exist for SoftObject / SoftClass — those use the path
	// string straight through TrySetDefaultValue (which serialises the FSoftObjectPath internally).
	if (bIsHardObjectCategory && ValueField->Type == EJson::String && !ValueAsString.IsEmpty())
	{
		UObject* TargetObject = LoadObject<UObject>(nullptr, *ValueAsString);
		if (!TargetObject)
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
				FString::Printf(TEXT("could not resolve object path '%s' for pin '%s' (PC_Object/PC_Class default)"),
					*ValueAsString, *PinName));
		}
		Schema->TrySetDefaultObject(*Pin, TargetObject, /*bMarkAsModified=*/ true);
		bWroteAsObject = true;
	}
	else
	{
		Schema->TrySetDefaultValue(*Pin, ValueAsString, /*bMarkAsModified=*/ true);
	}

	// Schema's TrySetDefault* validates internally via IsPinDefaultValid; on validation failure it
	// silently leaves the pin's DefaultValue/DefaultObject unchanged. Detect this by comparing
	// before/after — if neither slot changed, surface as PropertyTypeMismatch with the input.
	TSharedRef<FJsonObject> NewDefault = BGT_BuildPinDefaultSnapshot(Pin);
	{
		const FString PriorValStr  = PriorDefault->GetStringField(TEXT("default_value"));
		const FString NewValStr    = NewDefault->GetStringField(TEXT("default_value"));
		const FString PriorObjStr  = PriorDefault->HasTypedField<EJson::String>(TEXT("default_object"))
			? PriorDefault->GetStringField(TEXT("default_object")) : FString();
		const FString NewObjStr    = NewDefault->HasTypedField<EJson::String>(TEXT("default_object"))
			? NewDefault->GetStringField(TEXT("default_object")) : FString();
		// Surface mismatch ONLY when caller actually supplied something other than the prior — a
		// no-op (e.g. setting the existing value again) is a successful write.
		const bool bDidChange = PriorValStr != NewValStr || PriorObjStr != NewObjStr;
		const bool bDesiredChange = !ValueAsString.IsEmpty() &&
			(bWroteAsObject ? PriorObjStr != ValueAsString : PriorValStr != ValueAsString);
		if (bDesiredChange && !bDidChange)
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorPropertyTypeMismatch,
				FString::Printf(TEXT("schema rejected default '%s' for pin '%s' (category=%s) — "
					"value does not satisfy IsPinDefaultValid"),
					*ValueAsString, *PinName, *Pin->PinType.PinCategory.ToString()));
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	return FMCPJsonBuilder()
		.Str(TEXT("node_guid"), Node->NodeGuid.ToString(EGuidFormats::Digits))
		.Str(TEXT("pin_name"),  PinName)
		.ObjectShared(TEXT("prior_default"), PriorDefault)
		.ObjectShared(TEXT("new_default"),   NewDefault)
		.BuildSuccess(Request);
}

// ─── bp.delete_node ────────────────────────────────────────────────────────────────────────────
//
// Args:    { blueprint_path, graph_name?, node_guid }
// Result:  { deleted: bool, node_class, links_broken: int }
//
// Refuses K2Node_FunctionEntry / K2Node_FunctionResult / K2Node_Event (excluding K2Node_CustomEvent
// — user-created events are deletable). Defense-in-depth ``CanUserDeleteNode()`` guard catches
// any future engine-blessed undeletable subclass (Composites, Tunnels, etc.). ``Graph->RemoveNode``
// auto-breaks all linked pins; ``links_broken`` is the count summed across all pins BEFORE removal.
//
// Errors: -32050 GraphNotFound, -32051 NodeNotFound, -32602 InvalidParams (entry/result/event),
//         -32027 PIEActive.
FMCPResponse Tool_DeleteNode(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_DeleteBPNode", "MCP: bp.delete_node"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString BlueprintPath, NodeGuidStr;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("blueprint_path"), BlueprintPath, Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("node_guid"),      NodeGuidStr,   Err)) { return Err; }

	FString GraphName = TEXT("EventGraph");
	Request.Args->TryGetStringField(TEXT("graph_name"), GraphName);

	int32 LoadErrCode = 0;
	FString LoadErrMsg;
	UBlueprint* Blueprint = FMCPBlueprintUtils::LoadBlueprintByPath(BlueprintPath, LoadErrCode, LoadErrMsg);
	if (!Blueprint) { return FMCPToolHelpers::MakeError(Request, LoadErrCode, LoadErrMsg); }

	UEdGraph* Graph = BGT_FindGraphByName(Blueprint, GraphName);
	if (!Graph)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorGraphNotFound,
			FString::Printf(TEXT("graph '%s' not found on blueprint '%s'"), *GraphName, *BlueprintPath));
	}

	UEdGraphNode* Node = BGT_FindNodeByGuid(Graph, NodeGuidStr);
	if (!Node)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorNodeNotFound,
			FString::Printf(TEXT("node '%s' not found in graph '%s'"), *NodeGuidStr, *GraphName));
	}

	// Refuse entry/result/event terminators per Wave F1 brief. K2Node_CustomEvent is a subclass of
	// K2Node_Event but represents a user-created event — DELETABLE. The Cast<UK2Node_CustomEvent>
	// exclusion is what differentiates the two cases at this layer.
	const bool bIsFunctionEntry  = Node->IsA<UK2Node_FunctionEntry>();
	const bool bIsFunctionResult = Node->IsA<UK2Node_FunctionResult>();
	const bool bIsBuiltinEvent   = Node->IsA<UK2Node_Event>() && !Node->IsA<UK2Node_CustomEvent>();
	if (bIsFunctionEntry || bIsFunctionResult || bIsBuiltinEvent)
	{
		const TCHAR* WhichKind = bIsFunctionEntry  ? TEXT("FunctionEntry")
			                   : bIsFunctionResult ? TEXT("FunctionResult")
			                                       : TEXT("Event");
		return FMCPToolHelpers::MakeError(Request, kBGTErrorInvalidParams,
			FString::Printf(TEXT("cannot delete %s node '%s' (class=%s) — entry/result/builtin-event "
				"nodes anchor the graph and are undeletable; remove the graph itself via bp.remove_function "
				"or unbind via the editor"),
				WhichKind, *NodeGuidStr, *Node->GetClass()->GetName()));
	}

	// Defense-in-depth — catches K2Node_Tunnel / K2Node_Composite / future undeletables.
	if (!Node->CanUserDeleteNode())
	{
		return FMCPToolHelpers::MakeError(Request, kBGTErrorInvalidParams,
			FString::Printf(TEXT("node class '%s' reports CanUserDeleteNode=false; engine refuses deletion"),
				*Node->GetClass()->GetName()));
	}

	// Count links across all pins so the caller can compare against subsequent connect_pins ops.
	int32 LinksToBreak = 0;
	for (const UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin) { LinksToBreak += Pin->LinkedTo.Num(); }
	}

	const FString NodeClassPath = Node->GetClass()->GetPathName();
	const FString NodeTitle     = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();

	Blueprint->Modify();
	Graph->Modify();
	Node->Modify();
	Scope.DirtyPackage(Blueprint->GetPackage());

	// RemoveNode internally calls Pin->BreakAllPinLinks() on every pin then drops the node from
	// Graph->Nodes. Pass bBreakAllLinks=true (default) explicitly for clarity.
	Graph->RemoveNode(Node);

	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	return FMCPJsonBuilder()
		.Bool(TEXT("deleted"),       true)
		.Str(TEXT("node_class"),     NodeClassPath)
		.Str(TEXT("node_title"),     NodeTitle)
		.Num(TEXT("links_broken"),   static_cast<double>(LinksToBreak))
		.BuildSuccess(Request);
}

// ─── bp.disconnect_pin ─────────────────────────────────────────────────────────────────────────
//
// Args:    { blueprint_path, graph_name?, node_guid, pin_name }
// Result:  { pin_name, links_broken: int }
//
// Calls ``Pin->BreakAllPinLinks(true)`` — the ``true`` arg tells UE to notify connected pins'
// nodes via ``PinConnectionListChanged`` so they can react (e.g. K2 wildcards collapsing back
// to ``Wildcard`` category after losing their type-inducing connection).
//
// Errors: -32050 GraphNotFound, -32051 NodeNotFound, -32052 PinNotFound, -32027 PIEActive.
FMCPResponse Tool_DisconnectPin(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_DisconnectPin", "MCP: bp.disconnect_pin"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString BlueprintPath, NodeGuidStr, PinName;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("blueprint_path"), BlueprintPath, Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("node_guid"),      NodeGuidStr,   Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("pin_name"),       PinName,       Err)) { return Err; }

	FString GraphName = TEXT("EventGraph");
	Request.Args->TryGetStringField(TEXT("graph_name"), GraphName);

	int32 LoadErrCode = 0;
	FString LoadErrMsg;
	UBlueprint* Blueprint = FMCPBlueprintUtils::LoadBlueprintByPath(BlueprintPath, LoadErrCode, LoadErrMsg);
	if (!Blueprint) { return FMCPToolHelpers::MakeError(Request, LoadErrCode, LoadErrMsg); }

	UEdGraph* Graph = BGT_FindGraphByName(Blueprint, GraphName);
	if (!Graph)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorGraphNotFound,
			FString::Printf(TEXT("graph '%s' not found on blueprint '%s'"), *GraphName, *BlueprintPath));
	}

	UEdGraphNode* Node = BGT_FindNodeByGuid(Graph, NodeGuidStr);
	if (!Node)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorNodeNotFound,
			FString::Printf(TEXT("node '%s' not found in graph '%s'"), *NodeGuidStr, *GraphName));
	}

	UEdGraphPin* Pin = Node->FindPin(FName(*PinName));
	if (!Pin)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPinNotFound,
			FString::Printf(TEXT("pin '%s' not found on node '%s'"),
				*PinName, *Node->GetNodeTitle(ENodeTitleType::ListView).ToString()));
	}

	const int32 PriorLinks = Pin->LinkedTo.Num();

	if (PriorLinks == 0)
	{
		// No-op succeeds — caller may be in an idempotent disconnect loop. Return links_broken=0
		// rather than erroring so the caller's iteration completes cleanly.
		Scope.Abort();
		return FMCPJsonBuilder()
			.Str(TEXT("pin_name"), PinName)
			.Num(TEXT("links_broken"), 0.0)
			.BuildSuccess(Request);
	}

	Blueprint->Modify();
	Graph->Modify();
	Node->Modify();
	Scope.DirtyPackage(Blueprint->GetPackage());
	// Also Modify() all linked-to nodes so the transaction captures their pin state too — needed
	// for clean Undo.
	for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
	{
		if (LinkedPin && LinkedPin->GetOwningNode()) { LinkedPin->GetOwningNode()->Modify(); }
	}

	Pin->BreakAllPinLinks(/*bNotifyNodes=*/ true);

	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	return FMCPJsonBuilder()
		.Str(TEXT("pin_name"), PinName)
		.Num(TEXT("links_broken"), static_cast<double>(PriorLinks))
		.BuildSuccess(Request);
}

// ─── bp.move_node ──────────────────────────────────────────────────────────────────────────────
//
// Args:    { blueprint_path, graph_name?, node_guid, position: [x, y] }
// Result:  { node_guid, prior_position: [x, y], new_position: [x, y] }
//
// Pure cosmetic mutation — NodePosX / NodePosY are graph editor layout coords (int32, no unit).
// FScopedTransaction still wraps the change so Ctrl-Z reverts the move alongside any other
// transactions of the same session. Bypasses ``Node->ReconstructNode`` (position doesn't affect
// pin layout) for speed.
//
// Errors: -32050 GraphNotFound, -32051 NodeNotFound, -32602 InvalidParams (missing position),
//         -32027 PIEActive.
FMCPResponse Tool_MoveNode(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_MoveBPNode", "MCP: bp.move_node"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString BlueprintPath, NodeGuidStr;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("blueprint_path"), BlueprintPath, Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("node_guid"),      NodeGuidStr,   Err)) { return Err; }

	const TArray<TSharedPtr<FJsonValue>>* PositionArr = nullptr;
	if (!Request.Args->TryGetArrayField(TEXT("position"), PositionArr) || !PositionArr || PositionArr->Num() != 2)
	{
		return FMCPToolHelpers::MakeError(Request, kBGTErrorInvalidParams,
			TEXT("missing/invalid 'position' field — expected [x, y] number array of length 2"));
	}

	FString GraphName = TEXT("EventGraph");
	Request.Args->TryGetStringField(TEXT("graph_name"), GraphName);

	int32 LoadErrCode = 0;
	FString LoadErrMsg;
	UBlueprint* Blueprint = FMCPBlueprintUtils::LoadBlueprintByPath(BlueprintPath, LoadErrCode, LoadErrMsg);
	if (!Blueprint) { return FMCPToolHelpers::MakeError(Request, LoadErrCode, LoadErrMsg); }

	UEdGraph* Graph = BGT_FindGraphByName(Blueprint, GraphName);
	if (!Graph)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorGraphNotFound,
			FString::Printf(TEXT("graph '%s' not found on blueprint '%s'"), *GraphName, *BlueprintPath));
	}

	UEdGraphNode* Node = BGT_FindNodeByGuid(Graph, NodeGuidStr);
	if (!Node)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorNodeNotFound,
			FString::Printf(TEXT("node '%s' not found in graph '%s'"), *NodeGuidStr, *GraphName));
	}

	const int32 PriorX = Node->NodePosX;
	const int32 PriorY = Node->NodePosY;
	const int32 NewX = static_cast<int32>((*PositionArr)[0]->AsNumber());
	const int32 NewY = static_cast<int32>((*PositionArr)[1]->AsNumber());

	Blueprint->Modify();
	Graph->Modify();
	Node->Modify();
	Scope.DirtyPackage(Blueprint->GetPackage());

	Node->NodePosX = NewX;
	Node->NodePosY = NewY;

	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	TArray<TSharedPtr<FJsonValue>> PriorPosArr;
	PriorPosArr.Add(MakeShared<FJsonValueNumber>(PriorX));
	PriorPosArr.Add(MakeShared<FJsonValueNumber>(PriorY));

	TArray<TSharedPtr<FJsonValue>> NewPosArr;
	NewPosArr.Add(MakeShared<FJsonValueNumber>(NewX));
	NewPosArr.Add(MakeShared<FJsonValueNumber>(NewY));

	return FMCPJsonBuilder()
		.Str(TEXT("node_guid"), Node->NodeGuid.ToString(EGuidFormats::Digits))
		.Arr(TEXT("prior_position"), MoveTemp(PriorPosArr))
		.Arr(TEXT("new_position"),   MoveTemp(NewPosArr))
		.BuildSuccess(Request);
}

// ─── bp.add_comment ────────────────────────────────────────────────────────────────────────────
//
// Args:    { blueprint_path: string, graph_name?: string ("EventGraph"),
//            position: [x, y], size?: [w, h] ([300, 200]), text: string,
//            color?: [r, g, b, a] ([0.5, 0.5, 0.5, 1.0]) }
// Result:  { node_guid: string, position: [x, y], size: [w, h], text: string, graph_name: string }
//
// Comment nodes are cosmetic boxes that visually group regions of a graph. They live on the same
// UEdGraph as K2Node subclasses (added via Graph->AddNode) and survive recompiles. Color components
// MUST be supplied as four floats (RGBA, 0..1 each); failure to pass an array of length 4 falls
// back to the default mid-grey. NodeWidth / NodeHeight live on the base UEdGraphNode class — they
// are int32 graph-editor units, no real-world unit.
//
// PIE-guarded; FScopedTransaction-wrapped. ``Comment->CreateNewGuid()`` assigns a fresh FGuid which
// is returned in the response for subsequent deletion via ``bp.delete_comment``.
//
// Errors: -32050 GraphNotFound, -32602 InvalidParams (missing position/text), -32027 PIEActive.
FMCPResponse Tool_AddComment(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_AddCommentNode", "MCP: bp.add_comment"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString BlueprintPath, CommentText;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("blueprint_path"), BlueprintPath, Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("text"),           CommentText,   Err)) { return Err; }

	FString GraphName = TEXT("EventGraph");
	Request.Args->TryGetStringField(TEXT("graph_name"), GraphName);

	// Position: required [x, y] number array.
	const TArray<TSharedPtr<FJsonValue>>* PositionArr = nullptr;
	if (!Request.Args->TryGetArrayField(TEXT("position"), PositionArr) || !PositionArr || PositionArr->Num() != 2)
	{
		return FMCPToolHelpers::MakeError(Request, kBGTErrorInvalidParams,
			TEXT("missing/invalid 'position' field — expected [x, y] number array of length 2"));
	}
	const int32 PosX = static_cast<int32>((*PositionArr)[0]->AsNumber());
	const int32 PosY = static_cast<int32>((*PositionArr)[1]->AsNumber());

	// Size: optional [w, h]; default [300, 200] matches the BP editor's "Add Comment" hotkey default.
	int32 SizeW = 300;
	int32 SizeH = 200;
	const TArray<TSharedPtr<FJsonValue>>* SizeArr = nullptr;
	if (Request.Args->TryGetArrayField(TEXT("size"), SizeArr) && SizeArr && SizeArr->Num() == 2)
	{
		SizeW = static_cast<int32>((*SizeArr)[0]->AsNumber());
		SizeH = static_cast<int32>((*SizeArr)[1]->AsNumber());
	}

	// Color: optional [r, g, b, a] floats. Default mid-grey (matches engine default for new comments).
	FLinearColor CommentColor(0.5f, 0.5f, 0.5f, 1.0f);
	const TArray<TSharedPtr<FJsonValue>>* ColorArr = nullptr;
	if (Request.Args->TryGetArrayField(TEXT("color"), ColorArr) && ColorArr && ColorArr->Num() == 4)
	{
		CommentColor.R = static_cast<float>((*ColorArr)[0]->AsNumber());
		CommentColor.G = static_cast<float>((*ColorArr)[1]->AsNumber());
		CommentColor.B = static_cast<float>((*ColorArr)[2]->AsNumber());
		CommentColor.A = static_cast<float>((*ColorArr)[3]->AsNumber());
	}

	int32 LoadErrCode = 0;
	FString LoadErrMsg;
	UBlueprint* Blueprint = FMCPBlueprintUtils::LoadBlueprintByPath(BlueprintPath, LoadErrCode, LoadErrMsg);
	if (!Blueprint) { return FMCPToolHelpers::MakeError(Request, LoadErrCode, LoadErrMsg); }

	UEdGraph* Graph = BGT_FindGraphByName(Blueprint, GraphName);
	if (!Graph)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorGraphNotFound,
			FString::Printf(TEXT("graph '%s' not found on blueprint '%s'"), *GraphName, *BlueprintPath));
	}

	Blueprint->Modify();
	Graph->Modify();
	Scope.DirtyPackage(Blueprint->GetPackage());

	UEdGraphNode_Comment* CommentNode = NewObject<UEdGraphNode_Comment>(
		Graph, NAME_None, RF_Transactional);
	CommentNode->CreateNewGuid();
	CommentNode->NodePosX     = PosX;
	CommentNode->NodePosY     = PosY;
	CommentNode->NodeWidth    = SizeW;
	CommentNode->NodeHeight   = SizeH;
	CommentNode->NodeComment  = CommentText;
	CommentNode->CommentColor = CommentColor;

	Graph->AddNode(CommentNode, /*bUserAction*/ false, /*bSelectNewNode*/ false);
	CommentNode->PostPlacedNewNode();
	// UEdGraphNode_Comment::AllocateDefaultPins is an explicit no-op (header), but we call it to
	// stay consistent with the K2Node path used by bp.add_node.
	CommentNode->AllocateDefaultPins();

	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	TArray<TSharedPtr<FJsonValue>> PosResp;
	PosResp.Add(MakeShared<FJsonValueNumber>(PosX));
	PosResp.Add(MakeShared<FJsonValueNumber>(PosY));

	TArray<TSharedPtr<FJsonValue>> SizeResp;
	SizeResp.Add(MakeShared<FJsonValueNumber>(SizeW));
	SizeResp.Add(MakeShared<FJsonValueNumber>(SizeH));

	return FMCPJsonBuilder()
		.Str(TEXT("node_guid"),  CommentNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens))
		.Str(TEXT("graph_name"), GraphName)
		.Str(TEXT("text"),       CommentText)
		.Arr(TEXT("position"),   MoveTemp(PosResp))
		.Arr(TEXT("size"),       MoveTemp(SizeResp))
		.BuildSuccess(Request);
}

// ─── bp.delete_comment ─────────────────────────────────────────────────────────────────────────
//
// Args:    { blueprint_path: string, graph_name?: string ("EventGraph"), node_guid: string }
// Result:  { deleted: bool }
//
// Looks up the comment node by Guid, validates that it actually IS a UEdGraphNode_Comment (so we
// don't accidentally let callers use this path to delete K2 nodes — bp.delete_node is the right
// surface for those), then calls ``Graph->RemoveNode``.
//
// Errors: -32050 GraphNotFound, -32051 NodeNotFound (also for "found but not a comment node"),
//         -32602 InvalidParams (missing node_guid), -32027 PIEActive.
FMCPResponse Tool_DeleteComment(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_DeleteCommentNode", "MCP: bp.delete_comment"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString BlueprintPath, NodeGuidStr;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("blueprint_path"), BlueprintPath, Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("node_guid"),      NodeGuidStr,   Err)) { return Err; }

	FString GraphName = TEXT("EventGraph");
	Request.Args->TryGetStringField(TEXT("graph_name"), GraphName);

	int32 LoadErrCode = 0;
	FString LoadErrMsg;
	UBlueprint* Blueprint = FMCPBlueprintUtils::LoadBlueprintByPath(BlueprintPath, LoadErrCode, LoadErrMsg);
	if (!Blueprint) { return FMCPToolHelpers::MakeError(Request, LoadErrCode, LoadErrMsg); }

	UEdGraph* Graph = BGT_FindGraphByName(Blueprint, GraphName);
	if (!Graph)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorGraphNotFound,
			FString::Printf(TEXT("graph '%s' not found on blueprint '%s'"), *GraphName, *BlueprintPath));
	}

	UEdGraphNode* Node = BGT_FindNodeByGuid(Graph, NodeGuidStr);
	if (!Node)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorNodeNotFound,
			FString::Printf(TEXT("node '%s' not found in graph '%s'"), *NodeGuidStr, *GraphName));
	}

	UEdGraphNode_Comment* CommentNode = Cast<UEdGraphNode_Comment>(Node);
	if (!CommentNode)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorNodeNotFound,
			FString::Printf(
				TEXT("node '%s' in graph '%s' is class '%s', not UEdGraphNode_Comment — use bp.delete_node for K2 nodes"),
				*NodeGuidStr, *GraphName, *Node->GetClass()->GetName()));
	}

	Blueprint->Modify();
	Graph->Modify();
	CommentNode->Modify();
	Scope.DirtyPackage(Blueprint->GetPackage());

	Graph->RemoveNode(CommentNode);

	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	return FMCPJsonBuilder()
		.Bool(TEXT("deleted"), true)
		.BuildSuccess(Request);
}

// ─── bp.list_node_pins (Wave O — read-only pin enumeration, Lane A) ────────────────────────────
//
// Args:    { blueprint_path, graph_name?, node_guid }
// Result:  { node_guid, node_class, node_title, pins: [...full pin summaries...], total_count }
//
// Read-only inspection tool — caller uses the pin shape (name + pin_id + category + sub_category +
// sub_category_object + container_type + is_connected + default_value/object/text + linked_pins +
// hidden? + advanced_view?) to plan subsequent ``bp.connect_pins`` / ``bp.set_pin_default`` calls.
//
// Marked Lane A out of caution — touches UEdGraphNode + UEdGraphPin which technically live on the
// game thread (FName lookups + UObject* dereferences). Reviewer can promote to Lane B if profiling
// shows it's worth the audit.
//
// Errors: -32050 GraphNotFound, -32051 NodeNotFound, -32027 PIEActive (defensive — even read-only
// tools refuse during PIE since BP asset state is in flux during compile-after-PIE-exit).
FMCPResponse Tool_ListNodePins(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (FMCPWorldContext::IsPIEActive())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPIEActive, kMCPMessagePIEActive);
	}

	FString BlueprintPath, NodeGuidStr;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("blueprint_path"), BlueprintPath, Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("node_guid"),      NodeGuidStr,   Err)) { return Err; }

	FString GraphName = TEXT("EventGraph");
	Request.Args->TryGetStringField(TEXT("graph_name"), GraphName);

	int32 LoadErrCode = 0;
	FString LoadErrMsg;
	UBlueprint* Blueprint = FMCPBlueprintUtils::LoadBlueprintByPath(BlueprintPath, LoadErrCode, LoadErrMsg);
	if (!Blueprint) { return FMCPToolHelpers::MakeError(Request, LoadErrCode, LoadErrMsg); }

	UEdGraph* Graph = BGT_FindGraphByName(Blueprint, GraphName);
	if (!Graph)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorGraphNotFound,
			FString::Printf(TEXT("graph '%s' not found on blueprint '%s'"), *GraphName, *BlueprintPath));
	}

	UEdGraphNode* Node = BGT_FindNodeByGuid(Graph, NodeGuidStr);
	if (!Node)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorNodeNotFound,
			FString::Printf(TEXT("node '%s' not found in graph '%s'"), *NodeGuidStr, *GraphName));
	}

	TArray<TSharedPtr<FJsonValue>> Pins;
	Pins.Reserve(Node->Pins.Num());
	int32 TotalCount = 0;
	for (const UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin)
		{
			Pins.Add(MakeShared<FJsonValueObject>(BPG_BuildPinSummary(Pin)));
			++TotalCount;
		}
	}

	return FMCPJsonBuilder()
		.Str(TEXT("node_guid"),   Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens))
		.Str(TEXT("node_class"),  Node->GetClass()->GetPathName())
		.Str(TEXT("node_title"),  Node->GetNodeTitle(ENodeTitleType::ListView).ToString())
		.Arr(TEXT("pins"),        MoveTemp(Pins))
		.Num(TEXT("total_count"), static_cast<double>(TotalCount))
		.BuildSuccess(Request);
}

// ─── bp.add_cast_node (Wave O — UK2Node_DynamicCast, Lane A) ───────────────────────────────────
//
// Args:    { blueprint_path, graph_name?, target_class, position? }
// Result:  { node_guid, node_class, target_class, position: [x,y], pins: [...] }
//
// Creates a cast node where the input "Object" pin is cast to ``target_class``. Cast result and
// success/failure exec pins are typed correctly after ``AllocateDefaultPins`` reads ``TargetType``.
//
// Errors: -32050 GraphNotFound, -32020 ClassNotFound, -32011 WrongClass (not a UObject subclass),
//         -32027 PIEActive.
FMCPResponse Tool_AddCastNode(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_AddCastNode", "MCP: bp.add_cast_node"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString BlueprintPath, TargetClassPath;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("blueprint_path"), BlueprintPath, Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("target_class"),   TargetClassPath, Err)) { return Err; }

	FString GraphName = TEXT("EventGraph");
	Request.Args->TryGetStringField(TEXT("graph_name"), GraphName);

	int32 PosX = 0, PosY = 0;
	const TArray<TSharedPtr<FJsonValue>>* PositionArr = nullptr;
	if (Request.Args->TryGetArrayField(TEXT("position"), PositionArr) && PositionArr && PositionArr->Num() == 2)
	{
		PosX = static_cast<int32>((*PositionArr)[0]->AsNumber());
		PosY = static_cast<int32>((*PositionArr)[1]->AsNumber());
	}

	int32 LoadErrCode = 0;
	FString LoadErrMsg;
	UBlueprint* Blueprint = FMCPBlueprintUtils::LoadBlueprintByPath(BlueprintPath, LoadErrCode, LoadErrMsg);
	if (!Blueprint) { return FMCPToolHelpers::MakeError(Request, LoadErrCode, LoadErrMsg); }

	UEdGraph* Graph = BGT_FindGraphByName(Blueprint, GraphName);
	if (!Graph)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorGraphNotFound,
			FString::Printf(TEXT("graph '%s' not found on blueprint '%s'"), *GraphName, *BlueprintPath));
	}

	FString ResolveErr;
	UClass* TargetClass = FMCPClassResolver::ResolveLenient(TargetClassPath, UObject::StaticClass(), ResolveErr);
	if (!TargetClass)
	{
		// Distinguish "not loadable" from "not a UObject subclass" — both map to the same JSON-RPC
		// error, but ResolveErr carries the precise diagnostic.
		return FMCPToolHelpers::MakeError(Request,
			ResolveErr.Contains(TEXT("not a subclass")) ? kMCPErrorWrongClass : kMCPErrorClassNotFound,
			ResolveErr);
	}

	Blueprint->Modify();
	Graph->Modify();
	Scope.DirtyPackage(Blueprint->GetPackage());

	UK2Node_DynamicCast* CastNode = NewObject<UK2Node_DynamicCast>(Graph, NAME_None, RF_Transactional);
	CastNode->TargetType = TargetClass;
	BPG_FinalizeNewNode(CastNode, Graph, PosX, PosY);

	BPG_MarkModified(Blueprint);

	TArray<TSharedPtr<FJsonValue>> PositionResp;
	PositionResp.Add(MakeShared<FJsonValueNumber>(CastNode->NodePosX));
	PositionResp.Add(MakeShared<FJsonValueNumber>(CastNode->NodePosY));

	return FMCPJsonBuilder()
		.Str(TEXT("node_guid"),    CastNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens))
		.Str(TEXT("node_class"),   CastNode->GetClass()->GetPathName())
		.Str(TEXT("target_class"), TargetClass->GetPathName())
		.Arr(TEXT("position"),     MoveTemp(PositionResp))
		.Arr(TEXT("pins"),         BPG_PinsArray(CastNode))
		.BuildSuccess(Request);
}

// ─── bp.add_struct_break_node (Wave O — UK2Node_BreakStruct, Lane A) ───────────────────────────
//
// Args:    { blueprint_path, graph_name?, struct_path, position? }
// Result:  { node_guid, node_class, struct_path, position: [x,y], pins: [...per-member output pins...] }
//
// Creates a Break Struct node — input pin of struct type + one output pin per
// BlueprintReadable struct member. ``UK2Node_BreakStruct::CanBeBroken`` precondition: struct must
// be ``BlueprintType`` AND have at least one ``BlueprintVisible`` property AND NOT have a native
// ``BreakXXX`` function override.
//
// Errors: -32004 ObjectNotFound (struct path doesn't resolve), -32058 OperationFailed (struct is
// not breakable per CanBeBroken), -32050 GraphNotFound, -32027 PIEActive.
FMCPResponse Tool_AddStructBreakNode(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_AddStructBreakNode", "MCP: bp.add_struct_break_node"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString BlueprintPath, StructPath;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("blueprint_path"), BlueprintPath, Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("struct_path"),    StructPath,    Err)) { return Err; }

	FString GraphName = TEXT("EventGraph");
	Request.Args->TryGetStringField(TEXT("graph_name"), GraphName);

	int32 PosX = 0, PosY = 0;
	const TArray<TSharedPtr<FJsonValue>>* PositionArr = nullptr;
	if (Request.Args->TryGetArrayField(TEXT("position"), PositionArr) && PositionArr && PositionArr->Num() == 2)
	{
		PosX = static_cast<int32>((*PositionArr)[0]->AsNumber());
		PosY = static_cast<int32>((*PositionArr)[1]->AsNumber());
	}

	int32 LoadErrCode = 0;
	FString LoadErrMsg;
	UBlueprint* Blueprint = FMCPBlueprintUtils::LoadBlueprintByPath(BlueprintPath, LoadErrCode, LoadErrMsg);
	if (!Blueprint) { return FMCPToolHelpers::MakeError(Request, LoadErrCode, LoadErrMsg); }

	UEdGraph* Graph = BGT_FindGraphByName(Blueprint, GraphName);
	if (!Graph)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorGraphNotFound,
			FString::Printf(TEXT("graph '%s' not found on blueprint '%s'"), *GraphName, *BlueprintPath));
	}

	UScriptStruct* Struct = LoadObject<UScriptStruct>(nullptr, *StructPath);
	if (!Struct)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("UScriptStruct '%s' could not be loaded — expected e.g. "
				"'/Script/CoreUObject.Vector' or '/Game/Path/MyStruct.MyStruct'"), *StructPath));
	}

	// Mirror of ``UK2Node_BreakStruct::CanBeBroken`` (engine's static is NOT BLUEPRINTGRAPH_API
	// exported in UE 5.7 — only the MakeStruct twin is). Same logic: must be allowable as a BP
	// variable type, must NOT have a native ``Break<StructName>`` function override, and must
	// expose at least one BlueprintVisible property.
	{
		bool bBreakable = false;
		const bool bNoNativeBreak = !Struct->HasMetaData(FBlueprintMetadata::MD_NativeBreakFunction);
		const bool bAllowable = UEdGraphSchema_K2::IsAllowableBlueprintVariableType(Struct, /*bForInternalUse*/ false);
		if (bNoNativeBreak && bAllowable)
		{
			for (TFieldIterator<FProperty> It(Struct); It; ++It)
			{
				FProperty* Prop = *It;
				if (Prop && Prop->HasAnyPropertyFlags(CPF_BlueprintVisible))
				{
					bBreakable = true;
					break;
				}
			}
		}
		if (!bBreakable)
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorOperationFailed,
				FString::Printf(TEXT("struct '%s' cannot be broken (not BlueprintType, lacks BlueprintVisible "
					"members, or has a native 'Break%s' function override)"),
					*StructPath, *Struct->GetName()));
		}
	}

	Blueprint->Modify();
	Graph->Modify();
	Scope.DirtyPackage(Blueprint->GetPackage());

	UK2Node_BreakStruct* BreakNode = NewObject<UK2Node_BreakStruct>(Graph, NAME_None, RF_Transactional);
	// StructType lives on the UK2Node_StructOperation parent — set BEFORE AllocateDefaultPins so the
	// per-member output pins materialise on construction.
	BreakNode->StructType = Struct;
	BPG_FinalizeNewNode(BreakNode, Graph, PosX, PosY);

	BPG_MarkModified(Blueprint);

	TArray<TSharedPtr<FJsonValue>> PositionResp;
	PositionResp.Add(MakeShared<FJsonValueNumber>(BreakNode->NodePosX));
	PositionResp.Add(MakeShared<FJsonValueNumber>(BreakNode->NodePosY));

	return FMCPJsonBuilder()
		.Str(TEXT("node_guid"),   BreakNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens))
		.Str(TEXT("node_class"),  BreakNode->GetClass()->GetPathName())
		.Str(TEXT("struct_path"), Struct->GetPathName())
		.Arr(TEXT("position"),    MoveTemp(PositionResp))
		.Arr(TEXT("pins"),        BPG_PinsArray(BreakNode))
		.BuildSuccess(Request);
}

// ─── bp.add_struct_make_node (Wave O — UK2Node_MakeStruct, Lane A) ─────────────────────────────
//
// Mirror of bp.add_struct_break_node — same args/shape, different K2Node class + CanBeMade check.
//
// Errors: identical to add_struct_break_node.
FMCPResponse Tool_AddStructMakeNode(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_AddStructMakeNode", "MCP: bp.add_struct_make_node"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString BlueprintPath, StructPath;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("blueprint_path"), BlueprintPath, Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("struct_path"),    StructPath,    Err)) { return Err; }

	FString GraphName = TEXT("EventGraph");
	Request.Args->TryGetStringField(TEXT("graph_name"), GraphName);

	int32 PosX = 0, PosY = 0;
	const TArray<TSharedPtr<FJsonValue>>* PositionArr = nullptr;
	if (Request.Args->TryGetArrayField(TEXT("position"), PositionArr) && PositionArr && PositionArr->Num() == 2)
	{
		PosX = static_cast<int32>((*PositionArr)[0]->AsNumber());
		PosY = static_cast<int32>((*PositionArr)[1]->AsNumber());
	}

	int32 LoadErrCode = 0;
	FString LoadErrMsg;
	UBlueprint* Blueprint = FMCPBlueprintUtils::LoadBlueprintByPath(BlueprintPath, LoadErrCode, LoadErrMsg);
	if (!Blueprint) { return FMCPToolHelpers::MakeError(Request, LoadErrCode, LoadErrMsg); }

	UEdGraph* Graph = BGT_FindGraphByName(Blueprint, GraphName);
	if (!Graph)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorGraphNotFound,
			FString::Printf(TEXT("graph '%s' not found on blueprint '%s'"), *GraphName, *BlueprintPath));
	}

	UScriptStruct* Struct = LoadObject<UScriptStruct>(nullptr, *StructPath);
	if (!Struct)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("UScriptStruct '%s' could not be loaded"), *StructPath));
	}

	if (!UK2Node_MakeStruct::CanBeMade(Struct))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorOperationFailed,
			FString::Printf(TEXT("struct '%s' cannot be made (not BlueprintType, lacks "
				"BlueprintVisible non-readonly members, or has a native 'Make%s' function override)"),
				*StructPath, *Struct->GetName()));
	}

	Blueprint->Modify();
	Graph->Modify();
	Scope.DirtyPackage(Blueprint->GetPackage());

	UK2Node_MakeStruct* MakeNode = NewObject<UK2Node_MakeStruct>(Graph, NAME_None, RF_Transactional);
	MakeNode->StructType = Struct;
	BPG_FinalizeNewNode(MakeNode, Graph, PosX, PosY);

	BPG_MarkModified(Blueprint);

	TArray<TSharedPtr<FJsonValue>> PositionResp;
	PositionResp.Add(MakeShared<FJsonValueNumber>(MakeNode->NodePosX));
	PositionResp.Add(MakeShared<FJsonValueNumber>(MakeNode->NodePosY));

	return FMCPJsonBuilder()
		.Str(TEXT("node_guid"),   MakeNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens))
		.Str(TEXT("node_class"),  MakeNode->GetClass()->GetPathName())
		.Str(TEXT("struct_path"), Struct->GetPathName())
		.Arr(TEXT("position"),    MoveTemp(PositionResp))
		.Arr(TEXT("pins"),        BPG_PinsArray(MakeNode))
		.BuildSuccess(Request);
}

// ─── bp.add_macro_node (Wave O — UK2Node_MacroInstance, Lane A) ────────────────────────────────
//
// Args:    { blueprint_path, graph_name?, macro_path, position? }
// Result:  { node_guid, node_class, macro_path, position: [x,y], pins: [...] }
//
// ``macro_path`` format: ``<package_obj_path>:<graph_name>``. Example:
//   /Engine/EditorBlueprintResources/StandardMacros.StandardMacros:ForEachLoop
//
// First half (before ``:``) is loaded as a UObject (typically a UBlueprint — macro libraries are
// BPs with Macros only). Second half (after ``:``) is the macro graph's GetName() inside
// ``MacroLib->MacroGraphs``. Split via FString::Split(..., FromEnd) so paths containing dots
// (UE convention ``Package.Asset:Inner``) parse correctly.
//
// Errors: -32602 InvalidParams (malformed macro_path), -32004 ObjectNotFound (macro library or
// inner graph not found), -32050 GraphNotFound, -32027 PIEActive.
FMCPResponse Tool_AddMacroNode(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_AddMacroNode", "MCP: bp.add_macro_node"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString BlueprintPath, MacroPath;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("blueprint_path"), BlueprintPath, Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("macro_path"),     MacroPath,     Err)) { return Err; }

	FString GraphName = TEXT("EventGraph");
	Request.Args->TryGetStringField(TEXT("graph_name"), GraphName);

	int32 PosX = 0, PosY = 0;
	const TArray<TSharedPtr<FJsonValue>>* PositionArr = nullptr;
	if (Request.Args->TryGetArrayField(TEXT("position"), PositionArr) && PositionArr && PositionArr->Num() == 2)
	{
		PosX = static_cast<int32>((*PositionArr)[0]->AsNumber());
		PosY = static_cast<int32>((*PositionArr)[1]->AsNumber());
	}

	// Parse macro_path: split on the LAST ':' (asset object paths use ':' for sub-object refs but
	// the convention is one ':' between asset and inner-graph; FromEnd handles the rare case where
	// future asset names happen to contain ':' by reserving the last token for the graph name).
	FString PackagePart, GraphPart;
	if (!MacroPath.Split(TEXT(":"), &PackagePart, &GraphPart, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			FString::Printf(TEXT("macro_path '%s' is malformed — expected format "
				"'<package_obj_path>:<inner_graph_name>' (e.g. "
				"'/Engine/EditorBlueprintResources/StandardMacros.StandardMacros:ForEachLoop')"),
				*MacroPath));
	}
	if (PackagePart.IsEmpty() || GraphPart.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			FString::Printf(TEXT("macro_path '%s' has empty package or graph part"), *MacroPath));
	}

	int32 LoadErrCode = 0;
	FString LoadErrMsg;
	UBlueprint* Blueprint = FMCPBlueprintUtils::LoadBlueprintByPath(BlueprintPath, LoadErrCode, LoadErrMsg);
	if (!Blueprint) { return FMCPToolHelpers::MakeError(Request, LoadErrCode, LoadErrMsg); }

	UEdGraph* Graph = BGT_FindGraphByName(Blueprint, GraphName);
	if (!Graph)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorGraphNotFound,
			FString::Printf(TEXT("graph '%s' not found on blueprint '%s'"), *GraphName, *BlueprintPath));
	}

	// Macro libraries are UBlueprints with BPTYPE_MacroLibrary. LoadObject<UBlueprint> handles
	// both ``/Game/Path/MyLib`` and ``/Game/Path/MyLib.MyLib`` forms internally.
	UBlueprint* MacroLib = LoadObject<UBlueprint>(nullptr, *PackagePart);
	if (!MacroLib)
	{
		// Retry with .LeafName suffix — LoadObject only retries for some paths.
		const int32 LastSlash = PackagePart.Find(TEXT("/"), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		if (LastSlash != INDEX_NONE && !PackagePart.Contains(TEXT(".")))
		{
			const FString LeafName = PackagePart.RightChop(LastSlash + 1);
			MacroLib = LoadObject<UBlueprint>(nullptr, *(PackagePart + TEXT(".") + LeafName));
		}
	}
	if (!MacroLib)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("macro library blueprint '%s' could not be loaded "
				"(expected UBlueprint with macro graphs)"), *PackagePart));
	}

	// Find inner graph by name in MacroLib->MacroGraphs.
	UEdGraph* MacroGraph = nullptr;
	const FName MacroGraphFName(*GraphPart);
	for (UEdGraph* G : MacroLib->MacroGraphs)
	{
		if (G && G->GetFName() == MacroGraphFName)
		{
			MacroGraph = G;
			break;
		}
	}
	if (!MacroGraph)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("inner macro graph '%s' not found in library '%s' "
				"(%d macros available)"),
				*GraphPart, *PackagePart, MacroLib->MacroGraphs.Num()));
	}

	Blueprint->Modify();
	Graph->Modify();
	Scope.DirtyPackage(Blueprint->GetPackage());

	UK2Node_MacroInstance* MacroNode = NewObject<UK2Node_MacroInstance>(Graph, NAME_None, RF_Transactional);
	// SetMacroGraph populates MacroGraphReference; AllocateDefaultPins reads it to materialise the
	// macro's input/output pins on the instance.
	MacroNode->SetMacroGraph(MacroGraph);
	BPG_FinalizeNewNode(MacroNode, Graph, PosX, PosY);
	// Macro pins depend on the inner graph's Tunnel entry/exit nodes — ReconstructNode forces a
	// re-read after AllocateDefaultPins so any wildcard inference / late binding finalises.
	MacroNode->ReconstructNode();

	BPG_MarkModified(Blueprint);

	TArray<TSharedPtr<FJsonValue>> PositionResp;
	PositionResp.Add(MakeShared<FJsonValueNumber>(MacroNode->NodePosX));
	PositionResp.Add(MakeShared<FJsonValueNumber>(MacroNode->NodePosY));

	return FMCPJsonBuilder()
		.Str(TEXT("node_guid"),  MacroNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens))
		.Str(TEXT("node_class"), MacroNode->GetClass()->GetPathName())
		.Str(TEXT("macro_path"), FString::Printf(TEXT("%s:%s"),
			*MacroLib->GetPathName(), *MacroGraph->GetName()))
		.Arr(TEXT("position"),   MoveTemp(PositionResp))
		.Arr(TEXT("pins"),       BPG_PinsArray(MacroNode))
		.BuildSuccess(Request);
}

// ─── bp.add_input_action_event_node (Wave O — UK2Node_EnhancedInputAction, Lane A) ─────────────
//
// Args:    { blueprint_path, graph_name?, ia_path, trigger_event?, position? }
// Result:  { node_guid, node_class, ia_path, trigger_event, position: [x,y], pins: [...] }
//
// ``ia_path``: ``UInputAction`` asset path (e.g. ``/Game/Input/IA_Jump``). Loaded as UInputAction
// then assigned to the K2Node's ``InputAction`` field via reflection (no Build.cs dep on
// InputBlueprintNodes — that module is editor-private and not in our deps).
//
// ``trigger_event``: informational only — the node emits ALL trigger pins (Triggered/Started/
// Ongoing/Canceled/Completed) on AllocateDefaultPins. The caller picks which exec to wire.
//
// Errors: -32004 ObjectNotFound (IA path doesn't resolve), -32603 InternalError (InputBlueprintNodes
// module not loaded — K2Node class not findable), -32602 InvalidParams (trigger_event invalid),
// -32050 GraphNotFound, -32027 PIEActive.
FMCPResponse Tool_AddInputActionEventNode(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_AddInputActionEvent", "MCP: bp.add_input_action_event_node"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString BlueprintPath, IAPath;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("blueprint_path"), BlueprintPath, Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("ia_path"),        IAPath,        Err)) { return Err; }

	FString GraphName = TEXT("EventGraph");
	Request.Args->TryGetStringField(TEXT("graph_name"), GraphName);

	FString TriggerEvent = TEXT("Triggered");
	Request.Args->TryGetStringField(TEXT("trigger_event"), TriggerEvent);
	// Loose validation — the K2 node exposes all 5 trigger pins regardless; this gives the caller a
	// fast error if they mistyped. Names match the engine's ETriggerEvent enum value strings.
	static const TCHAR* const kKnownTriggers[] = {
		TEXT("Triggered"), TEXT("Started"), TEXT("Ongoing"), TEXT("Canceled"), TEXT("Completed")
	};
	bool bTriggerOk = false;
	for (const TCHAR* Known : kKnownTriggers)
	{
		if (TriggerEvent.Equals(Known, ESearchCase::IgnoreCase)) { bTriggerOk = true; break; }
	}
	if (!bTriggerOk)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			FString::Printf(TEXT("trigger_event '%s' is not one of "
				"Triggered/Started/Ongoing/Canceled/Completed"), *TriggerEvent));
	}

	int32 PosX = 0, PosY = 0;
	const TArray<TSharedPtr<FJsonValue>>* PositionArr = nullptr;
	if (Request.Args->TryGetArrayField(TEXT("position"), PositionArr) && PositionArr && PositionArr->Num() == 2)
	{
		PosX = static_cast<int32>((*PositionArr)[0]->AsNumber());
		PosY = static_cast<int32>((*PositionArr)[1]->AsNumber());
	}

	int32 LoadErrCode = 0;
	FString LoadErrMsg;
	UBlueprint* Blueprint = FMCPBlueprintUtils::LoadBlueprintByPath(BlueprintPath, LoadErrCode, LoadErrMsg);
	if (!Blueprint) { return FMCPToolHelpers::MakeError(Request, LoadErrCode, LoadErrMsg); }

	UEdGraph* Graph = BGT_FindGraphByName(Blueprint, GraphName);
	if (!Graph)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorGraphNotFound,
			FString::Printf(TEXT("graph '%s' not found on blueprint '%s'"), *GraphName, *BlueprintPath));
	}

	// Load IA — UInputAction lives in EnhancedInput (already a private dep on the bridge module).
	// Don't include its header here; use reflection-friendly UObject path then verify class.
	UObject* IAObject = LoadObject<UObject>(nullptr, *IAPath);
	if (!IAObject)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("UInputAction '%s' could not be loaded"), *IAPath));
	}
	// Soft-verify class via FindObject — avoids hard include of EnhancedInput's InputAction.h.
	UClass* InputActionClass = FindObject<UClass>(nullptr, TEXT("/Script/EnhancedInput.InputAction"));
	if (!InputActionClass || !IAObject->IsA(InputActionClass))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorWrongClass,
			FString::Printf(TEXT("asset '%s' (class %s) is not a UInputAction"),
				*IAPath, *IAObject->GetClass()->GetPathName()));
	}

	// Resolve the K2 node class via FindObject — InputBlueprintNodes is editor-only + private; we
	// don't link against it, but the module is loaded in editor sessions so the class is reachable.
	UClass* NodeClass = FindObject<UClass>(nullptr,
		TEXT("/Script/InputBlueprintNodes.K2Node_EnhancedInputAction"));
	if (!NodeClass)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInternal,
			TEXT("UK2Node_EnhancedInputAction class not findable — is the InputBlueprintNodes module loaded?"));
	}

	Blueprint->Modify();
	Graph->Modify();
	Scope.DirtyPackage(Blueprint->GetPackage());

	UK2Node* IANode = NewObject<UK2Node>(Graph, NodeClass, NAME_None, RF_Transactional);
	check(IANode);

	// Find + write the InputAction field via reflection. Field is TObjectPtr<const UInputAction>
	// (see K2Node_EnhancedInputAction.h:50) — its FObjectProperty is layout-compatible with a raw
	// UObject* assignment via ContainerPtrToValuePtr.
	FProperty* IAProp = NodeClass->FindPropertyByName(FName(TEXT("InputAction")));
	if (!IAProp)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInternal,
			TEXT("UK2Node_EnhancedInputAction has no 'InputAction' UPROPERTY — engine version mismatch?"));
	}
	FObjectProperty* IAObjProp = CastField<FObjectProperty>(IAProp);
	if (!IAObjProp)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInternal,
			TEXT("UK2Node_EnhancedInputAction::InputAction is not an FObjectProperty — engine version mismatch?"));
	}
	// FObjectProperty handles the TObjectPtr<T> shape transparently via SetObjectPropertyValue.
	IAObjProp->SetObjectPropertyValue_InContainer(IANode, IAObject);

	BPG_FinalizeNewNode(IANode, Graph, PosX, PosY);
	// ReconstructNode after AllocateDefaultPins so all trigger pins reflect the resolved IA's
	// value-type signature (Bool IAs have no value pin; Axis1D/2D/3D add a Value pin).
	IANode->ReconstructNode();

	BPG_MarkModified(Blueprint);

	TArray<TSharedPtr<FJsonValue>> PositionResp;
	PositionResp.Add(MakeShared<FJsonValueNumber>(IANode->NodePosX));
	PositionResp.Add(MakeShared<FJsonValueNumber>(IANode->NodePosY));

	return FMCPJsonBuilder()
		.Str(TEXT("node_guid"),     IANode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens))
		.Str(TEXT("node_class"),    NodeClass->GetPathName())
		.Str(TEXT("ia_path"),       IAObject->GetPathName())
		.Str(TEXT("trigger_event"), TriggerEvent)
		.Arr(TEXT("position"),      MoveTemp(PositionResp))
		.Arr(TEXT("pins"),          BPG_PinsArray(IANode))
		.BuildSuccess(Request);
}

// ─── bp.bind_component_event (Wave O — UK2Node_ComponentBoundEvent, Lane A) ────────────────────
//
// **THE critical Wave-O tool.** Creates a component bound event in the BP's EventGraph — the
// foundation for actor-BP reactive logic: OnComponentBeginOverlap, OnHit, OnClicked, OnDestroyed,
// etc. Without this, an AI agent cannot author typical actor BPs end-to-end.
//
// Args:    { blueprint_path, component_name, delegate_name, target_function_name?, position? }
// Result:  { node_guid, node_class, component_name, delegate_name, function_name, function_created,
//            position: [x,y], pins: [...event signature pins...] }
//
// Special handling:
//   1. The host graph is ALWAYS ``Blueprint->UbergraphPages[0]`` (the EventGraph). Components are
//      bound at construction — bound events on a function graph would not fire.
//   2. ``target_function_name`` (optional) renames the K2Node's ``CustomFunctionName`` so the
//      generated event has a readable name in the graph + the event-stub function tab. If omitted,
//      engine generates a deterministic name like ``OnComponentBeginOverlap_Sphere``.
//   3. Duplicate detection: ``FKismetEditorUtilities::FindBoundEventForComponent`` returns the
//      existing node if one already binds this (component, delegate) pair — we surface as
//      -32058 OperationFailed so the caller doesn't accidentally create dual binders.
//
// Errors: -32004 ObjectNotFound (component / delegate / EventGraph), -32058 OperationFailed
// (delegate already bound), -32027 PIEActive.
FMCPResponse Tool_BindComponentEvent(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_BindComponentEvent", "MCP: bp.bind_component_event"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString BlueprintPath, ComponentName, DelegateName;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("blueprint_path"), BlueprintPath, Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("component_name"), ComponentName, Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("delegate_name"),  DelegateName,  Err)) { return Err; }

	FString TargetFunctionName;
	Request.Args->TryGetStringField(TEXT("target_function_name"), TargetFunctionName);

	int32 PosX = 0, PosY = 0;
	const TArray<TSharedPtr<FJsonValue>>* PositionArr = nullptr;
	if (Request.Args->TryGetArrayField(TEXT("position"), PositionArr) && PositionArr && PositionArr->Num() == 2)
	{
		PosX = static_cast<int32>((*PositionArr)[0]->AsNumber());
		PosY = static_cast<int32>((*PositionArr)[1]->AsNumber());
	}

	int32 LoadErrCode = 0;
	FString LoadErrMsg;
	UBlueprint* Blueprint = FMCPBlueprintUtils::LoadBlueprintByPath(BlueprintPath, LoadErrCode, LoadErrMsg);
	if (!Blueprint) { return FMCPToolHelpers::MakeError(Request, LoadErrCode, LoadErrMsg); }

	// Find component in SCS — covers BP-authored components (the usual case for actor BPs).
	USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
	if (!SCS)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInternal,
			FString::Printf(TEXT("blueprint '%s' has no SimpleConstructionScript "
				"(non-AActor parent class? %s)"),
				*BlueprintPath,
				Blueprint->ParentClass ? *Blueprint->ParentClass->GetName() : TEXT("null parent")));
	}
	const FName ComponentFName(*ComponentName);
	USCS_Node* ComponentNode = SCS->FindSCSNode(ComponentFName);
	if (!ComponentNode)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("component '%s' not found in SimpleConstructionScript of '%s'"),
				*ComponentName, *BlueprintPath));
	}
	UClass* ComponentClass = ComponentNode->ComponentClass;
	if (!ComponentClass)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInternal,
			FString::Printf(TEXT("SCS node '%s' has null ComponentClass"), *ComponentName));
	}

	// Find delegate property on component class. Must walk parent classes — most overlap/hit
	// delegates live on UPrimitiveComponent (parent of UStaticMeshComponent, USphereComponent, etc.).
	FMulticastDelegateProperty* DelegateProp =
		FindFProperty<FMulticastDelegateProperty>(ComponentClass, FName(*DelegateName));
	if (!DelegateProp)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("delegate '%s' not found on component class '%s' "
				"(checked full inheritance chain)"),
				*DelegateName, *ComponentClass->GetPathName()));
	}

	// Find the FObjectProperty in the BP's GeneratedClass that holds the component pointer. SCS
	// nodes generate a property on the GeneratedClass named after the variable.
	UClass* BPGenClass = Blueprint->GeneratedClass;
	if (!BPGenClass)
	{
		// Force-compile if generated class missing (rare — usually Epic regenerates on load).
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInternal,
			FString::Printf(TEXT("blueprint '%s' has no GeneratedClass — needs initial compile"),
				*BlueprintPath));
	}
	FObjectProperty* ComponentProp = FindFProperty<FObjectProperty>(BPGenClass, ComponentFName);
	if (!ComponentProp)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("component property '%s' not found on GeneratedClass '%s' "
				"(SCS variable wasn't promoted to BP class — compile the BP first)"),
				*ComponentName, *BPGenClass->GetPathName()));
	}

	// EventGraph is the only valid host for component-bound events.
	if (Blueprint->UbergraphPages.Num() == 0 || !Blueprint->UbergraphPages[0])
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorGraphNotFound,
			FString::Printf(TEXT("blueprint '%s' has no UbergraphPages — no EventGraph to host the bound event"),
				*BlueprintPath));
	}
	UEdGraph* EventGraph = Blueprint->UbergraphPages[0];

	// Duplicate-bind check — if (component, delegate) already has a bound event, reject. Mimics
	// the editor's right-click-add-event UI which navigates to the existing node instead.
	if (const UK2Node_ComponentBoundEvent* Existing =
		FKismetEditorUtilities::FindBoundEventForComponent(Blueprint, DelegateProp->GetFName(), ComponentFName))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorOperationFailed,
			FString::Printf(TEXT("delegate '%s' on component '%s' is already bound by node '%s'"),
				*DelegateName, *ComponentName,
				*Existing->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens)));
	}

	Blueprint->Modify();
	EventGraph->Modify();
	Scope.DirtyPackage(Blueprint->GetPackage());

	UK2Node_ComponentBoundEvent* BoundEvent = NewObject<UK2Node_ComponentBoundEvent>(
		EventGraph, NAME_None, RF_Transactional);

	// InitializeComponentBoundEventParams auto-populates DelegatePropertyName / ComponentPropertyName
	// / EventReference + sets the EventName the engine uses for the event-stub function. Must be
	// called BEFORE AllocateDefaultPins so the event-signature pins (OtherActor, OtherComp, etc.)
	// materialise from the delegate's signature.
	BoundEvent->InitializeComponentBoundEventParams(ComponentProp, DelegateProp);

	// Custom function name override — must happen BEFORE FinalizeNewNode (specifically before
	// AllocateDefaultPins) so the generated event entry uses the desired name. The engine-default
	// name from InitializeComponentBoundEventParams is something like ``OnComponentBeginOverlap_Sphere``;
	// callers often want a domain-specific name like ``OnSphereOverlap_BP``.
	const bool bRenamedEvent = !TargetFunctionName.IsEmpty();
	if (bRenamedEvent)
	{
		BoundEvent->CustomFunctionName = FName(*TargetFunctionName);
	}

	// FinalizeNewNode handles guid + AddNode + PostPlacedNewNode + AllocateDefaultPins. After
	// AllocateDefaultPins the K2Node_Event base will have manufactured per-delegate-parameter
	// output pins (OtherActor / OtherComp / OtherBodyIndex / bFromSweep / SweepResult for begin
	// overlap, for example).
	BPG_FinalizeNewNode(BoundEvent, EventGraph, PosX, PosY);

	BPG_MarkModified(Blueprint);

	TArray<TSharedPtr<FJsonValue>> PositionResp;
	PositionResp.Add(MakeShared<FJsonValueNumber>(BoundEvent->NodePosX));
	PositionResp.Add(MakeShared<FJsonValueNumber>(BoundEvent->NodePosY));

	// Reported function_name reflects what the caller will see in the BP — the override if set,
	// otherwise the engine-generated default that InitializeComponentBoundEventParams produced.
	const FString FinalFunctionName = BoundEvent->CustomFunctionName.IsNone()
		? BoundEvent->GetFunctionName().ToString()
		: BoundEvent->CustomFunctionName.ToString();

	return FMCPJsonBuilder()
		.Str(TEXT("node_guid"),       BoundEvent->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens))
		.Str(TEXT("node_class"),      BoundEvent->GetClass()->GetPathName())
		.Str(TEXT("component_name"),  ComponentName)
		.Str(TEXT("delegate_name"),   DelegateName)
		.Str(TEXT("function_name"),   FinalFunctionName)
		.Bool(TEXT("function_created"), bRenamedEvent)
		.Arr(TEXT("position"),        MoveTemp(PositionResp))
		.Arr(TEXT("pins"),            BPG_PinsArray(BoundEvent))
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

	RegisterTool(TEXT("bp.add_node"),          &Tool_AddNode,          /*Lane A*/ false);
	RegisterTool(TEXT("bp.connect_pins"),      &Tool_ConnectPins,      /*Lane A*/ false);

	// Wave F Surface 1 — graph CRUD.
	RegisterTool(TEXT("bp.set_node_property"), &Tool_SetNodeProperty,  /*Lane A*/ false);
	RegisterTool(TEXT("bp.set_pin_default"),   &Tool_SetPinDefault,    /*Lane A*/ false);
	RegisterTool(TEXT("bp.delete_node"),       &Tool_DeleteNode,       /*Lane A*/ false);
	RegisterTool(TEXT("bp.disconnect_pin"),    &Tool_DisconnectPin,    /*Lane A*/ false);
	RegisterTool(TEXT("bp.move_node"),         &Tool_MoveNode,         /*Lane A*/ false);

	// Wave F Surface 5 — comment-node CRUD (paired with bp.set_variable_metadata + bp.list_categories
	// in BlueprintTools.cpp). Cosmetic graph annotation surface — same UEdGraph mutation pattern as
	// bp.add_node / bp.delete_node but no pin / type-binding considerations.
	RegisterTool(TEXT("bp.add_comment"),        &Tool_AddComment,       /*Lane A*/ false);
	RegisterTool(TEXT("bp.delete_comment"),     &Tool_DeleteComment,    /*Lane A*/ false);

	// Wave O — BP authoring gap closer. 7 specialised node spawners + pin introspection. All
	// Lane A (UObject mutation + game-thread-only K2 node construction). Closes the "20% gap"
	// where bp.add_node alone can't perform post-construction setup specific to certain K2 node
	// subclasses (cast TargetType, struct break/make StructType, macro graph reference, IA field
	// reflection, component-bound event delegate/component property pairing).
	RegisterTool(TEXT("bp.list_node_pins"),               &Tool_ListNodePins,             /*Lane A*/ false);
	RegisterTool(TEXT("bp.add_cast_node"),                &Tool_AddCastNode,              /*Lane A*/ false);
	RegisterTool(TEXT("bp.add_struct_break_node"),        &Tool_AddStructBreakNode,       /*Lane A*/ false);
	RegisterTool(TEXT("bp.add_struct_make_node"),         &Tool_AddStructMakeNode,        /*Lane A*/ false);
	RegisterTool(TEXT("bp.add_macro_node"),               &Tool_AddMacroNode,             /*Lane A*/ false);
	RegisterTool(TEXT("bp.add_input_action_event_node"),  &Tool_AddInputActionEventNode,  /*Lane A*/ false);
	RegisterTool(TEXT("bp.bind_component_event"),         &Tool_BindComponentEvent,       /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("BP graph surface registered: bp.add_node + bp.connect_pins (Wave B Tier 4) + "
		     "bp.set_node_property + bp.set_pin_default + bp.delete_node + bp.disconnect_pin + "
		     "bp.move_node (Wave F1) + bp.add_comment + bp.delete_comment (Wave F5) + "
		     "bp.list_node_pins + bp.add_cast_node + bp.add_struct_break_node + bp.add_struct_make_node + "
		     "bp.add_macro_node + bp.add_input_action_event_node + bp.bind_component_event (Wave O) "
		     "— all Lane A"));
}

} // namespace FBlueprintGraphTools

#undef LOCTEXT_NAMESPACE

MCP_REGISTER_SURFACE(BlueprintGraphTools, &FBlueprintGraphTools::Register)
