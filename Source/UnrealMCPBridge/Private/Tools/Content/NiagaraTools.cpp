// Copyright FatumGame. All Rights Reserved.

#include "NiagaraTools.h"

#include "MCPSurfaceRegistry.h"

#include "FMCPDispatchQueue.h"
#include "MCPAssetLoader.h"
#include "MCPJsonBuilder.h"
#include "MCPMutatorScope.h"
#include "MCPToolHelpers.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPAssetPathUtils.h"

#include "NiagaraComponent.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraEmitterFactoryNew.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraParameterStore.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "NiagaraUserRedirectionParameterStore.h"
#include "Engine/World.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"

#include "AssetToolsModule.h"
#include "Editor.h"
#include "GameFramework/Actor.h"
#include "IAssetTools.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ScopedTransaction.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "UObject/Package.h"

#include "Utils/MCPActorPathUtils.h"
#include "Utils/MCPAssetPathUtils.h"
#include "Utils/MCPWorldContext.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// NIA_ prefix retained for any helper unique to this surface.
	constexpr int32 kNIAErrorInvalidParams = kMCPErrorInvalidParams;
	constexpr int32 kNIAErrorInternal      = kMCPErrorInternal;

	// ─── value decoding ─────────────────────────────────────────────────────────────────────────

	/**
	 * Best-effort decode of a Niagara user parameter's binary value into a JSON value.
	 *
	 * Type coverage matches what FNiagaraTypeDefinition exposes via static GetXxxDef():
	 *   float / int32 / bool / FNiagaraBool / FVector2f / FVector3f / FVector4f / FLinearColor /
	 *   FQuat4f / FNiagaraPosition (treated as Vector3 with double precision in LWC).
	 *
	 * Unknown / unsupported types (UObject, DataInterface, custom structs, position 3-floats stored
	 * differently in cooked builds, etc.) → emit a string sentinel ``"<unsupported: <typename>>"``.
	 *
	 * NEVER returns null — see UMG_BuildPropertyValueJson contract in UMGTools for the same
	 * "always-emit-something" rule (the Python wrapper expects a present-or-null field).
	 */
	TSharedPtr<FJsonValue> NIA_DecodeUserParamDefault(
		const FNiagaraUserRedirectionParameterStore& Store,
		const FNiagaraVariable& Var)
	{
		const uint8* DataPtr = Store.GetParameterData(Var);
		if (!DataPtr)
		{
			return MakeShared<FJsonValueNull>();
		}

		const FNiagaraTypeDefinition& Type = Var.GetType();

		// Use IsSameBaseDefinition so static-flagged variants (TF_Static) of primitive types still
		// match their base. NiagaraTypeDefinition::operator== compares Flags strictly which would
		// miss static user params.
		auto IsSameBase = [&Type](const FNiagaraTypeDefinition& Reference)
		{
			return Type.IsSameBaseDefinition(Reference);
		};

		// Primitive scalars.
		if (IsSameBase(FNiagaraTypeDefinition::GetFloatDef()))
		{
			float V = 0.0f;
			FMemory::Memcpy(&V, DataPtr, sizeof(float));
			return MakeShared<FJsonValueNumber>(static_cast<double>(V));
		}
		if (IsSameBase(FNiagaraTypeDefinition::GetIntDef()))
		{
			int32 V = 0;
			FMemory::Memcpy(&V, DataPtr, sizeof(int32));
			return MakeShared<FJsonValueNumber>(static_cast<double>(V));
		}
		if (IsSameBase(FNiagaraTypeDefinition::GetBoolDef()))
		{
			// FNiagaraBool stores int32 (32-bit) under the hood — !=0 means true.
			int32 V = 0;
			FMemory::Memcpy(&V, DataPtr, sizeof(int32));
			return MakeShared<FJsonValueBoolean>(V != 0);
		}

		// Vector types.
		if (IsSameBase(FNiagaraTypeDefinition::GetVec2Def()))
		{
			FVector2f V(ForceInit);
			FMemory::Memcpy(&V, DataPtr, sizeof(FVector2f));
			TArray<TSharedPtr<FJsonValue>> Arr;
			Arr.Add(MakeShared<FJsonValueNumber>(V.X));
			Arr.Add(MakeShared<FJsonValueNumber>(V.Y));
			return MakeShared<FJsonValueArray>(Arr);
		}
		if (IsSameBase(FNiagaraTypeDefinition::GetVec3Def()))
		{
			FVector3f V(ForceInit);
			FMemory::Memcpy(&V, DataPtr, sizeof(FVector3f));
			TArray<TSharedPtr<FJsonValue>> Arr;
			Arr.Add(MakeShared<FJsonValueNumber>(V.X));
			Arr.Add(MakeShared<FJsonValueNumber>(V.Y));
			Arr.Add(MakeShared<FJsonValueNumber>(V.Z));
			return MakeShared<FJsonValueArray>(Arr);
		}
		if (IsSameBase(FNiagaraTypeDefinition::GetVec4Def()) || IsSameBase(FNiagaraTypeDefinition::GetQuatDef()))
		{
			FVector4f V(ForceInit);
			FMemory::Memcpy(&V, DataPtr, sizeof(FVector4f));
			TArray<TSharedPtr<FJsonValue>> Arr;
			Arr.Add(MakeShared<FJsonValueNumber>(V.X));
			Arr.Add(MakeShared<FJsonValueNumber>(V.Y));
			Arr.Add(MakeShared<FJsonValueNumber>(V.Z));
			Arr.Add(MakeShared<FJsonValueNumber>(V.W));
			return MakeShared<FJsonValueArray>(Arr);
		}
		if (IsSameBase(FNiagaraTypeDefinition::GetColorDef()))
		{
			FLinearColor C(ForceInit);
			FMemory::Memcpy(&C, DataPtr, sizeof(FLinearColor));
			TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("_kind"), TEXT("LinearColor"));
			Obj->SetNumberField(TEXT("r"), C.R);
			Obj->SetNumberField(TEXT("g"), C.G);
			Obj->SetNumberField(TEXT("b"), C.B);
			Obj->SetNumberField(TEXT("a"), C.A);
			return MakeShared<FJsonValueObject>(Obj);
		}
		if (IsSameBase(FNiagaraTypeDefinition::GetPositionDef()))
		{
			// Position is stored as FVector3f in the parameter buffer (LWC offset is applied during
			// emitter execution, NOT here). Decode as float3.
			FVector3f V(ForceInit);
			FMemory::Memcpy(&V, DataPtr, sizeof(FVector3f));
			TArray<TSharedPtr<FJsonValue>> Arr;
			Arr.Add(MakeShared<FJsonValueNumber>(V.X));
			Arr.Add(MakeShared<FJsonValueNumber>(V.Y));
			Arr.Add(MakeShared<FJsonValueNumber>(V.Z));
			return MakeShared<FJsonValueArray>(Arr);
		}

		// Fall-through — emit a typed sentinel string.
		return MakeShared<FJsonValueString>(
			FString::Printf(TEXT("<unsupported: %s>"), *Type.GetName()));
	}

	/** Single-emitter parameter collection — gathers Spawn + Update RapidIterationParameters. */
	void NIA_CollectEmitterParameters(
		const UNiagaraScript* SpawnScript,
		const UNiagaraScript* UpdateScript,
		TArray<FNiagaraVariable>& OutVariables)
	{
		auto AppendFromStore = [&OutVariables](const FNiagaraParameterStore& Store)
		{
			TArray<FNiagaraVariable> Tmp;
			Store.GetParameters(Tmp);
			for (const FNiagaraVariable& V : Tmp)
			{
				OutVariables.Add(V);
			}
		};
		if (SpawnScript)  { AppendFromStore(SpawnScript->RapidIterationParameters); }
		if (UpdateScript) { AppendFromStore(UpdateScript->RapidIterationParameters); }
	}

	/** Dedup helper — same name+type combo from spawn and update scripts should appear once. */
	void NIA_DedupParameters(TArray<FNiagaraVariable>& InOut)
	{
		// Stable order: keep first occurrence. O(N^2) over per-script param count — RIP params per
		// script are typically <50, well below the threshold where a Set lookup would win.
		TArray<FNiagaraVariable> Out;
		Out.Reserve(InOut.Num());
		for (const FNiagaraVariable& V : InOut)
		{
			const bool bAlreadyPresent = Out.ContainsByPredicate([&V](const FNiagaraVariable& X)
			{
				return X.GetName() == V.GetName() && X.GetType() == V.GetType();
			});
			if (!bAlreadyPresent)
			{
				Out.Add(V);
			}
		}
		InOut = MoveTemp(Out);
	}

	/** {name, type, default?} JSON builder for a system/emitter param (no value decode). */
	TSharedRef<FJsonObject> NIA_BuildParamJsonNoDefault(const FNiagaraVariable& Var)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Var.GetName().ToString());
		Obj->SetStringField(TEXT("type"), Var.GetType().GetName());
		return Obj;
	}

	/** {name, type, default} JSON for a USER parameter (with binary-decoded default value). */
	TSharedRef<FJsonObject> NIA_BuildUserParamJson(
		const FNiagaraUserRedirectionParameterStore& Store,
		const FNiagaraVariable& Var)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Var.GetName().ToString());
		Obj->SetStringField(TEXT("type"), Var.GetType().GetName());
		Obj->SetField(TEXT("default"), NIA_DecodeUserParamDefault(Store, Var));
		return Obj;
	}

	// ─── value encoding (Wave B niagara.set_user_param) ─────────────────────────────────────────

	/** Read a JSON array of 2/3/4 numeric components into ``OutValues``. Returns false on shape mismatch. */
	bool NIA_DecodeFloatArray(const TSharedPtr<FJsonValue>& Json, int32 Expected, TArray<float>& OutValues)
	{
		if (!Json.IsValid() || Json->Type != EJson::Array) { return false; }
		const TArray<TSharedPtr<FJsonValue>>& Arr = Json->AsArray();
		if (Arr.Num() != Expected) { return false; }
		OutValues.Reset(Expected);
		for (const TSharedPtr<FJsonValue>& V : Arr)
		{
			if (!V.IsValid() || V->Type != EJson::Number) { return false; }
			OutValues.Add(static_cast<float>(V->AsNumber()));
		}
		return true;
	}

	/**
	 * Inverse of NIA_DecodeUserParamDefault. Encode a JSON value into a typed byte buffer.
	 *
	 * Type coverage matches the decoder. On success populates ``OutBytes`` with the bytewise layout
	 * the parameter store expects and returns true. On failure returns false; ``OutError`` carries
	 * a user-facing message (caller wraps in -32602 InvalidParams or -32032 PinTypeUnsupported).
	 *
	 * Accepted JSON shapes:
	 *   float / int32   → JSON Number (int truncated; bool not accepted to keep types tight)
	 *   bool / FNiagaraBool → JSON Boolean  OR  Number (!=0 → true)
	 *   FVector2f       → [x, y]
	 *   FVector3f / FNiagaraPosition → [x, y, z]
	 *   FVector4f / FQuat4f → [x, y, z, w]
	 *   FLinearColor    → [r, g, b, a]  OR  {r:f, g:f, b:f, a:f}  (a optional, defaults to 1.0)
	 */
	bool NIA_EncodeUserParamValue(
		const FNiagaraVariable& Var,
		const TSharedPtr<FJsonValue>& Json,
		TArray<uint8>& OutBytes,
		FString& OutError,
		bool& bOutIsUnsupportedType)
	{
		bOutIsUnsupportedType = false;
		if (!Json.IsValid())
		{
			OutError = TEXT("value is missing or null");
			return false;
		}

		const FNiagaraTypeDefinition& Type = Var.GetType();
		auto IsSameBase = [&Type](const FNiagaraTypeDefinition& Reference)
		{
			return Type.IsSameBaseDefinition(Reference);
		};

		// ─── scalar float ─────────────────────────────────────────────────────────────────────────
		if (IsSameBase(FNiagaraTypeDefinition::GetFloatDef()))
		{
			if (Json->Type != EJson::Number)
			{
				OutError = FString::Printf(TEXT("expected JSON number for float param '%s'"), *Var.GetName().ToString());
				return false;
			}
			const float V = static_cast<float>(Json->AsNumber());
			OutBytes.SetNumUninitialized(sizeof(float));
			FMemory::Memcpy(OutBytes.GetData(), &V, sizeof(float));
			return true;
		}

		// ─── scalar int32 ─────────────────────────────────────────────────────────────────────────
		if (IsSameBase(FNiagaraTypeDefinition::GetIntDef()))
		{
			if (Json->Type != EJson::Number)
			{
				OutError = FString::Printf(TEXT("expected JSON number for int param '%s'"), *Var.GetName().ToString());
				return false;
			}
			const int32 V = static_cast<int32>(Json->AsNumber());
			OutBytes.SetNumUninitialized(sizeof(int32));
			FMemory::Memcpy(OutBytes.GetData(), &V, sizeof(int32));
			return true;
		}

		// ─── bool / FNiagaraBool ─────────────────────────────────────────────────────────────────
		if (IsSameBase(FNiagaraTypeDefinition::GetBoolDef()))
		{
			bool bValue = false;
			if (Json->Type == EJson::Boolean)        { bValue = Json->AsBool(); }
			else if (Json->Type == EJson::Number)    { bValue = (Json->AsNumber() != 0.0); }
			else
			{
				OutError = FString::Printf(TEXT("expected JSON bool or number for bool param '%s'"), *Var.GetName().ToString());
				return false;
			}
			// FNiagaraBool stores int32 (32-bit) — non-zero means true, exactly the value 0xFFFFFFFF (-1)
			// in the engine's serialization. We follow the engine convention (FNiagaraBool::True = 0xFFFFFFFF).
			const int32 V = bValue ? -1 : 0;
			OutBytes.SetNumUninitialized(sizeof(int32));
			FMemory::Memcpy(OutBytes.GetData(), &V, sizeof(int32));
			return true;
		}

		// ─── FVector2f ───────────────────────────────────────────────────────────────────────────
		if (IsSameBase(FNiagaraTypeDefinition::GetVec2Def()))
		{
			TArray<float> Components;
			if (!NIA_DecodeFloatArray(Json, 2, Components))
			{
				OutError = FString::Printf(TEXT("expected JSON [x,y] for Vec2 param '%s'"), *Var.GetName().ToString());
				return false;
			}
			OutBytes.SetNumUninitialized(sizeof(FVector2f));
			FMemory::Memcpy(OutBytes.GetData(), Components.GetData(), sizeof(FVector2f));
			return true;
		}

		// ─── FVector3f / FNiagaraPosition ────────────────────────────────────────────────────────
		if (IsSameBase(FNiagaraTypeDefinition::GetVec3Def()) || IsSameBase(FNiagaraTypeDefinition::GetPositionDef()))
		{
			TArray<float> Components;
			if (!NIA_DecodeFloatArray(Json, 3, Components))
			{
				OutError = FString::Printf(TEXT("expected JSON [x,y,z] for Vec3 param '%s'"), *Var.GetName().ToString());
				return false;
			}
			OutBytes.SetNumUninitialized(sizeof(FVector3f));
			FMemory::Memcpy(OutBytes.GetData(), Components.GetData(), sizeof(FVector3f));
			return true;
		}

		// ─── FVector4f / FQuat4f ─────────────────────────────────────────────────────────────────
		if (IsSameBase(FNiagaraTypeDefinition::GetVec4Def()) || IsSameBase(FNiagaraTypeDefinition::GetQuatDef()))
		{
			TArray<float> Components;
			if (!NIA_DecodeFloatArray(Json, 4, Components))
			{
				OutError = FString::Printf(TEXT("expected JSON [x,y,z,w] for Vec4/Quat param '%s'"), *Var.GetName().ToString());
				return false;
			}
			OutBytes.SetNumUninitialized(sizeof(FVector4f));
			FMemory::Memcpy(OutBytes.GetData(), Components.GetData(), sizeof(FVector4f));
			return true;
		}

		// ─── FLinearColor ────────────────────────────────────────────────────────────────────────
		if (IsSameBase(FNiagaraTypeDefinition::GetColorDef()))
		{
			FLinearColor C(0, 0, 0, 1);
			if (Json->Type == EJson::Array)
			{
				TArray<float> Components;
				const int32 N = Json->AsArray().Num();
				if (N != 3 && N != 4)
				{
					OutError = FString::Printf(TEXT("expected JSON [r,g,b] or [r,g,b,a] for LinearColor param '%s'"), *Var.GetName().ToString());
					return false;
				}
				if (!NIA_DecodeFloatArray(Json, N, Components))
				{
					OutError = FString::Printf(TEXT("LinearColor components must all be numbers for param '%s'"), *Var.GetName().ToString());
					return false;
				}
				C.R = Components[0]; C.G = Components[1]; C.B = Components[2];
				if (N == 4) { C.A = Components[3]; }
			}
			else if (Json->Type == EJson::Object)
			{
				const TSharedPtr<FJsonObject>& Obj = Json->AsObject();
				double R = 0, G = 0, B = 0, A = 1.0;
				if (!Obj->TryGetNumberField(TEXT("r"), R) ||
				    !Obj->TryGetNumberField(TEXT("g"), G) ||
				    !Obj->TryGetNumberField(TEXT("b"), B))
				{
					OutError = FString::Printf(TEXT("expected {r,g,b,a?} JSON object for LinearColor param '%s'"), *Var.GetName().ToString());
					return false;
				}
				Obj->TryGetNumberField(TEXT("a"), A);
				C.R = static_cast<float>(R);
				C.G = static_cast<float>(G);
				C.B = static_cast<float>(B);
				C.A = static_cast<float>(A);
			}
			else
			{
				OutError = FString::Printf(TEXT("expected JSON array or object for LinearColor param '%s'"), *Var.GetName().ToString());
				return false;
			}
			OutBytes.SetNumUninitialized(sizeof(FLinearColor));
			FMemory::Memcpy(OutBytes.GetData(), &C, sizeof(FLinearColor));
			return true;
		}

		// Fall-through — unsupported type (DataInterface / UObject / custom struct).
		bOutIsUnsupportedType = true;
		OutError = FString::Printf(
			TEXT("user_param '%s' has unsupported type '%s' (DataInterface / UObject / custom struct); ")
			TEXT("only float/int/bool/Vec2/Vec3/Vec4/Quat/LinearColor/Position are supported"),
			*Var.GetName().ToString(), *Type.GetName());
		return false;
	}

	// ─── world resolution (Wave E S2 runtime tools) ─────────────────────────────────────────────
	//
	// Mirrors DebugTools / PhysicsTools — PIE world first ("the window the user is watching"),
	// editor world fallback. Returns null only when GEditor itself is missing (commandlet / cooker).
	UWorld* NIA_ResolveWorld()
	{
		check(IsInGameThread());
		if (GEditor && GEditor->PlayWorld)
		{
			return GEditor->PlayWorld;
		}
		return FMCPWorldContext::GetEditorWorld();
	}

	const TCHAR* NIA_WorldKindName(const UWorld* World)
	{
		if (!World) { return TEXT("none"); }
		return World->WorldType == EWorldType::PIE ? TEXT("pie") : TEXT("editor");
	}

	/** Required [x,y,z] number array. Populates OutError + returns false on missing/malformed. */
	bool NIA_ReadVector3(
		const TSharedPtr<FJsonObject>& Args,
		const TCHAR* FieldName,
		FVector& OutV,
		FString& OutError)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Args.IsValid() || !Args->TryGetArrayField(FieldName, Arr) || !Arr)
		{
			OutError = FString::Printf(TEXT("missing required array field '%s' ([x,y,z])"), FieldName);
			return false;
		}
		if (Arr->Num() != 3)
		{
			OutError = FString::Printf(
				TEXT("'%s' must be [x,y,z] (3 numbers); got %d entries"), FieldName, Arr->Num());
			return false;
		}
		double X = 0.0, Y = 0.0, Z = 0.0;
		if (!(*Arr)[0]->TryGetNumber(X) || !(*Arr)[1]->TryGetNumber(Y) || !(*Arr)[2]->TryGetNumber(Z))
		{
			OutError = FString::Printf(TEXT("'%s' entries must all be numbers"), FieldName);
			return false;
		}
		OutV = FVector(X, Y, Z);
		return true;
	}

	/** Optional [x,y,z] number array. Returns DefaultValue when missing; errors when present-but-malformed. */
	bool NIA_ReadOptionalVector3(
		const TSharedPtr<FJsonObject>& Args,
		const TCHAR* FieldName,
		const FVector& DefaultValue,
		FVector& OutV,
		FString& OutError)
	{
		if (!Args.IsValid() || !Args->HasField(FieldName))
		{
			OutV = DefaultValue;
			return true;
		}
		return NIA_ReadVector3(Args, FieldName, OutV, OutError);
	}

	/** Find a Niagara user-param by FName. Returns whether it was found + writes Var by-ref. */
	bool NIA_FindUserParamByName(
		const FNiagaraUserRedirectionParameterStore& Store,
		const FString& Name,
		FNiagaraVariable& OutVar)
	{
		// User parameters often come prefixed by "User." (the redirection store strips it on lookup).
		// We accept both shapes — bare name OR "User.Name" — for caller ergonomics.
		const FName Target(*Name);
		const FName TargetWithPrefix = Name.StartsWith(TEXT("User.")) ? Target : FName(*(FString(TEXT("User.")) + Name));

		TArray<FNiagaraVariable> Vars;
		Store.GetUserParameters(Vars);
		for (const FNiagaraVariable& V : Vars)
		{
			if (V.GetName() == Target || V.GetName() == TargetWithPrefix)
			{
				OutVar = V;
				return true;
			}
		}
		return false;
	}
} // namespace

namespace FNiagaraTools
{

// ─── niagara.list_parameters ──────────────────────────────────────────────────────────────────
//
// Args:    { niagara_system_path: string }
// Result:  {
//            user_params:    [{ name, type, default }, ...],
//            system_params:  [{ name, type }, ...],
//            emitter_params: [{ emitter, name, type }, ...]
//          }
//
// Errors:
//   -32602 InvalidParams      missing niagara_system_path
//   -32010 InvalidPath        path malformed / unknown mount
//   -32004 ObjectNotFound     LoadObject returned null
//   -32011 WrongClass         asset is not UNiagaraSystem
//
// Sorting: user_params + system_params are NOT sorted (preserved in source order). emitter_params
// preserves emitter-handle order from GetEmitterHandles() (which matches the order shown in the
// Niagara editor's emitter tabs).
//
// Default values: ONLY user_params carry decoded defaults. System and emitter parameters live in
// RapidIterationParameters (internal iteration storage) — their binary layout matches the source
// script's pin defaults, but the Niagara editor doesn't surface them to artists. We emit name+type
// only for those two categories so the wire payload stays small.
FMCPResponse Tool_ListParameters(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString Path;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("niagara_system_path"), Path, Err)) { return Err; }

	int32 ErrCode = 0;
	FString ErrMsg;
	UNiagaraSystem* System = FMCPAssetLoader::Load<UNiagaraSystem>(Path, ErrCode, ErrMsg);
	if (!System) { return FMCPToolHelpers::MakeError(Request, ErrCode, ErrMsg); }

	// ─── User parameters (with decoded defaults) ────────────────────────────────────────────────
	const FNiagaraUserRedirectionParameterStore& UserStore = System->GetExposedParameters();
	TArray<FNiagaraVariable> UserVars;
	UserStore.GetUserParameters(UserVars);

	TArray<TSharedPtr<FJsonValue>> UserArr;
	UserArr.Reserve(UserVars.Num());
	for (const FNiagaraVariable& V : UserVars)
	{
		UserArr.Add(MakeShared<FJsonValueObject>(NIA_BuildUserParamJson(UserStore, V)));
	}

	// ─── System parameters (spawn + update scripts) ─────────────────────────────────────────────
	TArray<FNiagaraVariable> SystemVars;
	if (const UNiagaraScript* SpawnScript = System->GetSystemSpawnScript())
	{
		TArray<FNiagaraVariable> Tmp;
		SpawnScript->RapidIterationParameters.GetParameters(Tmp);
		SystemVars.Append(Tmp);
	}
	if (const UNiagaraScript* UpdateScript = System->GetSystemUpdateScript())
	{
		TArray<FNiagaraVariable> Tmp;
		UpdateScript->RapidIterationParameters.GetParameters(Tmp);
		SystemVars.Append(Tmp);
	}
	NIA_DedupParameters(SystemVars);

	TArray<TSharedPtr<FJsonValue>> SystemArr;
	SystemArr.Reserve(SystemVars.Num());
	for (const FNiagaraVariable& V : SystemVars)
	{
		SystemArr.Add(MakeShared<FJsonValueObject>(NIA_BuildParamJsonNoDefault(V)));
	}

	// ─── Emitter parameters (per emitter, spawn + update scripts) ───────────────────────────────
	TArray<TSharedPtr<FJsonValue>> EmitterArr;
	const TArray<FNiagaraEmitterHandle>& EmitterHandles = System->GetEmitterHandles();
	for (const FNiagaraEmitterHandle& Handle : EmitterHandles)
	{
		const FString EmitterName = Handle.GetName().ToString();
		const FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
		if (!EmitterData)
		{
			continue;
		}

		TArray<FNiagaraVariable> EmitterVars;
		NIA_CollectEmitterParameters(
			EmitterData->SpawnScriptProps.Script,
			EmitterData->UpdateScriptProps.Script,
			EmitterVars);
		NIA_DedupParameters(EmitterVars);

		for (const FNiagaraVariable& V : EmitterVars)
		{
			TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("emitter"), EmitterName);
			Obj->SetStringField(TEXT("name"), V.GetName().ToString());
			Obj->SetStringField(TEXT("type"), V.GetType().GetName());
			EmitterArr.Add(MakeShared<FJsonValueObject>(Obj));
		}
	}

	return FMCPJsonBuilder()
		.Arr(TEXT("user_params"),    MoveTemp(UserArr))
		.Arr(TEXT("system_params"),  MoveTemp(SystemArr))
		.Arr(TEXT("emitter_params"), MoveTemp(EmitterArr))
		.BuildSuccess(Request);
}

// ─── niagara.set_user_param ───────────────────────────────────────────────────────────────────
//
// Args:    { niagara_system_path: string, name: string, value: <JSON typed> }
// Result:  { changed: bool, name: string, type: string, prior: <prior_value>, new: <new_value> }
//
// Errors:
//   -32602 InvalidParams                missing args / wrong shape
//   -32010 InvalidPath                  niagara_system_path malformed / unknown mount
//   -32004 ObjectNotFound               system asset can't load
//   -32011 WrongClass                   asset isn't UNiagaraSystem
//   -32027 PIEActive                    PIE running; mutation refused
//   -32040 NiagaraParameterNotFound     ``name`` not present in user-param store
//   -32032 PinTypeUnsupported           parameter's FNiagaraTypeDefinition isn't in the encoder map
//   -32603 InternalError                SetParameterData refused (extremely rare)
FMCPResponse Tool_SetUserParam(const FMCPRequest& Request)
{
	check(IsInGameThread());

	// PIE guard — Niagara assets are shared between editor + PIE worlds, so mutating during PIE
	// would race the live simulation; refuse exactly like every other Phase 4+ asset-mutator.
	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_SetNiagaraUserParam", "Set Niagara user parameter"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString Path, Name;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("niagara_system_path"), Path, Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("name"),                Name, Err)) { return Err; }

	if (!Request.Args.IsValid() || !Request.Args->HasField(TEXT("value")))
	{
		return FMCPToolHelpers::MakeError(Request, kNIAErrorInvalidParams,
			TEXT("niagara.set_user_param requires args.value (typed JSON: number / bool / array / object)"));
	}
	const TSharedPtr<FJsonValue> ValueJson = Request.Args->TryGetField(TEXT("value"));

	int32 ErrCode = 0;
	FString ErrMsg;
	UNiagaraSystem* System = FMCPAssetLoader::Load<UNiagaraSystem>(Path, ErrCode, ErrMsg);
	if (!System) { return FMCPToolHelpers::MakeError(Request, ErrCode, ErrMsg); }

	FNiagaraUserRedirectionParameterStore& UserStore = System->GetExposedParameters();
	FNiagaraVariable Var;
	if (!NIA_FindUserParamByName(UserStore, Name, Var))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorNiagaraParameterNotFound,
			FString::Printf(
				TEXT("user_param '%s' not found on '%s' (call niagara.list_parameters for the inventory)"),
				*Name, *System->GetPathName()));
	}

	// Capture prior value first (for round-trip diff) BEFORE we mutate the store.
	TSharedPtr<FJsonValue> Prior = NIA_DecodeUserParamDefault(UserStore, Var);

	// Encode JSON → byte buffer (typed per FNiagaraTypeDefinition).
	TArray<uint8> Bytes;
	FString EncodeError;
	bool bUnsupportedType = false;
	if (!NIA_EncodeUserParamValue(Var, ValueJson, Bytes, EncodeError, bUnsupportedType))
	{
		const int32 Code = bUnsupportedType ? kMCPErrorPinTypeUnsupported : kNIAErrorInvalidParams;
		return FMCPToolHelpers::MakeError(Request, Code, EncodeError);
	}

	// ─── apply ───────────────────────────────────────────────────────────────────────────────────
	// Transaction opened by FMCPMutatorScope above. System->Modify() participates in the editor's
	// Undo stack alongside the parameter-store write (artists expect Ctrl-Z to revert).
	System->Modify();
	Scope.DirtyPackage(System->GetOutermost());

	// SetParameterData mutates the parameter store in place. ``bAdd=false`` because we already
	// verified the variable exists (NIA_FindUserParamByName); passing true would silently inject
	// a phantom variable if our lookup ever went stale.
	const bool bWriteOk = UserStore.SetParameterData(Bytes.GetData(), Var, /*bAdd*/ false);
	if (!bWriteOk)
	{
		return FMCPToolHelpers::MakeError(Request, kNIAErrorInternal,
			FString::Printf(
				TEXT("FNiagaraUserRedirectionParameterStore::SetParameterData failed for '%s' on '%s' (param vanished or type mismatch)"),
				*Var.GetName().ToString(), *System->GetPathName()));
	}

	// The store doesn't fire change broadcasts by itself; UNiagaraSystem::OnSystemPostEditChange
	// is the canonical re-bind hook so PIE components pick up the new value next tick. We don't
	// have direct access to it, but Modify()+MarkPackageDirty() is what the editor's own user-param
	// panel does — that's enough for the asset to round-trip on the next save.

	// Decode-after-write so the response carries the canonical "as the store sees it" value.
	TSharedPtr<FJsonValue> NewValue = NIA_DecodeUserParamDefault(UserStore, Var);

	return FMCPJsonBuilder()
		.Bool(TEXT("changed"), true)
		.Str(TEXT("name"), Var.GetName().ToString())
		.Str(TEXT("type"), Var.GetType().GetName())
		.Field(TEXT("prior"), Prior.ToSharedRef())
		.Field(TEXT("new"),   NewValue.ToSharedRef())
		.BuildSuccess(Request);
}

// ─── niagara.create_emitter ───────────────────────────────────────────────────────────────────
//
// Args:    { dest_path: string, save?: bool, add_default_modules?: bool }
// Result:  { created: bool, asset_path: string, saved: bool }
//
// Args.add_default_modules (default true) controls UNiagaraEmitterFactoryNew's
// bAddDefaultModulesAndRenderersToEmptyEmitter — when true the factory creates a default
// "Sprite Renderer + Initialize Particle / Particle Update" stack so the emitter is immediately
// runnable in the editor preview; when false the result is a bare emitter that the caller must
// populate via subsequent editor edits.
//
// Errors:
//   -32602 InvalidParams      missing dest_path
//   -32010 InvalidPath        dest_path malformed / unknown mount
//   -32014 PathInUse          dest_path already exists on disk
//   -32027 PIEActive          PIE running; asset creation refused
//   -32603 InternalError      IAssetTools::CreateAsset returned null
FMCPResponse Tool_CreateEmitter(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_CreateNiagaraEmitter", "Create Niagara Emitter"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString DestPathRaw;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("dest_path"), DestPathRaw, Err)) { return Err; }

	const FString DestPathNorm = FMCPAssetPathUtils::Normalize(DestPathRaw);
	if (DestPathNorm.IsEmpty() || !FMCPAssetPathUtils::IsValidGameOrPlugin(DestPathNorm))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidPath,
			FString::Printf(
				TEXT("dest_path '%s' is malformed or references an unknown mount (need /Game/... or /Plugin/...)"),
				*DestPathRaw));
	}
	const FString PackagePath = FPaths::GetPath(DestPathNorm);
	const FString AssetName   = FPaths::GetBaseFilename(DestPathNorm);

	// Wave R dual existence check — disk + in-memory. See bp.create_blueprint for full rationale.
	if (FPackageName::DoesPackageExist(DestPathNorm) ||
		FindObject<UObject>(nullptr, *(DestPathNorm + TEXT(".") + AssetName)) != nullptr)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPathInUse,
			FString::Printf(TEXT("dest_path '%s' already exists (on disk OR in memory)"), *DestPathNorm));
	}

	bool bAddDefaultModules = true;
	Request.Args->TryGetBoolField(TEXT("add_default_modules"), bAddDefaultModules);

	UNiagaraEmitterFactoryNew* Factory = NewObject<UNiagaraEmitterFactoryNew>();
	Factory->EmitterToCopy = nullptr;
	Factory->bUseInheritance = false;
	Factory->bAddDefaultModulesAndRenderersToEmptyEmitter = bAddDefaultModules;

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	UObject* NewAsset = AssetTools.CreateAsset(AssetName, PackagePath, UNiagaraEmitter::StaticClass(), Factory);
	if (!NewAsset)
	{
		return FMCPToolHelpers::MakeError(Request, kNIAErrorInternal,
			FString::Printf(TEXT("IAssetTools::CreateAsset returned null for emitter %s/%s "
				"(factory may have rejected; check editor log)"),
				*PackagePath, *AssetName));
	}

	Scope.DirtyPackage(NewAsset->GetOutermost());

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
		.Bool(TEXT("default_modules"), bAddDefaultModules)
		.BuildSuccess(Request);
}

// ─── niagara.set_emitter_enabled ──────────────────────────────────────────────────────────────
//
// Args:    { actor_path: string, emitter_index: number, enabled: bool, component_name?: string }
// Result:  { actor_path, emitter_name, emitter_index, enabled, component_name }
//
// Targets a placed AActor that has at least one UNiagaraComponent; flips one emitter's enable
// state via UNiagaraComponent::SetEmitterEnable. Lookup by emitter_index in the component's
// UNiagaraSystem.GetEmitterHandles() list. component_name optional — when there are multiple
// Niagara components on the actor it picks the named one; otherwise the first attached.
//
// NO PIE guard — works in editor world (component simulates in editor unless explicitly paused)
// AND PIE. Mutates only runtime component state (not asset).
//
// Errors:
//   -32602 InvalidParams      missing args
//   -32004 ObjectNotFound     actor or component missing
//   -32011 WrongClass         actor has no UNiagaraComponent
//   -32026 PropertyIndexOOB   emitter_index out of range for the component's system
//   -32024 AmbiguousComponent multiple Niagara components, no component_name to disambiguate
FMCPResponse Tool_SetEmitterEnabled(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString ActorPath;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("actor_path"), ActorPath, Err)) { return Err; }

	int32 EmitterIndex = -1;
	if (!Request.Args->TryGetNumberField(TEXT("emitter_index"), EmitterIndex) || EmitterIndex < 0)
	{
		return FMCPToolHelpers::MakeError(Request, kNIAErrorInvalidParams,
			TEXT("niagara.set_emitter_enabled requires args.emitter_index (non-negative integer)"));
	}

	bool bEnabled = false;
	if (!Request.Args->TryGetBoolField(TEXT("enabled"), bEnabled))
	{
		return FMCPToolHelpers::MakeError(Request, kNIAErrorInvalidParams,
			TEXT("niagara.set_emitter_enabled requires args.enabled (bool)"));
	}

	FString ComponentName;
	Request.Args->TryGetStringField(TEXT("component_name"), ComponentName);

	bool bAmbiguous = false;
	FString AmbiguityHint, ResolveErr;
	AActor* Actor = FMCPActorPathUtils::ResolveActor(ActorPath, /*bRejectPIE*/ false, bAmbiguous, AmbiguityHint, ResolveErr);
	if (!Actor)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("actor '%s' not found: %s%s"),
				*ActorPath, *ResolveErr, bAmbiguous ? *(FString(TEXT(" (candidates: ")) + AmbiguityHint + TEXT(")")) : TEXT("")));
	}

	// Collect Niagara components attached to the actor.
	TArray<UNiagaraComponent*> NiagaraComps;
	Actor->GetComponents<UNiagaraComponent>(NiagaraComps);
	if (NiagaraComps.Num() == 0)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorWrongClass,
			FString::Printf(TEXT("actor '%s' has no UNiagaraComponent"), *Actor->GetPathName()));
	}

	UNiagaraComponent* TargetComp = nullptr;
	if (!ComponentName.IsEmpty())
	{
		for (UNiagaraComponent* C : NiagaraComps)
		{
			if (C && C->GetName() == ComponentName) { TargetComp = C; break; }
		}
		if (!TargetComp)
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
				FString::Printf(TEXT("UNiagaraComponent named '%s' not found on actor '%s'"),
					*ComponentName, *Actor->GetPathName()));
		}
	}
	else if (NiagaraComps.Num() > 1)
	{
		FString CandidateList;
		for (int32 i = 0; i < NiagaraComps.Num() && i < 8; ++i)
		{
			CandidateList += (i == 0 ? TEXT("") : TEXT(", "));
			CandidateList += NiagaraComps[i]->GetName();
		}
		return FMCPToolHelpers::MakeError(Request, kMCPErrorAmbiguousComponent,
			FString::Printf(TEXT("actor '%s' has %d UNiagaraComponents; pass component_name to disambiguate (candidates: %s)"),
				*Actor->GetPathName(), NiagaraComps.Num(), *CandidateList));
	}
	else
	{
		TargetComp = NiagaraComps[0];
	}
	check(TargetComp);

	UNiagaraSystem* SysAsset = TargetComp->GetAsset();
	if (!SysAsset)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("UNiagaraComponent '%s' on actor '%s' has no system asset bound"),
				*TargetComp->GetName(), *Actor->GetPathName()));
	}

	const TArray<FNiagaraEmitterHandle>& Handles = SysAsset->GetEmitterHandles();
	if (EmitterIndex >= Handles.Num())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPropertyIndexOOB,
			FString::Printf(TEXT("emitter_index %d out of range (system has %d emitters)"),
				EmitterIndex, Handles.Num()));
	}

	const FName EmitterFName = Handles[EmitterIndex].GetName();
	TargetComp->SetEmitterEnable(EmitterFName, bEnabled);

	return FMCPJsonBuilder()
		.Str(TEXT("actor_path"), Actor->GetPathName())
		.Str(TEXT("component_name"), TargetComp->GetName())
		.Str(TEXT("emitter_name"), EmitterFName.ToString())
		.Num(TEXT("emitter_index"), static_cast<double>(EmitterIndex))
		.Bool(TEXT("enabled"), bEnabled)
		.BuildSuccess(Request);
}

// ─── niagara.spawn_at_location ────────────────────────────────────────────────────────────────
//
// Args:    { system_path: string, location: [x,y,z], rotation?: [pitch,yaw,roll] degrees,
//            scale?: [x,y,z] (default [1,1,1]), auto_destroy?: bool (default true) }
// Result:  { component_path: string, world: "editor"|"pie" }
//
// One-shot spawn via UNiagaraFunctionLibrary::SpawnSystemAtLocation. NO PIE guard — works in
// both editor and PIE worlds. World resolution: PIE first, editor fallback (mirrors DebugTools).
//
// Errors:
//   -32602 InvalidParams      missing system_path / location, or malformed vector
//   -32010 InvalidPath        system_path malformed / unknown mount
//   -32004 ObjectNotFound     system asset couldn't be loaded
//   -32011 WrongClass         asset isn't UNiagaraSystem
//   -32603 InternalError      no world available (GEditor missing — commandlet/cooker)
//                             OR SpawnSystemAtLocation returned null (e.g. system invalid for spawn)
FMCPResponse Tool_SpawnAtLocation(const FMCPRequest& Request)
{
	check(IsInGameThread());

	UWorld* World = NIA_ResolveWorld();
	if (!World)
	{
		return FMCPToolHelpers::MakeError(Request, kNIAErrorInternal, TEXT("no world available (GEditor missing — commandlet/cooker?)"));
	}

	FString Path;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("system_path"), Path, Err)) { return Err; }

	FVector Location, Rotation, Scale;
	FString ErrStr;
	if (!NIA_ReadVector3(Request.Args, TEXT("location"), Location, ErrStr))
	{
		return FMCPToolHelpers::MakeError(Request, kNIAErrorInvalidParams, ErrStr);
	}
	if (!NIA_ReadOptionalVector3(Request.Args, TEXT("rotation"), FVector::ZeroVector, Rotation, ErrStr))
	{
		return FMCPToolHelpers::MakeError(Request, kNIAErrorInvalidParams, ErrStr);
	}
	if (!NIA_ReadOptionalVector3(Request.Args, TEXT("scale"), FVector(1.0, 1.0, 1.0), Scale, ErrStr))
	{
		return FMCPToolHelpers::MakeError(Request, kNIAErrorInvalidParams, ErrStr);
	}

	bool bAutoDestroy = true;
	Request.Args->TryGetBoolField(TEXT("auto_destroy"), bAutoDestroy);

	int32 ErrCode = 0;
	FString ErrMsg;
	UNiagaraSystem* System = FMCPAssetLoader::Load<UNiagaraSystem>(Path, ErrCode, ErrMsg);
	if (!System) { return FMCPToolHelpers::MakeError(Request, ErrCode, ErrMsg); }

	// [pitch, yaw, roll] (degrees) → FRotator.
	const FRotator Rot(
		static_cast<float>(Rotation.X),
		static_cast<float>(Rotation.Y),
		static_cast<float>(Rotation.Z));

	UNiagaraComponent* Comp = UNiagaraFunctionLibrary::SpawnSystemAtLocation(
		World, System, Location, Rot, Scale,
		bAutoDestroy, /*bAutoActivate*/ true,
		ENCPoolMethod::None, /*bPreCullCheck*/ true);

	if (!Comp)
	{
		return FMCPToolHelpers::MakeError(Request, kNIAErrorInternal,
			FString::Printf(TEXT("UNiagaraFunctionLibrary::SpawnSystemAtLocation returned null for system '%s' "
				"(asset may be invalid for spawning, or world is uninitialised)"),
				*System->GetPathName()));
	}

	return FMCPJsonBuilder()
		.Str(TEXT("component_path"), Comp->GetPathName())
		.Str(TEXT("world"), NIA_WorldKindName(World))
		.Bool(TEXT("auto_destroy"), bAutoDestroy)
		.BuildSuccess(Request);
}

// ─── niagara.stop_all ─────────────────────────────────────────────────────────────────────────
//
// Args:    (none)
// Result:  { stopped_count: int, world: "editor"|"pie" }
//
// Enumerate every live UNiagaraComponent in the current world via TObjectIterator (filtered by
// GetWorld()), call DeactivateImmediate() on each. NO PIE guard. Returns the number of components
// that were active when stopped.
//
// Errors:
//   -32603 InternalError      no world available
FMCPResponse Tool_StopAll(const FMCPRequest& Request)
{
	check(IsInGameThread());

	UWorld* World = NIA_ResolveWorld();
	if (!World)
	{
		return FMCPToolHelpers::MakeError(Request, kNIAErrorInternal, TEXT("no world available (GEditor missing — commandlet/cooker?)"));
	}

	int32 StoppedCount = 0;
	for (TObjectIterator<UNiagaraComponent> It; It; ++It)
	{
		UNiagaraComponent* NC = *It;
		if (!IsValid(NC)) { continue; }
		if (NC->GetWorld() != World) { continue; }
		// IsActive captures the engine's "running" state; pooled-but-released components return false.
		if (!NC->IsActive()) { continue; }
		NC->DeactivateImmediate();
		++StoppedCount;
	}

	return FMCPJsonBuilder()
		.Num(TEXT("stopped_count"), static_cast<double>(StoppedCount))
		.Str(TEXT("world"), NIA_WorldKindName(World))
		.BuildSuccess(Request);
}

// ─── niagara.list_active ──────────────────────────────────────────────────────────────────────
//
// Args:    (none)
// Result:  { components: [{ component_path, owner_actor?, asset_path?, location, is_active,
//                            last_render_time }, ...],
//            count: int,
//            world: "editor"|"pie" }
//
// Walks TObjectIterator<UNiagaraComponent> filtered to the current world. Reports both active
// and recently-deactivated components (filter by ``is_active`` field on the caller side if you
// want only running ones). ``owner_actor`` is the outer AActor's path name if attached, else
// omitted (one-shot spawns parent themselves to the persistent level world settings, which is
// still an AActor — just typically the WorldSettings). NO PIE guard.
//
// ``last_render_time`` is ``UPrimitiveComponent::GetLastRenderTime()`` — the engine's
// game-time-seconds of the last frame this component was rendered. Compare against
// ``World->GetTimeSeconds()`` for "age since rendered".
//
// Errors:
//   -32603 InternalError      no world available
FMCPResponse Tool_ListActive(const FMCPRequest& Request)
{
	check(IsInGameThread());

	UWorld* World = NIA_ResolveWorld();
	if (!World)
	{
		return FMCPToolHelpers::MakeError(Request, kNIAErrorInternal, TEXT("no world available (GEditor missing — commandlet/cooker?)"));
	}

	TArray<TSharedPtr<FJsonValue>> Components;
	for (TObjectIterator<UNiagaraComponent> It; It; ++It)
	{
		UNiagaraComponent* NC = *It;
		if (!IsValid(NC)) { continue; }
		if (NC->GetWorld() != World) { continue; }

		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("component_path"), NC->GetPathName());

		if (AActor* Owner = NC->GetOwner())
		{
			Entry->SetStringField(TEXT("owner_actor"), Owner->GetPathName());
		}

		if (UNiagaraSystem* Asset = NC->GetAsset())
		{
			Entry->SetStringField(TEXT("asset_path"), Asset->GetPathName());
		}

		const FVector Loc = NC->GetComponentLocation();
		TArray<TSharedPtr<FJsonValue>> LocArr;
		LocArr.Add(MakeShared<FJsonValueNumber>(Loc.X));
		LocArr.Add(MakeShared<FJsonValueNumber>(Loc.Y));
		LocArr.Add(MakeShared<FJsonValueNumber>(Loc.Z));
		Entry->SetArrayField(TEXT("location"), LocArr);

		Entry->SetBoolField(TEXT("is_active"), NC->IsActive());
		Entry->SetNumberField(TEXT("last_render_time"), static_cast<double>(NC->GetLastRenderTime()));

		Components.Add(MakeShared<FJsonValueObject>(Entry));
	}

	const int32 ComponentsCount = Components.Num();
	return FMCPJsonBuilder()
		.Arr(TEXT("components"), MoveTemp(Components))
		.Num(TEXT("count"), static_cast<double>(ComponentsCount))
		.Str(TEXT("world"), NIA_WorldKindName(World))
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

	RegisterTool(TEXT("niagara.list_parameters"),     &Tool_ListParameters,    /*Lane A*/ false);
	// Wave B (2026-05): Niagara write surface.
	RegisterTool(TEXT("niagara.set_user_param"),      &Tool_SetUserParam,      /*Lane A*/ false);
	RegisterTool(TEXT("niagara.create_emitter"),      &Tool_CreateEmitter,     /*Lane A*/ false);
	RegisterTool(TEXT("niagara.set_emitter_enabled"), &Tool_SetEmitterEnabled, /*Lane A*/ false);
	// Wave E S2 (2026-05): Niagara runtime spawn / lifecycle.
	RegisterTool(TEXT("niagara.spawn_at_location"),   &Tool_SpawnAtLocation,   /*Lane A*/ false);
	RegisterTool(TEXT("niagara.stop_all"),            &Tool_StopAll,           /*Lane A*/ false);
	RegisterTool(TEXT("niagara.list_active"),         &Tool_ListActive,        /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("Niagara surface registered: 1 read (list_parameters) + 3 writes (set_user_param / create_emitter / set_emitter_enabled) + 3 runtime (spawn_at_location / stop_all / list_active), all Lane A"));
}

} // namespace FNiagaraTools

#undef LOCTEXT_NAMESPACE

MCP_REGISTER_SURFACE(NiagaraTools, &FNiagaraTools::Register)
