// Copyright FatumGame. All Rights Reserved.

#include "AIPerceptionTools.h"

#include "FMCPDispatchQueue.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPActorPathUtils.h"
#include "Utils/MCPWorldContext.h"

#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Perception/AIPerceptionComponent.h"
#include "Perception/AIPerceptionTypes.h"
#include "Perception/AISense.h"
#include "Perception/AISenseConfig.h"
#include "Perception/AISenseConfig_Damage.h"
#include "Perception/AISenseConfig_Hearing.h"
#include "Perception/AISenseConfig_Prediction.h"
#include "Perception/AISenseConfig_Sight.h"
#include "Perception/AISenseConfig_Team.h"
#include "Perception/AISenseConfig_Touch.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// AIPER_ prefix per the unity-build symbol-collision convention. Every Tools/*.cpp shares
	// the same anonymous namespace under unity builds, so helpers MUST be uniquely prefixed —
	// see the BlueprintComponentTools rename note in MEMORY.md (Wave F4) for the failure mode.
	constexpr int32 kAIPERErrorInvalidParams   = -32602;
	constexpr int32 kAIPERErrorInternal        = -32603;
	constexpr int32 kAIPERErrorObjectNotFound  = kMCPErrorObjectNotFound;  // -32004
	constexpr int32 kAIPERErrorWrongClass      = kMCPErrorWrongClass;      // -32011

	void AIPER_StampIds(const FMCPRequest& Request, FMCPResponse& Response)
	{
		Response.RequestId = Request.RequestId;
		Response.OriginalIdString = Request.OriginalIdString;
	}

	FMCPResponse AIPER_MakeError(const FMCPRequest& Request, int32 Code, const FString& Message)
	{
		FMCPResponse R;
		AIPER_StampIds(Request, R);
		R.bIsError = true;
		R.ErrorCode = Code;
		R.ErrorMessage = Message;
		return R;
	}

	FMCPResponse AIPER_MakeSuccessObj(const FMCPRequest& Request, TSharedPtr<FJsonObject> Result)
	{
		FMCPResponse R;
		AIPER_StampIds(Request, R);
		R.bIsError = false;
		R.Result = MakeShared<FJsonValueObject>(MoveTemp(Result));
		return R;
	}

	// ─── World resolution (PIE-first, editor-fallback) ───────────────────────────────────────────

	/**
	 * Resolve the perception-bearing world. PIE first (so runtime introspection during gameplay
	 * works — perception data only flows during PIE/Standalone), editor world otherwise. Returns
	 * null only when GEditor is unavailable (commandlet / cooker contexts).
	 */
	UWorld* AIPER_ResolveWorld()
	{
		check(IsInGameThread());
		if (GEditor && GEditor->PlayWorld)
		{
			return GEditor->PlayWorld;
		}
		return FMCPWorldContext::GetEditorWorld();
	}

	// ─── Sense-class formatting ──────────────────────────────────────────────────────────────────

	/**
	 * Format a UAISense subclass as the canonical full path string for the wire
	 * (e.g. ``/Script/AIModule.AISense_Sight``). Returns empty for null input.
	 */
	FString AIPER_SenseClassToWireString(const UClass* SenseClass)
	{
		if (!SenseClass) { return FString(); }
		return SenseClass->GetPathName();
	}

	/**
	 * Resolve a sense_filter wire string to a UAISense subclass. Accepts:
	 *   - Full class path: ``/Script/AIModule.AISense_Sight``
	 *   - Short class name: ``AISense_Sight``
	 *   - Class name with trailing ``_C`` (defensive — most callers won't use it for native classes).
	 *
	 * Returns null when the input doesn't resolve to a UAISense-derived UClass. OutError populated
	 * for caller surfacing as kAIPERErrorInvalidParams.
	 */
	UClass* AIPER_ResolveSenseClass(const FString& Raw, FString& OutError)
	{
		if (Raw.IsEmpty())
		{
			OutError = TEXT("sense_filter is empty (use absent/null for unfiltered)");
			return nullptr;
		}

		// Try full path first.
		UClass* Candidate = nullptr;
		if (Raw.StartsWith(TEXT("/")))
		{
			Candidate = LoadClass<UAISense>(nullptr, *Raw);
			if (!Candidate)
			{
				Candidate = FindObject<UClass>(nullptr, *Raw);
			}
		}
		else
		{
			// Short name — try the well-known AIModule path first, then fall back to scanning
			// loaded UClasses for a matching FName. AIModule is reliably loaded once any
			// perception component is touched, so the StaticLoadClass path is the fast path.
			const FString Composed = FString::Printf(TEXT("/Script/AIModule.%s"), *Raw);
			Candidate = LoadClass<UAISense>(nullptr, *Composed);
			if (!Candidate)
			{
				Candidate = FindObject<UClass>(nullptr, *Composed);
			}
			if (!Candidate)
			{
				// Last-resort scan — handles project-custom sense classes outside /Script/AIModule.
				for (TObjectIterator<UClass> It; It; ++It)
				{
					if (It->GetName() == Raw && It->IsChildOf(UAISense::StaticClass()))
					{
						Candidate = *It;
						break;
					}
				}
			}
		}

		if (!Candidate)
		{
			OutError = FString::Printf(
				TEXT("sense_filter '%s' did not resolve to a UClass (try full path like "
					 "'/Script/AIModule.AISense_Sight')"), *Raw);
			return nullptr;
		}
		if (!Candidate->IsChildOf(UAISense::StaticClass()))
		{
			OutError = FString::Printf(
				TEXT("sense_filter '%s' resolved to class '%s' which is not a UAISense subclass"),
				*Raw, *Candidate->GetPathName());
			return nullptr;
		}
		return Candidate;
	}

	// ─── Per-sense-config property surface ───────────────────────────────────────────────────────

	/**
	 * Append type-specific UPROPERTY values for a UAISenseConfig instance into OutProps. The
	 * common fields (max_age, starts_enabled, sense_class) are emitted by the caller — this
	 * helper only handles the per-subclass properties (sight_radius, hearing_range, etc.).
	 *
	 * Unknown sense subclasses (e.g. UAISenseConfig_Blueprint, project-custom configs) get an
	 * empty OutProps — fail-open rather than fail-hard, so list_components / get_config still
	 * surface the common triple for unfamiliar senses.
	 */
	void AIPER_AppendSenseSpecificProps(const UAISenseConfig* Config, TSharedRef<FJsonObject>& OutProps)
	{
		check(Config);

		if (const UAISenseConfig_Sight* Sight = Cast<UAISenseConfig_Sight>(Config))
		{
			OutProps->SetNumberField(TEXT("sight_radius"),                          Sight->SightRadius);
			OutProps->SetNumberField(TEXT("lose_sight_radius"),                     Sight->LoseSightRadius);
			OutProps->SetNumberField(TEXT("peripheral_vision_angle_degrees"),       Sight->PeripheralVisionAngleDegrees);
			OutProps->SetNumberField(TEXT("auto_success_range_from_last_seen"),     Sight->AutoSuccessRangeFromLastSeenLocation);
			OutProps->SetNumberField(TEXT("point_of_view_backward_offset"),         Sight->PointOfViewBackwardOffset);
			OutProps->SetNumberField(TEXT("near_clipping_radius"),                  Sight->NearClippingRadius);
			OutProps->SetBoolField  (TEXT("detect_enemies"),    Sight->DetectionByAffiliation.bDetectEnemies);
			OutProps->SetBoolField  (TEXT("detect_neutrals"),   Sight->DetectionByAffiliation.bDetectNeutrals);
			OutProps->SetBoolField  (TEXT("detect_friendlies"), Sight->DetectionByAffiliation.bDetectFriendlies);
			return;
		}
		if (const UAISenseConfig_Hearing* Hearing = Cast<UAISenseConfig_Hearing>(Config))
		{
			OutProps->SetNumberField(TEXT("hearing_range"), Hearing->HearingRange);
			OutProps->SetBoolField  (TEXT("detect_enemies"),    Hearing->DetectionByAffiliation.bDetectEnemies);
			OutProps->SetBoolField  (TEXT("detect_neutrals"),   Hearing->DetectionByAffiliation.bDetectNeutrals);
			OutProps->SetBoolField  (TEXT("detect_friendlies"), Hearing->DetectionByAffiliation.bDetectFriendlies);
			return;
		}
		// Damage / Prediction / Team / Touch / Blueprint / project-custom: no extra public fields
		// worth surfacing beyond the common triple. Leave OutProps empty — the wire entry still
		// carries sense_class, max_age, starts_enabled so the caller can identify the config.
		(void)Cast<UAISenseConfig_Damage>(Config);
		(void)Cast<UAISenseConfig_Prediction>(Config);
		(void)Cast<UAISenseConfig_Team>(Config);
		(void)Cast<UAISenseConfig_Touch>(Config);
	}

	/**
	 * Build the per-sense-config wire entry. Emits the common triple (sense_class, max_age,
	 * starts_enabled) plus the type-specific properties via AIPER_AppendSenseSpecificProps. The
	 * ``dominant`` flag is supplied by the caller (which knows the dominant-sense ID from the
	 * perception component).
	 */
	TSharedRef<FJsonObject> AIPER_BuildSenseConfigEntry(
		const UAISenseConfig* Config, bool bDominant)
	{
		check(Config);
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();

		// Resolve the IMPLEMENTED sense class (not the config class — those are 1:1 but the
		// public contract is "which UAISense subclass does this listen to").
		const TSubclassOf<UAISense> Impl = Config->GetSenseImplementation();
		Entry->SetStringField(TEXT("sense_class"), AIPER_SenseClassToWireString(*Impl));

		// MaxAge of 0 means "never expire" per UAISenseConfig::GetMaxAge contract — the function
		// returns NeverHappenedAge in that case. We surface the raw stored value so callers see
		// "0 = never" the same way the editor UI does.
		Entry->SetNumberField(TEXT("max_age"), Config->GetMaxAge() >= FAIStimulus::NeverHappenedAge
			? 0.0 : Config->GetMaxAge());
		Entry->SetBoolField(TEXT("starts_enabled"), Config->GetStartsEnabled());
		Entry->SetBoolField(TEXT("dominant"), bDominant);

		TSharedRef<FJsonObject> Props = MakeShared<FJsonObject>();
		AIPER_AppendSenseSpecificProps(Config, Props);
		Entry->SetObjectField(TEXT("sense_specific_props"), Props);
		return Entry;
	}

	// ─── Perception-component lookup ─────────────────────────────────────────────────────────────

	/**
	 * Resolve an actor by wire path and find its UAIPerceptionComponent. Returns null on any
	 * failure — OutErrorCode / OutError populated for caller surfacing. Sets OutActor to the
	 * resolved actor even when the component is null (useful for fine-grained error messages).
	 *
	 * Resolution honours PIE: bRejectPIE=false so PIE-spawned actors are visible to read-only
	 * tools when PIE is active.
	 */
	UAIPerceptionComponent* AIPER_ResolvePerceptionComponent(
		const FString& ActorPath,
		int32& OutErrorCode,
		FString& OutError,
		AActor*& OutActor)
	{
		OutActor = nullptr;
		bool bAmbiguous = false;
		FString AmbiguityHint, ResolveErr;
		AActor* Actor = FMCPActorPathUtils::ResolveActor(
			ActorPath, /*bRejectPIE*/ false, bAmbiguous, AmbiguityHint, ResolveErr);
		if (!Actor)
		{
			OutErrorCode = kAIPERErrorObjectNotFound;
			OutError = bAmbiguous
				? FString::Printf(TEXT("actor '%s' is ambiguous: %s"), *ActorPath, *AmbiguityHint)
				: FString::Printf(TEXT("actor '%s' not found: %s"), *ActorPath, *ResolveErr);
			return nullptr;
		}
		OutActor = Actor;
		UAIPerceptionComponent* Perc = Actor->FindComponentByClass<UAIPerceptionComponent>();
		if (!Perc)
		{
			OutErrorCode = kAIPERErrorWrongClass;
			OutError = FString::Printf(
				TEXT("actor '%s' has no UAIPerceptionComponent"), *ActorPath);
			return nullptr;
		}
		return Perc;
	}
} // namespace

namespace FAIPerceptionTools
{

// ─── ai.perception.list_components ────────────────────────────────────────────────────────────
//
// Args:    {} (no required fields)
// Result:  { perception_components: [{ owner_actor_path, sense_configs: [{ sense_class, dominant }] }],
//            total: int, world_kind: "editor"|"pie" }
//
// Walks TObjectIterator<UAIPerceptionComponent> and filters by world (PIE-first, editor-fallback).
// Empty array is a legitimate result — most maps have no AI. The per-entry sense_configs is a
// LIGHTWEIGHT projection — only sense_class + dominant flag. For full property surface use
// ai.perception.get_config on the actor_path. Read-only, no PIE guard.
//
// Errors: -32603 InternalError when no world is available (commandlet / cooker contexts).
FMCPResponse Tool_ListComponents(const FMCPRequest& Request)
{
	check(IsInGameThread());

	UWorld* World = AIPER_ResolveWorld();
	if (!World)
	{
		return AIPER_MakeError(Request, kAIPERErrorInternal,
			TEXT("no world available (GEditor missing OR no level loaded)"));
	}

	const TCHAR* WorldKind = (World->WorldType == EWorldType::PIE) ? TEXT("pie") : TEXT("editor");

	TArray<TSharedPtr<FJsonValue>> Items;
	for (TObjectIterator<UAIPerceptionComponent> It; It; ++It)
	{
		UAIPerceptionComponent* Perc = *It;
		if (!Perc) { continue; }
		// Skip CDOs / archetypes / pending-kill — TObjectIterator returns these too.
		if (Perc->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject)) { continue; }
		if (!IsValid(Perc)) { continue; }
		if (Perc->GetWorld() != World) { continue; }

		AActor* Owner = Perc->GetOwner();
		if (!Owner || !IsValid(Owner)) { continue; }

		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("owner_actor_path"), FMCPActorPathUtils::BuildActorPath(Owner));

		// Minimal sense_configs projection: sense_class + dominant only. Full inspection lives
		// in ai.perception.get_config to keep this enumerator response bounded for large AI casts.
		const TSubclassOf<UAISense> DominantSense = Perc->GetDominantSense();
		const UClass* DominantClass = *DominantSense;
		TArray<TSharedPtr<FJsonValue>> ConfigItems;
		for (auto ConfigIt = Perc->GetSensesConfigIterator(); ConfigIt; ++ConfigIt)
		{
			const UAISenseConfig* Config = *ConfigIt;
			if (!Config) { continue; }
			const TSubclassOf<UAISense> Impl = Config->GetSenseImplementation();
			TSharedRef<FJsonObject> ConfigEntry = MakeShared<FJsonObject>();
			ConfigEntry->SetStringField(TEXT("sense_class"), AIPER_SenseClassToWireString(*Impl));
			ConfigEntry->SetBoolField(TEXT("dominant"),
				DominantClass != nullptr && *Impl == DominantClass);
			ConfigItems.Add(MakeShared<FJsonValueObject>(ConfigEntry));
		}
		Entry->SetArrayField(TEXT("sense_configs"), ConfigItems);

		Items.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetArrayField(TEXT("perception_components"), Items);
	Out->SetNumberField(TEXT("total"), Items.Num());
	Out->SetStringField(TEXT("world_kind"), WorldKind);
	return AIPER_MakeSuccessObj(Request, Out);
}

// ─── ai.perception.get_config ─────────────────────────────────────────────────────────────────
//
// Args:    { actor_path: string }
// Result:  {
//            actor_path: string,
//            dominant_sense_class?: string,          // omitted when no dominant sense configured
//            sense_configs: [
//              {
//                sense_class: string,
//                max_age: float,                     // 0 = never expires (per UE convention)
//                starts_enabled: bool,
//                dominant: bool,
//                sense_specific_props: object        // sight_radius, hearing_range, etc.
//              }, ...
//            ]
//          }
//
// Detailed per-sense-config inspection for one perception component. Iterates the perception
// component's public ``GetSensesConfigIterator()`` and downcasts each ``UAISenseConfig`` to a
// known subclass (Sight / Hearing / Damage / Prediction / Team / Touch) to surface
// type-specific UPROPERTY values via ``AIPER_AppendSenseSpecificProps``. Unknown subclasses
// (UAISenseConfig_Blueprint, project-custom configs) get an empty sense_specific_props object
// — the common triple still surfaces. Read-only, no PIE guard.
//
// Errors:
//   -32602 InvalidParams      missing actor_path
//   -32004 ObjectNotFound     actor_path doesn't resolve to any actor
//   -32011 WrongClass         actor resolved but has no UAIPerceptionComponent
FMCPResponse Tool_GetConfig(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!Request.Args.IsValid())
	{
		return AIPER_MakeError(Request, kAIPERErrorInvalidParams, TEXT("missing args object"));
	}

	FString ActorPath;
	if (!Request.Args->TryGetStringField(TEXT("actor_path"), ActorPath) || ActorPath.IsEmpty())
	{
		return AIPER_MakeError(Request, kAIPERErrorInvalidParams,
			TEXT("missing required string field 'actor_path'"));
	}

	AActor* Actor = nullptr;
	int32 ErrorCode = 0;
	FString ResolveErr;
	UAIPerceptionComponent* Perc = AIPER_ResolvePerceptionComponent(
		ActorPath, ErrorCode, ResolveErr, Actor);
	if (!Perc)
	{
		return AIPER_MakeError(Request, ErrorCode, ResolveErr);
	}

	const TSubclassOf<UAISense> DominantSense = Perc->GetDominantSense();
	const UClass* DominantClass = *DominantSense;

	TArray<TSharedPtr<FJsonValue>> ConfigItems;
	for (auto ConfigIt = Perc->GetSensesConfigIterator(); ConfigIt; ++ConfigIt)
	{
		const UAISenseConfig* Config = *ConfigIt;
		if (!Config) { continue; }
		const TSubclassOf<UAISense> Impl = Config->GetSenseImplementation();
		const bool bDominant = (DominantClass != nullptr && *Impl == DominantClass);
		ConfigItems.Add(MakeShared<FJsonValueObject>(
			AIPER_BuildSenseConfigEntry(Config, bDominant)));
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("actor_path"), FMCPActorPathUtils::BuildActorPath(Actor));
	if (DominantClass)
	{
		Out->SetStringField(TEXT("dominant_sense_class"),
			AIPER_SenseClassToWireString(DominantClass));
	}
	Out->SetArrayField(TEXT("sense_configs"), ConfigItems);
	return AIPER_MakeSuccessObj(Request, Out);
}

// ─── ai.perception.get_perceived_actors ───────────────────────────────────────────────────────
//
// Args:    { actor_path: string, sense_filter?: string }
// Result:  {
//            actor_path: string,
//            perceived: [
//              {
//                actor_path: string,                 // perceived actor's wire path (may be null
//                                                    // for invalidated TWeakObjectPtr targets)
//                sense_class: string,                // /Script/AIModule.AISense_X
//                stimulus_age: float,
//                stimulus_location: [x, y, z],
//                receiver_location: [x, y, z],
//                is_active: bool,                    // currently sensed AND non-expired
//                is_successfully_sensed: bool,       // last poll succeeded (false = "lost track")
//                is_expired: bool
//              }, ...
//            ],
//            total: int
//          }
//
// Walks the perception component's PerceptualData map via ``GetPerceptualDataConstIterator()``,
// then per-target iterates ``LastSensedStimuli[]`` (indexed by FAISenseID). Each VALID stimulus
// (Type != InvalidID, GetAge < NeverHappenedAge) emits one wire entry. The optional
// ``sense_filter`` argument narrows to one sense class. Sense-class names returned are the
// full ``/Script/AIModule.<ClassName>`` form via ``UAISense::GetSenseClass``-equivalent (we
// re-resolve from the component's sense config since FAIStimulus only carries the FAISenseID).
//
// **PIE-safe.** This tool is the primary runtime introspection vector — the editor world has
// no live stimuli flowing through PerceptualData (the perception system only ticks during
// PIE/Standalone). Calling against an editor-world perception component returns an empty
// array, which is the correct truth.
//
// **TWeakObjectPtr targets.** ``FActorPerceptionInfo::Target`` is weak — perceived actors that
// have been destroyed between sensing and tool invocation surface ``actor_path=null`` rather
// than dropping the entry, so the caller can still see the stimulus metadata.
//
// Errors:
//   -32602 InvalidParams      missing actor_path OR malformed sense_filter
//   -32004 ObjectNotFound     actor_path doesn't resolve
//   -32011 WrongClass         actor resolved but has no UAIPerceptionComponent
FMCPResponse Tool_GetPerceivedActors(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!Request.Args.IsValid())
	{
		return AIPER_MakeError(Request, kAIPERErrorInvalidParams, TEXT("missing args object"));
	}

	FString ActorPath;
	if (!Request.Args->TryGetStringField(TEXT("actor_path"), ActorPath) || ActorPath.IsEmpty())
	{
		return AIPER_MakeError(Request, kAIPERErrorInvalidParams,
			TEXT("missing required string field 'actor_path'"));
	}

	// Optional sense filter — resolve early so a malformed string fails before any iteration.
	UClass* FilterClass = nullptr;
	FString FilterRaw;
	if (Request.Args->TryGetStringField(TEXT("sense_filter"), FilterRaw) && !FilterRaw.IsEmpty())
	{
		FString FilterErr;
		FilterClass = AIPER_ResolveSenseClass(FilterRaw, FilterErr);
		if (!FilterClass)
		{
			return AIPER_MakeError(Request, kAIPERErrorInvalidParams, FilterErr);
		}
	}

	AActor* Actor = nullptr;
	int32 ErrorCode = 0;
	FString ResolveErr;
	UAIPerceptionComponent* Perc = AIPER_ResolvePerceptionComponent(
		ActorPath, ErrorCode, ResolveErr, Actor);
	if (!Perc)
	{
		return AIPER_MakeError(Request, ErrorCode, ResolveErr);
	}

	// Pre-resolve the sense-ID → sense-class string mapping by walking the component's sense
	// configs. FAIStimulus only carries an FAISenseID (uint8); we need the UClass for the
	// wire path. SensesConfig is at most ~8 entries in practice — linear scan is fine.
	TMap<FAISenseID, FString> SenseIdToClassPath;
	FAISenseID FilterSenseId = FAISenseID::InvalidID();
	for (auto ConfigIt = Perc->GetSensesConfigIterator(); ConfigIt; ++ConfigIt)
	{
		const UAISenseConfig* Config = *ConfigIt;
		if (!Config) { continue; }
		const TSubclassOf<UAISense> Impl = Config->GetSenseImplementation();
		const FAISenseID SenseId = Config->GetSenseID();
		if (!SenseId.IsValid()) { continue; }
		SenseIdToClassPath.Add(SenseId, AIPER_SenseClassToWireString(*Impl));
		if (FilterClass != nullptr && *Impl == FilterClass)
		{
			FilterSenseId = SenseId;
		}
	}
	// If caller supplied a filter and the perception component is NOT configured for that
	// sense, the result is a legitimate empty perceived[] array — not an error. The filter
	// just doesn't match anything. Continue with FilterSenseId = InvalidID; the per-stimulus
	// check below will skip every entry.

	TArray<TSharedPtr<FJsonValue>> Items;
	for (auto DataIt = Perc->GetPerceptualDataConstIterator(); DataIt; ++DataIt)
	{
		const FActorPerceptionInfo& Info = DataIt->Value;

		// Perceived actor — may be invalidated since last stimulus. Caller still sees the
		// metadata via actor_path=null.
		AActor* Target = Info.Target.Get();

		for (const FAIStimulus& Stimulus : Info.LastSensedStimuli)
		{
			if (!Stimulus.IsValid()) { continue; }
			if (FilterClass != nullptr && Stimulus.Type != FilterSenseId) { continue; }

			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			if (Target)
			{
				Entry->SetStringField(TEXT("actor_path"),
					FMCPActorPathUtils::BuildActorPath(Target));
			}
			else
			{
				Entry->SetField(TEXT("actor_path"), MakeShared<FJsonValueNull>());
			}

			const FString* SenseClassStr = SenseIdToClassPath.Find(Stimulus.Type);
			Entry->SetStringField(TEXT("sense_class"),
				SenseClassStr ? *SenseClassStr : FString());

			Entry->SetNumberField(TEXT("stimulus_age"), Stimulus.GetAge());

			TArray<TSharedPtr<FJsonValue>> StimLoc;
			StimLoc.Reserve(3);
			StimLoc.Add(MakeShared<FJsonValueNumber>(Stimulus.StimulusLocation.X));
			StimLoc.Add(MakeShared<FJsonValueNumber>(Stimulus.StimulusLocation.Y));
			StimLoc.Add(MakeShared<FJsonValueNumber>(Stimulus.StimulusLocation.Z));
			Entry->SetArrayField(TEXT("stimulus_location"), StimLoc);

			TArray<TSharedPtr<FJsonValue>> RecvLoc;
			RecvLoc.Reserve(3);
			RecvLoc.Add(MakeShared<FJsonValueNumber>(Stimulus.ReceiverLocation.X));
			RecvLoc.Add(MakeShared<FJsonValueNumber>(Stimulus.ReceiverLocation.Y));
			RecvLoc.Add(MakeShared<FJsonValueNumber>(Stimulus.ReceiverLocation.Z));
			Entry->SetArrayField(TEXT("receiver_location"), RecvLoc);

			Entry->SetBoolField(TEXT("is_active"),                Stimulus.IsActive());
			Entry->SetBoolField(TEXT("is_successfully_sensed"),   Stimulus.WasSuccessfullySensed());
			Entry->SetBoolField(TEXT("is_expired"),               Stimulus.IsExpired());

			Items.Add(MakeShared<FJsonValueObject>(Entry));
		}
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("actor_path"), FMCPActorPathUtils::BuildActorPath(Actor));
	Out->SetArrayField(TEXT("perceived"), Items);
	Out->SetNumberField(TEXT("total"), Items.Num());
	return AIPER_MakeSuccessObj(Request, Out);
}

// ─── Registration ─────────────────────────────────────────────────────────────────────────────
void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	RegisterTool(TEXT("ai.perception.list_components"),      &Tool_ListComponents,     /*Lane A*/ false);
	RegisterTool(TEXT("ai.perception.get_config"),           &Tool_GetConfig,          /*Lane A*/ false);
	RegisterTool(TEXT("ai.perception.get_perceived_actors"), &Tool_GetPerceivedActors, /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("AIPerception surface registered: 3 ai.perception.* tools "
			 "(list_components + get_config + get_perceived_actors), all Lane A"));
}

} // namespace FAIPerceptionTools

// Wave J auto-registration via FMCPSurfaceRegistry — replaces the manual include + Register
// call in UnrealMCPBridge.cpp. Static initializer fires at module load.
#include "MCPSurfaceRegistry.h"
MCP_REGISTER_SURFACE(AIPerceptionTools, &FAIPerceptionTools::Register)
