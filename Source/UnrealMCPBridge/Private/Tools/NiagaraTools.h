// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Phase 5 — Chunk C, Category D (Niagara read-only). 1 user-visible tool, Lane A.
 *
 * Tool roster (per Phase 5 plan §C-Niagara lines 762-795):
 *   niagara.list_parameters  → enumerate user / system / emitter-scoped parameters of a Niagara System
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
}
