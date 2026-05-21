// Copyright FatumGame. All Rights Reserved.

#include "NavMeshTools.h"

#include "MCPSurfaceRegistry.h"

#include "FMCPDispatchQueue.h"
#include "MCPToolHelpers.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPActorPathUtils.h"
#include "Utils/MCPWorldContext.h"

#include "AI/Navigation/NavigationTypes.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "NavMesh/RecastNavMesh.h"
#include "NavigationData.h"
#include "NavigationPath.h"
#include "NavigationSystem.h"
#include "UObject/Package.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// NAV_ prefix per the unity-build symbol-collision convention. The four shared helpers
	// (StampIds / MakeError / MakeSuccessObj / RequireStringField) live in FMCPToolHelpers — see
	// Phase 1 helper extraction (commit b2fd19d).
	constexpr int32 kNAVErrorInvalidParams   = -32602;
	constexpr int32 kNAVErrorInternal        = -32603;
	constexpr int32 kNAVErrorObjectNotFound  = kMCPErrorObjectNotFound;  // -32004
	constexpr int32 kNAVErrorPIEActive       = kMCPErrorPIEActive;       // -32027

	// ─── World resolution (PIE-first, editor-fallback) ───────────────────────────────────────────

	/**
	 * Resolve the navigation-system-bearing world. Mirrors PhysicsTools / DebugTools convention:
	 * PIE first (so runtime introspection during gameplay works), editor world otherwise. Returns
	 * null only when GEditor is unavailable (commandlet / cooker contexts).
	 */
	UWorld* NAV_ResolveWorld()
	{
		check(IsInGameThread());
		if (GEditor && GEditor->PlayWorld)
		{
			return GEditor->PlayWorld;
		}
		return FMCPWorldContext::GetEditorWorld();
	}

	/** "editor" | "pie" — kept lowercase to match the brief's response-shape contract. */
	const TCHAR* NAV_WorldKindName(const UWorld* World)
	{
		if (!World) { return TEXT("none"); }
		return (World->WorldType == EWorldType::PIE) ? TEXT("pie") : TEXT("editor");
	}

	// ─── JSON parsing helpers ────────────────────────────────────────────────────────────────────

	/** Parse required [x,y,z] array. Returns false + populates OutError when missing or malformed. */
	bool NAV_ParseVector3(const TSharedPtr<FJsonObject>& Args, const TCHAR* Field, FVector& Out, FString& OutError)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Args.IsValid() || !Args->TryGetArrayField(Field, Arr) || !Arr)
		{
			OutError = FString::Printf(TEXT("missing required array field '%s' ([x,y,z])"), Field);
			return false;
		}
		if (Arr->Num() != 3)
		{
			OutError = FString::Printf(
				TEXT("'%s' must be [x,y,z] (3 numbers); got %d entries"), Field, Arr->Num());
			return false;
		}
		double X = 0.0, Y = 0.0, Z = 0.0;
		if (!(*Arr)[0]->TryGetNumber(X) || !(*Arr)[1]->TryGetNumber(Y) || !(*Arr)[2]->TryGetNumber(Z))
		{
			OutError = FString::Printf(TEXT("'%s' entries must all be numbers"), Field);
			return false;
		}
		Out = FVector(X, Y, Z);
		return true;
	}

	/** Optional [x,y,z] with default. Missing → default; malformed → error. */
	bool NAV_ParseOptionalVector3(
		const TSharedPtr<FJsonObject>& Args,
		const TCHAR* Field,
		const FVector& Default,
		FVector& Out,
		FString& OutError)
	{
		if (!Args.IsValid() || !Args->HasField(Field))
		{
			Out = Default;
			return true;
		}
		return NAV_ParseVector3(Args, Field, Out, OutError);
	}

	/** Convert FVector → JSON [x,y,z] array. */
	TArray<TSharedPtr<FJsonValue>> NAV_VectorToArray(const FVector& V)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Add(MakeShared<FJsonValueNumber>(V.X));
		Arr.Add(MakeShared<FJsonValueNumber>(V.Y));
		Arr.Add(MakeShared<FJsonValueNumber>(V.Z));
		return Arr;
	}

	// ─── ARecastNavMesh → JSON ───────────────────────────────────────────────────────────────────

	TSharedRef<FJsonObject> NAV_BuildRecastNavMeshJson(const ARecastNavMesh* RNM)
	{
		check(RNM);
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("actor_path"), FMCPActorPathUtils::BuildActorPath(RNM));
		Obj->SetNumberField(TEXT("agent_radius"),  static_cast<double>(RNM->AgentRadius));
		Obj->SetNumberField(TEXT("agent_height"),  static_cast<double>(RNM->AgentHeight));
		Obj->SetNumberField(TEXT("cell_size"),     static_cast<double>(RNM->CellSize));
		Obj->SetNumberField(TEXT("tile_size_uu"),  static_cast<double>(RNM->TileSizeUU));
		// HasValidNavmesh() returns false when the recast tile cache is empty (no build run yet).
		// We expose it as `is_initialized` so callers can distinguish "navmesh actor present but
		// empty" from "navmesh actor present + has tile data".
		Obj->SetBoolField(TEXT("is_initialized"), RNM->HasValidNavmesh());
		return Obj;
	}

	// ─── ARecastNavMesh resolution by actor_path (optional arg) ─────────────────────────────────

	/**
	 * Resolve an ARecastNavMesh by actor_path. Returns null + writes ``OutErr`` on failure.
	 * Path resolution permits PIE actors (read-only family of tools — caller may want to introspect
	 * the PIE navmesh).
	 */
	ARecastNavMesh* NAV_ResolveRecastNavMesh(const FString& ActorPath, FString& OutErr)
	{
		bool bAmbig = false;
		FString AmbigHint;
		AActor* Actor = FMCPActorPathUtils::ResolveActor(
			ActorPath, /*bRejectPIE*/ false, bAmbig, AmbigHint, OutErr);
		if (!Actor)
		{
			return nullptr;
		}
		ARecastNavMesh* RNM = Cast<ARecastNavMesh>(Actor);
		if (!RNM)
		{
			OutErr = FString::Printf(
				TEXT("actor '%s' is not an ARecastNavMesh (got class '%s')"),
				*ActorPath, *Actor->GetClass()->GetName());
			return nullptr;
		}
		return RNM;
	}
} // namespace

namespace FNavMeshTools
{

// ─── navmesh.list ──────────────────────────────────────────────────────────────────────────────
//
// Args: (no args)
//
// Result:
//   {
//     world      : "editor" | "pie",
//     navmeshes  : [
//       { actor_path, agent_radius, agent_height, cell_size, tile_size_uu, is_initialized }, ...
//     ]
//   }
//
// Errors:
//   -32603 Internal     no world available (commandlet / no map loaded)
//                       OR no UNavigationSystemV1 on the resolved world
//
// Notes:
//   - Walks ``UNavigationSystemV1::NavDataSet`` casting each entry to ARecastNavMesh; non-recast
//     navdata (e.g. custom subclasses) are silently SKIPPED. Caller expecting per-agent-class
//     enumeration of arbitrary NavData subclasses can read AgentDefinitions via marshall.read_property
//     against the NavigationSystem subsystem itself.
//   - Empty ``navmeshes`` array is a valid result — many editor maps don't have a NavMeshBoundsVolume
//     placed, so no recast actor exists.
FMCPResponse Tool_List(const FMCPRequest& Request)
{
	check(IsInGameThread());

	UWorld* World = NAV_ResolveWorld();
	if (!World)
	{
		return FMCPToolHelpers::MakeError(Request, kNAVErrorInternal,
			TEXT("no world available (GEditor missing OR no level loaded)"));
	}

	UNavigationSystemV1* NavSys = UNavigationSystemV1::GetCurrent(World);
	if (!NavSys)
	{
		return FMCPToolHelpers::MakeError(Request, kNAVErrorInternal,
			TEXT("no UNavigationSystemV1 on the resolved world (navigation subsystem disabled?)"));
	}

	TArray<TSharedPtr<FJsonValue>> NavMeshesArr;
	const TArray<TObjectPtr<ANavigationData>>& NavDataList = NavSys->NavDataSet;
	NavMeshesArr.Reserve(NavDataList.Num());
	for (ANavigationData* ND : NavDataList)
	{
		if (!ND) { continue; }
		ARecastNavMesh* RNM = Cast<ARecastNavMesh>(ND);
		if (!RNM) { continue; }  // Skip non-recast navdata subclasses.
		NavMeshesArr.Add(MakeShared<FJsonValueObject>(NAV_BuildRecastNavMeshJson(RNM)));
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("world"), NAV_WorldKindName(World));
	Out->SetArrayField(TEXT("navmeshes"), NavMeshesArr);
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── navmesh.rebuild ───────────────────────────────────────────────────────────────────────────
//
// Args:
//   navmesh_actor_path?: string   when omitted → UNavigationSystemV1::Build() (rebuilds ALL navdata)
//
// Result:
//   {
//     rebuilt              : true,
//     navmesh_actor_path   : string | null,    // null when no per-actor target (system-wide build)
//     duration_seconds     : float,            // wall-clock for the Build() / RebuildAll() call
//                                              // (note: build is ASYNC — this measures kick-off, not
//                                              // completion. Caller polls navmesh.list to see
//                                              // is_initialized flip.)
//     world                : "editor"
//   }
//
// Errors:
//   -32027 PIEActive            attempted during PIE (build is editor-time only on this surface)
//   -32004 ObjectNotFound       navmesh_actor_path supplied but doesn't resolve to ARecastNavMesh
//   -32603 Internal             no world OR no NavigationSystem
//
// Notes:
//   - PIE-guarded. Runtime navmesh rebuilds for dynamic-nav-mesh are out of scope; the engine's
//     dynamic-rebuild path requires AddNavigationRelevantActor / RemoveNavigationRelevantActor on
//     specific actors and is best modelled by a future ``navmesh.update_dynamic_*`` surface.
//   - When ``navmesh_actor_path`` is omitted, ``UNavigationSystemV1::Build()`` is called which
//     rebuilds EVERY navdata in NavDataSet (typically just one ARecastNavMesh per agent class). When
//     supplied, the specific actor's ``RebuildAll()`` is called.
//   - duration_seconds is the build-kickoff cost only. The actual tile generation runs asynchronously
//     on the navigation task graph — callers wanting to wait for completion poll ``navmesh.list``
//     and check ``is_initialized`` (or marshall.read_property NavigationSystem.bIsBuildInProgress).
FMCPResponse Tool_Rebuild(const FMCPRequest& Request)
{
	check(IsInGameThread());

	// PIE guard FIRST — matches Phase 3 mutator convention. Frozen message text per MCPTypes.h.
	if (FMCPWorldContext::IsPIEActive())
	{
		return FMCPToolHelpers::MakeError(Request, kNAVErrorPIEActive, kMCPMessagePIEActive);
	}

	UWorld* World = NAV_ResolveWorld();
	if (!World)
	{
		return FMCPToolHelpers::MakeError(Request, kNAVErrorInternal,
			TEXT("no world available (GEditor missing OR no level loaded)"));
	}

	UNavigationSystemV1* NavSys = UNavigationSystemV1::GetCurrent(World);
	if (!NavSys)
	{
		return FMCPToolHelpers::MakeError(Request, kNAVErrorInternal,
			TEXT("no UNavigationSystemV1 on the resolved world (navigation subsystem disabled?)"));
	}

	// Optional navmesh_actor_path — when present, narrow rebuild to that one actor. Missing OR empty
	// (string supplied but blank) → system-wide Build().
	FString NavMeshPath;
	Request.Args.IsValid() && Request.Args->TryGetStringField(TEXT("navmesh_actor_path"), NavMeshPath);

	ARecastNavMesh* TargetRNM = nullptr;
	if (!NavMeshPath.IsEmpty())
	{
		FString ResolveErr;
		TargetRNM = NAV_ResolveRecastNavMesh(NavMeshPath, ResolveErr);
		if (!TargetRNM)
		{
			return FMCPToolHelpers::MakeError(Request, kNAVErrorObjectNotFound,
				FString::Printf(TEXT("navmesh_actor_path '%s' did not resolve: %s"),
					*NavMeshPath, *ResolveErr));
		}
	}

	const double StartSec = FPlatformTime::Seconds();
	if (TargetRNM)
	{
		TargetRNM->RebuildAll();
	}
	else
	{
		NavSys->Build();
	}
	const double DurationSec = FPlatformTime::Seconds() - StartSec;

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("rebuilt"), true);
	if (TargetRNM)
	{
		Out->SetStringField(TEXT("navmesh_actor_path"), FMCPActorPathUtils::BuildActorPath(TargetRNM));
	}
	else
	{
		Out->SetField(TEXT("navmesh_actor_path"), MakeShared<FJsonValueNull>());
	}
	Out->SetNumberField(TEXT("duration_seconds"), DurationSec);
	Out->SetStringField(TEXT("world"), NAV_WorldKindName(World));
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── navmesh.find_path ─────────────────────────────────────────────────────────────────────────
//
// Args:
//   start         : [x, y, z]                         required
//   end           : [x, y, z]                         required
//   agent_class?  : string (default = default agent)  RESERVED — not currently wired (resolves to
//                                                     default nav data regardless; future revision
//                                                     can map agent class to a specific NavData entry)
//   tolerance?    : float (default 50.0)              RESERVED — not currently wired
//
// Result:
//   {
//     found       : bool,                  true iff FindPathSync returned a successful + valid path
//     path_length : float,                 cumulative distance along the path (0 when !found)
//     waypoints   : [ [x,y,z], ... ],      ordered start → end; empty when !found
//     world       : "editor" | "pie"
//   }
//
// Errors:
//   -32602 InvalidParams       missing/malformed start/end
//   -32603 Internal            no world OR no NavigationSystem OR no default nav data
//
// Notes:
//   - Uses ``UNavigationSystemV1::FindPathSync`` (blocking; runs the recast string-pull on the calling
//     thread). For long paths in PIE this can hitch — typical 5k-cm path completes in <1ms.
//   - ``agent_class`` / ``tolerance`` are accepted for forward-compat but not yet routed. A path
//     query against a non-default agent currently still resolves to the default nav data. Future
//     revision: walk NavDataSet matching FNavAgentProperties::PreferredNavData and pass that to the
//     query.
//   - When no navmesh is built (or no NavMeshBoundsVolume in the scene), FindPathSync returns
//     ``EPathFindingResult::Error`` and ``found=false`` with ``waypoints=[]``. This is NOT raised
//     as -32603; it's a legitimate "no path" result that the caller's planner consumes.
//   - ``path_length`` is the recast-reported total path distance, NOT the straight-line Manhattan
//     distance. A zigzag around obstacles can produce path_length >> Distance(start, end).
FMCPResponse Tool_FindPath(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kNAVErrorInvalidParams, TEXT("missing args object"));
	}

	FVector Start, End;
	FString ArgErr;
	if (!NAV_ParseVector3(Request.Args, TEXT("start"), Start, ArgErr))
	{
		return FMCPToolHelpers::MakeError(Request, kNAVErrorInvalidParams, ArgErr);
	}
	if (!NAV_ParseVector3(Request.Args, TEXT("end"), End, ArgErr))
	{
		return FMCPToolHelpers::MakeError(Request, kNAVErrorInvalidParams, ArgErr);
	}

	// agent_class / tolerance: accepted but not yet routed. Tolerated as silent no-ops.
	FString AgentClassUnused;
	Request.Args->TryGetStringField(TEXT("agent_class"), AgentClassUnused);
	double ToleranceUnused = 50.0;
	Request.Args->TryGetNumberField(TEXT("tolerance"), ToleranceUnused);

	UWorld* World = NAV_ResolveWorld();
	if (!World)
	{
		return FMCPToolHelpers::MakeError(Request, kNAVErrorInternal,
			TEXT("no world available (GEditor missing OR no level loaded)"));
	}

	UNavigationSystemV1* NavSys = UNavigationSystemV1::GetCurrent(World);
	if (!NavSys)
	{
		return FMCPToolHelpers::MakeError(Request, kNAVErrorInternal,
			TEXT("no UNavigationSystemV1 on the resolved world (navigation subsystem disabled?)"));
	}

	ANavigationData* DefaultNavData = NavSys->GetDefaultNavDataInstance(FNavigationSystem::DontCreate);
	if (!DefaultNavData)
	{
		// "No navdata available" is NOT an internal error — it's a legitimate state (empty map, no
		// NavMeshBoundsVolume placed). Surface as found=false so callers can branch on that, not as
		// -32603 which would imply broken infrastructure.
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("found"), false);
		Out->SetNumberField(TEXT("path_length"), 0.0);
		Out->SetArrayField(TEXT("waypoints"), TArray<TSharedPtr<FJsonValue>>());
		Out->SetStringField(TEXT("world"), NAV_WorldKindName(World));
		return FMCPToolHelpers::MakeSuccessObj(Request, Out);
	}

	FPathFindingQuery Query;
	Query.StartLocation = Start;
	Query.EndLocation = End;
	Query.NavData = DefaultNavData;
	Query.Owner = nullptr;

	const FPathFindingResult Result = NavSys->FindPathSync(Query);

	TArray<TSharedPtr<FJsonValue>> WaypointsArr;
	float PathLength = 0.0f;
	bool bFound = false;

	if (Result.IsSuccessful() && Result.Path.IsValid())
	{
		bFound = true;
		PathLength = Result.Path->GetLength();
		const TArray<FNavPathPoint>& Points = Result.Path->GetPathPoints();
		WaypointsArr.Reserve(Points.Num());
		for (const FNavPathPoint& Point : Points)
		{
			WaypointsArr.Add(MakeShared<FJsonValueArray>(NAV_VectorToArray(Point.Location)));
		}
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("found"), bFound);
	Out->SetNumberField(TEXT("path_length"), static_cast<double>(PathLength));
	Out->SetArrayField(TEXT("waypoints"), WaypointsArr);
	Out->SetStringField(TEXT("world"), NAV_WorldKindName(World));
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── navmesh.project_to_navmesh ────────────────────────────────────────────────────────────────
//
// Args:
//   location       : [x, y, z]                                       required
//   search_extent? : [x, y, z]   default [100, 100, 100]            box-half-extents searched
//
// Result:
//   {
//     projected   : bool,                  true iff a navmesh point was found within search_extent
//     location    : [x, y, z],             projected location (or original location when !projected)
//     world       : "editor" | "pie"
//   }
//
// Errors:
//   -32602 InvalidParams       missing/malformed location
//   -32603 Internal            no world OR no NavigationSystem
//
// Notes:
//   - ``UNavigationSystemV1::ProjectPointToNavigation`` searches inside a box of ±search_extent
//     around ``location`` for the nearest reachable navmesh vertex. Box semantics matter — increase
//     search_extent.Z if your navmesh is far above/below the query point.
//   - When no navmesh actor exists, ProjectPointToNavigation returns false; we report ``projected=false``
//     with the ORIGINAL location echoed back (not an error). Caller branches on the bool.
//   - Uses the DEFAULT nav data via ``GetDefaultNavDataInstance(FNavigationSystem::DontCreate)``.
//     Multi-agent worlds with per-agent navmeshes will need a future ``agent_class`` arg here too.
FMCPResponse Tool_ProjectToNavMesh(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kNAVErrorInvalidParams, TEXT("missing args object"));
	}

	FVector Location;
	FString ArgErr;
	if (!NAV_ParseVector3(Request.Args, TEXT("location"), Location, ArgErr))
	{
		return FMCPToolHelpers::MakeError(Request, kNAVErrorInvalidParams, ArgErr);
	}

	FVector SearchExtent;
	if (!NAV_ParseOptionalVector3(Request.Args, TEXT("search_extent"),
			FVector(100.0, 100.0, 100.0), SearchExtent, ArgErr))
	{
		return FMCPToolHelpers::MakeError(Request, kNAVErrorInvalidParams, ArgErr);
	}

	UWorld* World = NAV_ResolveWorld();
	if (!World)
	{
		return FMCPToolHelpers::MakeError(Request, kNAVErrorInternal,
			TEXT("no world available (GEditor missing OR no level loaded)"));
	}

	UNavigationSystemV1* NavSys = UNavigationSystemV1::GetCurrent(World);
	if (!NavSys)
	{
		return FMCPToolHelpers::MakeError(Request, kNAVErrorInternal,
			TEXT("no UNavigationSystemV1 on the resolved world (navigation subsystem disabled?)"));
	}

	FNavLocation OutLoc;
	const bool bProjected = NavSys->ProjectPointToNavigation(
		Location,
		OutLoc,
		SearchExtent,
		/*NavData*/ nullptr,
		/*FilterClass*/ nullptr);

	const FVector ReportedLoc = bProjected ? OutLoc.Location : Location;

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("projected"), bProjected);
	Out->SetArrayField(TEXT("location"), NAV_VectorToArray(ReportedLoc));
	Out->SetStringField(TEXT("world"), NAV_WorldKindName(World));
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

	RegisterTool(TEXT("navmesh.list"),                &Tool_List,             /*Lane A*/ false);
	RegisterTool(TEXT("navmesh.rebuild"),             &Tool_Rebuild,          /*Lane A*/ false);
	RegisterTool(TEXT("navmesh.find_path"),           &Tool_FindPath,         /*Lane A*/ false);
	RegisterTool(TEXT("navmesh.project_to_navmesh"),  &Tool_ProjectToNavMesh, /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("Wave G Surface 3 (NavMesh): registered 4 navmesh.* handlers (list / rebuild / find_path / project_to_navmesh, all Lane A)"));
}

} // namespace FNavMeshTools

#undef LOCTEXT_NAMESPACE

MCP_REGISTER_SURFACE(NavMeshTools, &FNavMeshTools::Register)
