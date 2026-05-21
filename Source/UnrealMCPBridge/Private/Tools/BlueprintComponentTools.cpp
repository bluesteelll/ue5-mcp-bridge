// Copyright FatumGame. All Rights Reserved.

#include "BlueprintComponentTools.h"

#include "MCPSurfaceRegistry.h"

#include "FMCPDispatchQueue.h"
#include "MCPJsonBuilder.h"
#include "MCPMutatorScope.h"
#include "MCPToolHelpers.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPBlueprintUtils.h"
#include "Utils/MCPReflection.h"
#include "Utils/MCPWorldContext.h"

#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// BPSCS_ prefix per the unity-build symbol-collision pattern. Per-surface error constants kept;
	// XX_StampIds/MakeError/MakeSuccessObj removed in Phase 3 — use FMCPToolHelpers::Xxx
	// from MCPToolHelpers.h. PIE-guard + FScopedTransaction in Tool_AddComponent and
	// Tool_RemoveComponent migrated to FMCPMutatorScope; Tool_SetComponentDefault keeps its inline
	// PIE check because FMCPWritePropertyScope owns its own transaction (avoid nested transactions).
	constexpr int32 kBPSCSErrorInvalidParams = -32602;
	constexpr int32 kBPSCSErrorInternal      = -32603;

	/** True if PIE is running. */
	bool BPSCS_IsPIEActive()
	{
		return FMCPWorldContext::IsPIEActive();
	}

	/** Frozen PIE-mutator refusal — used by Tool_SetComponentDefault whose transaction is owned by
	 *  FMCPWritePropertyScope (nested FMCPMutatorScope transactions would clash). */
	FMCPResponse BPSCS_MakePIEError(const FMCPRequest& Request)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPIEActive, kMCPMessagePIEActive);
	}

	/** Read ``args.blueprint_path`` field; emit -32602 InvalidParams on missing/empty. */
	bool BPSCS_RequireBlueprintPath(const FMCPRequest& Request, FString& OutPath, FMCPResponse& OutError)
	{
		return FMCPToolHelpers::RequireStringField(Request, TEXT("blueprint_path"), OutPath, OutError);
	}

	/** Resolve a blueprint by path with type validation. Returns nullptr + populates OutError on failure. */
	UBlueprint* BPSCS_ResolveBlueprintOrError(const FMCPRequest& Request, const FString& Path, FMCPResponse& OutError)
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

	/**
	 * Resolve ``component_class_path`` to a concrete UClass that is a UActorComponent subclass.
	 *
	 * Surfaces the 4-code family per Wave F3 brief:
	 *   -32023 InvalidClassPath  — syntactically malformed
	 *   -32020 ClassNotFound     — well-formed path, LoadObject returned null after _C retry
	 *   -32021 ClassAbstract     — class has CLASS_Abstract
	 *   -32011 WrongClass        — not a UActorComponent subclass (e.g. AActor path passed)
	 */
	UClass* BPSCS_ResolveComponentClass(const FMCPRequest& Request, const FString& ClassPath,
		FMCPResponse& OutError)
	{
		if (ClassPath.IsEmpty() || ClassPath[0] != TEXT('/'))
		{
			OutError = FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidClassPath,
				FString::Printf(
					TEXT("component_class_path '%s' invalid — must start with '/' (e.g. '/Script/Engine.StaticMeshComponent')"),
					*ClassPath));
			return nullptr;
		}
		if (ClassPath.Contains(TEXT("\\")))
		{
			OutError = FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidClassPath,
				FString::Printf(TEXT("component_class_path '%s' contains backslash"), *ClassPath));
			return nullptr;
		}

		UClass* Class = LoadObject<UClass>(nullptr, *ClassPath);
		if (!Class && !ClassPath.EndsWith(TEXT("_C")))
		{
			// BP component class probe — try the auto-generated _C suffix.
			const FString Retry = ClassPath + TEXT("_C");
			Class = LoadObject<UClass>(nullptr, *Retry);
		}
		if (!Class)
		{
			OutError = FMCPToolHelpers::MakeError(Request, kMCPErrorClassNotFound,
				FString::Printf(
					TEXT("component_class_path '%s' could not be resolved to a UClass (LoadObject returned null); ")
					TEXT("for Blueprint component classes try with trailing '_C'"),
					*ClassPath));
			return nullptr;
		}
		if (!Class->IsChildOf(UActorComponent::StaticClass()))
		{
			// Wave F3 brief specifies -32011 WrongClass for this case (rather than the broader
			// -32022 WrongClassFamily used by the component.add surface). Both communicate the
			// same intent but the brief's choice keeps BP-SCS errors in a tighter numeric range.
			OutError = FMCPToolHelpers::MakeError(Request, kMCPErrorWrongClass,
				FString::Printf(
					TEXT("component_class_path '%s' is not a UActorComponent subclass (got base '%s')"),
					*Class->GetPathName(),
					Class->GetSuperClass() ? *Class->GetSuperClass()->GetPathName() : TEXT("?")));
			return nullptr;
		}
		if (Class->HasAnyClassFlags(CLASS_Abstract))
		{
			OutError = FMCPToolHelpers::MakeError(Request, kMCPErrorClassAbstract,
				FString::Printf(
					TEXT("component_class_path '%s' is abstract — pick a concrete subclass"),
					*Class->GetPathName()));
			return nullptr;
		}
		return Class;
	}

	/**
	 * Build the JSON entry for one SCS node (used by bp.list_components).
	 *
	 * Shape:
	 *   { variable_name, component_class, parent_variable_name?, attach_socket? }
	 *
	 * ``parent_variable_name`` is null for root SCS nodes; ``attach_socket`` is null when
	 * ``AttachToName`` is NAME_None.
	 */
	TSharedRef<FJsonObject> BPSCS_BuildSCSNodeSummary(const USCS_Node* Node, const USCS_Node* ParentNode)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		if (!Node)
		{
			return Out;
		}
		Out->SetStringField(TEXT("variable_name"), Node->GetVariableName().ToString());
		Out->SetStringField(TEXT("component_class"),
			Node->ComponentClass ? Node->ComponentClass->GetPathName() : FString(TEXT("")));
		if (ParentNode)
		{
			Out->SetStringField(TEXT("parent_variable_name"), ParentNode->GetVariableName().ToString());
		}
		else
		{
			Out->SetField(TEXT("parent_variable_name"), MakeShared<FJsonValueNull>());
		}
		if (Node->AttachToName != NAME_None)
		{
			Out->SetStringField(TEXT("attach_socket"), Node->AttachToName.ToString());
		}
		else
		{
			Out->SetField(TEXT("attach_socket"), MakeShared<FJsonValueNull>());
		}
		return Out;
	}
} // namespace

namespace FBlueprintComponentTools
{

// ─── bp.add_component (Lane A, PIE-guarded) ──────────────────────────────────────────────────
//
// Args:    { blueprint_path: string, component_class_path: string, variable_name: string,
//            parent_component?: string (default = DefaultSceneRoot / first root) }
// Result:  { added: bool, variable_name, component_class, parent: string | null }
//
// Errors:
//   -32027 PIEActive
//   -32602 InvalidParams              — missing blueprint_path / component_class_path / variable_name
//   -32004 ObjectNotFound             — BP not found OR parent_component name not present on SCS
//   -32010 InvalidPath                — blueprint_path malformed
//   -32031 BlueprintTypeMismatch      — path resolved to non-UBlueprint asset
//   -32014 PathInUse                  — variable_name collides with an existing SCS_Node
//   -32020 ClassNotFound              — LoadObject failed for component_class_path
//   -32021 ClassAbstract              — class has CLASS_Abstract
//   -32011 WrongClass                 — class is not a UActorComponent subclass
//   -32023 InvalidClassPath           — class_path malformed
//   -32603 Internal                   — BP has no SimpleConstructionScript OR CreateNode returned null
FMCPResponse Tool_AddComponent(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("BPCAddComponent", "MCP: add SCS component"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString Path;
	FMCPResponse PathErr;
	if (!BPSCS_RequireBlueprintPath(Request, Path, PathErr)) { return PathErr; }

	FString ClassPath;
	if (!Request.Args->TryGetStringField(TEXT("component_class_path"), ClassPath) || ClassPath.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kBPSCSErrorInvalidParams,
			TEXT("missing required string field 'component_class_path'"));
	}

	FString VariableNameStr;
	if (!Request.Args->TryGetStringField(TEXT("variable_name"), VariableNameStr) || VariableNameStr.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kBPSCSErrorInvalidParams,
			TEXT("missing required string field 'variable_name'"));
	}

	// Optional parent component selector. When omitted/empty we fall back to DefaultSceneRoot (or
	// the first root node if DefaultSceneRoot is null after asset edits).
	FString ParentVarStr;
	const bool bParentProvided =
		Request.Args->TryGetStringField(TEXT("parent_component"), ParentVarStr) && !ParentVarStr.IsEmpty();

	FMCPResponse ResolveErr;
	UBlueprint* Blueprint = BPSCS_ResolveBlueprintOrError(Request, Path, ResolveErr);
	if (!Blueprint) { return ResolveErr; }

	USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
	if (!SCS)
	{
		return FMCPToolHelpers::MakeError(Request, kBPSCSErrorInternal,
			FString::Printf(TEXT("blueprint '%s' has no SimpleConstructionScript (non-AActor parent? %s)"),
				*Path,
				Blueprint->ParentClass ? *Blueprint->ParentClass->GetPathName() : TEXT("null")));
	}

	FMCPResponse ClassErr;
	UClass* CompClass = BPSCS_ResolveComponentClass(Request, ClassPath, ClassErr);
	if (!CompClass) { return ClassErr; }

	const FName VariableName(*VariableNameStr);

	// Pre-check name collision so caller gets -32014 instead of a silent UE auto-suffix. SCS->FindSCSNode
	// scans the AllNodes map so we cover both root + deeply-nested children in one lookup.
	if (SCS->FindSCSNode(VariableName) != nullptr)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPathInUse,
			FString::Printf(
				TEXT("variable_name '%s' already in use on blueprint '%s' SCS — pick a unique name or call bp.remove_component first"),
				*VariableNameStr, *Path));
	}

	// Resolve parent node (if provided). Implicitly null means root-tier placement (DefaultSceneRoot
	// for scene components, or just a non-scene SCS root for actor components).
	USCS_Node* ParentNode = nullptr;
	if (bParentProvided)
	{
		// "DefaultSceneRoot" is the conventional name of the implicit root scene component on bare
		// Actor BPs. We treat the literal string as a synonym for "explicit root" so callers can be
		// explicit without having to query the actual current root variable name first.
		const FName ParentFName(*ParentVarStr);
		ParentNode = SCS->FindSCSNode(ParentFName);
		if (!ParentNode)
		{
			// One special case: the caller asked for "DefaultSceneRoot" but the BP may have replaced
			// it with a user scene component (which is then the actual root). Fall back to the SCS's
			// concrete DefaultSceneRootNode lookup.
			if (ParentVarStr.Equals(TEXT("DefaultSceneRoot"), ESearchCase::IgnoreCase))
			{
				ParentNode = SCS->GetDefaultSceneRootNode();
			}
			if (!ParentNode)
			{
				return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
					FString::Printf(
						TEXT("parent_component '%s' not found in SCS of blueprint '%s'"),
						*ParentVarStr, *Path));
			}
		}
	}

	const bool bIsScene = CompClass->IsChildOf(USceneComponent::StaticClass());

	// FMCPMutatorScope at function-top owns the FScopedTransaction lifetime.
	SCS->Modify();

	USCS_Node* NewNode = SCS->CreateNode(CompClass, VariableName);
	if (!NewNode)
	{
		return FMCPToolHelpers::MakeError(Request, kBPSCSErrorInternal,
			FString::Printf(
				TEXT("USimpleConstructionScript::CreateNode returned null for class '%s' var '%s' on '%s'"),
				*CompClass->GetPathName(), *VariableNameStr, *Path));
	}

	// Placement rule:
	//   * scene component + caller-specified parent → AddChildNode under that parent
	//   * scene component + no parent → AddChildNode under DefaultSceneRoot if present, else AddNode
	//     (root). The "else" path handles bare Actor BPs with no default scene root (impossible in
	//     practice — UBlueprintFactory always creates one — but defensive).
	//   * non-scene component → AddNode (SCS root tier; non-scene SCS_Nodes never live under scene
	//     parents). parent_component is ignored with this class family per the header contract.
	FString FinalParentName;
	if (bIsScene)
	{
		USCS_Node* EffectiveParent = ParentNode;
		if (!EffectiveParent)
		{
			EffectiveParent = SCS->GetDefaultSceneRootNode();
		}
		if (EffectiveParent)
		{
			EffectiveParent->Modify();
			EffectiveParent->AddChildNode(NewNode);
			FinalParentName = EffectiveParent->GetVariableName().ToString();
		}
		else
		{
			SCS->AddNode(NewNode);
			// FinalParentName left empty → reported as null in the response.
		}
	}
	else
	{
		// Non-scene actor components live at the SCS root tier. The parent_component argument is
		// silently ignored (header contract — matches BP editor behaviour).
		SCS->AddNode(NewNode);
	}

	// Structural mutation → next compile must regenerate the BPGC's SCS layout. MarkBlueprintAsModified
	// alone would only refresh the variable table — wrong for SCS topology changes.
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	return FMCPJsonBuilder()
		.Bool(TEXT("added"), true)
		.Str(TEXT("variable_name"), NewNode->GetVariableName().ToString())
		.Str(TEXT("component_class"), CompClass->GetPathName())
		.If(!FinalParentName.IsEmpty(),
			[&](FMCPJsonBuilder& B) { B.Str(TEXT("parent"), FinalParentName); })
		.If(FinalParentName.IsEmpty(),
			[&](FMCPJsonBuilder& B) { B.Null(TEXT("parent")); })
		.BuildSuccess(Request);
}

// ─── bp.remove_component (Lane A, PIE-guarded) ───────────────────────────────────────────────
//
// Args:    { blueprint_path: string, variable_name: string }
// Result:  { removed: bool, reparented_children_count: number }
//
// Errors:
//   -32027 PIEActive
//   -32602 InvalidParams         — missing blueprint_path / variable_name
//   -32004 ObjectNotFound        — BP not found OR variable_name not present in SCS
//   -32010 InvalidPath           — blueprint_path malformed
//   -32031 BlueprintTypeMismatch — non-UBlueprint asset
//   -32603 Internal              — BP has no SimpleConstructionScript
FMCPResponse Tool_RemoveComponent(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("BPCRemoveComponent", "MCP: remove SCS component"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString Path;
	FMCPResponse PathErr;
	if (!BPSCS_RequireBlueprintPath(Request, Path, PathErr)) { return PathErr; }

	FString VariableNameStr;
	if (!Request.Args->TryGetStringField(TEXT("variable_name"), VariableNameStr) || VariableNameStr.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kBPSCSErrorInvalidParams,
			TEXT("missing required string field 'variable_name'"));
	}

	FMCPResponse ResolveErr;
	UBlueprint* Blueprint = BPSCS_ResolveBlueprintOrError(Request, Path, ResolveErr);
	if (!Blueprint) { return ResolveErr; }

	USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
	if (!SCS)
	{
		return FMCPToolHelpers::MakeError(Request, kBPSCSErrorInternal,
			FString::Printf(TEXT("blueprint '%s' has no SimpleConstructionScript"), *Path));
	}

	const FName VariableName(*VariableNameStr);
	USCS_Node* TargetNode = SCS->FindSCSNode(VariableName);
	if (!TargetNode)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(
				TEXT("variable_name '%s' not found in SCS of blueprint '%s'"),
				*VariableNameStr, *Path));
	}

	// Capture topology BEFORE mutation. ChildNodes is a TArray<TObjectPtr<USCS_Node>>; we want a
	// raw-pointer snapshot we can safely walk after the parent unlinks (the array on the source
	// is about to mutate).
	const TArray<USCS_Node*> Children = TargetNode->GetChildNodes();
	USCS_Node* ParentNode = SCS->FindParentNode(TargetNode);

	// FMCPMutatorScope at function-top owns the FScopedTransaction lifetime.
	SCS->Modify();
	TargetNode->Modify();
	if (ParentNode) { ParentNode->Modify(); }

	// Re-parent children FIRST — match BP editor "Delete" behaviour where children get promoted up
	// one level rather than being orphaned. ``RemoveChildNode`` calls cascade through
	// FSCSAllNodesHelper so the SCS->AllNodes map stays consistent.
	int32 ReparentedCount = 0;
	for (USCS_Node* Child : Children)
	{
		if (!Child) { continue; }
		Child->Modify();
		TargetNode->RemoveChildNode(Child, /*bRemoveFromAllNodes*/ false);
		if (ParentNode)
		{
			ParentNode->AddChildNode(Child, /*bAddToAllNodes*/ false);
		}
		else
		{
			// TargetNode was itself a root — promote child to root.
			SCS->AddNode(Child);
		}
		++ReparentedCount;
	}

	// Now remove the target. We pass bValidateSceneRootNodes=true so the SCS picks a new default
	// scene root if the removed node was the root (matches BP editor's "Delete Root" semantics).
	if (ParentNode)
	{
		ParentNode->RemoveChildNode(TargetNode);
	}
	else
	{
		SCS->RemoveNode(TargetNode, /*bValidateSceneRootNodes*/ true);
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	return FMCPJsonBuilder()
		.Bool(TEXT("removed"), true)
		.Num(TEXT("reparented_children_count"), static_cast<double>(ReparentedCount))
		.BuildSuccess(Request);
}

// ─── bp.list_components (Lane A, NO PIE guard — read) ────────────────────────────────────────
//
// Args:    { blueprint_path: string, recursive?: bool (default true) }
// Result:  { root: string | null, components: [{variable_name, component_class,
//                                              parent_variable_name?, attach_socket?}] }
//
// When ``recursive`` is false we return only the root-tier SCS nodes (matches the BP editor's
// "show only root components" view). Default ``recursive=true`` walks SCS->GetAllNodes which
// includes both roots + every nested child.
//
// Errors:
//   -32602 InvalidParams         — missing blueprint_path
//   -32004 ObjectNotFound        — BP not found
//   -32010 InvalidPath           — blueprint_path malformed
//   -32031 BlueprintTypeMismatch — non-UBlueprint asset
//   -32603 Internal              — BP has no SimpleConstructionScript
FMCPResponse Tool_ListComponents(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString Path;
	FMCPResponse PathErr;
	if (!BPSCS_RequireBlueprintPath(Request, Path, PathErr)) { return PathErr; }

	bool bRecursive = true;
	Request.Args->TryGetBoolField(TEXT("recursive"), bRecursive);

	FMCPResponse ResolveErr;
	UBlueprint* Blueprint = BPSCS_ResolveBlueprintOrError(Request, Path, ResolveErr);
	if (!Blueprint) { return ResolveErr; }

	USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
	if (!SCS)
	{
		return FMCPToolHelpers::MakeError(Request, kBPSCSErrorInternal,
			FString::Printf(TEXT("blueprint '%s' has no SimpleConstructionScript"), *Path));
	}

	// Root name reported separately so caller can disambiguate "DefaultSceneRoot" vs a user-promoted
	// scene component without scanning the components[] array.
	const USCS_Node* DefaultRoot = SCS->GetDefaultSceneRootNode();

	TArray<TSharedPtr<FJsonValue>> Entries;
	if (bRecursive)
	{
		const TArray<USCS_Node*>& AllNodes = SCS->GetAllNodes();
		Entries.Reserve(AllNodes.Num());
		for (USCS_Node* Node : AllNodes)
		{
			if (!Node) { continue; }
			USCS_Node* Parent = SCS->FindParentNode(Node);
			Entries.Add(MakeShared<FJsonValueObject>(BPSCS_BuildSCSNodeSummary(Node, Parent)));
		}
	}
	else
	{
		const TArray<USCS_Node*>& Roots = SCS->GetRootNodes();
		Entries.Reserve(Roots.Num());
		for (USCS_Node* Node : Roots)
		{
			if (!Node) { continue; }
			Entries.Add(MakeShared<FJsonValueObject>(BPSCS_BuildSCSNodeSummary(Node, /*ParentNode*/ nullptr)));
		}
	}

	// No DefaultSceneRootNode — possible if the BP's root was promoted to a user component AND
	// then that component was renamed. Report null; caller infers the root from components[]
	// (the entry whose parent_variable_name is null).
	const int32 EntriesNum = Entries.Num();
	return FMCPJsonBuilder()
		.If(DefaultRoot != nullptr,
			[&](FMCPJsonBuilder& B) { B.Str(TEXT("root"), DefaultRoot->GetVariableName().ToString()); })
		.If(DefaultRoot == nullptr,
			[&](FMCPJsonBuilder& B) { B.Null(TEXT("root")); })
		.Arr(TEXT("components"), MoveTemp(Entries))
		.Num(TEXT("total"), static_cast<double>(EntriesNum))
		.Bool(TEXT("recursive"), bRecursive)
		.BuildSuccess(Request);
}

// ─── bp.set_component_default (Lane A, PIE-guarded) ──────────────────────────────────────────
//
// Args:    { blueprint_path: string, variable_name: string, property_name: string, value: <JSON> }
// Result:  { variable_name, property_name, prior_value, new_value }
//
// Writes a UPROPERTY on the SCS_Node's ComponentTemplate. Uses the same FMCPReflection
// pipeline as bp.set_node_property and component.set_property — JSON values for vectors / enums /
// object refs round-trip identically. The 4-step PreEditChange / Modify / write /
// PostEditChangeProperty contract is enforced via FMCPWritePropertyScope.
//
// Errors:
//   -32027 PIEActive
//   -32602 InvalidParams                   — missing blueprint_path / variable_name / property_name / value
//   -32004 ObjectNotFound                  — BP not found OR variable_name not in SCS
//   -32010 InvalidPath                     — blueprint_path malformed
//   -32031 BlueprintTypeMismatch           — non-UBlueprint asset
//   -32005 PropertyNotFound                — property_name not found on ComponentTemplate class
//   -32006 PropertyTypeMismatch            — write rejected by FMCPReflection::WritePropertyValueAt
//   -32603 Internal                        — BP has no SCS OR SCS_Node has no ComponentTemplate
FMCPResponse Tool_SetComponentDefault(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (BPSCS_IsPIEActive()) { return BPSCS_MakePIEError(Request); }

	FString Path;
	FMCPResponse PathErr;
	if (!BPSCS_RequireBlueprintPath(Request, Path, PathErr)) { return PathErr; }

	FString VariableNameStr;
	if (!Request.Args->TryGetStringField(TEXT("variable_name"), VariableNameStr) || VariableNameStr.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kBPSCSErrorInvalidParams,
			TEXT("missing required string field 'variable_name'"));
	}

	FString PropertyName;
	if (!Request.Args->TryGetStringField(TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kBPSCSErrorInvalidParams,
			TEXT("missing required string field 'property_name'"));
	}

	const TSharedPtr<FJsonValue> ValueField = Request.Args->TryGetField(TEXT("value"));
	if (!ValueField.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kBPSCSErrorInvalidParams,
			TEXT("missing required field 'value' (any JSON value)"));
	}

	FMCPResponse ResolveErr;
	UBlueprint* Blueprint = BPSCS_ResolveBlueprintOrError(Request, Path, ResolveErr);
	if (!Blueprint) { return ResolveErr; }

	USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
	if (!SCS)
	{
		return FMCPToolHelpers::MakeError(Request, kBPSCSErrorInternal,
			FString::Printf(TEXT("blueprint '%s' has no SimpleConstructionScript"), *Path));
	}

	const FName VariableName(*VariableNameStr);
	USCS_Node* TargetNode = SCS->FindSCSNode(VariableName);
	if (!TargetNode)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(
				TEXT("variable_name '%s' not found in SCS of blueprint '%s'"),
				*VariableNameStr, *Path));
	}

	UActorComponent* Template = TargetNode->ComponentTemplate;
	if (!Template)
	{
		return FMCPToolHelpers::MakeError(Request, kBPSCSErrorInternal,
			FString::Printf(
				TEXT("SCS_Node '%s' on blueprint '%s' has no ComponentTemplate (corrupt SCS?)"),
				*VariableNameStr, *Path));
	}

	FProperty* Prop = Template->GetClass()->FindPropertyByName(FName(*PropertyName));
	if (!Prop)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPropertyNotFound,
			FString::Printf(
				TEXT("property '%s' not found on component class '%s' (template for SCS_Node '%s')"),
				*PropertyName, *Template->GetClass()->GetPathName(), *VariableNameStr));
	}

	void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Template);

	// Snapshot the prior value via the same reader marshall.read_property uses.
	TSharedPtr<FJsonValue> PriorValue = FMCPReflection::ReadPropertyValueAt(Prop, ValuePtr);

	// Note (header contract): the edit-const 3-flag gate (CPF_BlueprintReadOnly | CPF_EditConst |
	// CPF_DisableEditOnInstance) is INTENTIONALLY OMITTED here. SCS ComponentTemplate IS the
	// authoring surface — the BP editor's Details panel writes to this same archetype object, and
	// CPF_DisableEditOnInstance is the default for most component properties on the BP archetype.
	// Applying the gate here would falsely reject every legitimate authoring write.

	FString WriteError;
	bool bWriteOk = false;
	{
		FMCPWritePropertyScope Scope(Template, Prop,
			LOCTEXT("BPCSetComponentDefault", "MCP: set SCS component default"));
		bWriteOk = FMCPReflection::WritePropertyValueAt(Prop, ValuePtr, ValueField, Template, WriteError);
	}
	// PostEditChangeProperty has fired on Scope destructor by this point.

	if (!bWriteOk)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPropertyTypeMismatch,
			FString::Printf(TEXT("write rejected on '%s.%s': %s"),
				*Template->GetClass()->GetName(), *PropertyName, *WriteError));
	}

	// Re-read AFTER the write so ``new_value`` reflects any normalisation ImportText_Direct may
	// have applied (enum case-folding, numeric clamps, etc.).
	TSharedPtr<FJsonValue> NewValue = FMCPReflection::ReadPropertyValueAt(Prop, ValuePtr);

	// Default-value writes don't change SCS topology — MarkBlueprintAsModified is sufficient. We
	// avoid StructurallyModified here since it triggers a heavier RegenerateSkeletonOnly compile
	// pass we don't need for a leaf property change.
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	const TSharedRef<FJsonValue> PriorRef = PriorValue.IsValid()
		? PriorValue.ToSharedRef()
		: StaticCastSharedRef<FJsonValue>(MakeShared<FJsonValueNull>());
	const TSharedRef<FJsonValue> NewRef = NewValue.IsValid()
		? NewValue.ToSharedRef()
		: StaticCastSharedRef<FJsonValue>(MakeShared<FJsonValueNull>());
	return FMCPJsonBuilder()
		.Str(TEXT("variable_name"), VariableNameStr)
		.Str(TEXT("property_name"), PropertyName)
		.Field(TEXT("prior_value"), PriorRef)
		.Field(TEXT("new_value"),   NewRef)
		.BuildSuccess(Request);
}

// ─── Registration ────────────────────────────────────────────────────────────────────────────
void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	// Wave F Surface 3 — SCS component CRUD (4 tools, all Lane A). add/remove/set_component_default
	// are PIE-guarded mutators with FScopedTransaction; list is read-only and PIE-safe.
	RegisterTool(TEXT("bp.add_component"),           &Tool_AddComponent,          /*Lane A*/ false);
	RegisterTool(TEXT("bp.remove_component"),        &Tool_RemoveComponent,       /*Lane A*/ false);
	RegisterTool(TEXT("bp.list_components"),         &Tool_ListComponents,        /*Lane A*/ false);
	RegisterTool(TEXT("bp.set_component_default"),   &Tool_SetComponentDefault,   /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("Wave F Surface 3: registered 4 bp.* SCS-component handlers (all Lane A; ")
		TEXT("3 PIE-guarded mutators + 1 read)"));
}

} // namespace FBlueprintComponentTools

#undef LOCTEXT_NAMESPACE

MCP_REGISTER_SURFACE(BlueprintComponentTools, &FBlueprintComponentTools::Register)
