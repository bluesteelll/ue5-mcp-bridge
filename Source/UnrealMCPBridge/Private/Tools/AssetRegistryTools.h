// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Phase 2 — Category A (Asset Registry queries). 12 tools, mostly Lane B (subject to Day 0b audit
 * outcome — see ``Tests/lane_b_audit_results.md`` after audit runs). Each handler is a static
 * function taking ``const FMCPRequest&`` and returning ``FMCPResponse``.
 *
 * Lane B handlers MUST follow the contract documented in ``FMCPDispatchQueue.h`` — no UObject
 * access, no GWorld, no mutating editor APIs. Per the plan baseline 11 of the 12 tools register
 * with ``bThreadSafe=true``; the exception is ``asset.is_dirty`` which touches the loaded-package
 * map and is Lane A always.
 *
 * Thumbnail tools (``asset.get_thumbnail`` / ``asset.get_thumbnail_to_disk``) are Lane A because
 * thumbnail rendering enqueues to the render thread and needs game-thread context for the
 * FObjectThumbnail cache.
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
