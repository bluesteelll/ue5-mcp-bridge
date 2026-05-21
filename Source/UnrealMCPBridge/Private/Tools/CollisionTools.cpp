// Copyright FatumGame. All Rights Reserved.

#include "CollisionTools.h"

#include "FMCPDispatchQueue.h"
#include "UnrealMCPBridge.h"

#include "Engine/CollisionProfile.h"
#include "Engine/EngineTypes.h"
#include "ScopedTransaction.h"
#include "UObject/UnrealType.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// COLL_ prefix per the unity-build symbol-collision convention.
	constexpr int32 kCOLLErrorInvalidParams   = -32602;
	constexpr int32 kCOLLErrorInternal        = -32603;
	constexpr int32 kCOLLErrorObjectNotFound  = kMCPErrorObjectNotFound; // -32004

	void COLL_StampIds(const FMCPRequest& Request, FMCPResponse& Response)
	{
		Response.RequestId = Request.RequestId;
		Response.OriginalIdString = Request.OriginalIdString;
	}

	FMCPResponse COLL_MakeError(const FMCPRequest& Request, int32 Code, const FString& Message)
	{
		FMCPResponse R;
		COLL_StampIds(Request, R);
		R.bIsError = true;
		R.ErrorCode = Code;
		R.ErrorMessage = Message;
		return R;
	}

	FMCPResponse COLL_MakeSuccessObj(const FMCPRequest& Request, TSharedPtr<FJsonObject> Result)
	{
		FMCPResponse R;
		COLL_StampIds(Request, R);
		R.bIsError = false;
		R.Result = MakeShared<FJsonValueObject>(MoveTemp(Result));
		return R;
	}

	bool COLL_RequireStringField(const FMCPRequest& Request, const TCHAR* FieldName,
		FString& OutValue, FMCPResponse& OutError)
	{
		if (!Request.Args.IsValid())
		{
			OutError = COLL_MakeError(Request, kCOLLErrorInvalidParams, TEXT("missing args object"));
			return false;
		}
		if (!Request.Args->TryGetStringField(FieldName, OutValue) || OutValue.IsEmpty())
		{
			OutError = COLL_MakeError(Request, kCOLLErrorInvalidParams,
				FString::Printf(TEXT("missing required string field '%s'"), FieldName));
			return false;
		}
		return true;
	}

	/** Map ECollisionResponse → wire string. */
	const TCHAR* COLL_ResponseToString(ECollisionResponse Response)
	{
		switch (Response)
		{
			case ECR_Block:   return TEXT("Block");
			case ECR_Overlap: return TEXT("Overlap");
			case ECR_Ignore:  return TEXT("Ignore");
			default:          return TEXT("Unknown");
		}
	}

	/** Parse wire string → ECollisionResponse. Case-sensitive (matches editor Block/Overlap/Ignore). */
	bool COLL_ParseResponse(const FString& In, ECollisionResponse& Out)
	{
		if (In == TEXT("Block"))   { Out = ECR_Block;   return true; }
		if (In == TEXT("Overlap")) { Out = ECR_Overlap; return true; }
		if (In == TEXT("Ignore"))  { Out = ECR_Ignore;  return true; }
		return false;
	}

	/** Map ECollisionEnabled → wire string. Mirrors the engine's UMETA DisplayName entries. */
	const TCHAR* COLL_CollisionEnabledToString(ECollisionEnabled::Type Enabled)
	{
		switch (Enabled)
		{
			case ECollisionEnabled::NoCollision:       return TEXT("NoCollision");
			case ECollisionEnabled::QueryOnly:         return TEXT("QueryOnly");
			case ECollisionEnabled::PhysicsOnly:       return TEXT("PhysicsOnly");
			case ECollisionEnabled::QueryAndPhysics:   return TEXT("QueryAndPhysics");
			case ECollisionEnabled::ProbeOnly:         return TEXT("ProbeOnly");
			case ECollisionEnabled::QueryAndProbe:     return TEXT("QueryAndProbe");
			default:                                   return TEXT("Unknown");
		}
	}

	/**
	 * Walk channels 0..31 and emit per-index entries. Lambda receives (Index, DisplayName, bIsTrace).
	 * Skips empty display-name slots (channels with no registration).
	 */
	template <typename Fn>
	void COLL_ForEachChannel(const UCollisionProfile* CP, Fn&& Visitor)
	{
		check(CP);
		for (int32 Idx = 0; Idx < 32; ++Idx)
		{
			const FName DisplayName = CP->ReturnChannelNameFromContainerIndex(Idx);
			if (DisplayName == NAME_None) { continue; }
			const ECollisionChannel Channel = static_cast<ECollisionChannel>(Idx);
			const ETraceTypeQuery AsTrace = CP->ConvertToTraceType(Channel);
			const bool bIsTrace = (AsTrace != TraceTypeQuery_MAX);
			Visitor(Idx, DisplayName, bIsTrace);
		}
	}

	/**
	 * Look up the FArrayProperty for ``UCollisionProfile::Profiles`` (private but UPROPERTY-marked).
	 * Returns the property and a writable script-array helper pointed at the array's memory.
	 * Both pointers are valid as long as ``CP`` is valid (no copy is made).
	 */
	FArrayProperty* COLL_GetProfilesArrayProperty()
	{
		FArrayProperty* ArrProp = FindFProperty<FArrayProperty>(UCollisionProfile::StaticClass(),
			TEXT("Profiles"));
		return ArrProp;
	}

	/**
	 * Locate the FCollisionResponseTemplate entry in the live ``Profiles`` array whose Name matches.
	 * Returns a pointer into the live array memory (NOT a copy). Caller mutates in place.
	 * Returns nullptr if the named profile doesn't exist.
	 *
	 * **Lifetime contract.** The returned pointer is invalidated by any array growth/shrink on
	 * ``Profiles``. Within a single tool call we never mutate the array size — only the contents of
	 * one entry — so the pointer stays valid for the duration of the mutation.
	 */
	FCollisionResponseTemplate* COLL_FindLiveProfile(UCollisionProfile* CP, FName ProfileName)
	{
		check(CP);
		FArrayProperty* ArrProp = COLL_GetProfilesArrayProperty();
		if (!ArrProp) { return nullptr; }

		void* ArrayPtrAddr = ArrProp->ContainerPtrToValuePtr<void>(CP);
		FScriptArrayHelper Helper(ArrProp, ArrayPtrAddr);
		const int32 Num = Helper.Num();
		for (int32 i = 0; i < Num; ++i)
		{
			uint8* ElemMem = Helper.GetRawPtr(i);
			FCollisionResponseTemplate* Tpl = reinterpret_cast<FCollisionResponseTemplate*>(ElemMem);
			if (Tpl->Name == ProfileName)
			{
				return Tpl;
			}
		}
		return nullptr;
	}

	/**
	 * Regenerate ``Template.CustomResponses`` (the serialised form) from the live
	 * ``Template.ResponseToChannels`` container. Inlines the same logic as
	 * ``UCollisionProfile::SaveCustomResponses`` (which is private). Walks all 32 channels and
	 * emits only those whose response differs from the engine's ``DefaultResponseContainer``,
	 * gated by:
	 *   - Engine reserved channels (Index < ECC_EngineTraceChannel1) are always emittable.
	 *   - Custom channels are emittable only if the channel display name resolves to a
	 *     non-None FName via ``ReturnChannelNameFromContainerIndex`` (covers all registered game
	 *     channels — engine extension channels OR DefaultEngine.ini-assigned GameTraceChannel
	 *     names).
	 *
	 * This is the EXACT serialisation contract that
	 * ``TryUpdateDefaultConfigFile()`` will round-trip into the ``+Profiles=(...)`` ini entry.
	 */
	void COLL_RegenerateCustomResponses(const UCollisionProfile* CP, FCollisionResponseTemplate& Template)
	{
		check(CP);
		const FCollisionResponseContainer& DefaultContainer =
			FCollisionResponseContainer::GetDefaultResponseContainer();

		Template.CustomResponses.Reset();
		for (int32 Idx = 0; Idx < 32; ++Idx)
		{
			if (Template.ResponseToChannels.EnumArray[Idx] == DefaultContainer.EnumArray[Idx])
			{
				continue;
			}
			const FName ChannelDisplayName = CP->ReturnChannelNameFromContainerIndex(Idx);
			if (ChannelDisplayName == NAME_None) { continue; }
			// Engine-reserved channels (0..7 + 8..13) always emit; custom channels only when
			// registered (display name resolves). The display-name check above covers both —
			// unregistered indices yield NAME_None.
			Template.CustomResponses.Add(FResponseChannel(ChannelDisplayName,
				static_cast<ECollisionResponse>(Template.ResponseToChannels.EnumArray[Idx])));
		}
	}
} // namespace

namespace FCollisionTools
{

// ─── collision.list_channels ────────────────────────────────────────────────────────────────────
//
// Args:    (no args)
// Result:  { trace_channels: [{ index, name, display_name }],
//            object_channels: [{ index, name, display_name }] }
//
// Read-only — no PIE guard. Iterates 0..31 and partitions into trace vs object based on
// UCollisionProfile::ConvertToTraceType / ConvertToObjectType.
FMCPResponse Tool_ListChannels(const FMCPRequest& Request)
{
	check(IsInGameThread());

	const UCollisionProfile* CP = UCollisionProfile::Get();
	if (!CP)
	{
		return COLL_MakeError(Request, kCOLLErrorInternal,
			TEXT("UCollisionProfile::Get() returned null (Engine collision subsystem not initialised)"));
	}

	TArray<TSharedPtr<FJsonValue>> TraceArr;
	TArray<TSharedPtr<FJsonValue>> ObjectArr;
	COLL_ForEachChannel(CP, [&](int32 Index, FName DisplayName, bool bIsTrace)
	{
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetNumberField(TEXT("index"), Index);
		// Wire 'name' is the canonical display name (matches engine UI). 'display_name' is the
		// same value for forward-compat — future revisions may diverge if Epic re-introduces
		// per-channel metadata.
		Entry->SetStringField(TEXT("name"), DisplayName.ToString());
		Entry->SetStringField(TEXT("display_name"), DisplayName.ToString());
		if (bIsTrace)
		{
			TraceArr.Add(MakeShared<FJsonValueObject>(Entry));
		}
		else
		{
			ObjectArr.Add(MakeShared<FJsonValueObject>(Entry));
		}
	});

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetArrayField(TEXT("trace_channels"), TraceArr);
	Out->SetArrayField(TEXT("object_channels"), ObjectArr);
	return COLL_MakeSuccessObj(Request, Out);
}

// ─── collision.list_profiles ────────────────────────────────────────────────────────────────────
//
// Args:    (no args)
// Result:  { profiles: [{ name, collision_enabled, object_type, helper_description }] }
//
// Read-only — no PIE guard. Helper description is the engine-supplied HelpMessage (WITH_EDITORONLY_DATA;
// always present in editor builds where this tool runs).
FMCPResponse Tool_ListProfiles(const FMCPRequest& Request)
{
	check(IsInGameThread());

	const UCollisionProfile* CP = UCollisionProfile::Get();
	if (!CP)
	{
		return COLL_MakeError(Request, kCOLLErrorInternal,
			TEXT("UCollisionProfile::Get() returned null (Engine collision subsystem not initialised)"));
	}

	// GetProfileNames is static — populates a TArray<TSharedPtr<FName>>.
	TArray<TSharedPtr<FName>> ProfileNamePtrs;
	UCollisionProfile::GetProfileNames(ProfileNamePtrs);

	TArray<TSharedPtr<FJsonValue>> Arr;
	Arr.Reserve(ProfileNamePtrs.Num());
	for (const TSharedPtr<FName>& NamePtr : ProfileNamePtrs)
	{
		if (!NamePtr.IsValid()) { continue; }
		const FName ProfileName = *NamePtr;

		FCollisionResponseTemplate Tpl;
		if (!CP->GetProfileTemplate(ProfileName, Tpl)) { continue; }

		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), ProfileName.ToString());
		Entry->SetStringField(TEXT("collision_enabled"),
			COLL_CollisionEnabledToString(Tpl.CollisionEnabled.GetValue()));
		Entry->SetStringField(TEXT("object_type"), Tpl.ObjectTypeName.ToString());
#if WITH_EDITORONLY_DATA
		Entry->SetStringField(TEXT("helper_description"), Tpl.HelpMessage);
#else
		Entry->SetStringField(TEXT("helper_description"), FString());
#endif
		Arr.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetArrayField(TEXT("profiles"), Arr);
	return COLL_MakeSuccessObj(Request, Out);
}

// ─── collision.get_profile ──────────────────────────────────────────────────────────────────────
//
// Args:    { profile_name: string }
// Result:  { name, collision_enabled, object_type, helper_description,
//            response_to_channels: { channel_name: "Block"|"Overlap"|"Ignore" } }
//
// Read-only — no PIE guard. Iterates the live FCollisionResponseContainer for the profile and
// emits ONE entry per registered channel (skips unregistered slots).
FMCPResponse Tool_GetProfile(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString ProfileNameStr;
	FMCPResponse Err;
	if (!COLL_RequireStringField(Request, TEXT("profile_name"), ProfileNameStr, Err)) { return Err; }

	const UCollisionProfile* CP = UCollisionProfile::Get();
	if (!CP)
	{
		return COLL_MakeError(Request, kCOLLErrorInternal,
			TEXT("UCollisionProfile::Get() returned null (Engine collision subsystem not initialised)"));
	}

	const FName ProfileName(*ProfileNameStr);
	FCollisionResponseTemplate Tpl;
	if (!CP->GetProfileTemplate(ProfileName, Tpl))
	{
		return COLL_MakeError(Request, kCOLLErrorObjectNotFound,
			FString::Printf(TEXT("collision profile '%s' not found; use collision.list_profiles to enumerate"),
				*ProfileNameStr));
	}

	TSharedRef<FJsonObject> ResponsesObj = MakeShared<FJsonObject>();
	COLL_ForEachChannel(CP, [&](int32 Index, FName DisplayName, bool /*bIsTrace*/)
	{
		const ECollisionResponse Resp =
			static_cast<ECollisionResponse>(Tpl.ResponseToChannels.EnumArray[Index]);
		ResponsesObj->SetStringField(DisplayName.ToString(), COLL_ResponseToString(Resp));
	});

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("name"), ProfileName.ToString());
	Out->SetStringField(TEXT("collision_enabled"),
		COLL_CollisionEnabledToString(Tpl.CollisionEnabled.GetValue()));
	Out->SetStringField(TEXT("object_type"), Tpl.ObjectTypeName.ToString());
#if WITH_EDITORONLY_DATA
	Out->SetStringField(TEXT("helper_description"), Tpl.HelpMessage);
#else
	Out->SetStringField(TEXT("helper_description"), FString());
#endif
	Out->SetObjectField(TEXT("response_to_channels"), ResponsesObj);
	return COLL_MakeSuccessObj(Request, Out);
}

// ─── collision.set_profile_response ─────────────────────────────────────────────────────────────
//
// Args:    { profile_name: string, channel_name: string, response: "Block"|"Overlap"|"Ignore" }
// Result:  { updated: bool, prior_response: string, persisted_to_ini: bool }
//
// **NOT PIE-guarded** — collision config is editor-wide (UDeveloperSettings), not a per-PIE asset
// edit. The engine's own Project Settings dialog permits this mutation while PIE is active.
//
// Mutation flow:
//   1. Resolve profile by name via FindLiveProfile (reflective access to UCollisionProfile::Profiles).
//   2. Resolve channel name → container index via UCollisionProfile::ReturnContainerIndexFromChannelName
//      (case-sensitive, honours engine redirect map).
//   3. Wrap mutation in FScopedTransaction + UCollisionProfile::Modify (undo/redo support in editor).
//   4. Mutate ResponseToChannels via SetResponse, then regenerate CustomResponses (the serialised
//      form) via COLL_RegenerateCustomResponses.
//   5. TryUpdateDefaultConfigFile() persists the full Profiles UPROPERTY array to DefaultEngine.ini.
//   6. LoadProfileConfig(/*bForceInit=*/true) reloads runtime caches so subsequent body-instance
//      ReadConfig calls return the new response immediately.
FMCPResponse Tool_SetProfileResponse(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString ProfileNameStr, ChannelNameStr, ResponseStr;
	FMCPResponse Err;
	if (!COLL_RequireStringField(Request, TEXT("profile_name"), ProfileNameStr, Err)) { return Err; }
	if (!COLL_RequireStringField(Request, TEXT("channel_name"), ChannelNameStr, Err)) { return Err; }
	if (!COLL_RequireStringField(Request, TEXT("response"),     ResponseStr,    Err)) { return Err; }

	ECollisionResponse NewResponse;
	if (!COLL_ParseResponse(ResponseStr, NewResponse))
	{
		return COLL_MakeError(Request, kCOLLErrorInvalidParams,
			FString::Printf(TEXT("'response' must be 'Block', 'Overlap', or 'Ignore' (got '%s')"),
				*ResponseStr));
	}

	UCollisionProfile* CP = UCollisionProfile::Get();
	if (!CP)
	{
		return COLL_MakeError(Request, kCOLLErrorInternal,
			TEXT("UCollisionProfile::Get() returned null (Engine collision subsystem not initialised)"));
	}

	// Validate profile existence first (cheap, no reflection needed).
	const FName ProfileName(*ProfileNameStr);
	{
		FCollisionResponseTemplate ProbeTpl;
		if (!CP->GetProfileTemplate(ProfileName, ProbeTpl))
		{
			return COLL_MakeError(Request, kCOLLErrorObjectNotFound,
				FString::Printf(TEXT("collision profile '%s' not found; use collision.list_profiles to enumerate"),
					*ProfileNameStr));
		}
	}

	// Resolve channel name → container index. UCollisionProfile::ReturnContainerIndexFromChannelName
	// is declared without ENGINE_API (private/internal) so we cannot link it from a plugin module.
	// Use the exported ReverseReturnChannelNameFromContainerIndex helper instead — iterate the 32
	// possible channel slots and FName-compare to the caller's channel string.
	const FName ChannelDisplay(*ChannelNameStr);
	int32 ChannelIndex = INDEX_NONE;
	for (int32 i = 0; i < 32; ++i)
	{
		const FName CandidateName = CP->ReturnChannelNameFromContainerIndex(i);
		if (CandidateName.IsNone()) { continue; }
		if (CandidateName == ChannelDisplay)
		{
			ChannelIndex = i;
			break;
		}
	}
	if (ChannelIndex == INDEX_NONE)
	{
		return COLL_MakeError(Request, kCOLLErrorObjectNotFound,
			FString::Printf(TEXT("channel '%s' not registered; use collision.list_channels to enumerate"),
				*ChannelNameStr));
	}
	const ECollisionChannel Channel = static_cast<ECollisionChannel>(ChannelIndex);

	FScopedTransaction Transaction(LOCTEXT("MCP_Collision_SetProfileResponse",
		"Set Collision Profile Response"));
	CP->Modify();

	// Reflectively locate the live profile entry so we can mutate in place.
	FCollisionResponseTemplate* LiveTpl = COLL_FindLiveProfile(CP, ProfileName);
	if (!LiveTpl)
	{
		// Should never happen — GetProfileTemplate succeeded above. Defensive guard.
		return COLL_MakeError(Request, kCOLLErrorInternal,
			FString::Printf(TEXT("UCollisionProfile::Profiles array missing entry for '%s' after "
				"GetProfileTemplate succeeded (engine state inconsistent)"), *ProfileNameStr));
	}

	const ECollisionResponse PriorResponse =
		static_cast<ECollisionResponse>(LiveTpl->ResponseToChannels.EnumArray[ChannelIndex]);

	// Mutate the live profile's response container.
	LiveTpl->ResponseToChannels.SetResponse(Channel, NewResponse);

	// Regenerate the serialised CustomResponses array — this is what TryUpdateDefaultConfigFile
	// writes to disk. Without this regeneration, ResponseToChannels (which is non-serialised — see
	// the field's "not property serializable" comment in CollisionProfile.h) would silently revert
	// on the next LoadProfileConfig.
	COLL_RegenerateCustomResponses(CP, *LiveTpl);

	// Persist to DefaultEngine.ini. TryUpdateDefaultConfigFile walks all config-marked UPROPERTIES
	// on this UObject and writes them to the project's DefaultEngine.ini config-cascade entry.
	// Returns false on filesystem failure (read-only file, missing directory, locked by SCC, etc.).
	const bool bPersisted = CP->TryUpdateDefaultConfigFile();

	// Reload caches — without bForceInit=true, the engine's body-instance ReadConfig would still
	// see the old in-memory response container until the next save cycle.
	CP->LoadProfileConfig(/*bForceInit=*/true);

	if (!bPersisted)
	{
		// We've still mutated in-memory but the ini write failed. Surface the error so the caller
		// knows persistence didn't happen (the change will revert on editor restart).
		return COLL_MakeError(Request, kCOLLErrorInternal,
			FString::Printf(TEXT("in-memory profile updated, but TryUpdateDefaultConfigFile failed "
				"to write DefaultEngine.ini for profile '%s' / channel '%s' — check file permissions "
				"or source-control check-out"), *ProfileNameStr, *ChannelNameStr));
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("updated"), true);
	Out->SetStringField(TEXT("prior_response"), COLL_ResponseToString(PriorResponse));
	Out->SetBoolField(TEXT("persisted_to_ini"), bPersisted);
	return COLL_MakeSuccessObj(Request, Out);
}

// ─── Registration ─────────────────────────────────────────────────────────────────────────────
void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	RegisterTool(TEXT("collision.list_channels"),        &Tool_ListChannels,        /*Lane A*/ false);
	RegisterTool(TEXT("collision.list_profiles"),        &Tool_ListProfiles,        /*Lane A*/ false);
	RegisterTool(TEXT("collision.get_profile"),          &Tool_GetProfile,          /*Lane A*/ false);
	RegisterTool(TEXT("collision.set_profile_response"), &Tool_SetProfileResponse,  /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("Collision surface registered: 4 collision.* tools "
			 "(list_channels + list_profiles + get_profile + set_profile_response), all Lane A"));
}

} // namespace FCollisionTools

#include "MCPSurfaceRegistry.h"
MCP_REGISTER_SURFACE(CollisionTools, &FCollisionTools::Register)

#undef LOCTEXT_NAMESPACE
