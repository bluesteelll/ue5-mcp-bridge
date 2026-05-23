// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FMCPDispatchQueue;

/**
 * Unreal Insights trace-capture surface — start/stop/inspect .utrace recordings
 * programmatically. AI agents can capture a profiling window around a specific
 * code path (e.g. spawn 1000 actors, capture, analyse) without manually clicking
 * the Insights toolbar.
 *
 * Tools (4):
 *   insights.start_capture(channels?, output_path?)   Begin recording to .utrace
 *   insights.stop_capture()                            End recording; reports file size + elapsed time
 *   insights.get_status()                              Tracing yes/no + active channels + elapsed
 *   insights.list_channels()                           Enumerate every Trace channel and its enabled state
 *
 * Local-state contract: the only state the surface tracks itself is the start
 * timestamp (engine doesn't expose it). `FTraceAuxiliary` owns everything else
 * — IsConnected, Destination, ActiveChannels — and we read it on demand. Avoids
 * self-state desync if a trace is stopped externally (cmd-line, another tool).
 *
 * Error codes:
 *   -32013 PathEscape         output_path resolved outside the sandbox
 *   -32058 OperationFailed    FTraceAuxiliary::Start returned false (already tracing or
 *                              connection couldn't be established) or Stop returned false
 *                              (not currently tracing)
 *   -32602 InvalidParams      Malformed channels list (empty token, illegal char, etc.)
 *   -32603 InternalError      Filesystem failure (e.g. directory creation) — rare
 */
namespace FInsightsTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);
}
