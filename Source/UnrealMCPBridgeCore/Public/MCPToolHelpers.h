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
	UNREALMCPBRIDGECORE_API void StampIds(const FMCPRequest& Request, FMCPResponse& Response);

	/** Build a structured error response (`bIsError=true`, ErrorCode, ErrorMessage, ids stamped). */
	UNREALMCPBRIDGECORE_API FMCPResponse MakeError(const FMCPRequest& Request, int32 Code, const FString& Message);

	/** Wrap a JSON object as a success response. The shared-ref shape avoids accidental nullptr. */
	UNREALMCPBRIDGECORE_API FMCPResponse MakeSuccessObj(const FMCPRequest& Request, TSharedRef<FJsonObject> Result);

	/**
	 * Overload accepting `TSharedPtr<FJsonObject>` for ergonomic interop with existing call sites
	 * that hold a TSharedPtr. The pointer MUST be non-null (the function `check`s). Phase 2-3
	 * migration uses both shapes; this overload avoids forcing every surface to `.ToSharedRef()`.
	 */
	UNREALMCPBRIDGECORE_API FMCPResponse MakeSuccessObj(const FMCPRequest& Request, TSharedPtr<FJsonObject> Result);

	/** Wrap an arbitrary JSON value as a success response (used for `Result` = number/array/bool). */
	UNREALMCPBRIDGECORE_API FMCPResponse MakeSuccessValue(const FMCPRequest& Request, TSharedRef<FJsonValue> Result);

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
	UNREALMCPBRIDGECORE_API bool RequireStringField(const FMCPRequest& Request, const TCHAR* FieldName, FString& OutValue, FMCPResponse& OutError);

	/** Same shape, double-valued field (use for numeric inputs). */
	UNREALMCPBRIDGECORE_API bool RequireNumberField(const FMCPRequest& Request, const TCHAR* FieldName, double& OutValue, FMCPResponse& OutError);

	/** Same shape, int32 field. Accepts either JSON integer or double-with-integer-value. */
	UNREALMCPBRIDGECORE_API bool RequireIntField(const FMCPRequest& Request, const TCHAR* FieldName, int32& OutValue, FMCPResponse& OutError);

	/** Same shape, boolean field. */
	UNREALMCPBRIDGECORE_API bool RequireBoolField(const FMCPRequest& Request, const TCHAR* FieldName, bool& OutValue, FMCPResponse& OutError);

	/**
	 * Same shape, array field. On success `OutArrayPtr` points to the array inside Request.Args
	 * (lifetime tied to Request.Args). NEVER mutate via this pointer — surfaces only read.
	 */
	UNREALMCPBRIDGECORE_API bool RequireArrayField(const FMCPRequest& Request, const TCHAR* FieldName, const TArray<TSharedPtr<FJsonValue>>*& OutArrayPtr, FMCPResponse& OutError);

	/**
	 * Same shape, object field. On success `OutObjectPtr` points to the shared-ptr inside
	 * Request.Args. Surfaces typically dereference once and re-wrap as a local TSharedPtr.
	 */
	UNREALMCPBRIDGECORE_API bool RequireObjectField(const FMCPRequest& Request, const TCHAR* FieldName, const TSharedPtr<FJsonObject>*& OutObjectPtr, FMCPResponse& OutError);

	/**
	 * Apply a JSON properties dict to a UObject via FMCPReflection::WritePropertyValue.
	 *
	 * For each {key, value} in Props:
	 *   - Looks up the FProperty by key name on Target's class via FindPropertyByName.
	 *   - If found AND WritePropertyValue succeeds → bare key name added to OutApplied.
	 *   - If not found → "<key>: property not found on class '<ClassName>'" added to OutSkipped.
	 *   - If found but WritePropertyValue rejects → "<key>: <error message>" added to OutSkipped.
	 *
	 * **Contract:** caller owns the FMCPMutatorScope / FMCPWritePropertyScope — this helper does
	 * NOT open a transaction or invoke Pre/PostEditChange. Designed for freshly-NewObject'd asset
	 * state where no editor listeners are bound yet; outer scope's MarkPackageDirty captures the
	 * change. For mutating already-published assets (Details panel observers), wrap each property
	 * apply in its own FMCPWritePropertyScope instead.
	 *
	 * **Skipped format note:** OutSkipped entries are "<name>: <reason>" strings (informative for
	 * AI debugging — caller iterates the array and parses on ":" if it needs just the name). This
	 * supersedes the bare-name format used by the earlier per-surface INP_ApplyProperties helper
	 * (Wave N+1); the richer format is consistent across all surfaces post-Wave-Q1 unification.
	 *
	 * **Replaces** Wave N+1 INP_ApplyProperties + Wave P AIBT_ApplyProperties + AIEQS_ApplyProperties
	 * (~150 LOC of duplication across 3 surfaces / 10 call sites).
	 *
	 * @param Target      Non-null UObject to mutate. `check()`-asserted.
	 * @param Props       Properties JSON object. May be invalid/null — function is no-op then.
	 * @param OutApplied  Property names that succeeded (bare strings).
	 * @param OutSkipped  "<name>: <reason>" entries for not-found or write-rejected fields.
	 * @return            OutApplied.Num() — count of properties successfully written this call.
	 */
	UNREALMCPBRIDGECORE_API int32 ApplyJsonProperties(
		UObject* Target,
		const TSharedPtr<FJsonObject>& Props,
		TArray<FString>& OutApplied,
		TArray<FString>& OutSkipped);
}
