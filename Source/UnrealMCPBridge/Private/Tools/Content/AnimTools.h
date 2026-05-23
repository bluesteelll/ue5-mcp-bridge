// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Wave C Tier 5b — Animation surface. 5 user-visible tools, all Lane A.
 *
 * Tool roster:
 *   anim.list_sequences    → paginated UAnimSequence enumeration (path-prefix filtered)
 *   anim.create_montage    → create UAnimMontage asset from a source UAnimSequence
 *   anim.add_section       → append FCompositeSection to a montage
 *   anim.add_notify        → append FAnimNotifyEvent on a montage's default notify track
 *   anim.set_blend_mode    → set BlendIn / BlendOut times on a montage
 *
 * **All 5 tools are Lane A** (``bThreadSafe=false``). LoadObject<UAnimSequence/UAnimMontage>,
 * IAssetRegistry::GetAssets, USkeleton membership checks all assume the game thread.
 *
 * **PIE-guarded writes.** ``list_sequences`` is read-only and PIE-safe. The 4 mutators
 * (create_montage, add_section, add_notify, set_blend_mode) refuse during PIE with -32027.
 *
 * **Montage creation pattern (no editor pop-up).** ULevelSequence's IAssetTools::CreateAsset
 * issue (auto-opens editor) extends to UAnimMontage too. ``anim.create_montage`` replicates
 * UAnimMontageFactory::FactoryCreateNew inline (NewObject + SetSkeleton + AddSlotTrack +
 * EnsureStartingSection) so the bridge OnEndFrame drain doesn't stall on interactive Slate.
 *
 * **Notify track addressing.** ``anim.add_notify`` appends to the "Default" notify track
 * (NotifyTrackIndex=0) unless ``notify_track_name`` is provided. Most montages start with one
 * notify track; if no tracks exist the tool creates one named "Default".
 *
 * **Error codes (reuses Phase 4/5 + adds 2 new):**
 *   -32602 InvalidParams       missing required args
 *   -32004 ObjectNotFound      sequence / montage / skeleton not found
 *   -32010 InvalidPath         malformed dest_path
 *   -32011 WrongClass          asset isn't expected class
 *   -32014 PathInUse           dest_path already on disk OR in-memory
 *   -32020 ClassNotFound       notify_class not loadable
 *   -32027 PIEActive           PIE running
 *   -32054 SkeletonMismatch    (NEW) source sequence + target montage have different skeletons
 *   -32055 NotifyTrackNotFound (NEW) named notify track doesn't exist on the montage
 */
namespace FAnimTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	// ─── Wave C Tier 5b: Animation tools ───────────────────────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_ListSequences(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_CreateMontage(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_AddSection(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_AddNotify(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_SetBlendMode(const FMCPRequest& Request);
}
