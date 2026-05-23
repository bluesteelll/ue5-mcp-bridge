// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Wave H Surface 3 — Editor data-validation surface. 3 user-visible tools, all Lane A, no PIE
 * guard (read-only validation calls).
 *
 * Tool roster:
 *   data_validation.validate_asset  — Run UEditorValidatorSubsystem::IsObjectValidWithContext on a
 *                                      single loaded asset. Reports result ("valid" | "invalid" |
 *                                      "not_validated"), errors[], warnings[], validators_run (a
 *                                      conservative estimate based on ForEachEnabledValidator's
 *                                      enumeration count).
 *   data_validation.validate_path   — Enumerate assets under ``path_prefix`` via IAssetRegistry,
 *                                      run IsObjectValidWithContext on each, aggregate counts +
 *                                      collect per-failure detail. Optional ``recursive`` (default
 *                                      true) + ``max_assets`` (default 1000, hard cap 10000).
 *                                      No PIE guard (validation is read-only).
 *   data_validation.list_validators — Enumerate UEditorValidatorBase subclasses via TObjectIterator
 *                                      + their CDO's IsEnabled() state. Includes class_path,
 *                                      is_enabled, description (engine display-name when present,
 *                                      otherwise the class display name).
 *
 * **No PIE guard.** Validation is a read-only contextual analysis of the asset graph. It does NOT
 * mutate the asset, does NOT write to disk, and does NOT trigger transactions. Running during PIE
 * is harmless — same semantics as ``marshall.read_property`` / ``asset.list``.
 *
 * **FDataValidationContext API.** Use ``IsObjectValidWithContext(InObject, InContext)`` (the
 * single-arg overload returns errors/warnings as TArray<FText>& which we'd have to walk into a
 * fresh context anyway — going through the context API gives us the FIssue list directly with
 * severity attached). Iterate ``Context.GetIssues()`` → split on
 * EMessageSeverity::Error / EMessageSeverity::Warning. UE 5.7 message-log path is not exposed via
 * the bridge — tokenized messages render as their plain-text payload.
 *
 * **Validator enumeration.** No public API surfaces the registered validator set as
 * UClass* + IsEnabled — ``UEditorValidatorSubsystem::Validators`` is protected and
 * ``ForEachEnabledValidator`` only yields enabled validators (not disabled ones). For
 * ``list_validators`` we use ``TObjectIterator<UClass>`` filtered by
 * ``IsChildOf(UEditorValidatorBase::StaticClass())`` + ``!HasAnyClassFlags(CLASS_Abstract |
 * CLASS_Deprecated)``. Each class's CDO (``GetDefaultObject<UEditorValidatorBase>()``) reports
 * ``IsEnabled()`` (which folds bIsEnabled + bIsConfigDisabled).
 *
 * **Error codes (all reused from existing range — no new codes):**
 *   -32004 ObjectNotFound        Asset path doesn't load (validate_asset) or path_prefix yields
 *                                no assets (validate_path)
 *   -32010 InvalidPath           Malformed asset_path / path_prefix
 *   -32011 WrongClass            (Not currently raised — any UObject is validate-able)
 *   -32602 InvalidParams         Missing required field, max_assets out of [1, 10000]
 *   -32603 InternalError         UEditorValidatorSubsystem missing (GEditor null / commandlet)
 */
namespace FDataValidationTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	// ─── Wave H Surface 3: Data validation tools ────────────────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_ValidateAsset(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_ValidatePath(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_ListValidators(const FMCPRequest& Request);
}
