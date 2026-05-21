// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Wave I Surface 6 — Landscape inspection surface (landscape.*). 4 user-visible tools, all Lane A,
 * all read-only.
 *
 * Tool roster:
 *   landscape.list              — enumerate ALandscape actors in the editor world. Per entry:
 *                                 { actor_path, component_count, world_bounds: { origin, extent } }.
 *                                 Empty array is a legitimate result (most maps have no landscape).
 *                                 Read-only, no PIE guard.
 *   landscape.get_info          — detailed stats for a single landscape: ComponentSizeQuads,
 *                                 NumSubsections, layer info list (name + Weight/Visibility type),
 *                                 min/max Z (from aggregate bounds), total component count.
 *                                 Read-only, no PIE guard.
 *   landscape.get_height_at     — sample the heightmap at a world-XY point. Returns
 *                                 { height_z, has_data, world_xy }. has_data=false when XY falls
 *                                 outside the landscape's quad extent (clamped query rejected).
 *                                 Read-only, no PIE guard.
 *   landscape.get_layer_weights — sample weight per layer at a world-XY point. Returns
 *                                 { weights: { layer_name: float (0..1) }, has_data }.
 *                                 Layers without LayerInfoObj are skipped (engine can't read their
 *                                 weight texture without a backing layer info object).
 *                                 Read-only, no PIE guard.
 *
 * **Editor-world only.** ``landscape.list`` uses ``GEditor->GetEditorWorldContext().World()`` directly
 * — these tools are designed for editor-time inspection of placed landscape assets, not PIE-time
 * runtime sampling. ULandscapeInfo + FLandscapeEditDataInterface are editor-only data paths;
 * accessing them outside the editor world is undefined.
 *
 * **No PIE guard.** All 4 tools are read-only. The editor-world resolution is mandatory anyway —
 * there's no behavioural difference between "PIE active" and "PIE not active" because we always
 * operate against the editor world, not GEditor->PlayWorld.
 *
 * **Lane A justification.**
 *   - ``GEditor`` / ``UWorld`` / ``TActorIterator`` are GAME-THREAD-ONLY (UObject pointer traversal,
 *     GC interaction).
 *   - ``ULandscapeInfo`` / ``ULandscapeLayerInfoObject`` are UObject reads — GT only.
 *   - ``FLandscapeEditDataInterface::GetHeightData`` / ``GetWeightData`` walk per-component
 *     heightmap/weightmap UTexture2D mip data via FLandscapeTextureDataInfo; touching texture
 *     resources off-GT can race the render thread's texture-streaming path.
 *
 * **Coordinate conversion.** World XY → landscape quad index uses
 * ``LS->GetActorTransform().InverseTransformPosition()`` then divides each axis by
 * ``LS->GetActorScale().X/Y`` (component-space → quad-space) and floors. World Z height comes from
 * ``LandscapeDataAccess::GetLocalHeight(uint16) * Scale.Z + ActorLocation.Z``. The landscape's
 * actor-space origin is the (0,0) quad — negative quad indices ARE valid for landscapes whose
 * origin sits inside the heightmap (rare; standard landscapes start at quad 0).
 *
 * **has_data semantics.** ``get_height_at`` / ``get_layer_weights`` report ``has_data=false`` when
 * (a) the landscape has no ``ULandscapeInfo`` (un-registered proxy), OR (b) the resolved quad lies
 * outside ``ULandscapeInfo::XYComponentBounds`` (queried via ``GetLandscapeExtent``). The original
 * world XY is echoed back in both cases so callers don't need to recompute it.
 *
 * **Visibility layer detection.** The brief specifies layer type ``"Weight"`` vs ``"Visibility"``.
 * Visibility layers are detected via ``LandscapeUtils::IsVisibilityLayer(LayerInfoObj)`` —
 * landscapes can have at most one visibility layer (the engine uses it as a hole-mask).
 *
 * **Error codes (all reused — no new codes added in MCPTypes.h):**
 *   - -32004 ObjectNotFound      landscape_path doesn't resolve to any actor
 *   - -32010 InvalidPath         malformed landscape_path
 *   - -32011 WrongClass          actor resolved but is not an ALandscape
 *   - -32602 InvalidParams       missing required args (e.g. world_x / world_y absent)
 *   - -32603 Internal            no editor world OR landscape has no ULandscapeInfo (rare —
 *                                ULandscapeInfo is created lazily on registration but normally
 *                                always present for placed landscapes)
 *
 * Threading: GT only (``check(IsInGameThread())`` enforced).
 */
namespace FLandscapeTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	UNREALMCPBRIDGE_API FMCPResponse Tool_List(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_GetInfo(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_GetHeightAt(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_GetLayerWeights(const FMCPRequest& Request);
}
