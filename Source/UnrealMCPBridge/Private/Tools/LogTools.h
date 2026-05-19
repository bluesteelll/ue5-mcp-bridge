// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Phase 6 — Chunk D (Logs additions). 3 user-visible synchronous tools, all Lane A.
 *
 * Tool roster (per Phase 6 plan §Category-D):
 *   log.set_category_verbosity → mutate a single log category's verbosity for this editor session
 *                                (NOT persisted to ini per D6); routes through the canonical
 *                                ``Log <Category> <Verbosity>`` console command via
 *                                FSelfRegisteringExec::StaticExec so UE's own suppression
 *                                implementation applies the change identically to operator-typed
 *                                ``Log`` commands.
 *   log.list_categories        → paginated enumeration of categories observed in the bridge's
 *                                in-memory log stream since attach (FMCPLogStream populates a
 *                                cumulative {FName→LastVerbosity, ObservationCount} map inside
 *                                Serialize). NOT a full enumeration of UE's suppression registry
 *                                — the public FLogSuppressionInterface does not expose name→base*
 *                                lookup, so we report what we've actually observed.
 *   log.clear                  → empty the in-memory ring buffer that backs log.tail / log.search.
 *                                Does NOT touch the on-disk file logs (UE writes those append-only
 *                                via FOutputDeviceFile to Saved/Logs/<Project>.log).
 *
 * **Phase 1 already shipped 3 log.* tools** (``log.tail`` / ``log.search`` / ``log.subscribe``
 * registered as ``FMCPDay7Handlers::LogTail/Search/Subscribe``). These 3 additions complete the
 * log surface at **6 user-visible log.* tools** for Phase 6.
 *
 * **All 3 tools are Lane A** (``bThreadSafe=false``). Reasons:
 *   - ``log.set_category_verbosity`` invokes ``FSelfRegisteringExec::StaticExec`` which routes
 *     through ``FLogSuppressionImplementation::Exec_Runtime`` — that path mutates the shared
 *     ReverseAssociations / BootAssociations maps under no documented thread guarantee.
 *   - ``log.list_categories`` is a read-only TMap walk but pinned to Lane A for consistency with
 *     ``log.tail`` (Lane A) and to avoid the Phase 1/2 "uncertain → Lane A" pattern.
 *   - ``log.clear`` mutates the FMCPLogStream ring; Lane A keeps it atomic w.r.t. log.tail
 *     callers that may interleave.
 *
 * **Verbosity coverage.** The set tool accepts the canonical UE verbosity names:
 *   ``NoLogging`` (alias for off / Fatal-only — UE treats them identically internally)
 *   ``Fatal`` / ``Error`` / ``Warning`` / ``Display`` / ``Log`` / ``Verbose`` / ``VeryVerbose``
 *   ``All`` (alias for VeryVerbose per ELogVerbosity::All)
 *   ``Off`` (alias for NoLogging; matches operator console command vocabulary)
 *   ``Default`` (resets the category to its compile-time default via ``log <cat> default`` console)
 * Resolution is case-insensitive. Unknown name → -32602 InvalidParams with the accepted-list
 * echoed in the message body.
 *
 * **D6 — process-lifetime scope.** Verbosity changes persist for the current editor session only.
 * Restarting the editor reverts. To persist, write to the matching ``[Core.Log]`` section of
 * ``DefaultEngine.ini`` via ``cfg.write`` (Chunk C):
 *   ``cfg.write {ini_file: "DefaultEngine", section: "Core.Log", key: "LogMCP", value: "Verbose"}``
 *
 * **Compile-time verbosity caps (plan risk #5).** UE caches a compile-time verbosity per category
 * (the second template param on ``DECLARE_LOG_CATEGORY_EXTERN``) which is the upper bound — runtime
 * SetVerbosity is silently clamped to ``min(requested, CompileTimeVerbosity)``. Caller can detect
 * this by re-querying via log.list_categories and observing the applied value differs from the
 * requested value. We document this in the per-call response by echoing back the resolved (post-
 * clamp) verbosity if observable.
 *
 * **D6 — observed-set caveat.** ``log.list_categories`` only knows about categories that have
 * produced at least one log entry since FMCPLogStream attached (module startup). Quiet categories
 * (e.g. a custom category that hasn't fired yet) are INVISIBLE. This is a deliberate trade-off
 * over scraping the suppression-impl's private state via reflection or fork-of-engine surgery.
 * Workaround for "I know this category exists but list_categories doesn't show it": emit a
 * UE_LOG line at any verbosity to register it in the observed-set.
 *
 * **log.set_category_verbosity success surface.** Returns ``{applied: bool, prior_verbosity: str
 * (last-observed), new_verbosity: str (requested), category: str, compile_time_clamped: bool
 * (best-effort), warning: str (optional)}``. ``applied=true`` means the console command ran;
 * ``compile_time_clamped`` is detected by comparing the requested verbosity bucket against UE's
 * compile-time cap which is only knowable post-emission. ``warning`` populated when the category
 * has not been observed (forward-reference write) — operator informed but the command still went.
 *
 * **Pagination.** ``log.list_categories`` uses ``FMCPPageCursor`` keyed on the (FName::ToString)
 * sort + a filter hash of (prefix_filter). Page size defaults to 100, clamped [1, 1000]. Stale
 * cursor (prefix mutation across pages) → -32015 StaleCursor.
 *
 * **No PIE guard.** Log mutations are workspace-level and orthogonal to PIE. The Log console
 * command works during PIE; we match that behaviour.
 *
 * **Build.cs.** No new deps. ``FSelfRegisteringExec``, ``ELogVerbosity``,
 * ``ParseLogVerbosityFromString``, ``ToString(ELogVerbosity::Type)``, ``FStringOutputDevice`` are
 * all in ``Core`` (already a transitive dep of the bridge module).
 */
namespace FLogTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	// ─── Category D: Logs sync tools (Phase 6 additions) ────────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_SetCategoryVerbosity(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_ListCategories(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_Clear(const FMCPRequest& Request);
}
