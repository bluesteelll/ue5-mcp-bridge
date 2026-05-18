// Copyright FatumGame. All Rights Reserved.

#include "NiagaraTools.h"

#include "FMCPDispatchQueue.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPAssetPathUtils.h"

#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraParameterStore.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "NiagaraUserRedirectionParameterStore.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// NIA_ prefix per the unity-build symbol-collision pattern.
	constexpr int32 kNIAErrorInvalidParams = -32602;
	constexpr int32 kNIAErrorInternal      = -32603;

	void NIA_StampIds(const FMCPRequest& Request, FMCPResponse& Response)
	{
		Response.RequestId = Request.RequestId;
		Response.OriginalIdString = Request.OriginalIdString;
	}

	FMCPResponse NIA_MakeError(const FMCPRequest& Request, int32 Code, const FString& Message)
	{
		FMCPResponse R;
		NIA_StampIds(Request, R);
		R.bIsError = true;
		R.ErrorCode = Code;
		R.ErrorMessage = Message;
		return R;
	}

	FMCPResponse NIA_MakeSuccessObj(const FMCPRequest& Request, TSharedPtr<FJsonObject> Result)
	{
		FMCPResponse R;
		NIA_StampIds(Request, R);
		R.bIsError = false;
		R.Result = MakeShared<FJsonValueObject>(MoveTemp(Result));
		return R;
	}

	bool NIA_RequireStringField(const FMCPRequest& Request, const TCHAR* FieldName,
		FString& OutValue, FMCPResponse& OutError)
	{
		if (!Request.Args.IsValid())
		{
			OutError = NIA_MakeError(Request, kNIAErrorInvalidParams, TEXT("missing args object"));
			return false;
		}
		if (!Request.Args->TryGetStringField(FieldName, OutValue) || OutValue.IsEmpty())
		{
			OutError = NIA_MakeError(Request, kNIAErrorInvalidParams,
				FString::Printf(TEXT("missing required string field '%s'"), FieldName));
			return false;
		}
		return true;
	}

	// ─── NiagaraSystem resolution ────────────────────────────────────────────────────────────────

	/**
	 * Load a UNiagaraSystem by path. Mirror of FMCPMaterialUtils::LoadMaterialInterfaceByPath but
	 * for Niagara systems.
	 *
	 * Error map:
	 *   -32010 InvalidPath          — empty path, backslashes, unknown mount
	 *   -32004 ObjectNotFound       — LoadObject returned null
	 *   -32011 WrongClass           — loaded asset isn't a UNiagaraSystem
	 */
	UNiagaraSystem* NIA_LoadNiagaraSystemByPath(
		const FString& Path,
		int32& OutErrorCode,
		FString& OutError)
	{
		if (Path.IsEmpty())
		{
			OutErrorCode = kMCPErrorInvalidPath;
			OutError = TEXT("niagara_system_path is empty");
			return nullptr;
		}

		const FString Normalised = FMCPAssetPathUtils::Normalize(Path);
		if (Normalised.IsEmpty() || !FMCPAssetPathUtils::IsValidGameOrPlugin(Normalised))
		{
			OutErrorCode = kMCPErrorInvalidPath;
			OutError = FString::Printf(
				TEXT("niagara_system_path '%s' is malformed or references an unknown mount point"),
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
				TEXT("niagara_system_path '%s' could not be loaded (no asset found)"),
				*Path);
			return nullptr;
		}

		UNiagaraSystem* System = Cast<UNiagaraSystem>(Loaded);
		if (!System)
		{
			OutErrorCode = kMCPErrorWrongClass;
			OutError = FString::Printf(
				TEXT("niagara_system_path '%s' is class '%s'; expected UNiagaraSystem"),
				*Path, *Loaded->GetClass()->GetPathName());
			return nullptr;
		}
		return System;
	}

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
	if (!NIA_RequireStringField(Request, TEXT("niagara_system_path"), Path, Err)) { return Err; }

	int32 ErrCode = 0;
	FString ErrMsg;
	UNiagaraSystem* System = NIA_LoadNiagaraSystemByPath(Path, ErrCode, ErrMsg);
	if (!System) { return NIA_MakeError(Request, ErrCode, ErrMsg); }

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

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetArrayField(TEXT("user_params"),    UserArr);
	Out->SetArrayField(TEXT("system_params"),  SystemArr);
	Out->SetArrayField(TEXT("emitter_params"), EmitterArr);
	return NIA_MakeSuccessObj(Request, Out);
}

// ─── Registration ──────────────────────────────────────────────────────────────────────────────
void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	RegisterTool(TEXT("niagara.list_parameters"), &Tool_ListParameters, /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("Phase 5 Chunk C (Niagara): registered 1 niagara.* handler (list_parameters, Lane A)"));
}

} // namespace FNiagaraTools

#undef LOCTEXT_NAMESPACE
