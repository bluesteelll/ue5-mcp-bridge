// Copyright FatumGame. All Rights Reserved.

#include "SequencerExtTools.h"

#include "FMCPDispatchQueue.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPActorPathUtils.h"
#include "Utils/MCPAssetPathUtils.h"
#include "Utils/MCPWorldContext.h"

#include "GameFramework/Actor.h"
#include "LevelSequence.h"
#include "Misc/FrameNumber.h"
#include "Misc/Guid.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieSceneSection.h"
#include "MovieSceneTrack.h"
#include "ScopedTransaction.h"
#include "Sections/MovieScene3DTransformSection.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Sections/MovieSceneVisibilitySection.h"
#include "Tracks/MovieScene3DTransformTrack.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "Tracks/MovieSceneVisibilityTrack.h"
#include "UObject/Class.h"
#include "UObject/Package.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// SEQX_ prefix per the unity-build symbol-collision convention. Sibling SequencerTools.cpp
	// uses SEQ_ — separate prefixes prevent ODR clashes when both TUs land in the same unity blob.
	constexpr int32 kSEQXErrorInvalidParams = -32602;
	constexpr int32 kSEQXErrorInternal      = -32603;

	void SEQX_StampIds(const FMCPRequest& Request, FMCPResponse& Response)
	{
		Response.RequestId = Request.RequestId;
		Response.OriginalIdString = Request.OriginalIdString;
	}

	FMCPResponse SEQX_MakeError(const FMCPRequest& Request, int32 Code, const FString& Message)
	{
		FMCPResponse R;
		SEQX_StampIds(Request, R);
		R.bIsError = true;
		R.ErrorCode = Code;
		R.ErrorMessage = Message;
		return R;
	}

	FMCPResponse SEQX_MakeSuccessObj(const FMCPRequest& Request, TSharedPtr<FJsonObject> Result)
	{
		FMCPResponse R;
		SEQX_StampIds(Request, R);
		R.bIsError = false;
		R.Result = MakeShared<FJsonValueObject>(MoveTemp(Result));
		return R;
	}

	bool SEQX_RequireStringField(const FMCPRequest& Request, const TCHAR* FieldName,
		FString& OutValue, FMCPResponse& OutError)
	{
		if (!Request.Args.IsValid())
		{
			OutError = SEQX_MakeError(Request, kSEQXErrorInvalidParams, TEXT("missing args object"));
			return false;
		}
		if (!Request.Args->TryGetStringField(FieldName, OutValue) || OutValue.IsEmpty())
		{
			OutError = SEQX_MakeError(Request, kSEQXErrorInvalidParams,
				FString::Printf(TEXT("missing required string field '%s'"), FieldName));
			return false;
		}
		return true;
	}

	/**
	 * Load a ULevelSequence by path. Mirrors SequencerTools.cpp's SEQ_LoadLevelSequenceByPath
	 * verbatim — duplicated rather than promoted to a header because the helper is a 50-line
	 * boilerplate and adding cross-TU coupling for a single internal helper would create a
	 * worse maintenance surface than this minor copy. If a third Sequencer TU lands we'll
	 * extract to a shared utility module.
	 *
	 * Error map:
	 *   -32010 InvalidPath          — empty path, backslashes, unknown mount
	 *   -32004 ObjectNotFound       — LoadObject returned null
	 *   -32011 WrongClass           — loaded asset isn't a ULevelSequence
	 */
	ULevelSequence* SEQX_LoadLevelSequenceByPath(
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

	/**
	 * Resolve a designer-facing track-class string to a concrete UMovieSceneTrack subclass.
	 *
	 * Accepted strings (case-sensitive — matches the editor's track-add menu convention):
	 *   "Transform" / "3DTransform" → UMovieScene3DTransformTrack
	 *   "Visibility"                → UMovieSceneVisibilityTrack
	 *   "Float"                     → UMovieSceneFloatTrack
	 *
	 * Returns nullptr on unknown strings — caller surfaces -32602 InvalidParams with a hint
	 * listing the accepted set.
	 */
	UClass* SEQX_ResolveTrackClass(const FString& TrackClassName)
	{
		if (TrackClassName.Equals(TEXT("Transform"), ESearchCase::CaseSensitive) ||
		    TrackClassName.Equals(TEXT("3DTransform"), ESearchCase::CaseSensitive))
		{
			return UMovieScene3DTransformTrack::StaticClass();
		}
		if (TrackClassName.Equals(TEXT("Visibility"), ESearchCase::CaseSensitive))
		{
			return UMovieSceneVisibilityTrack::StaticClass();
		}
		if (TrackClassName.Equals(TEXT("Float"), ESearchCase::CaseSensitive))
		{
			return UMovieSceneFloatTrack::StaticClass();
		}
		return nullptr;
	}

	/**
	 * Parse a binding-guid string into an FGuid. Accepts the canonical
	 * EGuidFormats::DigitsWithHyphens form emitted by every other Sequencer tool
	 * (matches what ``sequencer.get_tracks`` / ``sequencer_ext.add_possessable`` return).
	 *
	 * FGuid::Parse already handles both hyphenated and digit-only variants — returns true on
	 * success. Empty string is treated as invalid (we don't accept a "default" GUID).
	 */
	bool SEQX_ParseBindingGuid(const FString& GuidStr, FGuid& OutGuid)
	{
		if (GuidStr.IsEmpty()) { return false; }
		return FGuid::Parse(GuidStr, OutGuid) && OutGuid.IsValid();
	}

	/**
	 * Verify a binding guid exists on the MovieScene either as a possessable OR a spawnable.
	 *
	 * FindBinding returning non-null is NOT sufficient — the binding may have ZERO tracks (which
	 * is the legitimate state right after AddPossessable). Both possessable list AND spawnable list
	 * are checked because both expose tracks indirectly via FindBinding.
	 *
	 * Returns true if the guid is registered as a possessable or spawnable.
	 */
	bool SEQX_BindingExists(UMovieScene* MovieScene, const FGuid& BindingGuid)
	{
		check(MovieScene != nullptr);

		const int32 PossCount = MovieScene->GetPossessableCount();
		for (int32 i = 0; i < PossCount; ++i)
		{
			if (MovieScene->GetPossessable(i).GetGuid() == BindingGuid)
			{
				return true;
			}
		}
		const int32 SpawnCount = MovieScene->GetSpawnableCount();
		for (int32 i = 0; i < SpawnCount; ++i)
		{
			if (MovieScene->GetSpawnable(i).GetGuid() == BindingGuid)
			{
				return true;
			}
		}
		return false;
	}
} // namespace

namespace FSequencerExtTools
{

// ─── sequencer_ext.add_possessable ────────────────────────────────────────────────────────────
//
// Args:    { sequence_path: string, actor_path: string, label?: string }
// Result:  { possessable_guid: string,
//            actor_path: string,           // canonical path resolved via MCPActorPathUtils
//            label: string,                // label used (caller's override OR actor's display label)
//            object_class: string }        // class path of the bound actor (for confirmation)
//
// Errors:
//   -32602 InvalidParams        missing sequence_path / actor_path
//   -32010 InvalidPath          path malformed / unknown mount
//   -32004 ObjectNotFound       LoadObject returned null OR actor not found
//   -32011 WrongClass           asset is not ULevelSequence
//   -32027 PIEActive            mutator refused during PIE
//   -32603 InternalError        MovieScene null / AddPossessable returned invalid guid
//
// Adds an actor as a new possessable to a ULevelSequence and binds it via
// ``ULevelSequence::BindPossessableObject`` — the canonical Sequencer-editor "add actor" path.
// The returned ``possessable_guid`` is the same guid format ``sequencer.get_tracks`` reports
// in its ``possessables[].binding_guid`` field — round-trip compatible.
//
// Actor resolution uses ``FMCPActorPathUtils::ResolveActor`` with ``bRejectPIE=true`` to match the
// editor-world-only mutator contract (PIE actors live in a different world and binding them to
// an editor-world ULevelSequence would silently fail at evaluation time). For bare-name
// inputs the helper returns ambiguity hints in the error message.
FMCPResponse Tool_AddPossessable(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (FMCPWorldContext::IsPIEActive())
	{
		return SEQX_MakeError(Request, kMCPErrorPIEActive, kMCPMessagePIEActive);
	}

	FString SeqPath, ActorPathRaw;
	FMCPResponse Err;
	if (!SEQX_RequireStringField(Request, TEXT("sequence_path"), SeqPath, Err)) { return Err; }
	if (!SEQX_RequireStringField(Request, TEXT("actor_path"),    ActorPathRaw, Err)) { return Err; }

	// Optional label — defaults to actor's display label if omitted.
	FString LabelOverride;
	Request.Args->TryGetStringField(TEXT("label"), LabelOverride);

	int32 ErrCode = 0;
	FString ErrMsg;
	ULevelSequence* Seq = SEQX_LoadLevelSequenceByPath(SeqPath, ErrCode, ErrMsg);
	if (!Seq) { return SEQX_MakeError(Request, ErrCode, ErrMsg); }

	UMovieScene* MovieScene = Seq->GetMovieScene();
	if (!MovieScene)
	{
		return SEQX_MakeError(Request, kSEQXErrorInternal,
			FString::Printf(TEXT("sequence '%s' has no MovieScene"), *SeqPath));
	}

	// Resolve the actor against the editor world. bRejectPIE=true mirrors Wave C
	// sequencer.add_camera_cut convention — editor-world possessables only.
	bool bAmbiguous = false;
	FString AmbiguityHint, ActorErr;
	AActor* Actor = FMCPActorPathUtils::ResolveActor(
		ActorPathRaw, /*bRejectPIE*/ true, bAmbiguous, AmbiguityHint, ActorErr);
	if (!Actor)
	{
		const FString FullMsg = bAmbiguous
			? FString::Printf(TEXT("actor_path '%s' is ambiguous; candidates: %s"),
				*ActorPathRaw, *AmbiguityHint)
			: FString::Printf(TEXT("actor_path '%s' not found: %s"),
				*ActorPathRaw, *ActorErr);
		return SEQX_MakeError(Request, kMCPErrorObjectNotFound, FullMsg);
	}

	const FString FinalLabel = LabelOverride.IsEmpty() ? Actor->GetActorLabel() : LabelOverride;

	FScopedTransaction Transaction(LOCTEXT("MCP_AddPossessable", "Add Sequencer Possessable"));
	MovieScene->Modify();
	Seq->Modify();

	const FGuid PossessableGuid = MovieScene->AddPossessable(FinalLabel, Actor->GetClass());
	if (!PossessableGuid.IsValid())
	{
		return SEQX_MakeError(Request, kSEQXErrorInternal,
			FString::Printf(TEXT("UMovieScene::AddPossessable returned invalid guid for actor '%s' on sequence '%s'"),
				*FinalLabel, *SeqPath));
	}

	// BindPossessableObject wires the guid → live actor mapping. World context comes from
	// the actor itself (editor world or sublevel containing it).
	Seq->BindPossessableObject(PossessableGuid, *Actor, Actor->GetWorld());

	if (UPackage* Pkg = Seq->GetOutermost()) { Pkg->MarkPackageDirty(); }

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("possessable_guid"),
		PossessableGuid.ToString(EGuidFormats::DigitsWithHyphens));
	Out->SetStringField(TEXT("actor_path"), FMCPActorPathUtils::BuildActorPath(Actor));
	Out->SetStringField(TEXT("label"),        FinalLabel);
	Out->SetStringField(TEXT("object_class"), Actor->GetClass()->GetPathName());
	return SEQX_MakeSuccessObj(Request, Out);
}

// ─── sequencer_ext.add_track ──────────────────────────────────────────────────────────────────
//
// Args:    { sequence_path: string, binding_guid: string,
//            track_class: "Transform" | "3DTransform" | "Visibility" | "Float" }
// Result:  { track_class_path: string,    // full UClass path (e.g. "/Script/MovieSceneTracks.MovieScene3DTransformTrack")
//            track_guid?: string,         // UMovieSceneTrack signature guid (uniquely identifies the track instance)
//            binding_guid: string,        // echo for confirmation
//            track_index: number }        // index within binding's tracks array (insertion order)
//
// Errors:
//   -32602 InvalidParams        missing fields / bad guid format / unknown track_class
//   -32010 InvalidPath          sequence_path malformed
//   -32004 ObjectNotFound       LoadObject returned null OR binding guid not on this sequence
//   -32011 WrongClass           asset is not ULevelSequence
//   -32027 PIEActive            mutator refused during PIE
//   -32603 InternalError        MovieScene null / AddTrack returned null
//
// Adds a track of the requested class to an existing binding. The track is created via
// ``UMovieScene::AddTrack(TrackClass, BindingGuid)`` — the binding-scoped variant — and the
// editor-side ``DisplayName`` is auto-derived from the track class's default name (matches
// the Sequencer editor's "+ Track" menu behaviour).
//
// **Adding multiple tracks of the same class on one binding is supported** — UMovieScene
// permits parallel tracks (e.g. two Float tracks on the same component, distinguished by
// per-track display name). Use ``sequencer.get_tracks`` to disambiguate after creation.
FMCPResponse Tool_AddTrack(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (FMCPWorldContext::IsPIEActive())
	{
		return SEQX_MakeError(Request, kMCPErrorPIEActive, kMCPMessagePIEActive);
	}

	FString SeqPath, BindingGuidStr, TrackClassName;
	FMCPResponse Err;
	if (!SEQX_RequireStringField(Request, TEXT("sequence_path"), SeqPath, Err))         { return Err; }
	if (!SEQX_RequireStringField(Request, TEXT("binding_guid"),  BindingGuidStr, Err))  { return Err; }
	if (!SEQX_RequireStringField(Request, TEXT("track_class"),   TrackClassName, Err))  { return Err; }

	FGuid BindingGuid;
	if (!SEQX_ParseBindingGuid(BindingGuidStr, BindingGuid))
	{
		return SEQX_MakeError(Request, kSEQXErrorInvalidParams,
			FString::Printf(TEXT("binding_guid '%s' is not a valid GUID "
				"(expected DigitsWithHyphens form, e.g. '12345678-1234-1234-1234-123456789012')"),
				*BindingGuidStr));
	}

	UClass* TrackClass = SEQX_ResolveTrackClass(TrackClassName);
	if (!TrackClass)
	{
		return SEQX_MakeError(Request, kSEQXErrorInvalidParams,
			FString::Printf(TEXT("track_class '%s' not recognised "
				"(accepted: 'Transform', '3DTransform', 'Visibility', 'Float')"),
				*TrackClassName));
	}

	int32 ErrCode = 0;
	FString ErrMsg;
	ULevelSequence* Seq = SEQX_LoadLevelSequenceByPath(SeqPath, ErrCode, ErrMsg);
	if (!Seq) { return SEQX_MakeError(Request, ErrCode, ErrMsg); }

	UMovieScene* MovieScene = Seq->GetMovieScene();
	if (!MovieScene)
	{
		return SEQX_MakeError(Request, kSEQXErrorInternal,
			FString::Printf(TEXT("sequence '%s' has no MovieScene"), *SeqPath));
	}

	if (!SEQX_BindingExists(MovieScene, BindingGuid))
	{
		return SEQX_MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("binding_guid '%s' is not a possessable or spawnable on sequence '%s' "
				"(use sequencer.get_tracks to enumerate binding GUIDs)"),
				*BindingGuid.ToString(EGuidFormats::DigitsWithHyphens), *SeqPath));
	}

	FScopedTransaction Transaction(LOCTEXT("MCP_AddTrack", "Add Sequencer Track"));
	MovieScene->Modify();
	Seq->Modify();

	UMovieSceneTrack* NewTrack = MovieScene->AddTrack(TrackClass, BindingGuid);
	if (!NewTrack)
	{
		return SEQX_MakeError(Request, kSEQXErrorInternal,
			FString::Printf(TEXT("UMovieScene::AddTrack returned null for class '%s' on binding '%s'"),
				*TrackClass->GetPathName(), *BindingGuid.ToString(EGuidFormats::DigitsWithHyphens)));
	}

	if (UPackage* Pkg = Seq->GetOutermost()) { Pkg->MarkPackageDirty(); }

	// Compute insertion index within the binding's tracks. FindBinding is const-safe here.
	int32 TrackIndexInBinding = INDEX_NONE;
	if (const FMovieSceneBinding* Binding = MovieScene->FindBinding(BindingGuid))
	{
		TrackIndexInBinding = Binding->GetTracks().Find(NewTrack);
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("track_class_path"), NewTrack->GetClass()->GetPathName());
	Out->SetStringField(TEXT("track_guid"),
		NewTrack->GetSignature().ToString(EGuidFormats::DigitsWithHyphens));
	Out->SetStringField(TEXT("binding_guid"),
		BindingGuid.ToString(EGuidFormats::DigitsWithHyphens));
	Out->SetNumberField(TEXT("track_index"), TrackIndexInBinding);
	return SEQX_MakeSuccessObj(Request, Out);
}

// ─── sequencer_ext.add_section ────────────────────────────────────────────────────────────────
//
// Args:    { sequence_path: string, binding_guid: string,
//            track_class:   "Transform" | "3DTransform" | "Visibility" | "Float",
//            start_frame?:  int (default 0),
//            end_frame?:    int (default 120) }
// Result:  { section_index: int,         // index within track's GetAllSections() array
//            section_class: string,      // full UClass path of the created section
//            start_frame:   int,         // ticks at MovieScene tick resolution (echoed)
//            end_frame:     int }
//
// Errors:
//   -32602 InvalidParams        missing fields / bad guid / unknown track_class / end<start
//   -32010 InvalidPath          sequence_path malformed
//   -32004 ObjectNotFound       LoadObject returned null OR binding/track not found
//   -32011 WrongClass           asset is not ULevelSequence
//   -32027 PIEActive            mutator refused during PIE
//   -32603 InternalError        MovieScene null / CreateNewSection returned null
//
// Adds a new section to the **first** track of the given class on the binding (matches the
// "add section to existing track" Sequencer editor flow). For multi-track-per-class bindings,
// callers needing index-specific targeting should use ``sequencer.set_section_range`` after
// creation OR call ``sequencer.get_tracks`` to discover the track index first.
//
// **Section creation path.** ``UMovieSceneTrack::CreateNewSection()`` is the virtual that each
// track subclass overrides to spawn its canonical section type (Transform → 3DTransformSection,
// Visibility → VisibilitySection, Float → FloatSection). The track owns the section after
// ``AddSection``, so no manual outer / RF_Transactional setup is needed here — the track
// constructor handles those flags via ``CreateNewSection``.
//
// **Frame defaults.** ``start_frame=0`` / ``end_frame=120`` are designer-friendly defaults
// matching UE's "5 second @ 24fps" pattern at the default MovieScene tick rate of 24000/1.
// Callers requiring an exact range should always pass both fields explicitly.
FMCPResponse Tool_AddSection(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (FMCPWorldContext::IsPIEActive())
	{
		return SEQX_MakeError(Request, kMCPErrorPIEActive, kMCPMessagePIEActive);
	}

	FString SeqPath, BindingGuidStr, TrackClassName;
	FMCPResponse Err;
	if (!SEQX_RequireStringField(Request, TEXT("sequence_path"), SeqPath, Err))         { return Err; }
	if (!SEQX_RequireStringField(Request, TEXT("binding_guid"),  BindingGuidStr, Err))  { return Err; }
	if (!SEQX_RequireStringField(Request, TEXT("track_class"),   TrackClassName, Err))  { return Err; }

	// Optional frame fields with sane defaults.
	int32 StartFrame = 0;
	int32 EndFrame   = 120;
	Request.Args->TryGetNumberField(TEXT("start_frame"), StartFrame);
	Request.Args->TryGetNumberField(TEXT("end_frame"),   EndFrame);
	if (EndFrame <= StartFrame)
	{
		return SEQX_MakeError(Request, kSEQXErrorInvalidParams,
			FString::Printf(TEXT("end_frame (%d) must be greater than start_frame (%d)"),
				EndFrame, StartFrame));
	}

	FGuid BindingGuid;
	if (!SEQX_ParseBindingGuid(BindingGuidStr, BindingGuid))
	{
		return SEQX_MakeError(Request, kSEQXErrorInvalidParams,
			FString::Printf(TEXT("binding_guid '%s' is not a valid GUID "
				"(expected DigitsWithHyphens form, e.g. '12345678-1234-1234-1234-123456789012')"),
				*BindingGuidStr));
	}

	UClass* TrackClass = SEQX_ResolveTrackClass(TrackClassName);
	if (!TrackClass)
	{
		return SEQX_MakeError(Request, kSEQXErrorInvalidParams,
			FString::Printf(TEXT("track_class '%s' not recognised "
				"(accepted: 'Transform', '3DTransform', 'Visibility', 'Float')"),
				*TrackClassName));
	}

	int32 ErrCode = 0;
	FString ErrMsg;
	ULevelSequence* Seq = SEQX_LoadLevelSequenceByPath(SeqPath, ErrCode, ErrMsg);
	if (!Seq) { return SEQX_MakeError(Request, ErrCode, ErrMsg); }

	UMovieScene* MovieScene = Seq->GetMovieScene();
	if (!MovieScene)
	{
		return SEQX_MakeError(Request, kSEQXErrorInternal,
			FString::Printf(TEXT("sequence '%s' has no MovieScene"), *SeqPath));
	}

	if (!SEQX_BindingExists(MovieScene, BindingGuid))
	{
		return SEQX_MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("binding_guid '%s' is not a possessable or spawnable on sequence '%s'"),
				*BindingGuid.ToString(EGuidFormats::DigitsWithHyphens), *SeqPath));
	}

	// FindTracks returns all tracks of the given class on the binding. We grab the first —
	// matches Sequencer editor "+ Section" behaviour against the visually-topmost track of
	// the requested type.
	const TArray<UMovieSceneTrack*> Tracks = MovieScene->FindTracks(TrackClass, BindingGuid);
	if (Tracks.Num() == 0)
	{
		return SEQX_MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("no track of class '%s' exists on binding '%s' "
				"(create one via sequencer_ext.add_track first)"),
				*TrackClass->GetPathName(),
				*BindingGuid.ToString(EGuidFormats::DigitsWithHyphens)));
	}
	UMovieSceneTrack* Track = Tracks[0];
	check(Track != nullptr);

	FScopedTransaction Transaction(LOCTEXT("MCP_AddSection", "Add Sequencer Section"));
	MovieScene->Modify();
	Track->Modify();

	UMovieSceneSection* Section = Track->CreateNewSection();
	if (!Section)
	{
		return SEQX_MakeError(Request, kSEQXErrorInternal,
			FString::Printf(TEXT("UMovieSceneTrack::CreateNewSection returned null for class '%s'"),
				*TrackClass->GetPathName()));
	}

	Section->SetRange(TRange<FFrameNumber>(FFrameNumber(StartFrame), FFrameNumber(EndFrame)));
	Track->AddSection(*Section);

	if (UPackage* Pkg = Seq->GetOutermost()) { Pkg->MarkPackageDirty(); }

	const int32 SectionIndex = Track->GetAllSections().Find(Section);

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetNumberField(TEXT("section_index"), SectionIndex);
	Out->SetStringField(TEXT("section_class"), Section->GetClass()->GetPathName());
	Out->SetNumberField(TEXT("start_frame"),   StartFrame);
	Out->SetNumberField(TEXT("end_frame"),     EndFrame);
	return SEQX_MakeSuccessObj(Request, Out);
}

// ─── Registration ──────────────────────────────────────────────────────────────────────────────
void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	RegisterTool(TEXT("sequencer_ext.add_possessable"), &Tool_AddPossessable, /*Lane A*/ false);
	RegisterTool(TEXT("sequencer_ext.add_track"),       &Tool_AddTrack,       /*Lane A*/ false);
	RegisterTool(TEXT("sequencer_ext.add_section"),     &Tool_AddSection,     /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("Sequencer extension surface registered: 3 tools "
			 "(sequencer_ext.add_possessable / add_track / add_section), all Lane A, PIE-guarded"));
}

} // namespace FSequencerExtTools

#undef LOCTEXT_NAMESPACE
