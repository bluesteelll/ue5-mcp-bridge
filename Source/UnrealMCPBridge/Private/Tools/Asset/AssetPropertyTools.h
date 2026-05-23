// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Wave N Surface 1 — Generic UPROPERTY read/write/list on any loaded UObject asset.
 *
 * Closes the "DataAsset / Profile / settings asset has no write API" gap. Before this surface,
 * only ``actor.set_property`` and ``component.set_property`` could mutate properties via the
 * MCP bridge — DataAssets (Profiles, EntityDefinitions, etc.) were read-only at runtime.
 *
 * Tool roster:
 *   asset.get_property    → dotted-path FProperty read (delegates to FMCPReflection helpers,
 *                           same pattern as actor.get_property minus the actor-resolve step)
 *   asset.set_property    → dotted-path FProperty write with PIE-guard, edit-const gate
 *                           (CPF_EditConst | CPF_DisableEditOnInstance unless bypass_readonly),
 *                           FMCPWritePropertyScope RAII (PreEdit/Modify/Transaction/PostEdit),
 *                           and MarkPackageDirty so the editor's "save" button activates
 *   asset.list_properties → TFieldIterator schema introspection; supports include_inherited
 *                           (default true) + max_depth (default 0 = top-level only; 1 recurses
 *                           into FStructProperty sub-fields, etc.)
 *
 * **All 3 tools are Lane A** (``bThreadSafe=false``). FMCPReflection helpers + FScopedTransaction +
 * MarkPackageDirty all require the game thread. asset.get_property + asset.list_properties COULD
 * be promoted to Lane B in the future (read-only TFieldIterator + ReadPropertyValueAt) but Wave N
 * ships them defense-first as Lane A.
 *
 * **Error codes (reuses existing — NO new codes):**
 *   -32004 ObjectNotFound          asset_path didn't resolve via FMCPReflection::ResolveObjectPath
 *   -32005 PropertyNotFound        property_path segment missing on resolved container
 *   -32006 PropertyTypeMismatch    JSON shape rejected by FMCPReflection::WritePropertyValueAt
 *   -32007 PropertyAccessDenied    CPF_EditConst / CPF_DisableEditOnInstance hit; pass
 *                                  args.bypass_readonly=true to override
 *   -32010 InvalidPath             asset_path empty / malformed
 *   -32027 PIEActive               editor-world mutator refused while GEditor->PlayWorld is non-null
 *   -32602 InvalidParams           missing required fields
 */
namespace FAssetPropertyTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	// ─── Wave N Surface 1: Generic UPROPERTY read/write/list on UObject assets ─────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_GetProperty(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_SetProperty(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_ListProperties(const FMCPRequest& Request);
}
