// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Wave J Surface 5 — UAIPerceptionComponent inspection surface (ai.perception.*).
 * 3 user-visible tools, all Lane A, all read-only.
 *
 * Tool roster:
 *   ai.perception.list_components       — enumerate every UAIPerceptionComponent in the current
 *                                         editor world (or PIE world when active). Per entry:
 *                                         { owner_actor_path, sense_configs: [{ sense_class,
 *                                         dominant: bool }] }. Empty array is a legitimate result
 *                                         (most maps have no AI). Read-only, no PIE guard.
 *   ai.perception.get_config            — detailed sense config for one perception component.
 *                                         Returns { dominant_sense_class?, sense_configs: [...] }
 *                                         where each entry exposes sense_class, max_age,
 *                                         starts_enabled, and sense_specific_props (a JSON object
 *                                         holding the type-specific fields — sight_radius,
 *                                         peripheral_vision_angle, hearing_range, etc.).
 *                                         Read-only, no PIE guard.
 *   ai.perception.get_perceived_actors  — list actors currently perceived by the component, with
 *                                         per-stimulus diagnostics. Returns
 *                                         { perceived: [{ actor_path, sense_class, stimulus_age,
 *                                         stimulus_location: [x,y,z], is_active,
 *                                         is_successfully_sensed }] }. PIE-safe runtime tool
 *                                         (perception is interesting mainly at runtime).
 *                                         Read-only, no PIE guard.
 *
 * **No PIE guard.** All 3 tools are read-only. ``list_components`` and ``get_config`` walk
 * GEditor->PlayWorld when PIE is active (so the caller sees the running AI), otherwise the
 * editor world. ``get_perceived_actors`` is explicitly designed for PIE-time inspection — the
 * editor world has no live stimuli flowing through ``PerceptualData``.
 *
 * **Sense identification on the wire.** A "sense" is identified by its UAISense subclass path
 * string — e.g. ``"/Script/AIModule.AISense_Sight"`` or the short name ``"AISense_Sight"``. The
 * ``sense_filter`` argument to ``get_perceived_actors`` accepts both forms; the response
 * always emits the full ``/Script/<Module>.<ClassName>`` form for round-trip stability.
 *
 * **Sense-config introspection.** ``get_config`` iterates the perception component's
 * ``GetSensesConfigIterator()`` (the only public iterator over the protected ``SensesConfig``
 * array). Each ``UAISenseConfig`` instance is inspected by ``Class->IsA<UAISenseConfig_Sight>()``
 * etc. to surface type-specific properties via direct UPROPERTY reads. Unknown sense subclasses
 * (e.g. ``UAISenseConfig_Blueprint``, project-specific custom senses) get an empty
 * ``sense_specific_props`` object — the common ``max_age`` / ``starts_enabled`` /
 * ``sense_class`` triple still surfaces.
 *
 * **Stimulus iteration.** ``get_perceived_actors`` walks ``GetPerceptualDataConstIterator()``
 * — a public ``TConstIterator`` over the component's private ``PerceptualData`` map. For each
 * ``FActorPerceptionInfo`` it iterates ``LastSensedStimuli[]`` (indexed by sense ID) and emits
 * one wire entry per valid stimulus (Type != InvalidID, GetAge() < NeverHappenedAge). Stimuli
 * with ``WasSuccessfullySensed()=false`` are surfaced when the filter allows them — the
 * ``is_successfully_sensed`` flag lets callers distinguish "lost sight of" from "currently
 * visible" without a second tool call.
 *
 * **Sense filter semantics.**
 *   - Absent / empty → all senses surface.
 *   - Provided string → resolve to a ``UAISense`` subclass via ``StaticLoadClass``; entries
 *     whose ``Stimulus.Type`` doesn't match the resolved sense ID are skipped. Malformed
 *     filter string surfaces -32602 InvalidParams.
 *
 * **Lane A justification.**
 *   - ``GEditor`` / ``UWorld`` traversal: GAME-THREAD-ONLY (UObject pointer walk + GC interaction).
 *   - ``TObjectIterator<UAIPerceptionComponent>``: walks the global UObject array, GT only.
 *   - ``GetPerceptualDataConstIterator``: reads the perception component's TMap — GT only
 *     (the perception system mutates it from the GT only, but iteration during a GT-blocked
 *     phase is still GT-only by contract).
 *
 * **Error codes (all reused — no new codes added in MCPTypes.h):**
 *   - -32004 ObjectNotFound      ``actor_path`` doesn't resolve to any actor
 *   - -32010 InvalidPath         malformed ``actor_path``
 *   - -32011 WrongClass          actor resolved but has no ``UAIPerceptionComponent``
 *   - -32602 InvalidParams       missing required args OR malformed ``sense_filter`` string
 *   - -32603 InternalError       no world available (commandlet/cooker context)
 *
 * Threading: GT only (``check(IsInGameThread())`` enforced).
 */
namespace FAIPerceptionTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	UNREALMCPBRIDGE_API FMCPResponse Tool_ListComponents(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_GetConfig(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_GetPerceivedActors(const FMCPRequest& Request);
}
