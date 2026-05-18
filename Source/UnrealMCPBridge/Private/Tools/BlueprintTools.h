// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Phase 4 — Category A (Blueprint reads). 6 user-visible tools, all Lane A.
 *
 * Lifecycle by day (per Phase 4 plan §day-by-day Days 1-5):
 *   Day 1: ``bp.exists``
 *   Day 2: ``bp.get_variable``
 *   Day 3: ``bp.list_variables``, ``bp.list_functions``
 *   Day 4: ``bp.get_function``, ``bp.list_nodes_in_function``
 *   Day 5: smoke + polish
 *
 * **All 6 tools are Lane A** (``bThreadSafe=false``). Reasons:
 *   - UBlueprint access requires GAME THREAD (Blueprint compile triggers + CDO touches under GC lock).
 *   - UEdGraph + UEdGraphNode + UEdGraphPin traversal is not thread-safe.
 *   - LoadObject<UBlueprint> can trigger deferred compile + reference graph autoload.
 *
 * **No PIE guard for Phase 4 read tools (D11).** Reads are PIE-safe — the editor's UBlueprint pointer
 * is identical in PIE and editor world (it's an asset, not a world object). Write-side tools
 * (Days 6-10) will adopt the standard PIE guard pattern.
 *
 * **No World Partition check (D12).** Asset namespace tools (`bp.*`, `material.*`) don't traverse
 * map data; the WP guard is reserved for `level.*` / `actor.*`.
 *
 * **Pagination scheme.** ``bp.list_variables`` / ``bp.list_functions`` /
 * ``bp.list_nodes_in_function`` use sentinel cursor (Phase 2 ``FMCPPageCursor``) — sort by stable
 * key (variable FName, function FName, node Guid string), encode {filter_hash, last_key} in the
 * cursor. FilterHash includes the blueprint path so mid-pagination BP swap → -32015 StaleCursor.
 *
 * **Pin type policy (D4).** Every variable/function/node pin runs through
 * ``FMCPPinTypeUtils::ToJson``. Unsupported categories produce -32032 PinTypeUnsupported — the
 * tool body returns IMMEDIATELY (no silent skip, no coercion to PC_String fallback). Forward-compat
 * for PC_Verse / future Epic-added types: appears as -32032 in the caller's response with the
 * category name in the message.
 */
namespace FBlueprintTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	// ─── Day 1: bp.exists ─────────────────────────────────────────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_Exists(const FMCPRequest& Request);

	// ─── Day 2: bp.get_variable ──────────────────────────────────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_GetVariable(const FMCPRequest& Request);

	// ─── Day 3: bp.list_variables + bp.list_functions ────────────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_ListVariables(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_ListFunctions(const FMCPRequest& Request);

	// ─── Day 4: bp.get_function + bp.list_nodes_in_function ──────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_GetFunction(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_ListNodesInFunction(const FMCPRequest& Request);
}
