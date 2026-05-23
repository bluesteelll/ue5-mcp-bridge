// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Phase 4 — Category D (Material reads) + Category E (MIC writes) + Category F (create + diagnostic).
 * 9 user-visible tools, ALL Lane A. No async composite for materials.
 *
 * Lifecycle by day (per Phase 4 plan §day-by-day Days 11-15):
 *   Day 11: ``material.list_parameters``, ``material.get_parameter``
 *   Day 12: ``material.set_scalar_param``, ``material.set_vector_param``
 *   Day 13: ``material.set_texture_param``, ``material.set_static_switch`` (+ ``is_shader_compiling``)
 *   Day 14: ``material.create_instance``, ``material.get_compile_errors``
 *   Day 15: smoke + README + buffer
 *
 * **All 9 tools are Lane A** (``bThreadSafe=false``). Reasons:
 *   - UMaterialEditingLibrary is documented GT-only by Epic.
 *   - GShaderCompilingManager queue mutations need GT for predictable backpressure.
 *   - LoadObject<UMaterialInterface> may touch shader cache + Outer chain walk under GC lock.
 *   - UMaterialInstanceConstant property writes need FScopedTransaction + PreEditChange/Modify/Post.
 *
 * **PIE guard policy (D11).** Reads (list_parameters, get_parameter, is_shader_compiling,
 * get_compile_errors) are PIE-SAFE — material assets are shared between editor and PIE worlds.
 * Writes (set_scalar/vector/texture/static_switch + create_instance) refuse during PIE with
 * ``kMCPErrorPIEActive`` (-32027) and the frozen ``kMCPMessagePIEActive`` text.
 *
 * **MIC-only writes (D9).** Every ``material.set_*`` is enforced by
 * ``FMCPMaterialUtils::LoadMICByPath`` returning -32034 MaterialClassMismatch when the path is a
 * base UMaterial / UMaterialInstanceDynamic / other UMaterialInterface subclass. Base UMaterial
 * mutations require graph-node edits (future Phase 7 ``material.edit_node``).
 *
 * **Static switch backpressure (D8).** ``material.set_static_switch`` triggers shader recompile
 * synchronously via UMaterialEditingLibrary. Before the call we check
 * ``GShaderCompilingManager->GetNumRemainingJobs() < kShaderQueueSoftLimit (=1000)`` and refuse
 * with -32035 ShaderRecompilePending if the queue is already saturated. Result includes
 * ``{recompile_triggered: true, recompile_already_pending: bool}`` so callers can poll
 * ``material.is_shader_compiling`` to know when the queue drains.
 *
 * **Dest-path conflict (D10).** ``material.create_instance`` checks
 * ``FPackageName::DoesPackageExist(dest_path)`` BEFORE creating; conflict surfaces as
 * -32014 PathInUse (existing Phase 2 code). Caller can ``cb.delete`` then retry or pick new path.
 *
 * **No World Partition check (D12).** Asset namespace; WP guards reserved for level/actor surfaces.
 *
 * **Pagination scheme.** ``material.list_parameters`` uses the standard FMCPPageCursor sentinel
 * with the material_path baked into the filter hash. Cursor key is the parameter name across the
 * FLATTENED list (scalar+vector+texture+static_switch sorted lexicographically). Mid-pagination
 * material swap → -32015 StaleCursor.
 *
 * **Group field (D5 / R5).** ``FMaterialParameterInfo`` does not surface a per-parameter group
 * name through the public UMaterialEditingLibrary API in UE 5.7. We emit ``group: ""`` for now —
 * marked optional in the wire schema. A future material.* extension may walk
 * UMaterialEditorInstanceConstant + ParameterValueGroups to populate.
 */
namespace FMaterialTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	// ─── Day 11: parameter enumeration ──────────────────────────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_ListParameters(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_GetParameter(const FMCPRequest& Request);

	// ─── Day 12: scalar + vector MIC writes (PIE-guarded; MIC-only) ─────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_SetScalarParam(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_SetVectorParam(const FMCPRequest& Request);

	// ─── Day 13: texture + static switch MIC writes ─────────────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_SetTextureParam(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_SetStaticSwitch(const FMCPRequest& Request);

	// ─── Day 13: shader queue diagnostic (no args) ──────────────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_IsShaderCompiling(const FMCPRequest& Request);

	// ─── Day 14: MIC factory + compile error read ───────────────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_CreateInstance(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_GetCompileErrors(const FMCPRequest& Request);

	// ─── Wave G Surface 2: material graph node editing (4 tools, all Lane A, PIE-guarded) ─────
	//
	// Analog of bp.add_node / bp.connect_pins / bp.set_node_property / (delete) for material graphs.
	// Operates on UMaterial assets only (NOT instances): mutating a MIC's graph is meaningless — instances
	// inherit the parent graph and only override parameter values, which is what the Day 12-13 tools cover.
	// Wrong asset class → -32011 WrongClass (consistent with bp.* graph tools).
	//
	// Lookup contract: nodes are addressed by ``UMaterialExpression::MaterialExpressionGuid`` (an FGuid
	// populated by ``CreateMaterialExpression``). FGuid strings round-trip through
	// EGuidFormats::Digits (32 hex chars, no hyphens) — same as bp.add_node's node_guid.
	//
	// Mutation contract: every tool wraps writes in ``Material->PreEditChange(nullptr)`` BEFORE and
	// ``Material->PostEditChange()`` AFTER. This triggers UE's per-tick material update cascade
	// (RecompileMaterial → RebuildMaterialInstanceEditors → shader compile queue submission). Plus
	// FScopedTransaction (editor Undo) + MarkPackageDirty.
	UNREALMCPBRIDGE_API FMCPResponse Tool_AddExpression(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_ConnectExpressions(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_SetExpressionParameter(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_DeleteExpression(const FMCPRequest& Request);
}
