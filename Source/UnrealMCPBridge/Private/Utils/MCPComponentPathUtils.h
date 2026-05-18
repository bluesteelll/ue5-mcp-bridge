// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class AActor;
class UActorComponent;

/**
 * Phase 3 utility — component path parsing / resolution.
 *
 * Wire surface for Phase 3 Component tools (Days 9-10) names a component as
 * ``<actor_path>/<component_name>``. The slash discriminator is chosen because:
 *   - UE component names are FName (no slashes), making the split unambiguous.
 *   - Avoids the ``:`` already used by actor sub-object paths (form 1) and ``::`` (form 2).
 *
 * Component name is matched against ``UActorComponent::GetFName().ToString()`` (the internal name,
 * NOT the display label). Phase 3 tools accepting components emit the same internal name in their
 * ``component_name`` response field for symmetric round-tripping.
 *
 * **Ambiguity (D6).** An actor may have multiple components with the same internal name
 * (constructed via SCS where the user renamed duplicates colliding in the editor — rare but legal).
 * ``ResolveComponent`` sets ``bOutAmbiguous=true`` and returns null when this happens; caller
 * surfaces ``kMCPErrorComponentNotFound`` (-32021, declared by Day 9+ work) with a message
 * listing the conflicting class types.
 *
 * Threading: GAME THREAD ONLY (calls FMCPActorPathUtils::ResolveActor + AActor::GetComponents).
 */
namespace FMCPComponentPathUtils
{
	/**
	 * Parsed component path components.
	 *
	 *   - ``ActorPathRaw`` — the substring before the final ``/`` separator. Passed through to
	 *     ``FMCPActorPathUtils::ParseActorPath`` for further validation.
	 *   - ``ComponentName`` — the substring after the final ``/``. Required, non-empty.
	 */
	struct FComponentPathParts
	{
		FString ActorPathRaw;
		FString ComponentName;
	};

	/**
	 * Split ``Raw`` at the LAST ``/`` into actor path + component name. Returns false on:
	 *   - empty input
	 *   - missing ``/`` separator
	 *   - empty actor or empty component segment
	 */
	UNREALMCPBRIDGE_API bool ParseComponentPath(
		const FString& Raw,
		FComponentPathParts& OutParts,
		FString& OutError);

	/**
	 * Reconstruct the canonical full-form path for a component.
	 *
	 * Output: ``<BuildActorPath(Owner)>/<component_fname>``. Returns empty for null components or
	 * components with no owning actor.
	 */
	UNREALMCPBRIDGE_API FString BuildComponentPath(const UActorComponent* Component);

	/**
	 * Resolve a component path to a live ``UActorComponent*``.
	 *
	 * Steps:
	 *   1. ParseComponentPath (kMCPErrorInvalidPath on failure via OutError).
	 *   2. Resolve owning actor via FMCPActorPathUtils::ResolveActor (passes through bRejectPIE).
	 *   3. Iterate ``Actor->GetComponents()`` looking for FName match.
	 *      - Zero hits → return null, OutAmbiguous=false (caller: kMCPErrorComponentNotFound).
	 *      - One hit → return the component.
	 *      - >1 hit → set OutAmbiguous=true, populate OutAmbiguityHint with class names, return null
	 *        (caller surfaces an ambiguity-flavoured kMCPErrorComponentNotFound).
	 *
	 * @param Raw           Combined ``<actor_path>/<component_name>`` from the wire.
	 * @param bRejectPIE    Forwarded to FMCPActorPathUtils::ResolveActor.
	 * @param OutAmbiguous  Set true ONLY when the owning actor has multiple components with the same
	 *                      FName. Caller differentiates "no such component" (false) from "duplicate
	 *                      component name on actor" (true).
	 * @param OutAmbiguityHint  ``;`` separated list of class names for the conflicting components,
	 *                          capped at 16 entries. Empty when OutAmbiguous=false.
	 * @param OutError      Detail message (always populated on null return).
	 */
	UNREALMCPBRIDGE_API UActorComponent* ResolveComponent(
		const FString& Raw,
		bool bRejectPIE,
		bool& OutAmbiguous,
		FString& OutAmbiguityHint,
		FString& OutError);
}
