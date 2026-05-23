// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Phase 3 — Category C (Component operations). 8 user-visible tools, all Lane A.
 *
 * Lifecycle by day (per Phase 3 plan §day-by-day Days 9-10):
 *   Day 9:  ``component.add``, ``component.remove``, ``component.get``, ``component.get_property``
 *   Day 10: ``component.set_property``, ``component.set_transform``, ``component.move_in_hierarchy``,
 *           ``component.list_class_default_subcomponents``
 *
 * **All 8 tools are Lane A** (``bThreadSafe=false``). Reasons:
 *   - Actor/component traversal requires GAME THREAD (AActor::GetComponents / NewObject are not
 *     thread-safe).
 *   - Mutators wrap in FScopedTransaction (game-thread only).
 *   - Class autoload via LoadObject<UClass> can drag in Blueprint reference graphs.
 *   - USceneComponent::AttachToComponent runs scene-graph updates that aren't off-GT safe.
 *
 * **Mutator PIE-guard (D10).** Every write-side handler (add, remove, set_property, set_transform,
 * move_in_hierarchy) checks ``FMCPWorldContext::IsPIEActive`` first and refuses with
 * ``kMCPErrorPIEActive`` (-32027) + frozen message. Read-only handlers (get, get_property,
 * list_class_default_subcomponents) work transparently during PIE — they see PIE actors via
 * ``FMCPComponentPathUtils::ResolveComponent(bRejectPIE=false)``.
 *
 * **D18 sublevel-visibility gate.** All component mutators additionally check the owning actor's
 * ``Actor->GetLevel()->bIsVisible`` before mutating — refuses with ``kMCPErrorLevelNotFound``
 * (-32019) when the owning sublevel is loaded but not visible. Prevents accidental writes to
 * "frozen" sublevels that the user has hidden in the outliner.
 *
 * **Ambiguity surface (D6 / D16).** ``FMCPComponentPathUtils::ResolveComponent`` may set its
 * ``bOutAmbiguous`` flag when an actor has multiple components with the same internal FName
 * (rare SCS rename collision). Tools surface this as ``kMCPErrorAmbiguousComponent`` (-32024)
 * with a class-name list embedded in the message — distinct from ``kMCPErrorComponentNotFound``
 * for "no such component".
 *
 * **Class-resolution surface (D9, mirrors actor.spawn).** ``component.add`` accepts a class path
 * identifying ``UActorComponent`` subclasses. Error families:
 *   -32023 InvalidClassPath  — bare name without /Script/..., missing leading slash, backslash, etc.
 *   -32020 ClassNotFound     — well-formed path, LoadObject returned null after best-effort autoload
 *   -32021 ClassAbstract     — class has CLASS_Abstract
 *   -32022 WrongClassFamily  — not a UActorComponent subclass
 */
namespace FComponentTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	// ─── Day 9: add / remove / get / get_property ──────────────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_Add(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_Remove(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_Get(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_GetProperty(const FMCPRequest& Request);

	// ─── Day 10: set_property / set_transform / move_in_hierarchy / list_class_defaults ────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_SetProperty(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_SetTransform(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_MoveInHierarchy(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_ListClassDefaultSubcomponents(const FMCPRequest& Request);
}
