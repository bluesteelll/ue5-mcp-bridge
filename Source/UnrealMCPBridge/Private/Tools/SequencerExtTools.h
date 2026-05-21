// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Wave I Surface 5 — Sequencer extension surface (``sequencer_ext.*``, 3 tools).
 *
 * Companion to Wave C Tier 5a's ``FSequencerTools`` (see ``SequencerTools.h``). Extends the
 * Sequencer write surface with **per-binding track/section/possessable creation** — i.e. the
 * pieces a caller needs to assemble a sequence from scratch around a specific actor instance
 * after ``sequencer.create_sequence`` lays down the empty ULevelSequence.
 *
 * The Wave C tools cover:
 *   - empty-sequence creation (``sequencer.create_sequence``)
 *   - top-level (un-bound) track creation (``sequencer.add_master_track``)
 *   - camera-cut convenience (``sequencer.add_camera_cut``)
 *   - section range mutation (``sequencer.set_section_range``)
 *   - keyframe writes on float/double/bool/int channels (``sequencer.add_keyframe``)
 *
 * Wave I closes the binding/track/section CRUD triangle that the above surface deliberately
 * deferred — once you have a binding GUID you can add Transform / Visibility / Float tracks
 * and then add sections + keyframes against them. Without this surface every per-actor
 * animation requires the Sequencer editor UI (no scriptable path).
 *
 * Tool roster:
 *   sequencer_ext.add_possessable  → bind an AActor as a new possessable on a ULevelSequence
 *   sequencer_ext.add_track        → add a Transform / Visibility / Float track to a binding
 *   sequencer_ext.add_section      → add a new section to the first track of the given class
 *
 * **All 3 tools are Lane A** (``bThreadSafe=false``). Same reasons as Wave C: LoadObject +
 * UMovieScene UObject* walks + actor resolution all require the game thread.
 *
 * **PIE-guarded.** Editor-world mutators — refuse during PIE with -32027 + the frozen
 * ``kMCPMessagePIEActive`` wire string.
 *
 * **Namespace decision.** Lives under ``sequencer_ext.*`` rather than extending ``sequencer.*``
 * to keep the unity-build symbol surface clean (sibling ``SequencerTools.cpp`` already uses the
 * ``SEQ_`` prefix; we use ``SEQX_`` here) AND to signal at the wire layer that the binding
 * CRUD is a distinct surface from the camera-cut / keyframe writes that landed in Wave C. The
 * two surfaces interoperate freely — e.g. caller flow is ``sequencer.create_sequence`` →
 * ``sequencer_ext.add_possessable`` → ``sequencer_ext.add_track`` → ``sequencer_ext.add_section``
 * → ``sequencer.add_keyframe`` → ``sequencer.set_section_range``.
 *
 * **track_class accepted strings.** ``"Transform"`` / ``"3DTransform"`` → ``UMovieScene3DTransformTrack``,
 * ``"Visibility"`` → ``UMovieSceneVisibilityTrack``, ``"Float"`` → ``UMovieSceneFloatTrack``. Anything
 * else returns -32602 InvalidParams. These three cover the vast majority of per-actor animation use
 * cases; additional classes can be wired in a follow-up wave by adding lines to ``SEQX_ResolveTrackClass``.
 *
 * **Build.cs.** No new deps — ``LevelSequence`` / ``MovieScene`` / ``MovieSceneTracks`` are already
 * linked by Wave C / Phase 5 Chunk D.
 */
namespace FSequencerExtTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	UNREALMCPBRIDGE_API FMCPResponse Tool_AddPossessable(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_AddTrack(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_AddSection(const FMCPRequest& Request);
}
