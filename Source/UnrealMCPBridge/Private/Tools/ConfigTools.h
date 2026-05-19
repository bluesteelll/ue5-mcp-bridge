// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Phase 6 — Chunk C (Config / CVars). 6 user-visible synchronous tools, all Lane A.
 *
 * Tool roster (per Phase 6 plan §Category-C):
 *   cfg.get_cvar      → read a console variable's typed value + metadata (help/default/set-by tier)
 *   cfg.set_cvar      → write a console variable from JSON int/float/bool/string (D4 typed marshall)
 *   cfg.list_cvars    → paginated enumeration of registered CVars (FMCPPageCursor, name prefix
 *                       filter); excludes IConsoleCommand entries (commands ≠ variables, D5)
 *   cfg.read          → read a key from a whitelisted ini file (D8 sandbox: DefaultEngine /
 *                       DefaultGame / DefaultInput / DefaultEditor only)
 *   cfg.write         → write a string value to a whitelisted ini file; flushes via GConfig
 *   cfg.list_sections → paginated section enumeration for a whitelisted ini file
 *
 * **All 6 tools are Lane A** (``bThreadSafe=false``). Reasons:
 *   - ``IConsoleManager`` reads (FindConsoleVariable, ForEachConsoleObjectThatStartsWith) are
 *     individually thread-safe per Epic's TArray<TUniquePtr<IConsoleObject>> internal cache, but
 *     **CVar writes fire registered sinks on the game thread** (per ``SetOnChangedCallback`` doc:
 *     "Will always be on the game thread"). Pinning the entire surface to Lane A avoids the
 *     read-vs-write split and matches the same conservative posture as Test/SourceControl (Phase 1
 *     lesson — "uncertain → Lane A").
 *   - ``GConfig`` accessors are documented as "safe to call from any thread once initialised", but
 *     writes mutate the in-memory cache + flush to disk. Pinning to Lane A so concurrent reads of
 *     the same section don't race the writer (no read-write lock on ``FConfigCacheIni`` itself).
 *
 * **CVar typed marshalling (D4).** ``cfg.set_cvar`` reads the ``IConsoleVariable``'s reported type
 * via ``IsVariableBool/Int/Float/String`` and validates the incoming JSON value against it:
 *
 *   - JSON null               → -32602 InvalidParams (must supply a value)
 *   - JSON bool ↔ Bool cvar  → ``Set(*FString::FromInt(value ? 1 : 0), ECVF_SetByCode)``
 *   - JSON number ↔ Int/Float → ``Set(*FString::SanitizeFloat / FromInt, ECVF_SetByCode)``
 *   - JSON string ↔ String   → ``Set(*value, ECVF_SetByCode)`` (verbatim)
 *   - JSON string ↔ Int/Float → numeric parse via ``FString::Atoi/Atof``, reject on parse failure
 *   - mismatched non-coercible → -32006 PropertyTypeMismatch with the cvar's type echoed
 *
 * **Commands are not variables (D5).** ``cfg.get_cvar`` / ``cfg.set_cvar`` reject ``IConsoleCommand``
 * objects with -32011 WrongClass + hint pointing at ``pie.console_exec`` for execution. Without
 * this gate, callers might use ``cfg.set_cvar`` to "set" a command (semantically meaningless on
 * IConsoleVariable cast failure) and get silent no-op behaviour.
 *
 * **Read-only cvars (D9).** ``cfg.set_cvar`` checks ``TestFlags(ECVF_ReadOnly)`` AFTER the
 * command/variable gate and returns -32047 CVarReadOnly with a recovery hint pointing at the
 * matching ``[ConsoleVariables]`` ini section. Note: ECVF_ReadOnly only forbids USER edits
 * (from console / Slate UI), Epic explicitly says "Changing from C++ or ini is still possible".
 * We still surface the gate because (a) this is exactly the user-experience CVar control surface
 * AI clients are interacting with, and (b) it's a strong signal the cvar's value bakes into
 * something the running engine can't pick up without restart.
 *
 * **ini sandbox (D8).** ``cfg.read`` / ``cfg.write`` / ``cfg.list_sections`` accept only the 4
 * whitelisted ini BASE names (no extension, no path):
 *
 *   "DefaultEngine", "DefaultGame", "DefaultInput", "DefaultEditor"
 *
 * Anything else → -32013 PathEscape with the accepted set echoed in the message. Resolution:
 * ``<ProjectDir>/Config/DefaultXxx.ini``. The on-disk path is reported back via ``ini_path`` so
 * callers can verify which file was touched (e.g. for cross-platform debugging where
 * SourceConfigDir() returns a platform-specific layout).
 *
 * **Pagination.** ``cfg.list_cvars`` + ``cfg.list_sections`` use ``FMCPPageCursor`` keyed on the
 * (CVar name) / (section name) string + a filter hash. Page size defaults to 100, clamped
 * [1, 1000]. Stale cursors → -32015 (filter mutation across pages forbidden).
 *
 * **No PIE guard.** CVars are designed for runtime tweaking and ini writes are workspace-level.
 * Both work transparently during PIE (which is fine — Epic's own Console UI works during PIE).
 * Per Phase 6 plan §D: "cvar writes during PIE allowed (cvars are designed for runtime tweaking)".
 *
 * **Build.cs.** No new module deps. ``IConsoleManager``, ``FConfigCacheIni``, ``FPaths`` are all
 * in ``Core`` (already a transitive dep of the bridge module).
 */
namespace FConfigTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	// ─── Category C: Config / CVars sync tools ──────────────────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_GetCVar(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_SetCVar(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_ListCVars(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_Read(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_Write(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_ListSections(const FMCPRequest& Request);
}
