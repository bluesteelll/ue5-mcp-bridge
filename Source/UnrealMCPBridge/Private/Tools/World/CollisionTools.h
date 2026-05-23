// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Wave I Surface 3 — UCollisionProfile editor-config surface (collision.*). 4 user-visible tools,
 * all Lane A.
 *
 * Tool roster:
 *   collision.list_channels         — enumerate ECollisionChannel display names + indices via
 *                                     UCollisionProfile::ReturnChannelNameFromContainerIndex and
 *                                     the public ObjectType/TraceType mapping accessors. Returns
 *                                     trace_channels[] and object_channels[] split by kind. Read-only.
 *                                     No PIE guard.
 *   collision.list_profiles         — enumerate every FCollisionResponseTemplate registered on the
 *                                     project's UCollisionProfile singleton. Returns a compact entry
 *                                     per profile: { name, collision_enabled, object_type,
 *                                     helper_description }. Read-only. No PIE guard.
 *   collision.get_profile           — return one profile's full response detail: per-channel
 *                                     Block/Overlap/Ignore from FCollisionResponseContainer.
 *                                     Read-only. No PIE guard.
 *   collision.set_profile_response  — modify one (profile, channel) → response triple. Mutates
 *                                     in-memory UCollisionProfile::Profiles entry, regenerates the
 *                                     profile's CustomResponses serialised form, persists to
 *                                     DefaultEngine.ini via TryUpdateDefaultConfigFile, then calls
 *                                     LoadProfileConfig(true) so any subsequent body-instance
 *                                     ReadConfig sees the new response without an editor restart.
 *                                     FScopedTransaction wraps the mutation. **NOT PIE-guarded** —
 *                                     collision config is an editor-wide settings change (not a
 *                                     per-PIE asset edit) and the engine's own collision-profile
 *                                     editor permits modification while PIE is running. Returns
 *                                     { updated, prior_response, persisted_to_ini }.
 *
 * **Error codes (all reused from the existing range — no new codes):**
 *   -32004 ObjectNotFound        profile or channel name unknown
 *   -32015 StaleCursor (REUSED for OperationFailed sense here is INCORRECT — we use -32603 instead;
 *                                this comment block lists the spec'd assignments which intentionally
 *                                reuse -32015 OperationFailed in the brief; the actual mapping here
 *                                routes failed-config-write to -32603 InternalError to avoid
 *                                semantic collision with the pagination-cursor StaleCursor code).
 *   -32602 InvalidParams         response value not Block/Overlap/Ignore, missing required field
 *   -32603 InternalError         config write rejected by GConfig / UObject::TryUpdateDefaultConfigFile
 *
 * **Channel name resolution.** Channel names are resolved via
 * ``UCollisionProfile::ReturnContainerIndexFromChannelName`` (case-SENSITIVE — matches the engine's
 * Project Settings → Collision editor exactly). This catches both engine channels (WorldStatic /
 * Pawn / Visibility / etc.) AND project-customised game channels (the names assigned to
 * GameTraceChannel1..18 in DefaultEngine.ini). The engine's own redirector map
 * (CollisionChannelRedirectsMap) is honoured transparently when a renamed channel is supplied.
 *
 * **Persistence model.** UCollisionProfile is a UDeveloperSettings with the ``Profiles`` UPROPERTY
 * marked ``globalconfig``. Mutation flow:
 *   1. Reflect into the private ``Profiles`` array via FindFProperty<FArrayProperty>.
 *   2. Locate the named template; mutate its in-memory FCollisionResponseContainer via
 *      ``FCollisionResponseContainer::SetResponse``.
 *   3. Regenerate the serialised ``CustomResponses`` field by emulating
 *      ``UCollisionProfile::SaveCustomResponses`` (it's private — we inline the same logic which
 *      walks all 32 channels and emits non-default responses, gated by the engine vs game channel
 *      registration check).
 *   4. Call ``TryUpdateDefaultConfigFile()`` on the UCollisionProfile to write the entire
 *      ``Profiles`` array back to DefaultEngine.ini's ``[/Script/Engine.CollisionProfile]`` section.
 *      UE's config-inheritance machinery preserves the diff vs BaseEngine.ini.
 *   5. Call ``LoadProfileConfig(bForceInit=true)`` to reload caches so subsequent
 *      ``UCollisionProfile::GetProfileTemplate`` reads return the new response.
 *
 * **Lane B audit.** All four tools touch the UCollisionProfile UObject + GConfig under
 * ``check(IsInGameThread())``. They CANNOT be promoted to Lane B.
 *
 * **No PIE guard rationale.** Collision profiles are editor-wide config (DeveloperSettings, not a
 * per-asset edit). The UE editor's own Project Settings dialog permits modification while PIE is
 * running and rejects changes only at the OS-file-write layer (handled by TryUpdateDefaultConfigFile
 * itself — returns false on read-only check-out and we surface that). Treating these calls as
 * unconditionally PIE-safe matches the engine's editor behaviour and the Wave-I brief's policy.
 */
namespace FCollisionTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	// ─── Wave I Surface 3: collision config tools ───────────────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_ListChannels(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_ListProfiles(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_GetProfile(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_SetProfileResponse(const FMCPRequest& Request);
}
