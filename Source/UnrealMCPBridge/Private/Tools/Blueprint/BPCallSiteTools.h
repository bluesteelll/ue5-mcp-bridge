// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Wave M (M1) — Blueprint call-site reverse-lookup surface.
 *
 * Tool roster (1):
 *   bp.find_function_callsites(target_function, target_class?, max_results?, cursor?)
 *     Reverse-lookup: enumerate every UK2Node_CallFunction node across all UBlueprint assets in
 *     the project whose FunctionReference matches the requested target. Critical for safe
 *     refactors — answers "who calls this function?" without manual asset hunting.
 *
 * **Kept separate from BlueprintTools** to limit unity-build bloat — the bp.* surface is already
 * spread across BlueprintTools / BlueprintCompositeTools / BlueprintComponentTools /
 * BlueprintGraphTools (5 .cpp files). Adding the call-site scan here keeps the symbol-table
 * footprint of each TU manageable.
 *
 * **Lane A.** ``LoadObject<UBlueprint>`` triggers Blueprint class deferred-compile + CDO touches
 * under the GC lock — not thread-safe. The asset-registry preflight check is also Lane A by
 * inheritance (UE 5.7 ``IAR.GetAssets`` regressed off-thread).
 *
 * **Error codes (all reused from existing range — no new codes):**
 *   -32602 InvalidParams       Missing target_function; bad cursor format.
 *   -32004 ObjectNotFound      target_class supplied but doesn't resolve to a known UClass.
 *   -32058 OperationFailed     Asset registry still loading (per critique C3 — caller retries).
 */
namespace FBPCallSiteTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	// ─── Wave M (M1): callsite reverse-lookup ────────────────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_FindFunctionCallsites(const FMCPRequest& Request);
}
