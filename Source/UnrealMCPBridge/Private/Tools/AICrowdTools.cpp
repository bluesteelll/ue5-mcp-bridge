// Copyright FatumGame. All Rights Reserved.

#include "AICrowdTools.h"

#include "FMCPDispatchQueue.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPActorPathUtils.h"
#include "Utils/MCPWorldContext.h"

#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Navigation/CrowdFollowingComponent.h"
#include "Navigation/CrowdManager.h"
#include "NavigationData.h"
#include "NavigationSystem.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// AICRWD_ prefix per the unity-build symbol-collision convention. Every Tools/*.cpp anonymous-
	// namespace helper MUST be uniquely prefixed across the module — see Wave F4 BlueprintComponentTools
	// rename note in MEMORY.md for the failure mode.
	constexpr int32 kAICRWDErrorInvalidParams   = -32602;
	constexpr int32 kAICRWDErrorInternal        = -32603;
	constexpr int32 kAICRWDErrorObjectNotFound  = kMCPErrorObjectNotFound;  // -32004

	void AICRWD_StampIds(const FMCPRequest& Request, FMCPResponse& Response)
	{
		Response.RequestId = Request.RequestId;
		Response.OriginalIdString = Request.OriginalIdString;
	}

	FMCPResponse AICRWD_MakeError(const FMCPRequest& Request, int32 Code, const FString& Message)
	{
		FMCPResponse R;
		AICRWD_StampIds(Request, R);
		R.bIsError = true;
		R.ErrorCode = Code;
		R.ErrorMessage = Message;
		return R;
	}

	FMCPResponse AICRWD_MakeSuccessObj(const FMCPRequest& Request, TSharedPtr<FJsonObject> Result)
	{
		FMCPResponse R;
		AICRWD_StampIds(Request, R);
		R.bIsError = false;
		R.Result = MakeShared<FJsonValueObject>(MoveTemp(Result));
		return R;
	}

	// ─── World resolution (PIE-first, editor-fallback) ───────────────────────────────────────────

	/**
	 * Resolve the navigation-system-bearing world. Mirrors PhysicsTools / NavMeshTools convention:
	 * PIE first (so runtime introspection during gameplay works — crowd manager only exists when
	 * pawns are alive, which is typically only in PIE), editor world otherwise. Returns null only
	 * when GEditor is unavailable (commandlet / cooker contexts).
	 */
	UWorld* AICRWD_ResolveWorld()
	{
		check(IsInGameThread());
		if (GEditor && GEditor->PlayWorld)
		{
			return GEditor->PlayWorld;
		}
		return FMCPWorldContext::GetEditorWorld();
	}

	/** "editor" | "pie" — lowercase to match the convention established by navmesh.* / physics.*. */
	const TCHAR* AICRWD_WorldKindName(const UWorld* World)
	{
		if (!World) { return TEXT("none"); }
		return (World->WorldType == EWorldType::PIE) ? TEXT("pie") : TEXT("editor");
	}

	/** Stringify ECrowdAvoidanceQuality::Type — used by list_agents + set_avoidance_quality response. */
	const TCHAR* AICRWD_QualityToString(ECrowdAvoidanceQuality::Type Quality)
	{
		switch (Quality)
		{
			case ECrowdAvoidanceQuality::Low:    return TEXT("Low");
			case ECrowdAvoidanceQuality::Medium: return TEXT("Medium");
			// ECrowdAvoidanceQuality::Good sits between Medium and High in the enum (4 values total)
			// — the brief's externally-visible enum only exposes Low/Medium/High. We stringify Good
			// AS "Good" for transparency in list_agents readouts (it can be the current value if a
			// developer set it via the underlying byte UPROPERTY directly), but set_avoidance_quality
			// will not accept "Good" as input.
			case ECrowdAvoidanceQuality::Good:   return TEXT("Good");
			case ECrowdAvoidanceQuality::High:   return TEXT("High");
		}
		return TEXT("Unknown");
	}

	/**
	 * Parse one of the 3 string forms ("Low" / "Medium" / "High") into the enum. Returns false +
	 * populates OutError if the string is anything else. Case-sensitive match per the brief's
	 * literal arg contract.
	 */
	bool AICRWD_ParseQuality(const FString& In, ECrowdAvoidanceQuality::Type& Out, FString& OutError)
	{
		if (In == TEXT("Low"))    { Out = ECrowdAvoidanceQuality::Low;    return true; }
		if (In == TEXT("Medium")) { Out = ECrowdAvoidanceQuality::Medium; return true; }
		if (In == TEXT("High"))   { Out = ECrowdAvoidanceQuality::High;   return true; }
		OutError = FString::Printf(
			TEXT("quality '%s' is not one of \"Low\" / \"Medium\" / \"High\""), *In);
		return false;
	}

	/** Convert FVector → JSON [x,y,z] array. */
	TArray<TSharedPtr<FJsonValue>> AICRWD_VectorToArray(const FVector& V)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Add(MakeShared<FJsonValueNumber>(V.X));
		Arr.Add(MakeShared<FJsonValueNumber>(V.Y));
		Arr.Add(MakeShared<FJsonValueNumber>(V.Z));
		return Arr;
	}

	// ─── UPROPERTY reflection helpers ─────────────────────────────────────────────────────────────
	//
	// UCrowdManager's interesting tunables (MaxAgents, MaxAgentRadius, MaxAvoidedAgents,
	// MaxAvoidedWalls, NavmeshCheckInterval, PathOptimizationInterval, SeparationDirClamp,
	// PathOffsetRadiusMultiplier, bResolveCollisions) are protected UPROPERTYs. The public surface
	// exposes no getters for them. Reflection via FindPropertyByName + ContainerPtrToValuePtr is
	// the only way to read them from outside the class hierarchy without an engine patch.
	//
	// All these reads are silent on missing fields — if a future engine revision renames one of
	// the properties the corresponding response field is simply omitted. We log nothing at lookup
	// time because the result still carries the world+actor info that the caller needs to retry
	// against the new schema.

	/** Read an int32 UPROPERTY by name; returns false if missing or wrong type. */
	bool AICRWD_TryReadInt32Prop(UObject* Owner, const TCHAR* PropName, int32& OutValue)
	{
		check(Owner);
		FProperty* Prop = Owner->GetClass()->FindPropertyByName(FName(PropName));
		if (!Prop) { return false; }
		FIntProperty* IntProp = CastField<FIntProperty>(Prop);
		if (!IntProp) { return false; }
		OutValue = IntProp->GetPropertyValue_InContainer(Owner);
		return true;
	}

	/** Read a float UPROPERTY by name; returns false if missing or wrong type. */
	bool AICRWD_TryReadFloatProp(UObject* Owner, const TCHAR* PropName, float& OutValue)
	{
		check(Owner);
		FProperty* Prop = Owner->GetClass()->FindPropertyByName(FName(PropName));
		if (!Prop) { return false; }
		FFloatProperty* FloatProp = CastField<FFloatProperty>(Prop);
		if (!FloatProp) { return false; }
		OutValue = FloatProp->GetPropertyValue_InContainer(Owner);
		return true;
	}

	/**
	 * Read a TArray<T> UPROPERTY by name and report its element count via reflection. Caller never
	 * sees the underlying elements — we use this only to surface "how many avoidance config slots
	 * are populated" / "how many sampling patterns are registered" for diagnostic visibility.
	 */
	bool AICRWD_TryReadArrayCountProp(UObject* Owner, const TCHAR* PropName, int32& OutCount)
	{
		check(Owner);
		FProperty* Prop = Owner->GetClass()->FindPropertyByName(FName(PropName));
		if (!Prop) { return false; }
		FArrayProperty* ArrProp = CastField<FArrayProperty>(Prop);
		if (!ArrProp) { return false; }
		const void* ContainerAddr = ArrProp->ContainerPtrToValuePtr<void>(Owner);
		FScriptArrayHelper Helper(ArrProp, ContainerAddr);
		OutCount = Helper.Num();
		return true;
	}

	// ─── Component resolution by actor_path ─────────────────────────────────────────────────────────

	/**
	 * Resolve an actor_path → UCrowdFollowingComponent. Mirrors NavMeshTools' RNM-resolve helper
	 * pattern but for components. Returns null + writes ``OutErr`` on either actor-resolution
	 * failure (not found / ambiguous) OR no UCrowdFollowingComponent attached.
	 *
	 * PIE is permitted (bRejectPIE=false) — set_avoidance_quality is a runtime mutator with no
	 * editor-asset side-effects.
	 */
	UCrowdFollowingComponent* AICRWD_ResolveCrowdFollowingComponent(
		const FString& ActorPath, FString& OutErr)
	{
		bool bAmbig = false;
		FString AmbigHint;
		AActor* Actor = FMCPActorPathUtils::ResolveActor(
			ActorPath, /*bRejectPIE*/ false, bAmbig, AmbigHint, OutErr);
		if (!Actor)
		{
			// Ambiguity hint is embedded by ResolveActor in OutErr already; surfaces with -32004.
			return nullptr;
		}
		UCrowdFollowingComponent* Agent = Actor->FindComponentByClass<UCrowdFollowingComponent>();
		if (!Agent)
		{
			OutErr = FString::Printf(
				TEXT("actor '%s' has no UCrowdFollowingComponent attached"), *ActorPath);
			return nullptr;
		}
		return Agent;
	}

	// ─── Per-agent JSON shape (shared between list_agents and any future single-agent read) ─────────

	TSharedRef<FJsonObject> AICRWD_BuildAgentJson(UCrowdFollowingComponent* Agent)
	{
		check(Agent);
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();

		const AActor* Owner = Agent->GetOwner();
		// Owner may be null in rare half-torn-down states; surface as empty string rather than crashing.
		Obj->SetStringField(TEXT("owner_actor_path"),
			Owner ? FMCPActorPathUtils::BuildActorPath(Owner) : FString());

		// GetCrowdAgentVelocity is the ICrowdAgentInterface accessor — returns the velocity that
		// detour-crowd is currently driving. For an inactive/idle agent this is ZeroVector.
		// IMPORTANT: do NOT use AActor::GetVelocity() — that returns the root component's physics
		// velocity which is NOT what detour-crowd is computing.
		const FVector Vel = Agent->GetCrowdAgentVelocity();
		Obj->SetArrayField(TEXT("current_velocity"), AICRWD_VectorToArray(Vel));

		Obj->SetStringField(TEXT("avoidance_quality"),
			AICRWD_QualityToString(Agent->GetCrowdAvoidanceQuality()));

		// ICrowdAgentInterface::GetCrowdAgentCollisions fills (radius, half_height) via out-params.
		// Many movement-component implementations don't override this — defaults to (0, 0) per
		// the base interface stub. We surface radius regardless; callers see 0 when the agent
		// hasn't published its cylinder collision shape.
		float CylRadius = 0.0f;
		float CylHalfHeight = 0.0f;
		Agent->GetCrowdAgentCollisions(CylRadius, CylHalfHeight);
		Obj->SetNumberField(TEXT("radius"), static_cast<double>(CylRadius));

		return Obj;
	}
} // namespace

namespace FAICrowdTools
{

// ─── ai.crowd.get_settings ─────────────────────────────────────────────────────────────────────
//
// Args: (no args)
//
// Result:
//   {
//     world                       : "editor" | "pie",
//     has_nav_data                : bool,            // CrowdManager has a bound ANavigationData
//     max_agents                  : int,             // UCrowdManager::MaxAgents (config UPROPERTY)
//     max_agent_radius            : float,           // UCrowdManager::MaxAgentRadius (config UPROPERTY)
//     max_avoided_agents          : int,             // UCrowdManager::MaxAvoidedAgents
//     max_avoided_walls           : int,             // UCrowdManager::MaxAvoidedWalls
//     navmesh_check_interval      : float,           // UCrowdManager::NavmeshCheckInterval (seconds)
//     path_optimization_interval  : float,           // UCrowdManager::PathOptimizationInterval (s)
//     separation_dir_clamp        : float,           // UCrowdManager::SeparationDirClamp
//     path_offset_radius_mult     : float,           // UCrowdManager::PathOffsetRadiusMultiplier
//     num_avoidance_configs       : int,             // AvoidanceConfig[] length
//     num_sampling_patterns       : int              // SamplingPatterns[] length
//   }
//
// Errors:
//   -32603 Internal   no world / no UNavigationSystemV1 / no UCrowdManager registered
//
// Notes:
//   - The brief's literal {anticipation_time, max_avoid_velocity, num_velocity_samples} fields
//     don't exist directly on UCrowdManager — those are per-config-slot inside
//     ``FCrowdAvoidanceConfig[]`` (with fields like VelocityBias, ImpactTimeWeight, etc.). A
//     future ``ai.crowd.get_avoidance_config(index)`` tool would expose those per-slot. This
//     tool exposes the actual top-level singleton properties.
//   - All scalar fields are read via UPROPERTY reflection (FindPropertyByName) because UCrowdManager
//     declares them ``protected``. Missing fields (engine API renamed in a future version) are
//     silently omitted from the response — callers that pivot on a field's presence should test
//     via ``has_key`` rather than assuming the field exists.
//   - ``num_avoidance_configs`` is the count of registered avoidance presets (used by detour's
//     velocity sampler). ``num_sampling_patterns`` is the count of registered fixed sampling
//     patterns (alternative to adaptive sampling). Both default to populated in standard projects.
//   - NO PIE guard — read-only introspection works in both editor and PIE worlds.
FMCPResponse Tool_GetSettings(const FMCPRequest& Request)
{
	check(IsInGameThread());

	UWorld* World = AICRWD_ResolveWorld();
	if (!World)
	{
		return AICRWD_MakeError(Request, kAICRWDErrorInternal,
			TEXT("no world available (GEditor missing OR no level loaded)"));
	}

	UNavigationSystemV1* NavSys = UNavigationSystemV1::GetCurrent(World);
	if (!NavSys)
	{
		return AICRWD_MakeError(Request, kAICRWDErrorInternal,
			TEXT("no UNavigationSystemV1 on the resolved world (navigation subsystem disabled?)"));
	}

	UCrowdManager* CM = UCrowdManager::GetCurrent(World);
	if (!CM)
	{
		return AICRWD_MakeError(Request, kAICRWDErrorInternal,
			TEXT("no UCrowdManager on the navigation subsystem (no NavMeshBoundsVolume in scope, OR "
				 "the project's NavigationSystem CrowdManagerClass is unset)"));
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("world"), AICRWD_WorldKindName(World));
	Out->SetBoolField(TEXT("has_nav_data"), CM->GetNavData() != nullptr);

	// Scalar UPROPERTY reads via reflection (all protected). Order matches the doc comment above.
	int32 IntVal = 0;
	float FloatVal = 0.0f;
	if (AICRWD_TryReadInt32Prop(CM, TEXT("MaxAgents"), IntVal))
	{
		Out->SetNumberField(TEXT("max_agents"), IntVal);
	}
	if (AICRWD_TryReadFloatProp(CM, TEXT("MaxAgentRadius"), FloatVal))
	{
		Out->SetNumberField(TEXT("max_agent_radius"), static_cast<double>(FloatVal));
	}
	if (AICRWD_TryReadInt32Prop(CM, TEXT("MaxAvoidedAgents"), IntVal))
	{
		Out->SetNumberField(TEXT("max_avoided_agents"), IntVal);
	}
	if (AICRWD_TryReadInt32Prop(CM, TEXT("MaxAvoidedWalls"), IntVal))
	{
		Out->SetNumberField(TEXT("max_avoided_walls"), IntVal);
	}
	if (AICRWD_TryReadFloatProp(CM, TEXT("NavmeshCheckInterval"), FloatVal))
	{
		Out->SetNumberField(TEXT("navmesh_check_interval"), static_cast<double>(FloatVal));
	}
	if (AICRWD_TryReadFloatProp(CM, TEXT("PathOptimizationInterval"), FloatVal))
	{
		Out->SetNumberField(TEXT("path_optimization_interval"), static_cast<double>(FloatVal));
	}
	if (AICRWD_TryReadFloatProp(CM, TEXT("SeparationDirClamp"), FloatVal))
	{
		Out->SetNumberField(TEXT("separation_dir_clamp"), static_cast<double>(FloatVal));
	}
	if (AICRWD_TryReadFloatProp(CM, TEXT("PathOffsetRadiusMultiplier"), FloatVal))
	{
		Out->SetNumberField(TEXT("path_offset_radius_mult"), static_cast<double>(FloatVal));
	}
	if (AICRWD_TryReadArrayCountProp(CM, TEXT("AvoidanceConfig"), IntVal))
	{
		Out->SetNumberField(TEXT("num_avoidance_configs"), IntVal);
	}
	if (AICRWD_TryReadArrayCountProp(CM, TEXT("SamplingPatterns"), IntVal))
	{
		Out->SetNumberField(TEXT("num_sampling_patterns"), IntVal);
	}
	return AICRWD_MakeSuccessObj(Request, Out);
}

// ─── ai.crowd.list_agents ──────────────────────────────────────────────────────────────────────
//
// Args: (no args)
//
// Result:
//   {
//     world  : "editor" | "pie",
//     count  : int,
//     agents : [
//       {
//         owner_actor_path  : string,   // canonical Actor->GetPathName() via MCPActorPathUtils
//         current_velocity  : [x,y,z],  // ICrowdAgentInterface::GetCrowdAgentVelocity
//         avoidance_quality : "Low" | "Medium" | "Good" | "High",  // current setting
//         radius            : float     // cylinder radius from GetCrowdAgentCollisions
//       },
//       ...
//     ]
//   }
//
// Errors:
//   -32603 Internal   no world (GEditor missing / no level loaded)
//
// Notes:
//   - Enumerates via TObjectIterator<UCrowdFollowingComponent> filtered to ``Agent->GetWorld() ==
//     World``. This catches every PathFollowingComponent subclass instance — the conventional
//     setup is one per AIController whose Pawn is registered with the crowd manager.
//   - Empty ``agents`` array is a LEGITIMATE result, not an error: many editor maps have no AI
//     pawns spawned, OR the AIController setup uses vanilla UPathFollowingComponent without crowd
//     simulation. The brief explicitly treats this as a SKIP-rest scenario, not -32603.
//   - Skips pending-kill / unreachable components defensively. ``IsValidLowLevel`` guards transient
//     half-torn-down state. ``HasAnyFlags(RF_BeginDestroyed)`` filters components in the middle of
//     GC tear-down.
//   - No UCrowdManager check here — list_agents is purely a component-enumeration view; it works
//     even when the crowd manager has never been instantiated (the components still exist on the
//     pawns, they just won't be steering each other).
//   - NO PIE guard — runtime read-only enumeration.
FMCPResponse Tool_ListAgents(const FMCPRequest& Request)
{
	check(IsInGameThread());

	UWorld* World = AICRWD_ResolveWorld();
	if (!World)
	{
		return AICRWD_MakeError(Request, kAICRWDErrorInternal,
			TEXT("no world available (GEditor missing OR no level loaded)"));
	}

	TArray<TSharedPtr<FJsonValue>> Arr;
	for (TObjectIterator<UCrowdFollowingComponent> It; It; ++It)
	{
		UCrowdFollowingComponent* Agent = *It;
		if (!Agent || !Agent->IsValidLowLevel()) { continue; }
		if (Agent->HasAnyFlags(RF_BeginDestroyed)) { continue; }
		if (Agent->GetWorld() != World) { continue; }
		Arr.Add(MakeShared<FJsonValueObject>(AICRWD_BuildAgentJson(Agent)));
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("world"), AICRWD_WorldKindName(World));
	Out->SetNumberField(TEXT("count"), Arr.Num());
	Out->SetArrayField(TEXT("agents"), Arr);
	return AICRWD_MakeSuccessObj(Request, Out);
}

// ─── ai.crowd.set_avoidance_quality ────────────────────────────────────────────────────────────
//
// Args:
//   actor_path : string                            required — resolves to an actor whose first
//                                                  UCrowdFollowingComponent receives the new setting
//   quality    : "Low" | "Medium" | "High"         required — case-sensitive
//
// Result:
//   {
//     world             : "editor" | "pie",
//     set               : true,
//     actor_path        : string,                  echo of resolved actor's canonical path
//     prior_quality     : "Low" | "Medium" | "Good" | "High",  // prior setting (may have been "Good")
//     new_quality       : "Low" | "Medium" | "High"            // the new setting (one of the 3 accepted)
//   }
//
// Errors:
//   -32004 ObjectNotFound   actor_path didn't resolve OR no UCrowdFollowingComponent attached
//   -32010 InvalidPath      malformed actor_path
//   -32602 InvalidParams    missing actor_path / quality OR quality is not one of the 3 strings
//   -32603 Internal         no world available
//
// Notes:
//   - Uses ``UCrowdFollowingComponent::SetCrowdAvoidanceQuality(Q, /*bUpdateAgent*/true)`` so the
//     change propagates into detour-crowd's per-agent params immediately via the agent's
//     ``UpdateCrowdAgentParams`` callback. Without bUpdateAgent=true the UPROPERTY would update but
//     the live detour state would lag until the next path-change event.
//   - ``prior_quality`` echoes the engine's enum stringification — if a developer previously set
//     the agent to ``ECrowdAvoidanceQuality::Good`` via marshall.set_property or C++ code, that
//     value surfaces here (even though the input enum forbids "Good"). This is intentional: gives
//     callers visibility into the underlying field even when restricted to a subset for writes.
//   - NO PIE guard / NO FScopedTransaction — runtime transient mutation, no editor undo state.
FMCPResponse Tool_SetAvoidanceQuality(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!Request.Args.IsValid())
	{
		return AICRWD_MakeError(Request, kAICRWDErrorInvalidParams, TEXT("missing args object"));
	}

	FString ActorPath;
	if (!Request.Args->TryGetStringField(TEXT("actor_path"), ActorPath) || ActorPath.IsEmpty())
	{
		return AICRWD_MakeError(Request, kAICRWDErrorInvalidParams,
			TEXT("missing required string field 'actor_path'"));
	}

	FString QualityStr;
	if (!Request.Args->TryGetStringField(TEXT("quality"), QualityStr) || QualityStr.IsEmpty())
	{
		return AICRWD_MakeError(Request, kAICRWDErrorInvalidParams,
			TEXT("missing required string field 'quality' (one of \"Low\" / \"Medium\" / \"High\")"));
	}

	// Parse quality first (cheap) before the more expensive actor resolution — surfaces bad input
	// without paying the cost of a level-actors scan.
	ECrowdAvoidanceQuality::Type NewQuality;
	FString ParseErr;
	if (!AICRWD_ParseQuality(QualityStr, NewQuality, ParseErr))
	{
		return AICRWD_MakeError(Request, kAICRWDErrorInvalidParams, ParseErr);
	}

	UWorld* World = AICRWD_ResolveWorld();
	if (!World)
	{
		return AICRWD_MakeError(Request, kAICRWDErrorInternal,
			TEXT("no world available (GEditor missing OR no level loaded)"));
	}

	FString ResolveErr;
	UCrowdFollowingComponent* Agent = AICRWD_ResolveCrowdFollowingComponent(ActorPath, ResolveErr);
	if (!Agent)
	{
		return AICRWD_MakeError(Request, kAICRWDErrorObjectNotFound,
			FString::Printf(TEXT("actor_path '%s' did not resolve: %s"), *ActorPath, *ResolveErr));
	}

	const ECrowdAvoidanceQuality::Type PriorQuality = Agent->GetCrowdAvoidanceQuality();
	Agent->SetCrowdAvoidanceQuality(NewQuality, /*bUpdateAgent*/ true);

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("world"), AICRWD_WorldKindName(World));
	Out->SetBoolField(TEXT("set"), true);

	const AActor* OwnerActor = Agent->GetOwner();
	Out->SetStringField(TEXT("actor_path"),
		OwnerActor ? FMCPActorPathUtils::BuildActorPath(OwnerActor) : ActorPath);
	Out->SetStringField(TEXT("prior_quality"), AICRWD_QualityToString(PriorQuality));
	Out->SetStringField(TEXT("new_quality"),   AICRWD_QualityToString(NewQuality));
	return AICRWD_MakeSuccessObj(Request, Out);
}

// ─── Registration ──────────────────────────────────────────────────────────────────────────────
void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	RegisterTool(TEXT("ai.crowd.get_settings"),           &Tool_GetSettings,         /*Lane A*/ false);
	RegisterTool(TEXT("ai.crowd.list_agents"),            &Tool_ListAgents,          /*Lane A*/ false);
	RegisterTool(TEXT("ai.crowd.set_avoidance_quality"),  &Tool_SetAvoidanceQuality, /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("Wave J Surface 6 (AICrowd): registered 3 ai.crowd.* handlers "
			 "(get_settings / list_agents / set_avoidance_quality, all Lane A)"));
}

} // namespace FAICrowdTools

// Wave J auto-registration via FMCPSurfaceRegistry — replaces the manual include + Register call
// in UnrealMCPBridge.cpp (Wave I refactor).
#include "MCPSurfaceRegistry.h"
MCP_REGISTER_SURFACE(AICrowdTools, &FAICrowdTools::Register)

#undef LOCTEXT_NAMESPACE
