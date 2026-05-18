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
 * Routing:
 *   - `_find_unused_internal`  → Lane B (pure AR walk + GetReferencers loop, no UObject)
 *   - `_size_report_internal`  → Lane B (AR walk + IFileManager::FileSize, no UObject)
 *   - `_batch_metadata_internal` → Lane A (Day 5/D5 — submits an async job whose body runs
 *     on a worker thread with bGameThreadRequired=false; the JOB body is Lane B-safe but the
 *     submit step itself enters the dispatch from listener thread anyway, registered Lane A
 *     for simplicity / parity with the other async submitters in ContentBrowserTools.cpp).
 */
namespace FAssetCompositeTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	// Day 11: internal Lane-B handlers (NOT in tools.list — leading-underscore convention).
	UNREALMCPBRIDGE_API FMCPResponse Tool_FindUnusedInternal(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_SizeReportInternal(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_BatchMetadataInternal(const FMCPRequest& Request);
}
