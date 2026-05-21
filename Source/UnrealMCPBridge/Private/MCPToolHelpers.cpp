// Copyright FatumGame. All Rights Reserved.

#include "MCPToolHelpers.h"

namespace FMCPToolHelpers
{
	void StampIds(const FMCPRequest& Request, FMCPResponse& Response)
	{
		Response.RequestId = Request.RequestId;
		Response.OriginalIdString = Request.OriginalIdString;
	}

	FMCPResponse MakeError(const FMCPRequest& Request, int32 Code, const FString& Message)
	{
		FMCPResponse R;
		StampIds(Request, R);
		R.bIsError = true;
		R.ErrorCode = Code;
		R.ErrorMessage = Message;
		return R;
	}

	FMCPResponse MakeSuccessObj(const FMCPRequest& Request, TSharedRef<FJsonObject> Result)
	{
		FMCPResponse R;
		StampIds(Request, R);
		R.bIsError = false;
		R.Result = MakeShared<FJsonValueObject>(Result);
		return R;
	}

	FMCPResponse MakeSuccessValue(const FMCPRequest& Request, TSharedRef<FJsonValue> Result)
	{
		FMCPResponse R;
		StampIds(Request, R);
		R.bIsError = false;
		R.Result = Result;
		return R;
	}

	bool RequireStringField(const FMCPRequest& Request, const TCHAR* FieldName, FString& OutValue, FMCPResponse& OutError)
	{
		if (!Request.Args.IsValid())
		{
			OutError = FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams, TEXT("missing args object"));
			return false;
		}
		if (!Request.Args->TryGetStringField(FieldName, OutValue) || OutValue.IsEmpty())
		{
			OutError = FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
				FString::Printf(TEXT("missing required string field '%s'"), FieldName));
			return false;
		}
		return true;
	}

	bool RequireNumberField(const FMCPRequest& Request, const TCHAR* FieldName, double& OutValue, FMCPResponse& OutError)
	{
		if (!Request.Args.IsValid())
		{
			OutError = FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams, TEXT("missing args object"));
			return false;
		}
		if (!Request.Args->TryGetNumberField(FieldName, OutValue))
		{
			OutError = FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
				FString::Printf(TEXT("missing required number field '%s'"), FieldName));
			return false;
		}
		return true;
	}

	bool RequireIntField(const FMCPRequest& Request, const TCHAR* FieldName, int32& OutValue, FMCPResponse& OutError)
	{
		double Tmp = 0.0;
		if (!RequireNumberField(Request, FieldName, Tmp, OutError))
		{
			return false;
		}
		OutValue = static_cast<int32>(Tmp);
		return true;
	}

	bool RequireBoolField(const FMCPRequest& Request, const TCHAR* FieldName, bool& OutValue, FMCPResponse& OutError)
	{
		if (!Request.Args.IsValid())
		{
			OutError = FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams, TEXT("missing args object"));
			return false;
		}
		if (!Request.Args->TryGetBoolField(FieldName, OutValue))
		{
			OutError = FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
				FString::Printf(TEXT("missing required bool field '%s'"), FieldName));
			return false;
		}
		return true;
	}

	bool RequireArrayField(const FMCPRequest& Request, const TCHAR* FieldName, const TArray<TSharedPtr<FJsonValue>>*& OutArrayPtr, FMCPResponse& OutError)
	{
		OutArrayPtr = nullptr;
		if (!Request.Args.IsValid())
		{
			OutError = FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams, TEXT("missing args object"));
			return false;
		}
		if (!Request.Args->TryGetArrayField(FieldName, OutArrayPtr) || OutArrayPtr == nullptr)
		{
			OutError = FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
				FString::Printf(TEXT("missing required array field '%s'"), FieldName));
			return false;
		}
		return true;
	}

	bool RequireObjectField(const FMCPRequest& Request, const TCHAR* FieldName, const TSharedPtr<FJsonObject>*& OutObjectPtr, FMCPResponse& OutError)
	{
		OutObjectPtr = nullptr;
		if (!Request.Args.IsValid())
		{
			OutError = FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams, TEXT("missing args object"));
			return false;
		}
		if (!Request.Args->TryGetObjectField(FieldName, OutObjectPtr) || OutObjectPtr == nullptr || !OutObjectPtr->IsValid())
		{
			OutError = FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
				FString::Printf(TEXT("missing required object field '%s'"), FieldName));
			return false;
		}
		return true;
	}
}
