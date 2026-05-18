// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Phase 3 utility — property path tokeniser supporting ``Foo.Bar[3].Baz`` syntax.
 *
 * Day 0's MCPReflection accepts ONLY dotted paths (e.g. ``RootComponent.RelativeLocation.X``). The
 * new Phase 3 property tools (actor.get_property / set_property / batch_set_property,
 * component.get_property / set_property, level.get_world_settings / set_world_settings) and the
 * refactored FMCPMarshalling extend the grammar to include array-index segments — designers
 * regularly need to address individual array elements (``SpawnPoints[2].Location``,
 * ``BlockedAttacks[0]``).
 *
 * **Tokeniser, NOT a walker.** This module returns a parsed step array. The actual property
 * traversal that consumes the steps lives in FMCPReflection — that's where the reflection
 * knowledge lives. Keeping the parse separated lets the same grammar power both the marshalling
 * surface and the Phase 3 tools without entangling the two with cross-includes.
 *
 * Grammar (EBNF-ish):
 *
 *     PropertyPath = Segment { '.' Segment }
 *     Segment      = Identifier [ ArrayIndex ]
 *     Identifier   = (Letter | '_') { Letter | Digit | '_' }
 *     ArrayIndex   = '[' Digits ']'      (Digits must form a non-negative int32)
 *
 * Whitespace is NOT permitted inside the path. Leading/trailing whitespace is trimmed by the
 * tokeniser before parsing.
 *
 * Threading: pure string operations, thread-safe. (The eventual walk in FMCPReflection is
 * game-thread-only, but the parse itself is not.)
 */
namespace FMCPPropertyPathParser
{
	/**
	 * One parsed segment.
	 *
	 *   - ``PropName`` — the identifier portion (FName-compatible). Always present, non-empty.
	 *   - ``ArrayIndex`` — non-negative when the segment included ``[N]``; ``INDEX_NONE`` (-1)
	 *     when the segment was a plain identifier.
	 */
	struct FPropertyPathStep
	{
		FString PropName;
		int32 ArrayIndex = INDEX_NONE;
	};

	/**
	 * Tokenise ``Path`` into an ordered list of steps.
	 *
	 * Returns true on success — every step in OutSteps is valid (non-empty PropName + valid
	 * ArrayIndex when present). OutSteps is empty when Path is empty after trim.
	 *
	 * Returns false on any syntax error — unmatched bracket, non-numeric index, empty identifier,
	 * negative or oversized index. ``OutError`` carries a human-readable message indicating the
	 * 0-based character offset of the first error.
	 *
	 * Implementation is a single-pass O(N) lex with no regex. No allocations beyond the OutSteps
	 * array growth itself.
	 */
	UNREALMCPBRIDGE_API bool Parse(const FString& Path, TArray<FPropertyPathStep>& OutSteps, FString& OutError);

	/**
	 * Inverse of Parse — produce the canonical wire form. Used for round-trip testing + diagnostic
	 * messages embedded in error responses (so callers see which step failed in the canonical
	 * form they sent).
	 */
	UNREALMCPBRIDGE_API FString FormatPath(const TArray<FPropertyPathStep>& Steps);
}
