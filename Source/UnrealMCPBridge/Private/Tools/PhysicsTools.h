// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Phase 5 — Chunk C, Category E (Physics traces). 2 user-visible tools, all Lane A.
 *
 * Tool roster (per Phase 5 plan §C-Physics lines 799-896):
 *   physics.line_trace      → line trace from start to end against a collision channel
 *   physics.sweep_capsule   → capsule sweep with optional rotation
 *
 * **All 2 tools are Lane A** (``bThreadSafe=false``). Reasons:
 *   - ``UWorld::LineTraceMultiByChannel`` / ``SweepMultiByChannel`` walk Chaos physics state under
 *     scene locks — calling from non-GT can deadlock with the physics tick.
 *   - PIE world resolution touches ``GEditor->PlayWorld`` and the global world-context array,
 *     both GT-only.
 *   - Actor resolution for ``ignore_actors`` uses ``FMCPActorPathUtils::ResolveActor`` which
 *     walks ``UWorld::GetLevels`` (GT-only).
 *
 * **World selection.** PIE world takes precedence when ``GEditor->PlayWorld`` is non-null;
 * otherwise the editor world from ``FMCPWorldContext::GetEditorWorld()``. This mirrors the
 * "trace operates on the world the user is interacting with" convention. The response includes
 * the world's package path so callers can confirm which world was queried (useful when PIE state
 * changes between calls).
 *
 * **FatumGame caveat (documented in tool description).** These tools operate on the UE physics
 * system (Chaos), NOT the Jolt/Barrage simulation thread that handles FatumGame projectiles,
 * destructibles, characters, etc. Flecs entities rendered via ISM have no UE physics body and are
 * INVISIBLE to these traces. A dedicated ``flecs.*`` trace tool family is reserved for a future
 * Phase if traces against the Jolt world become a recurring AI workflow.
 *
 * **Multi-hit vs single-hit (``multi_hit`` arg).** Always uses ``...MultiByChannel`` internally to
 * collect all-hits-until-blocking-hit; when ``multi_hit=false`` (default) we return only the FIRST
 * blocking hit if any (matches ``...SingleByChannel`` semantics). This keeps the wire shape
 * uniform — single ``hits`` array, ``hit`` boolean reflects whether at least one blocking hit
 * occurred.
 *
 * **Collision channel mapping.** Wire ``channel`` is a string like ``"Visibility"``,
 * ``"WorldStatic"``, etc. We map case-insensitively against the named ``ECC_*`` enum values via
 * ``FPhysicsTools::ParseCollisionChannel``. Unknown channel → -32041
 * (kMCPErrorInvalidCollisionChannel) with the accepted-names list in the message.
 *
 * **Ignore-actor resolution.** Each entry in ``ignore_actors`` is passed through the standard
 * Phase 3 ``FMCPActorPathUtils::ResolveActor(bRejectPIE=false)`` so callers can mix editor and
 * PIE actor IDs freely. Unresolved entries are SKIPPED with a warning log (NOT a hard error) —
 * the trace continues with whatever actors WERE resolved. Empty/missing array means "no ignores".
 *
 * **bReturnPhysicalMaterial.** Both tools set ``Params.bReturnPhysicalMaterial = true`` so the
 * resulting ``phys_mat`` field carries the physical material path when available (otherwise null).
 *
 * **Lane B audit.** These tools intentionally touch UWorld and AActor* — they CANNOT be promoted
 * to Lane B. ``check(IsInGameThread())`` enforces.
 */
namespace FPhysicsTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	// ─── Category E: Physics traces ─────────────────────────────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_LineTrace(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_SweepCapsule(const FMCPRequest& Request);
}
