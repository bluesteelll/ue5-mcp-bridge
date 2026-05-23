// Copyright FatumGame. All Rights Reserved.

#include "UMGTools.h"

#include "MCPSurfaceRegistry.h"

#include "FMCPDispatchQueue.h"
#include "MCPAssetLoader.h"
#include "MCPJsonBuilder.h"
#include "MCPMutatorScope.h"
#include "MCPToolHelpers.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPAssetPathUtils.h"
#include "Utils/MCPReflection.h"

#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "Components/PanelWidget.h"
#include "Components/Widget.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"
#include "WidgetBlueprint.h"

#include "AssetToolsModule.h"
#include "Editor.h"
#include "EdGraphSchema_K2.h"
#include "EdGraph/EdGraph.h"
#include "WidgetBlueprintFactory.h"
#include "IAssetTools.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Subsystems/EditorAssetSubsystem.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// UMG_ prefix per the unity-build symbol-collision pattern. Per-surface error constants kept;
	// XX_StampIds/MakeError/MakeSuccessObj/RequireStringField removed in Phase 3 — use
	// FMCPToolHelpers::Xxx from MCPToolHelpers.h. PIE+FScopedTransaction mutator pairs in
	// Tool_AddWidget / RemoveWidget / SetWidgetProperty migrated to FMCPMutatorScope.
	// Tool_CreateWidgetBlueprint and Tool_BindWidgetEvent are PIE-guarded but use no
	// FScopedTransaction so they keep inline PIE checks (no FMCPMutatorScope migration).
	// Tool_AddToViewport / RemoveFromViewport / ListRootWidgets REQUIRE PIE active — these MUST
	// NOT be wrapped in FMCPMutatorScope.
	constexpr int32 kUMGErrorInvalidParams = -32602;
	constexpr int32 kUMGErrorInternal      = -32603;

	/** Cap on the number of widget-name hints listed in the -32039 message body. */
	constexpr int32 kUMGWidgetHintCap = 16;

	// ─── WidgetBlueprint resolution ──────────────────────────────────────────────────────────────

	/**
	 * Load a UWidgetBlueprint by path. Returns nullptr + populated error code/message on failure.
	 * Thin wrapper over FMCPAssetLoader::Load<UWidgetBlueprint> (Phase 3 — was anonymous loader).
	 * Kept as a per-surface alias because Tool_GetWidgetProperty / Tool_ListWidgets each call it
	 * once and the alias preserves the existing wire-error wording.
	 */
	UWidgetBlueprint* UMG_LoadWidgetBlueprintByPath(
		const FString& Path,
		int32& OutErrorCode,
		FString& OutError)
	{
		return FMCPAssetLoader::Load<UWidgetBlueprint>(Path, OutErrorCode, OutError);
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
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("widget_bp_path"), Path, Err)) { return Err; }

	int32 ErrCode = 0;
	FString ErrMsg;
	UWidgetBlueprint* WBP = UMG_LoadWidgetBlueprintByPath(Path, ErrCode, ErrMsg);
	if (!WBP) { return FMCPToolHelpers::MakeError(Request, ErrCode, ErrMsg); }

	UWidgetTree* Tree = WBP->WidgetTree;
	if (!Tree)
	{
		return FMCPToolHelpers::MakeError(Request, kUMGErrorInternal,
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

	return FMCPJsonBuilder()
		.Arr(TEXT("widgets"), MoveTemp(WidgetsArr))
		.BuildSuccess(Request);
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
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("widget_bp_path"), Path, Err))      { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("widget_name"), WidgetName, Err))   { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("property_path"), PropertyPath, Err)) { return Err; }

	int32 ErrCode = 0;
	FString ErrMsg;
	UWidgetBlueprint* WBP = UMG_LoadWidgetBlueprintByPath(Path, ErrCode, ErrMsg);
	if (!WBP) { return FMCPToolHelpers::MakeError(Request, ErrCode, ErrMsg); }

	UWidgetTree* Tree = WBP->WidgetTree;
	if (!Tree)
	{
		return FMCPToolHelpers::MakeError(Request, kUMGErrorInternal,
			FString::Printf(TEXT("WidgetBlueprint '%s' has no WidgetTree"), *Path));
	}

	UWidget* TargetWidget = Tree->FindWidget(FName(*WidgetName));
	if (!TargetWidget)
	{
		// Build a hint list so the caller can correct typos without an extra umg.list_widgets call.
		TArray<UWidget*> AllWidgets;
		UMG_GatherAllWidgets(Tree, AllWidgets);
		const FString Hint = UMG_BuildWidgetNameHint(AllWidgets);
		return FMCPToolHelpers::MakeError(Request, kMCPErrorWidgetNotFound,
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
		return FMCPToolHelpers::MakeError(Request, ResolveErrCode, ResolveErr);
	}
	check(OutLeafProp != nullptr);
	check(OutContainerPtr != nullptr);

	TSharedPtr<FJsonValue> ValueJson = FMCPReflection::ReadPropertyValueAt(OutLeafProp, OutContainerPtr);
	if (!ValueJson.IsValid())
	{
		// ReadPropertyValueAt is documented to NEVER return null (unsupported types emit a wrapped
		// {"_kind":"Unsupported", ...} payload). A null here would indicate an internal contract
		// break — surface as -32603.
		return FMCPToolHelpers::MakeError(Request, kUMGErrorInternal,
			FString::Printf(TEXT("ReadPropertyValueAt returned null for property '%s' on widget '%s'"),
				*PropertyPath, *WidgetName));
	}

	return FMCPJsonBuilder()
		.Field(TEXT("value"), ValueJson.ToSharedRef())
		.Str(TEXT("type"), FMCPReflection::DescribePropertyType(OutLeafProp))
		.BuildSuccess(Request);
}

// ─── umg.create_widget_blueprint — create new UWidgetBlueprint with optional parent class ───
//
// Args:
//   - dest_path:                 string (required)  /Game/.../WBP_New
//   - parent_widget_class_path:  string (optional)  default /Script/UMG.UserWidget. Must be a
//                                                     UUserWidget subclass.
//   - save:                      bool   (optional)  default false
//
// Result: { created, asset_path, generated_class, parent_class, saved }
// PIE-guarded. Lane A.
FMCPResponse Tool_CreateWidgetBlueprint(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (GEditor && GEditor->PlayWorld != nullptr)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPIEActive, kMCPMessagePIEActive);
	}

	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kUMGErrorInvalidParams,
			TEXT("umg.create_widget_blueprint requires args.dest_path"));
	}

	FString DestPathRaw, ParentClassPath;
	if (!Request.Args->TryGetStringField(TEXT("dest_path"), DestPathRaw) || DestPathRaw.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kUMGErrorInvalidParams,
			TEXT("missing required string field 'dest_path'"));
	}
	Request.Args->TryGetStringField(TEXT("parent_widget_class_path"), ParentClassPath);

	UClass* ParentClass = nullptr;
	if (!ParentClassPath.IsEmpty())
	{
		ParentClass = LoadClass<UObject>(nullptr, *ParentClassPath);
		if (!ParentClass)
		{
			const FString WithC = ParentClassPath.EndsWith(TEXT("_C")) ? ParentClassPath : (ParentClassPath + TEXT("_C"));
			ParentClass = LoadClass<UObject>(nullptr, *WithC);
		}
		if (!ParentClass)
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorClassNotFound,
				FString::Printf(TEXT("could not load parent_widget_class_path '%s'"), *ParentClassPath));
		}
		if (!ParentClass->IsChildOf(UUserWidget::StaticClass()))
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorWrongClassFamily,
				FString::Printf(TEXT("parent_widget_class_path '%s' is not a UUserWidget subclass"),
					*ParentClass->GetPathName()));
		}
	}
	else
	{
		ParentClass = UUserWidget::StaticClass();
	}

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
	const FString AssetName   = FPaths::GetBaseFilename(DestPathNorm);

	UWidgetBlueprintFactory* Factory = NewObject<UWidgetBlueprintFactory>();
	Factory->ParentClass = ParentClass;

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	UObject* NewAsset = AssetTools.CreateAsset(AssetName, PackagePath, UWidgetBlueprint::StaticClass(), Factory);
	if (!NewAsset)
	{
		return FMCPToolHelpers::MakeError(Request, kUMGErrorInternal,
			FString::Printf(TEXT("UWidgetBlueprintFactory failed for parent '%s' at '%s'"),
				*ParentClass->GetPathName(), *DestPathNorm));
	}

	UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(NewAsset);
	if (WBP && WBP->GetOutermost()) { WBP->GetOutermost()->MarkPackageDirty(); }

	bool bSaveRequested = false, bSavedOk = false;
	Request.Args->TryGetBoolField(TEXT("save"), bSaveRequested);
	if (bSaveRequested)
	{
		if (UEditorAssetSubsystem* EAS = GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr)
		{
			bSavedOk = EAS->SaveLoadedAsset(NewAsset, /*bOnlyIfIsDirty*/ true);
		}
	}

	return FMCPJsonBuilder()
		.Bool(TEXT("created"), true)
		.Str(TEXT("asset_path"), NewAsset->GetPathName())
		.Str(TEXT("generated_class"),
			WBP && WBP->GeneratedClass ? WBP->GeneratedClass->GetPathName() : FString())
		.Str(TEXT("parent_class"), ParentClass->GetPathName())
		.Bool(TEXT("saved"), bSavedOk)
		.BuildSuccess(Request);
}

// ─── umg.add_widget — instantiate a UWidget into the BP's WidgetTree ────────────────────────
//
// Args:
//   - widget_bp_path:       string (required)  /Game/.../WBP.WBP
//   - widget_class_path:    string (required)  /Script/UMG.Button  etc. Must be UWidget subclass.
//   - widget_name:          string (required)  unique name within the tree
//   - parent_widget_name:   string (optional)  default = root widget. Must be a UPanelWidget.
//
// Result: { added, widget_name, widget_class, parent_widget_name, parent_widget_class }
// PIE-guarded. Lane A.
FMCPResponse Tool_AddWidget(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("UMGAddWidget", "MCP: add widget"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kUMGErrorInvalidParams,
			TEXT("umg.add_widget requires widget_bp_path + widget_class_path + widget_name"));
	}

	FString WBPPath, WidgetClassPath, WidgetName, ParentName;
	if (!Request.Args->TryGetStringField(TEXT("widget_bp_path"), WBPPath) || WBPPath.IsEmpty() ||
		!Request.Args->TryGetStringField(TEXT("widget_class_path"), WidgetClassPath) || WidgetClassPath.IsEmpty() ||
		!Request.Args->TryGetStringField(TEXT("widget_name"), WidgetName) || WidgetName.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kUMGErrorInvalidParams,
			TEXT("missing widget_bp_path / widget_class_path / widget_name"));
	}
	Request.Args->TryGetStringField(TEXT("parent_widget_name"), ParentName);

	UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *WBPPath);
	if (!WBP || !WBP->WidgetTree)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("could not load WidgetBlueprint '%s'"), *WBPPath));
	}

	UClass* WidgetClass = LoadClass<UWidget>(nullptr, *WidgetClassPath);
	if (!WidgetClass)
	{
		const FString WithC = WidgetClassPath.EndsWith(TEXT("_C")) ? WidgetClassPath : (WidgetClassPath + TEXT("_C"));
		WidgetClass = LoadClass<UWidget>(nullptr, *WithC);
	}
	if (!WidgetClass || !WidgetClass->IsChildOf(UWidget::StaticClass()))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorClassNotFound,
			FString::Printf(TEXT("'%s' is not a UWidget subclass"), *WidgetClassPath));
	}
	if (WidgetClass->HasAnyClassFlags(CLASS_Abstract))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorClassAbstract,
			FString::Printf(TEXT("widget class '%s' is abstract"), *WidgetClass->GetPathName()));
	}

	const FName WidgetFName(*WidgetName);
	if (WBP->WidgetTree->FindWidget(WidgetFName))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPathInUse,
			FString::Printf(TEXT("widget '%s' already exists in tree"), *WidgetName));
	}

	// FMCPMutatorScope at function-top owns the FScopedTransaction lifetime.
	WBP->Modify();
	WBP->WidgetTree->Modify();

	UWidget* NewWidget = WBP->WidgetTree->ConstructWidget<UWidget>(WidgetClass, WidgetFName);
	if (!NewWidget)
	{
		return FMCPToolHelpers::MakeError(Request, kUMGErrorInternal,
			FString::Printf(TEXT("WidgetTree::ConstructWidget returned null for class '%s'"),
				*WidgetClass->GetPathName()));
	}

	// Attach: either to specified parent or to RootWidget. If RootWidget is null, set new widget
	// AS the root.
	UWidget* ParentWidget = nullptr;
	UPanelWidget* ParentPanel = nullptr;
	FString ParentResolved;
	if (!ParentName.IsEmpty())
	{
		ParentWidget = WBP->WidgetTree->FindWidget(FName(*ParentName));
		if (!ParentWidget)
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
				FString::Printf(TEXT("parent_widget_name '%s' not found in tree"), *ParentName));
		}
		ParentPanel = Cast<UPanelWidget>(ParentWidget);
		if (!ParentPanel)
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorWrongClassFamily,
				FString::Printf(TEXT("parent '%s' is class '%s' which is not a UPanelWidget — cannot have children"),
					*ParentName, *ParentWidget->GetClass()->GetPathName()));
		}
		ParentPanel->AddChild(NewWidget);
		ParentResolved = ParentName;
	}
	else
	{
		if (WBP->WidgetTree->RootWidget == nullptr)
		{
			WBP->WidgetTree->RootWidget = NewWidget;
			ParentResolved = TEXT("<root>");
		}
		else
		{
			ParentPanel = Cast<UPanelWidget>(WBP->WidgetTree->RootWidget);
			if (!ParentPanel)
			{
				return FMCPToolHelpers::MakeError(Request, kMCPErrorWrongClassFamily,
					FString::Printf(TEXT("root widget '%s' (class '%s') is not a UPanelWidget; "
						"pass parent_widget_name explicitly to specify a panel parent"),
						*WBP->WidgetTree->RootWidget->GetName(),
						*WBP->WidgetTree->RootWidget->GetClass()->GetPathName()));
			}
			ParentPanel->AddChild(NewWidget);
			ParentResolved = WBP->WidgetTree->RootWidget->GetName();
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);

	return FMCPJsonBuilder()
		.Bool(TEXT("added"), true)
		.Str(TEXT("widget_name"), NewWidget->GetName())
		.Str(TEXT("widget_class"), WidgetClass->GetPathName())
		.Str(TEXT("parent_widget_name"), ParentResolved)
		.Str(TEXT("parent_widget_class"),
			ParentPanel ? ParentPanel->GetClass()->GetPathName() : FString(TEXT("<root>")))
		.BuildSuccess(Request);
}

// ─── umg.remove_widget — remove widget from BP's WidgetTree ─────────────────────────────────
FMCPResponse Tool_RemoveWidget(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("UMGRemoveWidget", "MCP: remove widget"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kUMGErrorInvalidParams,
			TEXT("umg.remove_widget requires widget_bp_path + widget_name"));
	}

	FString WBPPath, WidgetName;
	if (!Request.Args->TryGetStringField(TEXT("widget_bp_path"), WBPPath) ||
		!Request.Args->TryGetStringField(TEXT("widget_name"), WidgetName))
	{
		return FMCPToolHelpers::MakeError(Request, kUMGErrorInvalidParams,
			TEXT("missing widget_bp_path or widget_name"));
	}

	UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *WBPPath);
	if (!WBP || !WBP->WidgetTree)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("could not load WidgetBlueprint '%s'"), *WBPPath));
	}

	UWidget* W = WBP->WidgetTree->FindWidget(FName(*WidgetName));
	if (!W)
	{
		return FMCPJsonBuilder()
			.Bool(TEXT("removed"), false)
			.Bool(TEXT("was_present"), false)
			.BuildSuccess(Request);
	}

	// FMCPMutatorScope at function-top owns the FScopedTransaction lifetime.
	WBP->Modify();
	WBP->WidgetTree->Modify();
	const bool bRemoved = WBP->WidgetTree->RemoveWidget(W);
	FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);

	return FMCPJsonBuilder()
		.Bool(TEXT("removed"), bRemoved)
		.Bool(TEXT("was_present"), true)
		.BuildSuccess(Request);
}

// ─── umg.set_widget_property — write any UPROPERTY on a widget in the BP's tree ────────────
FMCPResponse Tool_SetWidgetProperty(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("UMGSetWidgetProp", "MCP: set widget property"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kUMGErrorInvalidParams,
			TEXT("umg.set_widget_property requires widget_bp_path + widget_name + property_path + value"));
	}

	FString WBPPath, WidgetName, PropertyPath;
	const TSharedPtr<FJsonValue> ValueField = Request.Args->TryGetField(TEXT("value"));
	if (!Request.Args->TryGetStringField(TEXT("widget_bp_path"), WBPPath) ||
		!Request.Args->TryGetStringField(TEXT("widget_name"),   WidgetName) ||
		!Request.Args->TryGetStringField(TEXT("property_path"), PropertyPath) ||
		!ValueField.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kUMGErrorInvalidParams,
			TEXT("missing widget_bp_path / widget_name / property_path / value"));
	}

	UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *WBPPath);
	if (!WBP || !WBP->WidgetTree)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("could not load WidgetBlueprint '%s'"), *WBPPath));
	}
	UWidget* W = WBP->WidgetTree->FindWidget(FName(*WidgetName));
	if (!W)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("widget '%s' not found in tree"), *WidgetName));
	}

	UObject* Container = nullptr;
	void* ValuePtr = nullptr;
	FProperty* Prop = nullptr;
	int32 ErrCode = 0;
	FString ErrMsg;
	if (!FMCPReflection::ResolvePropertyPath(W, PropertyPath, Container, ValuePtr, Prop, ErrCode, ErrMsg))
	{
		return FMCPToolHelpers::MakeError(Request, ErrCode, ErrMsg);
	}

	// TryGetBoolField returns false on missing OR wrong type, leaving OutValue default (false).
	// Prior pattern (HasField + GetBoolField) crashes if field exists but wrong JSON type
	// (e.g. caller passes `bypass_readonly: "yes"` as a string instead of a boolean).
	bool bBypass = false;
	Request.Args->TryGetBoolField(TEXT("bypass_readonly"), bBypass);
	const uint64 Flags = Prop->PropertyFlags;
	if (!bBypass && (Flags & (CPF_EditConst | CPF_DisableEditOnInstance)))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPropertyAccessDenied,
			FString::Printf(TEXT("property '%s' is read-only; pass bypass_readonly=true to override"),
				*Prop->GetName()));
	}

	// FMCPMutatorScope at function-top owns the FScopedTransaction lifetime.
	W->Modify();
	WBP->Modify();
	FString WriteErr;
	if (!FMCPReflection::WritePropertyValueAt(Prop, ValuePtr, ValueField, Container, WriteErr))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPropertyTypeMismatch,
			FString::Printf(TEXT("write rejected: %s"), *WriteErr));
	}
	FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);

	return FMCPJsonBuilder()
		.Bool(TEXT("applied"), true)
		.Str(TEXT("property"), Prop->GetName())
		.BuildSuccess(Request);
}

// ─── umg.bind_widget_event — bind multicast delegate event on a widget to a UFUNCTION ──────
//
// Args:
//   - widget_bp_path:    string (required)
//   - widget_name:       string (required)
//   - event_name:        string (required)   delegate UPROPERTY name (e.g. "OnClicked")
//   - function_name:     string (required)   target function on the WidgetBP (must exist OR
//                                              create_function_if_missing=true)
//   - create_function_if_missing: bool (optional)  default false. true → auto-create empty
//                                                                 function graph if missing.
//
// Result: { bound, event_name, function_name }
// PIE-guarded. Lane A.
FMCPResponse Tool_BindWidgetEvent(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (GEditor && GEditor->PlayWorld != nullptr)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPIEActive, kMCPMessagePIEActive);
	}

	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kUMGErrorInvalidParams,
			TEXT("umg.bind_widget_event requires widget_bp_path + widget_name + event_name + function_name"));
	}

	FString WBPPath, WidgetName, EventName, FunctionName;
	if (!Request.Args->TryGetStringField(TEXT("widget_bp_path"), WBPPath) ||
		!Request.Args->TryGetStringField(TEXT("widget_name"),    WidgetName) ||
		!Request.Args->TryGetStringField(TEXT("event_name"),     EventName) ||
		!Request.Args->TryGetStringField(TEXT("function_name"),  FunctionName))
	{
		return FMCPToolHelpers::MakeError(Request, kUMGErrorInvalidParams,
			TEXT("missing widget_bp_path / widget_name / event_name / function_name"));
	}

	UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *WBPPath);
	if (!WBP || !WBP->WidgetTree)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("could not load WidgetBlueprint '%s'"), *WBPPath));
	}
	UWidget* W = WBP->WidgetTree->FindWidget(FName(*WidgetName));
	if (!W)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("widget '%s' not found"), *WidgetName));
	}

	// Verify delegate property exists on widget class.
	FProperty* DelegateProp = W->GetClass()->FindPropertyByName(FName(*EventName));
	if (!DelegateProp || !DelegateProp->IsA<FMulticastDelegateProperty>())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPropertyNotFound,
			FString::Printf(TEXT("event '%s' is not a multicast delegate UPROPERTY on '%s'"),
				*EventName, *W->GetClass()->GetPathName()));
	}

	bool bCreateIfMissing = false;
	Request.Args->TryGetBoolField(TEXT("create_function_if_missing"), bCreateIfMissing);

	// Check if target function exists; create empty graph if requested.
	UEdGraph* TargetGraph = nullptr;
	for (UEdGraph* G : WBP->FunctionGraphs)
	{
		if (G && G->GetFName() == FName(*FunctionName)) { TargetGraph = G; break; }
	}
	if (!TargetGraph)
	{
		if (!bCreateIfMissing)
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorPropertyNotFound,
				FString::Printf(TEXT("function '%s' not found on '%s'; pass create_function_if_missing=true to auto-create"),
					*FunctionName, *WBPPath));
		}
		TargetGraph = FBlueprintEditorUtils::CreateNewGraph(WBP, FName(*FunctionName),
			UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		FBlueprintEditorUtils::AddFunctionGraph<UClass>(WBP, TargetGraph, /*bIsUserCreated*/ true,
			/*SignatureFromClass*/ nullptr);
	}

	// Add binding to WBP->Bindings (UWidgetBlueprint stores these — survives compile).
	FDelegateEditorBinding Binding;
	Binding.ObjectName       = WidgetName;
	Binding.PropertyName     = FName(*EventName);
	Binding.FunctionName     = FName(*FunctionName);
	Binding.MemberGuid       = FGuid::NewGuid();
	Binding.SourcePath       = FEditorPropertyPath();

	// Remove pre-existing same-binding (idempotency).
	WBP->Bindings.RemoveAll([&](const FDelegateEditorBinding& B)
	{
		return B.ObjectName == WidgetName && B.PropertyName == FName(*EventName);
	});
	WBP->Bindings.Add(Binding);
	FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);

	return FMCPJsonBuilder()
		.Bool(TEXT("bound"), true)
		.Str(TEXT("event_name"), EventName)
		.Str(TEXT("function_name"), FunctionName)
		.Str(TEXT("widget_name"), WidgetName)
		.BuildSuccess(Request);
}

// ─── umg.add_to_viewport — instantiate widget at runtime and add to viewport (PIE only) ─────
FMCPResponse Tool_AddToViewport(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!GEditor || !GEditor->PlayWorld)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPIEActive,
			TEXT("umg.add_to_viewport requires PIE running (GEditor->PlayWorld != null)"));
	}

	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kUMGErrorInvalidParams,
			TEXT("umg.add_to_viewport requires widget_bp_path"));
	}

	FString WBPPath;
	if (!Request.Args->TryGetStringField(TEXT("widget_bp_path"), WBPPath))
	{
		return FMCPToolHelpers::MakeError(Request, kUMGErrorInvalidParams, TEXT("missing widget_bp_path"));
	}
	int32 ZOrder = 0;
	if (Request.Args->HasField(TEXT("z_order")))
	{
		Request.Args->TryGetNumberField(TEXT("z_order"), ZOrder);
	}

	// Resolve generated class.
	UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *WBPPath);
	if (!BP || !BP->GeneratedClass)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("could not load WidgetBlueprint '%s' (or no GeneratedClass)"), *WBPPath));
	}
	if (!BP->GeneratedClass->IsChildOf(UUserWidget::StaticClass()))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorWrongClassFamily,
			FString::Printf(TEXT("class '%s' is not a UUserWidget"), *BP->GeneratedClass->GetPathName()));
	}

	UWorld* PIEWorld = GEditor->PlayWorld;
	// NewObject + Initialize is the C++-direct equivalent of the CreateWidget<T> helper template
	// (which has Owner-type template-deduction quirks under MCP's unity-build dispatch path).
	UUserWidget* W = NewObject<UUserWidget>(PIEWorld, BP->GeneratedClass);
	if (!W)
	{
		return FMCPToolHelpers::MakeError(Request, kUMGErrorInternal, TEXT("NewObject returned null for UserWidget"));
	}
	W->Initialize();
	W->AddToViewport(ZOrder);

	return FMCPJsonBuilder()
		.Bool(TEXT("added"), true)
		.Str(TEXT("widget_path"), W->GetPathName())
		.Str(TEXT("class_path"), BP->GeneratedClass->GetPathName())
		.Num(TEXT("z_order"), static_cast<double>(ZOrder))
		.BuildSuccess(Request);
}

// ─── umg.remove_from_viewport — remove a runtime widget instance from viewport (PIE only) ──
FMCPResponse Tool_RemoveFromViewport(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!GEditor || !GEditor->PlayWorld)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPIEActive,
			TEXT("umg.remove_from_viewport requires PIE running"));
	}

	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kUMGErrorInvalidParams, TEXT("requires widget_path"));
	}

	FString WidgetPath;
	if (!Request.Args->TryGetStringField(TEXT("widget_path"), WidgetPath))
	{
		return FMCPToolHelpers::MakeError(Request, kUMGErrorInvalidParams, TEXT("missing widget_path"));
	}

	UUserWidget* W = LoadObject<UUserWidget>(nullptr, *WidgetPath);
	if (!W)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("widget '%s' not found"), *WidgetPath));
	}
	const bool bWasInViewport = W->IsInViewport();
	W->RemoveFromParent();

	return FMCPJsonBuilder()
		.Bool(TEXT("removed"), true)
		.Bool(TEXT("was_in_viewport"), bWasInViewport)
		.BuildSuccess(Request);
}

// ─── umg.list_root_widgets — enumerate widgets currently in viewport (PIE only) ────────────
FMCPResponse Tool_ListRootWidgets(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!GEditor || !GEditor->PlayWorld)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPIEActive,
			TEXT("umg.list_root_widgets requires PIE running"));
	}

	TArray<TSharedPtr<FJsonValue>> Widgets;
	for (TObjectIterator<UUserWidget> It; It; ++It)
	{
		UUserWidget* W = *It;
		if (!W || !W->IsInViewport()) { continue; }
		// Only PIE-world widgets, not editor-world remnants
		if (W->GetWorld() != GEditor->PlayWorld) { continue; }
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("widget_path"), W->GetPathName());
		Obj->SetStringField(TEXT("class_path"), W->GetClass()->GetPathName());
		Obj->SetBoolField(TEXT("is_visible"), W->GetVisibility() != ESlateVisibility::Collapsed && W->GetVisibility() != ESlateVisibility::Hidden);
		Widgets.Add(MakeShared<FJsonValueObject>(Obj));
	}

	const int32 WidgetCount = Widgets.Num();
	return FMCPJsonBuilder()
		.Arr(TEXT("widgets"), MoveTemp(Widgets))
		.Num(TEXT("count"), static_cast<double>(WidgetCount))
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

	// Phase 5 Chunk C — reads.
	RegisterTool(TEXT("umg.list_widgets"),        &Tool_ListWidgets,        /*Lane A*/ false);
	RegisterTool(TEXT("umg.get_widget_property"), &Tool_GetWidgetProperty,  /*Lane A*/ false);

	// 2026-05 UI/HUD full surface — creation, tree manipulation, event binding, runtime.
	RegisterTool(TEXT("umg.create_widget_blueprint"), &Tool_CreateWidgetBlueprint, /*Lane A*/ false);
	RegisterTool(TEXT("umg.add_widget"),              &Tool_AddWidget,             /*Lane A*/ false);
	RegisterTool(TEXT("umg.remove_widget"),           &Tool_RemoveWidget,          /*Lane A*/ false);
	RegisterTool(TEXT("umg.set_widget_property"),     &Tool_SetWidgetProperty,     /*Lane A*/ false);
	RegisterTool(TEXT("umg.bind_widget_event"),       &Tool_BindWidgetEvent,       /*Lane A*/ false);
	RegisterTool(TEXT("umg.add_to_viewport"),         &Tool_AddToViewport,         /*Lane A*/ false);
	RegisterTool(TEXT("umg.remove_from_viewport"),    &Tool_RemoveFromViewport,    /*Lane A*/ false);
	RegisterTool(TEXT("umg.list_root_widgets"),       &Tool_ListRootWidgets,       /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("UMG surface: registered 11 umg.* handlers (2 reads + 1 creator + 4 tree manip + 3 runtime + 1 event binding, all Lane A)"));
}

} // namespace FUMGTools

#undef LOCTEXT_NAMESPACE

MCP_REGISTER_SURFACE(UMGTools, &FUMGTools::Register)
