// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Wave J Surface 4 — Environment Query System (EQS) inspection / run surface. 3 user-visible tools,
 * all Lane A.
 *
 * Tool roster:
 *   ai.eqs.list_queries     — paginated UEnvQuery enumeration via IAssetRegistry. Per-entry
 *                             { asset_path, options_count }. Standard FMCPPageCursor over ObjectPath
 *                             with filter hash including path_prefix. Read-only, no PIE guard.
 *   ai.eqs.get_query_info   — describe a single EQS query — number of options, per-option generator
 *                             class name, list of tests with their classes + scoring weight. Loads
 *                             the asset on demand. Read-only, no PIE guard. Per-option shape:
 *                             { option_name, generator_class, tests: [{ test_class, weight }] }.
 *   ai.eqs.run_query        — execute the query synchronously with the supplied querier actor as
 *                             context. Mode controls SingleResult vs AllMatching scoring. Returns
 *                             { status: "Success"/"Failed"/"Aborted", results: [{ location, score,
 *                             actor_path? }] }. PIE-safe — this is a runtime tool and the whole
 *                             point is to query the LIVE world (editor world or PIE play world).
 *
 * **No PIE guard at all** — read tools (list_queries / get_query_info) are pure asset-registry /
 * asset-load reads; run_query is a runtime tool that must work against the live (potentially-PIE)
 * world. Mutators are absent from this surface.
 *
 * **Sync execution path.** ``run_query`` uses ``UEnvQueryManager::RunInstantQuery`` which executes
 * the query synchronously on the calling thread, bypassing the manager's normal time-sliced async
 * pipeline. Epic's own header comment on RunInstantQuery reads "Do not use for anything other than
 * testing or when you know exactly what you're doing! Bypasses all EQS perf controlling and time
 * slicing mechanics." — the bridge IS a testing/inspection tool by definition, so this fits.
 *
 * **Querier resolution.** ``querier_actor_path`` goes through ``FMCPActorPathUtils::ResolveActor``
 * with ``bRejectPIE=false`` — we want to address actors in either the editor world OR the PIE play
 * world transparently. The querier's UWorld is what feeds ``UEnvQueryManager::GetCurrent(World)``,
 * so PIE actors run against the PIE EQS manager and editor actors against the editor EQS manager
 * — there's no cross-world contamination risk.
 *
 * **Run mode mapping** (JSON ↔ enum):
 *   "single_best"   ↔ EEnvQueryRunMode::SingleResult   (default; picks the single best-scoring item)
 *   "all_matching"  ↔ EEnvQueryRunMode::AllMatching    (returns every item that survives all
 *                                                       filtering tests, sorted by score)
 *
 * The other two engine values (RandomBest5Pct / RandomBest25Pct) are NOT exposed — they add wire
 * surface for what's effectively a debugging mode; callers wanting random-pick semantics can
 * compute on the AllMatching result themselves.
 *
 * **Result item shape.** Each result entry carries:
 *   - ``location`` — [x, y, z] world-space FVector (FEnvQueryResult::GetAllAsLocations output)
 *   - ``score``    — float scalar from ``FEnvQueryItem::Score`` (post-normalisation)
 *   - ``actor_path`` — full actor path when the item type is actor-based (GetAllAsActors returns
 *                     a non-null actor at that index). Omitted for location-only item types
 *                     (Points, Directions, etc.) which return null actors.
 *
 * The bridge uses FMCPActorPathUtils::BuildActorPath for actor_path so the format matches every
 * other actor-returning tool in the surface.
 *
 * **Error codes (all reused from existing range — no new codes):**
 *   -32004 ObjectNotFound        Query / querier / EQS manager not loadable / missing
 *   -32010 InvalidPath           Malformed query_path / querier_actor_path
 *   -32011 WrongClass            Asset isn't UEnvQuery / actor path resolves to non-actor
 *   -32015 StaleCursor           ``list_queries`` page_token's embedded filter_hash mismatches
 *                                the current call's filter (path_prefix changed mid-pagination).
 *                                Read pagination semantic, same as canonical.
 *   -32058 OperationFailed       ``run_query`` — query returned Failed status (RawStatus ==
 *                                EEnvQueryStatus::Failed / Aborted / OwnerLost / MissingParam,
 *                                kept under one wire code; status string in message disambiguates).
 *                                Phase 4 disambiguation: was incorrectly -32015 (DOUBLE-MEANING bug
 *                                with StaleCursor); now uses canonical kMCPErrorOperationFailed.
 *   -32602 InvalidParams         Missing args / unknown mode string
 *   -32603 InternalError         UEnvQueryManager::GetCurrent returned null for the querier's world
 */
namespace FAIEQSTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	// --- Wave J Surface 4: AI / EQS tools --------------------------------------------------------
	UNREALMCPBRIDGE_API FMCPResponse Tool_ListQueries(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_GetQueryInfo(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_RunQuery(const FMCPRequest& Request);
}
