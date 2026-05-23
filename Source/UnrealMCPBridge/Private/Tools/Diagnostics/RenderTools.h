// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Wave G Surface 5 — Render / show-flag / post-process control. 4 user-visible tools, all Lane A.
 *
 * Tool roster:
 *   render.list_show_flags                 → enumerate ``FEngineShowFlags`` entries on a viewport
 *                                             (name + enabled bool). No PIE guard — viewport state.
 *   render.set_show_flag                   → toggle one flag by name; returns prior_enabled +
 *                                             new_enabled diff. ``FLevelEditorViewportClient::
 *                                             EngineShowFlags`` + ``Invalidate``. No PIE guard.
 *   render.set_engine_stat                 → ``GEngine->SetEngineStat`` toggle (``stat fps`` /
 *                                             ``stat unit`` / etc.). No PIE guard — overlays are
 *                                             viewport-attached.
 *   render.set_post_process_volume_property → write a single FPostProcessSettings field on a
 *                                             named APostProcessVolume actor, automatically
 *                                             setting the companion ``bOverride_<X>=true``.
 *                                             PIE-guarded, transacted, dirties owning package.
 *
 * **Lane A.** Every entry-point requires GT — GEditor / FEngineShowFlags / UPostProcessVolume
 * traversal + FProperty reflection are all GT-only.
 *
 * **PIE guard.** Only ``set_post_process_volume_property`` refuses PIE with -32027. The three
 * viewport/stat tools transparently operate on the editor viewport state while PIE runs.
 *
 * **Viewport indexing** mirrors the Wave E ``viewport.*`` family — ``viewport_index`` (default 0)
 * indexes ``GEditor->GetLevelViewportClients()``. OOB → -32026 PropertyIndexOOB.
 *
 * **No new error codes.** Reuses:
 *   -32004 ObjectNotFound       flag name not found / stat name unrecognised / no viewports / actor
 *                               path doesn't resolve
 *   -32005 PropertyNotFound     property_path doesn't resolve to a UPROPERTY on FPostProcessSettings
 *   -32011 WrongClass           volume_actor_path resolves to a non-APostProcessVolume actor
 *   -32026 PropertyIndexOOB     viewport_index out of range
 *   -32027 PIEActive            set_post_process_volume_property attempted during PIE
 *   -32602 InvalidParams        missing args / malformed input
 *   -32603 Internal             GEditor missing (commandlet) / unexpected reflection failure
 */
namespace FRenderTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	UNREALMCPBRIDGE_API FMCPResponse Tool_ListShowFlags(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_SetShowFlag(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_SetEngineStat(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_SetPostProcessVolumeProperty(const FMCPRequest& Request);
}
