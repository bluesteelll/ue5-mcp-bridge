// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Phase 5 — Chunk A (PIE surface). 10 user-visible tools, all Lane A.
 *
 * Lifecycle by tool (per Phase 5 plan lines 39-352):
 *   pie.start             — RequestPlaySession + StartQueuedPlaySessionRequest
 *   pie.stop              — RequestEndPlayMap
 *   pie.pause             — SetPIEWorldsPaused(true)
 *   pie.resume            — SetPIEWorldsPaused(false)
 *   pie.step_frame        — PlaySessionSingleStepped (single-step a paused PIE)
 *   pie.console_exec      — World->Exec with output capture via FStringOutputDevice
 *   pie.is_running        — IsPlaySessionInProgress + world-context walk (PIE-state probe)
 *   pie.get_player_controller — UGameplayStatics::GetPlayerController(world, idx)
 *   pie.get_pawn          — PC->GetPawn()
 *   pie.focus_actor       — GEditor->MoveViewportCamerasToActor(actor, active_only=true)
 *
 * **All 10 tools are Lane A** (``bThreadSafe=false``). Reasons:
 *   - PIE lifecycle requires GAME THREAD (GEditor / RequestPlaySession / EndPlayMap not thread-safe).
 *   - World context walks read ``GEngine->GetWorldContexts()`` which is GT-only.
 *   - World->Exec, PC->ConsoleCommand assert IsInGameThread() internally.
 *   - MoveViewportCamerasToActor touches Slate viewport state (GT-only).
 *
 * **PIE-required guard (NEW pattern, inverse of Phase 3's -32027 PIEActive).**
 * Every tool EXCEPT ``pie.start`` and ``pie.is_running`` requires PIE to BE running. When
 * ``FMCPWorldContext::IsPIEActive()`` is false, refuses with ``kMCPErrorPIENotActive`` (-32038)
 * + frozen ``kMCPMessagePIENotActive`` text.
 *
 *   - ``pie.start``       — always callable; if PIE already running, returns ALREADY_RUNNING.
 *   - ``pie.is_running``  — pure probe, always callable (no PIE-required guard).
 *   - All 8 other tools   — gate on ``IsPIEActive()`` first.
 *
 * **Actor identity contract.** Tools that return actors (``pie.get_player_controller``,
 * ``pie.get_pawn``) emit both:
 *   - ``pc_actor_guid`` / ``pawn_actor_guid`` — ``AActor::GetActorGuid().ToString()`` (FGuid).
 *     Note: PIE-spawned actors may have a zero FGuid (Epic only auto-assigns to placed actors).
 *     In that case we emit an empty string; callers should fall back to the path.
 *   - ``pc_path`` / ``pawn_path`` — canonical ``FMCPActorPathUtils::BuildActorPath`` string,
 *     matching Phase 3 actor.* schema. Always non-empty for live actors.
 *
 * **No async composites in Chunk A.** No job-submit / batch tools — PIE lifecycle ops are
 * inherently single-shot and finish within one tick (or hand off to UE's deferred map-change
 * machinery which we don't poll here).
 *
 * **Console output capture.** ``pie.console_exec`` uses ``FStringOutputDevice`` as the FOutputDevice
 * sink to ``World->Exec(World, *Cmd, OutDevice)``. Output is best-effort — many console commands
 * (e.g. ``stat fps``) write directly to the viewport HUD rather than the output device, so the
 * captured string may be empty even on success. Surface as ``output`` field; ``executed=true`` iff
 * ``World->Exec`` returned true.
 */
namespace FPIETools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	// ─── PIE lifecycle ──────────────────────────────────────────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_Start(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_Stop(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_Pause(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_Resume(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_StepFrame(const FMCPRequest& Request);

	// ─── PIE introspection + console ────────────────────────────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_ConsoleExec(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_IsRunning(const FMCPRequest& Request);

	// ─── PIE actor identity + viewport focus ───────────────────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_GetPlayerController(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_GetPawn(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_FocusActor(const FMCPRequest& Request);
}
