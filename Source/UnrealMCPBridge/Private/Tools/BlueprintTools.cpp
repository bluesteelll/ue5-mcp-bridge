// Copyright FatumGame. All Rights Reserved.

#include "BlueprintTools.h"

#include "FMCPDispatchQueue.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPBlueprintUtils.h"
#include "Utils/MCPPageCursor.h"
#include "Utils/MCPPinTypeUtils.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_EditablePinBase.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/Class.h"
#include "UObject/Script.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	// BP_ prefix per the unity-build symbol-collision pattern (MakeError/MakeSuccess clash with
	// UE's global ValueOrError templates).
	constexpr int32 kBPErrorInvalidParams = -32602;

	void BP_StampIds(const FMCPRequest& Request, FMCPResponse& Response)
	{
		Response.RequestId = Request.RequestId;
		Response.OriginalIdString = Request.OriginalIdString;
	}

	FMCPResponse BP_MakeError(const FMCPRequest& Request, int32 Code, const FString& Message)
	{
		FMCPResponse R;
		BP_StampIds(Request, R);
		R.bIsError = true;
		R.ErrorCode = Code;
		R.ErrorMessage = Message;
		return R;
	}

	FMCPResponse BP_MakeSuccessObj(const FMCPRequest& Request, TSharedPtr<FJsonObject> Result)
	{
		FMCPResponse R;
		BP_StampIds(Request, R);
		R.bIsError = false;
		R.Result = MakeShared<FJsonValueObject>(MoveTemp(Result));
		return R;
	}

	// ─── arg parsing helpers ─────────────────────────────────────────────────────────────────────

	/** Read ``args.blueprint_path`` field; emit -32602 InvalidParams on missing/empty. */
	bool BP_RequireBlueprintPath(const FMCPRequest& Request, FString& OutPath, FMCPResponse& OutError)
	{
		if (!Request.Args.IsValid())
		{
			OutError = BP_MakeError(Request, kBPErrorInvalidParams, TEXT("missing args object"));
			return false;
		}
		if (!Request.Args->TryGetStringField(TEXT("blueprint_path"), OutPath) || OutPath.IsEmpty())
		{
			OutError = BP_MakeError(Request, kBPErrorInvalidParams,
				TEXT("missing required string field 'blueprint_path'"));
			return false;
		}
		return true;
	}

	/** Resolve blueprint by path + emit appropriate error. Returns nullptr + populates OutError on failure. */
	UBlueprint* BP_ResolveBlueprintOrError(const FMCPRequest& Request, const FString& Path, FMCPResponse& OutError)
	{
		int32 ErrCode = 0;
		FString ErrMsg;
		UBlueprint* Blueprint = FMCPBlueprintUtils::LoadBlueprintByPath(Path, ErrCode, ErrMsg);
		if (!Blueprint)
		{
			OutError = BP_MakeError(Request, ErrCode, ErrMsg);
			return nullptr;
		}
		return Blueprint;
	}

	int32 BP_ClampPageSize(const TSharedPtr<FJsonObject>& Args, const TCHAR* FieldName, int32 Default)
	{
		int32 Out = Default;
		if (Args.IsValid())
		{
			Args->TryGetNumberField(FieldName, Out);
		}
		return FMath::Clamp(Out, 1, 1000);
	}

	bool BP_DecodeCursor(
		const FMCPRequest& Request,
		const FString& TokenWire,
		uint64 ExpectedFilterHash,
		FMCPPageCursor& OutCursor,
		FMCPResponse& OutError)
	{
		FString DecodeErr;
		if (!FMCPPageCursorUtils::Decode(TokenWire, OutCursor, DecodeErr))
		{
			OutError = BP_MakeError(Request, kMCPErrorStaleCursor,
				FString::Printf(TEXT("page_token decode failed: %s"), *DecodeErr));
			return false;
		}
		if (!FMCPPageCursorUtils::ValidateAgainstFilter(OutCursor, ExpectedFilterHash))
		{
			OutError = BP_MakeError(Request, kMCPErrorStaleCursor,
				TEXT("page_token filter_hash mismatch — caller mutated filter (likely blueprint_path) "
					 "between pages; restart pagination with page_token=null"));
			return false;
		}
		return true;
	}

	/**
	 * Filter-hash for paginated bp.* tools. Blueprint path is the dominant key; any change to it
	 * between pages → -32015 StaleCursor on next call. Extra ``Extra`` field allows tools to add
	 * function-name (for list_nodes_in_function) without colliding with list_variables / list_functions.
	 */
	uint64 BP_HashFilter(const FString& BlueprintPath, const FString& Extra)
	{
		const uint32 H1 = GetTypeHash(BlueprintPath);
		const uint32 H2 = GetTypeHash(Extra);
		return (static_cast<uint64>(H1) << 32) ^ H2;
	}

	// ─── pin-type serialisation glue ──────────────────────────────────────────────────────────────

	/**
	 * Helper: serialise a pin type, propagating -32032 / -32004 errors from MCPPinTypeUtils into a
	 * tool response. On success returns the populated pin-type JSON; on failure populates OutError
	 * and returns nullptr.
	 */
	TSharedPtr<FJsonObject> BP_PinTypeToJsonOrError(
		const FMCPRequest& Request,
		const FEdGraphPinType& PinType,
		FMCPResponse& OutError)
	{
		int32 ErrCode = 0;
		FString ErrMsg;
		TSharedPtr<FJsonObject> Obj = FMCPPinTypeUtils::ToJson(PinType, ErrCode, ErrMsg);
		if (!Obj.IsValid())
		{
			OutError = BP_MakeError(Request, ErrCode, ErrMsg);
		}
		return Obj;
	}

	// ─── variable summary (used by both bp.get_variable + bp.list_variables) ──────────────────────

	/**
	 * Build a JSON summary of one FBPVariableDescription.
	 *
	 * Shape:
	 *   {
	 *     "name":             "FName",
	 *     "pin_type":         { ...MCPPinTypeUtils JSON... },
	 *     "default_value":    "text-format string from VarDesc.DefaultValue" | null,
	 *     "category_group":   "Category text from VarDesc.Category" | "",
	 *     "tooltip":          "FBPVariableMetaDataEntry[tooltip] value" | "",
	 *     "exposed_on_spawn": bool   (CPF_ExposeOnSpawn or meta "ExposeOnSpawn")
	 *     "replicated":       bool   (CPF_Net)
	 *     "friendly_name":    "FriendlyName" | ""
	 *   }
	 *
	 * Returns nullptr + populates OutError if the variable's pin_type is unsupported (-32032).
	 */
	TSharedPtr<FJsonObject> BP_BuildVariableSummary(
		const FMCPRequest& Request,
		const FBPVariableDescription& Var,
		FMCPResponse& OutError)
	{
		TSharedPtr<FJsonObject> PinTypeObj = BP_PinTypeToJsonOrError(Request, Var.VarType, OutError);
		if (!PinTypeObj.IsValid())
		{
			return nullptr;
		}

		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Var.VarName.ToString());
		Obj->SetObjectField(TEXT("pin_type"), PinTypeObj);

		if (Var.DefaultValue.IsEmpty())
		{
			Obj->SetField(TEXT("default_value"), MakeShared<FJsonValueNull>());
		}
		else
		{
			// FBPVariableDescription stores defaults as text-format strings (UE's ExportText form).
			// Phase 4 returns these verbatim — Day 7+ will parse via FMCPReflection::WritePropertyValueAt
			// for round-trip typed defaults. Reads now ship as opaque strings.
			Obj->SetStringField(TEXT("default_value"), Var.DefaultValue);
		}

		Obj->SetStringField(TEXT("category_group"), Var.Category.ToString());

		// Tooltip + ExposeOnSpawn live in MetaDataArray (FName→FString). Keys are the canonical
		// engine metadata names from FBlueprintMetadata (Tooltip, ExposeOnSpawn). Comparison is
		// case-INSENSITIVE because Epic occasionally serialises with mixed case across versions.
		FString Tooltip;
		bool bExposeOnSpawn = false;
		const FName MD_Tooltip(TEXT("Tooltip"));
		const FName MD_ExposeOnSpawn(TEXT("ExposeOnSpawn"));
		for (const FBPVariableMetaDataEntry& Meta : Var.MetaDataArray)
		{
			if (Meta.DataKey == MD_Tooltip)
			{
				Tooltip = Meta.DataValue;
			}
			else if (Meta.DataKey == MD_ExposeOnSpawn)
			{
				bExposeOnSpawn = Meta.DataValue.Equals(TEXT("true"), ESearchCase::IgnoreCase);
			}
		}
		// Also fold in property-flag based ExposeOnSpawn (engine sets either, depending on user path).
		if (!bExposeOnSpawn && (Var.PropertyFlags & CPF_ExposeOnSpawn) != 0)
		{
			bExposeOnSpawn = true;
		}
		Obj->SetStringField(TEXT("tooltip"), Tooltip);
		Obj->SetBoolField(TEXT("exposed_on_spawn"), bExposeOnSpawn);
		Obj->SetBoolField(TEXT("replicated"), (Var.PropertyFlags & CPF_Net) != 0);
		Obj->SetStringField(TEXT("friendly_name"), Var.FriendlyName);
		return Obj;
	}

	// ─── function signature extraction ────────────────────────────────────────────────────────────

	/**
	 * Resolve a function graph's entry + result nodes (used by bp.list_functions / bp.get_function
	 * to extract signature pins). Either may be null — entry exists for every function graph,
	 * result only when the function returns one or more values.
	 *
	 * The TWeakObjectPtr handoff in GetEntryAndResultNodes is the canonical Epic API; this wrapper
	 * dereferences to raw pointers for ergonomics.
	 */
	void BP_GetFunctionTerminators(
		const UEdGraph* Graph,
		UK2Node_FunctionEntry*& OutEntry,
		UK2Node_FunctionResult*& OutResult)
	{
		OutEntry = nullptr;
		OutResult = nullptr;
		if (!Graph) { return; }

		TWeakObjectPtr<UK2Node_EditablePinBase> EntryWk;
		TWeakObjectPtr<UK2Node_EditablePinBase> ResultWk;
		FBlueprintEditorUtils::GetEntryAndResultNodes(Graph, EntryWk, ResultWk);
		OutEntry = Cast<UK2Node_FunctionEntry>(EntryWk.Get());
		OutResult = Cast<UK2Node_FunctionResult>(ResultWk.Get());
	}

	/**
	 * Emit signature pins for one terminator (entry or result) as a JSON array. Skips Exec pins
	 * (those are flow-control, not parameters). Each entry: { name, pin_type }.
	 *
	 * Returns nullptr if any pin produces an unsupported pin-type error (-32032 surfaced via OutError).
	 */
	TSharedPtr<FJsonValue> BP_BuildSignaturePins(
		const FMCPRequest& Request,
		const UK2Node_EditablePinBase* Terminator,
		EEdGraphPinDirection PinFilterDirection,
		FMCPResponse& OutError)
	{
		TArray<TSharedPtr<FJsonValue>> Items;
		if (!Terminator)
		{
			return MakeShared<FJsonValueArray>(Items);
		}

		// We use the live Pins[] array (resolved + reflects refresh state). UserDefinedPins[] has
		// the authoring info but doesn't reflect inherited signature for interface overrides.
		for (const UEdGraphPin* Pin : Terminator->Pins)
		{
			if (!Pin) { continue; }
			if (Pin->Direction != PinFilterDirection) { continue; }
			// Skip exec pins (then/else flow) — they're not part of the user-declared signature.
			if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) { continue; }

			TSharedPtr<FJsonObject> PinTypeObj = BP_PinTypeToJsonOrError(Request, Pin->PinType, OutError);
			if (!PinTypeObj.IsValid()) { return nullptr; }

			TSharedRef<FJsonObject> PinObj = MakeShared<FJsonObject>();
			PinObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
			PinObj->SetObjectField(TEXT("pin_type"), PinTypeObj);
			Items.Add(MakeShared<FJsonValueObject>(PinObj));
		}
		return MakeShared<FJsonValueArray>(Items);
	}

	/**
	 * Resolve access specifier from FUNC_* flags on the entry node. FunctionEntry tracks
	 * Pub/Prot/Priv via ExtraFlags (user-toggled in the editor).
	 */
	FString BP_AccessSpecifierFromFlags(int32 FunctionFlags)
	{
		if ((FunctionFlags & FUNC_Private)   != 0) { return TEXT("Private"); }
		if ((FunctionFlags & FUNC_Protected) != 0) { return TEXT("Protected"); }
		// Default for K2 user functions is Public (FUNC_Public set in
		// FBlueprintEditorUtils::AddNewFunctionGraph).
		return TEXT("Public");
	}

	/**
	 * Build a JSON summary of one function graph + entry node. Used by bp.list_functions
	 * (compact) and bp.get_function (extended — caller adds local_variables / node_count).
	 *
	 * Shape:
	 *   {
	 *     "name":              "FName from graph",
	 *     "category":          "FKismetUserDeclaredFunctionMetadata::Category" | "",
	 *     "access_specifier":  "Public" | "Protected" | "Private",
	 *     "is_pure":           bool (FUNC_BlueprintPure),
	 *     "is_const":          bool (FUNC_Const),
	 *     "is_static":         bool (FUNC_Static),
	 *     "signature":         { inputs: [...], outputs: [...] }
	 *   }
	 *
	 * Returns nullptr if any pin type is unsupported (-32032 surfaced via OutError).
	 */
	TSharedPtr<FJsonObject> BP_BuildFunctionSummary(
		const FMCPRequest& Request,
		const UEdGraph* Graph,
		FMCPResponse& OutError)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (!Graph)
		{
			Obj->SetField(TEXT("name"), MakeShared<FJsonValueNull>());
			return Obj;
		}
		Obj->SetStringField(TEXT("name"), Graph->GetFName().ToString());

		UK2Node_FunctionEntry* Entry = nullptr;
		UK2Node_FunctionResult* Result = nullptr;
		BP_GetFunctionTerminators(Graph, Entry, Result);

		FString Category;
		if (FKismetUserDeclaredFunctionMetadata* Meta = FBlueprintEditorUtils::GetGraphFunctionMetaData(Graph))
		{
			Category = Meta->Category.ToString();
		}
		Obj->SetStringField(TEXT("category"), Category);

		const int32 Flags = Entry ? Entry->GetFunctionFlags() : 0;
		Obj->SetStringField(TEXT("access_specifier"), BP_AccessSpecifierFromFlags(Flags));
		Obj->SetBoolField(TEXT("is_pure"),   (Flags & FUNC_BlueprintPure) != 0);
		Obj->SetBoolField(TEXT("is_const"),  (Flags & FUNC_Const) != 0);
		Obj->SetBoolField(TEXT("is_static"), (Flags & FUNC_Static) != 0);

		// inputs[]: output pins ON the entry node (data flows OUT of entry INTO the function body)
		// outputs[]: input pins ON the result node (data flows INTO result FROM function body)
		TSharedRef<FJsonObject> SigObj = MakeShared<FJsonObject>();
		{
			TSharedPtr<FJsonValue> Inputs = BP_BuildSignaturePins(Request, Entry, EGPD_Output, OutError);
			if (!Inputs.IsValid()) { return nullptr; }
			SigObj->SetField(TEXT("inputs"), Inputs);

			TSharedPtr<FJsonValue> Outputs = BP_BuildSignaturePins(Request, Result, EGPD_Input, OutError);
			if (!Outputs.IsValid()) { return nullptr; }
			SigObj->SetField(TEXT("outputs"), Outputs);
		}
		Obj->SetObjectField(TEXT("signature"), SigObj);

		return Obj;
	}

	// ─── node summary (bp.list_nodes_in_function) ─────────────────────────────────────────────────

	/**
	 * Build a single connected-edge entry: { node_guid, pin_name }.
	 *
	 * Guards against dangling LinkedTo pointers (corrupt BP) — returns null if the linked pin or its
	 * owning node has gone invalid (caller's array filters these out).
	 */
	TSharedPtr<FJsonValue> BP_BuildPinEdge(const UEdGraphPin* LinkedPin)
	{
		if (!LinkedPin) { return nullptr; }
		const UEdGraphNode* OwningNode = LinkedPin->GetOwningNodeUnchecked();
		if (!OwningNode || !IsValid(OwningNode)) { return nullptr; }

		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("node_guid"), OwningNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
		Obj->SetStringField(TEXT("pin_name"), LinkedPin->PinName.ToString());
		return MakeShared<FJsonValueObject>(Obj);
	}

	/**
	 * Build a JSON summary of one UEdGraphPin + its LinkedTo connections.
	 *
	 * Shape:
	 *   {
	 *     "name":         "PinName",
	 *     "direction":    "Input" | "Output",
	 *     "pin_type":     { ...MCPPinTypeUtils JSON... },
	 *     "connected_to": [ { node_guid, pin_name }, ... ]
	 *   }
	 *
	 * Returns nullptr if the pin's pin_type is unsupported (-32032 propagated via OutError).
	 */
	TSharedPtr<FJsonObject> BP_BuildPinSummary(
		const FMCPRequest& Request,
		const UEdGraphPin* Pin,
		FMCPResponse& OutError)
	{
		if (!Pin) { return nullptr; }

		TSharedPtr<FJsonObject> PinTypeObj = BP_PinTypeToJsonOrError(Request, Pin->PinType, OutError);
		if (!PinTypeObj.IsValid()) { return nullptr; }

		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Pin->PinName.ToString());
		Obj->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("Input") : TEXT("Output"));
		Obj->SetObjectField(TEXT("pin_type"), PinTypeObj);

		TArray<TSharedPtr<FJsonValue>> Edges;
		Edges.Reserve(Pin->LinkedTo.Num());
		for (const UEdGraphPin* Linked : Pin->LinkedTo)
		{
			TSharedPtr<FJsonValue> Edge = BP_BuildPinEdge(Linked);
			if (Edge.IsValid())
			{
				Edges.Add(Edge);
			}
		}
		Obj->SetArrayField(TEXT("connected_to"), Edges);
		return Obj;
	}

	/**
	 * Build a JSON summary of one UEdGraphNode.
	 *
	 * Shape:
	 *   {
	 *     "node_guid": "FGuid stringified",
	 *     "class":     "/Script/.../UK2Node_CallFunction",
	 *     "title":     "GetActorLocation",
	 *     "pins":      [ ...pin summaries... ]
	 *   }
	 *
	 * Returns nullptr if any pin's pin_type is unsupported (-32032 surfaced via OutError).
	 */
	TSharedPtr<FJsonObject> BP_BuildNodeSummary(
		const FMCPRequest& Request,
		const UEdGraphNode* Node,
		FMCPResponse& OutError)
	{
		if (!Node) { return nullptr; }

		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("node_guid"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
		Obj->SetStringField(TEXT("class"), Node->GetClass()->GetPathName());
		Obj->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());

		TArray<TSharedPtr<FJsonValue>> PinArr;
		PinArr.Reserve(Node->Pins.Num());
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin) { continue; }
			TSharedPtr<FJsonObject> PinObj = BP_BuildPinSummary(Request, Pin, OutError);
			if (!PinObj.IsValid()) { return nullptr; }
			PinArr.Add(MakeShared<FJsonValueObject>(PinObj));
		}
		Obj->SetArrayField(TEXT("pins"), PinArr);
		return Obj;
	}
} // namespace

namespace FBlueprintTools
{

// ─── bp.exists (Lane A, no PIE guard) ────────────────────────────────────────────────────────
//
// Resolves a blueprint path and reports presence + parent class + data-only status. Useful for
// AI workflows that need to check whether a target asset exists before mutating it.
//
// Args:    { blueprint_path: string }
// Result:  { exists: bool, generated_class_path: string|null, parent_class_path: string|null,
//            is_data_only: bool }
//
// Errors:
//   -32010 InvalidPath              — path empty / mount point unknown / malformed
//   -32031 BlueprintTypeMismatch    — path resolves to a non-UBlueprint asset
//   (object-not-found surfaces as exists=false, NOT as -32004 — this matches Phase 2 cb.exists
//   convention; -32004 is reserved for paths that should-have-loaded-but-didn't downstream)
FMCPResponse Tool_Exists(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString Path;
	FMCPResponse PathErr;
	if (!BP_RequireBlueprintPath(Request, Path, PathErr)) { return PathErr; }

	int32 ErrCode = 0;
	FString ErrMsg;
	UBlueprint* Blueprint = FMCPBlueprintUtils::LoadBlueprintByPath(Path, ErrCode, ErrMsg);

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	if (!Blueprint)
	{
		// Path-shape errors and type-mismatch errors still surface as real errors. Only the
		// "no asset found" path collapses to exists=false (matches cb.exists semantics).
		if (ErrCode == kMCPErrorInvalidPath)
		{
			return BP_MakeError(Request, ErrCode, ErrMsg);
		}
		if (ErrCode == kMCPErrorBlueprintTypeMismatch)
		{
			return BP_MakeError(Request, ErrCode, ErrMsg);
		}
		// -32004 ObjectNotFound → exists=false success response.
		Out->SetBoolField(TEXT("exists"), false);
		Out->SetField(TEXT("generated_class_path"), MakeShared<FJsonValueNull>());
		Out->SetField(TEXT("parent_class_path"), MakeShared<FJsonValueNull>());
		Out->SetBoolField(TEXT("is_data_only"), false);
		return BP_MakeSuccessObj(Request, Out);
	}

	Out->SetBoolField(TEXT("exists"), true);
	if (UClass* GeneratedClass = FMCPBlueprintUtils::GetGeneratedClass(Blueprint))
	{
		Out->SetStringField(TEXT("generated_class_path"), GeneratedClass->GetPathName());
	}
	else
	{
		Out->SetField(TEXT("generated_class_path"), MakeShared<FJsonValueNull>());
	}
	if (UClass* ParentClass = Blueprint->ParentClass)
	{
		Out->SetStringField(TEXT("parent_class_path"), ParentClass->GetPathName());
	}
	else
	{
		Out->SetField(TEXT("parent_class_path"), MakeShared<FJsonValueNull>());
	}
	Out->SetBoolField(TEXT("is_data_only"), FMCPBlueprintUtils::IsDataOnlyBlueprint(Blueprint));
	return BP_MakeSuccessObj(Request, Out);
}

// ─── bp.get_variable (Lane A, no PIE guard) ──────────────────────────────────────────────────
//
// Returns a single variable's metadata + pin type + default value. Used by AI workflows that
// already know the variable name (no enumeration needed).
//
// Args:    { blueprint_path: string, variable_name: string }
// Result:  { variable: { name, pin_type, default_value, category_group, tooltip, exposed_on_spawn,
//                       replicated, friendly_name }, found: bool }
//
// Errors:
//   -32010 InvalidPath / -32004 ObjectNotFound / -32031 BlueprintTypeMismatch — from path resolve
//   -32602 InvalidParams            — missing variable_name
//   -32037 VariableNotFound         — name not in blueprint
//   -32032 PinTypeUnsupported       — variable's pin type uses a category MCPPinTypeUtils can't
//                                     round-trip (e.g. PC_Verse). Tool fails fast per plan D4.
FMCPResponse Tool_GetVariable(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString Path;
	FMCPResponse PathErr;
	if (!BP_RequireBlueprintPath(Request, Path, PathErr)) { return PathErr; }

	FString VarNameStr;
	if (!Request.Args->TryGetStringField(TEXT("variable_name"), VarNameStr) || VarNameStr.IsEmpty())
	{
		return BP_MakeError(Request, kBPErrorInvalidParams,
			TEXT("missing required string field 'variable_name'"));
	}

	FMCPResponse ResolveErr;
	UBlueprint* Blueprint = BP_ResolveBlueprintOrError(Request, Path, ResolveErr);
	if (!Blueprint) { return ResolveErr; }

	const FName VarName(*VarNameStr);
	const int32 Idx = FMCPBlueprintUtils::FindVariableIndex(Blueprint, VarName);
	if (Idx == INDEX_NONE)
	{
		return BP_MakeError(Request, kMCPErrorVariableNotFound,
			FString::Printf(TEXT("variable '%s' not found on blueprint '%s'"),
				*VarNameStr, *Path));
	}

	FMCPResponse VarErr;
	TSharedPtr<FJsonObject> VarObj = BP_BuildVariableSummary(Request, Blueprint->NewVariables[Idx], VarErr);
	if (!VarObj.IsValid())
	{
		return VarErr;
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetObjectField(TEXT("variable"), VarObj);
	Out->SetBoolField(TEXT("found"), true);
	return BP_MakeSuccessObj(Request, Out);
}

// ─── bp.list_variables (Lane A, no PIE guard, paginated) ─────────────────────────────────────
//
// Enumerates Blueprint->NewVariables[]. Sort key: variable FName (lex case-insensitive).
// FilterHash includes BlueprintPath — caller mutates the path between pages → -32015 StaleCursor.
//
// Args:    { blueprint_path, page_token?, page_size?=100 }
// Result:  { variables: [{name, pin_type, ...}], next_page_token?, total_known: int }
FMCPResponse Tool_ListVariables(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString Path;
	FMCPResponse PathErr;
	if (!BP_RequireBlueprintPath(Request, Path, PathErr)) { return PathErr; }

	FMCPResponse ResolveErr;
	UBlueprint* Blueprint = BP_ResolveBlueprintOrError(Request, Path, ResolveErr);
	if (!Blueprint) { return ResolveErr; }

	// Build a stable sort by variable name (case-insensitive). Holds indices into NewVariables[].
	TArray<int32> SortedIndices;
	SortedIndices.Reserve(Blueprint->NewVariables.Num());
	for (int32 i = 0; i < Blueprint->NewVariables.Num(); ++i)
	{
		SortedIndices.Add(i);
	}
	SortedIndices.StableSort([Blueprint](int32 A, int32 B)
	{
		return Blueprint->NewVariables[A].VarName.LexicalLess(Blueprint->NewVariables[B].VarName);
	});

	const uint64 FilterHash = BP_HashFilter(Path, FString());

	FString TokenWire;
	Request.Args->TryGetStringField(TEXT("page_token"), TokenWire);
	FMCPPageCursor Cursor;
	FMCPResponse CursorErr;
	if (!BP_DecodeCursor(Request, TokenWire, FilterHash, Cursor, CursorErr))
	{
		return CursorErr;
	}

	const int32 PageSize = BP_ClampPageSize(Request.Args, TEXT("page_size"), 100);

	// Skip past sentinel: LastAssetPath holds the previous page's last variable name (lower-case).
	int32 StartIdx = 0;
	if (!Cursor.LastAssetPath.IsEmpty())
	{
		while (StartIdx < SortedIndices.Num())
		{
			const FString CurKey = Blueprint->NewVariables[SortedIndices[StartIdx]].VarName.ToString();
			if (CurKey.Compare(Cursor.LastAssetPath, ESearchCase::IgnoreCase) > 0)
			{
				break;
			}
			++StartIdx;
		}
	}
	const int32 EndIdxExcl = FMath::Min(SortedIndices.Num(), StartIdx + PageSize);

	TArray<TSharedPtr<FJsonValue>> Items;
	Items.Reserve(EndIdxExcl - StartIdx);
	FString LastEmittedKey;
	for (int32 i = StartIdx; i < EndIdxExcl; ++i)
	{
		const FBPVariableDescription& Var = Blueprint->NewVariables[SortedIndices[i]];
		FMCPResponse VarErr;
		TSharedPtr<FJsonObject> VarObj = BP_BuildVariableSummary(Request, Var, VarErr);
		if (!VarObj.IsValid())
		{
			// Per D4 fail-fast — abort whole list on first unsupported pin type. The caller's
			// recovery: skip the offending variable via bp.get_variable on individual names then
			// retry the list with only-resolvable variables (not currently exposed; future scope).
			return VarErr;
		}
		Items.Add(MakeShared<FJsonValueObject>(VarObj));
		LastEmittedKey = Var.VarName.ToString();
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetArrayField(TEXT("variables"), Items);
	Out->SetNumberField(TEXT("total_known"), static_cast<double>(SortedIndices.Num()));

	if (EndIdxExcl < SortedIndices.Num() && !LastEmittedKey.IsEmpty())
	{
		FMCPPageCursor NextCursor;
		NextCursor.FilterHash = FilterHash;
		NextCursor.LastAssetPath = LastEmittedKey;
		NextCursor.TotalKnownSnapshot = SortedIndices.Num();
		Out->SetStringField(TEXT("next_page_token"), FMCPPageCursorUtils::Encode(NextCursor));
	}
	else
	{
		Out->SetField(TEXT("next_page_token"), MakeShared<FJsonValueNull>());
	}
	return BP_MakeSuccessObj(Request, Out);
}

// ─── bp.list_functions (Lane A, no PIE guard, paginated) ─────────────────────────────────────
//
// Enumerates Blueprint->FunctionGraphs[]. Sort key: function FName (lex case-insensitive).
// Each entry includes signature.inputs[] (entry node's output pins) + signature.outputs[]
// (result node's input pins).
//
// Args:    { blueprint_path, page_token?, page_size?=100 }
// Result:  { functions: [{name, category, access_specifier, is_pure, is_const, is_static,
//                        signature}], next_page_token?, total_known: int }
FMCPResponse Tool_ListFunctions(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString Path;
	FMCPResponse PathErr;
	if (!BP_RequireBlueprintPath(Request, Path, PathErr)) { return PathErr; }

	FMCPResponse ResolveErr;
	UBlueprint* Blueprint = BP_ResolveBlueprintOrError(Request, Path, ResolveErr);
	if (!Blueprint) { return ResolveErr; }

	// Filter out null graphs, then stable-sort by graph FName.
	TArray<UEdGraph*> Graphs;
	Graphs.Reserve(Blueprint->FunctionGraphs.Num());
	for (UEdGraph* G : Blueprint->FunctionGraphs)
	{
		if (G) { Graphs.Add(G); }
	}
	Graphs.StableSort([](const UEdGraph& A, const UEdGraph& B)
	{
		return A.GetFName().LexicalLess(B.GetFName());
	});

	const uint64 FilterHash = BP_HashFilter(Path, FString());

	FString TokenWire;
	Request.Args->TryGetStringField(TEXT("page_token"), TokenWire);
	FMCPPageCursor Cursor;
	FMCPResponse CursorErr;
	if (!BP_DecodeCursor(Request, TokenWire, FilterHash, Cursor, CursorErr))
	{
		return CursorErr;
	}

	const int32 PageSize = BP_ClampPageSize(Request.Args, TEXT("page_size"), 100);

	int32 StartIdx = 0;
	if (!Cursor.LastAssetPath.IsEmpty())
	{
		while (StartIdx < Graphs.Num())
		{
			const FString CurKey = Graphs[StartIdx]->GetFName().ToString();
			if (CurKey.Compare(Cursor.LastAssetPath, ESearchCase::IgnoreCase) > 0)
			{
				break;
			}
			++StartIdx;
		}
	}
	const int32 EndIdxExcl = FMath::Min(Graphs.Num(), StartIdx + PageSize);

	TArray<TSharedPtr<FJsonValue>> Items;
	Items.Reserve(EndIdxExcl - StartIdx);
	FString LastEmittedKey;
	for (int32 i = StartIdx; i < EndIdxExcl; ++i)
	{
		FMCPResponse FnErr;
		TSharedPtr<FJsonObject> FnObj = BP_BuildFunctionSummary(Request, Graphs[i], FnErr);
		if (!FnObj.IsValid()) { return FnErr; }
		Items.Add(MakeShared<FJsonValueObject>(FnObj));
		LastEmittedKey = Graphs[i]->GetFName().ToString();
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetArrayField(TEXT("functions"), Items);
	Out->SetNumberField(TEXT("total_known"), static_cast<double>(Graphs.Num()));

	if (EndIdxExcl < Graphs.Num() && !LastEmittedKey.IsEmpty())
	{
		FMCPPageCursor NextCursor;
		NextCursor.FilterHash = FilterHash;
		NextCursor.LastAssetPath = LastEmittedKey;
		NextCursor.TotalKnownSnapshot = Graphs.Num();
		Out->SetStringField(TEXT("next_page_token"), FMCPPageCursorUtils::Encode(NextCursor));
	}
	else
	{
		Out->SetField(TEXT("next_page_token"), MakeShared<FJsonValueNull>());
	}
	return BP_MakeSuccessObj(Request, Out);
}

// ─── bp.get_function (Lane A, no PIE guard) ──────────────────────────────────────────────────
//
// Single function lookup. Extends bp.list_functions's summary with local_variables (FBPVariableDescription
// from FunctionEntry->LocalVariables) + execution_path_node_count (Graph->Nodes.Num()).
//
// Args:    { blueprint_path, function_name }
// Result:  { function: { ...same as list entry..., local_variables: [...], execution_path_node_count } }
FMCPResponse Tool_GetFunction(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString Path;
	FMCPResponse PathErr;
	if (!BP_RequireBlueprintPath(Request, Path, PathErr)) { return PathErr; }

	FString FnNameStr;
	if (!Request.Args->TryGetStringField(TEXT("function_name"), FnNameStr) || FnNameStr.IsEmpty())
	{
		return BP_MakeError(Request, kBPErrorInvalidParams,
			TEXT("missing required string field 'function_name'"));
	}

	FMCPResponse ResolveErr;
	UBlueprint* Blueprint = BP_ResolveBlueprintOrError(Request, Path, ResolveErr);
	if (!Blueprint) { return ResolveErr; }

	const FName FnName(*FnNameStr);
	UEdGraph* Graph = FMCPBlueprintUtils::FindFunctionGraph(Blueprint, FnName);
	if (!Graph)
	{
		return BP_MakeError(Request, kMCPErrorVariableNotFound,
			FString::Printf(TEXT("function '%s' not found on blueprint '%s'"),
				*FnNameStr, *Path));
	}

	FMCPResponse FnErr;
	TSharedPtr<FJsonObject> FnObj = BP_BuildFunctionSummary(Request, Graph, FnErr);
	if (!FnObj.IsValid()) { return FnErr; }

	// Extension fields: local_variables[] (from K2Node_FunctionEntry::LocalVariables) + execution_path_node_count.
	UK2Node_FunctionEntry* Entry = nullptr;
	UK2Node_FunctionResult* Result = nullptr;
	BP_GetFunctionTerminators(Graph, Entry, Result);

	TArray<TSharedPtr<FJsonValue>> Locals;
	if (Entry)
	{
		Locals.Reserve(Entry->LocalVariables.Num());
		for (const FBPVariableDescription& LocalVar : Entry->LocalVariables)
		{
			FMCPResponse LocalErr;
			TSharedPtr<FJsonObject> LocalObj = BP_BuildVariableSummary(Request, LocalVar, LocalErr);
			if (!LocalObj.IsValid()) { return LocalErr; }
			Locals.Add(MakeShared<FJsonValueObject>(LocalObj));
		}
	}
	FnObj->SetArrayField(TEXT("local_variables"), Locals);
	FnObj->SetNumberField(TEXT("execution_path_node_count"), static_cast<double>(Graph->Nodes.Num()));

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetObjectField(TEXT("function"), FnObj);
	return BP_MakeSuccessObj(Request, Out);
}

// ─── bp.list_nodes_in_function (Lane A, no PIE guard, paginated) ─────────────────────────────
//
// Walks Graph->Nodes[]. Sort key: NodeGuid string (deterministic; nodes lack stable user-facing names).
// Each node emits class, title (ListView form), pins[] with LinkedTo edges flattened to
// {node_guid, pin_name} pairs.
//
// FilterHash includes BlueprintPath + FunctionName. Either mutation between pages → -32015.
//
// Args:    { blueprint_path, function_name, page_token?, page_size?=100 }
// Result:  { nodes: [{node_guid, class, title, pins: [{name, direction, pin_type, connected_to}]}],
//            next_page_token?, total_known }
FMCPResponse Tool_ListNodesInFunction(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString Path;
	FMCPResponse PathErr;
	if (!BP_RequireBlueprintPath(Request, Path, PathErr)) { return PathErr; }

	FString FnNameStr;
	if (!Request.Args->TryGetStringField(TEXT("function_name"), FnNameStr) || FnNameStr.IsEmpty())
	{
		return BP_MakeError(Request, kBPErrorInvalidParams,
			TEXT("missing required string field 'function_name'"));
	}

	FMCPResponse ResolveErr;
	UBlueprint* Blueprint = BP_ResolveBlueprintOrError(Request, Path, ResolveErr);
	if (!Blueprint) { return ResolveErr; }

	const FName FnName(*FnNameStr);
	UEdGraph* Graph = FMCPBlueprintUtils::FindFunctionGraph(Blueprint, FnName);
	if (!Graph)
	{
		return BP_MakeError(Request, kMCPErrorVariableNotFound,
			FString::Printf(TEXT("function '%s' not found on blueprint '%s'"),
				*FnNameStr, *Path));
	}

	// Filter null nodes, sort by NodeGuid string (deterministic).
	TArray<UEdGraphNode*> Nodes;
	Nodes.Reserve(Graph->Nodes.Num());
	for (UEdGraphNode* N : Graph->Nodes)
	{
		if (N) { Nodes.Add(N); }
	}
	Nodes.StableSort([](const UEdGraphNode& A, const UEdGraphNode& B)
	{
		return A.NodeGuid.ToString(EGuidFormats::DigitsWithHyphens)
			< B.NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);
	});

	const uint64 FilterHash = BP_HashFilter(Path, FnNameStr);

	FString TokenWire;
	Request.Args->TryGetStringField(TEXT("page_token"), TokenWire);
	FMCPPageCursor Cursor;
	FMCPResponse CursorErr;
	if (!BP_DecodeCursor(Request, TokenWire, FilterHash, Cursor, CursorErr))
	{
		return CursorErr;
	}

	const int32 PageSize = BP_ClampPageSize(Request.Args, TEXT("page_size"), 100);

	int32 StartIdx = 0;
	if (!Cursor.LastAssetPath.IsEmpty())
	{
		while (StartIdx < Nodes.Num())
		{
			const FString CurKey = Nodes[StartIdx]->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);
			if (CurKey.Compare(Cursor.LastAssetPath, ESearchCase::CaseSensitive) > 0)
			{
				break;
			}
			++StartIdx;
		}
	}
	const int32 EndIdxExcl = FMath::Min(Nodes.Num(), StartIdx + PageSize);

	TArray<TSharedPtr<FJsonValue>> Items;
	Items.Reserve(EndIdxExcl - StartIdx);
	FString LastEmittedKey;
	for (int32 i = StartIdx; i < EndIdxExcl; ++i)
	{
		FMCPResponse NodeErr;
		TSharedPtr<FJsonObject> NodeObj = BP_BuildNodeSummary(Request, Nodes[i], NodeErr);
		if (!NodeObj.IsValid()) { return NodeErr; }
		Items.Add(MakeShared<FJsonValueObject>(NodeObj));
		LastEmittedKey = Nodes[i]->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetArrayField(TEXT("nodes"), Items);
	Out->SetNumberField(TEXT("total_known"), static_cast<double>(Nodes.Num()));

	if (EndIdxExcl < Nodes.Num() && !LastEmittedKey.IsEmpty())
	{
		FMCPPageCursor NextCursor;
		NextCursor.FilterHash = FilterHash;
		NextCursor.LastAssetPath = LastEmittedKey;
		NextCursor.TotalKnownSnapshot = Nodes.Num();
		Out->SetStringField(TEXT("next_page_token"), FMCPPageCursorUtils::Encode(NextCursor));
	}
	else
	{
		Out->SetField(TEXT("next_page_token"), MakeShared<FJsonValueNull>());
	}
	return BP_MakeSuccessObj(Request, Out);
}

// ─── Registration ────────────────────────────────────────────────────────────────────────────
void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	// Day 1: bp.exists
	RegisterTool(TEXT("bp.exists"), &Tool_Exists, /*Lane A*/ false);

	// Day 2: bp.get_variable
	RegisterTool(TEXT("bp.get_variable"), &Tool_GetVariable, /*Lane A*/ false);

	// Day 3: bp.list_variables + bp.list_functions
	RegisterTool(TEXT("bp.list_variables"), &Tool_ListVariables, /*Lane A*/ false);
	RegisterTool(TEXT("bp.list_functions"), &Tool_ListFunctions, /*Lane A*/ false);

	// Day 4: bp.get_function + bp.list_nodes_in_function
	RegisterTool(TEXT("bp.get_function"),           &Tool_GetFunction,         /*Lane A*/ false);
	RegisterTool(TEXT("bp.list_nodes_in_function"), &Tool_ListNodesInFunction, /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("Phase 4 Days 1-5: registered 6 bp.* read handlers (all Lane A)"));
}

} // namespace FBlueprintTools
