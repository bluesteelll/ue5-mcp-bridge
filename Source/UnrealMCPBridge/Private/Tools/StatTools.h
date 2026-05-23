// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FMCPDispatchQueue;

/**
 * Runtime stat-group surface — interact with UE's `stat <X>` console command
 * family. Lets AI agents toggle stat overlays, scrape current frame metrics,
 * and capture a multi-frame stats file for offline analysis.
 *
 * Tools (4):
 *   stat.list_groups()                  Hardcoded canonical group list (engine
 *                                        doesn't publicly enumerate)
 *   stat.toggle(stat_name)              Toggle a stat overlay on/off
 *   stat.get_unit_values()              Snapshot of FPS / frame-time / draw call /
 *                                        triangle counters
 *   stat.dump_to_file(output_path?, frames?)
 *                                       Capture stats for N frames; returns
 *                                        immediately, file finalizes async
 *
 * Note: this surface is `stat.*` (singular). A pre-existing `stats.*` (plural)
 * surface in StatsTools.cpp provides aggregate engine/memory snapshots. Both
 * coexist intentionally.
 *
 * Error codes:
 *   -32013 PathEscape         output_path resolved outside the sandbox
 *   -32058 OperationFailed    Engine refused the command (rare — `GEngine->Exec`
 *                              of a stat-family command failed)
 *   -32602 InvalidParams      Missing stat_name; stat_name fails the alphanumeric
 *                              regex (anti-injection); frames out of [1, 10000]
 *   -32603 InternalError      No GEngine pointer (commandlet startup)
 */
namespace FStatTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);
}
