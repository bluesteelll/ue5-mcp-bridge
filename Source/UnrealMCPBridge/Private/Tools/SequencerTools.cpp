// Copyright FatumGame. All Rights Reserved.

#include "SequencerTools.h"

#include "FMCPDispatchQueue.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPAssetPathUtils.h"
#include "Utils/MCPWorldContext.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
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
	// SEQ_ prefix per the unity-build symbol-collision pattern (MakeError/MakeSuccess clash with
	// UE's global ValueOrError templates AND between sibling tool TUs in the unity build).
	constexpr int32 kSEQErrorInvalidParams = -32602;
	constexpr int32 kSEQErrorInternal      = -32603;

	void SEQ_StampIds(const FMCPRequest& Request, FMCPResponse& Response)
	{
		Response.RequestId = Request.RequestId;
		Response.OriginalIdString = Request.OriginalIdString;
	}

	FMCPResponse SEQ_MakeError(const FMCPRequest& Request, int32 Code, const FString& Message)
	{
		FMCPResponse R;
		SEQ_StampIds(Request, R);
		R.bIsError = true;
		R.ErrorCode = Code;
		R.ErrorMessage = Message;
		return R;
	}

	FMCPResponse SEQ_MakeSuccessObj(const FMCPRequest& Request, TSharedPtr<FJsonObject> Result)
	{
		FMCPResponse R;
		SEQ_StampIds(Request, R);
		R.bIsError = false;
		R.Result = MakeShared<FJsonValueObject>(MoveTemp(Result));
		return R;
	}

	bool SEQ_RequireStringField(const FMCPRequest& Request, const TCHAR* FieldName,
		FString& OutValue, FMCPResponse& OutError)
	{
		if (!Request.Args.IsValid())
		{
			OutError = SEQ_MakeError(Request, kSEQErrorInvalidParams, TEXT("missing args object"));
			return false;
		}
		if (!Request.Args->TryGetStringField(FieldName, OutValue) || OutValue.IsEmpty())
		{
			OutError = SEQ_MakeError(Request, kSEQErrorInvalidParams,
				FString::Printf(TEXT("missing required string field '%s'"), FieldName));
			return false;
		}
		return true;
	}

	// ─── LevelSequence resolution ────────────────────────────────────────────────────────────────

	/**
	 * Load a ULevelSequence by path. Mirror of FNiagaraTools::NIA_LoadNiagaraSystemByPath /
	 * FUMGTools::UMG_LoadWidgetBlueprintByPath but for level sequences.
	 *
	 * Error map:
	 *   -32010 InvalidPath          — empty path, backslashes, unknown mount
	 *   -32004 ObjectNotFound       — LoadObject returned null
	 *   -32011 WrongClass           — loaded asset isn't a ULevelSequence
	 */
	ULevelSequence* SEQ_LoadLevelSequenceByPath(
		const FString& Path,
		int32& OutErrorCode,
		FString& OutError)
	{
		if (Path.IsEmpty())
		{
			OutErrorCode = kMCPErrorInvalidPath;
			OutError = TEXT("sequence_path is empty");
			return nullptr;
		}

		const FString Normalised = FMCPAssetPathUtils::Normalize(Path);
		if (Normalised.IsEmpty() || !FMCPAssetPathUtils::IsValidGameOrPlugin(Normalised))
		{
			OutErrorCode = kMCPErrorInvalidPath;
			OutError = FString::Printf(
				TEXT("sequence_path '%s' is malformed or references an unknown mount point"),
				*Path);
			return nullptr;
		}

		UObject* Loaded = LoadObject<UObject>(nullptr, *Normalised);
		if (!Loaded)
		{
			const FString ObjectPath = FMCPAssetPathUtils::ToObjectPath(Normalised);
			if (!ObjectPath.IsEmpty() && ObjectPath != Normalised)
			{
				Loaded = LoadObject<UObject>(nullptr, *ObjectPath);
			}
		}
		if (!Loaded)
		{
			OutErrorCode = kMCPErrorObjectNotFound;
			OutError = FString::Printf(
				TEXT("sequence_path '%s' could not be loaded (no asset found)"),
				*Path);
			return nullptr;
		}

		ULevelSequence* Seq = Cast<ULevelSequence>(Loaded);
		if (!Seq)
		{
			OutErrorCode = kMCPErrorWrongClass;
			OutError = FString::Printf(
				TEXT("sequence_path '%s' is class '%s'; expected ULevelSequence"),
				*Path, *Loaded->GetClass()->GetPathName());
			return nullptr;
		}
		return Seq;
	}

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
					return SEQ_MakeError(Request, kSEQErrorInvalidParams,
						TEXT("scope_paths: expected array of strings"));
				}
				const FString Norm = FMCPAssetPathUtils::Normalize(S);
				if (Norm.IsEmpty())
				{
					return SEQ_MakeError(Request, kMCPErrorInvalidPath,
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

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetArrayField(TEXT("sequences"), SeqArr);
	return SEQ_MakeSuccessObj(Request, Out);
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
	if (!SEQ_RequireStringField(Request, TEXT("sequence_path"), Path, Err)) { return Err; }

	int32 ErrCode = 0;
	FString ErrMsg;
	ULevelSequence* Seq = SEQ_LoadLevelSequenceByPath(Path, ErrCode, ErrMsg);
	if (!Seq) { return SEQ_MakeError(Request, ErrCode, ErrMsg); }

	UMovieScene* MovieScene = Seq->GetMovieScene();
	if (!MovieScene)
	{
		return SEQ_MakeError(Request, kSEQErrorInternal,
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

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetArrayField(TEXT("master_tracks"), MasterArr);
	Out->SetArrayField(TEXT("possessables"),  PossArr);
	Out->SetArrayField(TEXT("spawnables"),    SpawnArr);
	return SEQ_MakeSuccessObj(Request, Out);
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
	if (!SEQ_RequireStringField(Request, TEXT("sequence_path"), Path, Err)) { return Err; }

	int32 ErrCode = 0;
	FString ErrMsg;
	ULevelSequence* Seq = SEQ_LoadLevelSequenceByPath(Path, ErrCode, ErrMsg);
	if (!Seq) { return SEQ_MakeError(Request, ErrCode, ErrMsg); }

	UMovieScene* MovieScene = Seq->GetMovieScene();
	if (!MovieScene)
	{
		return SEQ_MakeError(Request, kSEQErrorInternal,
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

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetArrayField(TEXT("cuts"), CutsArr);
	Out->SetObjectField(TEXT("frame_rate"), SEQ_BuildFrameRateJson(MovieScene->GetTickResolution()));
	return SEQ_MakeSuccessObj(Request, Out);
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
	if (!SEQ_RequireStringField(Request, TEXT("sequence_path"), SeqPath, Err)) { return Err; }

	FString TrackPath;
	if (!SEQ_RequireStringField(Request, TEXT("track_path"), TrackPath, Err)) { return Err; }

	int32 ErrCode = 0;
	FString ErrMsg;
	ULevelSequence* Seq = SEQ_LoadLevelSequenceByPath(SeqPath, ErrCode, ErrMsg);
	if (!Seq) { return SEQ_MakeError(Request, ErrCode, ErrMsg); }

	UMovieScene* MovieScene = Seq->GetMovieScene();
	if (!MovieScene)
	{
		return SEQ_MakeError(Request, kSEQErrorInternal,
			FString::Printf(TEXT("sequence_path '%s' has no MovieScene"), *SeqPath));
	}

	int32 ResolveErrCode = 0;
	FString ResolveErrMsg;
	UMovieSceneSection* Section = SEQ_ResolveTrackSection(MovieScene, TrackPath, ResolveErrCode, ResolveErrMsg);
	if (!Section) { return SEQ_MakeError(Request, ResolveErrCode, ResolveErrMsg); }

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
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetArrayField(TEXT("keys"), KeyArr);

	// Section range echo (for caller convenience — saves a round-trip to get_tracks).
	{
		TSharedRef<FJsonObject> RangeObj = MakeShared<FJsonObject>();
		const TRange<FFrameNumber> Range = Section->GetRange();
		const bool bHasLower = Range.HasLowerBound();
		const bool bHasUpper = Range.HasUpperBound();
		RangeObj->SetBoolField(TEXT("has_lower"), bHasLower);
		RangeObj->SetBoolField(TEXT("has_upper"), bHasUpper);
		RangeObj->SetNumberField(TEXT("start"),
			bHasLower ? static_cast<double>(Range.GetLowerBoundValue().Value) : 0.0);
		RangeObj->SetNumberField(TEXT("end"),
			bHasUpper ? static_cast<double>(Range.GetUpperBoundValue().Value) : 0.0);
		Out->SetObjectField(TEXT("section_range"), RangeObj);
	}

	Out->SetObjectField(TEXT("frame_rate"), SEQ_BuildFrameRateJson(MovieScene->GetTickResolution()));

	// Document which channel value-types ARE decoded (matches Phase 5 plan's "covers float/transform
	// + sentinel for others" guidance).
	{
		TArray<TSharedPtr<FJsonValue>> Types;
		Types.Add(MakeShared<FJsonValueString>(TEXT("float")));
		Types.Add(MakeShared<FJsonValueString>(TEXT("double")));
		Types.Add(MakeShared<FJsonValueString>(TEXT("bool")));
		Types.Add(MakeShared<FJsonValueString>(TEXT("integer")));
		Out->SetArrayField(TEXT("supported_types"), Types);
	}

	return SEQ_MakeSuccessObj(Request, Out);
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
		return SEQ_MakeError(Request, kMCPErrorNoActiveSequencer,
			TEXT("no Sequencer tab open or active sequence is not a ULevelSequence — "
				 "open a LevelSequence in the Sequencer editor and retry"));
	}

	UMovieScene* MovieScene = CurrentSeq->GetMovieScene();
	if (!MovieScene)
	{
		return SEQ_MakeError(Request, kSEQErrorInternal,
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

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetNumberField(TEXT("time_seconds"), TickRes.AsSeconds(CurrentTick));
	Out->SetNumberField(TEXT("frame"),        DisplayFrameTime.FrameNumber.Value);
	Out->SetNumberField(TEXT("tick"),         CurrentTick.FrameNumber.Value);
	Out->SetObjectField(TEXT("frame_rate"), SEQ_BuildFrameRateJson(DisplayRate));
	Out->SetObjectField(TEXT("tick_rate"),  SEQ_BuildFrameRateJson(TickRes));

	if (UPackage* SeqPkg = CurrentSeq->GetOutermost())
	{
		Out->SetStringField(TEXT("sequence_path"), SeqPkg->GetName());
	}
	else
	{
		Out->SetField(TEXT("sequence_path"), MakeShared<FJsonValueNull>());
	}

	Out->SetStringField(TEXT("world"), FMCPWorldContext::IsPIEActive() ? TEXT("pie") : TEXT("editor"));
	return SEQ_MakeSuccessObj(Request, Out);
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

	UE_LOG(LogMCP, Log,
		TEXT("Phase 5 Chunk D (Sequencer): registered 5 sequencer.* handlers "
			 "(list_cinematics + get_tracks + get_camera_cuts + get_keyframes + get_current_time, all Lane A)"));
}

} // namespace FSequencerTools

#undef LOCTEXT_NAMESPACE
