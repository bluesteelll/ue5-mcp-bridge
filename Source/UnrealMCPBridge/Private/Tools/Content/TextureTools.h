// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Wave E Surface 4 — UTexture2D inspection + edit surface. 4 user-visible tools, all Lane A.
 *
 * Tool roster:
 *   texture.list                  — paginated UTexture2D enumeration via IAssetRegistry::GetAssets,
 *                                    path_prefix-filtered. Standard FMCPPageCursor keyed by ObjectPath
 *                                    with filter hash including path_prefix. Mirror of mesh.list.
 *   texture.get_info              — size (w/h), pixel format string, compression settings string,
 *                                    mip count, sRGB flag, LOD group string, LOD bias, NeverStream
 *                                    flag, address modes (X/Y).
 *   texture.set_compression       — mutate UTexture2D::CompressionSettings. Optional update_resource
 *                                    triggers UpdateResource() so subsequent reads see the new
 *                                    encoding. FScopedTransaction + MarkPackageDirty. PIE-guarded.
 *   texture.generate_solid_color  — procedurally generate a UTexture2D asset filled with a single
 *                                    RGBA color. Manual NewObject + Source.Init pattern (NOT
 *                                    FImageUtils::CreateTexture2D — that path leaves Source data
 *                                    empty so the asset can't be re-imported / re-built; the manual
 *                                    Source.Init path stamps real BGRA8 source data into the asset).
 *                                    PIE-guarded. PathInUse check covers both on-disk persistence
 *                                    AND in-memory transient objects.
 *
 * **Read tools (list/get_info) bypass PIE guard.** Mutators (set_compression /
 * generate_solid_color) refuse during PIE with -32027 + frozen kMCPMessagePIEActive text.
 *
 * **Error codes (all reused from existing range — no new codes):**
 *   -32004 ObjectNotFound        texture not loadable
 *   -32010 InvalidPath           malformed dest_path / texture_path
 *   -32011 WrongClass            asset isn't UTexture2D
 *   -32014 PathInUse             texture.generate_solid_color dest_path already exists
 *   -32027 PIEActive             editor-world mutator during PIE
 *   -32602 InvalidParams         missing required args / bad compression_settings string / out-of-range size
 *   -32603 InternalError         CreatePackage / NewObject returned null
 */
namespace FTextureTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	// ─── Wave E Surface 4: Texture2D tools ─────────────────────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_List(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_GetInfo(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_SetCompression(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_GenerateSolidColor(const FMCPRequest& Request);
}
