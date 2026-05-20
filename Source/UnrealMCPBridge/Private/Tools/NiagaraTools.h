// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Phase 5 — Chunk C, Category D (Niagara read-only) + Wave B (Niagara writes) + Wave E S2 (runtime).
 * 7 user-visible tools, all Lane A.
 *
 * Tool roster:
 *   niagara.list_parameters       → enumerate user / system / emitter-scoped parameters of a System (Phase 5)
 *   niagara.set_user_param        → write a typed value into UNiagaraSystem::GetExposedParameters() (Wave B)
 *   niagara.create_emitter        → spawn a UNiagaraEmitter asset via UNiagaraEmitterFactoryNew (Wave B)
 *   niagara.set_emitter_enabled   → toggle UNiagaraComponent::SetEmitterEnable on a placed actor (Wave B)
 *   niagara.spawn_at_location     → one-shot SpawnSystemAtLocation into current world (Wave E S2)
 *   niagara.stop_all              → DeactivateImmediate on every live UNiagaraComponent in world (Wave E S2)
 *   niagara.list_active           → enumerate live UNiagaraComponents in current world (Wave E S2)
 *
 * **Lane A only** (``bThreadSafe=false``). Reasons:
 *   - ``LoadObject<UNiagaraSystem>`` may trigger asset-load delegates + shader-cache touches; GT-only.
 *   - ``GetExposedParameters()`` returns a reference to a UPROPERTY struct on the system; safe read
 *     under GC lock but standard Lane-A convention applies for consistency with other read tools.
 *   - ``UNiagaraSystem::GetEmitterHandles()`` / ``GetSystemSpawnScript()`` are documented runtime
 *     operations that read the live VersionedEmitterData — GT-only.
 *
 * **Read-only — no PIE guard.** Niagara System assets are shared between editor and PIE; reads are
 * safe in both contexts.
 *
 * **Parameter categories.**
 *   - ``user_params``     — User-namespace parameters from ``UNiagaraSystem::GetExposedParameters()``
 *                          (an FNiagaraUserRedirectionParameterStore). These are what artists expose
 *                          via the "User Parameters" panel and the runtime overrides via
 *                          ``UNiagaraComponent::SetVariableXxx``.
 *   - ``system_params``   — System-scope script parameters from the system spawn + update scripts'
 *                          ``RapidIterationParameters`` (deduplicated by name+type).
 *   - ``emitter_params``  — Per-emitter parameters from each emitter's spawn + update script
 *                          ``RapidIterationParameters``. Emitter name is reported alongside name+type
 *                          so the caller can tell which emitter a parameter belongs to.
 *
 * **Default value emission (user_params only).** ``GetExposedParameters()`` is a parameter STORE
 * with packed binary values, so we can emit a typed default for primitive types. For each user
 * parameter we attempt to extract the current value via ``GetParameterData(Variable)`` and decode:
 *   - float                          → emit number
 *   - int32 (signed) / bool / FNiagaraBool → emit number / bool
 *   - FVector2f / FVector3f / FVector4f / FLinearColor / FQuat4f → emit JSON array
 *   - Anything else (UObject / DataInterface / unknown struct) → emit a string sentinel
 *     ``"<unsupported: <typename>>"`` (NEVER null — round-trip parity for the Python wrapper).
 *
 * System and emitter parameters reside in a script's ``RapidIterationParameters`` store; we DO NOT
 * decode their default values (they're internal iteration storage and the binary layout matches the
 * source script's pin defaults — the editor doesn't usually display these to artists). Wire schema
 * marks ``default`` as optional for emitter_params per plan §C-Niagara.
 *
 * **Build.cs.** Adds ``Niagara`` private dep — UNiagaraSystem / FNiagaraVariable / FNiagaraTypeDefinition
 * / UNiagaraScript / FNiagaraParameterStore + FNiagaraUserRedirectionParameterStore all live there.
 * NO ``NiagaraEditor`` dep — read-side enumeration uses only runtime types.
 */
namespace FNiagaraTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	// ─── Category D: Niagara read-only ──────────────────────────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_ListParameters(const FMCPRequest& Request);

	// ─── Wave B: Niagara writes (3 tools) ───────────────────────────────────────────────────────
	//
	// Tool_SetUserParam — write a user parameter on a UNiagaraSystem asset. Decodes JSON value
	//   per FNiagaraTypeDefinition (float/int/bool/Vec2/3/4/Quat/LinearColor/Position). Mutator,
	//   PIE-guarded. Returns prior value for round-trip diff. Marks package dirty.
	//
	// Tool_CreateEmitter — create an empty UNiagaraEmitter asset at dest_path via
	//   UNiagaraEmitterFactoryNew (no parent emitter, no default modules). Caller wires modules
	//   via editor UI or future graph-editing tools. Lane A; PIE-guarded.
	//
	// Tool_SetEmitterEnabled — toggle one emitter inside a placed UNiagaraComponent at runtime.
	//   Reads UNiagaraSystem::GetEmitterHandles() to map emitter_index → FName, then calls
	//   UNiagaraComponent::SetEmitterEnable. Works in editor world (component simulates in
	//   editor unless explicitly paused) AND PIE — NO PIE guard. Touches no asset state.
	UNREALMCPBRIDGE_API FMCPResponse Tool_SetUserParam(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_CreateEmitter(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_SetEmitterEnabled(const FMCPRequest& Request);

	// ─── Wave E Surface 2: Niagara runtime (3 tools) ────────────────────────────────────────────
	//
	// Tool_SpawnAtLocation — one-shot ``UNiagaraFunctionLibrary::SpawnSystemAtLocation`` into the
	//   current world (PIE if running, otherwise editor world). NO PIE guard — works in both.
	//   Returns the spawned component's path + which world it landed in.
	//
	// Tool_StopAll — enumerate every live UNiagaraComponent in the current world via
	//   TObjectIterator filtered by GetWorld(), call DeactivateImmediate on each. Useful for
	//   nuking all VFX before a test or when the editor world is cluttered.
	//
	// Tool_ListActive — enumerate live UNiagaraComponents in the current world. Reports
	//   component_path / owner_actor / asset_path / location / is_active / last_render_time per
	//   component. NO PIE guard — works in both editor + PIE worlds.
	UNREALMCPBRIDGE_API FMCPResponse Tool_SpawnAtLocation(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_StopAll(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_ListActive(const FMCPRequest& Request);
}
