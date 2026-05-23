// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Wave C Tier 5c — WorldPartition surface. 3 user-visible tools, all Lane A.
 *
 * Tool roster (scope reduced from the original spec to "verifiable editor-side operations"):
 *   wp.is_partitioned          → check whether a level is WorldPartition-enabled
 *   wp.get_actor_runtime_grid  → read AActor::RuntimeGrid (FName)
 *   wp.set_actor_runtime_grid  → write AActor::RuntimeGrid; affects streaming cell membership
 *
 * **Scope.** The original spec listed wp.list_cells / wp.load_cell / wp.set_runtime_grid as
 * level-level operations. Those require active streaming + runtime hash classes whose
 * end-to-end behaviour depends on PIE context, complete cooked-data presence, and grid setup
 * — out of scope for the bridge's "scripted editor authoring" surface. Per-actor runtime-grid
 * editing is the most commonly-needed write op (it's what designers set in the actor outliner)
 * and works in pure editor world without any runtime streaming machinery.
 *
 * **All Lane A.** UWorld::GetWorldPartition + LoadObject<UWorld> + AActor property writes —
 * all GT-only. Writes PIE-guarded; is_partitioned + get_actor_runtime_grid are read-only.
 *
 * Errors: standard kMCPError* (no new codes).
 */
namespace FWorldPartitionTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	UNREALMCPBRIDGE_API FMCPResponse Tool_IsPartitioned(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_GetActorRuntimeGrid(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_SetActorRuntimeGrid(const FMCPRequest& Request);
}
