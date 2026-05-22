// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FMCPDispatchQueue;

/**
 * Spatial-query surface for actors — find actors by world-space region. Complements
 * `actor.*` (path-based) and `physics.overlap_test` (shape-vs-shape sweeps).
 *
 * Tools (2):
 *   actor.box_query(min, max, class_filter?, limit?)         AABB containment by AActor::GetActorLocation
 *   actor.sphere_query(center, radius, class_filter?, limit?) Distance check from center
 *
 * Distinct from physics queries: this is a pure-geometry filter over EditorWorld actors using
 * each actor's pivot location (NOT the rendered bounds, NOT physics collision). Useful for:
 *   - "list all enemies near the player" (sphere)
 *   - "list all props inside a kill volume" (box)
 *   - "list all spawn points in this room"
 *
 * Errors:
 *   -32602 InvalidParams        Missing or malformed min/max/center/radius
 *   -32011 WrongClass           class_filter resolved to non-AActor or unknown class
 *   -32603 InternalError        GEditor or editor world unavailable
 */
namespace FActorSpatialTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);
}
