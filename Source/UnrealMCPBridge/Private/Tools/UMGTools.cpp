// Copyright FatumGame. All Rights Reserved.

#include "UMGTools.h"

#include "FMCPDispatchQueue.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPAssetPathUtils.h"
#include "Utils/MCPReflection.h"

#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "Components/PanelWidget.h"
#include "Components/Widget.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "WidgetBlueprint.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// UMG_ prefix per the unity-build symbol-collision pattern (MakeError/MakeSuccess clash with
	// UE's global ValueOrError templates AND between sibling tool TUs in the unity build).
	constexpr int32 kUMGErrorInvalidParams = -32602;
	constexpr int32 kUMGErrorInternal      = -32603;

	/** Cap on the number of widget-name hints listed in the -32039 message body. */
	constexpr int32 kUMGWidgetHintCap = 16;

	void UMG_StampIds(const FMCPRequest& Request, FMCPResponse& Response)
	{
		Response.RequestId = Request.RequestId;
		Response.OriginalIdString = Request.OriginalIdString;
	}

	FMCPResponse UMG_MakeError(const FMCPRequest& Request, int32 Code, const FString& Message)
	{
		FMCPResponse R;
		UMG_StampIds(Request, R);
		R.bIsError = true;
		R.ErrorCode = Code;
		R.ErrorMessage = Message;
		return R;
	}

	FMCPResponse UMG_MakeSuccessObj(const FMCPRequest& Request, TSharedPtr<FJsonObject> Result)
	{
		FMCPResponse R;
		UMG_StampIds(Request, R);
		R.bIsError = false;
		R.Result = MakeShared<FJsonValueObject>(MoveTemp(Result));
		return R;
	}

	// ─── arg parsing helpers ─────────────────────────────────────────────────────────────────────

	bool UMG_RequireStringField(const FMCPRequest& Request, const TCHAR* FieldName,
		FString& OutValue, FMCPResponse& OutError)
	{
		if (!Request.Args.IsValid())
		{
			OutError = UMG_MakeError(Request, kUMGErrorInvalidParams, TEXT("missing args object"));
			return false;
		}
		if (!Request.Args->TryGetStringField(FieldName, OutValue) || OutValue.IsEmpty())
		{
			OutError = UMG_MakeError(Request, kUMGErrorInvalidParams,
				FString::Printf(TEXT("missing required string field '%s'"), FieldName));
			return false;
		}
		return true;
	}

	// ─── WidgetBlueprint resolution ──────────────────────────────────────────────────────────────

	/**
	 * Load a UWidgetBlueprint by path. Returns nullptr + populated error code/message on failure.
	 *
	 * Error map:
	 *   -32010 InvalidPath          — empty path, backslashes, unknown mount
	 *   -32004 ObjectNotFound       — LoadObject returned null
	 *   -32011 WrongClass           — loaded asset isn't a UWidgetBlueprint
	 *
	 * Path normalisation mirrors FMCPMaterialUtils::LoadMaterialInterfaceByPath — try package-name
	 * form first, then object-path form, so callers can pass either ``/Game/Foo/WBP_Foo`` or
	 * ``/Game/Foo/WBP_Foo.WBP_Foo``.
	 */
	UWidgetBlueprint* UMG_LoadWidgetBlueprintByPath(
		const FString& Path,
		int32& OutErrorCode,
		FString& OutError)
	{
		if (Path.IsEmpty())
		{
			OutErrorCode = kMCPErrorInvalidPath;
			OutError = TEXT("widget_bp_path is empty");
			return nullptr;
		}

		const FString Normalised = FMCPAssetPathUtils::Normalize(Path);
		if (Normalised.IsEmpty() || !FMCPAssetPathUtils::IsValidGameOrPlugin(Normalised))
		{
			OutErrorCode = kMCPErrorInvalidPath;
			OutError = FString::Printf(
				TEXT("widget_bp_path '%s' is malformed or references an unknown mount point"),
				*Path);
			return nullptr;
		}

		UObject* Loaded = LoadObject<UObject>(nullptr, *Normalised);
		if (!Loaded)
		{
			const FString ObjectPath = FMCPAssetPathUtils::ToObjectPath(Normalised);
			if (!ObjectPath.IsEmpty() && ObjectPath != Normalised)
			{
				Loaded = LoadObject<UObject>(nullptr, *ObjectPath);
			}
		}
		if (!Loaded)
		{
			OutErrorCode = kMCPErrorObjectNotFound;
			OutError = FString::Printf(
				TEXT("widget_bp_path '%s' could not be loaded (no asset found)"),
				*Path);
			return nullptr;
		}

		UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(Loaded);
		if (!WBP)
		{
			OutErrorCode = kMCPErrorWrongClass;
			OutError = FString::Printf(
				TEXT("widget_bp_path '%s' is class '%s'; expected UWidgetBlueprint"),
				*Path, *Loaded->GetClass()->GetPathName());
			return nullptr;
		}
		return WBP;
	}

	// ─── WidgetTree enumeration ──────────────────────────────────────────────────────────────────

	/** Collect ALL widgets in a tree (root + descendants, follows named-slot bindings). */
	void UMG_GatherAllWidgets(UWidgetTree* Tree, TArray<UWidget*>& OutWidgets)
	{
		check(Tree != nullptr);
		// ForEachWidgetAndDescendants includes named-slot contents AND foreign trees from nested
		// user widgets. For introspection we want the SAME tree only, so use ForEachWidget which
		// stops at foreign widget-tree boundaries (matches what the editor's "Widget Hierarchy"
		// panel shows for a single WBP).
		Tree->ForEachWidget([&OutWidgets](UWidget* W)
		{
			if (W) { OutWidgets.Add(W); }
		});
	}

	/** Compact "top-level widget names" list for -32039 messages. Capped at kUMGWidgetHintCap. */
	FString UMG_BuildWidgetNameHint(const TArray<UWidget*>& Widgets)
	{
		FString Out;
		const int32 ListCap = FMath::Min(kUMGWidgetHintCap, Widgets.Num());
		for (int32 i = 0; i < ListCap; ++i)
		{
			if (!Out.IsEmpty()) { Out += TEXT(", "); }
			Out += FString::Printf(TEXT("'%s'"), *Widgets[i]->GetName());
		}
		if (Widgets.Num() > ListCap)
		{
			Out += FString::Printf(TEXT(" (and %d more)"), Widgets.Num() - ListCap);
		}
		return Out;
	}
} // namespace

namespace FUMGTools
{

// ─── umg.list_widgets ──────────────────────────────────────────────────────────────────────────
//
// Args:    { widget_bp_path: string }
// Result:  { widgets: [{ name, class, parent, is_variable }, ...] }
//
// Errors:
//   -32602 InvalidParams        missing widget_bp_path
//   -32010 InvalidPath          path malformed / unknown mount
//   -32004 ObjectNotFound       LoadObject returned null
//   -32011 WrongClass           asset is not UWidgetBlueprint
//   -32603 Internal             WidgetTree was null (corrupted WBP — shouldn't happen on valid asset)
//
// Per-widget shape:
//   - name        : UWidget::GetName() (the FName the editor displays in the hierarchy panel)
//   - class       : full UClass path of the widget (e.g. /Script/UMG.TextBlock)
//   - parent      : parent widget's name (null for the RootWidget; non-null for everything else)
//   - is_variable : UWidget::bIsVariable (the "Is Variable" editor checkbox state)
FMCPResponse Tool_ListWidgets(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString Path;
	FMCPResponse Err;
	if (!UMG_RequireStringField(Request, TEXT("widget_bp_path"), Path, Err)) { return Err; }

	int32 ErrCode = 0;
	FString ErrMsg;
	UWidgetBlueprint* WBP = UMG_LoadWidgetBlueprintByPath(Path, ErrCode, ErrMsg);
	if (!WBP) { return UMG_MakeError(Request, ErrCode, ErrMsg); }

	UWidgetTree* Tree = WBP->WidgetTree;
	if (!Tree)
	{
		return UMG_MakeError(Request, kUMGErrorInternal,
			FString::Printf(TEXT("WidgetBlueprint '%s' has no WidgetTree (corrupted asset?)"), *Path));
	}

	TArray<UWidget*> AllWidgets;
	UMG_GatherAllWidgets(Tree, AllWidgets);

	TArray<TSharedPtr<FJsonValue>> WidgetsArr;
	WidgetsArr.Reserve(AllWidgets.Num());
	for (UWidget* W : AllWidgets)
	{
		if (!W) { continue; }
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), W->GetName());
		Obj->SetStringField(TEXT("class"), W->GetClass()->GetPathName());
		// Parent lookup — root widget returns null; everything else has a parent panel.
		int32 ChildIdx = 0;
		UPanelWidget* Parent = UWidgetTree::FindWidgetParent(W, ChildIdx);
		if (Parent)
		{
			Obj->SetStringField(TEXT("parent"), Parent->GetName());
		}
		else
		{
			Obj->SetField(TEXT("parent"), MakeShared<FJsonValueNull>());
		}
		Obj->SetBoolField(TEXT("is_variable"), W->bIsVariable != 0);
		WidgetsArr.Add(MakeShared<FJsonValueObject>(Obj));
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetArrayField(TEXT("widgets"), WidgetsArr);
	return UMG_MakeSuccessObj(Request, Out);
}

// ─── umg.get_widget_property ───────────────────────────────────────────────────────────────────
//
// Args:    { widget_bp_path: string, widget_name: string, property_path: string }
// Result:  { value: <typed>, type: string }
//
// Errors:
//   -32602 InvalidParams        missing required field
//   -32010 InvalidPath          widget_bp_path malformed / unknown mount
//   -32004 ObjectNotFound       LoadObject returned null
//   -32011 WrongClass           asset is not UWidgetBlueprint
//   -32039 WidgetNotFound       widget_name not in WidgetTree (message lists top-level names)
//   -32005 PropertyNotFound     property_path segment missing on widget class
//   -32006 PropertyTypeMismatch property_path traverses a non-object/non-struct segment
//   -32025 PropertyPathTooDeep  property_path nesting > 16 segments
//   -32026 PropertyIndexOOB     [N] index past array bounds
//   -32603 Internal             WidgetTree null
//
// Operates on the **CDO** of the widget — reads the editor-time DEFAULT value as designed in the
// WBP, NOT the runtime per-instance value. Equivalent to inspecting the property in the WBP's
// Designer panel. For runtime-instance reads, callers need ``actor.get_property`` against a live
// UMG host actor (out of scope for Chunk C).
FMCPResponse Tool_GetWidgetProperty(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString Path, WidgetName, PropertyPath;
	FMCPResponse Err;
	if (!UMG_RequireStringField(Request, TEXT("widget_bp_path"), Path, Err))      { return Err; }
	if (!UMG_RequireStringField(Request, TEXT("widget_name"), WidgetName, Err))   { return Err; }
	if (!UMG_RequireStringField(Request, TEXT("property_path"), PropertyPath, Err)) { return Err; }

	int32 ErrCode = 0;
	FString ErrMsg;
	UWidgetBlueprint* WBP = UMG_LoadWidgetBlueprintByPath(Path, ErrCode, ErrMsg);
	if (!WBP) { return UMG_MakeError(Request, ErrCode, ErrMsg); }

	UWidgetTree* Tree = WBP->WidgetTree;
	if (!Tree)
	{
		return UMG_MakeError(Request, kUMGErrorInternal,
			FString::Printf(TEXT("WidgetBlueprint '%s' has no WidgetTree"), *Path));
	}

	UWidget* TargetWidget = Tree->FindWidget(FName(*WidgetName));
	if (!TargetWidget)
	{
		// Build a hint list so the caller can correct typos without an extra umg.list_widgets call.
		TArray<UWidget*> AllWidgets;
		UMG_GatherAllWidgets(Tree, AllWidgets);
		const FString Hint = UMG_BuildWidgetNameHint(AllWidgets);
		return UMG_MakeError(Request, kMCPErrorWidgetNotFound,
			FString::Printf(
				TEXT("widget '%s' not found in WidgetTree of '%s'; available: %s"),
				*WidgetName, *Path, *Hint));
	}

	// Delegate to the Tier-1 marshalling pipeline. Same path-walking + JSON serialisation used by
	// marshall.read_property / actor.get_property — keeps the wire shape consistent.
	UObject* OutContainer = nullptr;
	void* OutContainerPtr = nullptr;
	FProperty* OutLeafProp = nullptr;
	int32 ResolveErrCode = 0;
	FString ResolveErr;
	if (!FMCPReflection::ResolvePropertyPath(
			TargetWidget, PropertyPath,
			OutContainer, OutContainerPtr, OutLeafProp,
			ResolveErrCode, ResolveErr))
	{
		return UMG_MakeError(Request, ResolveErrCode, ResolveErr);
	}
	check(OutLeafProp != nullptr);
	check(OutContainerPtr != nullptr);

	TSharedPtr<FJsonValue> ValueJson = FMCPReflection::ReadPropertyValueAt(OutLeafProp, OutContainerPtr);
	if (!ValueJson.IsValid())
	{
		// ReadPropertyValueAt is documented to NEVER return null (unsupported types emit a wrapped
		// {"_kind":"Unsupported", ...} payload). A null here would indicate an internal contract
		// break — surface as -32603.
		return UMG_MakeError(Request, kUMGErrorInternal,
			FString::Printf(TEXT("ReadPropertyValueAt returned null for property '%s' on widget '%s'"),
				*PropertyPath, *WidgetName));
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetField(TEXT("value"), ValueJson);
	Out->SetStringField(TEXT("type"), FMCPReflection::DescribePropertyType(OutLeafProp));
	return UMG_MakeSuccessObj(Request, Out);
}

// ─── Registration ──────────────────────────────────────────────────────────────────────────────
void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	RegisterTool(TEXT("umg.list_widgets"),        &Tool_ListWidgets,        /*Lane A*/ false);
	RegisterTool(TEXT("umg.get_widget_property"), &Tool_GetWidgetProperty,  /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("Phase 5 Chunk C (UMG): registered 2 umg.* handlers (list_widgets + get_widget_property, all Lane A)"));
}

} // namespace FUMGTools

#undef LOCTEXT_NAMESPACE
