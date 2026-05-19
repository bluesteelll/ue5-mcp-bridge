// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Stats / profiling surface. Editor-context queries (don't require PIE).
 *
 *   stats.get_engine    — frame time, FPS, RHI, draw stats (where exposed)
 *   stats.get_memory    — heap / physical / virtual / video memory breakdown
 *
 * Lane A — engine globals (GAverageFPS, FPlatformMemory) are not formally thread-safe.
 */
namespace FStatsTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	UNREALMCPBRIDGE_API FMCPResponse Tool_GetEngine(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_GetMemory(const FMCPRequest& Request);
}
