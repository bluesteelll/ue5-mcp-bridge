// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UWorld;
class ULevel;

/**
 * Phase 3 utility — world / PIE / streaming-level resolution.
 *
 * Centralises the "which UWorld am I operating on?" question for Phase 3 Level/Actor/Component tools.
 * The wire surface lets callers refer to "the editor world" (default) or any loaded sublevel by its
 * map asset path. All Phase 3 mutators additionally gate on PIE — when PIE is running the editor
 * world is read-only (Phase 5 will ship a parallel pie.* surface).
 *
 * Threading: ALL methods MUST run on the game thread. GEditor / GEditor->PlayWorld / UWorld traversal
 * are not thread-safe. Phase 3 Level tools all register Lane A so this is the natural execution mode.
 *
 * **World Partition stance.** Partitioned maps return true from UWorld::IsPartitionedWorld() — they
 * have a streaming model based on world cells rather than ULevelStreamingDynamic, and require the
 * UWorldPartitionSubsystem to add/remove actors. Phase 3 hard-rejects partitioned maps with
 * kMCPErrorWorldPartitionNotSupported (-32029). Phase 5 will ship dedicated wp.* tools.
 *
 * **PIE path family.** During PIE, GEditor->PlayWorld is the play world (transient package name
 * pattern ``/Temp/UEDPIE_<instance>_<orig>``). Mutators refuse against the editor world; read-only
 * tools transparently see GEditor->PlayWorld when called during PIE. See FMCPWorldContext::IsPIEActive.
 */
namespace FMCPWorldContext
{
	/**
	 * Resolve the editor's primary world (``GEditor->GetEditorWorldContext().World()``). Returns null
	 * when GEditor is missing (commandlet / cooked build / cooker init). Phase 3 tools are
	 * editor-only — null GEditor is a hard error, not a silent fallback.
	 */
	UNREALMCPBRIDGE_API UWorld* GetEditorWorld();

	/**
	 * True when PIE is active in any flavour (PIE-in-editor, PIE standalone, simulate-in-editor).
	 * Implementation: ``GEditor != nullptr && GEditor->PlayWorld != nullptr``. Callers that need the
	 * PIE world itself should reach for ``GEditor->PlayWorld`` directly; this helper only answers
	 * the yes/no gate.
	 */
	UNREALMCPBRIDGE_API bool IsPIEActive();

	/**
	 * Resolve a sublevel by its map asset path against ``World``. Accepts forms:
	 *   - ``/Game/Maps/MyLevel`` (package path only)
	 *   - ``/Game/Maps/MyLevel.MyLevel`` (full object path)
	 *
	 * Returns the matching ULevel on success — searches ``World->GetLevels()`` for a level whose
	 * outer package name matches the normalised input. Persistent level matches when its package
	 * name equals the input.
	 *
	 * **WP rejection (D13).** When ``bRejectPartitioned=true`` and ``World->IsPartitionedWorld()``
	 * is true, sets ``bOutWPRejected=true`` and returns null. Callers MUST check the flag BEFORE
	 * the null-return so they can surface ``kMCPErrorWorldPartitionNotSupported`` instead of
	 * ``kMCPErrorLevelNotFound``.
	 *
	 * Returns null + ``bOutWPRejected=false`` when the level is simply not in ``World->GetLevels()``
	 * (caller surfaces ``kMCPErrorLevelNotFound``).
	 */
	UNREALMCPBRIDGE_API ULevel* ResolveLevelByMapPath(
		UWorld* World,
		const FString& MapPath,
		bool bRejectPartitioned,
		bool& bOutWPRejected);

	/** Convenience: ``ResolveLevelByMapPath`` ignoring the WP rejection signal. */
	UNREALMCPBRIDGE_API ULevel* ResolveLevelOrNull(UWorld* World, const FString& MapPath);

	/**
	 * Normalise a map path argument — strip trailing ``.AssetName`` suffix if present, collapse
	 * ``\\`` to ``/``, strip trailing ``/``. Returns empty string when the input is empty or
	 * lacks a leading ``/`` (callers surface ``kMCPErrorInvalidPath``). The first segment must
	 * be a recognised mount point (``/Game`` / ``/Engine`` / ``/Script`` / ``/Plugins`` / etc.) —
	 * this matches the existing asset-path normalisation behaviour in FMCPAssetPathUtils so the
	 * Phase 3 surface stays consistent with Phase 2.
	 */
	UNREALMCPBRIDGE_API FString NormaliseMapPath(const FString& Raw);

	/**
	 * Return the full object path of the persistent level's package in ``World`` (e.g.
	 * ``/Game/Maps/MyLevel``). Asserts ``World != nullptr``. Used by ``level.current_map`` and as a
	 * fallback when the caller omits ``map_path``.
	 */
	UNREALMCPBRIDGE_API FString GetWorldPackagePath(UWorld* World);
}
