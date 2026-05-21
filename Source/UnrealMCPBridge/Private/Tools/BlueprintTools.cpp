// Copyright FatumGame. All Rights Reserved.

#include "BlueprintTools.h"

#include "FMCPDispatchQueue.h"
#include "MCPMutatorScope.h"
#include "MCPToolHelpers.h"
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
#include "UObject/Class.h"
#include "UObject/Script.h"
#include "UObject/UnrealType.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformTime.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// BP_ prefix per the unity-build symbol-collision pattern. XX_StampIds / XX_MakeError /
	// XX_MakeSuccessObj removed in Phase 3 — use FMCPToolHelpers::Xxx from MCPToolHelpers.h
	// (kMCPErrorInvalidParams replaced by canonical kMCPErrorInvalidParams). Per-surface internal
	// code retained for readability — same value as kMCPErrorInternal, distinct semantic naming.
	constexpr int32 kBPErrorInternal = -32603;

	// ─── arg parsing helpers ─────────────────────────────────────────────────────────────────────

	/** Read ``args.blueprint_path`` field; emit -32602 InvalidParams on missing/empty. */
	bool BP_RequireBlueprintPath(const FMCPRequest& Request, FString& OutPath, FMCPResponse& OutError)
	{
		return FMCPToolHelpers::RequireStringField(Request, TEXT("blueprint_path"), OutPath, OutError);
	}

	/** Resolve blueprint by path + emit appropriate error. Returns nullptr + populates OutError on failure. */
	UBlueprint* BP_ResolveBlueprintOrError(const FMCPRequest& Request, const FString& Path, FMCPResponse& OutError)
	{
		int32 ErrCode = 0;
		FString ErrMsg;
		UBlueprint* Blueprint = FMCPBlueprintUtils::LoadBlueprintByPath(Path, ErrCode, ErrMsg);
		if (!Blueprint)
		{
			OutError = FMCPToolHelpers::MakeError(Request, ErrCode, ErrMsg);
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
			OutError = FMCPToolHelpers::MakeError(Request, kMCPErrorStaleCursor,
				FString::Printf(TEXT("page_token decode failed: %s"), *DecodeErr));
			return false;
		}
		if (!FMCPPageCursorUtils::ValidateAgainstFilter(OutCursor, ExpectedFilterHash))
		{
			OutError = FMCPToolHelpers::MakeError(Request, kMCPErrorStaleCursor,
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
			OutError = FMCPToolHelpers::MakeError(Request, ErrCode, ErrMsg);
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
	// BP_MakePIEError / BP_IsPIEActive removed in Phase 3 — FMCPMutatorScope handles PIE guard
	// + FScopedTransaction + MarkPackageDirty queue as a single RAII bundle for every BP mutator.

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
			OutError = FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidClassPath,
				FString::Printf(
					TEXT("class_path '%s' invalid — must start with '/' (e.g. '/Script/Engine.Pawn')"),
					*ClassPath));
			return nullptr;
		}
		if (ClassPath.Contains(TEXT("\\")))
		{
			OutError = FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidClassPath,
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
			OutError = FMCPToolHelpers::MakeError(Request, kMCPErrorClassNotFound,
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
			OutError = FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
				FString::Printf(TEXT("missing required object field '%s'"), FieldName));
			return false;
		}
		int32 ErrCode = 0;
		FString ErrMsg;
		if (!FMCPPinTypeUtils::FromJson(*PinTypeObjPtr, OutPinType, ErrCode, ErrMsg))
		{
			OutError = FMCPToolHelpers::MakeError(Request, ErrCode, ErrMsg);
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

	// ─── Wave F2: function-signature edit helpers ────────────────────────────────────────────────

	/**
	 * Parse the ``direction`` arg ("input"/"output") into an EEdGraphPinDirection on the OWNING
	 * terminator. KEY semantic flip: user-facing INPUT params show up as OUTPUT pins on the
	 * K2Node_FunctionEntry node (the function "takes" them out), and user-facing OUTPUT params
	 * show up as INPUT pins on the K2Node_FunctionResult. So:
	 *   direction="input"  → terminator = Entry,  pin direction on terminator = EGPD_Output
	 *   direction="output" → terminator = Result, pin direction on terminator = EGPD_Input
	 *
	 * This mirrors the convention used by ``BP_BuildSignaturePins`` above (which filters
	 * Entry.Pins[Direction==Output] for inputs and Result.Pins[Direction==Input] for outputs).
	 */
	bool BP_ParseDirectionArg(
		const FMCPRequest& Request,
		FString& OutDirectionStr,
		bool& bIsInputDir,
		FMCPResponse& OutError)
	{
		if (!Request.Args->TryGetStringField(TEXT("direction"), OutDirectionStr) || OutDirectionStr.IsEmpty())
		{
			OutError = FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
				TEXT("missing required string field 'direction' ('input' or 'output')"));
			return false;
		}
		if (OutDirectionStr.Equals(TEXT("input"), ESearchCase::IgnoreCase))
		{
			bIsInputDir = true;
			return true;
		}
		if (OutDirectionStr.Equals(TEXT("output"), ESearchCase::IgnoreCase))
		{
			bIsInputDir = false;
			return true;
		}
		OutError = FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			FString::Printf(TEXT("'direction' must be 'input' or 'output', got '%s'"), *OutDirectionStr));
		return false;
	}

	/**
	 * Build a JSON summary of one FUserPinInfo (function-signature pin authoring entry).
	 *
	 * Shape (mirrors bp.list_functions's signature.inputs/outputs entries but adds default_value):
	 *   {
	 *     "name":          "FName from FUserPinInfo::PinName",
	 *     "pin_type":      { ...MCPPinTypeUtils JSON... },
	 *     "default_value": "PinDefaultValue" | null
	 *   }
	 *
	 * Reads UserDefinedPins (the authoring array) NOT Pins[] — bp.list_function_parameters cares
	 * about USER-declared params, not the compile-time resolved pin set (which would include
	 * inherited override signature pins for interface implementations).
	 *
	 * Returns nullptr if the pin type cannot be serialised (-32032 surfaced via OutError).
	 */
	TSharedPtr<FJsonObject> BP_BuildUserDefinedPinSummary(
		const FMCPRequest& Request,
		const FUserPinInfo& Info,
		FMCPResponse& OutError)
	{
		TSharedPtr<FJsonObject> PinTypeObj = BP_PinTypeToJsonOrError(Request, Info.PinType, OutError);
		if (!PinTypeObj.IsValid())
		{
			return nullptr;
		}
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Info.PinName.ToString());
		Obj->SetObjectField(TEXT("pin_type"), PinTypeObj);
		if (Info.PinDefaultValue.IsEmpty())
		{
			Obj->SetField(TEXT("default_value"), MakeShared<FJsonValueNull>());
		}
		else
		{
			Obj->SetStringField(TEXT("default_value"), Info.PinDefaultValue);
		}
		return Obj;
	}

	/**
	 * Locate the K2Node_FunctionResult on a function graph, creating it if missing. Used by
	 * ``bp.add_function_parameter`` when a caller adds the first output to a function that
	 * previously had none.
	 *
	 * Mirrors the lazy-creation pattern in ``Tool_AddFunction`` (Days 8): we spawn the node via
	 * ``FGraphNodeCreator`` so UE wires it into the graph's Nodes[] correctly, then link the
	 * entry's Then exec pin to the new result's Execute pin so the function still has flow. The
	 * existing entry→prior-terminus link (if any) is left intact — UE's schema will reconcile via
	 * the recompile triggered downstream by MarkBlueprintAsModified.
	 */
	UK2Node_FunctionResult* BP_GetOrCreateFunctionResult(
		UEdGraph* Graph,
		UK2Node_FunctionEntry* EntryNode,
		const FName& FunctionName)
	{
		check(Graph);
		check(EntryNode);

		UK2Node_FunctionEntry* TmpEntry = nullptr;
		UK2Node_FunctionResult* ExistingResult = nullptr;
		BP_GetFunctionTerminators(Graph, TmpEntry, ExistingResult);
		if (ExistingResult)
		{
			return ExistingResult;
		}

		FGraphNodeCreator<UK2Node_FunctionResult> Creator(*Graph);
		UK2Node_FunctionResult* NewResult = Creator.CreateNode();
		NewResult->FunctionReference.SetSelfMember(FunctionName);
		Creator.Finalize();

		UEdGraphPin* EntryThen = EntryNode->FindPin(UEdGraphSchema_K2::PN_Then);
		UEdGraphPin* ResultExec = NewResult->FindPin(UEdGraphSchema_K2::PN_Execute);
		if (EntryThen && ResultExec)
		{
			EntryThen->MakeLinkTo(ResultExec);
		}
		return NewResult;
	}

	/**
	 * Resolve a function graph + its entry+result terminators in one shot, populating standard
	 * not-found / invalid-args errors via OutError. Used by every Wave F2 tool entry point so the
	 * boilerplate doesn't repeat.
	 *
	 * Returns true on success. On failure populates OutError and returns false. Caller MUST check
	 * EntryOut for null before dereferencing — it's permitted to be null only if the function graph
	 * is corrupt (which we surface as -32603 Internal); a normal user-defined function ALWAYS has
	 * an entry node (Result may be null if no outputs were declared yet — that's normal).
	 */
	bool BP_ResolveFunctionContext(
		const FMCPRequest& Request,
		FString& OutPath,
		FString& OutFnNameStr,
		UBlueprint*& OutBlueprint,
		UEdGraph*& OutGraph,
		UK2Node_FunctionEntry*& OutEntry,
		UK2Node_FunctionResult*& OutResult,
		FMCPResponse& OutError)
	{
		if (!BP_RequireBlueprintPath(Request, OutPath, OutError))
		{
			return false;
		}
		if (!Request.Args->TryGetStringField(TEXT("function_name"), OutFnNameStr) || OutFnNameStr.IsEmpty())
		{
			OutError = FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
				TEXT("missing required string field 'function_name'"));
			return false;
		}
		OutBlueprint = BP_ResolveBlueprintOrError(Request, OutPath, OutError);
		if (!OutBlueprint) { return false; }

		const FName FnName(*OutFnNameStr);
		OutGraph = FMCPBlueprintUtils::FindFunctionGraph(OutBlueprint, FnName);
		if (!OutGraph)
		{
			OutError = FMCPToolHelpers::MakeError(Request, kMCPErrorVariableNotFound,
				FString::Printf(TEXT("function '%s' not found on blueprint '%s'"),
					*OutFnNameStr, *OutPath));
			return false;
		}

		BP_GetFunctionTerminators(OutGraph, OutEntry, OutResult);
		if (!OutEntry)
		{
			OutError = FMCPToolHelpers::MakeError(Request, kBPErrorInternal,
				FString::Printf(
					TEXT("function '%s' on '%s' has no UK2Node_FunctionEntry (corrupt blueprint?)"),
					*OutFnNameStr, *OutPath));
			return false;
		}
		return true;
	}

	/** Build a JSON snapshot of the current function metadata (used by set_function_metadata's prior/new). */
	TSharedPtr<FJsonObject> BP_BuildFunctionMetadataSnapshot(
		const UK2Node_FunctionEntry* Entry,
		const UEdGraph* Graph)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (!Entry || !Graph)
		{
			return Obj;
		}
		const int32 Flags = Entry->GetFunctionFlags();
		Obj->SetBoolField(TEXT("is_pure"),   (Flags & FUNC_BlueprintPure) != 0);
		Obj->SetBoolField(TEXT("is_const"),  (Flags & FUNC_Const) != 0);
		Obj->SetStringField(TEXT("access_specifier"), BP_AccessSpecifierFromFlags(Flags).ToLower());

		// Category lives on the graph-level metadata (FKismetUserDeclaredFunctionMetadata), not on
		// the entry-node MetaData directly — the two are kept in sync but the graph-level wrapper
		// is the canonical authoring surface.
		FString Category;
		if (FKismetUserDeclaredFunctionMetadata* GraphMeta =
			FBlueprintEditorUtils::GetGraphFunctionMetaData(Graph))
		{
			Category = GraphMeta->Category.ToString();
			Obj->SetStringField(TEXT("tooltip"), GraphMeta->ToolTip.ToString());
			Obj->SetBoolField(TEXT("call_in_editor"), GraphMeta->bCallInEditor);
		}
		else
		{
			// Fallback to entry node's local copy (still readable even if the graph wrapper is null)
			Obj->SetStringField(TEXT("tooltip"), Entry->MetaData.ToolTip.ToString());
			Obj->SetBoolField(TEXT("call_in_editor"), Entry->MetaData.bCallInEditor);
		}
		Obj->SetStringField(TEXT("category"), Category);
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
			return FMCPToolHelpers::MakeError(Request, ErrCode, ErrMsg);
		}
		if (ErrCode == kMCPErrorBlueprintTypeMismatch)
		{
			return FMCPToolHelpers::MakeError(Request, ErrCode, ErrMsg);
		}
		// -32004 ObjectNotFound → exists=false success response.
		Out->SetBoolField(TEXT("exists"), false);
		Out->SetField(TEXT("generated_class_path"), MakeShared<FJsonValueNull>());
		Out->SetField(TEXT("parent_class_path"), MakeShared<FJsonValueNull>());
		Out->SetBoolField(TEXT("is_data_only"), false);
		return FMCPToolHelpers::MakeSuccessObj(Request, Out);
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
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
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
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			TEXT("missing required string field 'variable_name'"));
	}

	FMCPResponse ResolveErr;
	UBlueprint* Blueprint = BP_ResolveBlueprintOrError(Request, Path, ResolveErr);
	if (!Blueprint) { return ResolveErr; }

	const FName VarName(*VarNameStr);
	const int32 Idx = FMCPBlueprintUtils::FindVariableIndex(Blueprint, VarName);
	if (Idx == INDEX_NONE)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorVariableNotFound,
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
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
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
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
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
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
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
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			TEXT("missing required string field 'function_name'"));
	}

	FMCPResponse ResolveErr;
	UBlueprint* Blueprint = BP_ResolveBlueprintOrError(Request, Path, ResolveErr);
	if (!Blueprint) { return ResolveErr; }

	const FName FnName(*FnNameStr);
	UEdGraph* Graph = FMCPBlueprintUtils::FindFunctionGraph(Blueprint, FnName);
	if (!Graph)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorVariableNotFound,
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
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
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
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			TEXT("missing required string field 'function_name'"));
	}

	FMCPResponse ResolveErr;
	UBlueprint* Blueprint = BP_ResolveBlueprintOrError(Request, Path, ResolveErr);
	if (!Blueprint) { return ResolveErr; }

	const FName FnName(*FnNameStr);
	UEdGraph* Graph = FMCPBlueprintUtils::FindFunctionGraph(Blueprint, FnName);
	if (!Graph)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorVariableNotFound,
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
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
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

	FMCPMutatorScope Scope(Request, LOCTEXT("BPAddVariable", "MCP: add blueprint variable"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString Path;
	FMCPResponse PathErr;
	if (!BP_RequireBlueprintPath(Request, Path, PathErr)) { return PathErr; }

	FString VarNameStr;
	if (!Request.Args->TryGetStringField(TEXT("variable_name"), VarNameStr) || VarNameStr.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
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
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPathInUse,
			FString::Printf(
				TEXT("variable '%s' already exists on blueprint '%s'; use bp.remove_variable + bp.add_variable to replace"),
				*VarNameStr, *Path));
	}

	const TSharedPtr<FJsonValue> DefaultValueField = Request.Args->TryGetField(TEXT("default_value"));
	const FString DefaultValueText = BP_ConvertJsonDefaultToText(DefaultValueField);

	// Scope owns the undo group; AddMemberVariable internally calls Modify+Mark.
	const bool bAdded = FBlueprintEditorUtils::AddMemberVariable(
		Blueprint, VarName, NewPinType, DefaultValueText);
	if (!bAdded)
	{
		// Fall-through: UE rejected the add for an internal reason we didn't pre-screen. Most
		// common is a colliding parent-class variable (we only checked the BP's own NewVariables).
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPathInUse,
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
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
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

	FMCPMutatorScope Scope(Request, LOCTEXT("BPRemoveVariable", "MCP: remove blueprint variable"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString Path;
	FMCPResponse PathErr;
	if (!BP_RequireBlueprintPath(Request, Path, PathErr)) { return PathErr; }

	FString VarNameStr;
	if (!Request.Args->TryGetStringField(TEXT("variable_name"), VarNameStr) || VarNameStr.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
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
		Scope.Abort();
		return FMCPToolHelpers::MakeSuccessObj(Request, Out);
	}

	FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, VarName);

	Out->SetBoolField(TEXT("removed"), true);
	Out->SetBoolField(TEXT("was_present"), true);
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
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

	FMCPMutatorScope Scope(Request, LOCTEXT("BPChangeVariableType", "MCP: change blueprint variable type"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString Path;
	FMCPResponse PathErr;
	if (!BP_RequireBlueprintPath(Request, Path, PathErr)) { return PathErr; }

	FString VarNameStr;
	if (!Request.Args->TryGetStringField(TEXT("variable_name"), VarNameStr) || VarNameStr.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
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
		return FMCPToolHelpers::MakeError(Request, kMCPErrorVariableNotFound,
			FString::Printf(TEXT("variable '%s' not found on blueprint '%s'"),
				*VarNameStr, *Path));
	}

	// Snapshot prior pin type BEFORE the mutation so we can echo it. ChangeMemberVariableType may
	// reset the FBPVariableDescription's VarType in place.
	FMCPResponse PriorErr;
	TSharedPtr<FJsonObject> PriorPinTypeObj = BP_PinTypeToJsonOrError(
		Request, Blueprint->NewVariables[Idx].VarType, PriorErr);
	if (!PriorPinTypeObj.IsValid()) { return PriorErr; }

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
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
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

	FMCPMutatorScope Scope(Request, LOCTEXT("BPAddFunction", "MCP: add blueprint function"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString Path;
	FMCPResponse PathErr;
	if (!BP_RequireBlueprintPath(Request, Path, PathErr)) { return PathErr; }

	FString FnNameStr;
	if (!Request.Args->TryGetStringField(TEXT("function_name"), FnNameStr) || FnNameStr.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			TEXT("missing required string field 'function_name'"));
	}

	FMCPResponse ResolveErr;
	UBlueprint* Blueprint = BP_ResolveBlueprintOrError(Request, Path, ResolveErr);
	if (!Blueprint) { return ResolveErr; }

	const FName FnName(*FnNameStr);
	if (FMCPBlueprintUtils::FindFunctionGraphIndex(Blueprint, FnName) != INDEX_NONE)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPathInUse,
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
				OutError = FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
					FString::Printf(TEXT("%s[%d] is not an object"), FieldName, i));
				return false;
			}
			const TSharedPtr<FJsonObject>& Item = V->AsObject();
			FString PinNameStr;
			if (!Item->TryGetStringField(TEXT("name"), PinNameStr) || PinNameStr.IsEmpty())
			{
				OutError = FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
					FString::Printf(TEXT("%s[%d] missing 'name'"), FieldName, i));
				return false;
			}
			const TSharedPtr<FJsonObject>* PinTypeObjPtr = nullptr;
			if (!Item->TryGetObjectField(TEXT("pin_type"), PinTypeObjPtr) || !PinTypeObjPtr || !PinTypeObjPtr->IsValid())
			{
				OutError = FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
					FString::Printf(TEXT("%s[%d] missing 'pin_type' object"), FieldName, i));
				return false;
			}
			FParsedSignaturePin Pin;
			Pin.Name = FName(*PinNameStr);
			int32 ErrCode = 0;
			FString ErrMsg;
			if (!FMCPPinTypeUtils::FromJson(*PinTypeObjPtr, Pin.Type, ErrCode, ErrMsg))
			{
				OutError = FMCPToolHelpers::MakeError(Request, ErrCode,
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

	// Create the empty function graph. UEdGraphSchema_K2 is the schema for K2 (Blueprint) graphs.
	UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
		Blueprint, FnName, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	if (!NewGraph)
	{
		return FMCPToolHelpers::MakeError(Request, kBPErrorInternal,
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
		return FMCPToolHelpers::MakeError(Request, kBPErrorInternal,
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
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
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

	FMCPMutatorScope Scope(Request, LOCTEXT("BPRemoveFunction", "MCP: remove blueprint function"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString Path;
	FMCPResponse PathErr;
	if (!BP_RequireBlueprintPath(Request, Path, PathErr)) { return PathErr; }

	FString FnNameStr;
	if (!Request.Args->TryGetStringField(TEXT("function_name"), FnNameStr) || FnNameStr.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
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
		Scope.Abort();
		return FMCPToolHelpers::MakeSuccessObj(Request, Out);
	}

	FBlueprintEditorUtils::RemoveGraph(Blueprint, Graph, EGraphRemoveFlags::Recompile);

	Out->SetBoolField(TEXT("removed"), true);
	Out->SetBoolField(TEXT("was_present"), true);
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
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

	FMCPMutatorScope Scope(Request, LOCTEXT("BPReparent", "MCP: reparent blueprint"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString Path;
	FMCPResponse PathErr;
	if (!BP_RequireBlueprintPath(Request, Path, PathErr)) { return PathErr; }

	FString NewParentPath;
	if (!Request.Args->TryGetStringField(TEXT("new_parent_class_path"), NewParentPath) || NewParentPath.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			TEXT("missing required string field 'new_parent_class_path'"));
	}

	// Confirm gate FIRST — even before resolving the BP, so the surface error is consistent
	// regardless of what else is wrong. Pass requires LITERAL bool true (truthy != true).
	bool bConfirmDangerous = false;
	const bool bHasConfirm = Request.Args->TryGetBoolField(TEXT("confirm_dangerous"), bConfirmDangerous);
	if (!bHasConfirm || !bConfirmDangerous)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorReparentUnsafe,
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
		return FMCPToolHelpers::MakeError(Request, kMCPErrorWrongClass,
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
		Scope.Abort();
		return FMCPToolHelpers::MakeSuccessObj(Request, NoOpOut);
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
	// compile pass. Scope (declared at function entry) owns the transaction.
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
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
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

	FMCPMutatorScope Scope(Request, LOCTEXT("BPCompile", "MCP: compile blueprint"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

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
		FMCPResponse Err = FMCPToolHelpers::MakeError(Request, kMCPErrorKismetCompilationError,
			FString::Printf(
				TEXT("blueprint '%s' failed strict compile: %d errors, %d warnings (status=%s)"),
				*Path, Errors.Num(), Warnings.Num(), StatusStr));
		// Embed the same diagnostic payload as the success path so strict callers don't have to
		// re-run with fail_on_error=false to get details.
		Err.Result = MakeShared<FJsonValueObject>(ResultObj);
		return Err;
	}

	return FMCPToolHelpers::MakeSuccessObj(Request, ResultObj);
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

	FMCPMutatorScope Scope(Request, LOCTEXT("BPCreateBlueprint", "MCP: create blueprint"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			TEXT("bp.create_blueprint requires args.parent_class_path + args.dest_path"));
	}

	FString ParentClassPath, DestPathRaw;
	if (!Request.Args->TryGetStringField(TEXT("parent_class_path"), ParentClassPath) || ParentClassPath.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			TEXT("missing required string field 'parent_class_path'"));
	}
	if (!Request.Args->TryGetStringField(TEXT("dest_path"), DestPathRaw) || DestPathRaw.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
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
		return FMCPToolHelpers::MakeError(Request, kMCPErrorClassNotFound,
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
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidPath,
			FString::Printf(TEXT("dest_path '%s' is not a valid mount-prefixed path"), *DestPathRaw));
	}
	if (FPackageName::DoesPackageExist(DestPathNorm))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPathInUse,
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
		return FMCPToolHelpers::MakeError(Request, kBPErrorInternal,
			FString::Printf(TEXT("UBlueprintFactory failed to create BP from parent '%s' at '%s'"),
				*ParentClass->GetPathName(), *DestPathNorm));
	}

	UBlueprint* NewBP = Cast<UBlueprint>(NewAsset);
	if (NewBP)
	{
		Scope.DirtyPackage(NewBP->GetOutermost());
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

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("created"), true);
	Out->SetStringField(TEXT("asset_path"), NewAsset->GetPathName());
	Out->SetStringField(TEXT("generated_class"),
		NewBP && NewBP->GeneratedClass ? NewBP->GeneratedClass->GetPathName() : FString());
	Out->SetStringField(TEXT("parent_class"), ParentClass->GetPathName());
	Out->SetBoolField(TEXT("saved"), bSavedOk);
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── bp.add_function_parameter (Lane A, PIE-guarded) ─────────────────────────────────────────
//
// Appends a new UserDefinedPin to either the K2Node_FunctionEntry (direction="input") or the
// K2Node_FunctionResult (direction="output") of an existing function graph. Auto-creates the
// result node + entry→result exec wire when adding the first output to a function that had
// none. Pin type goes through the standard MCPPinTypeUtils round-trip. The pin is appended via
// ``CreateUserDefinedPin(bUseUniqueName=false)`` and we pre-check for name collision so the
// caller gets -32057 instead of UE silently appending a suffix.
//
// Args:    { blueprint_path: string, function_name: string, param_name: string,
//            pin_type: { ...MCPPinTypeUtils JSON... },
//            direction: "input"|"output", default_value?: string }
// Result:  { added: bool, param_name: string, direction: string }
//
// Errors:
//   -32027 PIEActive
//   -32010 / -32004 / -32031 (resolve)
//   -32602 InvalidParams         — missing function_name / param_name / pin_type / direction;
//                                  direction not "input"|"output"
//   -32037 VariableNotFound      — function graph not present
//   -32032 PinTypeUnsupported    — bad pin_type JSON
//   -32057 FunctionParameterDuplicate — param_name already exists on the terminator
//   -32603 Internal              — function graph corrupt (no entry terminator)
FMCPResponse Tool_AddFunctionParameter(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("BPAddFunctionParameter", "MCP: add function parameter"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString Path, FnNameStr;
	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	UK2Node_FunctionEntry* Entry = nullptr;
	UK2Node_FunctionResult* Result = nullptr;
	FMCPResponse CtxErr;
	if (!BP_ResolveFunctionContext(Request, Path, FnNameStr, Blueprint, Graph, Entry, Result, CtxErr))
	{
		return CtxErr;
	}

	FString ParamNameStr;
	if (!Request.Args->TryGetStringField(TEXT("param_name"), ParamNameStr) || ParamNameStr.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			TEXT("missing required string field 'param_name'"));
	}

	FString DirectionStr;
	bool bIsInputDir = false;
	FMCPResponse DirErr;
	if (!BP_ParseDirectionArg(Request, DirectionStr, bIsInputDir, DirErr)) { return DirErr; }

	FEdGraphPinType PinType;
	FMCPResponse PinTypeErr;
	if (!BP_RequirePinTypeArg(Request, TEXT("pin_type"), PinType, PinTypeErr))
	{
		return PinTypeErr;
	}

	FString DefaultValue;
	Request.Args->TryGetStringField(TEXT("default_value"), DefaultValue);

	const FName ParamName(*ParamNameStr);

	// Resolve the owning terminator (Entry for inputs, Result for outputs). For outputs we lazily
	// create the result node if it doesn't yet exist on the function graph.
	UK2Node_EditablePinBase* Owner = nullptr;
	if (bIsInputDir)
	{
		Owner = Entry;
	}
	else
	{
		if (!Result)
		{
			Result = BP_GetOrCreateFunctionResult(Graph, Entry, FName(*FnNameStr));
		}
		Owner = Result;
	}
	if (!Owner)
	{
		return FMCPToolHelpers::MakeError(Request, kBPErrorInternal,
			TEXT("could not resolve or create function terminator for parameter add"));
	}

	// Pre-check duplicate: ``CreateUserDefinedPin(bUseUniqueName=false)`` would still append because
	// the underlying ``CreateUserDefinedPin`` only rejects via existing-name-check when bUseUniqueName
	// is TRUE. We want explicit -32057 rather than silent dedup OR silent overwrite.
	// UserDefinedPinExists is not exported (UE5.7 BLUEPRINTGRAPH_API omission); inline the scan
	// over UserDefinedPins[] — the same TArray that the engine helper iterates internally.
	auto UserDefinedPinExistsInline = [](const UK2Node_EditablePinBase* Node, const FName Name) -> bool
	{
		for (const TSharedPtr<FUserPinInfo>& Info : Node->UserDefinedPins)
		{
			if (Info.IsValid() && Info->PinName == Name) { return true; }
		}
		return false;
	};

	if (UserDefinedPinExistsInline(Owner, ParamName))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorFunctionParameterDuplicate,
			FString::Printf(
				TEXT("parameter '%s' already exists on %s of function '%s' on blueprint '%s'"),
				*ParamNameStr,
				bIsInputDir ? TEXT("K2Node_FunctionEntry (inputs)") : TEXT("K2Node_FunctionResult (outputs)"),
				*FnNameStr, *Path));
	}

	Owner->Modify();
	const EEdGraphPinDirection TerminatorPinDir = bIsInputDir ? EGPD_Output : EGPD_Input;
	UEdGraphPin* CreatedPin = Owner->CreateUserDefinedPin(ParamName, PinType, TerminatorPinDir, /*bUseUniqueName*/ false);
	if (!CreatedPin)
	{
		return FMCPToolHelpers::MakeError(Request, kBPErrorInternal,
			FString::Printf(
				TEXT("CreateUserDefinedPin returned null for '%s' on function '%s' "
					 "(terminator rejected the pin type via CanCreateUserDefinedPin)"),
				*ParamNameStr, *FnNameStr));
	}

	// Apply optional default value. ``ModifyUserDefinedPinDefaultValue`` updates BOTH the
	// UserDefinedPins entry and the live UEdGraphPin's default — the right authoring surface.
	if (!DefaultValue.IsEmpty())
	{
		for (TSharedPtr<FUserPinInfo>& Info : Owner->UserDefinedPins)
		{
			if (Info.IsValid() && Info->PinName == ParamName)
			{
				Owner->ModifyUserDefinedPinDefaultValue(Info, DefaultValue);
				break;
			}
		}
	}

	// Reconstruct rebuilds the live Pins[] array to match UserDefinedPins (UE convention after
	// authoring mutation). Without it the new pin may not appear on connected K2 callers until the
	// next BP recompile. ReconstructNode preserves existing pin links via RewireOldPinsToNewPins.
	Owner->ReconstructNode();
	// Signature change → StructurallyModified triggers RegenerateSkeletonOnly compile so the
	// UFunction shape stays in sync. Mirrors what Tool_AddFunction does at end-of-creation.
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("added"), true);
	Out->SetStringField(TEXT("param_name"), ParamNameStr);
	Out->SetStringField(TEXT("direction"), bIsInputDir ? TEXT("input") : TEXT("output"));
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── bp.remove_function_parameter (Lane A, PIE-guarded, idempotent on terminator side) ───────
//
// Removes a UserDefinedPin from whichever terminator (Entry or Result) owns it. Direction is
// auto-detected: we scan Entry's UserDefinedPins first then Result's. Returns
// ``{removed: false, direction: null}`` if the param doesn't exist on either terminator (caller
// can treat as idempotent / already-removed).
//
// Args:    { blueprint_path: string, function_name: string, param_name: string }
// Result:  { removed: bool, direction: "input"|"output"|null }
//
// Errors:
//   -32027 PIEActive
//   -32010 / -32004 / -32031 (resolve)
//   -32602 InvalidParams         — missing function_name / param_name
//   -32037 VariableNotFound      — function graph not present
//   -32603 Internal              — function graph corrupt (no entry terminator)
FMCPResponse Tool_RemoveFunctionParameter(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("BPRemoveFunctionParameter", "MCP: remove function parameter"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString Path, FnNameStr;
	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	UK2Node_FunctionEntry* Entry = nullptr;
	UK2Node_FunctionResult* Result = nullptr;
	FMCPResponse CtxErr;
	if (!BP_ResolveFunctionContext(Request, Path, FnNameStr, Blueprint, Graph, Entry, Result, CtxErr))
	{
		return CtxErr;
	}

	FString ParamNameStr;
	if (!Request.Args->TryGetStringField(TEXT("param_name"), ParamNameStr) || ParamNameStr.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			TEXT("missing required string field 'param_name'"));
	}

	const FName ParamName(*ParamNameStr);
	// UserDefinedPinExists is not exported (UE 5.7 BLUEPRINTGRAPH_API omission); inline the scan
	// over UserDefinedPins[] — the same TArray that the engine helper iterates internally.
	auto UserDefinedPinExistsInline = [](const UK2Node_EditablePinBase* Node, const FName Name) -> bool
	{
		if (!Node) { return false; }
		for (const TSharedPtr<FUserPinInfo>& Info : Node->UserDefinedPins)
		{
			if (Info.IsValid() && Info->PinName == Name) { return true; }
		}
		return false;
	};

	UK2Node_EditablePinBase* Owner = nullptr;
	bool bWasInputDir = false;
	if (UserDefinedPinExistsInline(Entry, ParamName))
	{
		Owner = Entry;
		bWasInputDir = true;
	}
	else if (UserDefinedPinExistsInline(Result, ParamName))
	{
		Owner = Result;
		bWasInputDir = false;
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	if (!Owner)
	{
		Out->SetBoolField(TEXT("removed"), false);
		Out->SetField(TEXT("direction"), MakeShared<FJsonValueNull>());
		Scope.Abort();
		return FMCPToolHelpers::MakeSuccessObj(Request, Out);
	}

	Owner->Modify();
	Owner->RemoveUserDefinedPinByName(ParamName);
	Owner->ReconstructNode();
	// Signature change → StructurallyModified triggers RegenerateSkeletonOnly so the UFunction
	// shape stays in sync. Same rationale as Tool_AddFunctionParameter.
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	Out->SetBoolField(TEXT("removed"), true);
	Out->SetStringField(TEXT("direction"), bWasInputDir ? TEXT("input") : TEXT("output"));
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── bp.list_function_parameters (Lane A, NO PIE guard — read) ───────────────────────────────
//
// Returns the function's user-declared parameter signature, split by direction.
// Reads UserDefinedPins on the Entry (inputs) and Result (outputs) — NOT the resolved Pins[]
// array. UserDefinedPins is the canonical AUTHORING surface (what the user typed in the BP
// editor) and preserves PinDefaultValue strings for round-trip with bp.add_function_parameter.
//
// Args:    { blueprint_path: string, function_name: string }
// Result:  { inputs: [{name, pin_type, default_value}], outputs: [{name, pin_type, default_value}] }
//
// Errors:
//   -32010 / -32004 / -32031 (resolve)
//   -32602 InvalidParams         — missing function_name
//   -32037 VariableNotFound      — function graph not present
//   -32032 PinTypeUnsupported    — any param's stored pin type uses an unsupported PC_* category
//   -32603 Internal              — function graph corrupt (no entry terminator)
FMCPResponse Tool_ListFunctionParameters(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString Path, FnNameStr;
	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	UK2Node_FunctionEntry* Entry = nullptr;
	UK2Node_FunctionResult* Result = nullptr;
	FMCPResponse CtxErr;
	if (!BP_ResolveFunctionContext(Request, Path, FnNameStr, Blueprint, Graph, Entry, Result, CtxErr))
	{
		return CtxErr;
	}

	auto BuildArray = [&Request](const UK2Node_EditablePinBase* Owner, FMCPResponse& OutError)
		-> TSharedPtr<FJsonValue>
	{
		TArray<TSharedPtr<FJsonValue>> Items;
		if (!Owner) { return MakeShared<FJsonValueArray>(Items); }
		Items.Reserve(Owner->UserDefinedPins.Num());
		for (const TSharedPtr<FUserPinInfo>& Info : Owner->UserDefinedPins)
		{
			if (!Info.IsValid()) { continue; }
			TSharedPtr<FJsonObject> ParamObj = BP_BuildUserDefinedPinSummary(Request, *Info, OutError);
			if (!ParamObj.IsValid()) { return nullptr; }
			Items.Add(MakeShared<FJsonValueObject>(ParamObj));
		}
		return MakeShared<FJsonValueArray>(Items);
	};

	FMCPResponse PinErr;
	TSharedPtr<FJsonValue> InputsArr = BuildArray(Entry, PinErr);
	if (!InputsArr.IsValid()) { return PinErr; }
	TSharedPtr<FJsonValue> OutputsArr = BuildArray(Result, PinErr);
	if (!OutputsArr.IsValid()) { return PinErr; }

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetField(TEXT("inputs"), InputsArr);
	Out->SetField(TEXT("outputs"), OutputsArr);
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── bp.set_function_metadata (Lane A, PIE-guarded) ──────────────────────────────────────────
//
// Mutates function-level metadata on the K2Node_FunctionEntry + the function graph's
// FKismetUserDeclaredFunctionMetadata wrapper. Every field is OPTIONAL — only the keys present
// in args.metadata are written. Returns a prior/new snapshot pair so the caller can revert if
// needed.
//
// Field mapping (UE 5.7):
//   is_pure           → Entry->ExtraFlags |= FUNC_BlueprintPure       (or cleared)
//   is_const          → Entry->ExtraFlags |= FUNC_Const               (or cleared)
//   category          → graph-level FKismetUserDeclaredFunctionMetadata::Category (FText)
//   access_specifier  → Entry->ExtraFlags FUNC_AccessSpecifiers bits (Public/Protected/Private)
//   call_in_editor    → graph-level Meta->bCallInEditor (also synced to Entry->MetaData)
//   tooltip           → graph-level Meta->ToolTip (also synced to Entry->MetaData)
//
// Note: access_specifier defaults to "public" for K2 user functions (FUNC_Public set in
// FBlueprintEditorUtils::AddNewFunctionGraph). Writing it via ExtraFlags + ReconstructNode
// matches what the BP editor's "Function Details" panel does internally.
//
// Args:    { blueprint_path: string, function_name: string,
//            metadata: { is_pure?: bool, is_const?: bool, category?: string,
//                        access_specifier?: "public"|"protected"|"private",
//                        call_in_editor?: bool, tooltip?: string } }
// Result:  { prior: { is_pure, is_const, access_specifier, category, call_in_editor, tooltip },
//            new:   { is_pure, is_const, access_specifier, category, call_in_editor, tooltip } }
//
// Errors:
//   -32027 PIEActive
//   -32010 / -32004 / -32031 (resolve)
//   -32602 InvalidParams         — missing function_name OR missing 'metadata' object OR
//                                  unknown access_specifier value
//   -32037 VariableNotFound      — function graph not present
//   -32603 Internal              — function graph corrupt (no entry terminator)
FMCPResponse Tool_SetFunctionMetadata(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("BPSetFunctionMetadata", "MCP: set function metadata"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString Path, FnNameStr;
	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	UK2Node_FunctionEntry* Entry = nullptr;
	UK2Node_FunctionResult* Result = nullptr;
	FMCPResponse CtxErr;
	if (!BP_ResolveFunctionContext(Request, Path, FnNameStr, Blueprint, Graph, Entry, Result, CtxErr))
	{
		return CtxErr;
	}

	const TSharedPtr<FJsonObject>* MetadataObjPtr = nullptr;
	if (!Request.Args->TryGetObjectField(TEXT("metadata"), MetadataObjPtr) || !MetadataObjPtr || !MetadataObjPtr->IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			TEXT("missing required object field 'metadata'"));
	}
	const TSharedPtr<FJsonObject>& Metadata = *MetadataObjPtr;

	// Validate access_specifier up-front so we don't half-apply metadata before failing.
	EFunctionFlags NewAccessSpec = FUNC_None;
	bool bHasAccessSpec = false;
	{
		FString AccessStr;
		if (Metadata->TryGetStringField(TEXT("access_specifier"), AccessStr))
		{
			if (AccessStr.Equals(TEXT("public"), ESearchCase::IgnoreCase))
			{
				NewAccessSpec = FUNC_Public;
			}
			else if (AccessStr.Equals(TEXT("protected"), ESearchCase::IgnoreCase))
			{
				NewAccessSpec = FUNC_Protected;
			}
			else if (AccessStr.Equals(TEXT("private"), ESearchCase::IgnoreCase))
			{
				NewAccessSpec = FUNC_Private;
			}
			else
			{
				return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
					FString::Printf(
						TEXT("metadata.access_specifier must be 'public'|'protected'|'private', got '%s'"),
						*AccessStr));
			}
			bHasAccessSpec = true;
		}
	}

	// Snapshot BEFORE we mutate.
	TSharedPtr<FJsonObject> PriorSnap = BP_BuildFunctionMetadataSnapshot(Entry, Graph);

	Entry->Modify();

	// is_pure / is_const — written to Entry->ExtraFlags. SetExtraFlags strips FUNC_Native; we
	// preserve everything else with explicit set/clear.
	bool bIsPure = false;
	if (Metadata->TryGetBoolField(TEXT("is_pure"), bIsPure))
	{
		if (bIsPure) { Entry->AddExtraFlags(FUNC_BlueprintPure); }
		else          { Entry->ClearExtraFlags(FUNC_BlueprintPure); }
	}
	bool bIsConst = false;
	if (Metadata->TryGetBoolField(TEXT("is_const"), bIsConst))
	{
		if (bIsConst) { Entry->AddExtraFlags(FUNC_Const); }
		else           { Entry->ClearExtraFlags(FUNC_Const); }
	}

	// access_specifier — only mutates the access-specifier bits; clear all three then set the
	// chosen one. Matches what the BP editor does when the user picks from the dropdown.
	if (bHasAccessSpec)
	{
		Entry->ClearExtraFlags(FUNC_AccessSpecifiers);
		Entry->AddExtraFlags(NewAccessSpec);
	}

	// Graph-level metadata for category / tooltip / call_in_editor. Sync to Entry->MetaData as
	// well so reads via the entry node match (BP editor keeps both in sync).
	FKismetUserDeclaredFunctionMetadata* GraphMeta =
		FBlueprintEditorUtils::GetGraphFunctionMetaData(Graph);

	FString CategoryStr;
	if (Metadata->TryGetStringField(TEXT("category"), CategoryStr))
	{
		const FText CategoryText = FText::FromString(CategoryStr);
		if (GraphMeta) { GraphMeta->Category = CategoryText; }
		Entry->MetaData.Category = CategoryText;
	}

	FString TooltipStr;
	if (Metadata->TryGetStringField(TEXT("tooltip"), TooltipStr))
	{
		const FText TooltipText = FText::FromString(TooltipStr);
		if (GraphMeta) { GraphMeta->ToolTip = TooltipText; }
		Entry->MetaData.ToolTip = TooltipText;
	}

	bool bCallInEditor = false;
	if (Metadata->TryGetBoolField(TEXT("call_in_editor"), bCallInEditor))
	{
		if (GraphMeta) { GraphMeta->bCallInEditor = bCallInEditor; }
		Entry->MetaData.bCallInEditor = bCallInEditor;
	}

	// Reconstruct rebuilds the entry node's pin set under the new flag combo (e.g. pure functions
	// lose their Exec pins — the BP editor relies on this). StructurallyModified retriggers a
	// skeleton compile so the UFunction's flag bits stay in sync.
	Entry->ReconstructNode();
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	TSharedPtr<FJsonObject> NewSnap = BP_BuildFunctionMetadataSnapshot(Entry, Graph);

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetObjectField(TEXT("prior"), PriorSnap);
	Out->SetObjectField(TEXT("new"), NewSnap);
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── Wave F Surface 4 — Blueprint interface implementation surface (3 tools) ─────────────────
//
// These tools wrap FBlueprintEditorUtils's interface APIs:
//   add    → ImplementNewInterface(UBlueprint*, FTopLevelAssetPath)  (UE 5.7 signature; the
//            FName overload is deprecated since 5.1)
//   remove → RemoveInterface(UBlueprint*, FTopLevelAssetPath, bPreserveFunctions=false)
//            We pass bPreserveFunctions=false: the brief says "remove" — promoting interface
//            graphs to standalone functions is a separate user intent. If callers ever need
//            preserve-promote semantics, a follow-up surface can expose it.
//
// "Generated event count" on add: ImplementNewInterface populates FBPInterfaceDescription.Graphs
// with one UEdGraph per interface UFUNCTION that has a non-void return signature — these become
// the auto-generated function override stubs. Pure events (void return, no outputs) are wired
// via the EventGraph as CustomEvent nodes and don't appear in Graphs[]. We report the Graphs[]
// count as a usable proxy for "what UE materialised for you"; a fully accurate event-vs-function
// breakdown would require walking the interface UClass's UFUNCTIONs manually, which we skip per
// brief scope.

namespace
{
	/**
	 * Resolve an interface UClass from the ``interface_class_path`` arg.
	 *
	 * Steps:
	 *   1. Read the string arg → -32602 InvalidParams on missing/empty.
	 *   2. BP_ResolveClassOrError handles path-syntax + LoadObject failure ( -32023 / -32020 ).
	 *   3. Confirm the resolved UClass has CLASS_Interface → -32011 WrongClass otherwise.
	 *
	 * Returns nullptr + populates OutError on any failure.
	 */
	UClass* BP_ResolveInterfaceClassOrError(
		const FMCPRequest& Request,
		FString& OutInterfaceClassPath,
		FMCPResponse& OutError)
	{
		if (!Request.Args.IsValid())
		{
			OutError = FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams, TEXT("missing args object"));
			return nullptr;
		}
		if (!Request.Args->TryGetStringField(TEXT("interface_class_path"), OutInterfaceClassPath)
			|| OutInterfaceClassPath.IsEmpty())
		{
			OutError = FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
				TEXT("missing required string field 'interface_class_path'"));
			return nullptr;
		}

		UClass* InterfaceClass = BP_ResolveClassOrError(Request, OutInterfaceClassPath, OutError);
		if (!InterfaceClass) { return nullptr; }

		if (!InterfaceClass->HasAnyClassFlags(CLASS_Interface))
		{
			OutError = FMCPToolHelpers::MakeError(Request, kMCPErrorWrongClass,
				FString::Printf(
					TEXT("interface_class_path '%s' resolves to UClass '%s' but it is not a UInterface "
						 "(CLASS_Interface flag absent); pass a path to an interface class such as "
						 "'/Script/Engine.ActorSoundParameterInterface' or a /Game/.../UIM_*_C BP interface"),
					*OutInterfaceClassPath, *InterfaceClass->GetName()));
			return nullptr;
		}
		return InterfaceClass;
	}
}

// ─── bp.add_interface (Lane A, PIE-guarded) ──────────────────────────────────────────────────
//
// Implements a UInterface on the blueprint via FBlueprintEditorUtils::ImplementNewInterface.
// UE 5.7 signature takes an FTopLevelAssetPath (FName overload is deprecated since 5.1).
//
// Behaviour:
//   - Pre-check duplicate via Blueprint->ImplementedInterfaces scan → -32014 PathInUse rather
//     than silent no-op (matches the explicit-rejection precedent of bp.add_function_parameter).
//   - FScopedTransaction so the editor's Undo/Redo can reverse the implement.
//   - MarkBlueprintAsStructurallyModified retriggers skeleton compile so the new interface UClass
//     surfaces on the generated UFunction list.
//   - Report Graphs.Num() from the new FBPInterfaceDescription as ``generated_event_count``
//     (proxy for "how many auto-generated function override stubs UE materialised" — see surface
//     comment above for accuracy caveat).
//
// Args:    { blueprint_path: string, interface_class_path: string }
// Result:  { added: bool, interface_class: "/Script/.../IFoo", generated_event_count: int }
//
// Errors:
//   -32027 PIEActive
//   -32010 / -32004 / -32031 (blueprint resolve)
//   -32602 InvalidParams           — missing interface_class_path
//   -32023 InvalidClassPath        — interface path malformed
//   -32020 ClassNotFound           — interface path doesn't load
//   -32011 WrongClass              — resolved class is not a UInterface
//   -32014 PathInUse               — interface already implemented
//   -32603 Internal                — ImplementNewInterface returned false
FMCPResponse Tool_AddInterface(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("BPAddInterface", "MCP: add blueprint interface"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString Path;
	FMCPResponse PathErr;
	if (!BP_RequireBlueprintPath(Request, Path, PathErr)) { return PathErr; }

	UBlueprint* Blueprint = BP_ResolveBlueprintOrError(Request, Path, PathErr);
	if (!Blueprint) { return PathErr; }

	FString InterfaceClassPath;
	FMCPResponse InterfaceErr;
	UClass* InterfaceClass = BP_ResolveInterfaceClassOrError(Request, InterfaceClassPath, InterfaceErr);
	if (!InterfaceClass) { return InterfaceErr; }

	// Duplicate check: refuse rather than silently no-op. ImplementNewInterface itself returns
	// false on duplicate but we want the explicit -32014 so AI callers can decide between
	// "already done — proceed" or "log + skip".
	for (const FBPInterfaceDescription& Desc : Blueprint->ImplementedInterfaces)
	{
		if (Desc.Interface == InterfaceClass)
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorPathInUse,
				FString::Printf(
					TEXT("interface '%s' is already implemented on blueprint '%s'"),
					*InterfaceClassPath, *Path));
		}
	}

	const FTopLevelAssetPath InterfacePath = InterfaceClass->GetClassPathName();
	const bool bImplemented = FBlueprintEditorUtils::ImplementNewInterface(Blueprint, InterfacePath);
	if (!bImplemented)
	{
		return FMCPToolHelpers::MakeError(Request, kBPErrorInternal,
			FString::Printf(
				TEXT("FBlueprintEditorUtils::ImplementNewInterface returned false for '%s' on '%s' "
					 "(interface may carry CannotImplementInterfaceInBlueprint meta, OR the BP's parent "
					 "class already implements it natively)"),
				*InterfaceClassPath, *Path));
	}

	// StructurallyModified triggers RegenerateSkeletonOnly so the new UFunction shape surfaces on
	// the generated UFunction list. ImplementNewInterface internally calls Modify() on the BP +
	// MarkBlueprintAsModified, but the structural variant is needed for the skeleton-class
	// regeneration that adds the interface's UFunctions to GeneratedClass.
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	// Count auto-generated function-override graphs UE materialised for this interface. Pure events
	// (void return, no outputs) get wired via the EventGraph as CustomEvent nodes and don't appear
	// in Graphs[] — so this count is "function stubs generated", NOT "total interface events".
	int32 GeneratedEventCount = 0;
	for (const FBPInterfaceDescription& Desc : Blueprint->ImplementedInterfaces)
	{
		if (Desc.Interface == InterfaceClass)
		{
			GeneratedEventCount = Desc.Graphs.Num();
			break;
		}
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("added"), true);
	Out->SetStringField(TEXT("interface_class"), InterfacePath.ToString());
	Out->SetNumberField(TEXT("generated_event_count"), GeneratedEventCount);
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── bp.remove_interface (Lane A, PIE-guarded) ───────────────────────────────────────────────
//
// Removes a UInterface implementation from the blueprint via FBlueprintEditorUtils::RemoveInterface.
// UE 5.7 signature takes an FTopLevelAssetPath (FName overload deprecated since 5.1).
//
// We pass bPreserveFunctions=false per brief: any auto-generated function graphs are discarded
// alongside the interface entry. A future surface could expose bPreserveFunctions=true (promotes
// the graphs to standalone functions) if a caller actually needs that path.
//
// Pre-check: -32004 ObjectNotFound if the interface isn't currently implemented on this BP.
// (RemoveInterface returns void, so a missing-interface case would silently no-op without this
// gate.)
//
// Args:    { blueprint_path: string, interface_class_path: string }
// Result:  { removed: bool }
//
// Errors:
//   -32027 PIEActive
//   -32010 / -32004 / -32031 (blueprint resolve)
//   -32602 InvalidParams           — missing interface_class_path
//   -32023 InvalidClassPath        — interface path malformed
//   -32020 ClassNotFound           — interface path doesn't load
//   -32011 WrongClass              — resolved class is not a UInterface
//   -32004 ObjectNotFound          — interface not currently implemented on the blueprint
FMCPResponse Tool_RemoveInterface(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("BPRemoveInterface", "MCP: remove blueprint interface"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString Path;
	FMCPResponse PathErr;
	if (!BP_RequireBlueprintPath(Request, Path, PathErr)) { return PathErr; }

	UBlueprint* Blueprint = BP_ResolveBlueprintOrError(Request, Path, PathErr);
	if (!Blueprint) { return PathErr; }

	FString InterfaceClassPath;
	FMCPResponse InterfaceErr;
	UClass* InterfaceClass = BP_ResolveInterfaceClassOrError(Request, InterfaceClassPath, InterfaceErr);
	if (!InterfaceClass) { return InterfaceErr; }

	bool bWasImplemented = false;
	for (const FBPInterfaceDescription& Desc : Blueprint->ImplementedInterfaces)
	{
		if (Desc.Interface == InterfaceClass)
		{
			bWasImplemented = true;
			break;
		}
	}
	if (!bWasImplemented)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(
				TEXT("interface '%s' is not implemented on blueprint '%s'; nothing to remove"),
				*InterfaceClassPath, *Path));
	}

	const FTopLevelAssetPath InterfacePath = InterfaceClass->GetClassPathName();
	FBlueprintEditorUtils::RemoveInterface(Blueprint, InterfacePath, /*bPreserveFunctions*/ false);

	// StructurallyModified retriggers skeleton compile so the removed UFunctions drop out of the
	// generated class. Mirrors the add path.
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("removed"), true);
	Out->SetStringField(TEXT("interface_class"), InterfacePath.ToString());
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── bp.list_interfaces (Lane A, read — no PIE guard) ────────────────────────────────────────
//
// Enumerates interfaces implemented on the blueprint. By default also walks the parent UClass's
// FImplementedInterface array so the caller sees the FULL set the generated class will expose,
// including native interfaces inherited from ParentClass.
//
// source="blueprint" entries come from UBlueprint::ImplementedInterfaces (added by this BP);
// source="parent" entries come from ParentClass->Interfaces (inherited from a native or BP base).
// Deduplication: a parent-implemented interface that the BP also re-implements appears ONCE with
// source="blueprint" — the BP's explicit choice takes precedence in the display order.
//
// Args:    { blueprint_path: string, include_parent_interfaces?: bool (default true) }
// Result:  { implemented_interfaces: [{ interface_class: "/Script/...", source: "blueprint"|"parent" }] }
//
// Errors:
//   -32010 / -32004 / -32031 (blueprint resolve)
FMCPResponse Tool_ListInterfaces(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString Path;
	FMCPResponse PathErr;
	if (!BP_RequireBlueprintPath(Request, Path, PathErr)) { return PathErr; }

	UBlueprint* Blueprint = BP_ResolveBlueprintOrError(Request, Path, PathErr);
	if (!Blueprint) { return PathErr; }

	bool bIncludeParent = true;
	if (Request.Args.IsValid())
	{
		Request.Args->TryGetBoolField(TEXT("include_parent_interfaces"), bIncludeParent);
	}

	TArray<TSharedPtr<FJsonValue>> Items;
	Items.Reserve(Blueprint->ImplementedInterfaces.Num() + (bIncludeParent ? 4 : 0));

	// Track which UClass*s we've already emitted so parent-class dups don't double-list.
	TSet<UClass*> Seen;
	Seen.Reserve(Blueprint->ImplementedInterfaces.Num());

	for (const FBPInterfaceDescription& Desc : Blueprint->ImplementedInterfaces)
	{
		UClass* IfaceClass = Desc.Interface.Get();
		if (!IfaceClass) { continue; }   // dangling reference (e.g. interface asset deleted)
		Seen.Add(IfaceClass);

		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("interface_class"), IfaceClass->GetClassPathName().ToString());
		Entry->SetStringField(TEXT("source"), TEXT("blueprint"));
		Items.Add(MakeShared<FJsonValueObject>(Entry));
	}

	if (bIncludeParent && Blueprint->ParentClass)
	{
		for (const FImplementedInterface& II : Blueprint->ParentClass->Interfaces)
		{
			UClass* IfaceClass = II.Class;
			if (!IfaceClass) { continue; }
			if (Seen.Contains(IfaceClass)) { continue; }   // BP already declared it — keep "blueprint" source
			Seen.Add(IfaceClass);

			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("interface_class"), IfaceClass->GetClassPathName().ToString());
			Entry->SetStringField(TEXT("source"), TEXT("parent"));
			Items.Add(MakeShared<FJsonValueObject>(Entry));
		}
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetArrayField(TEXT("implemented_interfaces"), Items);
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── Wave F Surface 5 — Blueprint variable metadata + category enumeration (2 tools) ─────────
//
// ``bp.set_variable_metadata`` flips property-flag bits + writes named ``FBPVariableMetaDataEntry``
// entries on ``Blueprint->NewVariables[i]``. We use ``FBlueprintEditorUtils`` helpers wherever they
// exist (Category, raw MetaData kv) and fall back to direct ``PropertyFlags |= / &= ~`` toggles for
// the bits that lack a dedicated setter (CPF_Edit / CPF_DisableEditOnInstance / CPF_BlueprintReadOnly
// / CPF_Net / CPF_RepNotify / CPF_SaveGame / CPF_Transient / CPF_ExposeOnSpawn).
//
// ``bp.list_categories`` is a pure read that walks both NewVariables and FunctionGraphs to compose
// the distinct sorted category set.

namespace
{
	/**
	 * Build a JSON snapshot of one variable's current metadata, used to populate the prior/new
	 * pair in the bp.set_variable_metadata response. Mirrors the ``BP_BuildVariableSummary``
	 * field set but trimmed to just the metadata bits — pin_type and default_value are out of
	 * scope (the caller's bp.get_variable still produces those).
	 *
	 * Shape:
	 *   {
	 *     "category":            "Stats" | "",
	 *     "tooltip":             "Tooltip text" | "",
	 *     "edit_anywhere":       bool (CPF_Edit set),
	 *     "blueprint_readable":  bool (CPF_BlueprintReadOnly inverted),
	 *     "blueprint_writable":  bool (CPF_BlueprintReadOnly absent),
	 *     "instance_editable":   bool (CPF_DisableEditOnInstance absent),
	 *     "expose_on_spawn":     bool (CPF_ExposeOnSpawn OR meta ExposeOnSpawn=true),
	 *     "replicate":           "none" | "replicated" | "rep_notify",
	 *     "rep_notify_function": "FName" | "",
	 *     "save_game":           bool (CPF_SaveGame),
	 *     "transient":           bool (CPF_Transient)
	 *   }
	 */
	TSharedPtr<FJsonObject> BP_BuildVariableMetadataSnapshot(const FBPVariableDescription& Var)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("category"), Var.Category.ToString());

		// Tooltip / ExposeOnSpawn live in MetaDataArray. We follow BP_BuildVariableSummary's
		// case-insensitive comparison since Epic sometimes serialises with mixed case.
		FString Tooltip;
		bool bExposeOnSpawnMeta = false;
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
				bExposeOnSpawnMeta = Meta.DataValue.Equals(TEXT("true"), ESearchCase::IgnoreCase);
			}
		}
		Obj->SetStringField(TEXT("tooltip"), Tooltip);

		const uint64 Flags = Var.PropertyFlags;
		// edit_anywhere — exposed in editor (Details panel) iff CPF_Edit is set. UE's variable
		// detail panel toggle "Instance Editable" controls CPF_DisableEditOnInstance separately;
		// edit_anywhere here = "the property is editable in any context".
		Obj->SetBoolField(TEXT("edit_anywhere"), (Flags & CPF_Edit) != 0);
		// blueprint_readable / blueprint_writable are derived from CPF_BlueprintReadOnly + the
		// presence of the variable on a BP at all (we know it's present — it's the one we just
		// looked up). Readable is ALWAYS true for BP vars; writable iff CPF_BlueprintReadOnly absent.
		Obj->SetBoolField(TEXT("blueprint_readable"), true);
		Obj->SetBoolField(TEXT("blueprint_writable"), (Flags & CPF_BlueprintReadOnly) == 0);
		Obj->SetBoolField(TEXT("instance_editable"), (Flags & CPF_DisableEditOnInstance) == 0);

		const bool bExposeOnSpawnFlag = (Flags & CPF_ExposeOnSpawn) != 0;
		Obj->SetBoolField(TEXT("expose_on_spawn"), bExposeOnSpawnFlag || bExposeOnSpawnMeta);

		const bool bIsNet = (Flags & CPF_Net) != 0;
		const bool bIsRepNotify = (Flags & CPF_RepNotify) != 0;
		const TCHAR* RepStr = TEXT("none");
		if (bIsRepNotify) { RepStr = TEXT("rep_notify"); }
		else if (bIsNet)  { RepStr = TEXT("replicated"); }
		Obj->SetStringField(TEXT("replicate"), RepStr);
		Obj->SetStringField(TEXT("rep_notify_function"), Var.RepNotifyFunc.IsNone() ? FString() : Var.RepNotifyFunc.ToString());

		Obj->SetBoolField(TEXT("save_game"), (Flags & CPF_SaveGame) != 0);
		Obj->SetBoolField(TEXT("transient"), (Flags & CPF_Transient) != 0);
		return Obj;
	}

	/**
	 * Apply a property-flag bit on FBPVariableDescription. ``bSet`` true → bitwise OR; false →
	 * bitwise AND-complement. Centralised so the set/clear branches don't sprawl through the
	 * tool body.
	 */
	void BP_ApplyVariablePropertyFlag(FBPVariableDescription& Var, uint64 Flag, bool bSet)
	{
		if (bSet) { Var.PropertyFlags |= Flag; }
		else      { Var.PropertyFlags &= ~Flag; }
	}
} // namespace

// ─── bp.set_variable_metadata (Lane A, PIE-guarded) ──────────────────────────────────────────
//
// Mutates the per-variable metadata + property-flag bits. EVERY field in args.metadata is OPTIONAL
// — only the keys present in the supplied object are written. Returns a {prior, new} snapshot pair
// so callers can revert via a follow-up set_variable_metadata call.
//
// Field mapping:
//   category            → FBPVariableDescription::Category (via SetBlueprintVariableCategory)
//   tooltip             → MetaDataArray["Tooltip"] (via SetBlueprintVariableMetaData)
//   edit_anywhere       → CPF_Edit bit + clears CPF_DisableEditOnInstance when enabling
//   blueprint_readable  → reserved (currently informational — BP vars are always BP-readable)
//   blueprint_writable  → CPF_BlueprintReadOnly bit (writable=true clears it, false sets it)
//   instance_editable   → CPF_DisableEditOnInstance bit (instance_editable=true CLEARS it)
//   expose_on_spawn     → CPF_ExposeOnSpawn bit AND MetaDataArray["ExposeOnSpawn"] (engine sets
//                         both depending on path; we sync them)
//   replicate           → "none" / "replicated" / "rep_notify" — manages CPF_Net + CPF_RepNotify
//   rep_notify_function → FBPVariableDescription::RepNotifyFunc (only meaningful when replicate
//                         is "rep_notify"; ignored otherwise)
//   save_game           → CPF_SaveGame bit
//   transient           → CPF_Transient bit
//
// Args:    { blueprint_path: string, variable_name: string,
//            metadata: { category?, tooltip?, edit_anywhere?, blueprint_readable?,
//                        blueprint_writable?, instance_editable?, expose_on_spawn?,
//                        replicate?: "none"|"replicated"|"rep_notify",
//                        rep_notify_function?, save_game?, transient? } }
// Result:  { prior: {...}, new: {...} }
//
// Errors:
//   -32027 PIEActive
//   -32010 / -32004 / -32031 (resolve)
//   -32602 InvalidParams       — missing variable_name OR missing 'metadata' object OR
//                                unknown 'replicate' value (must be one of three keywords)
//   -32037 VariableNotFound    — name not present in NewVariables[]
FMCPResponse Tool_SetVariableMetadata(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("BPSetVariableMetadata", "MCP: set variable metadata"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString Path;
	FMCPResponse PathErr;
	if (!BP_RequireBlueprintPath(Request, Path, PathErr)) { return PathErr; }

	FString VarNameStr;
	if (!Request.Args->TryGetStringField(TEXT("variable_name"), VarNameStr) || VarNameStr.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			TEXT("missing required string field 'variable_name'"));
	}

	const TSharedPtr<FJsonObject>* MetadataObjPtr = nullptr;
	if (!Request.Args->TryGetObjectField(TEXT("metadata"), MetadataObjPtr) || !MetadataObjPtr || !MetadataObjPtr->IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			TEXT("missing required object field 'metadata'"));
	}
	const TSharedPtr<FJsonObject>& Metadata = *MetadataObjPtr;

	FMCPResponse ResolveErr;
	UBlueprint* Blueprint = BP_ResolveBlueprintOrError(Request, Path, ResolveErr);
	if (!Blueprint) { return ResolveErr; }

	const FName VarName(*VarNameStr);
	const int32 Idx = FMCPBlueprintUtils::FindVariableIndex(Blueprint, VarName);
	if (Idx == INDEX_NONE)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorVariableNotFound,
			FString::Printf(TEXT("variable '%s' not found on blueprint '%s'"),
				*VarNameStr, *Path));
	}

	// Validate replicate field up-front so we fail before any mutation is applied.
	enum class EReplicateMode : uint8 { Unset, None, Replicated, RepNotify };
	EReplicateMode ReplicateMode = EReplicateMode::Unset;
	FString ReplicateStr;
	if (Metadata->TryGetStringField(TEXT("replicate"), ReplicateStr))
	{
		if      (ReplicateStr.Equals(TEXT("none"),       ESearchCase::IgnoreCase)) { ReplicateMode = EReplicateMode::None; }
		else if (ReplicateStr.Equals(TEXT("replicated"), ESearchCase::IgnoreCase)) { ReplicateMode = EReplicateMode::Replicated; }
		else if (ReplicateStr.Equals(TEXT("rep_notify"), ESearchCase::IgnoreCase)) { ReplicateMode = EReplicateMode::RepNotify; }
		else
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
				FString::Printf(
					TEXT("metadata.replicate must be 'none'|'replicated'|'rep_notify', got '%s'"),
					*ReplicateStr));
		}
	}

	// Snapshot prior BEFORE any mutation so the response carries the rollback state.
	TSharedPtr<FJsonObject> PriorSnap = BP_BuildVariableMetadataSnapshot(Blueprint->NewVariables[Idx]);

	// ─── Category (uses canonical helper which also handles recompile flag) ──────────────────
	FString CategoryStr;
	if (Metadata->TryGetStringField(TEXT("category"), CategoryStr))
	{
		FBlueprintEditorUtils::SetBlueprintVariableCategory(
			Blueprint, VarName, /*InScope*/ nullptr,
			FText::FromString(CategoryStr));
	}

	// ─── Tooltip (uses canonical helper) ─────────────────────────────────────────────────────
	FString TooltipStr;
	if (Metadata->TryGetStringField(TEXT("tooltip"), TooltipStr))
	{
		if (TooltipStr.IsEmpty())
		{
			// Clear-side: the canonical helper has no "remove" path for an empty value; use the
			// explicit RemoveBlueprintVariableMetaData helper so the kv entry drops entirely.
			FBlueprintEditorUtils::RemoveBlueprintVariableMetaData(
				Blueprint, VarName, /*InScope*/ nullptr, FName(TEXT("Tooltip")));
		}
		else
		{
			FBlueprintEditorUtils::SetBlueprintVariableMetaData(
				Blueprint, VarName, /*InScope*/ nullptr, FName(TEXT("Tooltip")), TooltipStr);
		}
	}

	// ─── Property-flag bits — direct manipulation ────────────────────────────────────────────
	// NB: All mutators that flip flags must Modify() the BP first so undo captures the prior
	// state. We've already entered FScopedTransaction; Modify() registers this BP's prior PropertyFlags.
	FBPVariableDescription& Var = Blueprint->NewVariables[Idx];
	bool bAnyFlagChanged = false;

	bool bEditAnywhere = false;
	if (Metadata->TryGetBoolField(TEXT("edit_anywhere"), bEditAnywhere))
	{
		// edit_anywhere=true → exposes the property in BOTH archetype editor AND placed instances:
		// we set CPF_Edit and CLEAR CPF_DisableEditOnInstance simultaneously. Setting it to false
		// just clears CPF_Edit (caller can independently re-enable instance_editable).
		Blueprint->Modify();
		BP_ApplyVariablePropertyFlag(Var, CPF_Edit, bEditAnywhere);
		if (bEditAnywhere)
		{
			BP_ApplyVariablePropertyFlag(Var, CPF_DisableEditOnInstance, false);
		}
		bAnyFlagChanged = true;
	}

	bool bBPWritable = false;
	if (Metadata->TryGetBoolField(TEXT("blueprint_writable"), bBPWritable))
	{
		Blueprint->Modify();
		// blueprint_writable=true → clear CPF_BlueprintReadOnly. =false → set it.
		BP_ApplyVariablePropertyFlag(Var, CPF_BlueprintReadOnly, !bBPWritable);
		bAnyFlagChanged = true;
	}

	// blueprint_readable is currently informational only — BP variables are always BP-readable in
	// UE (the flag controlling read access is just "is it on the class"). We accept the field
	// silently for forward-compat (would map to a future BlueprintHiddenAccessSpecifier).
	bool bBPReadableScratch = false;
	(void)Metadata->TryGetBoolField(TEXT("blueprint_readable"), bBPReadableScratch);

	bool bInstanceEditable = false;
	if (Metadata->TryGetBoolField(TEXT("instance_editable"), bInstanceEditable))
	{
		Blueprint->Modify();
		// instance_editable=true CLEARS CPF_DisableEditOnInstance.
		BP_ApplyVariablePropertyFlag(Var, CPF_DisableEditOnInstance, !bInstanceEditable);
		bAnyFlagChanged = true;
	}

	bool bExposeOnSpawn = false;
	if (Metadata->TryGetBoolField(TEXT("expose_on_spawn"), bExposeOnSpawn))
	{
		Blueprint->Modify();
		BP_ApplyVariablePropertyFlag(Var, CPF_ExposeOnSpawn, bExposeOnSpawn);
		// Engine also stores ExposeOnSpawn in MetaData for one of two reasons: legacy serialisation
		// path or BP variable detail panel UX. Sync both so reads via BP_BuildVariableSummary +
		// BP_BuildVariableMetadataSnapshot return consistent state regardless of which path the
		// engine queries.
		if (bExposeOnSpawn)
		{
			FBlueprintEditorUtils::SetBlueprintVariableMetaData(
				Blueprint, VarName, /*InScope*/ nullptr, FName(TEXT("ExposeOnSpawn")), TEXT("true"));
		}
		else
		{
			FBlueprintEditorUtils::RemoveBlueprintVariableMetaData(
				Blueprint, VarName, /*InScope*/ nullptr, FName(TEXT("ExposeOnSpawn")));
		}
		bAnyFlagChanged = true;
	}

	if (ReplicateMode != EReplicateMode::Unset)
	{
		Blueprint->Modify();
		// Manage CPF_Net + CPF_RepNotify pair atomically. rep_notify_function is read once and
		// only honoured when ReplicateMode=RepNotify (matches engine UX — the field is greyed out
		// for the other two modes in the BP variable detail panel).
		switch (ReplicateMode)
		{
		case EReplicateMode::None:
			BP_ApplyVariablePropertyFlag(Var, CPF_Net,        false);
			BP_ApplyVariablePropertyFlag(Var, CPF_RepNotify,  false);
			Var.RepNotifyFunc = NAME_None;
			break;
		case EReplicateMode::Replicated:
			BP_ApplyVariablePropertyFlag(Var, CPF_Net,        true);
			BP_ApplyVariablePropertyFlag(Var, CPF_RepNotify,  false);
			Var.RepNotifyFunc = NAME_None;
			break;
		case EReplicateMode::RepNotify:
		{
			BP_ApplyVariablePropertyFlag(Var, CPF_Net,        true);
			BP_ApplyVariablePropertyFlag(Var, CPF_RepNotify,  true);
			FString RepNotifyFuncStr;
			if (Metadata->TryGetStringField(TEXT("rep_notify_function"), RepNotifyFuncStr)
				&& !RepNotifyFuncStr.IsEmpty())
			{
				Var.RepNotifyFunc = FName(*RepNotifyFuncStr);
			}
			break;
		}
		default:
			break;
		}
		bAnyFlagChanged = true;
	}
	else
	{
		// replicate field absent but rep_notify_function may still be supplied (rare — caller is
		// just updating the function name without changing the mode). Honour it iff CPF_RepNotify
		// already set.
		FString RepNotifyFuncStr;
		if (Metadata->TryGetStringField(TEXT("rep_notify_function"), RepNotifyFuncStr))
		{
			Blueprint->Modify();
			Var.RepNotifyFunc = RepNotifyFuncStr.IsEmpty() ? NAME_None : FName(*RepNotifyFuncStr);
			bAnyFlagChanged = true;
		}
	}

	bool bSaveGame = false;
	if (Metadata->TryGetBoolField(TEXT("save_game"), bSaveGame))
	{
		Blueprint->Modify();
		BP_ApplyVariablePropertyFlag(Var, CPF_SaveGame, bSaveGame);
		bAnyFlagChanged = true;
	}
	bool bTransient = false;
	if (Metadata->TryGetBoolField(TEXT("transient"), bTransient))
	{
		Blueprint->Modify();
		BP_ApplyVariablePropertyFlag(Var, CPF_Transient, bTransient);
		bAnyFlagChanged = true;
	}

	// MarkBlueprintAsStructurallyModified: the canonical helpers above (Category / MetaData) already
	// dirty the BP via internal MarkBlueprintAsModified calls, but property-flag flips do NOT pass
	// through any helper that schedules recompile. Flag changes alter the generated UClass shape
	// (CPF_Net flips REPNOTIFY codegen, CPF_RepNotify flips OnRep_* lookups, etc.) — these REQUIRE
	// skeleton recompile to materialise on the GeneratedClass.
	if (bAnyFlagChanged)
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	}

	TSharedPtr<FJsonObject> NewSnap = BP_BuildVariableMetadataSnapshot(Blueprint->NewVariables[Idx]);

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetObjectField(TEXT("prior"), PriorSnap);
	Out->SetObjectField(TEXT("new"), NewSnap);
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── bp.list_categories (Lane A, no PIE guard, READ) ─────────────────────────────────────────
//
// Enumerate distinct category names across:
//   - Blueprint->NewVariables[i].Category (FText.ToString)
//   - Each function graph's FKismetUserDeclaredFunctionMetadata::Category (via GetGraphFunctionMetaData)
//
// Empty / unset categories are SKIPPED — they'd otherwise pollute the set with "" entries that
// have no surface meaning. Result is deterministically sorted (TSet.Array().Sort()).
//
// Args:    { blueprint_path: string }
// Result:  { categories: [string] }
//
// Errors:
//   -32010 / -32004 / -32031 (resolve)
FMCPResponse Tool_ListCategories(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString Path;
	FMCPResponse PathErr;
	if (!BP_RequireBlueprintPath(Request, Path, PathErr)) { return PathErr; }

	FMCPResponse ResolveErr;
	UBlueprint* Blueprint = BP_ResolveBlueprintOrError(Request, Path, ResolveErr);
	if (!Blueprint) { return ResolveErr; }

	TSet<FString> Categories;
	Categories.Reserve(Blueprint->NewVariables.Num() + Blueprint->FunctionGraphs.Num());

	for (const FBPVariableDescription& Var : Blueprint->NewVariables)
	{
		const FString Cat = Var.Category.ToString();
		if (!Cat.IsEmpty())
		{
			Categories.Add(Cat);
		}
	}

	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (!Graph) { continue; }
		if (FKismetUserDeclaredFunctionMetadata* Meta = FBlueprintEditorUtils::GetGraphFunctionMetaData(Graph))
		{
			const FString Cat = Meta->Category.ToString();
			if (!Cat.IsEmpty())
			{
				Categories.Add(Cat);
			}
		}
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetArrayField(TEXT("categories"), BP_StringSetToJsonArray(Categories));
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
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

	// Wave F Surface 2 — function-signature edit surface (4 tools, all Lane A — UEdGraph mutation
	// + Blueprint recompile demand the game thread). The list variant is the only read-side tool
	// (no PIE guard); add/remove/set_metadata are PIE-guarded mutators with FScopedTransaction.
	RegisterTool(TEXT("bp.add_function_parameter"),    &Tool_AddFunctionParameter,    /*Lane A*/ false);
	RegisterTool(TEXT("bp.remove_function_parameter"), &Tool_RemoveFunctionParameter, /*Lane A*/ false);
	RegisterTool(TEXT("bp.list_function_parameters"),  &Tool_ListFunctionParameters,  /*Lane A*/ false);
	RegisterTool(TEXT("bp.set_function_metadata"),     &Tool_SetFunctionMetadata,     /*Lane A*/ false);

	// Wave F Surface 4 — interface-implementation surface (3 tools, all Lane A — UBlueprint
	// mutation requires the game thread). add/remove are PIE-guarded; list is a pure read.
	RegisterTool(TEXT("bp.add_interface"),    &Tool_AddInterface,    /*Lane A*/ false);
	RegisterTool(TEXT("bp.remove_interface"), &Tool_RemoveInterface, /*Lane A*/ false);
	RegisterTool(TEXT("bp.list_interfaces"),  &Tool_ListInterfaces,  /*Lane A*/ false);

	// Wave F Surface 5 — variable-metadata + category enumeration (2 tools, Lane A). set_variable_
	// metadata is PIE-guarded (manipulates FBPVariableDescription PropertyFlags + MetaDataArray);
	// list_categories is a pure read with no PIE guard. The two comment-node tools that round out
	// Surface 5 (bp.add_comment / bp.delete_comment) live in BlueprintGraphTools alongside the
	// other UEdGraph-node CRUD tools.
	RegisterTool(TEXT("bp.set_variable_metadata"), &Tool_SetVariableMetadata, /*Lane A*/ false);
	RegisterTool(TEXT("bp.list_categories"),       &Tool_ListCategories,      /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("Phase 4 Days 1-10 + bp.create_blueprint + Wave F2 + Wave F4 + Wave F5: registered 23 bp.* handlers "
			 "(6 reads + 6 writes + 1 compile + 1 creator + 4 function-signature + 3 interface + 2 variable-metadata, all Lane A); ")
		TEXT("bp.compile_all_dirty registered separately via FBlueprintCompositeTools"));
}

} // namespace FBlueprintTools

#undef LOCTEXT_NAMESPACE
