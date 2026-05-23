// Copyright FatumGame. All Rights Reserved.

#include "WorldPartitionTools.h"

#include "MCPSurfaceRegistry.h"

#include "FMCPDispatchQueue.h"
#include "MCPAssetLoader.h"
#include "MCPJsonBuilder.h"
#include "MCPMutatorScope.h"
#include "MCPToolHelpers.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPActorPathUtils.h"

#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "WorldPartition/WorldPartition.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// WP_ prefix per unity-build convention. Per-surface error constants kept;
	// XX_StampIds/MakeError/MakeSuccessObj/RequireStringField/LoadWorldByPath removed in Phase 3 —
	// use FMCPToolHelpers::Xxx + FMCPAssetLoader::Load<UWorld> from the shared helpers.
	// Tool_SetActorRuntimeGrid PIE-guard + FScopedTransaction migrated to FMCPMutatorScope.
	constexpr int32 kWPErrorInvalidParams = -32602;
	constexpr int32 kWPErrorInternal      = -32603;
} // namespace

namespace FWorldPartitionTools
{

// ─── wp.is_partitioned ────────────────────────────────────────────────────────────────────────
//
// Args:    { level_path: string }
// Result:  { partitioned: bool, partition_path?: string }
FMCPResponse Tool_IsPartitioned(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString LevelPath;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("level_path"), LevelPath, Err)) { return Err; }

	int32 LoadErrCode = 0;
	FString LoadErrMsg;
	UWorld* World = FMCPAssetLoader::Load<UWorld>(LevelPath, LoadErrCode, LoadErrMsg);
	if (!World) { return FMCPToolHelpers::MakeError(Request, LoadErrCode, LoadErrMsg); }

	UWorldPartition* WP = World->GetWorldPartition();

	return FMCPJsonBuilder()
		.Bool(TEXT("partitioned"), WP != nullptr)
		.If(WP != nullptr,
			[&](FMCPJsonBuilder& B) { B.Str(TEXT("partition_path"), WP->GetPathName()); })
		.Str(TEXT("world_path"), World->GetPathName())
		.BuildSuccess(Request);
}

// ─── wp.get_actor_runtime_grid ────────────────────────────────────────────────────────────────
//
// Args:    { actor_path: string }
// Result:  { actor_path, runtime_grid }
FMCPResponse Tool_GetActorRuntimeGrid(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString ActorPath;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("actor_path"), ActorPath, Err)) { return Err; }

	bool bAmbiguous = false;
	FString AmbiguityHint, ResolveErr;
	AActor* Actor = FMCPActorPathUtils::ResolveActor(ActorPath, /*bRejectPIE*/ false,
		bAmbiguous, AmbiguityHint, ResolveErr);
	if (!Actor)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("actor '%s' not found: %s"), *ActorPath, *ResolveErr));
	}

	return FMCPJsonBuilder()
		.Str(TEXT("actor_path"), Actor->GetPathName())
		.Str(TEXT("runtime_grid"), Actor->GetRuntimeGrid().ToString())
		.BuildSuccess(Request);
}

// ─── wp.set_actor_runtime_grid ────────────────────────────────────────────────────────────────
//
// Args:    { actor_path: string, runtime_grid: string (empty/None to clear) }
// Result:  { actor_path, prior_grid, new_grid }
FMCPResponse Tool_SetActorRuntimeGrid(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_SetActorRuntimeGrid", "Set Actor RuntimeGrid"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString ActorPath;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("actor_path"), ActorPath, Err)) { return Err; }

	FString NewGrid;
	if (!Request.Args->TryGetStringField(TEXT("runtime_grid"), NewGrid))
	{
		return FMCPToolHelpers::MakeError(Request, kWPErrorInvalidParams,
			TEXT("wp.set_actor_runtime_grid requires args.runtime_grid (string; empty to clear)"));
	}

	bool bAmbiguous = false;
	FString AmbiguityHint, ResolveErr;
	AActor* Actor = FMCPActorPathUtils::ResolveActor(ActorPath, /*bRejectPIE*/ true,
		bAmbiguous, AmbiguityHint, ResolveErr);
	if (!Actor)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("actor '%s' not found: %s"), *ActorPath, *ResolveErr));
	}

	const FName Prior = Actor->GetRuntimeGrid();
	const FName Desired = NewGrid.IsEmpty() ? NAME_None : FName(*NewGrid);

	// FMCPMutatorScope at function-top owns the FScopedTransaction lifetime.
	Actor->Modify();
	Actor->SetRuntimeGrid(Desired);

	if (UPackage* ExternalPkg = Actor->GetExternalPackage())
	{
		Scope.DirtyPackage(ExternalPkg);
	}
	else if (UPackage* OuterPkg = Actor->GetOutermost())
	{
		Scope.DirtyPackage(OuterPkg);
	}

	return FMCPJsonBuilder()
		.Str(TEXT("actor_path"), Actor->GetPathName())
		.Str(TEXT("prior_grid"), Prior.ToString())
		.Str(TEXT("new_grid"),   Desired.ToString())
		.BuildSuccess(Request);
}

void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	RegisterTool(TEXT("wp.is_partitioned"),         &Tool_IsPartitioned,        /*Lane A*/ false);
	RegisterTool(TEXT("wp.get_actor_runtime_grid"), &Tool_GetActorRuntimeGrid,  /*Lane A*/ false);
	RegisterTool(TEXT("wp.set_actor_runtime_grid"), &Tool_SetActorRuntimeGrid,  /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("WorldPartition surface registered: 3 wp.* tools (is_partitioned + get/set_actor_runtime_grid), all Lane A"));
}

} // namespace FWorldPartitionTools

#undef LOCTEXT_NAMESPACE

MCP_REGISTER_SURFACE(WorldPartitionTools, &FWorldPartitionTools::Register)
