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
 * Routing (post-hotfix 2 2026-05): ALL three internals use the async-job pattern. The sync
 * handler body is Lane B (validate args + FMCPJobRegistry::SubmitJob — no AR work, no UObject
 * access); the heavy enumeration runs inside the submitted job body lambda.
 *
 *   - `_find_unused_internal`    → Lane B, bGameThreadRequired=true (job body calls IAR.GetAssets
 *     which asserts on non-game-thread in UE 5.7 — the registry dispatches via AsyncTask(GT, …))
 *   - `_size_report_internal`    → Lane B, bGameThreadRequired=true (same reason)
 *   - `_batch_metadata_internal` → Lane B, bGameThreadRequired=false (uses only
 *     IAR.GetAssetByObjectPath, a thread-safe single-point query)
 *
 * Why all Lane B? The Python composites call these via dispatch_internal (TCP loopback) from
 * inside CallPythonTool on the game thread. Lane A would queue the request back to the same
 * game thread that's blocked on socket.recv() → 60s deadlock until socket timeout. The
 * composites then poll job.result (also promoted to Lane B by hotfix 2 — see UnrealMCPBridge.cpp
 * + FMCPDay7Handlers.cpp) until terminal state and return the inner result payload.
 */
namespace FAssetCompositeTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	// Day 11: internal Lane-B handlers (NOT in tools.list — leading-underscore convention).
	UNREALMCPBRIDGE_API FMCPResponse Tool_FindUnusedInternal(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_SizeReportInternal(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_BatchMetadataInternal(const FMCPRequest& Request);
}
