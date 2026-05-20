// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Wave D Surface 5 — Level streaming (sub-level) management surface. 4 tools, all Lane A.
 *
 * Tool roster:
 *   level_streaming.list         — enumerate ULevelStreaming entries of a persistent world
 *   level_streaming.add          — add a sublevel to a persistent world (UEditorLevelUtils::AddLevelToWorld)
 *   level_streaming.remove       — remove a sublevel from a persistent world (RemoveLevelFromWorld)
 *   level_streaming.set_loaded   — toggle bShouldBeLoaded + bShouldBeVisible on a ULevelStreaming
 *
 * **PIE behaviour.** ``list`` + ``set_loaded`` are PIE-safe (no guard). ``add`` + ``remove`` are
 * PIE-guarded (-32027 PIEActive). ``set_loaded`` resolves the streaming instance via PIE-first /
 * editor-fallback world resolution, mirroring the trace-world convention; ``FlushLevelStreaming``
 * is only invoked when NOT in PIE (in PIE the world's normal streaming-update tick drains the
 * flag change without us forcing a synchronous flush).
 *
 * **Distinction from level.set_streaming_state.** That older tool (Phase 3 ``level.*`` surface)
 * is editor-world only and PIE-guarded. ``level_streaming.set_loaded`` is the more permissive
 * PIE-aware variant added for Wave D — same underlying ``SetShouldBeLoaded`` /
 * ``SetShouldBeVisible`` calls, different gate.
 *
 * **Errors.** Reuses existing codes — no new codes introduced:
 *   -32004 ObjectNotFound  (persistent level or sublevel not loadable)
 *   -32011 WrongClass      (path resolves to non-UWorld)
 *   -32014 PathInUse       (sublevel already in streaming list — level_streaming.add)
 *   -32027 PIEActive       (add / remove during PIE)
 *   -32602 InvalidParams   (missing/malformed args)
 */
namespace FLevelStreamingTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	UNREALMCPBRIDGE_API FMCPResponse Tool_List(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_Add(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_Remove(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_SetLoaded(const FMCPRequest& Request);
}
