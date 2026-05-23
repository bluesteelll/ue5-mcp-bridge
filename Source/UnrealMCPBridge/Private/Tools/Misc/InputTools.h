// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Wave E Surface 5 — Enhanced Input introspection surface. 4 user-visible tools, all Lane A,
 * all read-only (no PIE guard, no FScopedTransaction, no MarkPackageDirty).
 *
 * Tool roster:
 *   input.list_mapping_contexts  → paginated UInputMappingContext asset enumeration
 *   input.list_input_actions     → paginated UInputAction asset enumeration
 *   input.get_context_bindings   → walks UIMC::GetMappings() and reports per-mapping
 *                                   action_path + key + modifier-class list + trigger-class list
 *   input.list_player_contexts   → enumerate active mapping contexts on a player controller's
 *                                   UEnhancedInputLocalPlayerSubsystem
 *
 * **All 4 tools are Lane A** (``bThreadSafe=false``). LoadObject<UInputMappingContext/UInputAction>,
 * IAssetRegistry::GetAssets, ULocalPlayer/UEnhancedInputLocalPlayerSubsystem all assume the game
 * thread.
 *
 * **No PIE guard.** All 4 surfaces are introspection-only — they do not mutate the editor world.
 * input.list_player_contexts will likely return empty contexts in editor world (no possessed
 * player controller); the editor must be in PIE OR have a player-controlled GameMode active for
 * the subsystem-level enumeration to surface contexts.
 *
 * **list_player_contexts API caveat (UE 5.7).** The active applied mapping-context list lives in
 * ``UEnhancedPlayerInput::AppliedInputContextData`` (protected, friend-of-subsystem only).
 * IEnhancedInputSubsystemInterface exposes ``HasMappingContext(IMC, &OutPriority)`` for individual
 * lookups but NO public bulk enumerator in 5.7. Workaround: probe HasMappingContext against every
 * known UInputMappingContext asset in AssetRegistry — O(N) but bounded (typical projects have <50
 * IMC assets). The response includes ``hint`` field noting the probe-based approach when the asset
 * scan path is hit, distinguishing it from a "no player exists" return path.
 *
 * **Error codes (reuses existing):**
 *   -32602 InvalidParams  malformed args
 *   -32004 ObjectNotFound mapping-context / player-controller path not loadable
 *   -32010 InvalidPath    malformed asset / actor path
 *   -32011 WrongClass     asset isn't UInputMappingContext / actor isn't APlayerController
 *   -32015 StaleCursor    pagination token's filter_hash differs from current call
 */
namespace FInputTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	// ─── Wave E Surface 5: Enhanced Input introspection ────────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_ListMappingContexts(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_ListInputActions(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_GetContextBindings(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_ListPlayerContexts(const FMCPRequest& Request);
}
