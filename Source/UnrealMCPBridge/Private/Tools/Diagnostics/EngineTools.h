// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Wave G Surface 6 — Engine introspection + GC control. 3 user-visible tools, all Lane A.
 *
 * Tool roster:
 *   engine.get_info             → ``FEngineVersion::Current()`` + ``FApp::*`` + ``FPaths::*`` snapshot
 *                                  (UE version, build config, target type, project/engine dirs,
 *                                   platform, editor/unattended/commandlet flags, current world
 *                                   summary). Read-only, no PIE guard, no transactions.
 *   engine.gc_collect           → wrap ``CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, bForce)`` or
 *                                  ``GEngine->ForceGarbageCollection(bPurge)``. Reports allocator
 *                                  delta + wall-clock duration. No PIE guard — GC works the same
 *                                  in editor and PIE.
 *   engine.get_memory_snapshot  → ``FPlatformMemory::GetStats`` + ``GMalloc->GetDescriptiveName``
 *                                  snapshot. Optional ``include_breakdown=true`` adds platform-
 *                                  specific stats from ``FPlatformMemoryStats::GetPlatformSpecificStats``.
 *                                  Read-only, no PIE guard.
 *
 * **Lane A.** Every entry-point requires GT — ``GEngine`` / ``GEditor`` / ``CollectGarbage`` /
 * ``GMalloc`` / ``GEditor->PlayWorld`` access is not formally thread-safe (matches Phase 1 stats.*
 * convention).
 *
 * **No PIE guard.** All three tools are introspection or engine-level mutators that operate
 * uniformly in editor + PIE. ``engine.gc_collect`` deliberately allows GC during PIE (game-thread
 * pause is normal during a manual GC sweep).
 *
 * **No new error codes.** Reuses:
 *   -32603 Internal   ``GEngine``/``GMalloc`` null (commandlet or extremely-early init)
 *
 * **No new Build.cs deps.** All APIs live in Core (FApp / FEngineVersion / FPaths /
 * FPlatformMemory / FPlatformTime / IsRunningCommandlet / GMalloc / FPlatformProperties),
 * CoreUObject (CollectGarbage / GARBAGE_COLLECTION_KEEPFLAGS), Engine (GEngine /
 * ForceGarbageCollection), and UnrealEd (GEditor / PlayWorld / GetEditorWorldContext) — all
 * already linked by the bridge.
 */
namespace FEngineTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	UNREALMCPBRIDGE_API FMCPResponse Tool_GetInfo(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_GCCollect(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_GetMemorySnapshot(const FMCPRequest& Request);
}
