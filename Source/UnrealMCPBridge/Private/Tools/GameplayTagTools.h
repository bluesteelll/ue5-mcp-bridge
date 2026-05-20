// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Wave D Surface 1 — GameplayTag surface. 4 user-visible tools, all Lane A.
 *
 * Tool roster:
 *   gameplaytag.list                  → enumerate registered tags (parent-prefix filter, paginated)
 *   gameplaytag.query_actor           → read tags via IGameplayTagAssetInterface OR FGameplayTagContainer property
 *   gameplaytag.add_to_container      → add tag to a named FGameplayTagContainer property on an actor
 *   gameplaytag.remove_from_container → remove tag from same
 *
 * **Coverage model.** The IGameplayTagAssetInterface is read-only by design. For mutation we use
 * property reflection to find an FGameplayTagContainer-typed UPROPERTY by name on the target
 * actor (or any of its components) and mutate it via FStructProperty + Container value-ptr.
 *
 * **All Lane A** — UGameplayTagsManager::Get singleton + UObject property access.
 * Writes PIE-guarded; reads bypass.
 *
 * Errors: standard kMCPError* — no new codes (uses -32004/-32011/-32026/-32602).
 */
namespace FGameplayTagTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	UNREALMCPBRIDGE_API FMCPResponse Tool_List(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_QueryActor(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_AddToContainer(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_RemoveFromContainer(const FMCPRequest& Request);
}
