// Copyright FatumGame. All Rights Reserved.

#include "AIBehaviorTreeTools.h"

#include "FMCPDispatchQueue.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPActorPathUtils.h"
#include "Utils/MCPAssetPathUtils.h"
#include "Utils/MCPPageCursor.h"

#include "AIController.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTNode.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/BTTaskNode.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "UObject/Package.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// AIBT_ prefix per the unity-build symbol-collision convention. The plugin uses unity builds
	// so anonymous-namespace helpers MUST be uniquely prefixed across every Tools/*.cpp.
	constexpr int32 kAIBTErrorInvalidParams    = -32602;
	constexpr int32 kAIBTErrorObjectNotFound   = kMCPErrorObjectNotFound;   // -32004
	constexpr int32 kAIBTErrorInvalidPath      = kMCPErrorInvalidPath;      // -32010
	constexpr int32 kAIBTErrorWrongClass       = kMCPErrorWrongClass;       // -32011
	constexpr int32 kAIBTErrorStaleCursor      = kMCPErrorStaleCursor;      // -32015

	// Hard cap on recursion depth in NodeToJson — guards against pathological / cyclic trees that
	// somehow slipped past UBehaviorTree::PreSave validation. Real BTs never approach this.
	constexpr int32 kAIBTMaxRecursionDepth = 64;

	void AIBT_StampIds(const FMCPRequest& Request, FMCPResponse& Response)
	{
		Response.RequestId = Request.RequestId;
		Response.OriginalIdString = Request.OriginalIdString;
	}

	FMCPResponse AIBT_MakeError(const FMCPRequest& Request, int32 Code, const FString& Message)
	{
		FMCPResponse R;
		AIBT_StampIds(Request, R);
		R.bIsError = true;
		R.ErrorCode = Code;
		R.ErrorMessage = Message;
		return R;
	}

	FMCPResponse AIBT_MakeSuccessObj(const FMCPRequest& Request, TSharedPtr<FJsonObject> Result)
	{
		FMCPResponse R;
		AIBT_StampIds(Request, R);
		R.bIsError = false;
		R.Result = MakeShared<FJsonValueObject>(MoveTemp(Result));
		return R;
	}

	bool AIBT_RequireStringField(const FMCPRequest& Request, const TCHAR* FieldName,
		FString& OutValue, FMCPResponse& OutError)
	{
		if (!Request.Args.IsValid())
		{
			OutError = AIBT_MakeError(Request, kAIBTErrorInvalidParams, TEXT("missing args object"));
			return false;
		}
		if (!Request.Args->TryGetStringField(FieldName, OutValue) || OutValue.IsEmpty())
		{
			OutError = AIBT_MakeError(Request, kAIBTErrorInvalidParams,
				FString::Printf(TEXT("missing required string field '%s'"), FieldName));
			return false;
		}
		return true;
	}

	// ─── UBehaviorTree load (mirrors AnimBlueprintTools' ABP_LoadAnimBlueprintByPath pattern) ─────

	/**
	 * Resolve a path to a UBehaviorTree. Accepts package-name (``/Game/AI/BT_X``) and object-path
	 * (``/Game/AI/BT_X.BT_X``) forms — retries with the ``.LeafName`` suffix if the first attempt
	 * fails. Populates OutErrorCode + OutErrorMsg on failure. Distinguishes:
	 *   - Malformed path        → -32010 InvalidPath
	 *   - LoadObject failure    → -32004 ObjectNotFound
	 *   - Wrong class           → -32011 WrongClass (with actual class name)
	 */
	UBehaviorTree* AIBT_LoadBehaviorTreeByPath(const FString& Path, int32& OutErrorCode, FString& OutErrorMsg)
	{
		if (Path.IsEmpty())
		{
			OutErrorCode = kAIBTErrorInvalidPath;
			OutErrorMsg = TEXT("bt_path is empty");
			return nullptr;
		}
		const FString Normalised = FMCPAssetPathUtils::Normalize(Path);
		if (Normalised.IsEmpty() || !FMCPAssetPathUtils::IsValidGameOrPlugin(Normalised))
		{
			OutErrorCode = kAIBTErrorInvalidPath;
			OutErrorMsg = FString::Printf(TEXT("bt_path '%s' malformed or unknown mount"), *Path);
			return nullptr;
		}

		UObject* Loaded = LoadObject<UObject>(nullptr, *Normalised);
		if (!Loaded)
		{
			// Retry with the object-path form. Mirrors FMCPBlueprintUtils::LoadBlueprintByPath.
			const FString ObjPath = FMCPAssetPathUtils::ToObjectPath(Normalised);
			if (!ObjPath.IsEmpty() && ObjPath != Normalised)
			{
				Loaded = LoadObject<UObject>(nullptr, *ObjPath);
			}
		}
		if (!Loaded)
		{
			OutErrorCode = kAIBTErrorObjectNotFound;
			OutErrorMsg = FString::Printf(TEXT("bt_path '%s' not loadable"), *Path);
			return nullptr;
		}

		UBehaviorTree* BT = Cast<UBehaviorTree>(Loaded);
		if (!BT)
		{
			OutErrorCode = kAIBTErrorWrongClass;
			OutErrorMsg = FString::Printf(
				TEXT("'%s' is of class '%s'; expected UBehaviorTree"),
				*Path, *Loaded->GetClass()->GetPathName());
			return nullptr;
		}
		return BT;
	}

	// ─── Actor → AAIController resolution ────────────────────────────────────────────────────────

	/**
	 * Resolve an actor_path to an AAIController*. Accepts two cases:
	 *   - The actor itself IS an AAIController (rare but valid — Cast directly).
	 *   - The actor is an APawn whose Controller is an AAIController (the common path).
	 *
	 * Returns null + populates OutErrorCode + OutErrorMsg on failure. Pass-through for the
	 * underlying ResolveActor errors (-32010 invalid path, -32004 not found, etc.) plus the
	 * surface-specific -32011 for "actor exists but has no AAIController".
	 *
	 * bRejectPIE=false so this works on PIE pawns (start/stop are runtime-testing tools).
	 */
	AAIController* AIBT_ResolveAIController(const FString& ActorPath, int32& OutErrorCode, FString& OutErrorMsg)
	{
		bool bAmbiguous = false;
		FString AmbigHint;
		FString ResolveErr;
		AActor* Actor = FMCPActorPathUtils::ResolveActor(
			ActorPath, /*bRejectPIE*/ false, bAmbiguous, AmbigHint, ResolveErr);
		if (!Actor)
		{
			OutErrorCode = kAIBTErrorObjectNotFound;
			OutErrorMsg = bAmbiguous
				? FString::Printf(TEXT("actor_path '%s' is ambiguous; candidates: %s"), *ActorPath, *AmbigHint)
				: FString::Printf(TEXT("actor_path '%s' did not resolve: %s"), *ActorPath, *ResolveErr);
			return nullptr;
		}

		// Case 1: actor is itself an AAIController.
		if (AAIController* DirectAIC = Cast<AAIController>(Actor))
		{
			return DirectAIC;
		}

		// Case 2: actor is an APawn whose Controller is an AAIController.
		if (APawn* Pawn = Cast<APawn>(Actor))
		{
			if (AAIController* AIC = Cast<AAIController>(Pawn->GetController()))
			{
				return AIC;
			}
			OutErrorCode = kAIBTErrorWrongClass;
			OutErrorMsg = FString::Printf(
				TEXT("pawn '%s' has no AAIController (Controller=%s)"),
				*ActorPath,
				Pawn->GetController() ? *Pawn->GetController()->GetClass()->GetName() : TEXT("nullptr"));
			return nullptr;
		}

		// Case 3: actor is neither an AAIController nor an APawn.
		OutErrorCode = kAIBTErrorWrongClass;
		OutErrorMsg = FString::Printf(
			TEXT("actor '%s' is of class '%s'; expected AAIController or APawn-with-AAIController"),
			*ActorPath, *Actor->GetClass()->GetName());
		return nullptr;
	}

	// ─── BT node → JSON (recursive walk) ─────────────────────────────────────────────────────────

	// Forward decl — composite branch recurses via NodeToJson.
	TSharedRef<FJsonObject> AIBT_NodeToJson(const UBTNode* Node, int32 Depth);

	/** Serialize a UBTDecorator/UBTService aux node (no children — just class + name + index). */
	TSharedRef<FJsonObject> AIBT_AuxNodeToJson(const UBTNode* Aux)
	{
		check(Aux);
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("node_class"), Aux->GetClass()->GetName());
		Obj->SetStringField(TEXT("node_name"), Aux->GetNodeName());
		Obj->SetNumberField(TEXT("execution_index"), static_cast<double>(Aux->GetExecutionIndex()));
		return Obj;
	}

	TSharedRef<FJsonObject> AIBT_NodeToJson(const UBTNode* Node, int32 Depth)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (!Node)
		{
			Obj->SetStringField(TEXT("node_class"), TEXT("<null>"));
			Obj->SetStringField(TEXT("node_name"), TEXT("<null>"));
			return Obj;
		}

		Obj->SetStringField(TEXT("node_class"), Node->GetClass()->GetName());
		Obj->SetStringField(TEXT("node_name"), Node->GetNodeName());
		Obj->SetNumberField(TEXT("execution_index"), static_cast<double>(Node->GetExecutionIndex()));

		// Recursion guard. Real BTs never approach this; the cap exists purely to prevent runaway
		// stack growth on a corrupted tree (cycle / self-parent).
		if (Depth >= kAIBTMaxRecursionDepth)
		{
			Obj->SetBoolField(TEXT("truncated_at_max_depth"), true);
			return Obj;
		}

		// Composite nodes carry children + decorators + services. Tasks are leaves; decorators /
		// services are aux nodes serialized inline by their parent composite (via Children[].Decorators
		// and Services[]).
		if (const UBTCompositeNode* Composite = Cast<const UBTCompositeNode>(Node))
		{
			TArray<TSharedPtr<FJsonValue>> ChildArr;
			ChildArr.Reserve(Composite->Children.Num());
			for (const FBTCompositeChild& Child : Composite->Children)
			{
				// Per-child sub-object so callers see the decorator stack that gates THIS child slot
				// (BT decorators attach to the parent's child-edge, not the child node itself).
				TSharedRef<FJsonObject> ChildObj = MakeShared<FJsonObject>();

				// The child node itself — exactly one of ChildComposite / ChildTask is non-null per
				// FBTCompositeChild contract. The other branch covers the legal-empty case (e.g.
				// freshly-constructed composite with a placeholder slot — rare but UE allows it).
				const UBTNode* ChildNode = nullptr;
				if (Child.ChildComposite) { ChildNode = Child.ChildComposite; }
				else if (Child.ChildTask) { ChildNode = Child.ChildTask; }
				ChildObj->SetObjectField(TEXT("node"), AIBT_NodeToJson(ChildNode, Depth + 1));

				// Per-child decorator stack (UBTDecorator instances gating this branch).
				TArray<TSharedPtr<FJsonValue>> ChildDecArr;
				ChildDecArr.Reserve(Child.Decorators.Num());
				for (const UBTDecorator* Dec : Child.Decorators)
				{
					if (Dec) { ChildDecArr.Add(MakeShared<FJsonValueObject>(AIBT_AuxNodeToJson(Dec))); }
				}
				ChildObj->SetArrayField(TEXT("decorators"), ChildDecArr);

				ChildArr.Add(MakeShared<FJsonValueObject>(ChildObj));
			}
			Obj->SetArrayField(TEXT("children"), ChildArr);

			// Composite-level services (UBTService instances ticking while this composite is active).
			TArray<TSharedPtr<FJsonValue>> ServicesArr;
			ServicesArr.Reserve(Composite->Services.Num());
			for (const UBTService* Service : Composite->Services)
			{
				if (Service) { ServicesArr.Add(MakeShared<FJsonValueObject>(AIBT_AuxNodeToJson(Service))); }
			}
			Obj->SetArrayField(TEXT("services"), ServicesArr);
		}
		else if (Cast<const UBTTaskNode>(Node))
		{
			// Task = leaf. Empty children array is intentional (the absence is informative for
			// downstream consumers — they can tell at a glance which nodes are leaves).
			Obj->SetArrayField(TEXT("children"), TArray<TSharedPtr<FJsonValue>>());
		}
		// Other UBTNode subclasses (UBTAuxiliaryNode etc.) fall through with no children / services
		// fields — they shouldn't appear as standalone tree nodes (only as decorators/services), but
		// we serialize their class/name anyway so the caller can debug an unexpected shape.

		return Obj;
	}
} // namespace

namespace FAIBehaviorTreeTools
{

// ─── ai.bt.list_assets ─────────────────────────────────────────────────────────────────────────
//
// Args:
//   path_prefix?  : string   (default empty = all of /Game)
//   page_size?    : int      (default 100, clamp [1, 1000])
//   page_token?   : string   (opaque cursor returned by prior call)
//
// Result:
//   {
//     behavior_trees: [
//       {
//         asset_path             : string,    full /Game/AI/BT_X.BT_X soft-object-path form
//         blackboard_asset_path? : string,    omitted when BT has no BlackboardAsset
//         root_node_class?       : string,    omitted when BT has no RootNode (newly-created /
//                                             not-yet-edited tree)
//       }, ...
//     ],
//     total_known      : int,
//     next_page_token? : string
//   }
//
// Errors:
//   -32015 StaleCursor      filter mutated between pages (path_prefix changed; cursor's filter_hash
//                           no longer matches the current call's hash)
//   -32602 InvalidParams    malformed page_token (base64 decode / JSON parse failure)
//
// Notes on resilience:
//   - path_prefix is treated as an opaque string handed to FARFilter::PackagePaths (no -32010
//     up-front validation). Invalid prefixes silently return empty results rather than erroring —
//     matches the convention in data_table.list / asset.list.
//   - LoadModuleChecked("AssetRegistry") asserts on failure but the module is always loaded in
//     editor builds (the plugin is editor-only). No -32603 path here in practice.
//
// Notes:
//   - LoadObject for each entry on the page (NOT for total_known sweep) so blackboard_asset_path /
//     root_node_class are accurate without an extra round-trip. Loading is idempotent so callers
//     paginating pay the load cost once per BT.
//   - Pagination mirrors data_table.list (keyset-sorted by ObjectPath, base64 cursor with
//     filter_hash for staleness).
//   - Empty result is a legitimate state (project has no UBehaviorTree assets yet) — surfaced as
//     behavior_trees=[] with total_known=0, NOT as -32603.
FMCPResponse Tool_ListAssets(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString PathPrefix;
	if (Request.Args.IsValid()) { Request.Args->TryGetStringField(TEXT("path_prefix"), PathPrefix); }

	int32 PageSize = 100;
	if (Request.Args.IsValid()) { Request.Args->TryGetNumberField(TEXT("page_size"), PageSize); }
	PageSize = FMath::Clamp(PageSize, 1, 1000);

	FString PageToken;
	if (Request.Args.IsValid()) { Request.Args->TryGetStringField(TEXT("page_token"), PageToken); }

	// Hash the filter shape so the cursor can detect mid-pagination filter mutations.
	const uint32 FilterHash = GetTypeHash(PathPrefix);

	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FARFilter Filter;
	Filter.ClassPaths.Add(UBehaviorTree::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = false;
	Filter.bRecursivePaths   = true;
	if (!PathPrefix.IsEmpty())
	{
		Filter.PackagePaths.Add(*PathPrefix);
	}
	TArray<FAssetData> Assets;
	AR.GetAssets(Filter, Assets);

	// Stable sort by ObjectPath (the keyset-pagination sort key — same convention as data_table.list).
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
			return AIBT_MakeError(Request, kAIBTErrorInvalidParams,
				FString::Printf(TEXT("invalid page_token: %s"), *DecodeErr));
		}
		if (!FMCPPageCursorUtils::ValidateAgainstFilter(InCursor, FilterHash))
		{
			return AIBT_MakeError(Request, kAIBTErrorStaleCursor,
				TEXT("filter mutated between pages (path_prefix changed); restart pagination"));
		}
		while (StartIdx < Assets.Num() &&
			   Assets[StartIdx].GetSoftObjectPath().ToString() <= InCursor.LastAssetPath)
		{
			++StartIdx;
		}
	}

	TArray<TSharedPtr<FJsonValue>> BTArr;
	const int32 EndIdx = FMath::Min(StartIdx + PageSize, Assets.Num());
	BTArr.Reserve(EndIdx - StartIdx);
	for (int32 i = StartIdx; i < EndIdx; ++i)
	{
		const FAssetData& A = Assets[i];
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("asset_path"), A.GetSoftObjectPath().ToString());

		// Load the BT to read blackboard + root-class fields. AssetRegistry tags do NOT expose
		// these on UBehaviorTree (no equivalent of UDataTable's "RowStructure" AR tag). Load cost
		// is amortised across calls — UE caches the loaded UObject.
		if (UBehaviorTree* BT = Cast<UBehaviorTree>(A.GetAsset()))
		{
			if (BT->BlackboardAsset)
			{
				Obj->SetStringField(TEXT("blackboard_asset_path"),
					BT->BlackboardAsset->GetPathName());
			}
			if (BT->RootNode)
			{
				Obj->SetStringField(TEXT("root_node_class"),
					BT->RootNode->GetClass()->GetName());
			}
		}
		// If GetAsset() returned null (rare — could happen for an in-progress redirector), fall
		// through with just asset_path. Caller branches on the absence of optional fields.

		BTArr.Add(MakeShared<FJsonValueObject>(Obj));
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetArrayField(TEXT("behavior_trees"), BTArr);
	Out->SetNumberField(TEXT("total_known"), Assets.Num());

	if (EndIdx < Assets.Num() && EndIdx > 0)
	{
		FMCPPageCursor OutCursor;
		OutCursor.FilterHash = FilterHash;
		OutCursor.LastAssetPath = Assets[EndIdx - 1].GetSoftObjectPath().ToString();
		OutCursor.TotalKnownSnapshot = Assets.Num();
		Out->SetStringField(TEXT("next_page_token"), FMCPPageCursorUtils::Encode(OutCursor));
	}

	return AIBT_MakeSuccessObj(Request, Out);
}

// ─── ai.bt.get_nodes ───────────────────────────────────────────────────────────────────────────
//
// Args:
//   bt_path: string   required — UBehaviorTree asset path
//
// Result:
//   {
//     bt_path : string,                full asset path of the resolved tree
//     blackboard_asset_path? : string, when BlackboardAsset is non-null
//     root: {
//       node_class      : string,
//       node_name       : string,
//       execution_index : int,
//       children: [
//         {
//           node: { node_class, node_name, execution_index, children?, services? },
//           decorators: [{ node_class, node_name, execution_index }, ...]
//         }, ...
//       ],
//       services: [{ node_class, node_name, execution_index }, ...]
//     }
//   }
//
// Errors:
//   -32004 ObjectNotFound      bt_path not loadable
//   -32010 InvalidPath         malformed bt_path
//   -32011 WrongClass          path resolved but is not a UBehaviorTree
//   -32602 InvalidParams       missing bt_path
//
// Notes on null root:
//   - BT loaded with RootNode == null (newly-created / unedited tree) is NOT an error. We surface
//     a success response with root=null rather than -32603; the caller branches on the null.
//
// Notes:
//   - Recursive walk via AIBT_NodeToJson, depth-capped at kAIBTMaxRecursionDepth (64). Real BTs
//     are flat (rarely deeper than 6-8 levels) so this is purely a corruption guard.
//   - UBTDecorator instances are NOT serialized as children of the gated node — they live ON the
//     PARENT composite's Children[].Decorators array (BT decorators attach to child edges, not
//     child nodes). The JSON shape reflects that: each child entry under "children" carries its
//     own "decorators" sub-array.
//   - UBTService instances are composite-level: serialized under "services" on the composite
//     node, NOT per-child.
//   - Task nodes (UBTTaskNode) are leaves; their "children" array is always empty (preserved in
//     output so callers can do uniform shape-matching).
FMCPResponse Tool_GetNodes(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString BTPath;
	FMCPResponse ArgErr;
	if (!AIBT_RequireStringField(Request, TEXT("bt_path"), BTPath, ArgErr))
	{
		return ArgErr;
	}

	int32 ErrCode = 0;
	FString ErrMsg;
	UBehaviorTree* BT = AIBT_LoadBehaviorTreeByPath(BTPath, ErrCode, ErrMsg);
	if (!BT)
	{
		return AIBT_MakeError(Request, ErrCode, ErrMsg);
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("bt_path"), BT->GetPathName());
	if (BT->BlackboardAsset)
	{
		Out->SetStringField(TEXT("blackboard_asset_path"), BT->BlackboardAsset->GetPathName());
	}

	if (BT->RootNode)
	{
		Out->SetObjectField(TEXT("root"), AIBT_NodeToJson(BT->RootNode, /*Depth*/ 0));
	}
	else
	{
		// Newly-created BT with no root composite — legitimate state, surface as root=null.
		Out->SetField(TEXT("root"), MakeShared<FJsonValueNull>());
	}

	return AIBT_MakeSuccessObj(Request, Out);
}

// ─── ai.bt.start_on_actor ──────────────────────────────────────────────────────────────────────
//
// Args:
//   actor_path: string   required — actor path (any FMCPActorPathUtils form)
//   bt_path   : string   required — UBehaviorTree asset path
//
// Result:
//   {
//     started         : bool,    true if RunBehaviorTree returned true (false typically means the
//                                AAIController already had an incompatible BT running OR the BT's
//                                blackboard couldn't bind)
//     controller_path : string,  full path of the resolved AAIController
//     bt_path         : string   echoed for round-trip confirmation
//   }
//
// Errors:
//   -32004 ObjectNotFound  actor_path / bt_path not resolvable
//   -32010 InvalidPath     malformed bt_path
//   -32011 WrongClass      bt_path is not UBehaviorTree; OR actor has no AAIController
//   -32602 InvalidParams   missing args
//
// Notes:
//   - NOT PIE-guarded by design — runtime BT iteration testing IS the use case. Editor-world
//     AAIController instances are atypical (pawn controllers spawn at PIE start), so on the
//     editor world this typically surfaces -32011 (actor has no controller) which is the right
//     diagnosis.
//   - AAIController::RunBehaviorTree returns false on several legitimate failure modes (no
//     blackboard binding, etc.). We surface that as started=false (success response with bool
//     field), not as an error code — the caller can distinguish "BT failed to start" from "tool
//     failed".
FMCPResponse Tool_StartOnActor(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString ActorPath;
	FMCPResponse ArgErr;
	if (!AIBT_RequireStringField(Request, TEXT("actor_path"), ActorPath, ArgErr))
	{
		return ArgErr;
	}
	FString BTPath;
	if (!AIBT_RequireStringField(Request, TEXT("bt_path"), BTPath, ArgErr))
	{
		return ArgErr;
	}

	// Resolve BT first so the user gets the asset-side diagnosis before the actor-side one. Order
	// is cheap to swap; chose this purely for caller UX (an invalid BT path is more likely than an
	// invalid actor path in editor scripting).
	int32 BTErrCode = 0;
	FString BTErrMsg;
	UBehaviorTree* BT = AIBT_LoadBehaviorTreeByPath(BTPath, BTErrCode, BTErrMsg);
	if (!BT)
	{
		return AIBT_MakeError(Request, BTErrCode, BTErrMsg);
	}

	int32 AICErrCode = 0;
	FString AICErrMsg;
	AAIController* AIC = AIBT_ResolveAIController(ActorPath, AICErrCode, AICErrMsg);
	if (!AIC)
	{
		return AIBT_MakeError(Request, AICErrCode, AICErrMsg);
	}

	const bool bStarted = AIC->RunBehaviorTree(BT);

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("started"), bStarted);
	Out->SetStringField(TEXT("controller_path"), AIC->GetPathName());
	Out->SetStringField(TEXT("bt_path"), BT->GetPathName());
	return AIBT_MakeSuccessObj(Request, Out);
}

// ─── ai.bt.stop_on_actor ───────────────────────────────────────────────────────────────────────
//
// Args:
//   actor_path: string   required — actor path (any FMCPActorPathUtils form)
//
// Result:
//   {
//     stopped         : bool,    true when we successfully called StopTree (always true if the
//                                BTComponent was non-null; reports false only when there was no
//                                BehaviorTreeComponent on the controller, which means nothing was
//                                running to stop)
//     controller_path : string,  full path of the resolved AAIController
//     prior_active_bt?: string   asset path of the BT that was running BEFORE StopTree (omitted
//                                when stopped=false OR when the component had no current tree)
//   }
//
// Errors:
//   -32004 ObjectNotFound  actor_path not resolvable
//   -32011 WrongClass      actor has no AAIController
//   -32602 InvalidParams   missing actor_path
//
// Notes:
//   - NOT PIE-guarded (same rationale as start_on_actor — runtime introspection is the use case).
//   - We snapshot GetCurrentTree() BEFORE calling StopTree so the response can report what was
//     stopped. Mode defaults to EBTStopMode::Safe (the BT clean-shutdown path) — matches the
//     engine's default. Forced stop would require a separate arg if the caller has a specific
//     use case for it; deferred to a future revision.
FMCPResponse Tool_StopOnActor(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString ActorPath;
	FMCPResponse ArgErr;
	if (!AIBT_RequireStringField(Request, TEXT("actor_path"), ActorPath, ArgErr))
	{
		return ArgErr;
	}

	int32 AICErrCode = 0;
	FString AICErrMsg;
	AAIController* AIC = AIBT_ResolveAIController(ActorPath, AICErrCode, AICErrMsg);
	if (!AIC)
	{
		return AIBT_MakeError(Request, AICErrCode, AICErrMsg);
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("controller_path"), AIC->GetPathName());

	// Cast BrainComponent to UBehaviorTreeComponent — the only BT-aware brain subclass shipped by
	// the engine. Custom brain components (rare) won't have GetCurrentTree / StopTree and we
	// correctly report stopped=false.
	UBehaviorTreeComponent* BTC = Cast<UBehaviorTreeComponent>(AIC->BrainComponent);
	if (!BTC)
	{
		Out->SetBoolField(TEXT("stopped"), false);
		return AIBT_MakeSuccessObj(Request, Out);
	}

	UBehaviorTree* PriorTree = BTC->GetCurrentTree();
	BTC->StopTree(EBTStopMode::Safe);

	Out->SetBoolField(TEXT("stopped"), true);
	if (PriorTree)
	{
		Out->SetStringField(TEXT("prior_active_bt"), PriorTree->GetPathName());
	}
	return AIBT_MakeSuccessObj(Request, Out);
}

// ─── Registration ──────────────────────────────────────────────────────────────────────────────
void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	RegisterTool(TEXT("ai.bt.list_assets"),    &Tool_ListAssets,   /*Lane A*/ false);
	RegisterTool(TEXT("ai.bt.get_nodes"),      &Tool_GetNodes,     /*Lane A*/ false);
	RegisterTool(TEXT("ai.bt.start_on_actor"), &Tool_StartOnActor, /*Lane A*/ false);
	RegisterTool(TEXT("ai.bt.stop_on_actor"),  &Tool_StopOnActor,  /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("Wave J Surface 1 (AI Behavior Tree): registered 4 ai.bt.* handlers "
			 "(list_assets / get_nodes / start_on_actor / stop_on_actor, all Lane A)"));
}

} // namespace FAIBehaviorTreeTools

// Wave I refactor 2026-05: auto-registration via FMCPSurfaceRegistry replaces the
// manual include + Register call in UnrealMCPBridge.cpp.
#include "MCPSurfaceRegistry.h"
MCP_REGISTER_SURFACE(AIBehaviorTreeTools, &FAIBehaviorTreeTools::Register)

#undef LOCTEXT_NAMESPACE
