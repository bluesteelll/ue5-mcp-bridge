// Copyright FatumGame. All Rights Reserved.

#include "InputTools.h"

#include "FMCPDispatchQueue.h"
#include "MCPAssetLoader.h"
#include "MCPToolHelpers.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPActorPathUtils.h"
#include "Utils/MCPAssetPathUtils.h"
#include "Utils/MCPPageCursor.h"
#include "Utils/MCPWorldContext.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Editor.h"
#include "EnhancedActionKeyMapping.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "GameFramework/PlayerController.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "InputTriggers.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	/**
	 * Paginated asset enumeration helper. Mirrors AnimTools::Tool_ListSequences /
	 * MeshTools::Tool_List shape so the wire contract is identical across surfaces — caller
	 * sees ``{items: [{ asset_path, name }], total_known, next_page_token? }`` with the array
	 * field name supplied by the tool.
	 */
	FMCPResponse INP_ListAssetsOfClass(
		const FMCPRequest& Request,
		UClass* AssetClass,
		const TCHAR* ArrayFieldName)
	{
		check(IsInGameThread());
		check(AssetClass != nullptr);

		FString PathPrefix;
		if (Request.Args.IsValid()) { Request.Args->TryGetStringField(TEXT("path_prefix"), PathPrefix); }

		int32 PageSize = 100;
		if (Request.Args.IsValid()) { Request.Args->TryGetNumberField(TEXT("page_size"), PageSize); }
		PageSize = FMath::Clamp(PageSize, 1, 1000);

		FString PageToken;
		if (Request.Args.IsValid()) { Request.Args->TryGetStringField(TEXT("page_token"), PageToken); }

		// FilterHash so cursor staleness is detectable across pages.
		const uint32 FilterHash = GetTypeHash(PathPrefix);

		IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		FARFilter Filter;
		Filter.ClassPaths.Add(AssetClass->GetClassPathName());
		Filter.bRecursiveClasses = false;
		Filter.bRecursivePaths   = true;
		if (!PathPrefix.IsEmpty())
		{
			Filter.PackagePaths.Add(*PathPrefix);
		}
		TArray<FAssetData> Assets;
		AR.GetAssets(Filter, Assets);

		// Stable sort by ObjectPath (keyset pagination sort key).
		Assets.Sort([](const FAssetData& A, const FAssetData& B)
		{
			return A.GetSoftObjectPath().ToString() < B.GetSoftObjectPath().ToString();
		});

		// Decode + validate cursor.
		int32 StartIdx = 0;
		FMCPPageCursor InCursor;
		if (!PageToken.IsEmpty())
		{
			FString DecodeErr;
			if (!FMCPPageCursorUtils::Decode(PageToken, InCursor, DecodeErr))
			{
				return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
					FString::Printf(TEXT("invalid page_token: %s"), *DecodeErr));
			}
			if (!FMCPPageCursorUtils::ValidateAgainstFilter(InCursor, FilterHash))
			{
				return FMCPToolHelpers::MakeError(Request, kMCPErrorStaleCursor,
					TEXT("filter mutated between pages (path_prefix changed); restart pagination"));
			}
			while (StartIdx < Assets.Num() &&
			       Assets[StartIdx].GetSoftObjectPath().ToString() <= InCursor.LastAssetPath)
			{
				++StartIdx;
			}
		}

		TArray<TSharedPtr<FJsonValue>> ItemArr;
		const int32 EndIdx = FMath::Min(StartIdx + PageSize, Assets.Num());
		ItemArr.Reserve(EndIdx - StartIdx);
		for (int32 i = StartIdx; i < EndIdx; ++i)
		{
			const FAssetData& A = Assets[i];
			TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("asset_path"), A.GetSoftObjectPath().ToString());
			Obj->SetStringField(TEXT("name"), A.AssetName.ToString());
			ItemArr.Add(MakeShared<FJsonValueObject>(Obj));
		}

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetArrayField(ArrayFieldName, ItemArr);
		Out->SetNumberField(TEXT("total_known"), Assets.Num());

		if (EndIdx < Assets.Num() && EndIdx > 0)
		{
			FMCPPageCursor OutCursor;
			OutCursor.FilterHash = FilterHash;
			OutCursor.LastAssetPath = Assets[EndIdx - 1].GetSoftObjectPath().ToString();
			OutCursor.TotalKnownSnapshot = Assets.Num();
			Out->SetStringField(TEXT("next_page_token"), FMCPPageCursorUtils::Encode(OutCursor));
		}

		return FMCPToolHelpers::MakeSuccessObj(Request, Out);
	}
} // namespace

namespace FInputTools
{

// ─── input.list_mapping_contexts ──────────────────────────────────────────────────────────────
//
// Args:    { path_prefix?: string, page_size?: int (default 100, clamp [1,1000]), page_token?: string }
// Result:  { mapping_contexts: [{ asset_path, name }], total_known, next_page_token? }
FMCPResponse Tool_ListMappingContexts(const FMCPRequest& Request)
{
	return INP_ListAssetsOfClass(Request, UInputMappingContext::StaticClass(), TEXT("mapping_contexts"));
}

// ─── input.list_input_actions ─────────────────────────────────────────────────────────────────
//
// Args:    { path_prefix?: string, page_size?: int (default 100, clamp [1,1000]), page_token?: string }
// Result:  { input_actions: [{ asset_path, name }], total_known, next_page_token? }
FMCPResponse Tool_ListInputActions(const FMCPRequest& Request)
{
	return INP_ListAssetsOfClass(Request, UInputAction::StaticClass(), TEXT("input_actions"));
}

// ─── input.get_context_bindings ───────────────────────────────────────────────────────────────
//
// Args:    { mapping_context_path: string }
// Result:  { mapping_context: string,
//            mappings: [{ action: string (path | "" if null),
//                         key: string (e.g. "Gamepad_FaceButton_Bottom" or "" if Invalid),
//                         modifiers: [class_name_string],
//                         triggers:  [class_name_string] }],
//            mapping_count: int }
//
// Walks UIMC->GetMappings() (the default key mappings; profile overrides are not enumerated here —
// callers seeking per-profile overrides should use marshall.read_property on
// UInputMappingContext.MappingProfileOverrides directly).
FMCPResponse Tool_GetContextBindings(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString MappingContextPath;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("mapping_context_path"), MappingContextPath, Err))
	{
		return Err;
	}

	int32 LoadErrCode = 0;
	FString LoadErrMsg;
	UInputMappingContext* IMC = FMCPAssetLoader::Load<UInputMappingContext>(MappingContextPath, LoadErrCode, LoadErrMsg);
	if (!IMC) { return FMCPToolHelpers::MakeError(Request, LoadErrCode, LoadErrMsg); }

	const TArray<FEnhancedActionKeyMapping>& Mappings = IMC->GetMappings();

	TArray<TSharedPtr<FJsonValue>> MappingArr;
	MappingArr.Reserve(Mappings.Num());
	for (const FEnhancedActionKeyMapping& Mapping : Mappings)
	{
		TSharedRef<FJsonObject> MapObj = MakeShared<FJsonObject>();

		// Action path — Mapping.Action is a TObjectPtr<const UInputAction>.
		MapObj->SetStringField(TEXT("action"),
			Mapping.Action ? Mapping.Action->GetPathName() : FString());

		// Key name — FKey's FName ToString gives the canonical short name (e.g.
		// "Gamepad_FaceButton_Bottom", "LeftMouseButton", "A"). EKeys::Invalid → empty.
		MapObj->SetStringField(TEXT("key"),
			Mapping.Key.IsValid() ? Mapping.Key.GetFName().ToString() : FString());

		// Modifier class list — short class names so the wire payload stays compact. Skip null
		// entries (legacy data can have unset slots in the instanced array).
		TArray<TSharedPtr<FJsonValue>> ModArr;
		ModArr.Reserve(Mapping.Modifiers.Num());
		for (const UInputModifier* Mod : Mapping.Modifiers)
		{
			if (Mod && Mod->GetClass())
			{
				ModArr.Add(MakeShared<FJsonValueString>(Mod->GetClass()->GetName()));
			}
		}
		MapObj->SetArrayField(TEXT("modifiers"), ModArr);

		// Trigger class list.
		TArray<TSharedPtr<FJsonValue>> TrigArr;
		TrigArr.Reserve(Mapping.Triggers.Num());
		for (const UInputTrigger* Trig : Mapping.Triggers)
		{
			if (Trig && Trig->GetClass())
			{
				TrigArr.Add(MakeShared<FJsonValueString>(Trig->GetClass()->GetName()));
			}
		}
		MapObj->SetArrayField(TEXT("triggers"), TrigArr);

		MappingArr.Add(MakeShared<FJsonValueObject>(MapObj));
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("mapping_context"), IMC->GetPathName());
	Out->SetArrayField(TEXT("mappings"), MappingArr);
	Out->SetNumberField(TEXT("mapping_count"), Mappings.Num());
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── input.list_player_contexts ───────────────────────────────────────────────────────────────
//
// Args:    { player_controller_path?: string (default = first PC in current world) }
// Result:  { player_controller: string,
//            contexts: [{ context: string (path), priority: int }],
//            context_count: int,
//            hint?: string (only when the probe-based enumeration path was taken — see header) }
//
// Resolution order for the player controller:
//   1. If args.player_controller_path supplied → FMCPActorPathUtils::ResolveActorOrNull (PIE-safe).
//   2. Else: walk worlds — prefer GEditor->PlayWorld (PIE) when active, else editor world. Pick
//      ``UGameplayStatics::GetPlayerController(World, 0)``.
//
// **UE 5.7 enumeration approach.** UEnhancedPlayerInput::AppliedInputContextData is protected.
// IEnhancedInputSubsystemInterface::HasMappingContext(IMC, OutPriority) IS public, so we probe
// every UInputMappingContext asset in the AssetRegistry against the player's subsystem. O(N)
// where N = total IMC asset count (typically <50). The response includes a ``hint`` field noting
// the probe-based approach so callers know the data is best-effort (won't surface contexts whose
// IMC asset was deleted but is still referenced by a strong pointer somewhere in the live
// subsystem state — these are rare).
FMCPResponse Tool_ListPlayerContexts(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString PCPath;
	if (Request.Args.IsValid())
	{
		Request.Args->TryGetStringField(TEXT("player_controller_path"), PCPath);
	}

	// 1) Resolve player controller.
	APlayerController* PC = nullptr;
	if (!PCPath.IsEmpty())
	{
		AActor* Actor = FMCPActorPathUtils::ResolveActorOrNull(PCPath, /*bRejectPIE=*/ false);
		if (!Actor)
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
				FString::Printf(TEXT("player_controller_path '%s' not resolved"), *PCPath));
		}
		PC = Cast<APlayerController>(Actor);
		if (!PC)
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorWrongClass,
				FString::Printf(TEXT("'%s' is class '%s'; expected APlayerController"),
					*PCPath, *Actor->GetClass()->GetPathName()));
		}
	}
	else
	{
		// Default: first PC in current world. Prefer PIE world when active so the tool surfaces
		// runtime input state without needing the caller to pass paths.
		UWorld* World = nullptr;
		if (FMCPWorldContext::IsPIEActive() && GEditor && GEditor->PlayWorld)
		{
			World = GEditor->PlayWorld;
		}
		else
		{
			World = FMCPWorldContext::GetEditorWorld();
		}
		if (World)
		{
			// Iterate player controllers in the world directly (FConstPlayerControllerIterator
			// gives us controllers across all PlayerControllerList entries).
			for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
			{
				if (APlayerController* Candidate = It->Get())
				{
					PC = Candidate;
					break;
				}
			}
		}
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();

	if (!PC)
	{
		// No player controller available — typical in editor world without PIE. Return an empty
		// contexts list with a descriptive hint rather than erroring out, so callers can probe
		// before/after PIE-start without conditional code paths.
		Out->SetStringField(TEXT("player_controller"), FString());
		TArray<TSharedPtr<FJsonValue>> Empty;
		Out->SetArrayField(TEXT("contexts"), Empty);
		Out->SetNumberField(TEXT("context_count"), 0);
		Out->SetStringField(TEXT("hint"),
			TEXT("no APlayerController in current world; start PIE or pass player_controller_path"));
		return FMCPToolHelpers::MakeSuccessObj(Request, Out);
	}

	Out->SetStringField(TEXT("player_controller"), PC->GetPathName());

	// 2) Get the enhanced input local-player subsystem off the PC.
	ULocalPlayer* LP = PC->GetLocalPlayer();
	if (!LP)
	{
		TArray<TSharedPtr<FJsonValue>> Empty;
		Out->SetArrayField(TEXT("contexts"), Empty);
		Out->SetNumberField(TEXT("context_count"), 0);
		Out->SetStringField(TEXT("hint"),
			TEXT("APlayerController has no ULocalPlayer (likely remote/standalone controller)"));
		return FMCPToolHelpers::MakeSuccessObj(Request, Out);
	}

	UEnhancedInputLocalPlayerSubsystem* EISS = LP->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>();
	if (!EISS)
	{
		TArray<TSharedPtr<FJsonValue>> Empty;
		Out->SetArrayField(TEXT("contexts"), Empty);
		Out->SetNumberField(TEXT("context_count"), 0);
		Out->SetStringField(TEXT("hint"),
			TEXT("ULocalPlayer has no UEnhancedInputLocalPlayerSubsystem (Enhanced Input plugin "
				 "may be disabled or subsystem not yet initialised)"));
		return FMCPToolHelpers::MakeSuccessObj(Request, Out);
	}

	// 3) Probe-enumerate active contexts by asking HasMappingContext for every known IMC asset.
	// AppliedInputContextData lives on UEnhancedPlayerInput protected, so this is the only public
	// path in 5.7. AssetRegistry walk is O(N) over IMC assets — typically <50 per project.
	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FARFilter Filter;
	Filter.ClassPaths.Add(UInputMappingContext::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = false;
	Filter.bRecursivePaths   = true;
	TArray<FAssetData> IMCAssets;
	AR.GetAssets(Filter, IMCAssets);

	// Sort for stable response ordering (priority desc, then path asc).
	struct FActiveCtx
	{
		FString Path;
		int32   Priority;
	};
	TArray<FActiveCtx> Active;
	Active.Reserve(IMCAssets.Num());

	for (const FAssetData& A : IMCAssets)
	{
		// Probe without forcing a load — only check IMCs that are already in memory. Loading every
		// IMC asset just to probe HasMappingContext would blow editor memory on large projects;
		// the live subsystem only references IMCs that are already loaded anyway.
		UObject* Obj = A.FastGetAsset(/*bLoad=*/ false);
		UInputMappingContext* IMC = Obj ? Cast<UInputMappingContext>(Obj) : nullptr;
		if (!IMC) { continue; }

		int32 FoundPriority = -1;
		if (EISS->HasMappingContext(IMC, FoundPriority))
		{
			Active.Add({ IMC->GetPathName(), FoundPriority });
		}
	}

	// Stable sort: priority DESC (higher priority first), path ASC tiebreak.
	Active.Sort([](const FActiveCtx& X, const FActiveCtx& Y)
	{
		if (X.Priority != Y.Priority) { return X.Priority > Y.Priority; }
		return X.Path < Y.Path;
	});

	TArray<TSharedPtr<FJsonValue>> CtxArr;
	CtxArr.Reserve(Active.Num());
	for (const FActiveCtx& Ctx : Active)
	{
		TSharedRef<FJsonObject> CObj = MakeShared<FJsonObject>();
		CObj->SetStringField(TEXT("context"), Ctx.Path);
		CObj->SetNumberField(TEXT("priority"), Ctx.Priority);
		CtxArr.Add(MakeShared<FJsonValueObject>(CObj));
	}

	Out->SetArrayField(TEXT("contexts"), CtxArr);
	Out->SetNumberField(TEXT("context_count"), Active.Num());
	Out->SetStringField(TEXT("hint"),
		TEXT("contexts enumerated via HasMappingContext probe over loaded UInputMappingContext "
			 "assets (UE 5.7 has no public bulk enumerator). Unloaded IMC assets are skipped."));
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── Registration ──────────────────────────────────────────────────────────────────────────────
void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	RegisterTool(TEXT("input.list_mapping_contexts"), &Tool_ListMappingContexts, /*Lane A*/ false);
	RegisterTool(TEXT("input.list_input_actions"),    &Tool_ListInputActions,    /*Lane A*/ false);
	RegisterTool(TEXT("input.get_context_bindings"),  &Tool_GetContextBindings,  /*Lane A*/ false);
	RegisterTool(TEXT("input.list_player_contexts"),  &Tool_ListPlayerContexts,  /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("Input surface registered: 4 input.* tools "
			 "(list_mapping_contexts + list_input_actions + get_context_bindings + list_player_contexts), "
			 "all Lane A, all read-only"));
}

} // namespace FInputTools

#undef LOCTEXT_NAMESPACE
