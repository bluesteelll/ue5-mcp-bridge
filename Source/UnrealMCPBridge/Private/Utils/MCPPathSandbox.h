// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Disk-path sandbox for ``cb.export`` dest_file, ``cb.import`` source_file, and
 * ``asset.get_thumbnail_to_disk`` output_path (D8).
 *
 * **Whitelist:** the resolved absolute path must sit under one of:
 *   - ``<ProjectDir>`` (anywhere in the project tree, including ``Content/``)
 *   - ``<ProjectSavedDir>`` (covers default thumbnail output)
 *   - ``<ProjectIntermediateDir>``
 *   - ``<EngineDir>`` (for ``/Engine/...`` content imports)
 *
 * Anything else returns ``PATH_ESCAPE`` (kMCPErrorPathEscape = -32013).
 *
 * Resolution rules:
 *   - Relative path → resolved against ``<ProjectDir>``.
 *   - Absolute path → normalised (case-folded on Windows; collapsed ``..`` segments) then
 *     verified to start with one of the whitelist roots.
 *   - Empty input → caller's choice (return PATH_ESCAPE OR substitute the default per tool).
 *
 * Lane B status: filesystem-only, no UObject access — but currently called from Lane A tools
 * (cb.* mutators). The implementation is thread-safe by construction (FPaths::* are static
 * accessors on cached strings).
 */
namespace FMCPPathSandbox
{
	/**
	 * Resolve ``InPathRaw`` to an absolute path, normalise, and verify it sits under one of
	 * the whitelisted roots. Returns true on success — populates ``OutAbsPath`` with the
	 * normalised absolute form. Returns false on whitelist failure — populates ``OutError``
	 * with a descriptive message (caller surfaces as PATH_ESCAPE).
	 *
	 * The path does NOT need to exist on disk. Caller decides whether to validate file
	 * existence (e.g. ``cb.import`` checks SOURCE_NOT_FOUND post-resolve).
	 */
	UNREALMCPBRIDGE_API bool Resolve(const FString& InPathRaw, FString& OutAbsPath, FString& OutError);

	/**
	 * Standalone whitelist check on an already-absolute path. Used by callers that need to
	 * verify a pre-computed path string (e.g. UE's exporter wrote to ``X``; we want to confirm
	 * X is in-sandbox before reporting bytes-written).
	 */
	UNREALMCPBRIDGE_API bool IsInsideSandbox(const FString& AbsolutePath);
}
