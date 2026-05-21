// Copyright FatumGame. All Rights Reserved.

#include "AIControllerTools.h"

#include "FMCPDispatchQueue.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPActorPathUtils.h"

#include "AIController.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BehaviorTree/BlackboardData.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"                            // TActorIterator
#include "GameFramework/Pawn.h"
#include "GameFramework/Volume.h"                   // AVolume::EncompassesPoint
#include "NavMesh/NavMeshBoundsVolume.h"            // ANavMeshBoundsVolume
#include "Perception/AIPerceptionComponent.h"
#include "UObject/UObjectGlobals.h"                 // LoadObject / StaticLoadClass

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// AICTRL_ prefix per the unity-build symbol-collision convention. Every Tools/*.cpp anonymous-
	// namespace helper MUST be uniquely prefixed across the module — see Wave F4 BlueprintComponentTools
	// rename note in MEMORY.md for the failure mode.
	constexpr int32 kAICTRLErrorInvalidParams   = -32602;
	constexpr int32 kAICTRLErrorInternal        = -32603;
	constexpr int32 kAICTRLErrorObjectNotFound  = kMCPErrorObjectNotFound;  // -32004
	constexpr int32 kAICTRLErrorInvalidPath     = kMCPErrorInvalidPath;     // -32010
	constexpr int32 kAICTRLErrorWrongClass      = kMCPErrorWrongClass;      // -32011

	void AICTRL_StampIds(const FMCPRequest& Request, FMCPResponse& Response)
	{
		Response.RequestId = Request.RequestId;
		Response.OriginalIdString = Request.OriginalIdString;
	}

	FMCPResponse AICTRL_MakeError(const FMCPRequest& Request, int32 Code, const FString& Message)
	{
		FMCPResponse R;
		AICTRL_StampIds(Request, R);
		R.bIsError = true;
		R.ErrorCode = Code;
		R.ErrorMessage = Message;
		return R;
	}

	FMCPResponse AICTRL_MakeSuccessObj(const FMCPRequest& Request, TSharedPtr<FJsonObject> Result)
	{
		FMCPResponse R;
		AICTRL_StampIds(Request, R);
		R.bIsError = false;
		R.Result = MakeShared<FJsonValueObject>(MoveTemp(Result));
		return R;
	}

	bool AICTRL_RequireStringField(const FMCPRequest& Request, const TCHAR* FieldName,
		FString& OutValue, FMCPResponse& OutError)
	{
		if (!Request.Args.IsValid())
		{
			OutError = AICTRL_MakeError(Request, kAICTRLErrorInvalidParams, TEXT("missing args object"));
			return false;
		}
		if (!Request.Args->TryGetStringField(FieldName, OutValue) || OutValue.IsEmpty())
		{
			OutError = AICTRL_MakeError(Request, kAICTRLErrorInvalidParams,
				FString::Printf(TEXT("missing required string field '%s'"), FieldName));
			return false;
		}
		return true;
	}

	/**
	 * Resolve the editor world for enumeration. Returns null + writes code/message on failure. We
	 * deliberately use ``GEditor->GetEditorWorldContext().World()`` (editor world) per the brief
	 * even though PIE is not guarded — the brief's wording "current editor world" matches the
	 * outliner UX. Callers that want to enumerate PIE-side controllers can either stop PIE first
	 * or address them explicitly via their PIE actor paths (since this surface has no PIE guard,
	 * resolution against a PIE actor path will still succeed via FMCPActorPathUtils::ResolveActor).
	 */
	UWorld* AICTRL_ResolveWorld(int32& OutErrCode, FString& OutErr)
	{
		if (!GEditor)
		{
			OutErrCode = kAICTRLErrorInternal;
			OutErr = TEXT("no GEditor available (commandlet / cooked build)");
			return nullptr;
		}
		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!World)
		{
			OutErrCode = kAICTRLErrorInternal;
			OutErr = TEXT("no editor world available (no map loaded)");
			return nullptr;
		}
		return World;
	}

	/**
	 * Resolve the optional ``class_filter`` arg. Returns AAIController::StaticClass() when absent
	 * (matches every AI controller). Returns null + populates error fields when present-but-invalid
	 * (malformed path, unresolvable class, OR not derived from AAIController).
	 */
	UClass* AICTRL_ResolveClassFilter(const FMCPRequest& Request, int32& OutErrCode, FString& OutErr)
	{
		FString ClassPath;
		const bool bHasField = Request.Args.IsValid()
			&& Request.Args->TryGetStringField(TEXT("class_filter"), ClassPath)
			&& !ClassPath.IsEmpty();
		if (!bHasField)
		{
			return AAIController::StaticClass();
		}

		// Reject backslashes early — matches the IsValidGameOrPlugin gate semantics for asset paths.
		if (ClassPath.Contains(TEXT("\\")))
		{
			OutErrCode = kAICTRLErrorInvalidPath;
			OutErr = FString::Printf(TEXT("class_filter '%s' contains backslash"), *ClassPath);
			return nullptr;
		}

		// FindObject first (cheap — only succeeds if the class is already loaded), then StaticLoadClass
		// (autoload, for /Game/... Blueprint-derived AI controllers).
		UClass* Cls = FindObject<UClass>(nullptr, *ClassPath);
		if (!Cls)
		{
			Cls = StaticLoadClass(UObject::StaticClass(), nullptr, *ClassPath, nullptr, LOAD_NoWarn);
		}
		if (!Cls)
		{
			OutErrCode = kAICTRLErrorObjectNotFound;
			OutErr = FString::Printf(TEXT("class_filter '%s' did not resolve to a UClass"), *ClassPath);
			return nullptr;
		}
		if (!Cls->IsChildOf(AAIController::StaticClass()))
		{
			OutErrCode = kAICTRLErrorWrongClass;
			OutErr = FString::Printf(
				TEXT("class_filter '%s' resolves to '%s' which is not derived from AAIController"),
				*ClassPath, *Cls->GetName());
			return nullptr;
		}
		return Cls;
	}

	/**
	 * Resolve an AAIController by actor_path. Returns null + writes code/message on failure. Read-
	 * only AND mutator paths both use this — bRejectPIE=false because the brief explicitly opts out
	 * of the PIE guard for this surface (runtime AI introspection during PIE is the primary use
	 * case).
	 */
	AAIController* AICTRL_ResolveController(const FString& ActorPath, int32& OutErrCode, FString& OutErr)
	{
		if (ActorPath.IsEmpty())
		{
			OutErrCode = kAICTRLErrorInvalidPath;
			OutErr = TEXT("controller_path is empty");
			return nullptr;
		}

		bool bAmbig = false;
		FString AmbigHint;
		FString ResolveErr;
		AActor* Actor = FMCPActorPathUtils::ResolveActor(
			ActorPath, /*bRejectPIE*/ false, bAmbig, AmbigHint, ResolveErr);
		if (!Actor)
		{
			if (bAmbig)
			{
				OutErrCode = kAICTRLErrorObjectNotFound;
				OutErr = FString::Printf(
					TEXT("controller_path '%s' is ambiguous; candidates: %s"),
					*ActorPath, *AmbigHint);
				return nullptr;
			}
			OutErrCode = kAICTRLErrorObjectNotFound;
			OutErr = FString::Printf(
				TEXT("controller_path '%s' did not resolve: %s"),
				*ActorPath, *ResolveErr);
			return nullptr;
		}
		AAIController* AIC = Cast<AAIController>(Actor);
		if (!AIC)
		{
			OutErrCode = kAICTRLErrorWrongClass;
			OutErr = FString::Printf(
				TEXT("actor '%s' is not an AAIController (got class '%s')"),
				*ActorPath, *Actor->GetClass()->GetName());
			return nullptr;
		}
		return AIC;
	}

	/** Count UAIPerceptionComponent instances attached to the controller (typically 0 or 1). */
	int32 AICTRL_CountPerceptionComponents(const AAIController* AIC)
	{
		check(AIC);
		int32 Count = 0;
		// GetComponents fills a TInlineComponentArray<UActorComponent*> — bounded by typical actor
		// component counts (< 16). Iterating-and-counting is O(N) over actor components which is the
		// most we can do without engine reflection support for "all components of class T".
		TArray<UAIPerceptionComponent*> Found;
		AIC->GetComponents<UAIPerceptionComponent>(Found);
		Count = Found.Num();
		return Count;
	}

	/**
	 * Locate the first ANavMeshBoundsVolume in the world that encompasses ``Point``. Returns null
	 * when no volume contains the point. Used by get_state to give a coarse "is this AI inside a
	 * navmesh-relevant region?" answer — the result does NOT correspond to which navdata the
	 * pathfinder actually uses (NavigationSystem assigns navdata per-agent-class, not per-volume).
	 */
	ANavMeshBoundsVolume* AICTRL_FindContainingNavMeshVolume(UWorld* World, const FVector& Point)
	{
		check(World);
		for (TActorIterator<ANavMeshBoundsVolume> It(World); It; ++It)
		{
			ANavMeshBoundsVolume* Vol = *It;
			if (!Vol) { continue; }
			if (Vol->EncompassesPoint(Point))
			{
				return Vol;
			}
		}
		return nullptr;
	}

	/**
	 * Build the per-entry JSON for ai.controller.list. Centralised so the field-set stays in lockstep
	 * with the docstring schema if either side changes.
	 */
	TSharedRef<FJsonObject> AICTRL_BuildListEntry(AAIController* AIC)
	{
		check(AIC);
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("actor_path"), FMCPActorPathUtils::BuildActorPath(AIC));
		Obj->SetStringField(TEXT("class_path"), AIC->GetClass()->GetPathName());

		if (APawn* Pawn = AIC->GetPawn())
		{
			Obj->SetStringField(TEXT("possessed_pawn"), FMCPActorPathUtils::BuildActorPath(Pawn));
		}

		Obj->SetBoolField(TEXT("has_blackboard"), AIC->GetBlackboardComponent() != nullptr);

		// "Active BT" = a BehaviorTreeComponent exists AND it has a current tree. The brain component
		// is the engine's polymorphic root; we narrow to behavior-tree-specific runtime here so the
		// flag means what the field name suggests (custom Brain subclasses won't trip it).
		UBehaviorTreeComponent* BTC = AIC->FindComponentByClass<UBehaviorTreeComponent>();
		const bool bHasActiveBT = BTC && BTC->GetCurrentTree() != nullptr;
		Obj->SetBoolField(TEXT("has_active_bt"), bHasActiveBT);

		return Obj;
	}
} // namespace

namespace FAIControllerTools
{

// ─── ai.controller.list ──────────────────────────────────────────────────────────────────────────
//
// Args:
//   class_filter?: string    optional — full /Script/... or /Game/... UClass path. When supplied,
//                            the result is filtered to subclasses of the resolved class. The class
//                            MUST derive from AAIController; non-AI-controller classes yield -32011.
//                            Default = AAIController itself (matches every AI controller).
//
// Result:
//   {
//     controllers: [
//       {
//         actor_path:    string,
//         class_path:    string,
//         possessed_pawn?: string,   // omitted when no pawn possessed
//         has_blackboard: bool,
//         has_active_bt:  bool
//       },
//       ...
//     ]
//   }
//
// Errors:
//   -32004 ObjectNotFound       class_filter supplied but doesn't resolve to a UClass
//   -32010 InvalidPath          class_filter contains backslash
//   -32011 WrongClass           class_filter resolves to a class not derived from AAIController
//   -32603 Internal             no GEditor / no editor world
//
// Notes:
//   - Empty ``controllers`` array is a valid result — maps without AI pawns yield no controllers.
//   - The brief specifies no PIE guard. The iteration target is ALWAYS the editor world
//     (GEditor->GetEditorWorldContext().World()); PIE-only controllers in the transient PIE world
//     are NOT enumerated. Callers wanting PIE-side enumeration should stop PIE first OR resolve
//     the PIE-world actor by its full PIE actor path directly via get_state.
FMCPResponse Tool_List(const FMCPRequest& Request)
{
	check(IsInGameThread());

	int32 ErrCode = 0;
	FString ErrMsg;
	UWorld* World = AICTRL_ResolveWorld(ErrCode, ErrMsg);
	if (!World)
	{
		return AICTRL_MakeError(Request, ErrCode, ErrMsg);
	}

	UClass* FilterClass = AICTRL_ResolveClassFilter(Request, ErrCode, ErrMsg);
	if (!FilterClass)
	{
		return AICTRL_MakeError(Request, ErrCode, ErrMsg);
	}

	TArray<TSharedPtr<FJsonValue>> ControllersArr;
	for (TActorIterator<AAIController> It(World); It; ++It)
	{
		AAIController* AIC = *It;
		if (!AIC) { continue; }
		if (!AIC->IsA(FilterClass)) { continue; }
		ControllersArr.Add(MakeShared<FJsonValueObject>(AICTRL_BuildListEntry(AIC)));
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetArrayField(TEXT("controllers"), ControllersArr);
	return AICTRL_MakeSuccessObj(Request, Out);
}

// ─── ai.controller.get_state ─────────────────────────────────────────────────────────────────────
//
// Args:
//   controller_path: string    required — actor path of an AAIController (any of the forms accepted
//                              by FMCPActorPathUtils::ResolveActor: canonical full, "::"-joined
//                              map+name, or bare name).
//
// Result:
//   {
//     class_path             : string,
//     possessed_pawn?        : string,    // omitted when no pawn possessed
//     active_behavior_tree?  : string,    // package path of the UBehaviorTree currently running;
//                                         // omitted when no BehaviorTreeComponent OR no current tree
//     blackboard_asset?      : string,    // package path of the UBlackboardData backing the
//                                         // BlackboardComponent; omitted when no component / no asset
//     perception_components  : int,       // count of UAIPerceptionComponent on the controller
//     navmesh_volume?        : string     // actor path of the first ANavMeshBoundsVolume containing
//                                         // the possessed pawn's location; omitted when no pawn OR
//                                         // pawn outside every volume
//   }
//
// Errors:
//   -32004 ObjectNotFound       controller_path doesn't resolve to any actor
//   -32010 InvalidPath          controller_path empty
//   -32011 WrongClass           actor resolved but is not AAIController
//   -32602 InvalidParams        missing controller_path field
//   -32603 Internal             no GEditor / no editor world
//
// Notes:
//   - The navmesh-volume lookup uses the POSSESSED PAWN's actor location, not the controller's. AI
//     controllers don't have a meaningful world location themselves (they're invisible actors); the
//     pawn is the spatial anchor. When no pawn is possessed, the field is omitted entirely.
//   - ``active_behavior_tree`` uses ``UBehaviorTreeComponent::GetCurrentTree()`` which is the
//     RUN-TIME asset (not the design-time DefaultBehaviorTree). Authoring-side BT lookup (e.g.
//     "which BT is wired to this AI's BTComponent in the CDO?") needs marshall.read_property on
//     the CDO instead.
FMCPResponse Tool_GetState(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString ControllerPath;
	FMCPResponse Err;
	if (!AICTRL_RequireStringField(Request, TEXT("controller_path"), ControllerPath, Err)) { return Err; }

	int32 ErrCode = 0;
	FString ErrMsg;
	AAIController* AIC = AICTRL_ResolveController(ControllerPath, ErrCode, ErrMsg);
	if (!AIC)
	{
		return AICTRL_MakeError(Request, ErrCode, ErrMsg);
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("class_path"), AIC->GetClass()->GetPathName());

	APawn* Pawn = AIC->GetPawn();
	if (Pawn)
	{
		Out->SetStringField(TEXT("possessed_pawn"), FMCPActorPathUtils::BuildActorPath(Pawn));
	}

	if (UBehaviorTreeComponent* BTC = AIC->FindComponentByClass<UBehaviorTreeComponent>())
	{
		if (UBehaviorTree* ActiveBT = BTC->GetCurrentTree())
		{
			Out->SetStringField(TEXT("active_behavior_tree"), ActiveBT->GetPathName());
		}
	}

	if (UBlackboardComponent* BB = AIC->GetBlackboardComponent())
	{
		if (UBlackboardData* BBAsset = BB->GetBlackboardAsset())
		{
			Out->SetStringField(TEXT("blackboard_asset"), BBAsset->GetPathName());
		}
	}

	Out->SetNumberField(TEXT("perception_components"),
		static_cast<double>(AICTRL_CountPerceptionComponents(AIC)));

	// Navmesh-volume containment uses the pawn's location. World pulled off the controller — this
	// matches the world the controller currently lives in (editor OR PIE), which is what the brief's
	// no-PIE-guard policy implies.
	if (Pawn)
	{
		if (UWorld* PawnWorld = Pawn->GetWorld())
		{
			ANavMeshBoundsVolume* Vol = AICTRL_FindContainingNavMeshVolume(
				PawnWorld, Pawn->GetActorLocation());
			if (Vol)
			{
				Out->SetStringField(TEXT("navmesh_volume"), FMCPActorPathUtils::BuildActorPath(Vol));
			}
		}
	}

	return AICTRL_MakeSuccessObj(Request, Out);
}

// ─── ai.controller.respawn_blackboard ────────────────────────────────────────────────────────────
//
// Args:
//   controller_path:        string    required — actor path of an AAIController
//   blackboard_asset_path:  string    required — package path of a UBlackboardData asset
//
// Result:
//   {
//     replaced:           bool,
//     prior_blackboard?:  string    // package path of the BBData previously installed; omitted when
//                                   // no BlackboardComponent existed before (UseBlackboard path) OR
//                                   // when the prior component had no asset
//   }
//
// Errors:
//   -32004 ObjectNotFound       controller_path / blackboard_asset_path doesn't resolve
//   -32010 InvalidPath          controller_path empty
//   -32011 WrongClass           actor isn't AAIController; asset isn't UBlackboardData
//   -32602 InvalidParams        missing required field
//
// Notes:
//   - No PIE guard per the Wave J S3 brief (this is a runtime AI-author workflow — reseating a
//     blackboard during PIE is the primary use case).
//   - Two code paths depending on prior state:
//       (a) Controller has BlackboardComponent → capture prior asset (if any) → InitializeBlackboard.
//       (b) Controller has NO BlackboardComponent → AAIController::UseBlackboard creates BOTH
//           component AND initialises asset. ``prior_blackboard`` is omitted in this path.
//   - ``replaced`` is always true on success. Failure cases above return -32004/-32011 instead.
FMCPResponse Tool_RespawnBlackboard(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString ControllerPath;
	FMCPResponse Err;
	if (!AICTRL_RequireStringField(Request, TEXT("controller_path"), ControllerPath, Err)) { return Err; }

	FString BBAssetPath;
	if (!AICTRL_RequireStringField(Request, TEXT("blackboard_asset_path"), BBAssetPath, Err)) { return Err; }

	int32 ErrCode = 0;
	FString ErrMsg;
	AAIController* AIC = AICTRL_ResolveController(ControllerPath, ErrCode, ErrMsg);
	if (!AIC)
	{
		return AICTRL_MakeError(Request, ErrCode, ErrMsg);
	}

	UBlackboardData* NewBB = LoadObject<UBlackboardData>(nullptr, *BBAssetPath);
	if (!NewBB)
	{
		return AICTRL_MakeError(Request, kAICTRLErrorObjectNotFound,
			FString::Printf(TEXT("blackboard_asset_path '%s' did not resolve to a UBlackboardData"),
				*BBAssetPath));
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();

	UBlackboardComponent* ExistingBB = AIC->GetBlackboardComponent();
	if (ExistingBB)
	{
		// Capture prior asset BEFORE InitializeBlackboard reseats. The previous asset path lets
		// callers roll back without round-tripping through their own snapshot.
		if (UBlackboardData* PriorAsset = ExistingBB->GetBlackboardAsset())
		{
			Out->SetStringField(TEXT("prior_blackboard"), PriorAsset->GetPathName());
		}
		const bool bInit = ExistingBB->InitializeBlackboard(*NewBB);
		if (!bInit)
		{
			return AICTRL_MakeError(Request, kAICTRLErrorInternal,
				FString::Printf(
					TEXT("UBlackboardComponent::InitializeBlackboard returned false for asset '%s'"),
					*BBAssetPath));
		}
	}
	else
	{
		// No prior component → UseBlackboard creates one and initialises it in one call. The out-
		// param BBOut points at the freshly-created component; we don't need to retain it past the
		// call.
		UBlackboardComponent* BBOut = nullptr;
		const bool bOk = AIC->UseBlackboard(NewBB, BBOut);
		if (!bOk || !BBOut)
		{
			return AICTRL_MakeError(Request, kAICTRLErrorInternal,
				FString::Printf(
					TEXT("AAIController::UseBlackboard returned false for asset '%s'"), *BBAssetPath));
		}
		// prior_blackboard omitted intentionally — there was no prior component.
	}

	Out->SetBoolField(TEXT("replaced"), true);
	return AICTRL_MakeSuccessObj(Request, Out);
}

// ─── Registration ────────────────────────────────────────────────────────────────────────────────
void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	RegisterTool(TEXT("ai.controller.list"),               &Tool_List,              /*Lane A*/ false);
	RegisterTool(TEXT("ai.controller.get_state"),          &Tool_GetState,          /*Lane A*/ false);
	RegisterTool(TEXT("ai.controller.respawn_blackboard"), &Tool_RespawnBlackboard, /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("Wave J Surface 3 (AIController): registered 3 ai.controller.* handlers "
			 "(list / get_state / respawn_blackboard, all Lane A, no PIE guard)"));
}

} // namespace FAIControllerTools

#include "MCPSurfaceRegistry.h"
MCP_REGISTER_SURFACE(AIControllerTools, &FAIControllerTools::Register)

#undef LOCTEXT_NAMESPACE
