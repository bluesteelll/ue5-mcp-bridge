// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Wave H Surface 5 — Thumbnail manipulation surface. 3 user-visible tools that round out the
 * existing ``asset.get_thumbnail`` / ``asset.get_thumbnail_to_disk`` Phase 2 read tools with
 * batch generation, cache invalidation, and custom-image injection.
 *
 * Tool roster:
 *   thumbnail.batch_generate  — Render thumbnails for many assets in one round-trip. Per-asset
 *                                ``ThumbnailTools::RenderThumbnail`` (AlwaysFlush) at the
 *                                requested size, encoded to PNG (default) or JPG, written under
 *                                a sandbox-resolved ``output_directory``. Filenames derived from
 *                                the asset's leaf name (sanitised, ``.<ext>`` appended). Lane A
 *                                (game thread — render thread enqueue). NO PIE guard (read-only
 *                                output to disk; in-memory thumbnail cache is touched but never
 *                                written back to the asset's package).
 *   thumbnail.clear_cache     — Invalidate cached thumbnails via ``ThumbnailTools::CacheEmptyThumbnail``
 *                                so the next ``UThumbnailManager`` request will re-render. With
 *                                ``asset_paths`` omitted, walks every loaded package's
 *                                ``ThumbnailMap`` and empties every entry. With ``force_regenerate``,
 *                                re-runs ``RenderThumbnail(AlwaysFlush)`` on each cleared asset
 *                                so the cache repopulates synchronously. Lane A. NO PIE guard.
 *   thumbnail.set_custom      — Load PNG/JPG from disk via ``IImageWrapperModule``, normalise to
 *                                BGRA8/sRGB, stamp into the asset's package as a custom
 *                                ``FObjectThumbnail`` (``SetCreatedAfterCustomThumbsEnabled``).
 *                                Calls ``ThumbnailTools::CacheThumbnail`` (which writes into
 *                                the package's ``ThumbnailMap``), then ``MarkPackageDirty`` so
 *                                the next ``SaveLoadedAsset`` persists it. Lane A. PIE-guarded
 *                                (mutates the asset's outer package). FScopedTransaction.
 *
 * **Why Lane A for all three.**
 *   - ``RenderThumbnail`` enqueues a draw command on the render thread and (with AlwaysFlush)
 *     synchronises back. The producer side touches UObject CDO + UThumbnailRenderer state
 *     (FStaticMesh sample preview scene, FParticleSystem, ...) which is not thread-safe.
 *   - ``CacheThumbnail`` / ``CacheEmptyThumbnail`` walk the package's transient thumbnail map
 *     (``UPackage::ThumbnailMap``) — same package access pattern as ``MarkPackageDirty``,
 *     game-thread-only.
 *   - ``MarkPackageDirty`` is unsafe off the game thread (broadcasts to editor delegates).
 *
 * **PIE guard rationale (set_custom only).**
 *   ``batch_generate`` and ``clear_cache`` are read-or-cache-touch — they do not mutate the
 *   asset's package on disk OR mark it dirty. They are safe during PIE the same way
 *   ``asset.get_thumbnail`` is. ``set_custom`` mutates package state + dirties it; per the
 *   Phase 2 mutator contract this requires ``GEditor->PlayWorld == nullptr`` (return -32027).
 *
 * **Error codes (all reused — no new codes introduced):**
 *   -32004 ObjectNotFound        Asset path doesn't load (batch_generate / clear_cache / set_custom)
 *                                OR ``image_path`` doesn't exist (set_custom)
 *   -32010 InvalidPath           Malformed asset_path / image_path
 *   -32011 WrongClass            (Not currently raised — any UObject is thumbnailable)
 *   -32013 PathEscape            ``output_directory`` outside sandbox (batch_generate) OR
 *                                ``image_path`` outside sandbox (set_custom)
 *   -32018 ThumbnailRenderFailed RenderThumbnail returned empty (batch_generate per-asset failure
 *                                — collected into ``failures[]``, not the top-level error)
 *                                OR image-format decode failure (set_custom)
 *   -32027 PIEActive             ``set_custom`` only — refused when ``GEditor->PlayWorld != nullptr``
 *   -32602 InvalidParams         Missing required field, ``size`` out of [16, 2048], unknown
 *                                ``format`` value, empty ``asset_paths`` array
 *   -32603 InternalError         ``IImageWrapperModule`` returned null wrapper, package-write
 *                                failure
 */
namespace FThumbnailTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	// ─── Wave H Surface 5: Thumbnail manipulation tools ─────────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_ThumbnailBatchGenerate(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_ThumbnailClearCache(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_ThumbnailSetCustom(const FMCPRequest& Request);
}
