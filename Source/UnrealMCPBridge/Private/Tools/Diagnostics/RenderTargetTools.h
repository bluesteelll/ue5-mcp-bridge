// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FMCPDispatchQueue;

/**
 * Render Target inspection surface — read pixels from UTextureRenderTarget2D assets.
 * Closes the visual-feedback loop for AI: workflows that use SceneCapture2D / PostProcess /
 * canvas-render-to-target can now have their output inspected programmatically.
 *
 * Tools (2):
 *   render_target.dump(render_target_path, output_path?, format?)
 *     Snapshot the render target's current pixel buffer. Saves to disk OR returns inline base64.
 *   render_target.get_info(render_target_path)
 *     Read-only metadata — size_x, size_y, format, num_mips, is_currently_bound_to_camera.
 *
 * Errors:
 *   -32004 ObjectNotFound       render_target_path doesn't load
 *   -32011 WrongClass           Loaded asset isn't UTextureRenderTarget2D
 *   -32013 PathEscape           output_path outside the sandbox whitelist
 *   -32017 InputTooLarge        Render target exceeds 4096x4096 = 64MB cap
 *   -32018 ThumbnailRenderFailed Render target had no GPU-side resource (never rendered to)
 *                                or pixel readback / PNG encode failed
 *   -32602 InvalidParams        Bad format (must be "png" or "exr")
 */
namespace FRenderTargetTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);
}
