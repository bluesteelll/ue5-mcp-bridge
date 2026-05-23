// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Phase 5 — Chunk B (Editor utilities). 10 user-visible tools, all Lane A.
 *
 * Tool roster (per Phase 5 plan lines 352-678):
 *   editor.viewport_screenshot          → in-memory base64 PNG/JPG capture of focused level viewport
 *   editor.viewport_screenshot_to_disk  → file output, larger cap, sandbox-validated path
 *   pie.screenshot_to_disk              → PIE game-viewport capture (requires active PIE)
 *   editor.get_camera                   → focused viewport camera state {location, rotation, fov, ortho_width}
 *   editor.set_camera                   → set focused viewport camera (FOV optional)
 *   editor.get_selection                → world outliner selection (actors[] + components[])
 *   editor.set_selection                → select actors by id; clears or appends (cap 200)
 *   editor.show_message                 → corner toast notification via SlateNotificationManager
 *   editor.current_world                → editor world identity {world_path, world_name, pie_active}
 *   editor.tick_once                    → forces GEditor->Tick(dt) (skips Slate UI tick)
 *
 * **All 10 tools are Lane A** (``bThreadSafe=false``). Reasons:
 *   - Viewport capture (ReadPixels) flushes render commands; GT-only.
 *   - FLevelEditorViewportClient / GEditor / GCurrentLevelEditingViewportClient are GT-only.
 *   - FSlateNotificationManager::AddNotification touches Slate widget tree; GT-only.
 *   - Selection mutators (EditorActorSubsystem::SetSelectedLevelActors) raise editor delegates; GT-only.
 *
 * **pie.screenshot_to_disk gating.** Requires PIE active via ``FMCPWorldContext::IsPIEActive``;
 * refuses with ``kMCPErrorPIENotActive`` (-32038) + frozen ``kMCPMessagePIENotActive`` otherwise.
 * This mirrors the Chunk A pattern — Chunk B adds this single PIE-required tool to keep the
 * pie.* surface coherent (lives next to pie.start/pie.stop/etc. in the wire vocabulary).
 *
 * **Path sandbox.** ``editor.viewport_screenshot_to_disk`` and ``pie.screenshot_to_disk`` resolve
 * the ``path`` argument via ``FMCPPathSandbox::Resolve`` — out-of-sandbox paths return
 * ``kMCPErrorPathEscape`` (-32013). Default path is ``<Saved>/UnrealMCP/screenshots/<uuid>.<ext>``
 * (mirrors asset.get_thumbnail_to_disk).
 *
 * **Active viewport resolution.** ``GCurrentLevelEditingViewportClient`` is the hovered viewport;
 * falls back to ``GLastKeyLevelEditingViewportClient`` (last-interacted) when null; final fallback
 * is the LevelEditor module's ``GetFirstActiveLevelViewport()->GetActiveViewport()``. All three
 * paths can return null in pathological circumstances (no level editor tab open); we surface
 * a -32603 Internal error with a "no level viewport found" message.
 *
 * **Selection size cap.** ``editor.set_selection`` caps ``actor_ids`` at 200 to match Phase 2
 * ``asset.batch_metadata`` (``kMCPCapBatchMetadata``). Exceeding returns ``kMCPErrorInputTooLarge``
 * (-32017). 200 is a generous editor-UX limit — selecting more breaks Details panel performance.
 *
 * **Show-message level mapping.** Wire ``"level"`` (info/success/warning/error) → SNotificationItem
 * CompletionState (CS_None/CS_Success/CS_Pending/CS_Fail). FNotificationInfo color/icon set by
 * the SCS_* enum. We additionally set ``bUseSuccessFailIcons=true`` so the toast renders the
 * appropriate corner-icon for the level.
 *
 * **Tick semantics.** ``editor.tick_once`` calls ``GEditor->Tick(DeltaTime, false)`` with
 * ``bIdleMode=false`` (do real work). Default ``DeltaTime`` is one 60Hz frame (1/60 s). Slate UI
 * is NOT ticked by this path — toasts created via editor.show_message will only appear after the
 * next natural editor tick. Document this in the tool description.
 *
 * **No async composites in Chunk B.** All 10 tools are sync — capture/display ops complete
 * within one tick (or fail synchronously). The composite + Lane-B-internal pattern used by
 * Phase 2 asset.* + Phase 3 level._* is not applicable here.
 */
namespace FEditorTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	// ─── Screenshot family ──────────────────────────────────────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_ViewportScreenshot(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_ViewportScreenshotToDisk(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_PIEScreenshotToDisk(const FMCPRequest& Request);

	// ─── Camera ─────────────────────────────────────────────────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_GetCamera(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_SetCamera(const FMCPRequest& Request);

	// ─── Selection ──────────────────────────────────────────────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_GetSelection(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_SetSelection(const FMCPRequest& Request);

	// ─── Misc utility ───────────────────────────────────────────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_ShowMessage(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_CurrentWorld(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_TickOnce(const FMCPRequest& Request);
}
