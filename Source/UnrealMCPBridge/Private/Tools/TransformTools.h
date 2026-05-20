// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Wave D Surface 6 — batch actor transform surface. 3 user-visible tools, all Lane A,
 * all PIE-guarded mutators.
 *
 * Tool roster:
 *   transform.batch_set       → apply location / rotation / scale to N actors atomically
 *                               (relative or absolute). At least one of the 3 fields required.
 *   transform.snap_to_floor   → line-trace downward from each actor's location and snap to
 *                               the first hit (configurable trace channel + max distance).
 *   transform.align           → align N actors along an axis. Mode = set / min / max / average;
 *                               min/max/average compute the aggregate from current axis values
 *                               then apply to all (set just writes the supplied value verbatim).
 *
 * **Atomic-batch semantics.** Each call wraps the WHOLE batch in a single ``FScopedTransaction``
 * so Ctrl-Z reverts the entire batch in one go (matches the outliner multi-actor drag-move UX).
 * Per-actor failures (unresolved path / no root component / etc.) land in the response's
 * ``failures[]`` array; the call returns success as long as at least one actor resolved. Only
 * when ZERO actors resolved out of the supplied list does the call top-level error with
 * ``-32004 ObjectNotFound``. For ``transform.align`` ``mode=min/max/average``, an empty resolved
 * set is a top-level ``-32602 InvalidParams`` instead (can't aggregate the empty set).
 *
 * **External-package handling.** Each modified actor's external package (one-file-per-actor
 * under WorldPartition) is dirtied separately. Falls back to the actor's outermost package
 * when ``GetExternalPackage`` returns null (non-WP maps).
 *
 * **All Lane A.** Actor traversal + transform writes + line-trace queries are all GT-only.
 *
 * Errors: standard kMCPError* (-32004 ObjectNotFound, -32027 PIEActive) +
 * -32041 InvalidCollisionChannel for ``snap_to_floor`` + -32602 InvalidParams.
 */
namespace FTransformTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	UNREALMCPBRIDGE_API FMCPResponse Tool_BatchSet(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_SnapToFloor(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_Align(const FMCPRequest& Request);
}
