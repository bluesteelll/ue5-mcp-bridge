// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Wave E Surface 3 — Actor parenting / attachment surface. 3 user-visible tools, all Lane A.
 *
 * Tool roster:
 *   hierarchy.attach        → AActor::AttachToActor with optional socket + FAttachmentTransformRules
 *                              (per-axis KeepRelative / KeepWorld / SnapToTarget) + weld toggle
 *   hierarchy.detach        → AActor::DetachFromActor with FDetachmentTransformRules (per-axis
 *                              KeepRelative / KeepWorld) — SnapToTarget is NOT a detach rule
 *   hierarchy.list_children → AActor::GetAttachedActors, optionally recursive; returns child path,
 *                              socket name, and label
 *
 * **All Lane A.** AActor attachment APIs walk the SceneComponent attach graph (root component
 * pointer chain) and require GT-only access to USceneComponent::AttachParent / AttachChildren.
 *
 * **PIE guard.** attach + detach refuse PIE with -32027 (editor-world mutators do not run during
 * PIE; pie.* tools / runtime attach belong on a separate surface). list_children is read-only and
 * resolves against PIE actors transparently when PIE is running.
 *
 * **Transaction + dirty.** attach + detach wrap mutation in FScopedTransaction so Ctrl-Z reverts
 * the parent change atomically. The child actor's package (external-package-aware via
 * GetExternalPackage with GetOutermost fallback — WorldPartition one-file-per-actor) is marked
 * dirty so the editor records the modification for saving.
 *
 * **Rule string mapping.** Per-axis transform rules accept the wire strings:
 *   "keep_relative" → EAttachmentRule::KeepRelative / EDetachmentRule::KeepRelative
 *   "keep_world"    → EAttachmentRule::KeepWorld    / EDetachmentRule::KeepWorld
 *   "snap"          → EAttachmentRule::SnapToTarget (attach ONLY — detach raises -32602)
 *   Default when field absent: KeepRelative for attach, KeepWorld for detach (mirrors the engine's
 *   default ctor of each rule struct).
 *
 * Errors: standard kMCPError* — no new codes:
 *   -32004 ObjectNotFound  child actor OR parent actor missing
 *   -32027 PIEActive       attach/detach refused while PIE is running
 *   -32602 InvalidParams   unknown rule string, child==parent attempt, "snap" passed to detach
 */
namespace FHierarchyTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	UNREALMCPBRIDGE_API FMCPResponse Tool_Attach(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_Detach(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_ListChildren(const FMCPRequest& Request);
}
