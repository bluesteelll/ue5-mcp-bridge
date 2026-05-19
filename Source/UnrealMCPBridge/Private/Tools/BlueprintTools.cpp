// Copyright FatumGame. All Rights Reserved.

#include "BlueprintTools.h"

#include "FMCPDispatchQueue.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPAssetPathUtils.h"
#include "Utils/MCPBlueprintUtils.h"
#include "Utils/MCPPageCursor.h"
#include "Utils/MCPPinTypeUtils.h"
#include "Utils/MCPWorldContext.h"

#include "AssetToolsModule.h"
#include "EdGraph/EdGraph.h"
#include "Factories/BlueprintFactory.h"
#include "IAssetTools.h"
#include "Misc/PackageName.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "K2Node_EditablePinBase.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Logging/TokenizedMessage.h"
#include "ScopedTransaction.h"
#include "UObject/Class.h"
#include "UObject/Script.h"
#include "UObject/UnrealType.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformTime.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// BP_ prefix per the unity-build symbol-collision pattern (MakeError/MakeSuccess clash with
	// UE's global ValueOrError templates).
	constexpr int32 kBPErrorInvalidParams = -32602;
	constexpr int32 kBPErrorInternal      = -32603;

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

	// ─── Days 6-10: write-side helpers ────────────────────────────────────────────────────────────

	/** Frozen PIE-mutator refusal — every BP write surfaces this exact pair (smoke asserts substrings). */
	FMCPResponse BP_MakePIEError(const FMCPRequest& Request)
	{
		return BP_MakeError(Request, kMCPErrorPIEActive, kMCPMessagePIEActive);
	}

	/** True if PIE is running. Centralised so write tools share one call site. */
	bool BP_IsPIEActive()
	{
		return FMCPWorldContext::IsPIEActive();
	}

	/**
	 * Resolve a UClass from a class path arg. Returns nullptr + populates OutError on:
	 *   - empty path / no leading slash → -32023 InvalidClassPath
	 *   - LoadObject failure even after ``_C`` retry → -32020 ClassNotFound
	 *
	 * Distinct from ActorTools's helper because we use the kMCPErrorBlueprintTypeMismatch /
	 * kMCPErrorWrongClassFamily codes specific to BP surface diagnostics. Callers post-validate
	 * IsChildOf as appropriate (e.g. reparent's AActor-family check).
	 */
	UClass* BP_ResolveClassOrError(
		const FMCPRequest& Request,
		const FString& ClassPath,
		FMCPResponse& OutError)
	{
		if (ClassPath.IsEmpty() || ClassPath[0] != TEXT('/'))
		{
			OutError = BP_MakeError(Request, kMCPErrorInvalidClassPath,
				FString::Printf(
					TEXT("class_path '%s' invalid — must start with '/' (e.g. '/Script/Engine.Pawn')"),
					*ClassPath));
			return nullptr;
		}
		if (ClassPath.Contains(TEXT("\\")))
		{
			OutError = BP_MakeError(Request, kMCPErrorInvalidClassPath,
				FString::Printf(TEXT("class_path '%s' contains backslash"), *ClassPath));
			return nullptr;
		}
		UClass* Class = LoadObject<UClass>(nullptr, *ClassPath);
		if (!Class && !ClassPath.EndsWith(TEXT("_C")))
		{
			const FString Retry = ClassPath + TEXT("_C");
			Class = LoadObject<UClass>(nullptr, *Retry);
		}
		if (!Class)
		{
			OutError = BP_MakeError(Request, kMCPErrorClassNotFound,
				FString::Printf(
					TEXT("class_path '%s' could not be resolved (LoadObject returned null); ")
					TEXT("Blueprint paths usually need trailing '_C'"),
					*ClassPath));
			return nullptr;
		}
		return Class;
	}

	/**
	 * Parse the ``pin_type`` JSON object arg, propagating -32004 / -32032 errors. Returns true on
	 * success; populates OutError + returns false on failure.
	 */
	bool BP_RequirePinTypeArg(
		const FMCPRequest& Request,
		const TCHAR* FieldName,
		FEdGraphPinType& OutPinType,
		FMCPResponse& OutError)
	{
		const TSharedPtr<FJsonObject>* PinTypeObjPtr = nullptr;
		if (!Request.Args->TryGetObjectField(FieldName, PinTypeObjPtr) || !PinTypeObjPtr || !PinTypeObjPtr->IsValid())
		{
			OutError = BP_MakeError(Request, kBPErrorInvalidParams,
				FString::Printf(TEXT("missing required object field '%s'"), FieldName));
			return false;
		}
		int32 ErrCode = 0;
		FString ErrMsg;
		if (!FMCPPinTypeUtils::FromJson(*PinTypeObjPtr, OutPinType, ErrCode, ErrMsg))
		{
			OutError = BP_MakeError(Request, ErrCode, ErrMsg);
			return false;
		}
		return true;
	}

	/**
	 * Convert an FBPVariableDescription JSON ``default_value`` arg into an FBPVariableDescription
	 * DefaultValue string (UE's ExportText form).
	 *
	 * Strategy:
	 *   - null / missing  → empty string (UE will use the type's natural default)
	 *   - string          → returned verbatim (caller-supplied ExportText)
	 *   - bool/number     → printf to text (matches what UE itself emits for primitive defaults)
	 *   - object/array    → JSON re-serialised — supports {x,y,z} vector defaults, etc. This is a
	 *                       BEST-EFFORT path: ExportText for structs is not perfectly JSON-compatible,
	 *                       so callers wanting precise struct defaults should pass the verbatim
	 *                       ExportText string (e.g. "(X=1.0,Y=2.0,Z=3.0)").
	 *
	 * Phase 4 Days 1-5 reads ship FBPVariableDescription::DefaultValue verbatim; this writer keeps
	 * the contract simple by accepting either the verbatim text OR JSON-likely-correct primitives.
	 */
	FString BP_ConvertJsonDefaultToText(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid() || Value->Type == EJson::Null)
		{
			return FString();
		}
		switch (Value->Type)
		{
		case EJson::String:
			return Value->AsString();
		case EJson::Boolean:
			return Value->AsBool() ? TEXT("true") : TEXT("false");
		case EJson::Number:
			// FString::SanitizeFloat trims trailing zeros + uses %f. Matches int values too because
			// double 42 → "42.000000" → trimmed to "42".
			return FString::SanitizeFloat(Value->AsNumber());
		case EJson::Object:
		case EJson::Array:
		{
			FString Out;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
			FJsonSerializer::Serialize(Value.ToSharedRef(), TEXT(""), Writer);
			return Out;
		}
		default:
			return FString();
		}
	}

	// NOTE: A future bp.set_variable_default tool will need a helper to look up an FProperty by
	// name on the BP's SkeletonGeneratedClass (or GeneratedClass fallback). The plan calls for
	// running the canonical 3-flag edit-const gate before the write. That helper lives here when
	// the tool lands. Phase 4 Days 6-10 ships with the helper UNimplemented — see Tool_AddVariable
	// for the rationale (CPF_DisableEditOnInstance is default on every new BP var; the gate would
	// false-positive every default_value during add).

	/**
	 * Walk a UClass's TFieldIterator and collect the member variable FNames declared on the
	 * BLUEPRINT (CPF_BlueprintVisible). Used by bp.reparent to diff lost members.
	 *
	 * Iteration uses ExcludeSuper so we capture only what the parent itself declares — exactly the
	 * set the reparented child loses if it doesn't appear on the new parent.
	 */
	void BP_CollectClassDeclaredVariableNames(const UClass* Class, TSet<FString>& OutNames)
	{
		OutNames.Reset();
		if (!Class) { return; }
		for (TFieldIterator<FProperty> It(Class, EFieldIteratorFlags::ExcludeSuper); It; ++It)
		{
			const FProperty* Prop = *It;
			if (!Prop) { continue; }
			// Skip parameters / internal / delegates — only surface user-visible BP variables.
			if (Prop->HasAnyPropertyFlags(CPF_Parm)) { continue; }
			OutNames.Add(Prop->GetName());
		}
	}

	/**
	 * Walk a UClass's UFunctions and collect the function FNames declared on it (ExcludeSuper).
	 * Used by bp.reparent to diff lost functions.
	 */
	void BP_CollectClassDeclaredFunctionNames(const UClass* Class, TSet<FString>& OutNames)
	{
		OutNames.Reset();
		if (!Class) { return; }
		for (TFieldIterator<UFunction> It(Class, EFieldIteratorFlags::ExcludeSuper); It; ++It)
		{
			const UFunction* Fn = *It;
			if (!Fn) { continue; }
			OutNames.Add(Fn->GetName());
		}
	}

	/** Build a JSON array of strings from a TSet<FString> with deterministic ordering. */
	TArray<TSharedPtr<FJsonValue>> BP_StringSetToJsonArray(const TSet<FString>& Strings)
	{
		TArray<FString> Sorted = Strings.Array();
		Sorted.Sort();
		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Reserve(Sorted.Num());
		for (const FString& S : Sorted)
		{
			Out.Add(MakeShared<FJsonValueString>(S));
		}
		return Out;
	}

	/**
	 * Tokenize a Phase 4 compile result via the supplied FCompilerResultsLog. Splits messages into
	 * errors[] + warnings[] arrays in the response (note + perf-warning collapse to warnings;
	 * everything below Info is dropped).
	 */
	void BP_SplitCompileMessages(
		const FCompilerResultsLog& Log,
		TArray<TSharedPtr<FJsonValue>>& OutErrors,
		TArray<TSharedPtr<FJsonValue>>& OutWarnings)
	{
		OutErrors.Reset();
		OutWarnings.Reset();
		for (const TSharedRef<FTokenizedMessage>& Msg : Log.Messages)
		{
			const FString Text = Msg->ToText().ToString();
			const EMessageSeverity::Type Severity = Msg->GetSeverity();
			if (Severity == EMessageSeverity::Error)
			{
				OutErrors.Add(MakeShared<FJsonValueString>(Text));
			}
			else if (Severity == EMessageSeverity::Warning
				|| Severity == EMessageSeverity::PerformanceWarning)
			{
				OutWarnings.Add(MakeShared<FJsonValueString>(Text));
			}
			// Info / Notes silently dropped — they're advisory noise that AI clients don't action.
		}
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

// ─── bp.add_variable (Lane A, PIE-guarded — edit-const gate carve-out) ───────────────────────
//
// Adds a member variable to the blueprint with optional default_value, category, tooltip,
// exposed_on_spawn, replicated flags. Auto-triggers Blueprint recompile (UE's AddMemberVariable
// internally calls FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified).
//
// Args:    { blueprint_path, variable_name, pin_type: { ...MCPPinTypeUtils JSON... },
//            default_value?: any, category?: string, tooltip?: string,
//            exposed_on_spawn?: bool, replicated?: bool, bypass_readonly?: bool }
// Result:  { added: bool, variable_name: string }
//
// Errors:
//   -32027 PIEActive
//   -32010 InvalidPath / -32004 ObjectNotFound / -32031 BlueprintTypeMismatch (resolve)
//   -32602 InvalidParams                — missing variable_name OR missing pin_type object
//   -32032 PinTypeUnsupported           — pin_type cannot be parsed by MCPPinTypeUtils
//   -32014 PathInUse                    — variable name already exists on this BP (or on a base
//                                         class — AddMemberVariable refuses to mask inherited vars)
//
// Edit-const gate carve-out: UE defaults every new BP variable to CPF_DisableEditOnInstance, so
// the canonical 3-flag gate (CPF_BlueprintReadOnly | CPF_EditConst | CPF_DisableEditOnInstance)
// would false-positive every add. The default_value passed here lands in FBPVariableDescription::
// DefaultValue — that's the CDO authoring path, NOT a runtime placed-instance write, so the gate
// is semantically wrong. The plan reserves the gate for the future bp.set_variable_default tool.
// args.bypass_readonly is accepted but currently a no-op (forward-compat for when the carve-out
// is re-tightened).
//
// Default value note (D7): if default_value is a string the bridge passes it verbatim into UE's
// AddMemberVariable as the ExportText default. JSON primitive types (bool/number) get a sensible
// printf conversion. Objects/arrays serialise back to JSON — usable for primitive struct defaults
// but not perfectly compatible with all UE struct ExportText forms; callers wanting precise
// struct defaults should pass the verbatim "(X=1.0,Y=2.0,Z=3.0)" string.
FMCPResponse Tool_AddVariable(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (BP_IsPIEActive()) { return BP_MakePIEError(Request); }

	FString Path;
	FMCPResponse PathErr;
	if (!BP_RequireBlueprintPath(Request, Path, PathErr)) { return PathErr; }

	FString VarNameStr;
	if (!Request.Args->TryGetStringField(TEXT("variable_name"), VarNameStr) || VarNameStr.IsEmpty())
	{
		return BP_MakeError(Request, kBPErrorInvalidParams,
			TEXT("missing required string field 'variable_name'"));
	}

	FEdGraphPinType NewPinType;
	FMCPResponse PinTypeErr;
	if (!BP_RequirePinTypeArg(Request, TEXT("pin_type"), NewPinType, PinTypeErr))
	{
		return PinTypeErr;
	}

	FMCPResponse ResolveErr;
	UBlueprint* Blueprint = BP_ResolveBlueprintOrError(Request, Path, ResolveErr);
	if (!Blueprint) { return ResolveErr; }

	const FName VarName(*VarNameStr);

	// Pre-check name collision so we surface -32014 BEFORE any UE work (AddMemberVariable returns
	// false silently on collision — same surface but losing the per-tool error code).
	if (FMCPBlueprintUtils::FindVariableIndex(Blueprint, VarName) != INDEX_NONE)
	{
		return BP_MakeError(Request, kMCPErrorPathInUse,
			FString::Printf(
				TEXT("variable '%s' already exists on blueprint '%s'; use bp.remove_variable + bp.add_variable to replace"),
				*VarNameStr, *Path));
	}

	const TSharedPtr<FJsonValue> DefaultValueField = Request.Args->TryGetField(TEXT("default_value"));
	const FString DefaultValueText = BP_ConvertJsonDefaultToText(DefaultValueField);

	// FScopedTransaction owns the undo group; AddMemberVariable internally calls Modify+Mark.
	const FScopedTransaction Transaction(LOCTEXT("BPAddVariable", "MCP: add blueprint variable"));

	const bool bAdded = FBlueprintEditorUtils::AddMemberVariable(
		Blueprint, VarName, NewPinType, DefaultValueText);
	if (!bAdded)
	{
		// Fall-through: UE rejected the add for an internal reason we didn't pre-screen. Most
		// common is a colliding parent-class variable (we only checked the BP's own NewVariables).
		return BP_MakeError(Request, kMCPErrorPathInUse,
			FString::Printf(
				TEXT("AddMemberVariable failed for '%s' on '%s' — likely masks a variable in a base class"),
				*VarNameStr, *Path));
	}

	// Apply optional metadata fields. Each writes through FBlueprintEditorUtils so the BP is
	// flagged dirty for the next compile pass.
	FString CategoryStr;
	if (Request.Args->TryGetStringField(TEXT("category"), CategoryStr) && !CategoryStr.IsEmpty())
	{
		FBlueprintEditorUtils::SetBlueprintVariableCategory(Blueprint, VarName, /*InScope*/ nullptr,
			FText::FromString(CategoryStr));
	}
	FString TooltipStr;
	if (Request.Args->TryGetStringField(TEXT("tooltip"), TooltipStr) && !TooltipStr.IsEmpty())
	{
		FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, VarName, /*InScope*/ nullptr,
			TEXT("Tooltip"), TooltipStr);
	}
	bool bExposeOnSpawn = false;
	if (Request.Args->TryGetBoolField(TEXT("exposed_on_spawn"), bExposeOnSpawn) && bExposeOnSpawn)
	{
		FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, VarName, /*InScope*/ nullptr,
			TEXT("ExposeOnSpawn"), TEXT("true"));
	}
	bool bReplicated = false;
	if (Request.Args->TryGetBoolField(TEXT("replicated"), bReplicated) && bReplicated)
	{
		const int32 Idx = FMCPBlueprintUtils::FindVariableIndex(Blueprint, VarName);
		if (Idx != INDEX_NONE)
		{
			// Flip the CPF_Net bit on the BP variable's PropertyFlags. The recompile triggered
			// downstream will materialise this on the UClass.
			FBPVariableDescription& Var = Blueprint->NewVariables[Idx];
			Var.PropertyFlags |= CPF_Net;
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		}
	}

	// NOTE: The Phase 4 plan's edit-const 3-flag gate (CPF_BlueprintReadOnly | CPF_EditConst |
	// CPF_DisableEditOnInstance) is INTENTIONALLY OMITTED here. That gate is for runtime mutation
	// of placed instances (actor.set_property semantics) — but UE's AddMemberVariable defaults
	// every new BP variable to CPF_DisableEditOnInstance + CPF_BlueprintVisible + CPF_Edit. The
	// default_value we pass becomes the CDO authoring default (FBPVariableDescription::DefaultValue),
	// not a runtime write to a placed instance, so the gate would falsely reject every add.
	// The plan reserves the gate for the future bp.set_variable_default tool that mutates an
	// existing variable's default on a CDO post-creation — that one would correctly apply.
	// args.bypass_readonly is accepted but currently a no-op (forward-compat).

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("added"), true);
	Out->SetStringField(TEXT("variable_name"), VarNameStr);
	return BP_MakeSuccessObj(Request, Out);
}

// ─── bp.remove_variable (Lane A, PIE-guarded, idempotent) ────────────────────────────────────
//
// Removes a member variable by name. Idempotent — if the variable doesn't exist returns
// {removed: false, was_present: false} as a SUCCESS (matches Phase 3 actor.destroy idempotency).
// UE's RemoveMemberVariable also strips dependent variable nodes from all graphs.
//
// Args:    { blueprint_path, variable_name }
// Result:  { removed: bool, was_present: bool }
//
// Errors:
//   -32027 PIEActive
//   -32010 / -32004 / -32031 (resolve)
//   -32602 InvalidParams
FMCPResponse Tool_RemoveVariable(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (BP_IsPIEActive()) { return BP_MakePIEError(Request); }

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
	const bool bWasPresent = (FMCPBlueprintUtils::FindVariableIndex(Blueprint, VarName) != INDEX_NONE);

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	if (!bWasPresent)
	{
		Out->SetBoolField(TEXT("removed"), false);
		Out->SetBoolField(TEXT("was_present"), false);
		return BP_MakeSuccessObj(Request, Out);
	}

	const FScopedTransaction Transaction(LOCTEXT("BPRemoveVariable", "MCP: remove blueprint variable"));
	FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, VarName);

	Out->SetBoolField(TEXT("removed"), true);
	Out->SetBoolField(TEXT("was_present"), true);
	return BP_MakeSuccessObj(Request, Out);
}

// ─── bp.change_variable_type (Lane A, PIE-guarded) ───────────────────────────────────────────
//
// Changes a member variable's pin type. ALWAYS returns a non-empty warning describing graph-ref
// invalidation risk (UE's ChangeMemberVariableType may turn references in graphs into red error
// nodes that need manual reconnection). With ``drop_default_value=true`` the variable's default
// string is cleared; otherwise the (possibly type-mismatched) default is retained.
//
// Args:    { blueprint_path, variable_name, new_pin_type: { ...MCPPinTypeUtils JSON... },
//            drop_default_value?: bool }
// Result:  { changed: bool, prior_pin_type: { ... }, warning: string }
//
// Errors:
//   -32027 PIEActive
//   -32010 / -32004 / -32031 (resolve)
//   -32602 InvalidParams
//   -32037 VariableNotFound
//   -32032 PinTypeUnsupported (both new type and prior type round-trips)
FMCPResponse Tool_ChangeVariableType(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (BP_IsPIEActive()) { return BP_MakePIEError(Request); }

	FString Path;
	FMCPResponse PathErr;
	if (!BP_RequireBlueprintPath(Request, Path, PathErr)) { return PathErr; }

	FString VarNameStr;
	if (!Request.Args->TryGetStringField(TEXT("variable_name"), VarNameStr) || VarNameStr.IsEmpty())
	{
		return BP_MakeError(Request, kBPErrorInvalidParams,
			TEXT("missing required string field 'variable_name'"));
	}

	FEdGraphPinType NewPinType;
	FMCPResponse PinTypeErr;
	if (!BP_RequirePinTypeArg(Request, TEXT("new_pin_type"), NewPinType, PinTypeErr))
	{
		return PinTypeErr;
	}

	bool bDropDefault = false;
	Request.Args->TryGetBoolField(TEXT("drop_default_value"), bDropDefault);

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

	// Snapshot prior pin type BEFORE the mutation so we can echo it. ChangeMemberVariableType may
	// reset the FBPVariableDescription's VarType in place.
	FMCPResponse PriorErr;
	TSharedPtr<FJsonObject> PriorPinTypeObj = BP_PinTypeToJsonOrError(
		Request, Blueprint->NewVariables[Idx].VarType, PriorErr);
	if (!PriorPinTypeObj.IsValid()) { return PriorErr; }

	const FScopedTransaction Transaction(LOCTEXT("BPChangeVariableType", "MCP: change blueprint variable type"));

	if (bDropDefault)
	{
		// Clear the default BEFORE the type change so UE doesn't try to import the now-wrongly-typed
		// default during recompile. UE accepts an empty string as "use type's natural default".
		Blueprint->NewVariables[Idx].DefaultValue = FString();
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	}

	FBlueprintEditorUtils::ChangeMemberVariableType(Blueprint, VarName, NewPinType);

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("changed"), true);
	Out->SetObjectField(TEXT("prior_pin_type"), PriorPinTypeObj);
	Out->SetStringField(TEXT("warning"),
		TEXT("changing variable type may invalidate graph references to this variable — UE replaces "
			 "incompatible nodes with red error nodes that need manual reconnection. Recompile "
			 "and inspect the graph after this call. Pass drop_default_value=true if the prior "
			 "default is no longer type-compatible."));
	return BP_MakeSuccessObj(Request, Out);
}

// ─── bp.add_function (Lane A, PIE-guarded) ───────────────────────────────────────────────────
//
// Creates a new user function graph on the Blueprint with optional input/output signatures. The
// function entry node receives one UserDefinedPin per input; the function result node (lazily
// created when outputs are present) receives one per output.
//
// Args:    { blueprint_path, function_name,
//            inputs?:  [ { name, pin_type } ],
//            outputs?: [ { name, pin_type } ],
//            category?: string }
// Result:  { added: bool, function_name: string }
//
// Errors:
//   -32027 PIEActive
//   -32010 / -32004 / -32031 (resolve)
//   -32602 InvalidParams
//   -32014 PathInUse                   — function name collides with an existing function graph
//   -32032 PinTypeUnsupported          — any input/output pin_type fails to parse
//
// Note: ``access_specifier`` arg from the plan is intentionally OMITTED here. The function entry
// node defaults to Public; changing access requires writing FUNC_Private / FUNC_Protected onto
// Entry->ExtraFlags through UK2Node_FunctionEntry's API which is intricate enough to defer to a
// future bp.set_function_access tool. Callers post-create via that future tool.
FMCPResponse Tool_AddFunction(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (BP_IsPIEActive()) { return BP_MakePIEError(Request); }

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
	if (FMCPBlueprintUtils::FindFunctionGraphIndex(Blueprint, FnName) != INDEX_NONE)
	{
		return BP_MakeError(Request, kMCPErrorPathInUse,
			FString::Printf(
				TEXT("function '%s' already exists on blueprint '%s'; use bp.remove_function first"),
				*FnNameStr, *Path));
	}

	// Parse inputs[] + outputs[] arrays up-front so we can fail fast on bad pin types BEFORE
	// creating the graph. Each entry: { name: string, pin_type: object }.
	struct FParsedSignaturePin
	{
		FName Name;
		FEdGraphPinType Type;
	};
	TArray<FParsedSignaturePin> Inputs;
	TArray<FParsedSignaturePin> Outputs;

	auto ParseSignatureArray = [&Request](const TCHAR* FieldName, TArray<FParsedSignaturePin>& Out,
		FMCPResponse& OutError) -> bool
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Request.Args->TryGetArrayField(FieldName, Arr) || !Arr)
		{
			return true; // optional field
		}
		Out.Reserve(Arr->Num());
		for (int32 i = 0; i < Arr->Num(); ++i)
		{
			const TSharedPtr<FJsonValue>& V = (*Arr)[i];
			if (!V.IsValid() || V->Type != EJson::Object)
			{
				OutError = BP_MakeError(Request, kBPErrorInvalidParams,
					FString::Printf(TEXT("%s[%d] is not an object"), FieldName, i));
				return false;
			}
			const TSharedPtr<FJsonObject>& Item = V->AsObject();
			FString PinNameStr;
			if (!Item->TryGetStringField(TEXT("name"), PinNameStr) || PinNameStr.IsEmpty())
			{
				OutError = BP_MakeError(Request, kBPErrorInvalidParams,
					FString::Printf(TEXT("%s[%d] missing 'name'"), FieldName, i));
				return false;
			}
			const TSharedPtr<FJsonObject>* PinTypeObjPtr = nullptr;
			if (!Item->TryGetObjectField(TEXT("pin_type"), PinTypeObjPtr) || !PinTypeObjPtr || !PinTypeObjPtr->IsValid())
			{
				OutError = BP_MakeError(Request, kBPErrorInvalidParams,
					FString::Printf(TEXT("%s[%d] missing 'pin_type' object"), FieldName, i));
				return false;
			}
			FParsedSignaturePin Pin;
			Pin.Name = FName(*PinNameStr);
			int32 ErrCode = 0;
			FString ErrMsg;
			if (!FMCPPinTypeUtils::FromJson(*PinTypeObjPtr, Pin.Type, ErrCode, ErrMsg))
			{
				OutError = BP_MakeError(Request, ErrCode,
					FString::Printf(TEXT("%s[%d]: %s"), FieldName, i, *ErrMsg));
				return false;
			}
			Out.Add(Pin);
		}
		return true;
	};

	FMCPResponse SignatureErr;
	if (!ParseSignatureArray(TEXT("inputs"), Inputs, SignatureErr))   { return SignatureErr; }
	if (!ParseSignatureArray(TEXT("outputs"), Outputs, SignatureErr)) { return SignatureErr; }

	const FScopedTransaction Transaction(LOCTEXT("BPAddFunction", "MCP: add blueprint function"));

	// Create the empty function graph. UEdGraphSchema_K2 is the schema for K2 (Blueprint) graphs.
	UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
		Blueprint, FnName, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	if (!NewGraph)
	{
		return BP_MakeError(Request, -32603,
			FString::Printf(TEXT("CreateNewGraph returned null for function '%s'"), *FnNameStr));
	}

	// AddFunctionGraph<UFunction*>(..., SignatureFromObject=nullptr) creates user-editable
	// Entry/Result nodes and adds the graph to Blueprint->FunctionGraphs. nullptr SignatureType
	// is the canonical Epic pattern for "user-defined function, no override".
	UFunction* NullSignature = nullptr;
	FBlueprintEditorUtils::AddFunctionGraph<UFunction>(Blueprint, NewGraph,
		/*bIsUserCreated*/ true, NullSignature);

	// Locate the entry node so we can append input UserDefinedPins.
	UK2Node_FunctionEntry* EntryNode = nullptr;
	UK2Node_FunctionResult* ResultNode = nullptr;
	BP_GetFunctionTerminators(NewGraph, EntryNode, ResultNode);
	if (!EntryNode)
	{
		// AddFunctionGraph guarantees an entry node — if missing the BP is corrupt; surface.
		FBlueprintEditorUtils::RemoveGraph(Blueprint, NewGraph, EGraphRemoveFlags::Recompile);
		return BP_MakeError(Request, -32603,
			TEXT("AddFunctionGraph did not produce a UK2Node_FunctionEntry; aborted (graph rolled back)"));
	}

	for (const FParsedSignaturePin& InputPin : Inputs)
	{
		EntryNode->CreateUserDefinedPin(InputPin.Name, InputPin.Type, EGPD_Output, /*bUseUniqueName*/ true);
	}

	if (Outputs.Num() > 0)
	{
		// Result node is created lazily — only when the function needs to return values. Use
		// SpawnNode on the schema to honor proper initialisation order. The simpler API: locate or
		// add via the schema-aware helper from UEdGraphSchema_K2.
		if (!ResultNode)
		{
			// SpawnIntermediate is the Epic-blessed way to add a result node to an existing
			// function graph after the fact. We create it parented to NewGraph and let UE wire it.
			FGraphNodeCreator<UK2Node_FunctionResult> Creator(*NewGraph);
			ResultNode = Creator.CreateNode();
			ResultNode->FunctionReference.SetSelfMember(FnName);
			Creator.Finalize();

			// Connect the entry's Then pin to the result's Execute pin so the function has flow.
			UEdGraphPin* EntryThen = EntryNode->FindPin(UEdGraphSchema_K2::PN_Then);
			UEdGraphPin* ResultExec = ResultNode->FindPin(UEdGraphSchema_K2::PN_Execute);
			if (EntryThen && ResultExec)
			{
				EntryThen->MakeLinkTo(ResultExec);
			}
		}
		for (const FParsedSignaturePin& OutputPin : Outputs)
		{
			ResultNode->CreateUserDefinedPin(OutputPin.Name, OutputPin.Type, EGPD_Input, /*bUseUniqueName*/ true);
		}
	}

	// Optional category metadata — stored on the function graph via FKismetUserDeclaredFunctionMetadata.
	FString CategoryStr;
	if (Request.Args->TryGetStringField(TEXT("category"), CategoryStr) && !CategoryStr.IsEmpty())
	{
		if (FKismetUserDeclaredFunctionMetadata* Meta = FBlueprintEditorUtils::GetGraphFunctionMetaData(NewGraph))
		{
			Meta->Category = FText::FromString(CategoryStr);
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("added"), true);
	Out->SetStringField(TEXT("function_name"), FnNameStr);
	return BP_MakeSuccessObj(Request, Out);
}

// ─── bp.remove_function (Lane A, PIE-guarded, idempotent) ────────────────────────────────────
//
// Removes a function graph by name. Idempotent — if missing, returns {removed: false,
// was_present: false}. UE's RemoveGraph + EGraphRemoveFlags::Recompile retriggers compile so
// dependent nodes invalidate cleanly.
//
// Args:    { blueprint_path, function_name }
// Result:  { removed: bool, was_present: bool }
//
// Errors: -32027 / -32010 / -32004 / -32031 / -32602 (standard set)
FMCPResponse Tool_RemoveFunction(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (BP_IsPIEActive()) { return BP_MakePIEError(Request); }

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

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	if (!Graph)
	{
		Out->SetBoolField(TEXT("removed"), false);
		Out->SetBoolField(TEXT("was_present"), false);
		return BP_MakeSuccessObj(Request, Out);
	}

	const FScopedTransaction Transaction(LOCTEXT("BPRemoveFunction", "MCP: remove blueprint function"));
	FBlueprintEditorUtils::RemoveGraph(Blueprint, Graph, EGraphRemoveFlags::Recompile);

	Out->SetBoolField(TEXT("removed"), true);
	Out->SetBoolField(TEXT("was_present"), true);
	return BP_MakeSuccessObj(Request, Out);
}

// ─── bp.reparent (Lane A, PIE-guarded, EXPERIMENTAL, confirm_dangerous-gated) ────────────────
//
// Changes the parent class of a Blueprint. EXPERIMENTAL — UE provides no clean rollback for
// reparent; variables/functions inherited from the prior parent that don't exist on the new
// parent become invalid (red error nodes / removed). Tool gate:
//   1. ``args.confirm_dangerous=true`` REQUIRED (literal bool). Missing/false → -32033 ReparentUnsafe.
//   2. New parent must resolve to a UClass.
//   3. New parent must be in the same AActor / UObject family as the current parent (-32011 if not).
//
// Args:    { blueprint_path, new_parent_class_path, confirm_dangerous: true }
// Result:  { reparented: bool, prior_parent: string, lost_variables: [string], lost_functions: [string] }
//
// Errors:
//   -32027 PIEActive
//   -32010 / -32004 / -32031 (resolve)
//   -32602 InvalidParams (missing new_parent_class_path)
//   -32023 / -32020 (class resolution)
//   -32033 ReparentUnsafe (confirm_dangerous missing/false)
//   -32011 WrongClass (AActor BP reparented to non-AActor class)
//
// Logs at Display: "MCP bp.reparent: <bp_path> from <old_parent> to <new_parent> (lost N vars,
// M funcs)" — same pattern as UBlueprintEditorLibrary::ReparentBlueprint warnings, scoped to MCP.
FMCPResponse Tool_Reparent(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (BP_IsPIEActive()) { return BP_MakePIEError(Request); }

	FString Path;
	FMCPResponse PathErr;
	if (!BP_RequireBlueprintPath(Request, Path, PathErr)) { return PathErr; }

	FString NewParentPath;
	if (!Request.Args->TryGetStringField(TEXT("new_parent_class_path"), NewParentPath) || NewParentPath.IsEmpty())
	{
		return BP_MakeError(Request, kBPErrorInvalidParams,
			TEXT("missing required string field 'new_parent_class_path'"));
	}

	// Confirm gate FIRST — even before resolving the BP, so the surface error is consistent
	// regardless of what else is wrong. Pass requires LITERAL bool true (truthy != true).
	bool bConfirmDangerous = false;
	const bool bHasConfirm = Request.Args->TryGetBoolField(TEXT("confirm_dangerous"), bConfirmDangerous);
	if (!bHasConfirm || !bConfirmDangerous)
	{
		return BP_MakeError(Request, kMCPErrorReparentUnsafe,
			TEXT("bp.reparent requires args.confirm_dangerous=true; this operation may invalidate "
				 "variables/functions inherited from prior parent class; see failure_modes"));
	}

	FMCPResponse ResolveErr;
	UBlueprint* Blueprint = BP_ResolveBlueprintOrError(Request, Path, ResolveErr);
	if (!Blueprint) { return ResolveErr; }

	FMCPResponse ClassErr;
	UClass* NewParentClass = BP_ResolveClassOrError(Request, NewParentPath, ClassErr);
	if (!NewParentClass) { return ClassErr; }

	// Validate class family — if current parent is AActor-family, new parent MUST also be. UE
	// allows arbitrary reparent technically but the result is unusable; surface as -32011 so AI
	// gets a typed error rather than a compiler explosion later.
	UClass* OldParentClass = Blueprint->ParentClass;
	if (OldParentClass && OldParentClass->IsChildOf(AActor::StaticClass())
		&& !NewParentClass->IsChildOf(AActor::StaticClass()))
	{
		return BP_MakeError(Request, kMCPErrorWrongClass,
			FString::Printf(
				TEXT("blueprint's current parent '%s' is an AActor subclass; cannot reparent to non-AActor class '%s'"),
				*OldParentClass->GetPathName(), *NewParentClass->GetPathName()));
	}

	// No-op short-circuit — same parent is not an error but produces zero work and a noisy log if
	// we just forwarded to UE.
	if (NewParentClass == OldParentClass)
	{
		TSharedRef<FJsonObject> NoOpOut = MakeShared<FJsonObject>();
		NoOpOut->SetBoolField(TEXT("reparented"), false);
		NoOpOut->SetStringField(TEXT("prior_parent"), OldParentClass->GetPathName());
		NoOpOut->SetArrayField(TEXT("lost_variables"), TArray<TSharedPtr<FJsonValue>>());
		NoOpOut->SetArrayField(TEXT("lost_functions"), TArray<TSharedPtr<FJsonValue>>());
		return BP_MakeSuccessObj(Request, NoOpOut);
	}

	// Diff variables / functions that the OLD parent declared but the NEW parent does NOT.
	// These are the members the child BP would lose access to after reparent.
	TSet<FString> OldVars, NewVars, OldFuncs, NewFuncs;
	BP_CollectClassDeclaredVariableNames(OldParentClass, OldVars);
	BP_CollectClassDeclaredVariableNames(NewParentClass, NewVars);
	BP_CollectClassDeclaredFunctionNames(OldParentClass, OldFuncs);
	BP_CollectClassDeclaredFunctionNames(NewParentClass, NewFuncs);

	const TSet<FString> LostVars = OldVars.Difference(NewVars);
	const TSet<FString> LostFuncs = OldFuncs.Difference(NewFuncs);

	// Perform reparent — mirror UBlueprintEditorLibrary::ReparentBlueprint's body to avoid a hard
	// dep on the BlueprintEditorLibrary module. Same compile options it uses for the post-reparent
	// compile pass.
	const FScopedTransaction Transaction(LOCTEXT("BPReparent", "MCP: reparent blueprint"));
	Blueprint->Modify();

	Blueprint->ParentClass = NewParentClass;

	if (Blueprint->SimpleConstructionScript != nullptr)
	{
		Blueprint->SimpleConstructionScript->ValidateSceneRootNodes();
	}

	FBlueprintEditorUtils::RefreshAllNodes(Blueprint);
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	EBlueprintCompileOptions CompileOptions =
		EBlueprintCompileOptions::SkipSave
		| EBlueprintCompileOptions::UseDeltaSerializationDuringReinstancing
		| EBlueprintCompileOptions::SkipNewVariableDefaultsDetection;
	if (GEditor && GEditor->PlayWorld != nullptr)
	{
		// Wouldn't happen here because PIE guard above refuses, but UE's library guards this so we
		// mirror for safety in case the PIE check window race-allows entry.
		CompileOptions |= EBlueprintCompileOptions::IncludeCDOInReferenceReplacement;
	}
	FKismetEditorUtilities::CompileBlueprint(Blueprint, CompileOptions);

	const FString OldParentName = OldParentClass ? OldParentClass->GetPathName() : FString(TEXT("None"));
	UE_LOG(LogMCP, Display,
		TEXT("MCP bp.reparent: %s from %s to %s (lost %d vars, %d funcs)"),
		*Path, *OldParentName, *NewParentClass->GetPathName(),
		LostVars.Num(), LostFuncs.Num());

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("reparented"), true);
	Out->SetStringField(TEXT("prior_parent"), OldParentName);
	Out->SetArrayField(TEXT("lost_variables"), BP_StringSetToJsonArray(LostVars));
	Out->SetArrayField(TEXT("lost_functions"), BP_StringSetToJsonArray(LostFuncs));
	return BP_MakeSuccessObj(Request, Out);
}

// ─── bp.compile (Lane A sync, PIE-guarded) ────────────────────────────────────────────────────
//
// Synchronous single-Blueprint compile. Captures messages into a private FCompilerResultsLog and
// splits into errors[] / warnings[] arrays. Returns compiled=true iff post-compile
// Blueprint->Status ∈ {BS_UpToDate, BS_UpToDateWithWarnings}.
//
// Args:    { blueprint_path, fail_on_error?: bool=false }
// Result:  { compiled: bool, errors: [string], warnings: [string], duration_ms: float, status: string }
//
// Errors:
//   -32027 PIEActive
//   -32010 / -32004 / -32031 (resolve)
//   -32030 KismetCompilationError — only when args.fail_on_error=true AND compile produced errors.
//                                    Same result payload is embedded in the error envelope for
//                                    AI strict-mode diagnostic surface.
//
// Threading: bp.compile internally schedules compile work but blocks until done. ~50ms-2s per BP
// in our experience. Async batch use bp.compile_all_dirty instead.
FMCPResponse Tool_Compile(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (BP_IsPIEActive()) { return BP_MakePIEError(Request); }

	FString Path;
	FMCPResponse PathErr;
	if (!BP_RequireBlueprintPath(Request, Path, PathErr)) { return PathErr; }

	bool bFailOnError = false;
	if (Request.Args.IsValid())
	{
		Request.Args->TryGetBoolField(TEXT("fail_on_error"), bFailOnError);
	}

	FMCPResponse ResolveErr;
	UBlueprint* Blueprint = BP_ResolveBlueprintOrError(Request, Path, ResolveErr);
	if (!Blueprint) { return ResolveErr; }

	const double StartTime = FPlatformTime::Seconds();

	FCompilerResultsLog ResultsLog;
	ResultsLog.bSilentMode = true;
	ResultsLog.SetSourcePath(Path);

	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &ResultsLog);

	const double DurationMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;

	TArray<TSharedPtr<FJsonValue>> Errors;
	TArray<TSharedPtr<FJsonValue>> Warnings;
	BP_SplitCompileMessages(ResultsLog, Errors, Warnings);

	const EBlueprintStatus Status = Blueprint->Status;
	const bool bCompiled = (Status == BS_UpToDate || Status == BS_UpToDateWithWarnings);

	const TCHAR* StatusStr = TEXT("Unknown");
	switch (Status)
	{
	case BS_Unknown:               StatusStr = TEXT("Unknown");               break;
	case BS_Dirty:                 StatusStr = TEXT("Dirty");                 break;
	case BS_Error:                 StatusStr = TEXT("Error");                 break;
	case BS_UpToDate:              StatusStr = TEXT("UpToDate");              break;
	case BS_BeingCreated:          StatusStr = TEXT("BeingCreated");          break;
	case BS_UpToDateWithWarnings:  StatusStr = TEXT("UpToDateWithWarnings");  break;
	default:                       StatusStr = TEXT("Unknown");               break;
	}

	TSharedRef<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetBoolField(TEXT("compiled"), bCompiled);
	ResultObj->SetArrayField(TEXT("errors"), Errors);
	ResultObj->SetArrayField(TEXT("warnings"), Warnings);
	ResultObj->SetNumberField(TEXT("duration_ms"), DurationMs);
	ResultObj->SetStringField(TEXT("status"), StatusStr);

	if (bFailOnError && !bCompiled)
	{
		FMCPResponse Err = BP_MakeError(Request, kMCPErrorKismetCompilationError,
			FString::Printf(
				TEXT("blueprint '%s' failed strict compile: %d errors, %d warnings (status=%s)"),
				*Path, Errors.Num(), Warnings.Num(), StatusStr));
		// Embed the same diagnostic payload as the success path so strict callers don't have to
		// re-run with fail_on_error=false to get details.
		Err.Result = MakeShared<FJsonValueObject>(ResultObj);
		return Err;
	}

	return BP_MakeSuccessObj(Request, ResultObj);
}

// ─── bp.create_blueprint — create new UBlueprint asset with specified parent class ──────────
//
// Args:
//   - parent_class_path: string (required)  /Script/Module.Class  OR  /Game/.../BP.BP_C
//                                            The new BP will subclass this class. Examples:
//                                              /Script/Engine.Actor          → actor BP
//                                              /Script/Engine.HUD            → HUD BP
//                                              /Script/Engine.GameModeBase   → game mode BP
//                                              /Script/Engine.Pawn           → pawn BP
//                                              /Script/UMG.UserWidget        → user widget (also see umg.create_widget_blueprint)
//   - dest_path:         string (required)  /Game/.../BP_NewName
//   - save:              bool   (optional)  default false
//
// Result:
//   - created:          bool
//   - asset_path:       string  (e.g. "/Game/UI/BP_MainHUD.BP_MainHUD")
//   - generated_class:  string  ("/Game/UI/BP_MainHUD.BP_MainHUD_C")
//   - parent_class:     string  echo
//   - saved:            bool
//
// PIE-guarded. Lane A.
//
// Note: BP factories are class-specific. We use UBlueprintFactory (covers AActor and most
// engine subclasses). For UserWidget specifically use umg.create_widget_blueprint which
// uses UWidgetBlueprintFactory.
FMCPResponse Tool_CreateBlueprint(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (GEditor && GEditor->PlayWorld != nullptr)
	{
		return BP_MakeError(Request, kMCPErrorPIEActive, kMCPMessagePIEActive);
	}

	if (!Request.Args.IsValid())
	{
		return BP_MakeError(Request, kBPErrorInvalidParams,
			TEXT("bp.create_blueprint requires args.parent_class_path + args.dest_path"));
	}

	FString ParentClassPath, DestPathRaw;
	if (!Request.Args->TryGetStringField(TEXT("parent_class_path"), ParentClassPath) || ParentClassPath.IsEmpty())
	{
		return BP_MakeError(Request, kBPErrorInvalidParams,
			TEXT("missing required string field 'parent_class_path'"));
	}
	if (!Request.Args->TryGetStringField(TEXT("dest_path"), DestPathRaw) || DestPathRaw.IsEmpty())
	{
		return BP_MakeError(Request, kBPErrorInvalidParams,
			TEXT("missing required string field 'dest_path'"));
	}

	// Resolve parent class (with _C autoload).
	UClass* ParentClass = LoadClass<UObject>(nullptr, *ParentClassPath);
	if (!ParentClass)
	{
		const FString WithC = ParentClassPath.EndsWith(TEXT("_C")) ? ParentClassPath : (ParentClassPath + TEXT("_C"));
		ParentClass = LoadClass<UObject>(nullptr, *WithC);
	}
	if (!ParentClass)
	{
		return BP_MakeError(Request, kMCPErrorClassNotFound,
			FString::Printf(TEXT("could not LoadClass '%s' (also tried _C suffix)"), *ParentClassPath));
	}
	if (ParentClass->HasAnyClassFlags(CLASS_Abstract))
	{
		// Abstract parents are actually FINE for BP subclasses (most engine parents are abstract).
		// Skip the abstract check here.
	}

	// Validate dest path.
	const FString DestPathNorm = FMCPAssetPathUtils::Normalize(DestPathRaw);
	if (DestPathNorm.IsEmpty())
	{
		return BP_MakeError(Request, kMCPErrorInvalidPath,
			FString::Printf(TEXT("dest_path '%s' is not a valid mount-prefixed path"), *DestPathRaw));
	}
	if (FPackageName::DoesPackageExist(DestPathNorm))
	{
		return BP_MakeError(Request, kMCPErrorPathInUse,
			FString::Printf(TEXT("dest_path '%s' already exists on disk"), *DestPathNorm));
	}

	const FString PackagePath = FPaths::GetPath(DestPathNorm);
	const FString AssetName = FPaths::GetBaseFilename(DestPathNorm);

	// Create via UBlueprintFactory.
	UBlueprintFactory* Factory = NewObject<UBlueprintFactory>();
	Factory->ParentClass = ParentClass;

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	UObject* NewAsset = AssetTools.CreateAsset(AssetName, PackagePath, UBlueprint::StaticClass(), Factory);
	if (!NewAsset)
	{
		return BP_MakeError(Request, kBPErrorInternal,
			FString::Printf(TEXT("UBlueprintFactory failed to create BP from parent '%s' at '%s'"),
				*ParentClass->GetPathName(), *DestPathNorm));
	}

	UBlueprint* NewBP = Cast<UBlueprint>(NewAsset);
	if (NewBP && NewBP->GetOutermost())
	{
		NewBP->GetOutermost()->MarkPackageDirty();
	}

	bool bSaveRequested = false, bSavedOk = false;
	Request.Args->TryGetBoolField(TEXT("save"), bSaveRequested);
	if (bSaveRequested)
	{
		if (UEditorAssetSubsystem* EAS = GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr)
		{
			bSavedOk = EAS->SaveLoadedAsset(NewAsset, /*bOnlyIfIsDirty*/ true);
		}
	}

	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("created"), true);
	Out->SetStringField(TEXT("asset_path"), NewAsset->GetPathName());
	Out->SetStringField(TEXT("generated_class"),
		NewBP && NewBP->GeneratedClass ? NewBP->GeneratedClass->GetPathName() : FString());
	Out->SetStringField(TEXT("parent_class"), ParentClass->GetPathName());
	Out->SetBoolField(TEXT("saved"), bSavedOk);
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

	// Day 6: variable add/remove (writes, PIE-guarded)
	RegisterTool(TEXT("bp.add_variable"),     &Tool_AddVariable,    /*Lane A*/ false);
	RegisterTool(TEXT("bp.remove_variable"),  &Tool_RemoveVariable, /*Lane A*/ false);

	// Day 7: variable retype (writes, PIE-guarded)
	RegisterTool(TEXT("bp.change_variable_type"), &Tool_ChangeVariableType, /*Lane A*/ false);

	// Day 8: function add/remove (writes, PIE-guarded)
	RegisterTool(TEXT("bp.add_function"),    &Tool_AddFunction,    /*Lane A*/ false);
	RegisterTool(TEXT("bp.remove_function"), &Tool_RemoveFunction, /*Lane A*/ false);

	// Day 9: reparent (experimental, PIE-guarded, confirm_dangerous-gated)
	RegisterTool(TEXT("bp.reparent"), &Tool_Reparent, /*Lane A*/ false);

	// Day 10: synchronous single-BP compile (PIE-guarded). bp.compile_all_dirty is the async
	// composite registered separately by FBlueprintCompositeTools::Register.
	RegisterTool(TEXT("bp.compile"), &Tool_Compile, /*Lane A*/ false);

	// 2026-05 addition: generic BP asset creator with explicit parent class. Covers HUD,
	// GameModeBase, Pawn, Actor, etc. UserWidget specifically also has umg.create_widget_blueprint
	// for proper UWidgetBlueprintFactory invocation.
	RegisterTool(TEXT("bp.create_blueprint"), &Tool_CreateBlueprint, /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("Phase 4 Days 1-10 + bp.create_blueprint: registered 14 bp.* handlers (6 reads + 6 writes + 1 compile + 1 creator, all Lane A); ")
		TEXT("bp.compile_all_dirty registered separately via FBlueprintCompositeTools"));
}

} // namespace FBlueprintTools

#undef LOCTEXT_NAMESPACE
