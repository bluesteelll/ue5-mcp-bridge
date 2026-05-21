// Copyright FatumGame. All Rights Reserved.

#include "UFunctionTools.h"

#include "MCPSurfaceRegistry.h"

#include "FMCPDispatchQueue.h"
#include "MCPToolHelpers.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPPageCursor.h"
#include "Utils/MCPReflection.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/ScopeExit.h"
#include "UObject/Class.h"
#include "UObject/Field.h"
#include "UObject/Object.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// UFN_ prefix per the unity-build symbol-collision pattern. Per-surface error constants kept;
	// XX_MakeError/MakeSuccessObj removed in Phase 3 — use FMCPToolHelpers::Xxx from
	// MCPToolHelpers.h. NO PIE-guard / FScopedTransaction by design (bp.call_function legitimately
	// invokes UFUNCTIONs in PIE; bp.list_class_functions is read-only).
	constexpr int32 kUFNErrorInvalidParams = -32602;
	constexpr int32 kUFNErrorInternal     = -32603;

	/**
	 * Resolve target_path into (UClass, UObject) tuple.
	 *
	 * Path forms accepted:
	 *   - ``/Script/Module.ClassName``           — native UClass; result = (Class, Class->GetDefaultObject())
	 *   - ``/Game/.../BP.BP_C``                  — Blueprint generated class; result = (Class, CDO)
	 *   - ``/Game/.../Asset.Asset``              — instance object (e.g. data asset); result = (Asset->GetClass(), Asset)
	 *   - ``/Temp/Map.Map:PersistentLevel.X``    — actor/component instance
	 *
	 * Returns false on resolution failure with ErrCode/ErrMsg set. Otherwise OutClass + OutTarget
	 * both populated.
	 */
	bool UFN_ResolveTarget(const FString& TargetPath, UClass*& OutClass, UObject*& OutTarget,
		int32& OutErrCode, FString& OutErrMsg)
	{
		OutClass = nullptr;
		OutTarget = nullptr;

		if (TargetPath.IsEmpty())
		{
			OutErrCode = kUFNErrorInvalidParams;
			OutErrMsg = TEXT("target_path is empty");
			return false;
		}

		// Try as UClass first (covers /Script/Module.Class and /Game/.../BP.BP_C).
		UClass* AsClass = LoadClass<UObject>(nullptr, *TargetPath);
		if (!AsClass)
		{
			// Try BP autoload (_C suffix).
			const FString WithC = TargetPath.EndsWith(TEXT("_C")) ? TargetPath : (TargetPath + TEXT("_C"));
			AsClass = LoadClass<UObject>(nullptr, *WithC);
		}
		if (AsClass)
		{
			OutClass = AsClass;
			OutTarget = AsClass->GetDefaultObject();
			return true;
		}

		// Try as object path.
		UObject* AsObject = StaticLoadObject(UObject::StaticClass(), nullptr, *TargetPath);
		if (AsObject)
		{
			OutClass = AsObject->GetClass();
			OutTarget = AsObject;
			return true;
		}

		OutErrCode = kMCPErrorObjectNotFound;
		OutErrMsg = FString::Printf(TEXT("could not resolve target_path '%s' as UClass or UObject"),
			*TargetPath);
		return false;
	}

	/** Build a readable "Module.Class::Function(Param1, Param2, ...) -> ReturnType" signature. */
	FString UFN_BuildSignature(const UFunction* Func)
	{
		if (!Func) { return FString(); }
		FString Sig = Func->GetOwnerClass() ? Func->GetOwnerClass()->GetPathName() : FString(TEXT("?"));
		Sig += TEXT("::") + Func->GetName() + TEXT("(");

		bool bFirstParam = true;
		FProperty* ReturnProp = nullptr;
		for (TFieldIterator<FProperty> It(Func); It; ++It)
		{
			FProperty* P = *It;
			if (!(P->PropertyFlags & CPF_Parm)) { continue; }
			if (P->PropertyFlags & CPF_ReturnParm) { ReturnProp = P; continue; }
			if (!bFirstParam) { Sig += TEXT(", "); }
			bFirstParam = false;
			Sig += P->GetCPPType() + TEXT(" ") + P->GetName();
			if (P->PropertyFlags & CPF_OutParm) { Sig += TEXT(" /*out*/"); }
		}
		Sig += TEXT(")");
		if (ReturnProp) { Sig += TEXT(" -> ") + ReturnProp->GetCPPType(); }
		return Sig;
	}

	/** Stringify FUNC_* flags for tools.list metadata. */
	TArray<TSharedPtr<FJsonValue>> UFN_FlagsToJsonArray(uint32 FunctionFlags)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		#define UFN_FLAG(F) if (FunctionFlags & F) { Out.Add(MakeShared<FJsonValueString>(TEXT(#F))); }
		UFN_FLAG(FUNC_Final);
		UFN_FLAG(FUNC_RequiredAPI);
		UFN_FLAG(FUNC_BlueprintAuthorityOnly);
		UFN_FLAG(FUNC_BlueprintCosmetic);
		UFN_FLAG(FUNC_Net);
		UFN_FLAG(FUNC_NetReliable);
		UFN_FLAG(FUNC_NetRequest);
		UFN_FLAG(FUNC_Exec);
		UFN_FLAG(FUNC_Native);
		UFN_FLAG(FUNC_Event);
		UFN_FLAG(FUNC_NetResponse);
		UFN_FLAG(FUNC_Static);
		UFN_FLAG(FUNC_NetMulticast);
		UFN_FLAG(FUNC_UbergraphFunction);
		UFN_FLAG(FUNC_MulticastDelegate);
		UFN_FLAG(FUNC_Public);
		UFN_FLAG(FUNC_Private);
		UFN_FLAG(FUNC_Protected);
		UFN_FLAG(FUNC_Delegate);
		UFN_FLAG(FUNC_NetServer);
		UFN_FLAG(FUNC_HasOutParms);
		UFN_FLAG(FUNC_HasDefaults);
		UFN_FLAG(FUNC_NetClient);
		UFN_FLAG(FUNC_DLLImport);
		UFN_FLAG(FUNC_BlueprintCallable);
		UFN_FLAG(FUNC_BlueprintEvent);
		UFN_FLAG(FUNC_BlueprintPure);
		UFN_FLAG(FUNC_EditorOnly);
		UFN_FLAG(FUNC_Const);
		UFN_FLAG(FUNC_NetValidate);
		#undef UFN_FLAG
		return Out;
	}
}

namespace FUFunctionTools
{

// ─── bp.call_function ────────────────────────────────────────────────────────────────────────
//
// Args:
//   - target_path:    string (required)  /Script/Module.Class  OR  /Game/.../BP.BP_C  OR  /Path/To/Object
//   - function_name:  string (required)  UFUNCTION name (case-sensitive, must match exactly)
//   - args:           object (optional)  {ParamName: JsonValue, ...} — maps to UFUNCTION input params
//   - allow_any:      bool   (optional)  default false. true → bypass BlueprintCallable/Pure/Exec gate
//
// Result:
//   - called:             bool
//   - return_value:       any|null  (null if function has no return parm)
//   - out_params:         object    ({ParamName: value, ...} for CPF_OutParm)
//   - function_signature: string    debug-readable signature
//
// Error codes:
//   -32004 ObjectNotFound        target_path didn't resolve to UClass or UObject
//   -32005 PropertyNotFound      function_name not found on target's class
//   -32006 PropertyTypeMismatch  arg type couldn't be marshalled to UFUNCTION param type
//   -32007 PropertyAccessDenied  function isn't BlueprintCallable/Pure/Exec and allow_any not set
//   -32602 InvalidParams         missing target_path/function_name
//   -32603 InternalError         ProcessEvent threw (rare)
//
// Lane A. NO PIE-guard (caller decides — many crafting/inventory funcs are meant for PIE).
FMCPResponse Tool_CallFunction(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kUFNErrorInvalidParams,
			TEXT("bp.call_function requires args.target_path + args.function_name"));
	}

	FString TargetPath;
	FString FunctionName;
	if (!Request.Args->TryGetStringField(TEXT("target_path"), TargetPath) || TargetPath.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kUFNErrorInvalidParams,
			TEXT("missing required string field 'target_path'"));
	}
	if (!Request.Args->TryGetStringField(TEXT("function_name"), FunctionName) || FunctionName.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kUFNErrorInvalidParams,
			TEXT("missing required string field 'function_name'"));
	}

	// Resolve target.
	UClass* TargetClass = nullptr;
	UObject* TargetObject = nullptr;
	int32 ResolveErrCode = 0;
	FString ResolveErrMsg;
	if (!UFN_ResolveTarget(TargetPath, TargetClass, TargetObject, ResolveErrCode, ResolveErrMsg))
	{
		return FMCPToolHelpers::MakeError(Request, ResolveErrCode, ResolveErrMsg);
	}

	// Find function by name (walks parent classes via UClass::FindFunctionByName).
	UFunction* Func = TargetClass->FindFunctionByName(FName(*FunctionName));
	if (!Func)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPropertyNotFound,
			FString::Printf(TEXT("function '%s' not found on class '%s' (or its parents); "
				"use bp.list_class_functions to discover available functions"),
				*FunctionName, *TargetClass->GetPathName()));
	}

	// Safety gate.
	bool bAllowAny = false;
	Request.Args->TryGetBoolField(TEXT("allow_any"), bAllowAny);
	const uint32 FuncFlags = Func->FunctionFlags;
	if (!bAllowAny &&
		!(FuncFlags & (FUNC_BlueprintCallable | FUNC_BlueprintPure | FUNC_Exec)))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPropertyAccessDenied,
			FString::Printf(TEXT("function '%s' is not BlueprintCallable/BlueprintPure/Exec "
				"(FUNC flags=%u); pass args.allow_any=true to override"),
				*FunctionName, FuncFlags));
	}

	// Allocate parameter frame on the heap (avoids stack overflow for big param structs).
	TArray<uint8> ParmsBufferHeap;
	ParmsBufferHeap.SetNumZeroed(Func->ParmsSize);
	uint8* ParmsBuffer = ParmsBufferHeap.GetData();
	Func->InitializeStruct(ParmsBuffer);

	ON_SCOPE_EXIT { Func->DestroyStruct(ParmsBuffer); };

	// Marshal input args.
	const TSharedPtr<FJsonObject>* ArgsObj = nullptr;
	Request.Args->TryGetObjectField(TEXT("args"), ArgsObj);

	for (TFieldIterator<FProperty> It(Func); It; ++It)
	{
		FProperty* Prop = *It;
		const uint64 PFlags = Prop->PropertyFlags;
		if (!(PFlags & CPF_Parm)) { continue; }
		if (PFlags & CPF_ReturnParm) { continue; }
		// Skip pure-out params (no input value expected). CPF_ReferenceParm = inout, accept input.
		if ((PFlags & CPF_OutParm) && !(PFlags & CPF_ReferenceParm)) { continue; }

		const FString ParamName = Prop->GetName();
		if (!ArgsObj || !ArgsObj->Get() || !(*ArgsObj)->HasField(ParamName))
		{
			// Param not supplied — leave at zero-initialised default. Most BP functions tolerate.
			continue;
		}

		const TSharedPtr<FJsonValue> ArgValue = (*ArgsObj)->TryGetField(ParamName);
		void* ParamPtr = Prop->ContainerPtrToValuePtr<void>(ParmsBuffer);
		FString WriteErr;
		if (!FMCPReflection::WritePropertyValueAt(Prop, ParamPtr, ArgValue, TargetObject, WriteErr))
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorPropertyTypeMismatch,
				FString::Printf(TEXT("param '%s' (%s): %s"),
					*ParamName, *Prop->GetCPPType(), *WriteErr));
		}
	}

	// Invoke. ProcessEvent handles native/BP/event dispatch transparently.
	TargetObject->ProcessEvent(Func, ParmsBuffer);

	// Read return value + out params.
	TSharedPtr<FJsonValue> ReturnValue;
	TSharedRef<FJsonObject> OutParamsObj = MakeShared<FJsonObject>();
	for (TFieldIterator<FProperty> It(Func); It; ++It)
	{
		FProperty* Prop = *It;
		const uint64 PFlags = Prop->PropertyFlags;
		if (!(PFlags & CPF_Parm)) { continue; }

		void* ValPtr = Prop->ContainerPtrToValuePtr<void>(ParmsBuffer);
		if (PFlags & CPF_ReturnParm)
		{
			ReturnValue = FMCPReflection::ReadPropertyValueAt(Prop, ValPtr);
		}
		else if (PFlags & CPF_OutParm)
		{
			OutParamsObj->SetField(Prop->GetName(),
				FMCPReflection::ReadPropertyValueAt(Prop, ValPtr));
		}
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("called"), true);
	if (ReturnValue.IsValid())
	{
		Result->SetField(TEXT("return_value"), ReturnValue);
	}
	else
	{
		Result->SetField(TEXT("return_value"), MakeShared<FJsonValueNull>());
	}
	Result->SetObjectField(TEXT("out_params"), OutParamsObj);
	Result->SetStringField(TEXT("function_signature"), UFN_BuildSignature(Func));
	Result->SetStringField(TEXT("target_class"), TargetClass->GetPathName());
	Result->SetStringField(TEXT("target_object"), TargetObject->GetPathName());
	return FMCPToolHelpers::MakeSuccessObj(Request, Result);
}

// ─── bp.list_class_functions ────────────────────────────────────────────────────────────────
//
// Enumerate UFunctions on a UClass with full parameter introspection. Discovery tool used to
// find available callable functions before invoking via bp.call_function.
//
// Args:
//   - class_path:        string (required)  /Script/Module.Class  OR  /Game/.../BP.BP_C
//   - flags_filter:      [string] (optional)  filter to UFunctions matching ALL listed FUNC_*
//                                              flags (e.g. ["FUNC_BlueprintCallable"])
//   - include_inherited: bool (optional)     default true. Walk parent classes.
//   - prefix_filter:     string (optional)   case-insensitive function-name prefix
//   - page_token, page_size                  FMCPPageCursor sentinel pagination
//
// Result:
//   - functions[{name, signature, flags[], is_static, is_const, is_bp_callable, parameters[{name, cpp_type, is_in, is_out, is_return}], owner_class}]
//   - next_page_token, total_known
//
// Lane A.
FMCPResponse Tool_ListClassFunctions(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kUFNErrorInvalidParams,
			TEXT("bp.list_class_functions requires args.class_path"));
	}

	FString ClassPath;
	if (!Request.Args->TryGetStringField(TEXT("class_path"), ClassPath) || ClassPath.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kUFNErrorInvalidParams,
			TEXT("missing required string field 'class_path'"));
	}

	UClass* TargetClass = LoadClass<UObject>(nullptr, *ClassPath);
	if (!TargetClass)
	{
		const FString WithC = ClassPath.EndsWith(TEXT("_C")) ? ClassPath : (ClassPath + TEXT("_C"));
		TargetClass = LoadClass<UObject>(nullptr, *WithC);
	}
	if (!TargetClass)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorClassNotFound,
			FString::Printf(TEXT("could not LoadClass '%s' (also tried _C suffix)"), *ClassPath));
	}

	bool bIncludeInherited = true;
	Request.Args->TryGetBoolField(TEXT("include_inherited"), bIncludeInherited);

	FString PrefixFilter;
	Request.Args->TryGetStringField(TEXT("prefix_filter"), PrefixFilter);

	// Parse flags filter.
	uint32 FlagsRequired = 0;
	{
		const TArray<TSharedPtr<FJsonValue>>* FlagsArr = nullptr;
		if (Request.Args->TryGetArrayField(TEXT("flags_filter"), FlagsArr) && FlagsArr)
		{
			for (const TSharedPtr<FJsonValue>& V : *FlagsArr)
			{
				if (!V.IsValid()) { continue; }
				FString FlagName;
				if (!V->TryGetString(FlagName) || FlagName.IsEmpty()) { continue; }
				#define UFN_PARSE(F) if (FlagName.Equals(TEXT(#F), ESearchCase::IgnoreCase)) { FlagsRequired |= F; continue; }
				UFN_PARSE(FUNC_Final);
				UFN_PARSE(FUNC_BlueprintAuthorityOnly);
				UFN_PARSE(FUNC_BlueprintCosmetic);
				UFN_PARSE(FUNC_Net);
				UFN_PARSE(FUNC_Exec);
				UFN_PARSE(FUNC_Native);
				UFN_PARSE(FUNC_Event);
				UFN_PARSE(FUNC_Static);
				UFN_PARSE(FUNC_Public);
				UFN_PARSE(FUNC_Private);
				UFN_PARSE(FUNC_Protected);
				UFN_PARSE(FUNC_BlueprintCallable);
				UFN_PARSE(FUNC_BlueprintEvent);
				UFN_PARSE(FUNC_BlueprintPure);
				UFN_PARSE(FUNC_EditorOnly);
				UFN_PARSE(FUNC_Const);
				#undef UFN_PARSE
				return FMCPToolHelpers::MakeError(Request, kUFNErrorInvalidParams,
					FString::Printf(TEXT("unknown flag '%s' in flags_filter; see UFunction::FunctionFlags FUNC_* constants"),
						*FlagName));
			}
		}
	}

	int32 PageSize = 100;
	{
		int32 RawPageSize = 0;
		if (Request.Args->TryGetNumberField(TEXT("page_size"), RawPageSize))
		{
			PageSize = FMath::Clamp(RawPageSize, 1, 1000);
		}
	}

	// Filter hash binds discovery args.
	const uint64 FilterHash =
		static_cast<uint64>(GetTypeHash(ClassPath)) ^
		static_cast<uint64>(GetTypeHash(PrefixFilter)) ^
		(static_cast<uint64>(FlagsRequired) << 16) ^
		(static_cast<uint64>(bIncludeInherited ? 1 : 0) << 48) ^
		0xF4E700CA11000001ULL;

	FString PageTokenWire;
	Request.Args->TryGetStringField(TEXT("page_token"), PageTokenWire);
	FMCPPageCursor Cursor;
	if (!PageTokenWire.IsEmpty())
	{
		FString DecodeErr;
		if (!FMCPPageCursorUtils::Decode(PageTokenWire, Cursor, DecodeErr))
		{
			return FMCPToolHelpers::MakeError(Request, kUFNErrorInvalidParams,
				FString::Printf(TEXT("page_token decode failed: %s"), *DecodeErr));
		}
		if (Cursor.FilterHash != FilterHash)
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorStaleCursor,
				TEXT("page_token's filter_hash doesn't match current call's filter"));
		}
	}

	// Enumerate functions. IncludeSuper iterator handles inheritance.
	TArray<UFunction*> AllFuncs;
	const EFieldIteratorFlags::SuperClassFlags SuperFlag =
		bIncludeInherited ? EFieldIteratorFlags::IncludeSuper : EFieldIteratorFlags::ExcludeSuper;
	for (TFieldIterator<UFunction> It(TargetClass, SuperFlag); It; ++It)
	{
		UFunction* F = *It;
		if (!F) { continue; }
		if (FlagsRequired != 0 && (F->FunctionFlags & FlagsRequired) != FlagsRequired) { continue; }
		if (!PrefixFilter.IsEmpty() &&
			!F->GetName().StartsWith(PrefixFilter, ESearchCase::IgnoreCase))
		{
			continue;
		}
		AllFuncs.Add(F);
	}
	AllFuncs.StableSort([](const UFunction& A, const UFunction& B)
	{
		return A.GetName().Compare(B.GetName(), ESearchCase::IgnoreCase) < 0;
	});

	const int32 TotalKnown = AllFuncs.Num();

	int32 StartIdx = 0;
	if (!Cursor.LastAssetPath.IsEmpty())
	{
		while (StartIdx < TotalKnown)
		{
			if (AllFuncs[StartIdx]->GetName().Compare(Cursor.LastAssetPath, ESearchCase::IgnoreCase) > 0)
			{
				break;
			}
			++StartIdx;
		}
	}
	const int32 EndExcl = FMath::Min(TotalKnown, StartIdx + PageSize);

	TArray<TSharedPtr<FJsonValue>> Out;
	Out.Reserve(EndExcl - StartIdx);
	for (int32 i = StartIdx; i < EndExcl; ++i)
	{
		UFunction* F = AllFuncs[i];
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), F->GetName());
		Obj->SetStringField(TEXT("signature"), UFN_BuildSignature(F));
		Obj->SetArrayField(TEXT("flags"), UFN_FlagsToJsonArray(F->FunctionFlags));
		Obj->SetBoolField(TEXT("is_static"), (F->FunctionFlags & FUNC_Static) != 0);
		Obj->SetBoolField(TEXT("is_const"), (F->FunctionFlags & FUNC_Const) != 0);
		Obj->SetBoolField(TEXT("is_bp_callable"),
			(F->FunctionFlags & (FUNC_BlueprintCallable | FUNC_BlueprintPure | FUNC_Exec)) != 0);
		Obj->SetStringField(TEXT("owner_class"),
			F->GetOwnerClass() ? F->GetOwnerClass()->GetPathName() : FString());

		TArray<TSharedPtr<FJsonValue>> ParamsArr;
		for (TFieldIterator<FProperty> Pit(F); Pit; ++Pit)
		{
			FProperty* P = *Pit;
			if (!(P->PropertyFlags & CPF_Parm)) { continue; }
			TSharedRef<FJsonObject> ParamObj = MakeShared<FJsonObject>();
			ParamObj->SetStringField(TEXT("name"), P->GetName());
			ParamObj->SetStringField(TEXT("cpp_type"), P->GetCPPType());
			ParamObj->SetBoolField(TEXT("is_in"),
				!(P->PropertyFlags & CPF_ReturnParm) &&
				(!(P->PropertyFlags & CPF_OutParm) || (P->PropertyFlags & CPF_ReferenceParm)));
			ParamObj->SetBoolField(TEXT("is_out"), (P->PropertyFlags & CPF_OutParm) != 0);
			ParamObj->SetBoolField(TEXT("is_return"), (P->PropertyFlags & CPF_ReturnParm) != 0);
			ParamObj->SetBoolField(TEXT("is_ref"), (P->PropertyFlags & CPF_ReferenceParm) != 0);
			ParamsArr.Add(MakeShared<FJsonValueObject>(ParamObj));
		}
		Obj->SetArrayField(TEXT("parameters"), ParamsArr);

		Out.Add(MakeShared<FJsonValueObject>(Obj));
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("class_path"), TargetClass->GetPathName());
	Result->SetArrayField(TEXT("functions"), Out);
	Result->SetNumberField(TEXT("total_known"), static_cast<double>(TotalKnown));
	if (EndExcl < TotalKnown && Out.Num() > 0)
	{
		FMCPPageCursor NewCursor;
		NewCursor.FilterHash = FilterHash;
		NewCursor.LastAssetPath = AllFuncs[EndExcl - 1]->GetName();
		NewCursor.TotalKnownSnapshot = TotalKnown;
		Result->SetStringField(TEXT("next_page_token"), FMCPPageCursorUtils::Encode(NewCursor));
	}
	else
	{
		Result->SetField(TEXT("next_page_token"), MakeShared<FJsonValueNull>());
	}
	return FMCPToolHelpers::MakeSuccessObj(Request, Result);
}

void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	RegisterTool(TEXT("bp.call_function"),         &Tool_CallFunction,        /*Lane A*/ false);
	RegisterTool(TEXT("bp.list_class_functions"),  &Tool_ListClassFunctions,  /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("UFunction tools registered: bp.call_function + bp.list_class_functions "
			 "(generic reflective UFUNCTION invoker; covers BP function libraries, BP events, Exec, etc.)"));
}

} // namespace FUFunctionTools

#undef LOCTEXT_NAMESPACE

MCP_REGISTER_SURFACE(UFunctionTools, &FUFunctionTools::Register)
