// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Phase 5 — Chunk C, Category E (Physics traces). 2 read tools (Phase 5) + 4 write tools (Wave G S1),
 * 6 user-visible tools total. All Lane A.
 *
 * Tool roster:
 *   Phase 5 Chunk C — read-only traces:
 *     physics.line_trace        → line trace from start to end against a collision channel
 *     physics.sweep_capsule     → capsule sweep with optional rotation
 *   Wave G Surface 1 — runtime mutation:
 *     physics.apply_impulse     → AddImpulse on a UPrimitiveComponent (world/local frame)
 *     physics.set_simulation    → SetSimulatePhysics on/off (single or recursive)
 *     physics.set_velocity      → SetPhysicsLinearVelocity + SetPhysicsAngularVelocityInDegrees
 *     physics.overlap_test      → OverlapMultiByChannel sphere query at a world point
 *
 * **All 6 tools are Lane A** (``bThreadSafe=false``). Reasons:
 *   - ``UWorld::LineTraceMultiByChannel`` / ``SweepMultiByChannel`` / ``OverlapMultiByChannel`` walk
 *     Chaos physics state under scene locks — calling from non-GT can deadlock with the physics tick.
 *   - ``UPrimitiveComponent::AddImpulse`` / ``SetSimulatePhysics`` / ``SetPhysicsLinearVelocity`` /
 *     ``SetPhysicsAngularVelocityInDegrees`` all reach into the Chaos solver under the same lock
 *     and require GT.
 *   - PIE world resolution touches ``GEditor->PlayWorld`` and the global world-context array,
 *     both GT-only.
 *   - Actor resolution for ``ignore_actors`` / ``actor_path`` uses ``FMCPActorPathUtils::ResolveActor``
 *     which walks ``UWorld::GetLevels`` (GT-only).
 *
 * **PIE guard policy — runtime physics tools are NOT PIE-guarded.** The four Wave-G writes
 * (apply_impulse / set_simulation / set_velocity / overlap_test) operate transparently on whichever
 * world resolves first (PIE > editor) just like the traces. Physics manipulation is a runtime concern
 * (Chaos solver state) rather than an undoable asset edit — no FScopedTransaction, no MarkPackageDirty.
 * The caller is responsible for choosing the correct world context (start PIE first if the intent is
 * runtime sim manipulation; otherwise the editor-world bodies are typically inert until simulation
 * is enabled).
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

	// ─── Category E: Physics traces (Phase 5 Chunk C) ───────────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_LineTrace(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_SweepCapsule(const FMCPRequest& Request);

	// ─── Wave G Surface 1: runtime physics writes ────────────────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_ApplyImpulse(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_SetSimulation(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_SetVelocity(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_OverlapTest(const FMCPRequest& Request);
}
