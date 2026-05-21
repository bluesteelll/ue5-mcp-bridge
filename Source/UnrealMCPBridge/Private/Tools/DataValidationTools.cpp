// Copyright FatumGame. All Rights Reserved.

#include "DataValidationTools.h"

#include "FMCPDispatchQueue.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPAssetPathUtils.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Editor.h"
#include "EditorValidatorBase.h"
#include "EditorValidatorSubsystem.h"
#include "Logging/TokenizedMessage.h"
#include "Misc/DataValidation.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// DV_ prefix per the unity-build symbol-collision convention.
	constexpr int32 kDVErrorInvalidParams = -32602;
	constexpr int32 kDVErrorInternal      = -32603;

	// validate_path hard cap — keeps a single tool call bounded against accidental
	// /Game-recursive sweeps (which can easily produce 100k+ assets on a real project).
	constexpr int32 kDVMaxAssetsHardCap   = 10000;

	void DV_StampIds(const FMCPRequest& Request, FMCPResponse& Response)
	{
		Response.RequestId = Request.RequestId;
		Response.OriginalIdString = Request.OriginalIdString;
	}

	FMCPResponse DV_MakeError(const FMCPRequest& Request, int32 Code, const FString& Message)
	{
		FMCPResponse R;
		DV_StampIds(Request, R);
		R.bIsError = true;
		R.ErrorCode = Code;
		R.ErrorMessage = Message;
		return R;
	}

	FMCPResponse DV_MakeSuccessObj(const FMCPRequest& Request, TSharedPtr<FJsonObject> Result)
	{
		FMCPResponse R;
		DV_StampIds(Request, R);
		R.bIsError = false;
		R.Result = MakeShared<FJsonValueObject>(MoveTemp(Result));
		return R;
	}

	bool DV_RequireStringField(const FMCPRequest& Request, const TCHAR* FieldName,
		FString& OutValue, FMCPResponse& OutError)
	{
		if (!Request.Args.IsValid())
		{
			OutError = DV_MakeError(Request, kDVErrorInvalidParams, TEXT("missing args object"));
			return false;
		}
		if (!Request.Args->TryGetStringField(FieldName, OutValue) || OutValue.IsEmpty())
		{
			OutError = DV_MakeError(Request, kDVErrorInvalidParams,
				FString::Printf(TEXT("missing required string field '%s'"), FieldName));
			return false;
		}
		return true;
	}

	/**
	 * Resolve the bridge's UEditorValidatorSubsystem instance. The subsystem is loaded by the
	 * DataValidation plugin at editor startup; we still guard against GEditor==null (commandlet)
	 * and the subsystem being absent (plugin disabled in some project configurations).
	 */
	UEditorValidatorSubsystem* DV_GetValidatorSubsystem(FString& OutError)
	{
		if (!GEditor)
		{
			OutError = TEXT("GEditor is null (commandlet/cooker context)");
			return nullptr;
		}
		UEditorValidatorSubsystem* VS = GEditor->GetEditorSubsystem<UEditorValidatorSubsystem>();
		if (!VS)
		{
			OutError = TEXT("UEditorValidatorSubsystem not available "
				"(DataValidation plugin disabled?)");
			return nullptr;
		}
		return VS;
	}

	/** Map ``EDataValidationResult`` -> wire string. */
	const TCHAR* DV_ResultToString(EDataValidationResult Result)
	{
		switch (Result)
		{
		case EDataValidationResult::Valid:        return TEXT("valid");
		case EDataValidationResult::Invalid:      return TEXT("invalid");
		case EDataValidationResult::NotValidated: return TEXT("not_validated");
		default:                                  return TEXT("not_validated");
		}
	}

	/**
	 * Run validation on a single asset and pack the result into JSON. ``EmittedErrors`` /
	 * ``EmittedWarnings`` are populated with the FText messages from the context's FIssue list
	 * split on EMessageSeverity::Error / Warning. Tokenized messages have their plain-text
	 * payload extracted via ``FTokenizedMessage::ToText``.
	 */
	EDataValidationResult DV_ValidateOne(UEditorValidatorSubsystem* VS, UObject* Asset,
		TArray<TSharedPtr<FJsonValue>>& OutErrorArr,
		TArray<TSharedPtr<FJsonValue>>& OutWarningArr)
	{
		check(VS);
		check(Asset);

		// Build a Manual-usecase context. bWasAssetLoadedForValidation=false because the asset was
		// already loaded by LoadObject (validate-as-side-effect path the editor takes when the
		// asset is already cached).
		FDataValidationContext Context(
			/*InWasAssetLoadedForValidation=*/false,
			EDataValidationUsecase::Manual,
			/*InAssociatedObjects=*/{});

		const EDataValidationResult Result = VS->IsObjectValidWithContext(Asset, Context);

		// Split issues by severity. FIssue::Severity is EMessageSeverity::Type (int); plain-text
		// messages live in FIssue::Message, tokenized messages need ToText() extraction.
		for (const FDataValidationContext::FIssue& Issue : Context.GetIssues())
		{
			FText MessageText;
			if (Issue.TokenizedMessage.IsValid())
			{
				// Tokenized message — extract its rendered text. Token-specific UI hyperlinks are
				// dropped (plain-text round-trip).
				MessageText = Issue.TokenizedMessage->ToText();
			}
			else
			{
				MessageText = Issue.Message;
			}
			const FString MessageStr = MessageText.ToString();

			// EMessageSeverity::Type ordering: Error < PerformanceWarning < Warning < Info.
			// Treat Error as "errors"; PerformanceWarning + Warning as warnings; Info dropped
			// (not surface-relevant for the validator wire contract).
			//
			// UE 5.7 deprecated EMessageSeverity::CriticalError ("removed because it can't trigger
			// an assert at the callsite — use checkf instead"). Validators no longer emit it, so
			// the branch was unreachable in practice and removing it eliminates the deprecation
			// warning that would otherwise become a hard error in a future UE release.
			if (Issue.Severity == EMessageSeverity::Error)
			{
				OutErrorArr.Add(MakeShared<FJsonValueString>(MessageStr));
			}
			else if (Issue.Severity == EMessageSeverity::Warning ||
					 Issue.Severity == EMessageSeverity::PerformanceWarning)
			{
				OutWarningArr.Add(MakeShared<FJsonValueString>(MessageStr));
			}
			// Info severity intentionally dropped.
		}

		return Result;
	}

	/**
	 * Count how many enabled validators the subsystem has. Used as a conservative proxy for
	 * ``validators_run`` — the exact "how many validators actually fired on this asset" requires
	 * subsystem-internal state we can't access. ForEachEnabledValidator yields enabled validators
	 * that COULD validate, which is the next-best signal.
	 */
	int32 DV_CountEnabledValidators(UEditorValidatorSubsystem* VS)
	{
		check(VS);
		int32 Count = 0;
		VS->ForEachEnabledValidator([&Count](UEditorValidatorBase* Validator) -> bool
		{
			if (Validator) { ++Count; }
			return true; // continue iteration
		});
		return Count;
	}
} // namespace

namespace FDataValidationTools
{

// ─── data_validation.validate_asset ───────────────────────────────────────────────────────────
//
// Args:    { asset_path: string }
// Result:  { asset_path: string, result: "valid"|"invalid"|"not_validated",
//            errors: [string], warnings: [string], validators_run: int }
//
// Read-only — no PIE guard. Single-asset validation: loads the asset (cheap, cached after the
// first hit) then routes through UEditorValidatorSubsystem::IsObjectValidWithContext.
FMCPResponse Tool_ValidateAsset(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString AssetPath;
	FMCPResponse Err;
	if (!DV_RequireStringField(Request, TEXT("asset_path"), AssetPath, Err)) { return Err; }

	FString SubsysErr;
	UEditorValidatorSubsystem* VS = DV_GetValidatorSubsystem(SubsysErr);
	if (!VS) { return DV_MakeError(Request, kDVErrorInternal, SubsysErr); }

	// Normalise + validate the path before LoadObject (the same shape every other tool uses).
	const FString Normalised = FMCPAssetPathUtils::Normalize(AssetPath);
	if (Normalised.IsEmpty() || !FMCPAssetPathUtils::IsValidGameOrPlugin(Normalised))
	{
		return DV_MakeError(Request, kMCPErrorInvalidPath,
			FString::Printf(TEXT("path '%s' malformed or unknown mount"), *AssetPath));
	}

	UObject* Asset = LoadObject<UObject>(nullptr, *Normalised);
	if (!Asset)
	{
		// Try the explicit object-path form as a fallback (matches CurveTools / DataTableTools).
		const FString ObjPath = FMCPAssetPathUtils::ToObjectPath(Normalised);
		if (!ObjPath.IsEmpty() && ObjPath != Normalised)
		{
			Asset = LoadObject<UObject>(nullptr, *ObjPath);
		}
	}
	if (!Asset)
	{
		return DV_MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("'%s' not loadable"), *AssetPath));
	}

	TArray<TSharedPtr<FJsonValue>> Errors;
	TArray<TSharedPtr<FJsonValue>> Warnings;
	const EDataValidationResult Result = DV_ValidateOne(VS, Asset, Errors, Warnings);
	const int32 ValidatorsRun = DV_CountEnabledValidators(VS);

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("asset_path"), Asset->GetPathName());
	Out->SetStringField(TEXT("result"), DV_ResultToString(Result));
	Out->SetArrayField(TEXT("errors"), Errors);
	Out->SetArrayField(TEXT("warnings"), Warnings);
	Out->SetNumberField(TEXT("validators_run"), ValidatorsRun);
	return DV_MakeSuccessObj(Request, Out);
}

// ─── data_validation.validate_path ────────────────────────────────────────────────────────────
//
// Args:    { path_prefix: string, recursive?: bool (default true), max_assets?: int
//            (default 1000, hard cap 10000) }
// Result:  { path_prefix: string, total_validated: int, valid_count: int, invalid_count: int,
//            not_validated_count: int, validators_run: int,
//            failures: [{ asset_path, result, errors[], warnings[] }] }
//
// Read-only — no PIE guard. ``failures`` contains EVERY asset that resolved to Invalid or
// NotValidated (i.e. anything that's not Valid). Aggregate counts cover all visited assets
// including the Valid ones.
//
// Cost model: each asset triggers a LoadObject + IsObjectValidWithContext, both of which can
// touch the disk + run BP validators. With max_assets=10000 this is a multi-minute call on a
// large project — the hard cap is intentional. Callers should narrow ``path_prefix`` to a
// subfolder for routine validation.
FMCPResponse Tool_ValidatePath(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString PathPrefix;
	FMCPResponse Err;
	if (!DV_RequireStringField(Request, TEXT("path_prefix"), PathPrefix, Err)) { return Err; }

	bool bRecursive = true;
	if (Request.Args.IsValid()) { Request.Args->TryGetBoolField(TEXT("recursive"), bRecursive); }

	int32 MaxAssets = 1000;
	if (Request.Args.IsValid()) { Request.Args->TryGetNumberField(TEXT("max_assets"), MaxAssets); }
	if (MaxAssets < 1 || MaxAssets > kDVMaxAssetsHardCap)
	{
		return DV_MakeError(Request, kDVErrorInvalidParams,
			FString::Printf(TEXT("max_assets=%d out of range [1, %d]"),
				MaxAssets, kDVMaxAssetsHardCap));
	}

	FString SubsysErr;
	UEditorValidatorSubsystem* VS = DV_GetValidatorSubsystem(SubsysErr);
	if (!VS) { return DV_MakeError(Request, kDVErrorInternal, SubsysErr); }

	// Normalise + validate path_prefix as a content mount (so /Game/Foo is fine, /InvalidMount
	// is rejected up-front instead of silently producing zero results).
	const FString Normalised = FMCPAssetPathUtils::Normalize(PathPrefix);
	if (Normalised.IsEmpty() || !FMCPAssetPathUtils::IsValidGameOrPlugin(Normalised))
	{
		return DV_MakeError(Request, kMCPErrorInvalidPath,
			FString::Printf(TEXT("path_prefix '%s' malformed or unknown mount"), *PathPrefix));
	}

	IAssetRegistry& AR =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FARFilter Filter;
	Filter.PackagePaths.Add(*Normalised);
	Filter.bRecursivePaths = bRecursive;
	TArray<FAssetData> Assets;
	AR.GetAssets(Filter, Assets);

	if (Assets.Num() == 0)
	{
		return DV_MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("no assets under '%s' (recursive=%s)"),
				*PathPrefix, bRecursive ? TEXT("true") : TEXT("false")));
	}

	// Stable sort so the failures[] order is deterministic across calls (same asset graph -> same
	// order). Caller-visible determinism matters more than walk cost (AR.GetAssets is already O(N)
	// in the result set).
	Assets.Sort([](const FAssetData& A, const FAssetData& B)
	{
		return A.GetSoftObjectPath().ToString() < B.GetSoftObjectPath().ToString();
	});

	int32 ValidCount = 0, InvalidCount = 0, NotValidatedCount = 0;
	TArray<TSharedPtr<FJsonValue>> Failures;

	const int32 EffectiveMax = FMath::Min(Assets.Num(), MaxAssets);
	for (int32 i = 0; i < EffectiveMax; ++i)
	{
		const FAssetData& A = Assets[i];
		UObject* Asset = A.GetAsset();
		if (!Asset)
		{
			// AR entry exists but the asset failed to load — count as not_validated and surface
			// in failures[] so the caller can diagnose. No fail-fast: a single broken asset
			// shouldn't block the whole batch.
			++NotValidatedCount;
			TSharedRef<FJsonObject> FailObj = MakeShared<FJsonObject>();
			FailObj->SetStringField(TEXT("asset_path"), A.GetSoftObjectPath().ToString());
			FailObj->SetStringField(TEXT("result"), TEXT("not_validated"));
			TArray<TSharedPtr<FJsonValue>> LoadErrArr;
			LoadErrArr.Add(MakeShared<FJsonValueString>(TEXT("asset failed to load")));
			FailObj->SetArrayField(TEXT("errors"), LoadErrArr);
			FailObj->SetArrayField(TEXT("warnings"), TArray<TSharedPtr<FJsonValue>>());
			Failures.Add(MakeShared<FJsonValueObject>(FailObj));
			continue;
		}

		TArray<TSharedPtr<FJsonValue>> AssetErrors;
		TArray<TSharedPtr<FJsonValue>> AssetWarnings;
		const EDataValidationResult R = DV_ValidateOne(VS, Asset, AssetErrors, AssetWarnings);

		switch (R)
		{
		case EDataValidationResult::Valid:
			++ValidCount;
			break;
		case EDataValidationResult::Invalid:
			++InvalidCount;
			break;
		case EDataValidationResult::NotValidated:
		default:
			++NotValidatedCount;
			break;
		}

		// Surface every non-Valid asset in failures[]. NotValidated entries are typically benign
		// (no validator opined on them) but useful for diagnostic — callers can filter by result
		// field if they only care about Invalid.
		if (R != EDataValidationResult::Valid)
		{
			TSharedRef<FJsonObject> FailObj = MakeShared<FJsonObject>();
			FailObj->SetStringField(TEXT("asset_path"), Asset->GetPathName());
			FailObj->SetStringField(TEXT("result"), DV_ResultToString(R));
			FailObj->SetArrayField(TEXT("errors"), AssetErrors);
			FailObj->SetArrayField(TEXT("warnings"), AssetWarnings);
			Failures.Add(MakeShared<FJsonValueObject>(FailObj));
		}
	}

	const int32 ValidatorsRun = DV_CountEnabledValidators(VS);

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("path_prefix"), Normalised);
	Out->SetBoolField(TEXT("recursive"), bRecursive);
	Out->SetNumberField(TEXT("total_validated"), EffectiveMax);
	Out->SetNumberField(TEXT("total_known"), Assets.Num());
	Out->SetNumberField(TEXT("valid_count"), ValidCount);
	Out->SetNumberField(TEXT("invalid_count"), InvalidCount);
	Out->SetNumberField(TEXT("not_validated_count"), NotValidatedCount);
	Out->SetNumberField(TEXT("validators_run"), ValidatorsRun);
	Out->SetArrayField(TEXT("failures"), Failures);
	return DV_MakeSuccessObj(Request, Out);
}

// ─── data_validation.list_validators ──────────────────────────────────────────────────────────
//
// Args:    (none)
// Result:  { validators: [{ class_path, is_enabled, description }], total: int }
//
// Read-only — no PIE guard. Enumerates every concrete UEditorValidatorBase subclass via
// TObjectIterator + filters out abstract + deprecated classes. ``is_enabled`` comes from the
// CDO's IsEnabled() override (folds bIsEnabled + bIsConfigDisabled — see EditorValidatorBase.h).
//
// **Caveat.** This lists validator CLASSES, not registered INSTANCES held by the subsystem. A
// validator class may be defined but not actually loaded into the subsystem's Validators map (e.g.
// project-specific subclass that requires explicit AddValidator). For the typical inspection
// usecase (which validators COULD run on my assets) this is the relevant set.
FMCPResponse Tool_ListValidators(const FMCPRequest& Request)
{
	check(IsInGameThread());

	// Subsystem presence is a sanity check — the rest of the function uses TObjectIterator over
	// UClass which doesn't need the subsystem at all, but if the subsystem is missing we're not
	// in a state where listing validators makes sense.
	FString SubsysErr;
	UEditorValidatorSubsystem* VS = DV_GetValidatorSubsystem(SubsysErr);
	if (!VS) { return DV_MakeError(Request, kDVErrorInternal, SubsysErr); }

	TArray<TSharedPtr<FJsonValue>> ValidatorArr;

	// Walk every UClass that's a UEditorValidatorBase subclass. TObjectIterator skips GC-pending
	// + transient classes; we additionally filter out abstract + deprecated + the base class
	// itself (which has CLASS_Abstract set).
	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Class = *It;
		if (!Class) { continue; }
		if (!Class->IsChildOf(UEditorValidatorBase::StaticClass())) { continue; }
		if (Class == UEditorValidatorBase::StaticClass()) { continue; }
		if (Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
		{
			continue;
		}

		const FString ClassPath = Class->GetPathName();

		// Read enabled state from the CDO. GetDefaultObject<T>() returns the CDO cast to T or
		// nullptr if the CDO is missing (shouldn't happen for a loaded UClass).
		bool bEnabled = true;
		if (UEditorValidatorBase* CDO = Class->GetDefaultObject<UEditorValidatorBase>())
		{
			bEnabled = CDO->IsEnabled();
		}

		// Description = engine display name. Falls back to the class name. UE 5.7 uses
		// GetDisplayNameText() for class metadata; native classes typically return the class name.
		const FString Description = Class->GetDisplayNameText().ToString();

		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("class_path"), ClassPath);
		Obj->SetBoolField(TEXT("is_enabled"), bEnabled);
		Obj->SetStringField(TEXT("description"), Description);
		ValidatorArr.Add(MakeShared<FJsonValueObject>(Obj));
	}

	// Stable sort by class_path so the listing order is deterministic across calls.
	ValidatorArr.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
	{
		const TSharedPtr<FJsonObject>& AObj = A->AsObject();
		const TSharedPtr<FJsonObject>& BObj = B->AsObject();
		FString APath, BPath;
		if (AObj.IsValid()) { AObj->TryGetStringField(TEXT("class_path"), APath); }
		if (BObj.IsValid()) { BObj->TryGetStringField(TEXT("class_path"), BPath); }
		return APath < BPath;
	});

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetArrayField(TEXT("validators"), ValidatorArr);
	Out->SetNumberField(TEXT("total"), ValidatorArr.Num());
	return DV_MakeSuccessObj(Request, Out);
}

// ─── Registration ──────────────────────────────────────────────────────────────────────────────
void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	RegisterTool(TEXT("data_validation.validate_asset"),  &Tool_ValidateAsset,  /*Lane A*/ false);
	RegisterTool(TEXT("data_validation.validate_path"),   &Tool_ValidatePath,   /*Lane A*/ false);
	RegisterTool(TEXT("data_validation.list_validators"), &Tool_ListValidators, /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("DataValidation surface registered: 3 data_validation.* tools "
			 "(validate_asset + validate_path + list_validators), all Lane A, no PIE guard"));
}

} // namespace FDataValidationTools

#undef LOCTEXT_NAMESPACE
