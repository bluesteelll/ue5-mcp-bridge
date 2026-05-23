// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FMCPDispatchQueue;

/**
 * Level-intelligence surface — aggregate queries about "what's in this level". Complements
 * ``actor.*`` (per-actor inspect) and ``actor.box_query`` / ``actor.sphere_query`` (spatial).
 *
 * Tools (2):
 *   level.actor_summary(top_n_classes?)
 *     Composite intel — total actor count, counts by class (top N), world AABB.
 *   level.find_actors_with_component(component_class, limit?, class_filter?)
 *     Filter actors by component class — "all actors with UStaticMeshComponent".
 *
 * Read-only. Lane A (TActorIterator + UActorComponent iteration are GT-only).
 *
 * Errors:
 *   -32602 InvalidParams        Missing/empty component_class, malformed top_n_classes
 *   -32011 WrongClass           component_class doesn't resolve to UActorComponent subclass
 *   -32603 InternalError        Editor world unavailable
 */
namespace FLevelIntelTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);
}
