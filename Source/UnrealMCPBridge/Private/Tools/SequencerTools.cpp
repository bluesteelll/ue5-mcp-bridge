// Copyright FatumGame. All Rights Reserved.

#include "SequencerTools.h"

#include "MCPSurfaceRegistry.h"

#include "FMCPDispatchQueue.h"
#include "MCPAssetLoader.h"
#include "MCPJsonBuilder.h"
#include "MCPMutatorScope.h"
#include "MCPToolHelpers.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPAssetPathUtils.h"
#include "Utils/MCPWorldContext.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Editor.h"
#include "GameFramework/Actor.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ScopedTransaction.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "UObject/Package.h"
#include "Utils/MCPActorPathUtils.h"
#include "Channels/MovieSceneBoolChannel.h"
#include "Channels/MovieSceneChannelData.h"
#include "Channels/MovieSceneChannelEditorData.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Channels/MovieSceneIntegerChannel.h"
#include "Curves/RichCurve.h"
#include "LevelSequence.h"
#include "LevelSequenceEditorBlueprintLibrary.h"
#include "Misc/FrameNumber.h"
#include "Misc/FrameRate.h"
#include "Misc/FrameTime.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieSceneObjectBindingID.h"
#include "MovieScenePossessable.h"
#include "MovieSceneSection.h"
#include "MovieSceneSequencePlayer.h"
#include "MovieSceneSpawnable.h"
#include "MovieSceneTrack.h"
#include "Sections/MovieSceneCameraCutSection.h"
#include "Tracks/MovieSceneCameraCutTrack.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// Per-file alias constants (kept for readability at call sites — helpers themselves come from
	// FMCPToolHelpers).
	constexpr int32 kSEQErrorInvalidParams = kMCPErrorInvalidParams;
	constexpr int32 kSEQErrorInternal      = kMCPErrorInternal;

	// ─── Frame/time JSON helpers ─────────────────────────────────────────────────────────────────

	/**
	 * Emit FFrameRate as {numerator, denominator, decimal} so callers can convert ticks to seconds
	 * exactly (rational arithmetic) OR to a float "frames per second" view.
	 */
	TSharedRef<FJsonObject> SEQ_BuildFrameRateJson(const FFrameRate& Rate)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("numerator"),   Rate.Numerator);
		Obj->SetNumberField(TEXT("denominator"), Rate.Denominator);
		// AsDecimal returns Numerator/Denominator as double — convenience for callers that just want
		// a single "fps" number.
		Obj->SetNumberField(TEXT("decimal"), Rate.AsDecimal());
		return Obj;
	}

	// ─── Track helpers ───────────────────────────────────────────────────────────────────────────

	/**
	 * Single-track summary used by both ``get_tracks`` master_tracks AND the per-binding nested
	 * tracks array.
	 *
	 * - ``name`` mirrors UMovieSceneTrack::GetDisplayName() when WITH_EDITORONLY_DATA is enabled
	 *   (always true for editor builds — this plugin is editor-only); falls back to GetName() then
	 *   class leaf name.
	 * - ``class`` is the full UClass path (e.g. ``/Script/MovieSceneTracks.MovieSceneTransformTrack``).
	 * - ``section_count`` is ``GetAllSections().Num()`` — used by the caller to pick a valid
	 *   section index for ``get_keyframes``.
	 */
	TSharedRef<FJsonObject> SEQ_BuildTrackSummaryJson(const UMovieSceneTrack* Track)
	{
		check(Track != nullptr);
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();

		FString Name;
#if WITH_EDITORONLY_DATA
		Name = Track->GetDisplayName().ToString();
#endif
		if (Name.IsEmpty())
		{
			Name = Track->GetName();
		}
		if (Name.IsEmpty())
		{
			Name = Track->GetClass()->GetName();
		}
		Obj->SetStringField(TEXT("name"),  Name);
		Obj->SetStringField(TEXT("class"), Track->GetClass()->GetPathName());
		Obj->SetNumberField(TEXT("section_count"), Track->GetAllSections().Num());
		return Obj;
	}

	// ─── Channel walker (get_keyframes) ──────────────────────────────────────────────────────────

	/**
	 * Map ERichCurveInterpMode to the wire string per the plan's interp enum.
	 *
	 * RCIM_RichCurveTangent values map:
	 *   RCIM_Linear   → "linear"
	 *   RCIM_Constant → "constant"
	 *   RCIM_Cubic    → "cubic"
	 *   RCIM_None     → "auto"  (sentinel — bool/int channels with no smooth interp)
	 */
	const TCHAR* SEQ_InterpModeToString(ERichCurveInterpMode Mode)
	{
		switch (Mode)
		{
		case RCIM_Linear:   return TEXT("linear");
		case RCIM_Constant: return TEXT("constant");
		case RCIM_Cubic:    return TEXT("cubic");
		case RCIM_None:     return TEXT("auto");
		default:            return TEXT("auto");
		}
	}

	/**
	 * Emit a single keyframe JSON object: {channel, time, value, interp}.
	 *
	 * - ``channel`` is the channel's display name (e.g. "Location.X" for a transform track) or
	 *   FName fallback ("Value"/"BoolValue") when the channel has no editor-only display data.
	 * - ``time`` is the keyframe time in TICKS at the sequence's tick resolution (see header docs).
	 * - ``value`` is the typed scalar (float/double/bool/int) OR null for unsupported channel types.
	 * - ``interp`` is the per-key curve interpolation mode (only meaningful for float/double).
	 *
	 * Caller is responsible for FoundEntry validity; this builder unconditionally writes the four
	 * fields above (``value`` may be JsonValueNull for non-numeric channels).
	 */
	TSharedRef<FJsonValue> SEQ_BuildKeyJson(
		const FString& ChannelName,
		int32 TimeTicks,
		TSharedPtr<FJsonValue> Value,
		ERichCurveInterpMode InterpMode)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("channel"), ChannelName);
		Obj->SetNumberField(TEXT("time"),    static_cast<double>(TimeTicks));
		Obj->SetField(TEXT("value"), Value.IsValid() ? Value.ToSharedRef() : MakeShared<FJsonValueNull>());
		Obj->SetStringField(TEXT("interp"),  SEQ_InterpModeToString(InterpMode));
		return MakeShared<FJsonValueObject>(Obj);
	}

	/**
	 * Build the wire ``channel`` name. Prefer the meta-data DisplayText (matches what the Sequencer
	 * editor shows in the curve view), then fall back to the meta-data Name FName, then to the
	 * channel TypeName + index suffix.
	 */
	FString SEQ_ResolveChannelName(
		const FMovieSceneChannelEntry& Entry,
		int32 ChannelIndex)
	{
		FString Out;
#if WITH_EDITOR
		TArrayView<const FMovieSceneChannelMetaData> Meta = Entry.GetMetaData();
		if (ChannelIndex >= 0 && ChannelIndex < Meta.Num())
		{
			const FMovieSceneChannelMetaData& M = Meta[ChannelIndex];
			if (!M.DisplayText.IsEmpty())
			{
				Out = M.DisplayText.ToString();
			}
			else if (!M.Name.IsNone())
			{
				Out = M.Name.ToString();
			}
		}
#endif
		if (Out.IsEmpty())
		{
			Out = FString::Printf(TEXT("%s[%d]"), *Entry.GetChannelTypeName().ToString(), ChannelIndex);
		}
		return Out;
	}

	/**
	 * Walk a single FMovieSceneChannelEntry for a section. Handles float/double/bool/integer.
	 * Other channel types append one sentinel entry per channel ({channel, time:0, value:null,
	 * interp:"auto"}) — mark the wire shape's ``value=null`` semantics as "unsupported type".
	 *
	 * Appends to OutKeys; never throws. OutKeys grows by NumChannelsInEntry × NumKeysPerChannel.
	 */
	void SEQ_WalkChannelEntry(
		const FMovieSceneChannelEntry& Entry,
		TArray<TSharedPtr<FJsonValue>>& OutKeys)
	{
		const FName TypeName = Entry.GetChannelTypeName();
		TArrayView<FMovieSceneChannel* const> Channels = Entry.GetChannels();

		// Float — has Tangent / InterpMode per key.
		if (TypeName == FMovieSceneFloatChannel::StaticStruct()->GetFName())
		{
			for (int32 Ci = 0; Ci < Channels.Num(); ++Ci)
			{
				const FMovieSceneFloatChannel* Ch = static_cast<const FMovieSceneFloatChannel*>(Channels[Ci]);
				if (!Ch) { continue; }
				const FString ChName = SEQ_ResolveChannelName(Entry, Ci);
				TArrayView<const FFrameNumber> Times = Ch->GetTimes();
				TArrayView<const FMovieSceneFloatValue> Values = Ch->GetValues();
				const int32 NumKeys = FMath::Min(Times.Num(), Values.Num());
				for (int32 Ki = 0; Ki < NumKeys; ++Ki)
				{
					TSharedPtr<FJsonValue> V = MakeShared<FJsonValueNumber>(static_cast<double>(Values[Ki].Value));
					OutKeys.Add(SEQ_BuildKeyJson(ChName, Times[Ki].Value, V, Values[Ki].InterpMode));
				}
			}
			return;
		}

		// Double — same shape as float (LWC transforms).
		if (TypeName == FMovieSceneDoubleChannel::StaticStruct()->GetFName())
		{
			for (int32 Ci = 0; Ci < Channels.Num(); ++Ci)
			{
				const FMovieSceneDoubleChannel* Ch = static_cast<const FMovieSceneDoubleChannel*>(Channels[Ci]);
				if (!Ch) { continue; }
				const FString ChName = SEQ_ResolveChannelName(Entry, Ci);
				TArrayView<const FFrameNumber> Times = Ch->GetTimes();
				TArrayView<const FMovieSceneDoubleValue> Values = Ch->GetValues();
				const int32 NumKeys = FMath::Min(Times.Num(), Values.Num());
				for (int32 Ki = 0; Ki < NumKeys; ++Ki)
				{
					TSharedPtr<FJsonValue> V = MakeShared<FJsonValueNumber>(Values[Ki].Value);
					OutKeys.Add(SEQ_BuildKeyJson(ChName, Times[Ki].Value, V, Values[Ki].InterpMode));
				}
			}
			return;
		}

		// Bool — no curve interp; emit "auto".
		if (TypeName == FMovieSceneBoolChannel::StaticStruct()->GetFName())
		{
			for (int32 Ci = 0; Ci < Channels.Num(); ++Ci)
			{
				const FMovieSceneBoolChannel* Ch = static_cast<const FMovieSceneBoolChannel*>(Channels[Ci]);
				if (!Ch) { continue; }
				const FString ChName = SEQ_ResolveChannelName(Entry, Ci);
				TArrayView<const FFrameNumber> Times = Ch->GetTimes();
				TArrayView<const bool> Values = Ch->GetValues();
				const int32 NumKeys = FMath::Min(Times.Num(), Values.Num());
				for (int32 Ki = 0; Ki < NumKeys; ++Ki)
				{
					TSharedPtr<FJsonValue> V = MakeShared<FJsonValueBoolean>(Values[Ki]);
					OutKeys.Add(SEQ_BuildKeyJson(ChName, Times[Ki].Value, V, RCIM_None));
				}
			}
			return;
		}

		// Integer — emit number, no curve interp.
		if (TypeName == FMovieSceneIntegerChannel::StaticStruct()->GetFName())
		{
			for (int32 Ci = 0; Ci < Channels.Num(); ++Ci)
			{
				const FMovieSceneIntegerChannel* Ch = static_cast<const FMovieSceneIntegerChannel*>(Channels[Ci]);
				if (!Ch) { continue; }
				const FString ChName = SEQ_ResolveChannelName(Entry, Ci);
				TArrayView<const FFrameNumber> Times = Ch->GetTimes();
				TArrayView<const int32> Values = Ch->GetValues();
				const int32 NumKeys = FMath::Min(Times.Num(), Values.Num());
				for (int32 Ki = 0; Ki < NumKeys; ++Ki)
				{
					TSharedPtr<FJsonValue> V = MakeShared<FJsonValueNumber>(static_cast<double>(Values[Ki]));
					OutKeys.Add(SEQ_BuildKeyJson(ChName, Times[Ki].Value, V, RCIM_None));
				}
			}
			return;
		}

		// Unsupported channel type — emit one sentinel entry per channel so caller knows the channel
		// exists but its value type isn't decoded. Plan §F-Sequencer L1287 (R: "channel-type coverage
		// is significant work; Phase 5 covers float + transform; other types return sentinel").
		for (int32 Ci = 0; Ci < Channels.Num(); ++Ci)
		{
			const FString ChName = SEQ_ResolveChannelName(Entry, Ci);
			OutKeys.Add(SEQ_BuildKeyJson(ChName, /*time*/ 0, MakeShared<FJsonValueNull>(), RCIM_None));
		}
	}

	// ─── Track-path resolution (sequencer.get_keyframes) ─────────────────────────────────────────

	/**
	 * Resolve dotted ``track_path`` against a MovieScene. Three patterns:
	 *   1. "MasterName.SectionIdx"            — master track by name + section
	 *   2. "PossessableName.TrackName.SectionIdx" — possessable-bound track + section
	 *   3. "SpawnableName.TrackName.SectionIdx"   — spawnable-bound track + section
	 *
	 * On success: returns the resolved UMovieSceneSection* via OutSection (non-null). On failure:
	 * populates OutErrorCode (-32602 InvalidParams / -32043 TrackNotFound / -32044 SectionIndexOOB)
	 * and OutError + leaves OutSection null.
	 *
	 * Master / possessable / spawnable names are compared case-SENSITIVELY (matches Sequencer
	 * convention; the editor's tree view is case-sensitive too). Section index is parsed as int32
	 * with a strict numeric check (rejects "1foo", accepts "1").
	 */
	UMovieSceneSection* SEQ_ResolveTrackSection(
		UMovieScene* MovieScene,
		const FString& TrackPath,
		int32& OutErrorCode,
		FString& OutError)
	{
		check(MovieScene != nullptr);

		TArray<FString> Parts;
		TrackPath.ParseIntoArray(Parts, TEXT("."), /*InCullEmpty*/ true);

		if (Parts.Num() < 2)
		{
			OutErrorCode = kSEQErrorInvalidParams;
			OutError = FString::Printf(
				TEXT("track_path '%s' must be 'MasterName.SectionIdx' OR 'BindingName.TrackName.SectionIdx'"),
				*TrackPath);
			return nullptr;
		}

		// Parse section index (always last segment).
		const FString& SectionIdxStr = Parts.Last();
		if (!SectionIdxStr.IsNumeric())
		{
			OutErrorCode = kSEQErrorInvalidParams;
			OutError = FString::Printf(
				TEXT("track_path '%s' last segment '%s' is not a numeric section index"),
				*TrackPath, *SectionIdxStr);
			return nullptr;
		}
		const int32 SectionIdx = FCString::Atoi(*SectionIdxStr);
		if (SectionIdx < 0)
		{
			OutErrorCode = kSEQErrorInvalidParams;
			OutError = FString::Printf(
				TEXT("track_path '%s' section index %d is negative"),
				*TrackPath, SectionIdx);
			return nullptr;
		}

		UMovieSceneTrack* ResolvedTrack = nullptr;

		// Pattern 1: "MasterName.SectionIdx" (Parts.Num()==2)
		if (Parts.Num() == 2)
		{
			const FString& MasterName = Parts[0];
			for (UMovieSceneTrack* Track : MovieScene->GetTracks())
			{
				if (!Track) { continue; }
				FString TrackName;
#if WITH_EDITORONLY_DATA
				TrackName = Track->GetDisplayName().ToString();
#endif
				if (TrackName.IsEmpty()) { TrackName = Track->GetName(); }
				if (TrackName.IsEmpty()) { TrackName = Track->GetClass()->GetName(); }
				if (TrackName.Equals(MasterName, ESearchCase::CaseSensitive))
				{
					ResolvedTrack = Track;
					break;
				}
			}
			// Camera-cut fallback for pattern 1 — if MasterName == "CameraCutTrack" and not found
			// in GetTracks (camera cut lives on its own slot), try GetCameraCutTrack().
			if (!ResolvedTrack)
			{
				UMovieSceneTrack* CCT = MovieScene->GetCameraCutTrack();
				if (CCT)
				{
					FString CCTName;
#if WITH_EDITORONLY_DATA
					CCTName = CCT->GetDisplayName().ToString();
#endif
					if (CCTName.IsEmpty()) { CCTName = CCT->GetName(); }
					if (CCTName.IsEmpty()) { CCTName = CCT->GetClass()->GetName(); }
					if (CCTName.Equals(MasterName, ESearchCase::CaseSensitive)
						|| MasterName.Equals(TEXT("CameraCutTrack"), ESearchCase::CaseSensitive))
					{
						ResolvedTrack = CCT;
					}
				}
			}
			if (!ResolvedTrack)
			{
				OutErrorCode = kMCPErrorTrackNotFound;
				OutError = FString::Printf(
					TEXT("track_path '%s': no master track named '%s' (use sequencer.get_tracks to enumerate)"),
					*TrackPath, *MasterName);
				return nullptr;
			}
		}
		// Pattern 2/3: "BindingName.TrackName.SectionIdx" (Parts.Num()>=3 — join middle segments)
		else
		{
			const FString& BindingName = Parts[0];
			// Middle segments joined with '.' — allows track names with dots in them (rare, but
			// possible for property paths like ``LightComponent0.Intensity``).
			FString TrackName;
			for (int32 i = 1; i < Parts.Num() - 1; ++i)
			{
				if (!TrackName.IsEmpty()) { TrackName += TEXT("."); }
				TrackName += Parts[i];
			}

			// Find the binding GUID by display name.
			FGuid BindingGuid;
			bool bFoundBinding = false;
			const int32 PossessableCount = MovieScene->GetPossessableCount();
			for (int32 i = 0; i < PossessableCount; ++i)
			{
				const FMovieScenePossessable& P = MovieScene->GetPossessable(i);
				if (P.GetName().Equals(BindingName, ESearchCase::CaseSensitive))
				{
					BindingGuid = P.GetGuid();
					bFoundBinding = true;
					break;
				}
			}
			if (!bFoundBinding)
			{
				const int32 SpawnableCount = MovieScene->GetSpawnableCount();
				for (int32 i = 0; i < SpawnableCount; ++i)
				{
					const FMovieSceneSpawnable& S = MovieScene->GetSpawnable(i);
					if (S.GetName().Equals(BindingName, ESearchCase::CaseSensitive))
					{
						BindingGuid = S.GetGuid();
						bFoundBinding = true;
						break;
					}
				}
			}
			if (!bFoundBinding)
			{
				OutErrorCode = kMCPErrorTrackNotFound;
				OutError = FString::Printf(
					TEXT("track_path '%s': no possessable or spawnable named '%s' (use sequencer.get_tracks)"),
					*TrackPath, *BindingName);
				return nullptr;
			}

			// Locate the binding via GUID and walk its tracks.
			const FMovieSceneBinding* Binding = MovieScene->FindBinding(BindingGuid);
			if (!Binding)
			{
				OutErrorCode = kSEQErrorInternal;
				OutError = FString::Printf(
					TEXT("track_path '%s': binding guid %s present in possessables/spawnables but no FMovieSceneBinding"),
					*TrackPath, *BindingGuid.ToString());
				return nullptr;
			}

			for (UMovieSceneTrack* Track : Binding->GetTracks())
			{
				if (!Track) { continue; }
				FString CandidateName;
#if WITH_EDITORONLY_DATA
				CandidateName = Track->GetDisplayName().ToString();
#endif
				if (CandidateName.IsEmpty()) { CandidateName = Track->GetName(); }
				if (CandidateName.IsEmpty()) { CandidateName = Track->GetClass()->GetName(); }
				if (CandidateName.Equals(TrackName, ESearchCase::CaseSensitive))
				{
					ResolvedTrack = Track;
					break;
				}
			}
			if (!ResolvedTrack)
			{
				OutErrorCode = kMCPErrorTrackNotFound;
				OutError = FString::Printf(
					TEXT("track_path '%s': binding '%s' has no track named '%s'"),
					*TrackPath, *BindingName, *TrackName);
				return nullptr;
			}
		}

		// Section index check.
		const TArray<UMovieSceneSection*>& Sections = ResolvedTrack->GetAllSections();
		if (SectionIdx >= Sections.Num())
		{
			OutErrorCode = kMCPErrorSectionIndexOOB;
			OutError = FString::Printf(
				TEXT("track_path '%s': section index %d out of range (track has %d sections)"),
				*TrackPath, SectionIdx, Sections.Num());
			return nullptr;
		}

		UMovieSceneSection* Section = Sections[SectionIdx];
		if (!Section)
		{
			OutErrorCode = kSEQErrorInternal;
			OutError = FString::Printf(
				TEXT("track_path '%s': section %d is null in track '%s'"),
				*TrackPath, SectionIdx, *ResolvedTrack->GetName());
			return nullptr;
		}
		return Section;
	}
} // namespace

namespace FSequencerTools
{

// ─── sequencer.list_cinematics ────────────────────────────────────────────────────────────────
//
// Args:    { scope_paths?: array<string>  default ["/Game"] }
// Result:  { sequences: [{ path, name, duration_secs, frame_rate }, ...] }
//
// Errors:
//   -32010 InvalidPath          scope_paths entry malformed / unknown mount
//   -32603 Internal             AssetRegistry not available
//
// Iterates LevelSequence assets via FARFilter(ClassPaths=["/Script/LevelSequence.LevelSequence"]).
// For each FAssetData we LoadObject<ULevelSequence> + read MovieScene->GetPlaybackRange() +
// TickResolution. Cold-loaded assets are loaded on first query (intentional — the cinematic count
// is typically O(10-100) per project and the AI tools want the full metadata). For large catalogs
// future polish may add cold/hot toggle + pagination.
//
// Sorting: sequences are NOT sorted (preserved in AR order — usually package-path lex). Caller
// can sort client-side; preserving AR order gives a stable surface for diff tooling.
FMCPResponse Tool_ListCinematics(const FMCPRequest& Request)
{
	check(IsInGameThread());

	// Parse scope_paths (optional, default ["/Game"]).
	TArray<FName> Scope;
	if (Request.Args.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* ScopeArr = nullptr;
		if (Request.Args->TryGetArrayField(TEXT("scope_paths"), ScopeArr) && ScopeArr)
		{
			for (const TSharedPtr<FJsonValue>& V : *ScopeArr)
			{
				FString S;
				if (!V.IsValid() || !V->TryGetString(S))
				{
					return FMCPToolHelpers::MakeError(Request, kSEQErrorInvalidParams,
						TEXT("scope_paths: expected array of strings"));
				}
				const FString Norm = FMCPAssetPathUtils::Normalize(S);
				if (Norm.IsEmpty())
				{
					return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidPath,
						FString::Printf(TEXT("scope_paths entry '%s' is malformed"), *S));
				}
				Scope.Add(FName(*Norm));
			}
		}
	}
	if (Scope.Num() == 0)
	{
		Scope.Add(FName(TEXT("/Game")));
	}

	IAssetRegistry& IAR = FAssetRegistryModule::GetRegistry();

	// Build filter — LevelSequence class only, recursive over scope_paths.
	FARFilter Filter;
	Filter.ClassPaths.Add(FTopLevelAssetPath(FName(TEXT("/Script/LevelSequence")), FName(TEXT("LevelSequence"))));
	Filter.PackagePaths = MoveTemp(Scope);
	Filter.bRecursivePaths = true;

	TArray<FAssetData> Assets;
	IAR.GetAssets(Filter, Assets);

	TArray<TSharedPtr<FJsonValue>> SeqArr;
	SeqArr.Reserve(Assets.Num());
	for (const FAssetData& Data : Assets)
	{
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		const FString PackageName = Data.PackageName.ToString();
		Entry->SetStringField(TEXT("path"), PackageName);
		Entry->SetStringField(TEXT("name"), Data.AssetName.ToString());

		// Load + compute duration/frame_rate. LoadObject on UE 5.7 is safe under the GT lock; cold
		// assets are loaded on demand (the user opted in by calling list_cinematics on this scope).
		ULevelSequence* Seq = Cast<ULevelSequence>(Data.GetAsset());
		if (Seq && Seq->GetMovieScene())
		{
			const UMovieScene* MovieScene = Seq->GetMovieScene();
			const FFrameRate TickRes = MovieScene->GetTickResolution();
			const TRange<FFrameNumber> Range = MovieScene->GetPlaybackRange();
			double DurationSecs = 0.0;
			if (Range.HasLowerBound() && Range.HasUpperBound())
			{
				const int32 StartTicks = Range.GetLowerBoundValue().Value;
				const int32 EndTicks   = Range.GetUpperBoundValue().Value;
				const int32 DurationTicks = EndTicks - StartTicks;
				DurationSecs = TickRes.AsSeconds(FFrameTime(DurationTicks));
			}
			Entry->SetNumberField(TEXT("duration_secs"), DurationSecs);
			Entry->SetObjectField(TEXT("frame_rate"), SEQ_BuildFrameRateJson(MovieScene->GetDisplayRate()));
		}
		else
		{
			// Asset didn't load OR has no movie scene — emit nulls. Caller can detect and retry with
			// explicit cb.save / asset.metadata for diagnosis.
			Entry->SetField(TEXT("duration_secs"), MakeShared<FJsonValueNull>());
			Entry->SetField(TEXT("frame_rate"), MakeShared<FJsonValueNull>());
		}

		SeqArr.Add(MakeShared<FJsonValueObject>(Entry));
	}

	return FMCPJsonBuilder()
		.Arr(TEXT("sequences"), MoveTemp(SeqArr))
		.BuildSuccess(Request);
}

// ─── sequencer.get_tracks ─────────────────────────────────────────────────────────────────────
//
// Args:    { sequence_path: string }
// Result:  {
//            master_tracks: [{ name, class, section_count }, ...],
//            possessables:  [{ name, object_class, binding_guid, tracks: [{ name, class, section_count }] }, ...],
//            spawnables:    [{ name, object_class, binding_guid, tracks: [...] }, ...]
//          }
//
// Errors:
//   -32602 InvalidParams        missing sequence_path
//   -32010 InvalidPath          path malformed / unknown mount
//   -32004 ObjectNotFound       LoadObject returned null
//   -32011 WrongClass           asset is not ULevelSequence
//   -32603 Internal             MovieScene null on a valid ULevelSequence (shouldn't happen)
//
// CameraCut track (if present) is included in master_tracks via the MovieScene->GetCameraCutTrack()
// fallback path — UE 5.7 stores the camera cut track in its own slot, NOT in GetTracks().
FMCPResponse Tool_GetTracks(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString Path;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("sequence_path"), Path, Err)) { return Err; }

	int32 ErrCode = 0;
	FString ErrMsg;
	ULevelSequence* Seq = FMCPAssetLoader::Load<ULevelSequence>(Path, ErrCode, ErrMsg);
	if (!Seq) { return FMCPToolHelpers::MakeError(Request, ErrCode, ErrMsg); }

	UMovieScene* MovieScene = Seq->GetMovieScene();
	if (!MovieScene)
	{
		return FMCPToolHelpers::MakeError(Request, kSEQErrorInternal,
			FString::Printf(TEXT("sequence_path '%s' has no MovieScene"), *Path));
	}

	// ─── Master tracks ──────────────────────────────────────────────────────────────────────────
	TArray<TSharedPtr<FJsonValue>> MasterArr;
	for (UMovieSceneTrack* Track : MovieScene->GetTracks())
	{
		if (!Track) { continue; }
		MasterArr.Add(MakeShared<FJsonValueObject>(SEQ_BuildTrackSummaryJson(Track)));
	}
	// CameraCut track lives in its own slot; surface it under master_tracks.
	if (UMovieSceneTrack* CCT = MovieScene->GetCameraCutTrack())
	{
		MasterArr.Add(MakeShared<FJsonValueObject>(SEQ_BuildTrackSummaryJson(CCT)));
	}

	// Helper to walk a single binding's tracks (used by both possessables and spawnables).
	auto BuildBindingTracksJson = [MovieScene](const FGuid& BindingGuid) -> TArray<TSharedPtr<FJsonValue>>
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		const FMovieSceneBinding* B = MovieScene->FindBinding(BindingGuid);
		if (!B) { return Out; }
		for (UMovieSceneTrack* Track : B->GetTracks())
		{
			if (!Track) { continue; }
			Out.Add(MakeShared<FJsonValueObject>(SEQ_BuildTrackSummaryJson(Track)));
		}
		return Out;
	};

	// ─── Possessables ───────────────────────────────────────────────────────────────────────────
	TArray<TSharedPtr<FJsonValue>> PossArr;
	const int32 PossCount = MovieScene->GetPossessableCount();
	for (int32 i = 0; i < PossCount; ++i)
	{
		const FMovieScenePossessable& P = MovieScene->GetPossessable(i);
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), P.GetName());
		const UClass* PossClass = P.GetPossessedObjectClass();
		if (PossClass)
		{
			Obj->SetStringField(TEXT("object_class"), PossClass->GetPathName());
		}
		else
		{
			Obj->SetField(TEXT("object_class"), MakeShared<FJsonValueNull>());
		}
		Obj->SetStringField(TEXT("binding_guid"), P.GetGuid().ToString(EGuidFormats::DigitsWithHyphens));
		Obj->SetArrayField(TEXT("tracks"), BuildBindingTracksJson(P.GetGuid()));
		PossArr.Add(MakeShared<FJsonValueObject>(Obj));
	}

	// ─── Spawnables ─────────────────────────────────────────────────────────────────────────────
	TArray<TSharedPtr<FJsonValue>> SpawnArr;
	const int32 SpawnCount = MovieScene->GetSpawnableCount();
	for (int32 i = 0; i < SpawnCount; ++i)
	{
		FMovieSceneSpawnable& S = MovieScene->GetSpawnable(i);
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), S.GetName());
		const UClass* SpawnClass = S.GetObjectTemplate() ? S.GetObjectTemplate()->GetClass() : nullptr;
		if (SpawnClass)
		{
			Obj->SetStringField(TEXT("object_class"), SpawnClass->GetPathName());
		}
		else
		{
			Obj->SetField(TEXT("object_class"), MakeShared<FJsonValueNull>());
		}
		Obj->SetStringField(TEXT("binding_guid"), S.GetGuid().ToString(EGuidFormats::DigitsWithHyphens));
		Obj->SetArrayField(TEXT("tracks"), BuildBindingTracksJson(S.GetGuid()));
		SpawnArr.Add(MakeShared<FJsonValueObject>(Obj));
	}

	return FMCPJsonBuilder()
		.Arr(TEXT("master_tracks"), MoveTemp(MasterArr))
		.Arr(TEXT("possessables"),  MoveTemp(PossArr))
		.Arr(TEXT("spawnables"),    MoveTemp(SpawnArr))
		.BuildSuccess(Request);
}

// ─── sequencer.get_camera_cuts ────────────────────────────────────────────────────────────────
//
// Args:    { sequence_path: string }
// Result:  {
//            cuts: [{ start_frame, end_frame, camera_binding: <guid string|null>, has_lower_bound, has_upper_bound }, ...],
//            frame_rate: { numerator, denominator, decimal }
//          }
//
// Errors:
//   -32602 InvalidParams        missing sequence_path
//   -32010 InvalidPath          path malformed / unknown mount
//   -32004 ObjectNotFound       LoadObject returned null
//   -32011 WrongClass           asset is not ULevelSequence
//
// When no camera-cut track exists, ``cuts`` is empty (NOT an error — matches plan's NO_CAMERA_CUT
// failure mode: "Empty cuts; not an error").
//
// Frames are emitted in TICKS at the MovieScene's TickResolution; the response embeds the rate so
// callers can convert: seconds = ticks * denominator / numerator.
FMCPResponse Tool_GetCameraCuts(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString Path;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("sequence_path"), Path, Err)) { return Err; }

	int32 ErrCode = 0;
	FString ErrMsg;
	ULevelSequence* Seq = FMCPAssetLoader::Load<ULevelSequence>(Path, ErrCode, ErrMsg);
	if (!Seq) { return FMCPToolHelpers::MakeError(Request, ErrCode, ErrMsg); }

	UMovieScene* MovieScene = Seq->GetMovieScene();
	if (!MovieScene)
	{
		return FMCPToolHelpers::MakeError(Request, kSEQErrorInternal,
			FString::Printf(TEXT("sequence_path '%s' has no MovieScene"), *Path));
	}

	TArray<TSharedPtr<FJsonValue>> CutsArr;
	if (UMovieSceneTrack* CCT = MovieScene->GetCameraCutTrack())
	{
		for (UMovieSceneSection* Section : CCT->GetAllSections())
		{
			if (!Section) { continue; }
			UMovieSceneCameraCutSection* CutSection = Cast<UMovieSceneCameraCutSection>(Section);

			TSharedRef<FJsonObject> CutObj = MakeShared<FJsonObject>();
			const TRange<FFrameNumber> Range = Section->GetRange();
			const bool bHasLower = Range.HasLowerBound();
			const bool bHasUpper = Range.HasUpperBound();
			CutObj->SetBoolField(TEXT("has_lower_bound"), bHasLower);
			CutObj->SetBoolField(TEXT("has_upper_bound"), bHasUpper);
			CutObj->SetNumberField(TEXT("start_frame"),
				bHasLower ? static_cast<double>(Range.GetLowerBoundValue().Value) : 0.0);
			CutObj->SetNumberField(TEXT("end_frame"),
				bHasUpper ? static_cast<double>(Range.GetUpperBoundValue().Value) : 0.0);

			if (CutSection)
			{
				const FGuid CamGuid = CutSection->GetCameraBindingID().GetGuid();
				if (CamGuid.IsValid())
				{
					CutObj->SetStringField(TEXT("camera_binding"),
						CamGuid.ToString(EGuidFormats::DigitsWithHyphens));
				}
				else
				{
					CutObj->SetField(TEXT("camera_binding"), MakeShared<FJsonValueNull>());
				}
			}
			else
			{
				CutObj->SetField(TEXT("camera_binding"), MakeShared<FJsonValueNull>());
			}
			CutsArr.Add(MakeShared<FJsonValueObject>(CutObj));
		}
	}

	return FMCPJsonBuilder()
		.Arr(TEXT("cuts"), MoveTemp(CutsArr))
		.ObjectShared(TEXT("frame_rate"), SEQ_BuildFrameRateJson(MovieScene->GetTickResolution()))
		.BuildSuccess(Request);
}

// ─── sequencer.get_keyframes ──────────────────────────────────────────────────────────────────
//
// Args:    { sequence_path: string, track_path: string }
// Result:  {
//            keys: [{ channel, time, value, interp }, ...],
//            section_range: { has_lower, has_upper, start, end },
//            frame_rate: { numerator, denominator, decimal },
//            supported_types: ["float","double","bool","integer"]
//          }
//
// Errors:
//   -32602 InvalidParams        missing sequence_path or track_path / malformed track_path
//   -32010 InvalidPath          sequence_path malformed
//   -32004 ObjectNotFound       LoadObject returned null
//   -32011 WrongClass           asset is not ULevelSequence
//   -32043 TrackNotFound        track_path didn't resolve to a track
//   -32044 SectionIndexOOB      section index segment exceeds GetAllSections().Num()
//   -32603 Internal             MovieScene null / section null
//
// track_path forms (see SEQ_ResolveTrackSection docs):
//   "MasterName.SectionIdx"               (e.g. "CameraCutTrack.0")
//   "BindingName.TrackName.SectionIdx"    (e.g. "Cube.Transform.0")
//
// Channel coverage: float, double, bool, integer. Other types emit sentinel keys with value=null.
FMCPResponse Tool_GetKeyframes(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString SeqPath;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("sequence_path"), SeqPath, Err)) { return Err; }

	FString TrackPath;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("track_path"), TrackPath, Err)) { return Err; }

	int32 ErrCode = 0;
	FString ErrMsg;
	ULevelSequence* Seq = FMCPAssetLoader::Load<ULevelSequence>(SeqPath, ErrCode, ErrMsg);
	if (!Seq) { return FMCPToolHelpers::MakeError(Request, ErrCode, ErrMsg); }

	UMovieScene* MovieScene = Seq->GetMovieScene();
	if (!MovieScene)
	{
		return FMCPToolHelpers::MakeError(Request, kSEQErrorInternal,
			FString::Printf(TEXT("sequence_path '%s' has no MovieScene"), *SeqPath));
	}

	int32 ResolveErrCode = 0;
	FString ResolveErrMsg;
	UMovieSceneSection* Section = SEQ_ResolveTrackSection(MovieScene, TrackPath, ResolveErrCode, ResolveErrMsg);
	if (!Section) { return FMCPToolHelpers::MakeError(Request, ResolveErrCode, ResolveErrMsg); }

	// Walk channels via ChannelProxy.
	TArray<TSharedPtr<FJsonValue>> KeyArr;
	{
		FMovieSceneChannelProxy& Proxy = Section->GetChannelProxy();
		for (const FMovieSceneChannelEntry& Entry : Proxy.GetAllEntries())
		{
			SEQ_WalkChannelEntry(Entry, KeyArr);
		}
	}

	// Build response.
	const TRange<FFrameNumber> Range = Section->GetRange();
	const bool bHasLower = Range.HasLowerBound();
	const bool bHasUpper = Range.HasUpperBound();

	TArray<TSharedPtr<FJsonValue>> Types;
	Types.Add(MakeShared<FJsonValueString>(TEXT("float")));
	Types.Add(MakeShared<FJsonValueString>(TEXT("double")));
	Types.Add(MakeShared<FJsonValueString>(TEXT("bool")));
	Types.Add(MakeShared<FJsonValueString>(TEXT("integer")));

	return FMCPJsonBuilder()
		.Arr(TEXT("keys"), MoveTemp(KeyArr))
		.Object(TEXT("section_range"), [&](FMCPJsonBuilder& R)
		{
			// Section range echo (for caller convenience — saves a round-trip to get_tracks).
			R.Bool(TEXT("has_lower"), bHasLower)
			 .Bool(TEXT("has_upper"), bHasUpper)
			 .Num(TEXT("start"),
				bHasLower ? static_cast<double>(Range.GetLowerBoundValue().Value) : 0.0)
			 .Num(TEXT("end"),
				bHasUpper ? static_cast<double>(Range.GetUpperBoundValue().Value) : 0.0);
		})
		.ObjectShared(TEXT("frame_rate"), SEQ_BuildFrameRateJson(MovieScene->GetTickResolution()))
		// Document which channel value-types ARE decoded (matches Phase 5 plan's "covers float/transform
		// + sentinel for others" guidance).
		.Arr(TEXT("supported_types"), MoveTemp(Types))
		.BuildSuccess(Request);
}

// ─── sequencer.get_current_time ───────────────────────────────────────────────────────────────
//
// Args:    {}
// Result:  {
//            time_seconds:  number,
//            frame:         integer,     // at display rate
//            tick:          integer,     // at tick resolution
//            frame_rate:    { numerator, denominator, decimal },  // display rate
//            tick_rate:     { numerator, denominator, decimal },  // tick resolution
//            sequence_path: string,
//            world:         "editor" | "pie"
//          }
//
// Errors:
//   -32042 NoActiveSequencer   no Sequencer tab open OR active sequence is not ULevelSequence
//   -32603 Internal            MovieScene null on a valid ULevelSequence
//
// Sources the active sequence via ``ULevelSequenceEditorBlueprintLibrary::GetCurrentLevelSequence``
// (Python-exposed; takes the FIRST opened Sequencer tab). For sub-sequences use the
// ``GetFocusedLevelSequence`` variant in a future tool — this tool intentionally returns the ROOT
// to match the simpler "what's playing now" mental model.
FMCPResponse Tool_GetCurrentTime(const FMCPRequest& Request)
{
	check(IsInGameThread());

	ULevelSequence* CurrentSeq = ULevelSequenceEditorBlueprintLibrary::GetCurrentLevelSequence();
	if (!CurrentSeq)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorNoActiveSequencer,
			TEXT("no Sequencer tab open or active sequence is not a ULevelSequence — "
				 "open a LevelSequence in the Sequencer editor and retry"));
	}

	UMovieScene* MovieScene = CurrentSeq->GetMovieScene();
	if (!MovieScene)
	{
		return FMCPToolHelpers::MakeError(Request, kSEQErrorInternal,
			TEXT("active LevelSequence has no MovieScene"));
	}

	// Pull the global playback position. EMovieSceneTimeUnit::TickResolution is what we want for
	// exact tick-accurate frame numbers; convert to display rate + seconds for client convenience.
	const FMovieSceneSequencePlaybackParams Params =
		ULevelSequenceEditorBlueprintLibrary::GetGlobalPosition(EMovieSceneTimeUnit::TickResolution);

	const FFrameRate TickRes  = MovieScene->GetTickResolution();
	const FFrameRate DisplayRate = MovieScene->GetDisplayRate();
	const FFrameTime CurrentTick = Params.Frame;
	const FFrameTime DisplayFrameTime = FFrameRate::TransformTime(CurrentTick, TickRes, DisplayRate);

	UPackage* SeqPkg = CurrentSeq->GetOutermost();
	return FMCPJsonBuilder()
		.Num(TEXT("time_seconds"), TickRes.AsSeconds(CurrentTick))
		.Num(TEXT("frame"),        DisplayFrameTime.FrameNumber.Value)
		.Num(TEXT("tick"),         CurrentTick.FrameNumber.Value)
		.ObjectShared(TEXT("frame_rate"), SEQ_BuildFrameRateJson(DisplayRate))
		.ObjectShared(TEXT("tick_rate"),  SEQ_BuildFrameRateJson(TickRes))
		.If(SeqPkg != nullptr,  [&](FMCPJsonBuilder& B) { B.Str(TEXT("sequence_path"), SeqPkg->GetName()); })
		.If(SeqPkg == nullptr,  [&](FMCPJsonBuilder& B) { B.Null(TEXT("sequence_path")); })
		.Str(TEXT("world"), FMCPWorldContext::IsPIEActive() ? TEXT("pie") : TEXT("editor"))
		.BuildSuccess(Request);
}

// ════════════════════════════════════════════════════════════════════════════════════════════════
// Wave C Tier 5a — Sequencer write surface (5 tools, all Lane A, PIE-guarded)
// ════════════════════════════════════════════════════════════════════════════════════════════════

namespace
{
	/**
	 * Compute a designer-facing display name for a freshly-spawned track. Used by add_master_track's
	 * response (returns the same name a subsequent get_tracks would emit so the caller can write
	 * follow-up tools without an extra round-trip).
	 */
	FString SEQ_GetTrackDisplayName(const UMovieSceneTrack* Track)
	{
		check(Track != nullptr);
		FString Name;
#if WITH_EDITORONLY_DATA
		Name = Track->GetDisplayName().ToString();
#endif
		if (Name.IsEmpty()) { Name = Track->GetName(); }
		if (Name.IsEmpty()) { Name = Track->GetClass()->GetName(); }
		return Name;
	}
} // namespace

// ─── sequencer.create_sequence ────────────────────────────────────────────────────────────────
//
// Args:    { dest_path: string, save?: bool }
// Result:  { created, asset_path, saved, factory_used }
//
// Errors: -32602 / -32010 / -32014 PathInUse / -32027 PIEActive / -32603 InternalError.
FMCPResponse Tool_CreateSequence(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_CreateSequence", "Create Level Sequence"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString DestPathRaw;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("dest_path"), DestPathRaw, Err)) { return Err; }

	const FString DestPathNorm = FMCPAssetPathUtils::Normalize(DestPathRaw);
	if (DestPathNorm.IsEmpty() || !FMCPAssetPathUtils::IsValidGameOrPlugin(DestPathNorm))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidPath,
			FString::Printf(TEXT("dest_path '%s' malformed or unknown mount"), *DestPathRaw));
	}
	const FString PackagePath = FPaths::GetPath(DestPathNorm);
	const FString AssetName   = FPaths::GetBaseFilename(DestPathNorm);

	// PathInUse check — cover BOTH on-disk persistence AND in-memory transient packages. Without
	// the in-memory probe a freshly-created (but unsaved) sequence reads as "not present" and a
	// double-create silently overwrites the prior UObject.
	if (FPackageName::DoesPackageExist(DestPathNorm) ||
	    FindObject<UObject>(nullptr, *(DestPathNorm + TEXT(".") + AssetName)) != nullptr)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPathInUse,
			FString::Printf(TEXT("dest_path '%s' already exists (on disk or in-memory)"), *DestPathNorm));
	}

	// CRITICAL: do NOT use IAssetTools::CreateAsset for LevelSequence — it auto-opens the Sequencer
	// editor tab on success which yields the game thread to interactive Slate state and starves the
	// MCP OnEndFrame drain. We replicate ULevelSequenceFactoryNew::FactoryCreateNew inline (which is
	// just NewObject + Initialize() + AssetRegistry::AssetCreated) — no editor pop-up, no factory
	// post-create-callback, no interactive yield.
	const FString PackageName = PackagePath + TEXT("/") + AssetName;
	UPackage* SeqPackage = CreatePackage(*PackageName);
	if (!SeqPackage)
	{
		return FMCPToolHelpers::MakeError(Request, kSEQErrorInternal,
			FString::Printf(TEXT("CreatePackage returned null for '%s'"), *PackageName));
	}
	SeqPackage->FullyLoad();

	ULevelSequence* NewAsset = NewObject<ULevelSequence>(
		SeqPackage, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
	if (!NewAsset)
	{
		return FMCPToolHelpers::MakeError(Request, kSEQErrorInternal,
			FString::Printf(TEXT("ULevelSequence NewObject returned null for %s"), *DestPathNorm));
	}
	NewAsset->Initialize();
	FAssetRegistryModule::AssetCreated(NewAsset);

	Scope.DirtyPackage(NewAsset->GetPackage());

	bool bSaveRequested = false;
	bool bSavedOk       = false;
	Request.Args->TryGetBoolField(TEXT("save"), bSaveRequested);
	if (bSaveRequested)
	{
		if (UEditorAssetSubsystem* EAS = GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr)
		{
			bSavedOk = EAS->SaveLoadedAsset(NewAsset, /*bOnlyIfIsDirty*/ true);
		}
	}

	return FMCPJsonBuilder()
		.Bool(TEXT("created"), true)
		.Str(TEXT("asset_path"), NewAsset->GetPathName())
		.Bool(TEXT("saved"), bSavedOk)
		.Str(TEXT("method"), TEXT("manual_init"))
		.BuildSuccess(Request);
}

// ─── sequencer.add_master_track ───────────────────────────────────────────────────────────────
//
// Args:    { sequence_path: string, track_class: string }
// Result:  { track_name, track_class, master_index }
//
// Adds an un-bound master track (e.g. UMovieSceneCameraCutTrack, UMovieSceneAudioTrack,
// UMovieSceneFadeTrack). For binding-scoped tracks (transform / property animation on a
// possessable) use a future sequencer.add_track_to_binding tool — out of Tier 5a scope.
//
// Errors: standard + -32011 WrongClass (track_class not a UMovieSceneTrack), -32603 InternalError.
FMCPResponse Tool_AddMasterTrack(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_AddMasterTrack", "Add Sequencer Master Track"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString SeqPath, TrackClassPath;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("sequence_path"), SeqPath, Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("track_class"),   TrackClassPath, Err)) { return Err; }

	int32 ErrCode = 0;
	FString ErrMsg;
	ULevelSequence* Seq = FMCPAssetLoader::Load<ULevelSequence>(SeqPath, ErrCode, ErrMsg);
	if (!Seq) { return FMCPToolHelpers::MakeError(Request, ErrCode, ErrMsg); }

	UMovieScene* MovieScene = Seq->GetMovieScene();
	if (!MovieScene)
	{
		return FMCPToolHelpers::MakeError(Request, kSEQErrorInternal,
			FString::Printf(TEXT("sequence '%s' has no MovieScene"), *SeqPath));
	}

	UClass* TrackClass = LoadObject<UClass>(nullptr, *TrackClassPath);
	if (!TrackClass)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorClassNotFound,
			FString::Printf(TEXT("track_class '%s' could not be loaded "
				"(e.g. /Script/MovieSceneTracks.MovieSceneCameraCutTrack)"), *TrackClassPath));
	}
	if (!TrackClass->IsChildOf(UMovieSceneTrack::StaticClass()))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorWrongClass,
			FString::Printf(TEXT("track_class '%s' is not a UMovieSceneTrack subclass"), *TrackClassPath));
	}
	if (TrackClass->HasAnyClassFlags(CLASS_Abstract))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorClassAbstract,
			FString::Printf(TEXT("track_class '%s' is abstract"), *TrackClassPath));
	}

	MovieScene->Modify();
	Seq->Modify();

	// CameraCutTrack lives in a dedicated slot — UMovieScene::AddTrack DOES insert into
	// GetTracks(), but the canonical pattern is AddCameraCutTrack which sets up the unique slot.
	UMovieSceneTrack* NewTrack = nullptr;
	if (TrackClass->IsChildOf(UMovieSceneCameraCutTrack::StaticClass()))
	{
		NewTrack = MovieScene->AddCameraCutTrack(TrackClass);
	}
	else
	{
		NewTrack = MovieScene->AddTrack(TrackClass);
	}

	if (!NewTrack)
	{
		return FMCPToolHelpers::MakeError(Request, kSEQErrorInternal,
			FString::Printf(TEXT("AddTrack returned null for class %s on sequence %s"),
				*TrackClassPath, *SeqPath));
	}

	Scope.DirtyPackage(Seq->GetPackage());

	return FMCPJsonBuilder()
		.Str(TEXT("track_name"),  SEQ_GetTrackDisplayName(NewTrack))
		.Str(TEXT("track_class"), NewTrack->GetClass()->GetPathName())
		.Num(TEXT("master_index"), MovieScene->GetTracks().Find(NewTrack))
		.BuildSuccess(Request);
}

// ─── sequencer.add_camera_cut ─────────────────────────────────────────────────────────────────
//
// Args:    { sequence_path: string,
//            start_frame: number, end_frame: number,
//            camera_actor_path?: string (optional — binds the camera into the sequence) }
// Result:  { section_index, section_class, start_frame, end_frame, camera_actor_path?, binding_guid? }
//
// Ensures CameraCutTrack exists (auto-creates if missing), then adds a new
// UMovieSceneCameraCutSection. If camera_actor_path is provided, possesses the actor into the
// sequence and binds the section to that actor's GUID.
//
// Errors: standard + -32603 if AddCameraCutTrack returns null.
FMCPResponse Tool_AddCameraCut(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_AddCameraCut", "Add Sequencer Camera Cut"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString SeqPath;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("sequence_path"), SeqPath, Err)) { return Err; }

	int32 StartFrameRaw = 0, EndFrameRaw = 0;
	if (!Request.Args->TryGetNumberField(TEXT("start_frame"), StartFrameRaw) ||
	    !Request.Args->TryGetNumberField(TEXT("end_frame"),   EndFrameRaw))
	{
		return FMCPToolHelpers::MakeError(Request, kSEQErrorInvalidParams,
			TEXT("sequencer.add_camera_cut requires args.start_frame + args.end_frame (integer ticks)"));
	}
	if (EndFrameRaw <= StartFrameRaw)
	{
		return FMCPToolHelpers::MakeError(Request, kSEQErrorInvalidParams,
			FString::Printf(TEXT("end_frame (%d) must be greater than start_frame (%d)"),
				EndFrameRaw, StartFrameRaw));
	}

	FString CameraActorPath;
	Request.Args->TryGetStringField(TEXT("camera_actor_path"), CameraActorPath);

	int32 ErrCode = 0;
	FString ErrMsg;
	ULevelSequence* Seq = FMCPAssetLoader::Load<ULevelSequence>(SeqPath, ErrCode, ErrMsg);
	if (!Seq) { return FMCPToolHelpers::MakeError(Request, ErrCode, ErrMsg); }
	UMovieScene* MovieScene = Seq->GetMovieScene();
	if (!MovieScene)
	{
		return FMCPToolHelpers::MakeError(Request, kSEQErrorInternal,
			FString::Printf(TEXT("sequence '%s' has no MovieScene"), *SeqPath));
	}

	MovieScene->Modify();
	Seq->Modify();

	UMovieSceneCameraCutTrack* CCT = Cast<UMovieSceneCameraCutTrack>(MovieScene->GetCameraCutTrack());
	if (!CCT)
	{
		CCT = Cast<UMovieSceneCameraCutTrack>(
			MovieScene->AddCameraCutTrack(UMovieSceneCameraCutTrack::StaticClass()));
	}
	if (!CCT)
	{
		return FMCPToolHelpers::MakeError(Request, kSEQErrorInternal,
			TEXT("could not obtain or create the CameraCutTrack on this sequence"));
	}

	// Optional binding — resolve camera actor and possess it into the sequence if needed.
	FGuid CameraGuid;
	if (!CameraActorPath.IsEmpty())
	{
		bool bAmbiguous = false;
		FString AmbiguityHint, ActorErr;
		AActor* CameraActor = FMCPActorPathUtils::ResolveActor(CameraActorPath, /*bRejectPIE*/ true,
			bAmbiguous, AmbiguityHint, ActorErr);
		if (!CameraActor)
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
				FString::Printf(TEXT("camera_actor '%s' not found: %s"), *CameraActorPath, *ActorErr));
		}
		// Cast<UCameraComponent> isn't required for sequencer binding — any AActor with a transform
		// can be a "camera". The Sequencer editor enforces camera-component presence interactively;
		// we honour the user's intent literally.

		// Possess by adding a new possessable bound to the actor in the persistent level.
		CameraGuid = MovieScene->AddPossessable(CameraActor->GetActorLabel(), CameraActor->GetClass());
		Seq->BindPossessableObject(CameraGuid, *CameraActor, CameraActor->GetWorld());
	}

	UMovieSceneCameraCutSection* Section = NewObject<UMovieSceneCameraCutSection>(
		CCT, NAME_None, RF_Transactional);
	Section->SetRange(TRange<FFrameNumber>(FFrameNumber(StartFrameRaw), FFrameNumber(EndFrameRaw)));
	if (CameraGuid.IsValid())
	{
		Section->SetCameraGuid(CameraGuid);
	}
	CCT->AddSection(*Section);

	Scope.DirtyPackage(Seq->GetPackage());

	const int32 SectionIdx = CCT->GetAllSections().Find(Section);

	return FMCPJsonBuilder()
		.Num(TEXT("section_index"), SectionIdx)
		.Str(TEXT("section_class"), Section->GetClass()->GetPathName())
		.Num(TEXT("start_frame"), StartFrameRaw)
		.Num(TEXT("end_frame"),   EndFrameRaw)
		.If(CameraGuid.IsValid(), [&](FMCPJsonBuilder& B)
		{
			B.Str(TEXT("camera_actor_path"), CameraActorPath)
			 .Str(TEXT("binding_guid"), CameraGuid.ToString(EGuidFormats::Digits));
		})
		.BuildSuccess(Request);
}

// ─── sequencer.add_keyframe ───────────────────────────────────────────────────────────────────
//
// Args:    { sequence_path: string,
//            track_path:   string  (dotted form per get_keyframes — see SEQ_ResolveTrackSection),
//            channel_index: number (which channel inside the section, ChannelProxy order),
//            time_frame:   number  (FFrameNumber tick — at MovieScene tick resolution),
//            value:        <typed JSON> (number for float/double/int, bool for bool channels) }
// Result:  { added, channel_type, total_keys_after }
//
// Errors: standard + -32032 PinTypeUnsupported (channel type not in float/double/bool/int).
FMCPResponse Tool_AddKeyframe(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_AddKeyframe", "Add Sequencer Keyframe"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString SeqPath, TrackPath;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("sequence_path"), SeqPath, Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("track_path"),    TrackPath, Err)) { return Err; }

	int32 ChannelIndex = -1;
	if (!Request.Args->TryGetNumberField(TEXT("channel_index"), ChannelIndex) || ChannelIndex < 0)
	{
		return FMCPToolHelpers::MakeError(Request, kSEQErrorInvalidParams,
			TEXT("sequencer.add_keyframe requires args.channel_index (non-negative integer)"));
	}

	int32 TimeFrame = 0;
	if (!Request.Args->TryGetNumberField(TEXT("time_frame"), TimeFrame))
	{
		return FMCPToolHelpers::MakeError(Request, kSEQErrorInvalidParams,
			TEXT("sequencer.add_keyframe requires args.time_frame (integer tick number)"));
	}

	if (!Request.Args.IsValid() || !Request.Args->HasField(TEXT("value")))
	{
		return FMCPToolHelpers::MakeError(Request, kSEQErrorInvalidParams,
			TEXT("sequencer.add_keyframe requires args.value (number for float/double/int channels, bool for bool channels)"));
	}
	const TSharedPtr<FJsonValue> ValueJson = Request.Args->TryGetField(TEXT("value"));

	int32 ErrCode = 0;
	FString ErrMsg;
	ULevelSequence* Seq = FMCPAssetLoader::Load<ULevelSequence>(SeqPath, ErrCode, ErrMsg);
	if (!Seq) { return FMCPToolHelpers::MakeError(Request, ErrCode, ErrMsg); }
	UMovieScene* MovieScene = Seq->GetMovieScene();
	if (!MovieScene) { return FMCPToolHelpers::MakeError(Request, kSEQErrorInternal, TEXT("no MovieScene")); }

	UMovieSceneSection* Section = SEQ_ResolveTrackSection(MovieScene, TrackPath, ErrCode, ErrMsg);
	if (!Section) { return FMCPToolHelpers::MakeError(Request, ErrCode, ErrMsg); }

	MovieScene->Modify();
	Section->Modify();

	FMovieSceneChannelProxy& Proxy = Section->GetChannelProxy();
	const FFrameNumber FrameTime(TimeFrame);

	// Try each channel-type bucket — adding a key has the same shape across channel types but the
	// AddCubicKey / AddConstantKey variants differ per channel struct, so we dispatch by index.
	FString ChannelType;
	int32 TotalKeysAfter = -1;

	// Float channels.
	{
		TMovieSceneChannelHandle<FMovieSceneFloatChannel> Handle = Proxy.MakeHandle<FMovieSceneFloatChannel>(ChannelIndex);
		if (FMovieSceneFloatChannel* Channel = Handle.Get())
		{
			if (ValueJson->Type != EJson::Number)
			{
				return FMCPToolHelpers::MakeError(Request, kSEQErrorInvalidParams,
					TEXT("channel is float — value must be a JSON number"));
			}
			Channel->AddCubicKey(FrameTime, static_cast<float>(ValueJson->AsNumber()));
			ChannelType = TEXT("float");
			TotalKeysAfter = Channel->GetData().GetTimes().Num();
		}
	}
	// Double channels.
	if (TotalKeysAfter < 0)
	{
		TMovieSceneChannelHandle<FMovieSceneDoubleChannel> Handle = Proxy.MakeHandle<FMovieSceneDoubleChannel>(ChannelIndex);
		if (FMovieSceneDoubleChannel* Channel = Handle.Get())
		{
			if (ValueJson->Type != EJson::Number)
			{
				return FMCPToolHelpers::MakeError(Request, kSEQErrorInvalidParams,
					TEXT("channel is double — value must be a JSON number"));
			}
			Channel->AddCubicKey(FrameTime, ValueJson->AsNumber());
			ChannelType = TEXT("double");
			TotalKeysAfter = Channel->GetData().GetTimes().Num();
		}
	}
	// Bool channels.
	if (TotalKeysAfter < 0)
	{
		TMovieSceneChannelHandle<FMovieSceneBoolChannel> Handle = Proxy.MakeHandle<FMovieSceneBoolChannel>(ChannelIndex);
		if (FMovieSceneBoolChannel* Channel = Handle.Get())
		{
			bool bValue = false;
			if (ValueJson->Type == EJson::Boolean)     { bValue = ValueJson->AsBool(); }
			else if (ValueJson->Type == EJson::Number) { bValue = (ValueJson->AsNumber() != 0.0); }
			else
			{
				return FMCPToolHelpers::MakeError(Request, kSEQErrorInvalidParams,
					TEXT("channel is bool — value must be JSON bool or number"));
			}
			Channel->GetData().UpdateOrAddKey(FrameTime, bValue);
			ChannelType = TEXT("bool");
			TotalKeysAfter = Channel->GetData().GetTimes().Num();
		}
	}
	// Integer channels.
	if (TotalKeysAfter < 0)
	{
		TMovieSceneChannelHandle<FMovieSceneIntegerChannel> Handle = Proxy.MakeHandle<FMovieSceneIntegerChannel>(ChannelIndex);
		if (FMovieSceneIntegerChannel* Channel = Handle.Get())
		{
			if (ValueJson->Type != EJson::Number)
			{
				return FMCPToolHelpers::MakeError(Request, kSEQErrorInvalidParams,
					TEXT("channel is integer — value must be a JSON number"));
			}
			Channel->GetData().UpdateOrAddKey(FrameTime, static_cast<int32>(ValueJson->AsNumber()));
			ChannelType = TEXT("integer");
			TotalKeysAfter = Channel->GetData().GetTimes().Num();
		}
	}

	if (TotalKeysAfter < 0)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPinTypeUnsupported,
			FString::Printf(
				TEXT("channel %d on track section '%s' is not float/double/bool/integer "
					 "(other channel types not yet supported by sequencer.add_keyframe)"),
				ChannelIndex, *TrackPath));
	}

	Scope.DirtyPackage(Seq->GetPackage());

	return FMCPJsonBuilder()
		.Bool(TEXT("added"), true)
		.Str(TEXT("channel_type"), ChannelType)
		.Num(TEXT("total_keys_after"), TotalKeysAfter)
		.BuildSuccess(Request);
}

// ─── sequencer.set_section_range ──────────────────────────────────────────────────────────────
//
// Args:    { sequence_path: string, track_path: string,
//            start_frame: number, end_frame: number }
// Result:  { prior_range: [s, e], new_range: [s, e] }
//
// Errors: standard.
FMCPResponse Tool_SetSectionRange(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_SetSectionRange", "Set Sequencer Section Range"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString SeqPath, TrackPath;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("sequence_path"), SeqPath, Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("track_path"),    TrackPath, Err)) { return Err; }

	int32 StartFrame = 0, EndFrame = 0;
	if (!Request.Args->TryGetNumberField(TEXT("start_frame"), StartFrame) ||
	    !Request.Args->TryGetNumberField(TEXT("end_frame"),   EndFrame))
	{
		return FMCPToolHelpers::MakeError(Request, kSEQErrorInvalidParams,
			TEXT("sequencer.set_section_range requires args.start_frame + args.end_frame (integer ticks)"));
	}
	if (EndFrame <= StartFrame)
	{
		return FMCPToolHelpers::MakeError(Request, kSEQErrorInvalidParams,
			FString::Printf(TEXT("end_frame (%d) must be greater than start_frame (%d)"), EndFrame, StartFrame));
	}

	int32 ErrCode = 0;
	FString ErrMsg;
	ULevelSequence* Seq = FMCPAssetLoader::Load<ULevelSequence>(SeqPath, ErrCode, ErrMsg);
	if (!Seq) { return FMCPToolHelpers::MakeError(Request, ErrCode, ErrMsg); }
	UMovieScene* MovieScene = Seq->GetMovieScene();
	if (!MovieScene) { return FMCPToolHelpers::MakeError(Request, kSEQErrorInternal, TEXT("no MovieScene")); }

	UMovieSceneSection* Section = SEQ_ResolveTrackSection(MovieScene, TrackPath, ErrCode, ErrMsg);
	if (!Section) { return FMCPToolHelpers::MakeError(Request, ErrCode, ErrMsg); }

	Section->Modify();

	const TRange<FFrameNumber> Prior = Section->GetRange();
	Section->SetRange(TRange<FFrameNumber>(FFrameNumber(StartFrame), FFrameNumber(EndFrame)));

	Scope.DirtyPackage(Seq->GetPackage());

	auto RangeToJson = [](const TRange<FFrameNumber>& R) -> TSharedRef<FJsonValueArray>
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Add(MakeShared<FJsonValueNumber>(
			R.HasLowerBound() ? R.GetLowerBoundValue().Value : 0));
		Arr.Add(MakeShared<FJsonValueNumber>(
			R.HasUpperBound() ? R.GetUpperBoundValue().Value : 0));
		return MakeShared<FJsonValueArray>(Arr);
	};

	return FMCPJsonBuilder()
		.Field(TEXT("prior_range"), RangeToJson(Prior))
		.Field(TEXT("new_range"),
			RangeToJson(TRange<FFrameNumber>(FFrameNumber(StartFrame), FFrameNumber(EndFrame))))
		.BuildSuccess(Request);
}

// ─── Registration ──────────────────────────────────────────────────────────────────────────────
void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	RegisterTool(TEXT("sequencer.list_cinematics"),  &Tool_ListCinematics,  /*Lane A*/ false);
	RegisterTool(TEXT("sequencer.get_tracks"),       &Tool_GetTracks,       /*Lane A*/ false);
	RegisterTool(TEXT("sequencer.get_camera_cuts"),  &Tool_GetCameraCuts,   /*Lane A*/ false);
	RegisterTool(TEXT("sequencer.get_keyframes"),    &Tool_GetKeyframes,    /*Lane A*/ false);
	RegisterTool(TEXT("sequencer.get_current_time"), &Tool_GetCurrentTime,  /*Lane A*/ false);

	// Wave C Tier 5a (2026-05): Sequencer write surface.
	RegisterTool(TEXT("sequencer.create_sequence"),   &Tool_CreateSequence,   /*Lane A*/ false);
	RegisterTool(TEXT("sequencer.add_master_track"),  &Tool_AddMasterTrack,   /*Lane A*/ false);
	RegisterTool(TEXT("sequencer.add_camera_cut"),    &Tool_AddCameraCut,     /*Lane A*/ false);
	RegisterTool(TEXT("sequencer.add_keyframe"),      &Tool_AddKeyframe,      /*Lane A*/ false);
	RegisterTool(TEXT("sequencer.set_section_range"), &Tool_SetSectionRange,  /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("Sequencer surface registered: 5 reads + 5 writes "
			 "(list_cinematics/get_tracks/get_camera_cuts/get_keyframes/get_current_time + "
			 "create_sequence/add_master_track/add_camera_cut/add_keyframe/set_section_range), all Lane A"));
}

} // namespace FSequencerTools

#undef LOCTEXT_NAMESPACE

MCP_REGISTER_SURFACE(SequencerTools, &FSequencerTools::Register)
