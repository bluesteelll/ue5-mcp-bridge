// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Wave E Surface 1 — Editor viewport camera control. 4 user-visible tools, all Lane A.
 *
 * Tool roster:
 *   viewport.list             → enumerate all FLevelEditorViewportClient entries with per-viewport
 *                                index/type/active-flag/camera state
 *   viewport.get_camera       → read a single viewport's camera state by index (default 0)
 *   viewport.set_camera       → teleport a viewport's camera; at least one of location/rotation/fov
 *                                must be supplied; returns prior + new state for round-trip diffing
 *   viewport.focus_on_actor   → frame an actor in a viewport via FocusViewportOnBox(Bounds, true)
 *
 * **All Lane A.** FLevelEditorViewportClient + GEditor->GetLevelViewportClients are GT-only.
 * SetViewLocation/SetViewRotation/Invalidate write directly into the client structure.
 *
 * **No PIE guard.** Viewport ops are editor-only by definition; the editor viewport remains
 * accessible while PIE runs (the PIE game viewport is separate). No FScopedTransaction (viewport
 * state isn't part of the undo system) and no MarkPackageDirty (no asset state is touched).
 *
 * **Viewport indexing.** GEditor->GetLevelViewportClients() returns a flat array of
 * FLevelEditorViewportClient*; we use that array's index as the user-visible viewport_index.
 * Default index is 0 (first available viewport — typically the main perspective viewport).
 *
 * Errors: standard kMCPError* — no new codes:
 *   -32004 ObjectNotFound      no level viewports OR focus_on_actor target missing
 *   -32026 PropertyIndexOOB    viewport_index out of range
 *   -32602 InvalidParams       set_camera supplied with no fields OR malformed vector/rotator
 */
namespace FViewportTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	UNREALMCPBRIDGE_API FMCPResponse Tool_List(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_GetCamera(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_SetCamera(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_FocusOnActor(const FMCPRequest& Request);
}
