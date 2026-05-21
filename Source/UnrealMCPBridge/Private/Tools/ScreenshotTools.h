// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Wave H Surface 4 — Extended screenshot surface. 3 user-visible tools that go beyond the
 * Phase 5 single-shot ``editor.viewport_screenshot`` family by adding multiplier-driven
 * high-resolution capture, AABB-framed actor capture, and pure-image diffing.
 *
 * Tool roster:
 *   screenshot.high_resolution  — Multiplier-driven capture. Native viewport size × multiplier
 *                                 piped through the existing ``FMCPScreenshotUtils::CaptureViewport``
 *                                 (which does Draw + ReadPixels + FImageUtils::ImageResize) and
 *                                 ``EncodeAndSaveToDisk``. NOT the engine's ``HighResShot`` console
 *                                 command — that path is async (``FImageWriteTask``) and incompatible
 *                                 with our synchronous request/response model. Multiplier-resize
 *                                 IS NOT tile-rendering; the resize hides aliasing at >2x but the
 *                                 underlying scene render is still native. Same caveat as the Phase 5
 *                                 ``viewport_screenshot_to_disk`` family — see
 *                                 ``MCPScreenshotUtils.h`` "No FHighResScreenshotConfig path." block.
 *                                 Lane A (viewport access).
 *   screenshot.region_capture   — Save camera state → ``FocusViewportOnBox`` over actor's
 *                                 ``GetComponentsBoundingBox()`` (expanded by padding/extent_ratio)
 *                                 → synchronous Draw → capture at supplied resolution → restore camera.
 *                                 Re-uses the camera-restore pattern from ``editor.set_camera``
 *                                 (``SetViewLocation`` + ``SetViewRotation`` + ``Invalidate``).
 *                                 Lane A (viewport access).
 *   screenshot.diff             — Pure image compute. Loads both files (any format auto-detected by
 *                                 ``IImageWrapperModule::DecompressImage``), normalises both to
 *                                 BGRA8/sRGB via ``FImage::ChangeFormat``, walks pixels with a fixed
 *                                 5-byte noise tolerance per channel. Optional ``diff_output_path``
 *                                 writes a PNG marking different pixels in semi-transparent red over
 *                                 a desaturated greyscale base.
 *                                 Lane B (no UObject access, no GT requirement).
 *
 * **No PIE guard.** Screenshots are runtime operations; they work in PIE and outside it. The
 * underlying viewport access goes through ``FMCPScreenshotUtils`` which doesn't distinguish.
 *
 * **No FScopedTransaction.** Screenshots aren't undoable; they emit a side-effect file but don't
 * mutate asset/world state. Camera restore in ``region_capture`` is best-effort (a stray Slate
 * tick between save and restore could write a different value first — vanishingly rare in practice).
 *
 * **Error codes (all reused from existing range — no new codes):**
 *   -32004 ObjectNotFound        Actor path (region_capture) or input file (diff) doesn't resolve.
 *   -32010 InvalidPath           Output path empty or sandbox-rejected.
 *   -32013 PathEscape            Output path resolves outside the sandbox whitelist.
 *   -32602 InvalidParams         Bad multiplier / resolution / threshold; missing required fields.
 *   -32603 InternalError         Viewport unavailable / capture failed / decode failed.
 */
namespace FScreenshotTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	// ─── Wave H Surface 4: Extended screenshot tools ────────────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_HighResolution(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_RegionCapture(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_Diff(const FMCPRequest& Request);
}
