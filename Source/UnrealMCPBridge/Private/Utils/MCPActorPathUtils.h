// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class AActor;
class UWorld;

/**
 * Phase 3 utility — actor path parsing / resolution.
 *
 * Phase 3 Actor tools (Days 4-8) accept three syntactic forms for identifying an actor:
 *   1. **Full soft-object path** — ``/Game/Maps/MyLevel.MyLevel:PersistentLevel.BP_Character_C_5``
 *      The unambiguous canonical form. Round-trips through ``BuildActorPath`` exactly.
 *   2. **{map_path, actor_name} JSON pair** — handled by callers via ``ParseActorPath`` on a
 *      pre-composed ``"<map_path>::<actor_name>"`` string OR by the per-tool dispatch glue. Keeps
 *      the wire surface simple (single string field) while letting AI agents that lack the full
 *      sublevel suffix still address a unique actor by name.
 *   3. **Bare name** — ``BP_Character_C_5``. Only accepted when exactly ONE loaded map contains an
 *      actor with that label/name. Multi-match → ``kMCPErrorObjectNotFound`` with disambiguation
 *      message listing all candidates.
 *
 * The grammar is deliberately permissive: the editor's own outliner displays bare names. Tools
 * targeting "the actor I just spawned" or "the first matching actor in this map" benefit; tools
 * needing strict identity (e.g. component sub-targeting) require the full path.
 *
 * **PIE rejection.** When ``bRejectPIE=true``, resolving an actor against ``GEditor->PlayWorld``
 * returns null. Phase 3 mutators use this; read-only tools pass ``false`` so they transparently
 * see PIE actors. The flag is independent of the PIE-mutator guard in MCPWorldContext —
 * MCPWorldContext refuses to MUTATE the editor world during PIE; this flag refuses to RESOLVE
 * actors that live in the PIE-only transient world.
 *
 * Threading: GAME THREAD ONLY. Iterates ``UWorld::GetLevels`` and inspects each ``ULevel::Actors``
 * directly — neither is safe off-GT.
 */
namespace FMCPActorPathUtils
{
	/**
	 * Parsed actor path components.
	 *
	 *   - ``MapPath`` — full package path of the level containing the actor, e.g. ``/Game/Maps/X``.
	 *     Empty for bare-name inputs (caller searches all loaded maps).
	 *   - ``ActorName`` — the FName::ToString() of the actor (NOT its display label). Required.
	 *   - ``bIsFullPath`` — true when MapPath was supplied (forms 1 + 2); false for bare-name form.
	 */
	struct FActorPathParts
	{
		FString MapPath;
		FString ActorName;
		bool bIsFullPath = false;
	};

	/**
	 * Parse an actor path string into components. Accepts forms 1 (canonical), 2 (joined with
	 * ``"::"`` separator), and 3 (bare name).
	 *
	 * Returns false on malformed input — leading/trailing whitespace, embedded ``\\``, empty
	 * actor-name segment. ``OutError`` carries a human-readable message; caller surfaces as
	 * ``kMCPErrorInvalidPath``.
	 */
	UNREALMCPBRIDGE_API bool ParseActorPath(const FString& Raw, FActorPathParts& OutParts, FString& OutError);

	/**
	 * Reconstruct the canonical full-form path for an actor. Output:
	 * ``<map_package_path>:PersistentLevel.<actor_fname>`` (with sublevel substituted for
	 * persistent when the actor lives in a streamed sublevel).
	 *
	 * Returns empty string for null / pending-kill actors. Asserts not null in the wrapper that
	 * expects a live actor; callers that may pass null should check before invoking.
	 */
	UNREALMCPBRIDGE_API FString BuildActorPath(const AActor* Actor);

	/**
	 * Resolve an actor path against the editor world.
	 *
	 * Resolution order:
	 *   1. Parse via ``ParseActorPath`` (returns kMCPErrorInvalidPath surface on failure).
	 *   2. If MapPath is non-empty:
	 *        a. ResolveLevelByMapPath against the editor world (or PIE world if bRejectPIE=false
	 *           and PIE is active).
	 *        b. Find actor by FName within the level's Actors array.
	 *   3. Bare name: scan ``World->GetLevels()`` for actors whose FName matches; ambiguous match
	 *      (>1) → return null + set bOutAmbiguous=true with a colon-joined candidate list in
	 *      ``OutAmbiguityHint`` (caller surfaces kMCPErrorObjectNotFound with the hint embedded).
	 *
	 * @param Raw           Path argument from the wire (any of forms 1-3).
	 * @param bRejectPIE    When true, refuses to resolve actors against ``GEditor->PlayWorld``.
	 *                      Phase 3 mutators pass true; read-only tools pass false.
	 * @param OutAmbiguous  Set true ONLY when bare-name search returned multiple candidates. Caller
	 *                      uses this to differentiate "no such actor" (false) from "name collision"
	 *                      (true) in the error surface.
	 * @param OutAmbiguityHint Populated only when OutAmbiguous=true — a ``;`` separated list of full
	 *                      candidate paths, capped at 16 entries (so the wire message stays bounded).
	 * @param OutError      Detail message when the return is null (caller surfaces).
	 */
	UNREALMCPBRIDGE_API AActor* ResolveActor(
		const FString& Raw,
		bool bRejectPIE,
		bool& OutAmbiguous,
		FString& OutAmbiguityHint,
		FString& OutError);

	/**
	 * Convenience wrapper around ``ResolveActor`` that discards the ambiguity signal and treats
	 * any null return as "not found". Used by tools that don't need to distinguish the cases (most
	 * read-only callers).
	 */
	UNREALMCPBRIDGE_API AActor* ResolveActorOrNull(const FString& Raw, bool bRejectPIE);
}
