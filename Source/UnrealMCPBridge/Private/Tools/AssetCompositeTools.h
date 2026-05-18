// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Phase 2 — Category D internal C++ Lane handlers backing the Python composite tools.
 *
 * These 3 methods are registered with the dispatch queue but FILTERED out of ``tools.list``
 * (they have the leading-underscore convention `asset._..._internal`). Python composites
 * (`asset_composites.py`) call them by exact name as one-round-trip helpers, avoiding the
 * per-asset Python ferry catastrophe that would otherwise occur for find_unused / size_report.
 *
 * Routing (post-hotfix 2026-05, Plan R11):
 *   - `_find_unused_internal`  → Lane A (calls IAR.GetAssets which asserts on listener thread
 *     in UE 5.7 — see AssetRegistryTools.cpp registration block for full context)
 *   - `_size_report_internal`  → Lane A (same reason — IAR.GetAssets enumeration)
 *   - `_batch_metadata_internal` → **Lane B (kept)**. Synchronous body only does string
 *     normalisation + FMCPJobRegistry::SubmitJob — no AR enumeration. The submitted lambda
 *     uses IAR.GetAssetByObjectPath (single point query, not enumerating). MUST stay Lane B
 *     because the Python composite calls this via dispatch_internal (TCP loopback) from inside
 *     FMCPPythonEval::CallPythonTool on the game thread — routing it back through Lane A would
 *     deadlock (game thread waits 60s on socket.recv).
 */
namespace FAssetCompositeTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	// Day 11: internal Lane-B handlers (NOT in tools.list — leading-underscore convention).
	UNREALMCPBRIDGE_API FMCPResponse Tool_FindUnusedInternal(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_SizeReportInternal(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_BatchMetadataInternal(const FMCPRequest& Request);
}
