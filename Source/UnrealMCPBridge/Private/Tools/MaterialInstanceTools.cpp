// Copyright FatumGame. All Rights Reserved.

#include "MaterialInstanceTools.h"

#include "FMCPDispatchQueue.h"
#include "MCPAssetLoader.h"
#include "MCPJsonBuilder.h"
#include "MCPMutatorScope.h"
#include "MCPToolHelpers.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPPageCursor.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/Texture.h"
#include "MaterialEditingLibrary.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	/**
	 * True iff the MIC has a local override entry for the given scalar parameter name. Checks the
	 * MIC's own ScalarParameterValues array (matches what UE editor shows as "bold/overridden").
	 *
	 * FMaterialParameterInfo equality matches on Name + Association + Index — Wave I S4 only
	 * operates on GlobalParameter scope (the common case), so a default-constructed info with the
	 * name is sufficient.
	 */
	bool MI_IsScalarOverridden(const UMaterialInstanceConstant* MIC, const FName& ParamName)
	{
		check(MIC);
		const FMaterialParameterInfo Target(ParamName);
		for (const FScalarParameterValue& V : MIC->ScalarParameterValues)
		{
			if (V.ParameterInfo == Target) { return true; }
		}
		return false;
	}

	bool MI_IsVectorOverridden(const UMaterialInstanceConstant* MIC, const FName& ParamName)
	{
		check(MIC);
		const FMaterialParameterInfo Target(ParamName);
		for (const FVectorParameterValue& V : MIC->VectorParameterValues)
		{
			if (V.ParameterInfo == Target) { return true; }
		}
		return false;
	}

	bool MI_IsTextureOverridden(const UMaterialInstanceConstant* MIC, const FName& ParamName)
	{
		check(MIC);
		const FMaterialParameterInfo Target(ParamName);
		for (const FTextureParameterValue& V : MIC->TextureParameterValues)
		{
			if (V.ParameterInfo == Target) { return true; }
		}
		return false;
	}

	/** {r,g,b,a} JSON array serialiser for FLinearColor — matches the brief's wire shape. */
	TSharedRef<FJsonValueArray> MI_LinearColorToArray(const FLinearColor& C)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Reserve(4);
		Arr.Add(MakeShared<FJsonValueNumber>(C.R));
		Arr.Add(MakeShared<FJsonValueNumber>(C.G));
		Arr.Add(MakeShared<FJsonValueNumber>(C.B));
		Arr.Add(MakeShared<FJsonValueNumber>(C.A));
		return MakeShared<FJsonValueArray>(MoveTemp(Arr));
	}

	/**
	 * Parse [r, g, b, a] from a JSON array. Returns false with OutErrorMessage populated if the
	 * shape is wrong (must be exactly 4 numeric entries).
	 */
	bool MI_ReadLinearColorArray(const TArray<TSharedPtr<FJsonValue>>& Arr, FLinearColor& OutC,
		FString& OutErrorMessage)
	{
		if (Arr.Num() != 4)
		{
			OutErrorMessage = FString::Printf(
				TEXT("vector value must be a 4-element [r,g,b,a] array; got %d elements"),
				Arr.Num());
			return false;
		}
		double Channels[4] = {0.0, 0.0, 0.0, 0.0};
		for (int32 i = 0; i < 4; ++i)
		{
			if (!Arr[i].IsValid() || !Arr[i]->TryGetNumber(Channels[i]))
			{
				OutErrorMessage = FString::Printf(
					TEXT("vector value element [%d] is not a number"), i);
				return false;
			}
		}
		OutC = FLinearColor(
			static_cast<float>(Channels[0]),
			static_cast<float>(Channels[1]),
			static_cast<float>(Channels[2]),
			static_cast<float>(Channels[3]));
		return true;
	}
} // namespace

namespace FMaterialInstanceTools
{

// ─── mat_inst.list ────────────────────────────────────────────────────────────────────────────
//
// Args:    { path_prefix?: string, page_size?: int (default 100, clamp [1,1000]), page_token?: string }
// Result:  { instances: [{ asset_path, parent_path }], total_known, next_page_token? }
//
// Read-only — no PIE guard. ``parent_path`` is read from the AR ``Parent`` tag where available
// (avoids loading the MIC just to print its parent reference); falls back to loading the asset
// when the tag is absent (e.g. older asset versions that didn't emit the tag).
FMCPResponse Tool_List(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString PathPrefix;
	if (Request.Args.IsValid()) { Request.Args->TryGetStringField(TEXT("path_prefix"), PathPrefix); }

	int32 PageSize = 100;
	if (Request.Args.IsValid()) { Request.Args->TryGetNumberField(TEXT("page_size"), PageSize); }
	PageSize = FMath::Clamp(PageSize, 1, 1000);

	FString PageToken;
	if (Request.Args.IsValid()) { Request.Args->TryGetStringField(TEXT("page_token"), PageToken); }

	const uint32 FilterHash = GetTypeHash(PathPrefix);

	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FARFilter Filter;
	Filter.ClassPaths.Add(UMaterialInstanceConstant::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = false;
	Filter.bRecursivePaths   = true;
	if (!PathPrefix.IsEmpty())
	{
		Filter.PackagePaths.Add(*PathPrefix);
	}
	TArray<FAssetData> Assets;
	AR.GetAssets(Filter, Assets);

	// Stable sort by ObjectPath (keyset pagination sort key).
	Assets.Sort([](const FAssetData& A, const FAssetData& B)
	{
		return A.GetSoftObjectPath().ToString() < B.GetSoftObjectPath().ToString();
	});

	// Decode + validate cursor.
	int32 StartIdx = 0;
	FMCPPageCursor InCursor;
	if (!PageToken.IsEmpty())
	{
		FString DecodeErr;
		if (!FMCPPageCursorUtils::Decode(PageToken, InCursor, DecodeErr))
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
				FString::Printf(TEXT("invalid page_token: %s"), *DecodeErr));
		}
		if (!FMCPPageCursorUtils::ValidateAgainstFilter(InCursor, FilterHash))
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorStaleCursor,
				TEXT("filter mutated between pages (path_prefix changed); restart pagination"));
		}
		while (StartIdx < Assets.Num() &&
			   Assets[StartIdx].GetSoftObjectPath().ToString() <= InCursor.LastAssetPath)
		{
			++StartIdx;
		}
	}

	static const FName ParentTag(TEXT("Parent"));

	TArray<TSharedPtr<FJsonValue>> InstanceArr;
	const int32 EndIdx = FMath::Min(StartIdx + PageSize, Assets.Num());
	InstanceArr.Reserve(EndIdx - StartIdx);
	for (int32 i = StartIdx; i < EndIdx; ++i)
	{
		const FAssetData& A = Assets[i];
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("asset_path"), A.GetSoftObjectPath().ToString());

		// Try the AR tag first — avoids loading the MIC just to read the parent reference.
		FString ParentTagValue;
		if (A.GetTagValue(ParentTag, ParentTagValue) && !ParentTagValue.IsEmpty())
		{
			Obj->SetStringField(TEXT("parent_path"), ParentTagValue);
		}
		else
		{
			// Fallback: load the asset and read MIC->Parent directly. Adds load cost but only for
			// assets whose AR tag is stale/missing (uncommon for current-version assets).
			if (UMaterialInstanceConstant* MIC = Cast<UMaterialInstanceConstant>(A.GetAsset()))
			{
				const UMaterialInterface* Parent = MIC->Parent;
				Obj->SetStringField(TEXT("parent_path"),
					Parent ? Parent->GetPathName() : FString());
			}
			else
			{
				Obj->SetStringField(TEXT("parent_path"), FString());
			}
		}

		InstanceArr.Add(MakeShared<FJsonValueObject>(Obj));
	}

	const int32 TotalKnown = Assets.Num();
	const bool bHasNextPage = (EndIdx < TotalKnown && EndIdx > 0);
	return FMCPJsonBuilder()
		.Arr(TEXT("instances"), MoveTemp(InstanceArr))
		.Num(TEXT("total_known"), TotalKnown)
		.If(bHasNextPage, [&](FMCPJsonBuilder& B)
		{
			FMCPPageCursor OutCursor;
			OutCursor.FilterHash = FilterHash;
			OutCursor.LastAssetPath = Assets[EndIdx - 1].GetSoftObjectPath().ToString();
			OutCursor.TotalKnownSnapshot = TotalKnown;
			B.Str(TEXT("next_page_token"), FMCPPageCursorUtils::Encode(OutCursor));
		})
		.BuildSuccess(Request);
}

// ─── mat_inst.get_params ──────────────────────────────────────────────────────────────────────
//
// Args:    { instance_path: string, include_inherited?: bool (default false) }
// Result:  {
//            scalar_params:  [{ name, value: float,        is_override: bool }, ...],
//            vector_params:  [{ name, value: [r,g,b,a],    is_override: bool }, ...],
//            texture_params: [{ name, texture_path: string, is_override: bool }, ...]
//          }
//
// Read-only — no PIE guard. ``include_inherited=false`` returns ONLY parameters present in the
// MIC's local override arrays (visibly bold in the editor). ``include_inherited=true`` walks the
// full parameter set known to the MIC's parent chain (via UMaterialEditingLibrary::Get*ParameterNames)
// and emits every one, with ``is_override`` toggling per-parameter.
//
// Errors: -32010 / -32004 / -32011 from MIC resolve.
FMCPResponse Tool_GetParams(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString InstancePath;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("instance_path"), InstancePath, Err)) { return Err; }

	bool bIncludeInherited = false;
	if (Request.Args.IsValid())
	{
		Request.Args->TryGetBoolField(TEXT("include_inherited"), bIncludeInherited);
	}

	int32 LoadErrCode = 0;
	FString LoadErrMsg;
	UMaterialInstanceConstant* MIC = FMCPAssetLoader::Load<UMaterialInstanceConstant>(InstancePath, LoadErrCode, LoadErrMsg);
	if (!MIC) { return FMCPToolHelpers::MakeError(Request, LoadErrCode, LoadErrMsg); }

	// Collect the canonical parameter name set from the parent chain (for include_inherited=true)
	// OR derive directly from the local override arrays (include_inherited=false).
	TArray<FName> ScalarNames, VectorNames, TextureNames;

	if (bIncludeInherited)
	{
		UMaterialEditingLibrary::GetScalarParameterNames(MIC, ScalarNames);
		UMaterialEditingLibrary::GetVectorParameterNames(MIC, VectorNames);
		UMaterialEditingLibrary::GetTextureParameterNames(MIC, TextureNames);
	}
	else
	{
		ScalarNames.Reserve(MIC->ScalarParameterValues.Num());
		for (const FScalarParameterValue& V : MIC->ScalarParameterValues)
		{
			ScalarNames.Add(V.ParameterInfo.Name);
		}
		VectorNames.Reserve(MIC->VectorParameterValues.Num());
		for (const FVectorParameterValue& V : MIC->VectorParameterValues)
		{
			VectorNames.Add(V.ParameterInfo.Name);
		}
		TextureNames.Reserve(MIC->TextureParameterValues.Num());
		for (const FTextureParameterValue& V : MIC->TextureParameterValues)
		{
			TextureNames.Add(V.ParameterInfo.Name);
		}
	}

	// Lex sort within each category — stable wire shape for diffing across calls.
	auto SortFn = [](const FName& A, const FName& B) { return A.LexicalLess(B); };
	ScalarNames.Sort(SortFn);
	VectorNames.Sort(SortFn);
	TextureNames.Sort(SortFn);

	TArray<TSharedPtr<FJsonValue>> ScalarArr;
	ScalarArr.Reserve(ScalarNames.Num());
	for (const FName& Name : ScalarNames)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Name.ToString());
		float Value = 0.0f;
		MIC->GetScalarParameterValue(FHashedMaterialParameterInfo(Name), Value);
		Obj->SetNumberField(TEXT("value"), Value);
		Obj->SetBoolField(TEXT("is_override"), MI_IsScalarOverridden(MIC, Name));
		ScalarArr.Add(MakeShared<FJsonValueObject>(Obj));
	}

	TArray<TSharedPtr<FJsonValue>> VectorArr;
	VectorArr.Reserve(VectorNames.Num());
	for (const FName& Name : VectorNames)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Name.ToString());
		FLinearColor Value = FLinearColor::Black;
		MIC->GetVectorParameterValue(FHashedMaterialParameterInfo(Name), Value);
		Obj->SetField(TEXT("value"), MI_LinearColorToArray(Value));
		Obj->SetBoolField(TEXT("is_override"), MI_IsVectorOverridden(MIC, Name));
		VectorArr.Add(MakeShared<FJsonValueObject>(Obj));
	}

	TArray<TSharedPtr<FJsonValue>> TextureArr;
	TextureArr.Reserve(TextureNames.Num());
	for (const FName& Name : TextureNames)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Name.ToString());
		UTexture* Tex = nullptr;
		MIC->GetTextureParameterValue(FHashedMaterialParameterInfo(Name), Tex);
		Obj->SetStringField(TEXT("texture_path"), Tex ? Tex->GetPathName() : FString());
		Obj->SetBoolField(TEXT("is_override"), MI_IsTextureOverridden(MIC, Name));
		TextureArr.Add(MakeShared<FJsonValueObject>(Obj));
	}

	return FMCPJsonBuilder()
		.Arr(TEXT("scalar_params"),  MoveTemp(ScalarArr))
		.Arr(TEXT("vector_params"),  MoveTemp(VectorArr))
		.Arr(TEXT("texture_params"), MoveTemp(TextureArr))
		.BuildSuccess(Request);
}

// ─── mat_inst.set_scalar_param ────────────────────────────────────────────────────────────────
//
// Args:    { instance_path: string, param_name: string, value: float }
// Result:  { set: bool, prior_value: float, prior_overridden: bool }
//
// PIE-guarded. FMCPMutatorScope wraps the write (PIE-guard + transaction + dirty-flush); the
// engine's SetMaterialInstance*ParameterValue internally invokes UpdateMaterialInstance to refresh
// shaders.
//
// Param-existence validation: the engine's SetMaterialInstance*ParameterValue does NOT validate
// the param name against the parent material's parameter set (it just adds to the override array
// regardless). We validate via the prior GetScalarParameterValue lookup — if that returns false,
// the param genuinely doesn't exist on the parent chain and we surface -32005 PropertyNotFound
// before mutating.
//
// Errors:
//   -32027 PIEActive
//   -32010 InvalidPath / -32004 ObjectNotFound / -32011 WrongClass (instance resolve)
//   -32005 PropertyNotFound (param_name unknown to parent material)
//   -32602 InvalidParams (missing/non-number 'value')
FMCPResponse Tool_SetScalarParam(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCPMatInstSetScalar", "MCP: set MIC scalar param"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString InstancePath, ParamNameStr;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("instance_path"), InstancePath, Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("param_name"),    ParamNameStr, Err)) { return Err; }

	double ValueDouble = 0.0;
	if (!FMCPToolHelpers::RequireNumberField(Request, TEXT("value"), ValueDouble, Err))
	{
		return Err;
	}
	const float ValueF = static_cast<float>(ValueDouble);

	int32 LoadErrCode = 0;
	FString LoadErrMsg;
	UMaterialInstanceConstant* MIC = FMCPAssetLoader::Load<UMaterialInstanceConstant>(InstancePath, LoadErrCode, LoadErrMsg);
	if (!MIC) { return FMCPToolHelpers::MakeError(Request, LoadErrCode, LoadErrMsg); }

	const FName ParamName(*ParamNameStr);

	// Capture prior state BEFORE the write. If the parameter doesn't exist on the parent chain at
	// all, GetScalarParameterValue returns false — that's our -32005 PropertyNotFound.
	float PriorValue = 0.0f;
	if (!MIC->GetScalarParameterValue(FHashedMaterialParameterInfo(ParamName), PriorValue))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPropertyNotFound,
			FString::Printf(
				TEXT("scalar parameter '%s' not found on instance '%s' (or its parent material)"),
				*ParamNameStr, *InstancePath));
	}
	const bool bPriorOverridden = MI_IsScalarOverridden(MIC, ParamName);

	// NOTE: UE 5.7 ``UMaterialEditingLibrary::SetMaterialInstance*ParameterValue`` ALWAYS returns
	// false (engine bug — bResult is never assigned to true; see MaterialEditingLibrary.cpp:1165).
	// We cannot use the return value for validation. Parameter-existence validation is performed
	// via the prior ``GetScalarParameterValue`` check above. The engine call also internally
	// invokes ``UpdateMaterialInstance`` — no need to call it again here.
	MIC->Modify();
	UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(MIC, ParamName, ValueF);
	Scope.DirtyPackage(MIC->GetOutermost());

	return FMCPJsonBuilder()
		.Bool(TEXT("set"), true)
		.Num(TEXT("prior_value"), PriorValue)
		.Bool(TEXT("prior_overridden"), bPriorOverridden)
		.BuildSuccess(Request);
}

// ─── mat_inst.set_vector_param ────────────────────────────────────────────────────────────────
//
// Args:    { instance_path: string, param_name: string, value: [r, g, b, a] }
// Result:  { set: bool, prior_value: [r,g,b,a], prior_overridden: bool }
//
// PIE-guarded. value MUST be a 4-element JSON array (wrong arity → -32602). Same error families
// as set_scalar_param.
FMCPResponse Tool_SetVectorParam(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCPMatInstSetVector", "MCP: set MIC vector param"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString InstancePath, ParamNameStr;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("instance_path"), InstancePath, Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("param_name"),    ParamNameStr, Err)) { return Err; }

	const TArray<TSharedPtr<FJsonValue>>* ValueArrPtr = nullptr;
	if (!FMCPToolHelpers::RequireArrayField(Request, TEXT("value"), ValueArrPtr, Err))
	{
		return Err;
	}
	FLinearColor Value;
	FString ReadErr;
	if (!MI_ReadLinearColorArray(*ValueArrPtr, Value, ReadErr))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams, ReadErr);
	}

	int32 LoadErrCode = 0;
	FString LoadErrMsg;
	UMaterialInstanceConstant* MIC = FMCPAssetLoader::Load<UMaterialInstanceConstant>(InstancePath, LoadErrCode, LoadErrMsg);
	if (!MIC) { return FMCPToolHelpers::MakeError(Request, LoadErrCode, LoadErrMsg); }

	const FName ParamName(*ParamNameStr);

	FLinearColor PriorValue = FLinearColor::Black;
	if (!MIC->GetVectorParameterValue(FHashedMaterialParameterInfo(ParamName), PriorValue))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPropertyNotFound,
			FString::Printf(
				TEXT("vector parameter '%s' not found on instance '%s' (or its parent material)"),
				*ParamNameStr, *InstancePath));
	}
	const bool bPriorOverridden = MI_IsVectorOverridden(MIC, ParamName);

	// See scalar setter for the engine-return-value note.
	MIC->Modify();
	UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(MIC, ParamName, Value);
	Scope.DirtyPackage(MIC->GetOutermost());

	return FMCPJsonBuilder()
		.Bool(TEXT("set"), true)
		.Field(TEXT("prior_value"), MI_LinearColorToArray(PriorValue))
		.Bool(TEXT("prior_overridden"), bPriorOverridden)
		.BuildSuccess(Request);
}

// ─── mat_inst.set_texture_param ───────────────────────────────────────────────────────────────
//
// Args:    { instance_path: string, param_name: string, texture_path: string }
// Result:  { set: bool, prior_texture_path: string, prior_overridden: bool }
//
// PIE-guarded. ``texture_path`` resolution mirrors the instance_path resolve: normalise +
// IsValidGameOrPlugin gate + LoadObject (package + object-path fallback) + UTexture cast.
//
// Errors: same as scalar PLUS -32004 ObjectNotFound when texture_path doesn't resolve, and
// -32011 WrongClass when texture_path resolves to a non-UTexture asset.
FMCPResponse Tool_SetTextureParam(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCPMatInstSetTexture", "MCP: set MIC texture param"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString InstancePath, ParamNameStr, TexturePathRaw;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("instance_path"), InstancePath,    Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("param_name"),    ParamNameStr,    Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("texture_path"),  TexturePathRaw,  Err)) { return Err; }

	int32 LoadErrCode = 0;
	FString LoadErrMsg;
	UMaterialInstanceConstant* MIC = FMCPAssetLoader::Load<UMaterialInstanceConstant>(InstancePath, LoadErrCode, LoadErrMsg);
	if (!MIC) { return FMCPToolHelpers::MakeError(Request, LoadErrCode, LoadErrMsg); }

	int32 TexErrCode = 0;
	FString TexErrMsg;
	UTexture* Texture = FMCPAssetLoader::Load<UTexture>(TexturePathRaw, TexErrCode, TexErrMsg);
	if (!Texture) { return FMCPToolHelpers::MakeError(Request, TexErrCode, TexErrMsg); }

	const FName ParamName(*ParamNameStr);

	UTexture* PriorTexture = nullptr;
	if (!MIC->GetTextureParameterValue(FHashedMaterialParameterInfo(ParamName), PriorTexture))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPropertyNotFound,
			FString::Printf(
				TEXT("texture parameter '%s' not found on instance '%s' (or its parent material)"),
				*ParamNameStr, *InstancePath));
	}
	const bool bPriorOverridden = MI_IsTextureOverridden(MIC, ParamName);

	// See scalar setter for the engine-return-value note.
	MIC->Modify();
	UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(MIC, ParamName, Texture);
	Scope.DirtyPackage(MIC->GetOutermost());

	return FMCPJsonBuilder()
		.Bool(TEXT("set"), true)
		.Str(TEXT("prior_texture_path"), PriorTexture ? PriorTexture->GetPathName() : FString())
		.Bool(TEXT("prior_overridden"), bPriorOverridden)
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

	RegisterTool(TEXT("mat_inst.list"),               &Tool_List,             /*Lane A*/ false);
	RegisterTool(TEXT("mat_inst.get_params"),         &Tool_GetParams,        /*Lane A*/ false);
	RegisterTool(TEXT("mat_inst.set_scalar_param"),   &Tool_SetScalarParam,   /*Lane A*/ false);
	RegisterTool(TEXT("mat_inst.set_vector_param"),   &Tool_SetVectorParam,   /*Lane A*/ false);
	RegisterTool(TEXT("mat_inst.set_texture_param"),  &Tool_SetTextureParam,  /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("MaterialInstance surface registered: 5 mat_inst.* tools "
			 "(list + get_params + set_scalar_param + set_vector_param + set_texture_param), all Lane A"));
}

} // namespace FMaterialInstanceTools

#include "MCPSurfaceRegistry.h"
MCP_REGISTER_SURFACE(MaterialInstanceTools, &FMaterialInstanceTools::Register)

#undef LOCTEXT_NAMESPACE
