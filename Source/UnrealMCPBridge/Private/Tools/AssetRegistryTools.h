// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Phase 2 — Category A (Asset Registry queries). 13 tools, ALL Lane A post-hotfix (2026-05).
 * Each handler is a static function taking ``const FMCPRequest&`` and returning ``FMCPResponse``.
 *
 * **HOTFIX (Plan R11 systemic-unsafe contingency):** the original design registered 10 of 13
 * tools Lane B (``bThreadSafe=true``) since the AR read API is documented thread-safe since UE
 * 5.0. Autonomous testing in UE 5.7 revealed ``IAR.GetAssets()`` asserts when enumerating
 * in-memory assets off the game thread — Epic comment "there are too many GetAssetRegistryTags()
 * still not thread-safe" (``AssetRegistry.cpp:2906``). All AR tools were demoted to Lane A.
 * The handler bodies were already authored to the Lane B contract (no UObject access, no GWorld,
 * AR-only reads) so re-promoting in Phase 3+ requires only changing the per-tool flag — possibly
 * after switching ``IAR.GetAssets()`` calls to pass ``Filter.bIncludeOnlyOnDiskAssets=true`` to
 * skip the unsafe in-memory enumeration path. See ``FMCPDispatchQueue.h`` Lane B contract block.
 *
 * Thumbnail tools (``asset.get_thumbnail`` / ``asset.get_thumbnail_to_disk``) and
 * ``asset.is_dirty`` were Lane A from day one (RT enqueue + game-thread FObjectThumbnail cache
 * for the thumbnail pair; loaded-package map walk for is_dirty).
 */
namespace FAssetRegistryTools
{
	/**
	 * Wire all 12 Category-A tools into the dispatch queue.
	 *
	 * @param OutRegisteredMethodNames  Append-only; bridge cleanup loops over this and calls
	 *                                  ``UnregisterHandler`` per name on module shutdown.
	 */
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	// ─── Category A: Asset Registry reads ─────────────────────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_AssetExists(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_AssetMetadata(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_AssetList(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_AssetFindReferences(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_AssetFindDependents(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_AssetSearchByClass(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_AssetSearchByTag(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_AssetSearchByName(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_AssetGetThumbnail(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_AssetGetThumbnailToDisk(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_AssetGetClassHierarchy(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_AssetIsDirty(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_AssetGetOutermostPackage(const FMCPRequest& Request);
}
