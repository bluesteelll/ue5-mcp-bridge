// Copyright FatumGame. All Rights Reserved.

#include "AIBlackboardTools.h"

#include "FMCPDispatchQueue.h"
#include "MCPAssetLoader.h"
#include "MCPJsonBuilder.h"
#include "MCPMutatorScope.h"
#include "MCPToolHelpers.h"
#include "MCPAssetFactory.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPActorPathUtils.h"
#include "Utils/MCPAssetPathUtils.h"

#include "AIController.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Bool.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Class.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Enum.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Float.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Int.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Name.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Rotator.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_String.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Vector.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Class.h"
#include "UObject/Object.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// AIBB_ prefix per the unity-build symbol-collision pattern (see PackageTools / PhysicsTools
	// notes — every Tools/*.cpp must use a unique prefix because the plugin builds unity-style).
	constexpr int32 kAIBBErrorInternal        = -32603;
	constexpr int32 kAIBBErrorObjectNotFound  = kMCPErrorObjectNotFound;     // -32004
	constexpr int32 kAIBBErrorKeyNotFound     = kMCPErrorPropertyNotFound;   // -32005
	// Note: -32010 InvalidPath is NOT raised directly from this surface — malformed actor paths
	// surface as -32004 ObjectNotFound with the parser error string embedded in the message
	// (FMCPActorPathUtils::ResolveActor returns null + sets OutError on both not-found and
	// path-malformed cases without a separate signal). Listed in the header tool roster for
	// completeness; reserved if a future arg path needs it.

	// Wire type-string for unknown subclasses retains the "BlackboardKeyType_" prefix so the
	// caller can identify the raw subclass. Recognised subclasses strip it.
	const TCHAR* const kAIBBTypePrefix = TEXT("BlackboardKeyType_");

	/**
	 * Resolve the actor (Pawn OR AIController) referenced by ``actor_path`` and return the
	 * UBlackboardComponent attached to its AIController.
	 *
	 * Resolution order matches the surface contract: APawn paths walk Pawn->GetController() to
	 * find the controller; AAIController paths are used directly. Both paths converge on
	 * AIController->GetBlackboardComponent(). Any failure along the chain produces a populated
	 * OutError response — caller forwards it verbatim.
	 *
	 * GAME THREAD ONLY (FMCPActorPathUtils::ResolveActor iterates ULevel::Actors).
	 */
	UBlackboardComponent* AIBB_ResolveBlackboard(
		const FMCPRequest& Request,
		const FString& ActorPath,
		FMCPResponse& OutError)
	{
		bool bAmbiguous = false;
		FString AmbiguityHint, ResolveErr;
		AActor* Actor = FMCPActorPathUtils::ResolveActor(
			ActorPath, /*bRejectPIE*/ false, bAmbiguous, AmbiguityHint, ResolveErr);
		if (!Actor)
		{
			// Disambiguate "not found" from "path malformed" via the underlying error text. The
			// parser sets bIsFullPath/ActorName fields in OutParts but the friendlier signal is in
			// ResolveErr — we forward it raw so the caller sees the exact failure.
			OutError = FMCPToolHelpers::MakeError(Request, kAIBBErrorObjectNotFound,
				bAmbiguous
					? FString::Printf(TEXT("actor '%s' is ambiguous; candidates: %s"),
						*ActorPath, *AmbiguityHint)
					: FString::Printf(TEXT("actor '%s' not found: %s"), *ActorPath, *ResolveErr));
			return nullptr;
		}

		// Resolve to AIController: either the actor IS one, or it's a Pawn we walk to its controller.
		AAIController* AIC = Cast<AAIController>(Actor);
		if (!AIC)
		{
			APawn* Pawn = Cast<APawn>(Actor);
			if (Pawn)
			{
				AIC = Cast<AAIController>(Pawn->GetController());
			}
		}
		if (!AIC)
		{
			OutError = FMCPToolHelpers::MakeError(Request, kAIBBErrorObjectNotFound,
				FString::Printf(TEXT("actor '%s' has no AIController (expected APawn with AAIController, "
					"or a direct AAIController path)"), *ActorPath));
			return nullptr;
		}

		UBlackboardComponent* BB = AIC->GetBlackboardComponent();
		if (!BB)
		{
			OutError = FMCPToolHelpers::MakeError(Request, kAIBBErrorObjectNotFound,
				FString::Printf(TEXT("AIController on actor '%s' has no UBlackboardComponent "
					"(behaviour tree not running, or no blackboard asset assigned)"), *ActorPath));
			return nullptr;
		}

		return BB;
	}

	/**
	 * Map a UBlackboardKeyType subclass to the friendly wire-format type-name. Recognised types
	 * strip the ``BlackboardKeyType_`` prefix; unknown subclasses return the raw class name
	 * (with prefix retained) so the caller can branch on it.
	 *
	 * Returns FName::ToString() of either the friendly suffix or the raw class FName. Callers
	 * compare against the recognised set with ``IsA<...>()`` before falling here for unknowns.
	 */
	FString AIBB_KeyTypeToFriendlyName(const UBlackboardKeyType* KeyType)
	{
		if (!KeyType)
		{
			return TEXT("None");
		}
		const UClass* Cls = KeyType->GetClass();
		if (!Cls)
		{
			return TEXT("None");
		}
		const FString RawName = Cls->GetName();
		if (RawName.StartsWith(kAIBBTypePrefix, ESearchCase::CaseSensitive))
		{
			return RawName.RightChop(FCString::Strlen(kAIBBTypePrefix));
		}
		// Unknown subclass (Struct, NativeEnum, SOClaimHandle, future entries) — return the raw
		// class name so callers can identify it. The leading "BlackboardKeyType_" prefix is
		// intentionally PRESERVED here as a discriminator from the friendly names.
		return RawName;
	}

	/**
	 * Cheap one-line summary for the ``value_repr`` field of ai.bb.list_keys. Uses the engine's
	 * own DescribeValue() if a key-type-specific override is available; otherwise falls back to
	 * a typed get + string conversion. We never crash — empty string is acceptable.
	 *
	 * NOTE: DescribeValue is virtual on UBlackboardKeyType. Some subclasses (Struct) format as
	 * binary blob; that's the engine's behaviour, not ours to override.
	 */
	FString AIBB_DescribeValue(const UBlackboardComponent& BB, const UBlackboardKeyType* KeyType, const FName& KeyName)
	{
		if (!KeyType)
		{
			return FString();
		}
		const uint8* RawData = BB.GetKeyRawData(KeyName);
		if (!RawData)
		{
			return FString();
		}
		// DescribeValue is the engine's own one-line dump — works across every key type without
		// us having to hand-write a fallback per subclass.
		return KeyType->WrappedDescribeValue(BB, RawData);
	}

	/** Convert a 3-component JSON array (or JSON object {x,y,z}) into an FVector. */
	bool AIBB_ReadVectorValue(const TSharedPtr<FJsonValue>& Value, FVector& OutV, FString& OutError)
	{
		if (!Value.IsValid())
		{
			OutError = TEXT("vector value missing");
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Value->TryGetArray(Arr) && Arr)
		{
			if (Arr->Num() != 3)
			{
				OutError = FString::Printf(TEXT("vector array must have 3 entries; got %d"), Arr->Num());
				return false;
			}
			double X = 0.0, Y = 0.0, Z = 0.0;
			if (!(*Arr)[0]->TryGetNumber(X) || !(*Arr)[1]->TryGetNumber(Y) || !(*Arr)[2]->TryGetNumber(Z))
			{
				OutError = TEXT("vector array entries must all be numbers");
				return false;
			}
			OutV = FVector(X, Y, Z);
			return true;
		}
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		if (Value->TryGetObject(Obj) && Obj && Obj->IsValid())
		{
			double X = 0.0, Y = 0.0, Z = 0.0;
			if (!(*Obj)->TryGetNumberField(TEXT("x"), X)
				|| !(*Obj)->TryGetNumberField(TEXT("y"), Y)
				|| !(*Obj)->TryGetNumberField(TEXT("z"), Z))
			{
				OutError = TEXT("vector object must have numeric fields {x, y, z}");
				return false;
			}
			OutV = FVector(X, Y, Z);
			return true;
		}
		OutError = TEXT("vector value must be a 3-number array OR an object with {x,y,z} fields");
		return false;
	}

	/** Convert a 3-component JSON array (or JSON object {pitch,yaw,roll}) into an FRotator. */
	bool AIBB_ReadRotatorValue(const TSharedPtr<FJsonValue>& Value, FRotator& OutR, FString& OutError)
	{
		if (!Value.IsValid())
		{
			OutError = TEXT("rotator value missing");
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Value->TryGetArray(Arr) && Arr)
		{
			if (Arr->Num() != 3)
			{
				OutError = FString::Printf(TEXT("rotator array must have 3 entries; got %d"), Arr->Num());
				return false;
			}
			double P = 0.0, Y = 0.0, R = 0.0;
			if (!(*Arr)[0]->TryGetNumber(P) || !(*Arr)[1]->TryGetNumber(Y) || !(*Arr)[2]->TryGetNumber(R))
			{
				OutError = TEXT("rotator array entries must all be numbers");
				return false;
			}
			OutR = FRotator(P, Y, R);
			return true;
		}
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		if (Value->TryGetObject(Obj) && Obj && Obj->IsValid())
		{
			double P = 0.0, Y = 0.0, R = 0.0;
			if (!(*Obj)->TryGetNumberField(TEXT("pitch"), P)
				|| !(*Obj)->TryGetNumberField(TEXT("yaw"), Y)
				|| !(*Obj)->TryGetNumberField(TEXT("roll"), R))
			{
				OutError = TEXT("rotator object must have numeric fields {pitch, yaw, roll}");
				return false;
			}
			OutR = FRotator(P, Y, R);
			return true;
		}
		OutError = TEXT("rotator value must be a 3-number array [pitch,yaw,roll] OR object {pitch,yaw,roll}");
		return false;
	}

	/** Serialize an FVector as a 3-number JSON array (matching the engine's wire convention). */
	TArray<TSharedPtr<FJsonValue>> AIBB_VectorToArray(const FVector& V)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Add(MakeShared<FJsonValueNumber>(V.X));
		Arr.Add(MakeShared<FJsonValueNumber>(V.Y));
		Arr.Add(MakeShared<FJsonValueNumber>(V.Z));
		return Arr;
	}

	/** Serialize an FRotator as a 3-number JSON array (pitch, yaw, roll). */
	TArray<TSharedPtr<FJsonValue>> AIBB_RotatorToArray(const FRotator& R)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Add(MakeShared<FJsonValueNumber>(R.Pitch));
		Arr.Add(MakeShared<FJsonValueNumber>(R.Yaw));
		Arr.Add(MakeShared<FJsonValueNumber>(R.Roll));
		return Arr;
	}

	/**
	 * Build the typed JSON value for a blackboard key, dispatched on the UBlackboardKeyType
	 * subclass. Returns null FJsonValue (FJsonValueNull) when the key type is unrecognised and
	 * the caller did not request the "value" field — the descriptive string is populated into
	 * ``OutDescriptiveFallback`` for use in the list_keys ``value_repr`` field, since the engine
	 * still exposes a one-line dump for unknown types.
	 *
	 * The split is intentional: get_value's typed result MUST be a discriminated JSON value the
	 * caller can switch on; list_keys's value_repr is human-readable text. Unknown types must
	 * never produce structurally-misleading JSON (e.g. nesting a stringified-struct under a
	 * "value" field that the caller treats as typed) — so we return FJsonValueNull and let
	 * get_value surface -32603 with a helpful message instead.
	 */
	TSharedPtr<FJsonValue> AIBB_TypedValueToJson(
		const UBlackboardComponent& BB,
		const UBlackboardKeyType* KeyType,
		const FName& KeyName)
	{
		if (!KeyType)
		{
			return MakeShared<FJsonValueNull>();
		}
		if (KeyType->IsA<UBlackboardKeyType_Bool>())
		{
			return MakeShared<FJsonValueBoolean>(BB.GetValueAsBool(KeyName));
		}
		if (KeyType->IsA<UBlackboardKeyType_Int>())
		{
			return MakeShared<FJsonValueNumber>(static_cast<double>(BB.GetValueAsInt(KeyName)));
		}
		if (KeyType->IsA<UBlackboardKeyType_Float>())
		{
			return MakeShared<FJsonValueNumber>(static_cast<double>(BB.GetValueAsFloat(KeyName)));
		}
		if (KeyType->IsA<UBlackboardKeyType_String>())
		{
			return MakeShared<FJsonValueString>(BB.GetValueAsString(KeyName));
		}
		if (KeyType->IsA<UBlackboardKeyType_Name>())
		{
			return MakeShared<FJsonValueString>(BB.GetValueAsName(KeyName).ToString());
		}
		if (KeyType->IsA<UBlackboardKeyType_Vector>())
		{
			return MakeShared<FJsonValueArray>(AIBB_VectorToArray(BB.GetValueAsVector(KeyName)));
		}
		if (KeyType->IsA<UBlackboardKeyType_Rotator>())
		{
			return MakeShared<FJsonValueArray>(AIBB_RotatorToArray(BB.GetValueAsRotator(KeyName)));
		}
		if (KeyType->IsA<UBlackboardKeyType_Object>())
		{
			// Avoid a derived/base ternary mismatch — TSharedRef<FJsonValueString> vs
			// TSharedRef<FJsonValueNull> cannot deduce a common type in the ?: expression. Use
			// explicit branches so the implicit conversion to TSharedPtr<FJsonValue> happens
			// at the return site for each branch independently.
			UObject* Obj = BB.GetValueAsObject(KeyName);
			if (Obj) { return MakeShared<FJsonValueString>(Obj->GetPathName()); }
			return MakeShared<FJsonValueNull>();
		}
		if (KeyType->IsA<UBlackboardKeyType_Class>())
		{
			UClass* Cls = BB.GetValueAsClass(KeyName);
			if (Cls) { return MakeShared<FJsonValueString>(Cls->GetPathName()); }
			return MakeShared<FJsonValueNull>();
		}
		if (KeyType->IsA<UBlackboardKeyType_Enum>())
		{
			return MakeShared<FJsonValueNumber>(static_cast<double>(BB.GetValueAsEnum(KeyName)));
		}
		// Unknown subclass — caller must decide how to surface. We return null here so the
		// caller can branch on it (get_value adds a description string; list_keys ignores).
		return MakeShared<FJsonValueNull>();
	}

	/**
	 * Apply a typed JSON value to a blackboard key, dispatched on the UBlackboardKeyType
	 * subclass. Returns false + populated ``OutError`` on type mismatch (e.g. trying to assign
	 * a string to a Bool key). Successful writes consult the typed setter for the subclass.
	 *
	 * Object/Class writes accept EITHER a string path (LoadObject/StaticLoadClass) OR the
	 * literal JSON null (clears the value). Object writes do NOT validate against the
	 * blackboard's BaseClass meta — UE's own SetValueAsObject does no such check either; the
	 * caller assumes the responsibility (matches engine semantics, no silent constraint).
	 *
	 * Class writes only accept the JSON null OR a valid class_path that resolves to a UClass*.
	 * Object paths that resolve to non-UClass UObjects are rejected with -32602.
	 */
	bool AIBB_SetTypedValue(
		UBlackboardComponent& BB,
		const UBlackboardKeyType* KeyType,
		const FName& KeyName,
		const TSharedPtr<FJsonValue>& Value,
		FString& OutError)
	{
		if (!KeyType)
		{
			OutError = TEXT("blackboard key has no key-type instance (corrupt asset?)");
			return false;
		}
		if (!Value.IsValid())
		{
			OutError = TEXT("missing 'value' field");
			return false;
		}

		if (KeyType->IsA<UBlackboardKeyType_Bool>())
		{
			bool B = false;
			if (!Value->TryGetBool(B))
			{
				OutError = TEXT("Bool key requires a boolean value");
				return false;
			}
			BB.SetValueAsBool(KeyName, B);
			return true;
		}
		if (KeyType->IsA<UBlackboardKeyType_Int>())
		{
			// TryGetNumber accepts both integer and float JSON literals; we cast to int32. JSON
			// numbers don't carry a discriminator so we accept "5" or "5.0" equivalently.
			double N = 0.0;
			if (!Value->TryGetNumber(N))
			{
				OutError = TEXT("Int key requires a numeric value");
				return false;
			}
			BB.SetValueAsInt(KeyName, static_cast<int32>(N));
			return true;
		}
		if (KeyType->IsA<UBlackboardKeyType_Float>())
		{
			double N = 0.0;
			if (!Value->TryGetNumber(N))
			{
				OutError = TEXT("Float key requires a numeric value");
				return false;
			}
			BB.SetValueAsFloat(KeyName, static_cast<float>(N));
			return true;
		}
		if (KeyType->IsA<UBlackboardKeyType_String>())
		{
			FString S;
			if (!Value->TryGetString(S))
			{
				OutError = TEXT("String key requires a string value");
				return false;
			}
			BB.SetValueAsString(KeyName, S);
			return true;
		}
		if (KeyType->IsA<UBlackboardKeyType_Name>())
		{
			FString S;
			if (!Value->TryGetString(S))
			{
				OutError = TEXT("Name key requires a string value");
				return false;
			}
			BB.SetValueAsName(KeyName, FName(*S));
			return true;
		}
		if (KeyType->IsA<UBlackboardKeyType_Vector>())
		{
			FVector V;
			if (!AIBB_ReadVectorValue(Value, V, OutError))
			{
				return false;
			}
			BB.SetValueAsVector(KeyName, V);
			return true;
		}
		if (KeyType->IsA<UBlackboardKeyType_Rotator>())
		{
			FRotator R;
			if (!AIBB_ReadRotatorValue(Value, R, OutError))
			{
				return false;
			}
			BB.SetValueAsRotator(KeyName, R);
			return true;
		}
		if (KeyType->IsA<UBlackboardKeyType_Object>())
		{
			// JSON null clears the slot; string is treated as a UObject path.
			if (Value->IsNull())
			{
				BB.SetValueAsObject(KeyName, nullptr);
				return true;
			}
			FString Path;
			if (!Value->TryGetString(Path))
			{
				OutError = TEXT("Object key requires a string path OR null");
				return false;
			}
			UObject* Obj = StaticLoadObject(UObject::StaticClass(), nullptr, *Path);
			if (!Obj)
			{
				OutError = FString::Printf(TEXT("Object key value '%s' did not resolve to a UObject"), *Path);
				return false;
			}
			BB.SetValueAsObject(KeyName, Obj);
			return true;
		}
		if (KeyType->IsA<UBlackboardKeyType_Class>())
		{
			if (Value->IsNull())
			{
				BB.SetValueAsClass(KeyName, nullptr);
				return true;
			}
			FString Path;
			if (!Value->TryGetString(Path))
			{
				OutError = TEXT("Class key requires a string class_path OR null");
				return false;
			}
			UClass* Cls = StaticLoadClass(UObject::StaticClass(), nullptr, *Path);
			if (!Cls)
			{
				OutError = FString::Printf(TEXT("Class key value '%s' did not resolve to a UClass"), *Path);
				return false;
			}
			BB.SetValueAsClass(KeyName, Cls);
			return true;
		}
		if (KeyType->IsA<UBlackboardKeyType_Enum>())
		{
			// Enum keys store a uint8; AI agents pass the numeric value directly. We do NOT
			// attempt enum-name resolution here — UE's EnumType pointer would let us look up by
			// string, but the API surface stays minimal: numeric in, numeric out.
			double N = 0.0;
			if (!Value->TryGetNumber(N))
			{
				OutError = TEXT("Enum key requires a numeric value (uint8 ordinal)");
				return false;
			}
			if (N < 0.0 || N > 255.0)
			{
				OutError = FString::Printf(TEXT("Enum value %g out of uint8 range [0, 255]"), N);
				return false;
			}
			BB.SetValueAsEnum(KeyName, static_cast<uint8>(N));
			return true;
		}

		// Unknown subclass (Struct, NativeEnum, SOClaimHandle, future) — we don't have a typed
		// setter for it. Reject with a descriptive error so the caller knows their key type is
		// unsupported by this surface.
		OutError = FString::Printf(
			TEXT("blackboard key type '%s' not supported by ai.bb.set_value (only Bool/Int/Float/"
				 "String/Name/Vector/Rotator/Object/Class/Enum are supported)"),
			*AIBB_KeyTypeToFriendlyName(KeyType));
		return false;
	}

	// ─── Wave P authoring helpers ──────────────────────────────────────────────────────────────────

	/**
	 * Wave P: map a short key-type name ("Bool", "Int", "Float", "String", "Name", "Vector",
	 * "Rotator", "Class", "Object", "Enum") to the UBlackboardKeyType_X UClass. Returns nullptr for
	 * unknown short names — caller surfaces -32602. The accepted set mirrors the friendly-name set
	 * used by ai.bb.list_keys / ai.bb.get_value so the round-trip is symmetric.
	 */
	UClass* AIBB_KeyTypeShortNameToClass(const FString& ShortName)
	{
		if (ShortName.Equals(TEXT("Bool"),    ESearchCase::IgnoreCase)) { return UBlackboardKeyType_Bool   ::StaticClass(); }
		if (ShortName.Equals(TEXT("Int"),     ESearchCase::IgnoreCase)) { return UBlackboardKeyType_Int    ::StaticClass(); }
		if (ShortName.Equals(TEXT("Float"),   ESearchCase::IgnoreCase)) { return UBlackboardKeyType_Float  ::StaticClass(); }
		if (ShortName.Equals(TEXT("String"),  ESearchCase::IgnoreCase)) { return UBlackboardKeyType_String ::StaticClass(); }
		if (ShortName.Equals(TEXT("Name"),    ESearchCase::IgnoreCase)) { return UBlackboardKeyType_Name   ::StaticClass(); }
		if (ShortName.Equals(TEXT("Vector"),  ESearchCase::IgnoreCase)) { return UBlackboardKeyType_Vector ::StaticClass(); }
		if (ShortName.Equals(TEXT("Rotator"), ESearchCase::IgnoreCase)) { return UBlackboardKeyType_Rotator::StaticClass(); }
		if (ShortName.Equals(TEXT("Class"),   ESearchCase::IgnoreCase)) { return UBlackboardKeyType_Class  ::StaticClass(); }
		if (ShortName.Equals(TEXT("Object"),  ESearchCase::IgnoreCase)) { return UBlackboardKeyType_Object ::StaticClass(); }
		if (ShortName.Equals(TEXT("Enum"),    ESearchCase::IgnoreCase)) { return UBlackboardKeyType_Enum   ::StaticClass(); }
		return nullptr;
	}

} // namespace

namespace FAIBlackboardTools
{

// ─── ai.bb.list_keys ───────────────────────────────────────────────────────────────────────────
//
// Args:    { actor_path: string }
// Result:  { keys: [{ name, type, value_repr }] }
//
// Walks the BlackboardComponent's BlackboardAsset Keys[] array. Friendly type name
// (BlackboardKeyType_ prefix stripped); ``value_repr`` is the engine's DescribeValue() one-line
// dump (best-effort, never crashes). Read-only — no PIE guard, no transaction.
//
// Returns -32603 InternalError if the AIController has no BlackboardAsset — that signals the
// behaviour tree was never initialised (we already verified the BlackboardComponent itself
// exists via AIBB_ResolveBlackboard).
FMCPResponse Tool_ListKeys(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString ActorPath;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("actor_path"), ActorPath, Err)) { return Err; }

	UBlackboardComponent* BB = AIBB_ResolveBlackboard(Request, ActorPath, Err);
	if (!BB) { return Err; }

	UBlackboardData* Data = BB->GetBlackboardAsset();
	if (!Data)
	{
		return FMCPToolHelpers::MakeError(Request, kAIBBErrorInternal,
			FString::Printf(TEXT("blackboard component on actor '%s' has no BlackboardAsset "
				"(behaviour tree never initialised?)"), *ActorPath));
	}

	TArray<TSharedPtr<FJsonValue>> KeysArr;
	KeysArr.Reserve(Data->Keys.Num());
	for (const FBlackboardEntry& Entry : Data->Keys)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Entry.EntryName.ToString());
		Obj->SetStringField(TEXT("type"), AIBB_KeyTypeToFriendlyName(Entry.KeyType));
		Obj->SetStringField(TEXT("value_repr"), AIBB_DescribeValue(*BB, Entry.KeyType, Entry.EntryName));
		KeysArr.Add(MakeShared<FJsonValueObject>(Obj));
	}

	const int32 TotalKeys = KeysArr.Num();
	return FMCPJsonBuilder()
		.Arr(TEXT("keys"), MoveTemp(KeysArr))
		.Num(TEXT("total"), TotalKeys)
		.Str(TEXT("actor_path"), ActorPath)
		.BuildSuccess(Request);
}

// ─── ai.bb.get_value ───────────────────────────────────────────────────────────────────────────
//
// Args:    { actor_path: string, key_name: string }
// Result:  { type: string, value: <typed>, name: string }
//
// Typed read of a single key. Read-only, no PIE guard. The ``value`` field's JSON type matches
// the blackboard key type (Bool->bool, Int->int, Float->number, String/Name->string,
// Vector->[x,y,z], Rotator->[pitch,yaw,roll], Object/Class->path-string-or-null, Enum->int).
// For unknown subclasses (Struct, NativeEnum, SOClaimHandle, future) the ``value`` field is
// the descriptive-string fallback under a ``value_repr`` field instead and ``value`` is null —
// AI callers can branch on the ``type`` field to detect the unsupported case.
//
// Errors: -32004 actor/AIC/BB missing, -32005 key not on BB, -32602 missing required arg.
FMCPResponse Tool_GetValue(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString ActorPath;
	FString KeyName;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("actor_path"), ActorPath, Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("key_name"), KeyName, Err)) { return Err; }

	UBlackboardComponent* BB = AIBB_ResolveBlackboard(Request, ActorPath, Err);
	if (!BB) { return Err; }

	// Validate the key exists on the blackboard asset before issuing the typed getter (which
	// returns a default-constructed value for missing keys — silent data corruption otherwise).
	const FName KeyFName(*KeyName);
	const FBlackboard::FKey KeyId = BB->GetKeyID(KeyFName);
	if (KeyId == FBlackboard::InvalidKey)
	{
		return FMCPToolHelpers::MakeError(Request, kAIBBErrorKeyNotFound,
			FString::Printf(TEXT("blackboard on actor '%s' has no key '%s'"), *ActorPath, *KeyName));
	}

	// Look up the key's UBlackboardKeyType subclass via the asset. We need the concrete instance
	// (not just the TSubclassOf) to dispatch IsA<>() checks below — Data->Keys[i].KeyType is the
	// stored instance.
	UBlackboardData* Data = BB->GetBlackboardAsset();
	check(Data); // ResolveBlackboard guarantees BB exists; the BB's asset must exist too — if
	             // this trips, the engine has corrupt state and we'd rather assert than silently
	             // return junk.
	const UBlackboardKeyType* KeyType = nullptr;
	for (const FBlackboardEntry& Entry : Data->Keys)
	{
		if (Entry.EntryName == KeyFName)
		{
			KeyType = Entry.KeyType;
			break;
		}
	}
	if (!KeyType)
	{
		// Shouldn't happen — GetKeyID returned valid, but the asset's Keys[] doesn't contain it.
		// Possible if the BB inherits from a parent asset (ParentKeys[]) — fall back there.
		for (const FBlackboardEntry& Entry : Data->ParentKeys)
		{
			if (Entry.EntryName == KeyFName)
			{
				KeyType = Entry.KeyType;
				break;
			}
		}
	}
	if (!KeyType)
	{
		return FMCPToolHelpers::MakeError(Request, kAIBBErrorInternal,
			FString::Printf(TEXT("blackboard on actor '%s' has key '%s' in component but no entry in "
				"asset Keys[] or ParentKeys[] (corrupt asset?)"), *ActorPath, *KeyName));
	}

	const FString TypeName = AIBB_KeyTypeToFriendlyName(KeyType);
	TSharedPtr<FJsonValue> TypedValue = AIBB_TypedValueToJson(*BB, KeyType, KeyFName);

	// Detect unsupported subclasses: AIBB_TypedValueToJson returns FJsonValueNull when it didn't
	// match any IsA<>() branch. Distinguish from a genuine null Object/Class value by checking
	// the type name — if it still has the BlackboardKeyType_ prefix it's an unrecognised type.
	const bool bUnsupportedType = TypeName.StartsWith(kAIBBTypePrefix, ESearchCase::CaseSensitive);
	return FMCPJsonBuilder()
		.Str(TEXT("name"), KeyName)
		.Str(TEXT("type"), TypeName)
		.Str(TEXT("actor_path"), ActorPath)
		.If(bUnsupportedType, [&](FMCPJsonBuilder& B)
		{
			B.Null(TEXT("value"))
			 .Str(TEXT("value_repr"), AIBB_DescribeValue(*BB, KeyType, KeyFName));
		})
		.If(!bUnsupportedType, [&](FMCPJsonBuilder& B)
		{
			B.Field(TEXT("value"), TypedValue.ToSharedRef());
		})
		.BuildSuccess(Request);
}

// ─── ai.bb.set_value ───────────────────────────────────────────────────────────────────────────
//
// Args:    { actor_path: string, key_name: string, value: <typed> }
// Result:  { set: bool, prior_value: <typed>, type: string, name: string }
//
// Typed write of a single key. PIE-SAFE — blackboards are runtime state, exactly what this tool
// exists to mutate during runtime testing. Value is validated against the key's UBlackboardKeyType
// before being applied; type mismatch -> -32602 with a descriptive message.
//
// ``prior_value`` is captured by routing through the same JSON-value pipeline as get_value
// BEFORE the write fires. For unsupported subclasses (Struct/NativeEnum/SOClaimHandle/...) the
// write rejects upfront with -32602 — we don't write what we cannot also read back.
//
// Errors: -32004 actor/AIC/BB missing, -32005 key not on BB, -32602 missing arg / type mismatch,
//         -32603 corrupt blackboard asset.
FMCPResponse Tool_SetValue(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString ActorPath;
	FString KeyName;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("actor_path"), ActorPath, Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("key_name"), KeyName, Err)) { return Err; }

	// "value" must be present even when its JSON type is null (callers explicitly clearing an
	// Object/Class key). HasField + GetField distinguishes "absent" (-32602) from "present null".
	if (!Request.Args.IsValid() || !Request.Args->HasField(TEXT("value")))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			TEXT("missing required field 'value' (use JSON null to clear Object/Class keys)"));
	}
	const TSharedPtr<FJsonValue> ValueField = Request.Args->TryGetField(TEXT("value"));
	if (!ValueField.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			TEXT("required field 'value' could not be parsed"));
	}

	UBlackboardComponent* BB = AIBB_ResolveBlackboard(Request, ActorPath, Err);
	if (!BB) { return Err; }

	const FName KeyFName(*KeyName);
	const FBlackboard::FKey KeyId = BB->GetKeyID(KeyFName);
	if (KeyId == FBlackboard::InvalidKey)
	{
		return FMCPToolHelpers::MakeError(Request, kAIBBErrorKeyNotFound,
			FString::Printf(TEXT("blackboard on actor '%s' has no key '%s'"), *ActorPath, *KeyName));
	}

	UBlackboardData* Data = BB->GetBlackboardAsset();
	check(Data);
	const UBlackboardKeyType* KeyType = nullptr;
	for (const FBlackboardEntry& Entry : Data->Keys)
	{
		if (Entry.EntryName == KeyFName) { KeyType = Entry.KeyType; break; }
	}
	if (!KeyType)
	{
		for (const FBlackboardEntry& Entry : Data->ParentKeys)
		{
			if (Entry.EntryName == KeyFName) { KeyType = Entry.KeyType; break; }
		}
	}
	if (!KeyType)
	{
		return FMCPToolHelpers::MakeError(Request, kAIBBErrorInternal,
			FString::Printf(TEXT("blackboard on actor '%s' has key '%s' but no key-type entry in "
				"asset (corrupt asset?)"), *ActorPath, *KeyName));
	}

	const FString TypeName = AIBB_KeyTypeToFriendlyName(KeyType);

	// Capture prior value via the same typed pipeline as get_value. Unsupported subclasses fall
	// through to a null with a value_repr field — the write below will reject the same case with
	// -32602 anyway, so this asymmetry never reaches the wire.
	TSharedPtr<FJsonValue> PriorValue = AIBB_TypedValueToJson(*BB, KeyType, KeyFName);
	FString PriorRepr = AIBB_DescribeValue(*BB, KeyType, KeyFName);

	FString SetErr;
	if (!AIBB_SetTypedValue(*BB, KeyType, KeyFName, ValueField, SetErr))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			FString::Printf(TEXT("set rejected for key '%s' (type '%s'): %s"),
				*KeyName, *TypeName, *SetErr));
	}

	const TSharedRef<FJsonValue> PriorValueRef = PriorValue.IsValid()
		? PriorValue.ToSharedRef()
		: StaticCastSharedRef<FJsonValue>(MakeShared<FJsonValueNull>());
	return FMCPJsonBuilder()
		.Bool(TEXT("set"), true)
		.Str(TEXT("name"), KeyName)
		.Str(TEXT("type"), TypeName)
		.Str(TEXT("actor_path"), ActorPath)
		.Field(TEXT("prior_value"), PriorValueRef)
		.Str(TEXT("prior_value_repr"), PriorRepr)
		.BuildSuccess(Request);
}

// ─── ai.bb.create_asset (Wave P) ───────────────────────────────────────────────────────────────
//
// Args:    { path: string, parent_bb?: string }
// Result:  { asset_path, has_parent }
//
// Creates a new UBlackboardData asset at the supplied /Game/... path. Optional ``parent_bb``
// path sets the inheritance link (BB->Parent) so child keys merge with parent keys at runtime.
//
// Pattern: CreatePackage + NewObject<UBlackboardData> + FAssetRegistryModule::AssetCreated +
// Scope.DirtyPackage. Caller saves explicitly via cb.save / asset.save_loaded (not auto-saved
// here — matches anim.create_montage / asset.create_data_asset convention).
//
// Errors:
//   -32010 InvalidPath        path malformed / unknown mount
//   -32014 PathInUse          path already exists on disk
//   -32004 ObjectNotFound     parent_bb supplied but not loadable / wrong class
//   -32027 PIEActive          PIE running
//   -32603 InternalError      CreatePackage / NewObject returned null
FMCPResponse Tool_CreateAsset(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_BBCreateAsset", "Create Blackboard Asset"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString DestPathRaw;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("path"), DestPathRaw, Err)) { return Err; }

	// Optional parent_bb resolution — must come BEFORE Create so we fail-fast without polluting
	// the asset registry with an orphan BB if the parent is bad.
	UBlackboardData* ParentBB = nullptr;
	FString ParentBBPath;
	if (Request.Args.IsValid() && Request.Args->TryGetStringField(TEXT("parent_bb"), ParentBBPath)
		&& !ParentBBPath.IsEmpty())
	{
		int32 LoadErr = 0;
		FString LoadMsg;
		ParentBB = FMCPAssetLoader::Load<UBlackboardData>(ParentBBPath, LoadErr, LoadMsg);
		if (!ParentBB) { return FMCPToolHelpers::MakeError(Request, LoadErr, LoadMsg); }
	}

	// Wave Q3 — path validation + existence check + CreatePackage + NewObject delegated to
	// FMCPAssetFactory::Create. Per-type configure (Parent) follows; caller triggers
	// Scope.DirtyPackage once with the fully-populated asset.
	FMCPAssetCreateResult R = FMCPAssetFactory::Create<UBlackboardData>(DestPathRaw);
	if (!R.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, R.ErrorCode, R.ErrorMessage);
	}
	UBlackboardData* BB = Cast<UBlackboardData>(R.Asset);
	check(BB);

	if (ParentBB) { BB->Parent = ParentBB; }

	Scope.DirtyPackage(R.Package);

	return FMCPJsonBuilder()
		.Str (TEXT("asset_path"), BB->GetPathName())
		.Bool(TEXT("has_parent"), ParentBB != nullptr)
		.OptStr(TEXT("parent_path"), ParentBB ? ParentBB->GetPathName() : FString())
		.BuildSuccess(Request);
}

// ─── ai.bb.add_key (Wave P) ────────────────────────────────────────────────────────────────────
//
// Args:    { bb_path: string, key_name: string, key_type: string,
//            key_options?: { base_class?: string, enum_path?: string },
//            instance_synced?: bool (default false) }
// Result:  { added, key_name, key_type, key_index }
//
// key_type is the short-name form ("Bool" / "Int" / "Float" / "String" / "Name" / "Vector" /
// "Rotator" / "Class" / "Object" / "Enum"), matching ai.bb.list_keys' wire shape.
//
// For Class / Object key types: ``key_options.base_class`` sets the UBlackboardKeyType_X->BaseClass
// constraint (filters which classes/objects can be assigned at runtime).
// For Enum key type: ``key_options.enum_path`` sets EnumType (e.g. "/Script/MyGame.EAlertLevel").
//
// **DEFAULT VALUES NOT SUPPORTED IN v1.** Default values live in per-type binary serialization
// (UBlackboardKeyType_X stores them in subclass-specific fields). Callers use ai.bb.set_value at
// runtime to set initial values per-instance instead.
//
// Errors:
//   -32602 InvalidParams       missing args / unknown key_type / key_name collision in args
//   -32005 PropertyNotFound    key_name already exists on BB (KeyNotFound code reused as duplicate)
//   -32004 ObjectNotFound      bb_path not loadable / base_class / enum_path not loadable
//   -32027 PIEActive
FMCPResponse Tool_AddKey(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_BBAddKey", "Add Blackboard Key"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString BBPath, KeyName, KeyType;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("bb_path"),  BBPath,  Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("key_name"), KeyName, Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("key_type"), KeyType, Err)) { return Err; }

	int32 LoadErr = 0;
	FString LoadMsg;
	UBlackboardData* BB = FMCPAssetLoader::Load<UBlackboardData>(BBPath, LoadErr, LoadMsg);
	if (!BB) { return FMCPToolHelpers::MakeError(Request, LoadErr, LoadMsg); }

	UClass* KeyClass = AIBB_KeyTypeShortNameToClass(KeyType);
	if (!KeyClass)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			FString::Printf(TEXT("unknown key_type '%s' (expected one of Bool/Int/Float/String/Name/"
				"Vector/Rotator/Class/Object/Enum)"), *KeyType));
	}

	// Duplicate-name check: scan BB->Keys (parent keys are looked up separately at runtime; we only
	// guard against duplicates within THIS asset's own Keys[] list, matching engine semantics where
	// a child can legally override an inherited parent key — but cannot have two same-named in itself).
	const FName KeyFName(*KeyName);
	for (const FBlackboardEntry& Existing : BB->Keys)
	{
		if (Existing.EntryName == KeyFName)
		{
			return FMCPToolHelpers::MakeError(Request, kAIBBErrorKeyNotFound,
				FString::Printf(TEXT("blackboard '%s' already has a key named '%s'; remove first via "
					"ai.bb.remove_key or pick a unique name"), *BB->GetPathName(), *KeyName));
		}
	}

	// Pre-validate key_options before mutating asset state so a bad sub-arg doesn't leak a half-
	// constructed Keys[] entry into the BB.
	const TSharedPtr<FJsonObject>* OptionsObj = nullptr;
	const bool bHasOptions = Request.Args.IsValid()
		&& Request.Args->TryGetObjectField(TEXT("key_options"), OptionsObj)
		&& OptionsObj && (*OptionsObj).IsValid();

	UClass* OptionalBaseClass = nullptr;
	UEnum*  OptionalEnumType  = nullptr;
	if (bHasOptions)
	{
		FString BaseClassPath;
		if ((*OptionsObj)->TryGetStringField(TEXT("base_class"), BaseClassPath) && !BaseClassPath.IsEmpty())
		{
			// Only meaningful for Class / Object key types.
			if (KeyClass != UBlackboardKeyType_Class::StaticClass()
				&& KeyClass != UBlackboardKeyType_Object::StaticClass())
			{
				return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
					FString::Printf(TEXT("key_options.base_class is only valid for Class or Object "
						"key types (got '%s')"), *KeyType));
			}
			OptionalBaseClass = LoadClass<UObject>(nullptr, *BaseClassPath);
			if (!OptionalBaseClass)
			{
				return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
					FString::Printf(TEXT("key_options.base_class '%s' did not resolve to a UClass"),
						*BaseClassPath));
			}
		}

		FString EnumPath;
		if ((*OptionsObj)->TryGetStringField(TEXT("enum_path"), EnumPath) && !EnumPath.IsEmpty())
		{
			if (KeyClass != UBlackboardKeyType_Enum::StaticClass())
			{
				return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
					FString::Printf(TEXT("key_options.enum_path is only valid for Enum key type "
						"(got '%s')"), *KeyType));
			}
			OptionalEnumType = LoadObject<UEnum>(nullptr, *EnumPath);
			if (!OptionalEnumType)
			{
				return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
					FString::Printf(TEXT("key_options.enum_path '%s' did not resolve to a UEnum"),
						*EnumPath));
			}
		}
	}

	// Construct the key type instance owned by the BB asset (matches engine pattern in
	// UBlackboardData::UpdatePersistentKey — KeyType is Instanced and owned by the asset).
	UBlackboardKeyType* NewKT = NewObject<UBlackboardKeyType>(
		BB, KeyClass, NAME_None, RF_Public | RF_Transactional);
	check(NewKT);

	// Apply per-type options. Casts are safe — guarded above.
	if (OptionalBaseClass)
	{
		if (UBlackboardKeyType_Class* AsClassKT = Cast<UBlackboardKeyType_Class>(NewKT))
		{
			AsClassKT->BaseClass = OptionalBaseClass;
		}
		else if (UBlackboardKeyType_Object* AsObjectKT = Cast<UBlackboardKeyType_Object>(NewKT))
		{
			AsObjectKT->BaseClass = OptionalBaseClass;
		}
	}
	if (OptionalEnumType)
	{
		UBlackboardKeyType_Enum* AsEnumKT = Cast<UBlackboardKeyType_Enum>(NewKT);
		check(AsEnumKT);
		AsEnumKT->EnumType = OptionalEnumType;
	}

	// Optional instance-synced flag (per-key bit on FBlackboardEntry, NOT on KeyType).
	bool bInstanceSynced = false;
	if (Request.Args.IsValid()) { Request.Args->TryGetBoolField(TEXT("instance_synced"), bInstanceSynced); }

	FBlackboardEntry NewEntry;
	NewEntry.EntryName      = KeyFName;
	NewEntry.KeyType        = NewKT;
	NewEntry.bInstanceSynced = bInstanceSynced ? 1 : 0;

	BB->Modify();
	const int32 NewIdx = BB->Keys.Add(NewEntry);
	Scope.DirtyPackage(BB->GetPackage());

	return FMCPJsonBuilder()
		.Bool(TEXT("added"),     true)
		.Str (TEXT("key_name"),  KeyName)
		.Str (TEXT("key_type"),  KeyType)
		.Int (TEXT("key_index"), NewIdx)
		.Str (TEXT("bb_path"),   BB->GetPathName())
		.BuildSuccess(Request);
}

// ─── ai.bb.remove_key (Wave P) ─────────────────────────────────────────────────────────────────
//
// Args:    { bb_path: string, key_name: string }
// Result:  { removed, key_name, prior_key_type? }
//
// Removes a key by matching EntryName. Returns removed=false if the key wasn't present (caller can
// treat as idempotent). Does NOT walk Parent keys — only this asset's own Keys[] list (engine
// semantics: you can't "remove" an inherited parent key from a child).
//
// Errors:
//   -32004 ObjectNotFound  bb_path not loadable
//   -32027 PIEActive
FMCPResponse Tool_RemoveKey(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_BBRemoveKey", "Remove Blackboard Key"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString BBPath, KeyName;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("bb_path"),  BBPath,  Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("key_name"), KeyName, Err)) { return Err; }

	int32 LoadErr = 0;
	FString LoadMsg;
	UBlackboardData* BB = FMCPAssetLoader::Load<UBlackboardData>(BBPath, LoadErr, LoadMsg);
	if (!BB) { return FMCPToolHelpers::MakeError(Request, LoadErr, LoadMsg); }

	const FName KeyFName(*KeyName);
	int32 FoundIdx = INDEX_NONE;
	FString PriorTypeName;
	for (int32 i = 0; i < BB->Keys.Num(); ++i)
	{
		if (BB->Keys[i].EntryName == KeyFName)
		{
			FoundIdx = i;
			PriorTypeName = AIBB_KeyTypeToFriendlyName(BB->Keys[i].KeyType);
			break;
		}
	}

	if (FoundIdx == INDEX_NONE)
	{
		// Idempotent: report removed=false but DON'T raise — caller can call this in a cleanup loop
		// without worrying about which keys exist.
		return FMCPJsonBuilder()
			.Bool(TEXT("removed"),  false)
			.Str (TEXT("key_name"), KeyName)
			.Str (TEXT("bb_path"),  BB->GetPathName())
			.BuildSuccess(Request);
	}

	BB->Modify();
	BB->Keys.RemoveAt(FoundIdx);
	Scope.DirtyPackage(BB->GetPackage());

	return FMCPJsonBuilder()
		.Bool(TEXT("removed"),         true)
		.Str (TEXT("key_name"),        KeyName)
		.Str (TEXT("prior_key_type"),  PriorTypeName)
		.Str (TEXT("bb_path"),         BB->GetPathName())
		.BuildSuccess(Request);
}

// ─── Registration ─────────────────────────────────────────────────────────────────────────────
void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	RegisterTool(TEXT("ai.bb.list_keys"),    &Tool_ListKeys,    /*Lane A*/ false);
	RegisterTool(TEXT("ai.bb.get_value"),    &Tool_GetValue,    /*Lane A*/ false);
	RegisterTool(TEXT("ai.bb.set_value"),    &Tool_SetValue,    /*Lane A*/ false);
	RegisterTool(TEXT("ai.bb.create_asset"), &Tool_CreateAsset, /*Lane A*/ false);
	RegisterTool(TEXT("ai.bb.add_key"),      &Tool_AddKey,      /*Lane A*/ false);
	RegisterTool(TEXT("ai.bb.remove_key"),   &Tool_RemoveKey,   /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("AIBlackboard surface registered: 6 ai.bb.* tools "
			 "(list_keys + get_value + set_value + create_asset + add_key + remove_key), all Lane A"));
}

} // namespace FAIBlackboardTools

// Wave I refactor 2026-05: auto-registration via FMCPSurfaceRegistry replaces the
// manual include + Register call in UnrealMCPBridge.cpp.
#include "MCPSurfaceRegistry.h"
MCP_REGISTER_SURFACE(AIBlackboardTools, &FAIBlackboardTools::Register)

#undef LOCTEXT_NAMESPACE
