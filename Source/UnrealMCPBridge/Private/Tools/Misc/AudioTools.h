// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Wave C Tier 5c — Audio surface. 3 user-visible tools, all Lane A.
 *
 * Tool roster:
 *   audio.create_sound_cue   → create USoundCue asset (optional initial USoundWave wired in)
 *   audio.set_attenuation    → set or clear USoundAttenuation on any USoundBase asset
 *   audio.list_mix_classes   → enumerate USoundClass + USoundMix assets (path-prefix filtered)
 *
 * **All Lane A** — LoadObject<USoundCue/USoundWave/USoundAttenuation> + IAssetRegistry.
 * Writes PIE-guarded; list_mix_classes is read-only.
 *
 * **Sound-cue creation (no editor pop-up).** Replicates the relevant portion of
 * USoundCueFactoryNew::FactoryCreateNew inline — NewObject + (optional) USoundNodeWavePlayer
 * + FirstNode wiring — skipping IAssetTools::CreateAsset which opens the Sound Cue editor.
 *
 * Errors: standard kMCPError* — no new codes.
 */
namespace FAudioTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	UNREALMCPBRIDGE_API FMCPResponse Tool_CreateSoundCue(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_SetAttenuation(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_ListMixClasses(const FMCPRequest& Request);
}
