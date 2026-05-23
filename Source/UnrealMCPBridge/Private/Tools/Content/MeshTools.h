// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Wave D Surface 3 — UStaticMesh inspection + edit surface. 5 user-visible tools, all Lane A.
 *
 * Tool roster:
 *   mesh.list                — paginated UStaticMesh enumeration via IAssetRegistry::GetAssets,
 *                              path_prefix-filtered. Standard FMCPPageCursor keyed by ObjectPath
 *                              with filter hash including path_prefix.
 *   mesh.get_info            — bounds (AABB + sphere radius), LOD count, material slots (incl.
 *                              material path per slot), vertex / triangle count at LOD 0,
 *                              source model count.
 *   mesh.list_lods           — per-LOD vertex / triangle count + screen-size threshold (from
 *                              FStaticMeshSourceModel::ScreenSize default platform).
 *   mesh.set_material_slot   — overwrite the material at a given slot index. FScopedTransaction +
 *                              MarkPackageDirty. PIE-guarded. Returns prior + new material.
 *   mesh.duplicate           — manual DuplicateObject<UStaticMesh> into a fresh package +
 *                              AssetRegistry::AssetCreated. PIE-guarded. PathInUse check covers
 *                              both on-disk persistence AND in-memory transient objects. DO NOT
 *                              route through IAssetTools::DuplicateAsset — it opens the Static
 *                              Mesh editor and stalls the game thread (same lesson as Wave C
 *                              creator tools).
 *
 * **Read tools (list/get_info/list_lods) bypass PIE guard.** Mutators (set_material_slot /
 * duplicate) refuse during PIE with -32027 + frozen kMCPMessagePIEActive text.
 *
 * **Error codes (all reused from existing range — no new codes):**
 *   -32004 ObjectNotFound        mesh / material / source mesh not loadable
 *   -32010 InvalidPath           malformed dest_path / mesh_path
 *   -32011 WrongClass            asset isn't UStaticMesh / UMaterialInterface
 *   -32014 PathInUse             mesh.duplicate dest_path already exists
 *   -32026 PropertyIndexOOB      mesh.set_material_slot slot_index out of range
 *   -32027 PIEActive             editor-world mutator during PIE
 *   -32602 InvalidParams         missing required args
 *   -32603 InternalError         CreatePackage / DuplicateObject returned null
 */
namespace FMeshTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	// ─── Wave D Surface 3: StaticMesh tools ────────────────────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_List(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_GetInfo(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_ListLODs(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_SetMaterialSlot(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_Duplicate(const FMCPRequest& Request);
}
