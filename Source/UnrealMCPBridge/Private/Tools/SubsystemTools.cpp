// Copyright FatumGame. All Rights Reserved.

#include "SubsystemTools.h"

#include "MCPSurfaceRegistry.h"

#include "FMCPDispatchQueue.h"
#include "MCPToolHelpers.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPReflection.h"
#include "Utils/MCPWorldContext.h"

#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Misc/ScopeExit.h"
#include "EditorSubsystem.h"
#include "Subsystems/EngineSubsystem.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Subsystems/LocalPlayerSubsystem.h"
#include "Subsystems/Subsystem.h"
#include "Subsystems/WorldSubsystem.h"
#include "UObject/Class.h"
#include "UObject/Field.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	/**
	 * Resolve the "best" world for World/GameInstance/LocalPlayer subsystem enumeration.
	 *
	 * Prefer GEditor->PlayWorld when PIE is active (live runtime subsystems are typically what the
	 * caller cares about); fall back to the editor world otherwise. Returns null when neither is
	 * available (cooker / commandlet — shouldn't happen for an editor-only plugin but we guard).
	 */
	UWorld* SUB_ResolveBestWorld()
	{
		if (FMCPWorldContext::IsPIEActive() && GEditor && GEditor->PlayWorld)
		{
			return GEditor->PlayWorld;
		}
		return FMCPWorldContext::GetEditorWorld();
	}

	/** Resolve the first ULocalPlayer in the best world (PIE → editor preference). */
	ULocalPlayer* SUB_ResolveFirstLocalPlayer(UWorld* World)
	{
		if (!World) { return nullptr; }
		UGameInstance* GI = World->GetGameInstance();
		if (!GI) { return nullptr; }
		APlayerController* PC = GI->GetFirstLocalPlayerController(World);
		return PC ? PC->GetLocalPlayer() : nullptr;
	}

	/**
	 * Classify a USubsystem instance into one of the five canonical kinds. The order here matches
	 * the class hierarchy precedence — UEditorSubsystem and UGameInstanceSubsystem are both rooted
	 * directly at USubsystem, so the IsChildOf check sequence matters for disambiguation only when
	 * a single subsystem subclasses multiple bases (rare/synthetic — UE doesn't do this in stock).
	 */
	const TCHAR* SUB_ClassifyKind(const UClass* Class)
	{
		if (!Class) { return TEXT("unknown"); }
		if (Class->IsChildOf<UEngineSubsystem>())        { return TEXT("engine"); }
		if (Class->IsChildOf<UEditorSubsystem>())        { return TEXT("editor"); }
		if (Class->IsChildOf<UWorldSubsystem>())         { return TEXT("world"); }
		if (Class->IsChildOf<UGameInstanceSubsystem>())  { return TEXT("game_instance"); }
		if (Class->IsChildOf<ULocalPlayerSubsystem>())   { return TEXT("local_player"); }
		return TEXT("unknown");
	}

	/**
	 * Resolve a USubsystem instance by class path. Walks each of the 5 collections in turn (best
	 * world used for World/GI/LP). Returns nullptr + sets OutErrCode/OutError on failure.
	 *
	 * Failure modes:
	 *   -32020 ClassNotFound      LoadClass failed (path malformed or asset missing)
	 *   -32011 WrongClass         class loaded but is not a USubsystem subclass
	 *   -32004 ObjectNotFound     class is a valid subsystem subclass but no instance exists
	 *                             (typical: editor subsystem only instantiated when the editor
	 *                             is in a state that needs it, or world/GI subsystems before
	 *                             PIE start)
	 */
	USubsystem* SUB_ResolveSubsystemByClassPath(const FString& ClassPath,
		UClass*& OutClass, int32& OutErrCode, FString& OutError)
	{
		OutClass = nullptr;

		if (ClassPath.IsEmpty())
		{
			OutErrCode = kMCPErrorInvalidParams;
			OutError = TEXT("class_path is empty");
			return nullptr;
		}

		// Reuse the LoadClass + _C-suffix fallback pattern from UFunctionTools::UFN_ResolveTarget —
		// keeps Blueprint subclasses resolvable when callers omit the trailing _C.
		UClass* Class = LoadClass<UObject>(nullptr, *ClassPath);
		if (!Class)
		{
			const FString WithC = ClassPath.EndsWith(TEXT("_C")) ? ClassPath : (ClassPath + TEXT("_C"));
			Class = LoadClass<UObject>(nullptr, *WithC);
		}
		if (!Class)
		{
			OutErrCode = kMCPErrorClassNotFound;
			OutError = FString::Printf(TEXT("could not LoadClass '%s' (also tried _C suffix)"), *ClassPath);
			return nullptr;
		}

		OutClass = Class;

		// Engine subsystem path.
		if (Class->IsChildOf<UEngineSubsystem>())
		{
			if (!GEngine)
			{
				OutErrCode = kMCPErrorObjectNotFound;
				OutError = TEXT("GEngine is null (commandlet/cooker context)");
				return nullptr;
			}
			USubsystem* Inst = GEngine->GetEngineSubsystemBase(
				TSubclassOf<UEngineSubsystem>(Class));
			if (!Inst)
			{
				OutErrCode = kMCPErrorObjectNotFound;
				OutError = FString::Printf(
					TEXT("UEngineSubsystem '%s' is not instantiated (ShouldCreateSubsystem returned false?)"),
					*Class->GetPathName());
				return nullptr;
			}
			return Inst;
		}

		// Editor subsystem path.
		if (Class->IsChildOf<UEditorSubsystem>())
		{
			if (!GEditor)
			{
				OutErrCode = kMCPErrorObjectNotFound;
				OutError = TEXT("GEditor is null (commandlet/non-editor context)");
				return nullptr;
			}
			USubsystem* Inst = GEditor->GetEditorSubsystemBase(
				TSubclassOf<UEditorSubsystem>(Class));
			if (!Inst)
			{
				OutErrCode = kMCPErrorObjectNotFound;
				OutError = FString::Printf(
					TEXT("UEditorSubsystem '%s' is not instantiated"), *Class->GetPathName());
				return nullptr;
			}
			return Inst;
		}

		// World/GI/LP paths share the best-world resolver.
		UWorld* World = SUB_ResolveBestWorld();

		if (Class->IsChildOf<UWorldSubsystem>())
		{
			if (!World)
			{
				OutErrCode = kMCPErrorObjectNotFound;
				OutError = FString::Printf(
					TEXT("no World available for UWorldSubsystem '%s'"), *Class->GetPathName());
				return nullptr;
			}
			USubsystem* Inst = World->GetSubsystemBase(TSubclassOf<UWorldSubsystem>(Class));
			if (!Inst)
			{
				OutErrCode = kMCPErrorObjectNotFound;
				OutError = FString::Printf(
					TEXT("UWorldSubsystem '%s' is not instantiated in world '%s'"),
					*Class->GetPathName(), *World->GetPathName());
				return nullptr;
			}
			return Inst;
		}

		if (Class->IsChildOf<UGameInstanceSubsystem>())
		{
			UGameInstance* GI = World ? World->GetGameInstance() : nullptr;
			if (!GI)
			{
				OutErrCode = kMCPErrorObjectNotFound;
				OutError = FString::Printf(
					TEXT("no UGameInstance for UGameInstanceSubsystem '%s' (typically requires PIE)"),
					*Class->GetPathName());
				return nullptr;
			}
			USubsystem* Inst = GI->GetSubsystemBase(TSubclassOf<UGameInstanceSubsystem>(Class));
			if (!Inst)
			{
				OutErrCode = kMCPErrorObjectNotFound;
				OutError = FString::Printf(
					TEXT("UGameInstanceSubsystem '%s' is not instantiated"), *Class->GetPathName());
				return nullptr;
			}
			return Inst;
		}

		if (Class->IsChildOf<ULocalPlayerSubsystem>())
		{
			ULocalPlayer* LP = SUB_ResolveFirstLocalPlayer(World);
			if (!LP)
			{
				OutErrCode = kMCPErrorObjectNotFound;
				OutError = FString::Printf(
					TEXT("no ULocalPlayer for ULocalPlayerSubsystem '%s' (typically requires PIE)"),
					*Class->GetPathName());
				return nullptr;
			}
			USubsystem* Inst = LP->GetSubsystemBase(TSubclassOf<ULocalPlayerSubsystem>(Class));
			if (!Inst)
			{
				OutErrCode = kMCPErrorObjectNotFound;
				OutError = FString::Printf(
					TEXT("ULocalPlayerSubsystem '%s' is not instantiated"), *Class->GetPathName());
				return nullptr;
			}
			return Inst;
		}

		OutErrCode = kMCPErrorWrongClass;
		OutError = FString::Printf(
			TEXT("class '%s' is not a USubsystem subclass (must derive from UEngineSubsystem, "
				 "UEditorSubsystem, UWorldSubsystem, UGameInstanceSubsystem, or ULocalPlayerSubsystem)"),
			*Class->GetPathName());
		return nullptr;
	}

	/** Build the per-entry JSON object for ``subsystem.list``. */
	TSharedRef<FJsonObject> SUB_MakeListEntry(const USubsystem* Inst,
		const TCHAR* Kind, const FString& OwnerContext)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("class_path"),
			Inst && Inst->GetClass() ? Inst->GetClass()->GetPathName() : FString());
		Obj->SetStringField(TEXT("kind"), Kind);
		Obj->SetStringField(TEXT("owner_context"), OwnerContext);
		return Obj;
	}
} // namespace

namespace FSubsystemTools
{

// ─── subsystem.list ───────────────────────────────────────────────────────────────────────────
//
// Args:
//   - kind?: string  one of "engine" | "editor" | "world" | "game_instance" | "local_player" | "all"
//                    (default "all"). Unknown value → -32602 InvalidParams.
//
// Result:
//   - subsystems: [{ class_path, kind, owner_context }]
//   - total: int
//
// Owner context strings:
//   engine        → "GEngine"
//   editor        → "GEditor"
//   world         → World's path name (e.g. "/Temp/UEDPIE_0_FloorMap.FloorMap")
//   game_instance → GameInstance's path name
//   local_player  → LocalPlayer's path name (e.g. "/Engine/Transient.LocalPlayer_0")
//
// Lane A. NO PIE guard — read-only.
FMCPResponse Tool_List(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString Kind = TEXT("all");
	if (Request.Args.IsValid())
	{
		Request.Args->TryGetStringField(TEXT("kind"), Kind);
	}

	// Normalise to lowercase for the comparison switch; rejected unknowns include the original
	// spelling in the error message so the caller can see what they sent.
	const FString KindLower = Kind.ToLower();
	const bool bAll          = (KindLower == TEXT("all"));
	const bool bEngine       = bAll || (KindLower == TEXT("engine"));
	const bool bEditor       = bAll || (KindLower == TEXT("editor"));
	const bool bWorld        = bAll || (KindLower == TEXT("world"));
	const bool bGameInstance = bAll || (KindLower == TEXT("game_instance"));
	const bool bLocalPlayer  = bAll || (KindLower == TEXT("local_player"));

	if (!bAll && !bEngine && !bEditor && !bWorld && !bGameInstance && !bLocalPlayer)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			FString::Printf(
				TEXT("unknown kind '%s'; expected one of: engine|editor|world|game_instance|local_player|all"),
				*Kind));
	}

	TArray<TSharedPtr<FJsonValue>> Items;

	// Engine subsystems.
	if (bEngine && GEngine)
	{
		const TArray<UEngineSubsystem*> ESubs = GEngine->GetEngineSubsystemArrayCopy<UEngineSubsystem>();
		Items.Reserve(Items.Num() + ESubs.Num());
		for (UEngineSubsystem* S : ESubs)
		{
			if (!S) { continue; }
			Items.Add(MakeShared<FJsonValueObject>(
				SUB_MakeListEntry(S, SUB_ClassifyKind(S->GetClass()), TEXT("GEngine"))));
		}
	}

	// Editor subsystems.
	if (bEditor && GEditor)
	{
		const TArray<UEditorSubsystem*> EdSubs = GEditor->GetEditorSubsystemArrayCopy<UEditorSubsystem>();
		Items.Reserve(Items.Num() + EdSubs.Num());
		for (UEditorSubsystem* S : EdSubs)
		{
			if (!S) { continue; }
			Items.Add(MakeShared<FJsonValueObject>(
				SUB_MakeListEntry(S, SUB_ClassifyKind(S->GetClass()), TEXT("GEditor"))));
		}
	}

	// World / GameInstance / LocalPlayer share the best-world resolver.
	UWorld* World = (bWorld || bGameInstance || bLocalPlayer) ? SUB_ResolveBestWorld() : nullptr;

	if (bWorld && World)
	{
		const TArray<UWorldSubsystem*> WSubs = World->GetSubsystemArrayCopy<UWorldSubsystem>();
		const FString WorldCtx = World->GetPathName();
		Items.Reserve(Items.Num() + WSubs.Num());
		for (UWorldSubsystem* S : WSubs)
		{
			if (!S) { continue; }
			Items.Add(MakeShared<FJsonValueObject>(
				SUB_MakeListEntry(S, SUB_ClassifyKind(S->GetClass()), WorldCtx)));
		}
	}

	if (bGameInstance && World)
	{
		UGameInstance* GI = World->GetGameInstance();
		if (GI)
		{
			const TArray<UGameInstanceSubsystem*> GISubs =
				GI->GetSubsystemArrayCopy<UGameInstanceSubsystem>();
			const FString GICtx = GI->GetPathName();
			Items.Reserve(Items.Num() + GISubs.Num());
			for (UGameInstanceSubsystem* S : GISubs)
			{
				if (!S) { continue; }
				Items.Add(MakeShared<FJsonValueObject>(
					SUB_MakeListEntry(S, SUB_ClassifyKind(S->GetClass()), GICtx)));
			}
		}
	}

	if (bLocalPlayer && World)
	{
		ULocalPlayer* LP = SUB_ResolveFirstLocalPlayer(World);
		if (LP)
		{
			const TArray<ULocalPlayerSubsystem*> LPSubs =
				LP->GetSubsystemArrayCopy<ULocalPlayerSubsystem>();
			const FString LPCtx = LP->GetPathName();
			Items.Reserve(Items.Num() + LPSubs.Num());
			for (ULocalPlayerSubsystem* S : LPSubs)
			{
				if (!S) { continue; }
				Items.Add(MakeShared<FJsonValueObject>(
					SUB_MakeListEntry(S, SUB_ClassifyKind(S->GetClass()), LPCtx)));
			}
		}
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetArrayField(TEXT("subsystems"), Items);
	Out->SetNumberField(TEXT("total"), Items.Num());
	Out->SetStringField(TEXT("kind_filter"), KindLower);
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── subsystem.get_property ───────────────────────────────────────────────────────────────────
//
// Args:
//   - class_path:     string (required)  /Script/Module.SubsystemClass (e.g.
//                                          "/Script/UnrealEd.EditorAssetSubsystem")
//   - property_name:  string (required)  top-level UPROPERTY name (case-sensitive; FProperty
//                                          lookup walks parent classes)
//
// Result:
//   - class_path:     string  echo of resolved class path
//   - property_name:  string  echo of property name
//   - type:           string  FProperty::GetCPPType (e.g. "int32", "FVector", "TArray<float>")
//   - value:          any     typed JSON via FMCPReflection::ReadPropertyValueAt
//
// Lane A. NO PIE guard — read-only.
FMCPResponse Tool_GetProperty(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			TEXT("subsystem.get_property requires args.class_path + args.property_name"));
	}

	FString ClassPath;
	FString PropertyName;
	if (!Request.Args->TryGetStringField(TEXT("class_path"), ClassPath) || ClassPath.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			TEXT("missing required string field 'class_path'"));
	}
	if (!Request.Args->TryGetStringField(TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			TEXT("missing required string field 'property_name'"));
	}

	UClass* SubsystemClass = nullptr;
	int32 ResolveErrCode = 0;
	FString ResolveErrMsg;
	USubsystem* Instance = SUB_ResolveSubsystemByClassPath(
		ClassPath, SubsystemClass, ResolveErrCode, ResolveErrMsg);
	if (!Instance)
	{
		return FMCPToolHelpers::MakeError(Request, ResolveErrCode, ResolveErrMsg);
	}
	check(SubsystemClass != nullptr);

	// FindPropertyByName walks the parent class chain. NOT a "FindFieldChecked" — we want a
	// graceful -32005 PropertyNotFound on miss.
	FProperty* Prop = SubsystemClass->FindPropertyByName(FName(*PropertyName));
	if (!Prop)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPropertyNotFound,
			FString::Printf(TEXT("property '%s' not found on subsystem class '%s' (or its parents); "
				"use marshall.list_properties on the subsystem instance to enumerate available properties"),
				*PropertyName, *SubsystemClass->GetPathName()));
	}

	// Read the value. ContainerPtrToValuePtr is safe — Prop comes from SubsystemClass which Instance
	// is guaranteed to be (or derive from).
	const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Instance);
	TSharedPtr<FJsonValue> Value = FMCPReflection::ReadPropertyValueAt(Prop, ValuePtr);

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("class_path"), SubsystemClass->GetPathName());
	Out->SetStringField(TEXT("property_name"), PropertyName);
	Out->SetStringField(TEXT("type"), FMCPReflection::DescribePropertyType(Prop));
	Out->SetField(TEXT("value"), Value.IsValid() ? Value : MakeShared<FJsonValueNull>());
	Out->SetStringField(TEXT("instance_path"), Instance->GetPathName());
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── subsystem.call_function ──────────────────────────────────────────────────────────────────
//
// Args:
//   - class_path:    string (required)  /Script/Module.SubsystemClass
//   - function_name: string (required)  UFUNCTION name (case-sensitive)
//   - args:          object (optional)  {ParamName: JsonValue, ...}
//   - allow_any:     bool   (optional)  default false. true → bypass BPCallable/Pure/Exec gate
//
// Result:
//   - class_path:          string  echo of resolved class path
//   - function_name:       string  echo of function name
//   - function_signature:  string  debug-readable signature ("Class::Func(Args) -> Return")
//   - return_value:        any|null
//   - out_params:          object  ({ParamName: value, ...} for CPF_OutParm)
//   - is_state_changing:   bool    !(FUNC_Const | FUNC_BlueprintPure) — heuristic; caller
//                                    responsible for PIE-side effects
//   - instance_path:       string  the subsystem instance's UObject path (for diagnostic)
//
// Lane A. NO PIE guard — caller responsibility (matches bp.call_function precedent). The
// is_state_changing field surfaces a heuristic so the caller can post-hoc decide whether the call
// likely mutated state, but we don't block based on it.
//
// Function safety gate: by default only FUNC_BlueprintCallable | FUNC_BlueprintPure | FUNC_Exec
// are callable. Pass args.allow_any=true to bypass.
FMCPResponse Tool_CallFunction(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			TEXT("subsystem.call_function requires args.class_path + args.function_name"));
	}

	FString ClassPath;
	FString FunctionName;
	if (!Request.Args->TryGetStringField(TEXT("class_path"), ClassPath) || ClassPath.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			TEXT("missing required string field 'class_path'"));
	}
	if (!Request.Args->TryGetStringField(TEXT("function_name"), FunctionName) || FunctionName.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			TEXT("missing required string field 'function_name'"));
	}

	UClass* SubsystemClass = nullptr;
	int32 ResolveErrCode = 0;
	FString ResolveErrMsg;
	USubsystem* Instance = SUB_ResolveSubsystemByClassPath(
		ClassPath, SubsystemClass, ResolveErrCode, ResolveErrMsg);
	if (!Instance)
	{
		return FMCPToolHelpers::MakeError(Request, ResolveErrCode, ResolveErrMsg);
	}
	check(SubsystemClass != nullptr);

	UFunction* Func = SubsystemClass->FindFunctionByName(FName(*FunctionName));
	if (!Func)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPropertyNotFound,
			FString::Printf(TEXT("function '%s' not found on subsystem class '%s' (or its parents); "
				"use bp.list_class_functions on the class path to enumerate available functions"),
				*FunctionName, *SubsystemClass->GetPathName()));
	}

	// Safety gate — mirrors bp.call_function precedent so the wire contract is consistent.
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

	// Allocate parameter frame on the heap (avoids stack overflow for big param structs — same
	// pattern as bp.call_function).
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
		if (!FMCPReflection::WritePropertyValueAt(Prop, ParamPtr, ArgValue, Instance, WriteErr))
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorPropertyTypeMismatch,
				FString::Printf(TEXT("param '%s' (%s): %s"),
					*ParamName, *Prop->GetCPPType(), *WriteErr));
		}
	}

	// Invoke. ProcessEvent handles native/BP/event dispatch transparently.
	Instance->ProcessEvent(Func, ParmsBuffer);

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

	// Build function signature (matches UFN_BuildSignature shape).
	FString Signature = SubsystemClass->GetPathName() + TEXT("::") + FunctionName + TEXT("(");
	{
		bool bFirst = true;
		FProperty* RetProp = nullptr;
		for (TFieldIterator<FProperty> It(Func); It; ++It)
		{
			FProperty* P = *It;
			if (!(P->PropertyFlags & CPF_Parm)) { continue; }
			if (P->PropertyFlags & CPF_ReturnParm) { RetProp = P; continue; }
			if (!bFirst) { Signature += TEXT(", "); }
			bFirst = false;
			Signature += P->GetCPPType() + TEXT(" ") + P->GetName();
			if (P->PropertyFlags & CPF_OutParm) { Signature += TEXT(" /*out*/"); }
		}
		Signature += TEXT(")");
		if (RetProp) { Signature += TEXT(" -> ") + RetProp->GetCPPType(); }
	}

	// State-changing heuristic: FUNC_Const = "this method doesn't mutate", FUNC_BlueprintPure =
	// "pure getter", anything else assumed to mutate. NOT authoritative — a non-const non-pure
	// function might still be a pure getter that just isn't tagged correctly. Caller responsibility.
	const bool bIsStateChanging =
		!((FuncFlags & FUNC_Const) || (FuncFlags & FUNC_BlueprintPure));

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("class_path"), SubsystemClass->GetPathName());
	Result->SetStringField(TEXT("function_name"), FunctionName);
	Result->SetStringField(TEXT("function_signature"), Signature);
	if (ReturnValue.IsValid())
	{
		Result->SetField(TEXT("return_value"), ReturnValue);
	}
	else
	{
		Result->SetField(TEXT("return_value"), MakeShared<FJsonValueNull>());
	}
	Result->SetObjectField(TEXT("out_params"), OutParamsObj);
	Result->SetBoolField(TEXT("is_state_changing"), bIsStateChanging);
	Result->SetStringField(TEXT("instance_path"), Instance->GetPathName());
	return FMCPToolHelpers::MakeSuccessObj(Request, Result);
}

// ─── Registration ──────────────────────────────────────────────────────────────────────────────
void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	RegisterTool(TEXT("subsystem.list"),          &Tool_List,         /*Lane A*/ false);
	RegisterTool(TEXT("subsystem.get_property"),  &Tool_GetProperty,  /*Lane A*/ false);
	RegisterTool(TEXT("subsystem.call_function"), &Tool_CallFunction, /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("Subsystem surface registered: 3 subsystem.* tools "
			 "(list + get_property + call_function), all Lane A. Covers all 5 UE subsystem "
			 "collections (Engine/Editor/World/GameInstance/LocalPlayer)."));
}

} // namespace FSubsystemTools

#undef LOCTEXT_NAMESPACE

MCP_REGISTER_SURFACE(SubsystemTools, &FSubsystemTools::Register)
