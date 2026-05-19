// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Phase 5 — Chunk D, Category F (Sequencer read-only) + Wave C Tier 5a (Sequencer writes).
 * 10 user-visible tools, all Lane A.
 *
 * Tool roster:
 *   sequencer.list_cinematics    → enumerate ULevelSequence assets in scope (Phase 5)
 *   sequencer.get_tracks         → master tracks + per-binding tracks (possessables/spawnables)
 *   sequencer.get_camera_cuts    → camera cut sections of a sequence
 *   sequencer.get_keyframes      → keyframes of a specific track section (float/double/bool/int)
 *   sequencer.get_current_time   → current playhead time of the focused Sequencer editor tab
 *
 *   sequencer.create_sequence    → create empty ULevelSequence asset (Wave C)
 *   sequencer.add_master_track   → add a top-level (un-bound) UMovieSceneTrack to a sequence
 *   sequencer.add_camera_cut     → add a camera-cut section + optional camera-actor binding
 *   sequencer.add_keyframe       → add a typed key on a section's channel (float/double/bool/int)
 *   sequencer.set_section_range  → set TRange<FFrameNumber> on an existing section
 *
 * **All 5 tools are Lane A** (``bThreadSafe=false``). Reasons:
 *   - ``LoadObject<ULevelSequence>`` touches the package loader + GC visited set; GT-only.
 *   - ``IAssetRegistry::GetAssets`` (used by ``list_cinematics``) asserts off-GT in UE 5.7
 *     (the GetAssetRegistryTags hazard documented in Phase 2's Hotfix 1 demotion).
 *   - ``UMovieScene::GetTracks`` / ``GetBindings`` / ``GetCameraCutTrack`` walk UObject* graphs.
 *   - ``ULevelSequenceEditorBlueprintLibrary::GetCurrentLevelSequence`` reaches into the live
 *     Sequencer editor tab — GT-only by definition (Slate state).
 *
 * **Read-only — no PIE guard.** LevelSequence assets are shared between editor and PIE worlds;
 * reads are safe in both contexts. For ``get_current_time`` the response reports the current
 * world kind ("editor" or "pie") so the caller can disambiguate.
 *
 * **Pagination intentionally omitted** — the plan's ``list_cinematics`` schema has no
 * ``page_size`` / ``next_page_token``. LevelSequence asset counts in practice are bounded by
 * cinematic scope (typically O(10-100) per project, not O(10k)); a single round-trip returning
 * the full list keeps the wire shape simple. Should this become an issue, future tools can
 * adopt ``FMCPPageCursor`` mirroring ``asset.search_by_class``.
 *
 * **Track addressing.** The ``get_tracks`` response splits tracks into two arrays:
 *   - ``master_tracks``  — ``MovieScene->GetTracks()`` (top-level tracks NOT bound to an object;
 *                         e.g. camera cuts, audio, fade, sub-sequences). Each entry has ``name``
 *                         (display name OR class name fallback) + ``class`` (UClass path) +
 *                         ``section_count``.
 *   - ``possessables``   — ``MovieScene->GetPossessable(i)`` + spawnables. Each entry has
 *                         ``name`` (display), ``object_class`` (path or null), ``binding_guid``,
 *                         ``tracks`` (list of ``{name, class, section_count}``).
 *
 * ``get_keyframes`` uses a dotted ``track_path`` to address a specific track section:
 *   - ``"MasterTrack.SectionIndex"``       — e.g. ``"CameraCutTrack.0"``
 *   - ``"PossessableName.TrackName.SectionIndex"`` — e.g. ``"Cube.Transform.0"``
 *
 * **Keyframe channel coverage (D1, plan §F-Sequencer L1287).** Phase 5 covers:
 *   - Float channels    (``FMovieSceneFloatChannel``)    — Transform tracks, scalar properties
 *   - Double channels   (``FMovieSceneDoubleChannel``)   — LWC transforms
 *   - Bool channels     (``FMovieSceneBoolChannel``)     — visibility, bool properties
 *   - Integer channels  (``FMovieSceneIntegerChannel``)  — enum/int properties
 *
 * Other channel types (event, byte, object, string, sub-section, blendable transform) fall
 * through to a sentinel entry ``{channel, type, ..., value: null}`` so the response shape stays
 * uniform. The wire schema marks ``value`` as optional for forward compatibility.
 *
 * **Frame-time semantics.** All ``time`` fields are emitted in TICKS at the sequence's tick
 * resolution (``UMovieScene::GetTickResolution()``); ``frame_rate`` carries the resolution
 * numerator+denominator. The caller divides ``ticks / frame_rate_numerator * frame_rate_denominator``
 * to get seconds, or ``ticks * display_rate / tick_resolution`` to get display-rate frames.
 *
 * **``get_current_time`` failure modes (D2).** If no Sequencer editor tab is open OR the active
 * sequence is not a ULevelSequence (e.g. a sub-sequence type unique to MetaSounds) we return
 * the new -32042 NoActiveSequencer code with the recovery hint ``"Open a LevelSequence in the
 * Sequencer editor"``. The ``world`` field reflects ``IsPIEActive()`` at call time.
 *
 * **Build.cs.** Adds private deps ``LevelSequence``, ``MovieScene``, ``MovieSceneTracks``
 * (runtime modules; needed for the static API). ``LevelSequenceEditor`` is also required for
 * ``ULevelSequenceEditorBlueprintLibrary``; it's editor-only but the plugin already declares
 * an editor-only ``Type`` in the .uplugin so the link is safe in the only build context.
 */
namespace FSequencerTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	// ─── Category F: Sequencer read-only ────────────────────────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_ListCinematics(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_GetTracks(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_GetCameraCuts(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_GetKeyframes(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_GetCurrentTime(const FMCPRequest& Request);

	// ─── Wave C Tier 5a: Sequencer writes ───────────────────────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_CreateSequence(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_AddMasterTrack(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_AddCameraCut(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_AddKeyframe(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_SetSectionRange(const FMCPRequest& Request);
}
