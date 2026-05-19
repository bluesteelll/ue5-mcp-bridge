// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Phase 4 — Category A (Blueprint reads) + Category B (Blueprint writes) + Category C (Build).
 * 13 user-visible tools, all Lane A. The 14th Phase 4 BP tool (``bp.compile_all_dirty``) lives
 * in ``BlueprintCompositeTools`` (async composite — Lane B submitter + GT body).
 *
 * Lifecycle by day (per Phase 4 plan §day-by-day Days 1-10):
 *   Day 1: ``bp.exists``
 *   Day 2: ``bp.get_variable``
 *   Day 3: ``bp.list_variables``, ``bp.list_functions``
 *   Day 4: ``bp.get_function``, ``bp.list_nodes_in_function``
 *   Day 5: smoke + polish (reads complete)
 *   Day 6: ``bp.add_variable``, ``bp.remove_variable``
 *   Day 7: ``bp.change_variable_type``
 *   Day 8: ``bp.add_function``, ``bp.remove_function``
 *   Day 9: ``bp.reparent`` (experimental, confirm_dangerous-gated)
 *   Day 10: ``bp.compile`` (sync) + ``bp.compile_all_dirty`` (async, in BlueprintCompositeTools)
 *
 * **All 13 tools are Lane A** (``bThreadSafe=false``). Reasons:
 *   - UBlueprint access requires GAME THREAD (Blueprint compile triggers + CDO touches under GC lock).
 *   - UEdGraph + UEdGraphNode + UEdGraphPin traversal is not thread-safe.
 *   - LoadObject<UBlueprint> can trigger deferred compile + reference graph autoload.
 *   - FBlueprintEditorUtils mutators all assume GT + open transactions.
 *
 * **PIE guard policy (D11).** Read tools (exists, get/list_*) are PIE-SAFE — the editor's UBlueprint
 * pointer is identical in PIE and editor world (it's an asset, not a world object). Write-side
 * tools (add/remove/change_variable_*, add/remove_function, reparent, compile) refuse during PIE
 * with ``kMCPErrorPIEActive`` (-32027) and the frozen ``kMCPMessagePIEActive`` text. Mutating a
 * Blueprint asset during PIE corrupts the shared asset — PIE editor uses a cloned world but the
 * BP asset itself is shared.
 *
 * **Edit-const gate carve-out (Phase 3 D8 / Phase 4 D7).** The canonical 3-flag gate
 * (``CPF_BlueprintReadOnly | CPF_EditConst | CPF_DisableEditOnInstance``) is INTENTIONALLY
 * SKIPPED for ``bp.add_variable``'s ``default_value`` path. UE's ``AddMemberVariable`` defaults
 * every new BP variable to ``CPF_DisableEditOnInstance | CPF_BlueprintVisible | CPF_Edit`` —
 * the default is the CDO authoring path (``FBPVariableDescription::DefaultValue``), not a
 * runtime placed-instance mutation, so the gate would false-positive every add. The plan
 * reserves the gate for the future ``bp.set_variable_default`` tool that mutates an existing
 * variable's default post-creation; that tool will correctly apply the gate.
 *
 * **Reparent confirm gate (D2).** ``bp.reparent`` refuses without
 * ``args.confirm_dangerous=true`` (literal bool) and emits -32033 ReparentUnsafe with the frozen
 * advisory: "may invalidate variables/functions inherited from prior parent class; see
 * failure_modes". The dangerous-flag pattern mirrors ``cb.delete force=true``.
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
 *
 * **Compile semantics (D5).** ``bp.compile`` is SYNCHRONOUS Lane A. It captures a private
 * ``FCompilerResultsLog`` and returns ``{compiled, errors[], warnings[], duration_ms}``. Status
 * field uses ``UBlueprint::Status`` post-compile (BS_UpToDate / BS_Error / BS_Dirty). With default
 * ``args.fail_on_error=false`` the response is a SUCCESS with ``compiled=false`` when errors
 * occurred; with ``fail_on_error=true`` the same shape returns inside a -32030 KismetCompilationError
 * envelope so AI strict-mode callers can short-circuit.
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

	// ─── Day 6: variable add/remove (PIE-guarded; gate carve-out — see header note) ──────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_AddVariable(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_RemoveVariable(const FMCPRequest& Request);

	// ─── Day 7: variable retype (PIE-guarded; emits warning string) ──────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_ChangeVariableType(const FMCPRequest& Request);

	// ─── Day 8: function add/remove (PIE-guarded; userdef pins for inputs/outputs) ───────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_AddFunction(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_RemoveFunction(const FMCPRequest& Request);

	// ─── Day 9: reparent (experimental, PIE-guarded, confirm_dangerous-gated) ────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_Reparent(const FMCPRequest& Request);

	// ─── Day 10: synchronous single-BP compile (PIE-guarded) ─────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_Compile(const FMCPRequest& Request);

	// ─── 2026-05: generic BP asset creator with explicit parent class ────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_CreateBlueprint(const FMCPRequest& Request);
}
