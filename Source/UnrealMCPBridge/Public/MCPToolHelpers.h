// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MCPTypes.h"

/**
 * Shared helpers extracted from the 63 per-surface anonymous-namespace blocks. Every Phase 2+
 * surface uses these instead of duplicating XX_StampIds / XX_MakeError / XX_MakeSuccessObj /
 * XX_RequireXxxField. Replaces ~3 000 LOC of pure copy-paste.
 *
 * **Thread-safety:** All helpers are pure POD shuffling — no UObject access, no GEditor, no global
 * state. Safe to call from Lane B worker threads.
 */

/** JSON-RPC stdlib codes used by every surface. */
inline constexpr int32 kMCPErrorInvalidParams = -32602;
inline constexpr int32 kMCPErrorInternal      = -32603;

namespace FMCPToolHelpers
{
	/** Echo RequestId + OriginalIdString from request into response (every response stamps these). */
	UNREALMCPBRIDGE_API void StampIds(const FMCPRequest& Request, FMCPResponse& Response);

	/** Build a structured error response (`bIsError=true`, ErrorCode, ErrorMessage, ids stamped). */
	UNREALMCPBRIDGE_API FMCPResponse MakeError(const FMCPRequest& Request, int32 Code, const FString& Message);

	/** Wrap a JSON object as a success response. The shared-ref shape avoids accidental nullptr. */
	UNREALMCPBRIDGE_API FMCPResponse MakeSuccessObj(const FMCPRequest& Request, TSharedRef<FJsonObject> Result);

	/** Wrap an arbitrary JSON value as a success response (used for `Result` = number/array/bool). */
	UNREALMCPBRIDGE_API FMCPResponse MakeSuccessValue(const FMCPRequest& Request, TSharedRef<FJsonValue> Result);

	/**
	 * Read a required string field from `Request.Args`. On success: returns true, `OutValue` set,
	 * `OutError` left default. On failure: returns false, `OutError` populated with a -32602 error
	 * (missing args object, missing field, OR empty-string value all map to the same error).
	 *
	 * Caller pattern:
	 *     FMCPResponse Err;
	 *     FString Path;
	 *     if (!FMCPToolHelpers::RequireStringField(Request, TEXT("path"), Path, Err)) return Err;
	 */
	UNREALMCPBRIDGE_API bool RequireStringField(const FMCPRequest& Request, const TCHAR* FieldName, FString& OutValue, FMCPResponse& OutError);

	/** Same shape, double-valued field (use for numeric inputs). */
	UNREALMCPBRIDGE_API bool RequireNumberField(const FMCPRequest& Request, const TCHAR* FieldName, double& OutValue, FMCPResponse& OutError);

	/** Same shape, int32 field. Accepts either JSON integer or double-with-integer-value. */
	UNREALMCPBRIDGE_API bool RequireIntField(const FMCPRequest& Request, const TCHAR* FieldName, int32& OutValue, FMCPResponse& OutError);

	/** Same shape, boolean field. */
	UNREALMCPBRIDGE_API bool RequireBoolField(const FMCPRequest& Request, const TCHAR* FieldName, bool& OutValue, FMCPResponse& OutError);

	/**
	 * Same shape, array field. On success `OutArrayPtr` points to the array inside Request.Args
	 * (lifetime tied to Request.Args). NEVER mutate via this pointer — surfaces only read.
	 */
	UNREALMCPBRIDGE_API bool RequireArrayField(const FMCPRequest& Request, const TCHAR* FieldName, const TArray<TSharedPtr<FJsonValue>>*& OutArrayPtr, FMCPResponse& OutError);

	/**
	 * Same shape, object field. On success `OutObjectPtr` points to the shared-ptr inside
	 * Request.Args. Surfaces typically dereference once and re-wrap as a local TSharedPtr.
	 */
	UNREALMCPBRIDGE_API bool RequireObjectField(const FMCPRequest& Request, const TCHAR* FieldName, const TSharedPtr<FJsonObject>*& OutObjectPtr, FMCPResponse& OutError);
}
