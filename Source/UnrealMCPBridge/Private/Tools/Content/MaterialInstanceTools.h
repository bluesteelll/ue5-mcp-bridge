// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Wave I Surface 4 — UMaterialInstanceConstant introspection + parameter override surface.
 * 5 user-visible tools, ALL Lane A.
 *
 * Tool roster:
 *   mat_inst.list              — paginated UMaterialInstanceConstant enumeration via
 *                                IAssetRegistry::GetAssets, path_prefix-filtered. Per-entry:
 *                                { asset_path, parent_path }. Standard FMCPPageCursor keyed by
 *                                ObjectPath with filter hash including path_prefix. Read-only,
 *                                no PIE guard.
 *   mat_inst.get_params        — enumerate ALL scalar + vector + texture parameters of a single MIC.
 *                                Per-param: { name, value, is_override }. ``is_override`` reflects
 *                                whether the value lives in the MIC's own override array (i.e.
 *                                visibly bold in the editor) versus being inherited from the parent
 *                                chain. ``include_inherited`` (default false) controls whether to
 *                                include parameters that resolve via the parent chain but have no
 *                                local override — when true, every parent-chain parameter is
 *                                returned (with is_override=false); when false, only parameters
 *                                present in the override arrays are returned. Read-only, no PIE guard.
 *   mat_inst.set_scalar_param  — Override a scalar parameter on the MIC. Wrapped in
 *                                FScopedTransaction + MarkPackageDirty + UpdateMaterialInstance.
 *                                Returns { set, prior_value, prior_overridden }. PIE-guarded.
 *   mat_inst.set_vector_param  — Same shape as scalar; value is [r,g,b,a]. PIE-guarded.
 *   mat_inst.set_texture_param — Same shape as scalar; value is asset path string. PIE-guarded.
 *
 * **MIC-only writes.** Every setter requires UMaterialInstanceConstant — UMaterialInstanceDynamic
 * (runtime instance, never an asset) is filtered out at the load step (not a registered Constant
 * subclass for asset registry enumeration anyway). Base UMaterial → -32011 WrongClass.
 *
 * **Read tools (list/get_params) bypass PIE guard.** Mutators refuse during PIE with -32027 +
 * frozen kMCPMessagePIEActive text.
 *
 * **Pagination.** ``mat_inst.list`` uses the standard FMCPPageCursor over ObjectPath. No
 * pagination on ``mat_inst.get_params`` — parameter count per MIC is bounded by parent material
 * graph (typically <50, hard rarely >200).
 *
 * **Error codes (all reused from existing range — no new codes):**
 *   -32004 ObjectNotFound        instance / texture path not loadable
 *   -32010 InvalidPath           malformed instance_path / texture_path
 *   -32011 WrongClass            asset isn't UMaterialInstanceConstant (UMaterial or other)
 *   -32005 PropertyNotFound      param_name not present on the parent material's parameter set
 *   -32027 PIEActive             editor-world mutator during PIE
 *   -32602 InvalidParams         missing required args / vector value not 4-element array
 *   -32603 InternalError         reserved (engine setters always return false — see cpp note —
 *                                so this code path is currently unreachable in normal operation)
 */
namespace FMaterialInstanceTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	// ─── Wave I Surface 4: mat_inst.* tools ─────────────────────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_List(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_GetParams(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_SetScalarParam(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_SetVectorParam(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_SetTextureParam(const FMCPRequest& Request);
}
