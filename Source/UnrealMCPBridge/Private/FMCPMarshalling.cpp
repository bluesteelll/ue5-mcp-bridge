// Copyright FatumGame. All Rights Reserved.

#include "FMCPMarshalling.h"

#include "UnrealMCPBridge.h"
#include "Utility/MCPReflection.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/Class.h"
#include "UObject/Field.h"
#include "UObject/Object.h"
#include "UObject/UnrealType.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// NOTE: cannot name these `MakeError` / `MakeSuccess` — UE's global `MakeError` template
	// (`Templates/ValueOrError.h`) wins overload resolution and produces TValueOrError_ErrorProxy
	// instead of FMCPResponse. Prefix with MCP_ to disambiguate (anonymous-namespace alone is
	// insufficient because the global template participates in unqualified lookup).

	/** Build an error response with stamped ids. Keeps every handler from re-stamping by hand. */
	FMCPResponse MCP_MakeError(const FMCPRequest& Req, int32 Code, const FString& Message)
	{
		FMCPResponse Resp;
		Resp.RequestId = Req.RequestId;
		Resp.OriginalIdString = Req.OriginalIdString;
		Resp.bIsError = true;
		Resp.ErrorCode = Code;
		Resp.ErrorMessage = Message;
		return Resp;
	}

	/** Build a success response with stamped ids and an FJsonValue payload. */
	FMCPResponse MCP_MakeSuccess(const FMCPRequest& Req, TSharedPtr<FJsonValue> Result)
	{
		FMCPResponse Resp;
		Resp.RequestId = Req.RequestId;
		Resp.OriginalIdString = Req.OriginalIdString;
		Resp.bIsError = false;
		Resp.Result = MoveTemp(Result);
		return Resp;
	}

	/** Convenience: success response carrying a JSON object. */
	FMCPResponse MCP_MakeSuccessObj(const FMCPRequest& Req, TSharedPtr<FJsonObject> Obj)
	{
		return MCP_MakeSuccess(Req, MakeShared<FJsonValueObject>(MoveTemp(Obj)));
	}
} // namespace

// =====================================================================================================
// Public handler implementations — thin dispatch wrappers over FMCPReflection.
// =====================================================================================================

FMCPResponse FMCPMarshalling::ListProperties(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!Request.Args.IsValid())
	{
		return MCP_MakeError(Request, -32602, TEXT("marshall.list_properties requires args.object_path"));
	}
	FString ObjectPath;
	if (!Request.Args->TryGetStringField(TEXT("object_path"), ObjectPath) || ObjectPath.IsEmpty())
	{
		return MCP_MakeError(Request, -32602, TEXT("marshall.list_properties requires args.object_path (non-empty string)"));
	}

	UObject* Obj = FMCPReflection::ResolveObjectPath(ObjectPath);
	if (!Obj)
	{
		return MCP_MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("could not resolve object: %s"), *ObjectPath));
	}

	TArray<TSharedPtr<FJsonValue>> Items;
	for (TFieldIterator<FProperty> It(Obj->GetClass(), EFieldIteratorFlags::IncludeSuper); It; ++It)
	{
		Items.Add(MakeShared<FJsonValueObject>(FMCPReflection::MakePropertySummary(*It)));
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("object_path"), Obj->GetPathName());
	Result->SetStringField(TEXT("class"), Obj->GetClass()->GetName());
	Result->SetArrayField(TEXT("properties"), Items);
	return MCP_MakeSuccessObj(Request, Result);
}

FMCPResponse FMCPMarshalling::ReadProperty(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!Request.Args.IsValid())
	{
		return MCP_MakeError(Request, -32602, TEXT("marshall.read_property requires args.object_path + args.property_path"));
	}
	FString ObjectPath;
	FString PropertyPath;
	if (!Request.Args->TryGetStringField(TEXT("object_path"), ObjectPath) || ObjectPath.IsEmpty())
	{
		return MCP_MakeError(Request, -32602, TEXT("marshall.read_property: missing args.object_path"));
	}
	if (!Request.Args->TryGetStringField(TEXT("property_path"), PropertyPath) || PropertyPath.IsEmpty())
	{
		return MCP_MakeError(Request, -32602, TEXT("marshall.read_property: missing args.property_path"));
	}

	UObject* Obj = FMCPReflection::ResolveObjectPath(ObjectPath);
	if (!Obj)
	{
		return MCP_MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("could not resolve object: %s"), *ObjectPath));
	}

	UObject* Container = nullptr;
	void* LeafValuePtr = nullptr;
	FProperty* LeafProp = nullptr;
	int32 ErrCode = 0;
	FString ErrMsg;
	if (!FMCPReflection::ResolvePropertyPath(Obj, PropertyPath, Container, LeafValuePtr, LeafProp, ErrCode, ErrMsg))
	{
		return MCP_MakeError(Request, ErrCode, ErrMsg);
	}

	TSharedPtr<FJsonValue> Value = FMCPReflection::ReadPropertyValueAt(LeafProp, LeafValuePtr);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("object_path"), Obj->GetPathName());
	Result->SetStringField(TEXT("property_path"), PropertyPath);
	Result->SetStringField(TEXT("type"), FMCPReflection::DescribePropertyType(LeafProp));
	Result->SetField(TEXT("value"), Value);
	return MCP_MakeSuccessObj(Request, Result);
}

FMCPResponse FMCPMarshalling::WriteProperty(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!Request.Args.IsValid())
	{
		return MCP_MakeError(Request, -32602,
			TEXT("marshall.write_property requires args.object_path + property_path + value"));
	}
	FString ObjectPath;
	FString PropertyPath;
	if (!Request.Args->TryGetStringField(TEXT("object_path"), ObjectPath) || ObjectPath.IsEmpty())
	{
		return MCP_MakeError(Request, -32602, TEXT("marshall.write_property: missing args.object_path"));
	}
	if (!Request.Args->TryGetStringField(TEXT("property_path"), PropertyPath) || PropertyPath.IsEmpty())
	{
		return MCP_MakeError(Request, -32602, TEXT("marshall.write_property: missing args.property_path"));
	}
	const TSharedPtr<FJsonValue> ValueField = Request.Args->TryGetField(TEXT("value"));
	if (!ValueField.IsValid())
	{
		return MCP_MakeError(Request, -32602, TEXT("marshall.write_property: missing args.value"));
	}

	UObject* Obj = FMCPReflection::ResolveObjectPath(ObjectPath);
	if (!Obj)
	{
		return MCP_MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("could not resolve object: %s"), *ObjectPath));
	}

	UObject* Container = nullptr;
	void* LeafValuePtr = nullptr;
	FProperty* LeafProp = nullptr;
	int32 ErrCode = 0;
	FString ErrMsg;
	if (!FMCPReflection::ResolvePropertyPath(Obj, PropertyPath, Container, LeafValuePtr, LeafProp, ErrCode, ErrMsg))
	{
		return MCP_MakeError(Request, ErrCode, ErrMsg);
	}

	// Guard: don't write to BlueprintReadOnly / EditConst from outside the editor UI without a
	// flag — surface as -32007 PropertyAccessDenied so the client knows the schema, not the wire,
	// rejected the write. Caller can override by passing args.bypass_readonly=true if they really
	// need to (escape hatch for tooling that knows what it's doing — also required for CDO writes,
	// see smoke_ping.py sub-test 9b which writes FrameRateLimit on the GameUserSettings CDO).
	const bool bBypassReadOnly = Request.Args->HasField(TEXT("bypass_readonly")) &&
		Request.Args->GetBoolField(TEXT("bypass_readonly"));
	const uint64 Flags = LeafProp->PropertyFlags;
	if (!bBypassReadOnly && (Flags & (CPF_BlueprintReadOnly | CPF_EditConst | CPF_DisableEditOnInstance)))
	{
		return MCP_MakeError(Request, kMCPErrorPropertyAccessDenied,
			FString::Printf(
				TEXT("property '%s' is read-only (CPF flags=%llu); pass args.bypass_readonly=true to override"),
				*LeafProp->GetName(), static_cast<unsigned long long>(Flags)));
	}

	// RAII scope owns the 4-step contract (PreEditChange → Modify → write → PostEditChangeProperty).
	// ``FScopedTransaction`` inside the scope participates in editor Undo. ``Container`` is the same
	// UObject* as ``Obj`` (top-level owner), surfaced explicitly by ResolvePropertyPath for clarity.
	FString WriteError;
	bool bWriteOk = false;
	{
		FMCPWritePropertyScope Scope(Container, LeafProp, LOCTEXT("MCPWriteProperty", "MCP: write_property"));
		bWriteOk = FMCPReflection::WritePropertyValueAt(LeafProp, LeafValuePtr, ValueField, Container, WriteError);
	}
	// PostEditChangeProperty has fired on Scope destructor by this point.

	if (!bWriteOk)
	{
		return MCP_MakeError(Request, kMCPErrorPropertyTypeMismatch,
			FString::Printf(TEXT("write rejected: %s"), *WriteError));
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("object_path"), Obj->GetPathName());
	Result->SetStringField(TEXT("property_path"), PropertyPath);
	return MCP_MakeSuccessObj(Request, Result);
}

FMCPResponse FMCPMarshalling::DescribeStruct(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!Request.Args.IsValid())
	{
		return MCP_MakeError(Request, -32602, TEXT("marshall.describe_struct requires args.struct_type_path"));
	}
	FString StructPath;
	if (!Request.Args->TryGetStringField(TEXT("struct_type_path"), StructPath) || StructPath.IsEmpty())
	{
		return MCP_MakeError(Request, -32602,
			TEXT("marshall.describe_struct requires args.struct_type_path (non-empty string)"));
	}

	const UScriptStruct* Struct = FMCPReflection::ResolveScriptStructPath(StructPath);
	if (!Struct)
	{
		return MCP_MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("could not resolve UScriptStruct: %s"), *StructPath));
	}

	TArray<TSharedPtr<FJsonValue>> Items;
	for (TFieldIterator<FProperty> It(Struct, EFieldIteratorFlags::IncludeSuper); It; ++It)
	{
		Items.Add(MakeShared<FJsonValueObject>(FMCPReflection::MakePropertySummary(*It)));
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("struct_type_path"), Struct->GetStructPathName().ToString());
	Result->SetStringField(TEXT("name"), Struct->GetName());
	Result->SetNumberField(TEXT("size"), static_cast<double>(Struct->GetStructureSize()));
	Result->SetArrayField(TEXT("fields"), Items);
	return MCP_MakeSuccessObj(Request, Result);
}

#undef LOCTEXT_NAMESPACE
