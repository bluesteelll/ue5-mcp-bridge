// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Phase 3 — Category B (Actor operations). 20 user-visible tools, all Lane A.
 *
 * Lifecycle by day (per Phase 3 plan §day-by-day Days 4-8):
 *   Day 4: ``actor.spawn``, ``actor.destroy``, ``actor.duplicate``, ``actor.get``
 *   Day 5: ``actor.set_transform``, ``actor.set_location``, ``actor.set_rotation``,
 *          ``actor.set_scale``
 *   Day 6: ``actor.set_label``, ``actor.set_folder``, ``actor.attach``, ``actor.detach``
 *   Day 7: ``actor.get_property``, ``actor.set_property``, ``actor.exists``,
 *          ``actor.select_in_editor``
 *   Day 8: ``actor.find_by_class``, ``actor.find_by_label``, ``actor.find_by_tag``,
 *          ``actor.list_components``
 *
 * **All 20 tools are Lane A** (``bThreadSafe=false``). Reasons:
 *   - Actor traversal requires GAME THREAD (UWorld / ULevel / Actor APIs not thread-safe).
 *   - Mutators wrap in FScopedTransaction (game-thread only).
 *   - Class autoload via LoadObject<UClass> can drag in Blueprint reference graphs and asserts GT.
 *
 * **Mutator PIE-guard (D10).** Every write-side handler (spawn, destroy, duplicate, set_*, attach,
 * detach, set_property) checks ``FMCPWorldContext::IsPIEActive`` first and refuses with
 * ``kMCPErrorPIEActive`` (-32027) + frozen message. Read-only handlers (get, find_by_*, exists,
 * get_property, list_components) work transparently during PIE — they see ``GEditor->PlayWorld``
 * when present via ``FMCPActorPathUtils::ResolveActor(bRejectPIE=false)``.
 *
 * **D18 sublevel-visibility gate.** All actor mutators additionally check
 * ``Actor->GetLevel()->bIsVisible`` before mutating — refuses with ``kMCPErrorLevelNotFound``
 * (-32019) when the owning sublevel is loaded but not visible. Prevents accidental writes to
 * "frozen" sublevels that the user has hidden in the outliner.
 *
 * **actor.select_in_editor exception (D10 edge case).** Selection state is purely editor-side
 * (USelection lives outside both editor and play worlds), so select_in_editor IS allowed during
 * PIE. It wraps in FScopedTransaction per M9 so users can undo selection changes.
 *
 * **Pagination scheme (D12).** ``actor.find_by_*`` use sentinel cursor (Phase 2 ``FMCPPageCursor``)
 * — sort matching actors lexicographically by GetPathName(), encode {filter_hash, last_actor_path}
 * in the cursor. Survives insertions/deletions between pages (new actor lands in its sort slot;
 * deleted actor's stable key still routes to next-greater surviving entry). FilterHash validates
 * the caller didn't mutate the filter mid-pagination.
 */
namespace FActorTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	// ─── Day 4: spawn / destroy / duplicate / get ────────────────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_Spawn(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_Destroy(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_Duplicate(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_Get(const FMCPRequest& Request);

	// ─── Day 5: transform mutators ───────────────────────────────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_SetTransform(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_SetLocation(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_SetRotation(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_SetScale(const FMCPRequest& Request);

	// ─── Day 6: identity + outliner + attachment ─────────────────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_SetLabel(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_SetFolder(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_Attach(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_Detach(const FMCPRequest& Request);

	// ─── Day 7: property reflection + presence + selection ───────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_GetProperty(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_SetProperty(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_Exists(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_SelectInEditor(const FMCPRequest& Request);

	// ─── Day 8: find + list_components ───────────────────────────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_FindByClass(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_FindByLabel(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_FindByTag(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_ListComponents(const FMCPRequest& Request);
}
