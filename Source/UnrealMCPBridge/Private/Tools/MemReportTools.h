// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FMCPDispatchQueue;

/**
 * Memory diagnostic surface — drive `MemReport` console captures and read live
 * memory counters. AI agents can scrape RAM usage at any moment OR generate a
 * full forensic dump for offline analysis (asset breakdown, biggest allocators,
 * etc.).
 *
 * Tools (2):
 *   memreport.dump(output_path?, full?, timeout_seconds?)
 *                     Trigger MemReport, sync-poll the output dir up to
 *                     timeout_seconds for the new file. Optional `full` mode
 *                     uses MemReport -FULL (slower, much more detail).
 *
 *   memreport.get_quick_stats()
 *                     Instant snapshot of FPlatformMemoryStats + GUObjectArray
 *                     counters. No file IO; safe for high-frequency polling.
 *
 * Note on blocking: `memreport.dump` blocks the game thread for up to
 * `timeout_seconds` (default 30) waiting for the file to appear on disk.
 * MemReport itself freezes the editor during generation regardless of how we
 * wrap it — sync-wait is the cleanest abstraction.
 *
 * Error codes:
 *   -32013 PathEscape         output_path resolved outside the sandbox
 *   -32058 OperationFailed    MemReport file did not appear within timeout
 *                              (engine busy or IO subsystem hung)
 *   -32602 InvalidParams      timeout_seconds out of [1, 120]
 *   -32603 InternalError      No GEngine pointer (commandlet startup)
 */
namespace FMemReportTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);
}
