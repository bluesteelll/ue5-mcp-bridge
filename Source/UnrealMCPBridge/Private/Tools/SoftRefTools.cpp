// Copyright FatumGame. All Rights Reserved.

#include "SoftRefTools.h"

#include "FMCPDispatchQueue.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPAssetPathUtils.h"
#include "Utils/MCPPageCursor.h"
#include "Utils/MCPWorldContext.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "ScopedTransaction.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectGlobals.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// SR_ prefix per the unity-build symbol-collision convention.
	constexpr int32 kSRErrorInvalidParams = -32602;
	constexpr int32 kSRErrorInternal      = -32603;

	void SR_StampIds(const FMCPRequest& Request, FMCPResponse& Response)
	{
		Response.RequestId = Request.RequestId;
		Response.OriginalIdString = Request.OriginalIdString;
	}

	FMCPResponse SR_MakeError(const FMCPRequest& Request, int32 Code, const FString& Message)
	{
		FMCPResponse R;
		SR_StampIds(Request, R);
		R.bIsError = true;
		R.ErrorCode = Code;
		R.ErrorMessage = Message;
		return R;
	}

	FMCPResponse SR_MakeSuccessObj(const FMCPRequest& Request, TSharedPtr<FJsonObject> Result)
	{
		FMCPResponse R;
		SR_StampIds(Request, R);
		R.bIsError = false;
		R.Result = MakeShared<FJsonValueObject>(MoveTemp(Result));
		return R;
	}

	bool SR_RequireStringField(const FMCPRequest& Request, const TCHAR* FieldName,
		FString& OutValue, FMCPResponse& OutError)
	{
		if (!Request.Args.IsValid())
		{
			OutError = SR_MakeError(Request, kSRErrorInvalidParams, TEXT("missing args object"));
			return false;
		}
		if (!Request.Args->TryGetStringField(FieldName, OutValue) || OutValue.IsEmpty())
		{
			OutError = SR_MakeError(Request, kSRErrorInvalidParams,
				FString::Printf(TEXT("missing required string field '%s'"), FieldName));
			return false;
		}
		return true;
	}

	/**
	 * Build an FSoftObjectPath from an arbitrary caller string. Returns the parsed path; the
	 * out-parameter bOutValidSyntax reports IsValid(). We never throw — IsValid==false is a
	 * value-level signal, not an exception.
	 *
	 * Note: FSoftObjectPath's string constructor accepts forms like ``/Game/Foo/Bar``,
	 * ``/Game/Foo/Bar.Bar``, and even legacy ``ClassName'/Game/Foo.Bar'`` notation.
	 */
	FSoftObjectPath SR_ParseSoftPath(const FString& InString, bool& bOutValidSyntax)
	{
		FSoftObjectPath Soft(InString);
		bOutValidSyntax = Soft.IsValid();
		return Soft;
	}

	/**
	 * Best-effort look up of the redirector's destination object PATH without bringing the
	 * destination into memory. Loads the redirector itself (FindObject + LoadObject fallback) and
	 * reads DestinationObject's pathname. Returns empty string + populates OutErrorReason on any
	 * failure (redirector unloadable, cast failed, destination null).
	 *
	 * Returned destination path is the resolved hard path — useful as the "this is where the
	 * reference SHOULD point" answer.
	 */
	FString SR_LoadRedirectorTarget(const FAssetData& RedirectorAssetData, FString& OutErrorReason)
	{
		const FString ObjectPath = RedirectorAssetData.GetObjectPathString();
		UObject* Loaded = FindObject<UObject>(nullptr, *ObjectPath);
		if (!Loaded)
		{
			Loaded = LoadObject<UObject>(nullptr, *ObjectPath);
		}
		if (!Loaded)
		{
			OutErrorReason = FString::Printf(TEXT("redirector '%s' not loadable"), *ObjectPath);
			return FString();
		}
		UObjectRedirector* Redir = Cast<UObjectRedirector>(Loaded);
		if (!Redir)
		{
			OutErrorReason = FString::Printf(TEXT("'%s' is class '%s'; expected UObjectRedirector"),
				*ObjectPath, *Loaded->GetClass()->GetPathName());
			return FString();
		}
		if (!Redir->DestinationObject)
		{
			OutErrorReason = FString::Printf(TEXT("redirector '%s' has null DestinationObject (broken redirector)"),
				*ObjectPath);
			return FString();
		}
		return Redir->DestinationObject->GetPathName();
	}
} // namespace

namespace FSoftRefTools
{

// ─── soft_ref.validate ───────────────────────────────────────────────────────────────────────
//
// Args:    { soft_path: string }
// Result:  { valid_syntax: bool, target_exists: bool, target_class?: string }
//
// Read-only — no PIE guard. ``valid_syntax`` is the FSoftObjectPath::IsValid result;
// ``target_exists`` is an IAssetRegistry::GetAssetByObjectPath single-point lookup. When the
// target exists we also report its class path (e.g. ``/Script/Engine.StaticMesh``) — useful for
// callers wanting to confirm a soft ref points at the expected asset family before reaching
// for the heavyweight load path.
FMCPResponse Tool_Validate(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString SoftPathStr;
	FMCPResponse Err;
	if (!SR_RequireStringField(Request, TEXT("soft_path"), SoftPathStr, Err)) { return Err; }

	bool bValidSyntax = false;
	const FSoftObjectPath Soft = SR_ParseSoftPath(SoftPathStr, bValidSyntax);

	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("valid_syntax"), bValidSyntax);

	if (!bValidSyntax)
	{
		Out->SetBoolField(TEXT("target_exists"), false);
		return SR_MakeSuccessObj(Request, Out);
	}

	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	const FAssetData Data = AR.GetAssetByObjectPath(Soft, /*bIncludeOnlyOnDiskAssets*/ false);
	const bool bExists = Data.IsValid();
	Out->SetBoolField(TEXT("target_exists"), bExists);

	if (bExists)
	{
		Out->SetStringField(TEXT("target_class"), Data.AssetClassPath.ToString());
	}

	return SR_MakeSuccessObj(Request, Out);
}

// ─── soft_ref.resolve ────────────────────────────────────────────────────────────────────────
//
// Args:    { soft_path: string, force_load?: bool (default false) }
// Result:  { resolved_path?: string, was_loaded: bool, target_class?: string }
//
// Read-only on the asset registry; ``force_load=true`` MAY bring the target into memory via
// FSoftObjectPath::TryLoad. No PIE guard — loading is not a state-mutation event. ``was_loaded``
// reports whether the resolved object is currently in memory (true if force_load succeeded OR
// the asset was already loaded; false if not loaded and force_load was false/null).
//
// ``resolved_path`` is the UObject's full path name when the asset can be resolved (already
// loaded, or successfully force_loaded). Without force_load we fall back to the AR's
// canonical object path so the caller still gets a useful answer for unloaded targets.
//
// Malformed soft_path → -32010. Resolvable syntax but no asset at that path AND no force_load
// hit → still success (resolved_path absent, was_loaded=false) — caller uses ``validate`` to
// distinguish "doesn't exist" from "exists but unloaded".
FMCPResponse Tool_Resolve(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString SoftPathStr;
	FMCPResponse Err;
	if (!SR_RequireStringField(Request, TEXT("soft_path"), SoftPathStr, Err)) { return Err; }

	bool bForceLoad = false;
	if (Request.Args.IsValid()) { Request.Args->TryGetBoolField(TEXT("force_load"), bForceLoad); }

	bool bValidSyntax = false;
	const FSoftObjectPath Soft = SR_ParseSoftPath(SoftPathStr, bValidSyntax);
	if (!bValidSyntax)
	{
		return SR_MakeError(Request, kMCPErrorInvalidPath,
			FString::Printf(TEXT("soft_path '%s' is not a valid FSoftObjectPath"), *SoftPathStr));
	}

	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();

	// Probe for an already-loaded object first (Soft.ResolveObject is non-loading).
	UObject* Resolved = Soft.ResolveObject();
	bool bWasLoaded = (Resolved != nullptr);

	if (!Resolved && bForceLoad)
	{
		Resolved = Soft.TryLoad();
		bWasLoaded = (Resolved != nullptr);
	}

	Out->SetBoolField(TEXT("was_loaded"), bWasLoaded);

	if (Resolved)
	{
		Out->SetStringField(TEXT("resolved_path"), Resolved->GetPathName());
		Out->SetStringField(TEXT("target_class"), Resolved->GetClass()->GetPathName());
		return SR_MakeSuccessObj(Request, Out);
	}

	// Not loaded (and either force_load=false or force_load attempt failed). Fall back to AR:
	// if the asset registry knows the path, report its canonical object path + class so caller
	// has a useful answer even without bringing the asset into memory.
	//
	// Wave I bug-fix: Data.GetObjectPathString() sometimes returns weird concatenated forms
	// (observed: `/Engine/EditorMaterials/EditorAxisMaterial./Engine/EditorMaterials/EditorAxisMaterial`
	// when input was the short-form path `/Engine/EditorMaterials/EditorAxisMaterial`). FSoftObjectPath::
	// ToString returns the canonical `Package.AssetName` form the caller passed (or the auto-
	// promoted variant FSoftObjectPath constructed) — that's what we want to echo.
	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	const FAssetData Data = AR.GetAssetByObjectPath(Soft, /*bIncludeOnlyOnDiskAssets*/ false);
	if (Data.IsValid())
	{
		Out->SetStringField(TEXT("resolved_path"), Soft.ToString());
		Out->SetStringField(TEXT("target_class"), Data.AssetClassPath.ToString());
	}
	// Otherwise: resolved_path absent, was_loaded=false. Still a success — caller uses validate
	// to disambiguate "no asset" from "asset present but unloaded".

	return SR_MakeSuccessObj(Request, Out);
}

// ─── soft_ref.find_redirectors ───────────────────────────────────────────────────────────────
//
// Args:    { path_prefix?: string, page_size?: int (default 100, clamp [1,1000]), page_token?: string }
// Result:  { redirectors: [{ redirector_path, target_path }], total_known, next_page_token? }
//
// Read-only — no PIE guard. ``path_prefix`` is optional; when absent, scans every mount
// (matches Phase 2 cb.fix_redirectors semantics on the filter side). Per-entry includes the
// resolved ``target_path`` read from UObjectRedirector::DestinationObject — that read requires
// loading each redirector (no AR tag for target). Broken redirectors (null DestinationObject)
// are still included in the response with ``target_path`` set to an empty string + the redirector
// path's diagnostic appended to the message field — but we keep the response shape uniform so
// callers can iterate the array uniformly.
//
// Standard FMCPPageCursor over ObjectPath, filter hash includes path_prefix.
FMCPResponse Tool_FindRedirectors(const FMCPRequest& Request)
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
	Filter.ClassPaths.Add(UObjectRedirector::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = false;
	Filter.bRecursivePaths   = true;
	if (!PathPrefix.IsEmpty())
	{
		Filter.PackagePaths.Add(FName(*PathPrefix));
	}
	TArray<FAssetData> Redirectors;
	AR.GetAssets(Filter, Redirectors);

	// Stable sort by ObjectPath (keyset pagination sort key).
	Redirectors.Sort([](const FAssetData& A, const FAssetData& B)
	{
		return A.GetSoftObjectPath().ToString() < B.GetSoftObjectPath().ToString();
	});

	int32 StartIdx = 0;
	FMCPPageCursor InCursor;
	if (!PageToken.IsEmpty())
	{
		FString DecodeErr;
		if (!FMCPPageCursorUtils::Decode(PageToken, InCursor, DecodeErr))
		{
			return SR_MakeError(Request, kSRErrorInvalidParams,
				FString::Printf(TEXT("invalid page_token: %s"), *DecodeErr));
		}
		if (!FMCPPageCursorUtils::ValidateAgainstFilter(InCursor, FilterHash))
		{
			return SR_MakeError(Request, kMCPErrorStaleCursor,
				TEXT("filter mutated between pages (path_prefix changed); restart pagination"));
		}
		while (StartIdx < Redirectors.Num() &&
			   Redirectors[StartIdx].GetSoftObjectPath().ToString() <= InCursor.LastAssetPath)
		{
			++StartIdx;
		}
	}

	TArray<TSharedPtr<FJsonValue>> RedirectorArr;
	const int32 EndIdx = FMath::Min(StartIdx + PageSize, Redirectors.Num());
	RedirectorArr.Reserve(EndIdx - StartIdx);
	for (int32 i = StartIdx; i < EndIdx; ++i)
	{
		const FAssetData& RD = Redirectors[i];
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("redirector_path"), RD.GetSoftObjectPath().ToString());

		// Load the redirector to read its target. Failures (broken redirector, load failure)
		// yield empty target_path — caller can detect via the empty string. We don't promote
		// per-redirector load failure to a top-level error because that would derail an
		// otherwise-useful enumeration of healthy redirectors.
		FString TargetLoadErr;
		const FString TargetPath = SR_LoadRedirectorTarget(RD, TargetLoadErr);
		Obj->SetStringField(TEXT("target_path"), TargetPath);
		if (!TargetLoadErr.IsEmpty())
		{
			UE_LOG(LogMCP, Verbose,
				TEXT("soft_ref.find_redirectors: target load for '%s' failed: %s"),
				*RD.GetObjectPathString(), *TargetLoadErr);
		}

		RedirectorArr.Add(MakeShared<FJsonValueObject>(Obj));
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetArrayField(TEXT("redirectors"), RedirectorArr);
	Out->SetNumberField(TEXT("total_known"), Redirectors.Num());

	if (EndIdx < Redirectors.Num() && EndIdx > 0)
	{
		FMCPPageCursor OutCursor;
		OutCursor.FilterHash = FilterHash;
		OutCursor.LastAssetPath = Redirectors[EndIdx - 1].GetSoftObjectPath().ToString();
		OutCursor.TotalKnownSnapshot = Redirectors.Num();
		Out->SetStringField(TEXT("next_page_token"), FMCPPageCursorUtils::Encode(OutCursor));
	}

	return SR_MakeSuccessObj(Request, Out);
}

// ─── soft_ref.fix_redirectors ────────────────────────────────────────────────────────────────
//
// Args:    { redirector_paths?: [string], path_prefix?: string, dry_run?: bool (default false) }
// Result:  { fixed: int, skipped: int, errors: [{ path, reason }] }
//
// PIE-guarded mutator (unless ``dry_run=true``). Caller MUST supply EITHER ``redirector_paths``
// OR ``path_prefix`` — both empty → -32602. When both are supplied the explicit list wins and
// ``path_prefix`` is ignored (no implicit union — too easy to surprise the caller).
//
// ``dry_run=true`` collects the redirector set, validates each can be loaded + cast, and
// reports what WOULD be fixed without touching FixupReferencers. PIE guard is bypassed in
// dry-run mode (no state mutation occurs).
//
// On the real path: FAssetToolsModule::Get().FixupReferencers walks every package that REFERS
// to each loaded redirector, mutates each reference to point at the redirector's
// DestinationObject, then DELETES the redirector itself
// (ERedirectFixupMode::DeleteFixedUpRedirectors). ``fixed`` counts redirectors successfully
// dispatched into FixupReferencers; ``skipped`` counts inputs we couldn't even load/cast (and
// thus didn't submit to FixupReferencers). Per-skip diagnostics in the ``errors`` array.
FMCPResponse Tool_FixRedirectors(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!Request.Args.IsValid())
	{
		return SR_MakeError(Request, kSRErrorInvalidParams, TEXT("missing args object"));
	}

	bool bDryRun = false;
	Request.Args->TryGetBoolField(TEXT("dry_run"), bDryRun);

	// PIE guard only when actually mutating.
	if (!bDryRun && FMCPWorldContext::IsPIEActive())
	{
		return SR_MakeError(Request, kMCPErrorPIEActive, kMCPMessagePIEActive);
	}

	// Collect explicit redirector_paths (optional).
	TArray<FString> ExplicitPaths;
	const TArray<TSharedPtr<FJsonValue>>* RedirArr = nullptr;
	if (Request.Args->TryGetArrayField(TEXT("redirector_paths"), RedirArr) && RedirArr)
	{
		ExplicitPaths.Reserve(RedirArr->Num());
		for (const TSharedPtr<FJsonValue>& V : *RedirArr)
		{
			FString S;
			if (V.IsValid() && V->TryGetString(S) && !S.IsEmpty())
			{
				ExplicitPaths.Add(MoveTemp(S));
			}
		}
	}

	FString PathPrefix;
	Request.Args->TryGetStringField(TEXT("path_prefix"), PathPrefix);

	if (ExplicitPaths.Num() == 0 && PathPrefix.IsEmpty())
	{
		return SR_MakeError(Request, kSRErrorInvalidParams,
			TEXT("soft_ref.fix_redirectors requires EITHER args.redirector_paths (non-empty array) "
				 "OR args.path_prefix (non-empty string)"));
	}

	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	// Build the candidate FAssetData set. Explicit list takes precedence; absent that we sweep
	// the AR under path_prefix.
	TArray<FAssetData> CandidateData;
	if (ExplicitPaths.Num() > 0)
	{
		CandidateData.Reserve(ExplicitPaths.Num());
		for (const FString& Path : ExplicitPaths)
		{
			const FSoftObjectPath Soft(Path);
			if (!Soft.IsValid())
			{
				// Will be reported as a skipped entry in the load loop below — push a sentinel.
				FAssetData Sentinel;
				CandidateData.Add(Sentinel);
				continue;
			}
			const FAssetData Data = AR.GetAssetByObjectPath(Soft, /*bIncludeOnlyOnDiskAssets*/ false);
			CandidateData.Add(Data);  // may be invalid — handled below
		}
	}
	else
	{
		FARFilter Filter;
		Filter.ClassPaths.Add(UObjectRedirector::StaticClass()->GetClassPathName());
		Filter.bRecursiveClasses = false;
		Filter.bRecursivePaths   = true;
		Filter.PackagePaths.Add(FName(*PathPrefix));
		AR.GetAssets(Filter, CandidateData);
	}

	// Load + cast each candidate. Collect successful UObjectRedirector* into Loaded; per-failure
	// diagnostics into errors.
	TArray<UObjectRedirector*> Loaded;
	Loaded.Reserve(CandidateData.Num());

	TArray<TSharedPtr<FJsonValue>> ErrorArr;
	int32 Skipped = 0;

	const auto RecordSkip = [&ErrorArr, &Skipped](const FString& Path, const FString& Reason)
	{
		TSharedRef<FJsonObject> ErrObj = MakeShared<FJsonObject>();
		ErrObj->SetStringField(TEXT("path"), Path);
		ErrObj->SetStringField(TEXT("reason"), Reason);
		ErrorArr.Add(MakeShared<FJsonValueObject>(ErrObj));
		++Skipped;
	};

	for (int32 i = 0; i < CandidateData.Num(); ++i)
	{
		const FAssetData& AD = CandidateData[i];
		const FString DisplayPath = AD.IsValid()
			? AD.GetObjectPathString()
			: (ExplicitPaths.IsValidIndex(i) ? ExplicitPaths[i] : TEXT("<unknown>"));

		if (!AD.IsValid())
		{
			RecordSkip(DisplayPath, TEXT("not found in AssetRegistry (kMCPErrorObjectNotFound)"));
			continue;
		}

		const FString ObjectPath = AD.GetObjectPathString();
		UObject* Obj = FindObject<UObject>(nullptr, *ObjectPath);
		if (!Obj) { Obj = LoadObject<UObject>(nullptr, *ObjectPath); }
		if (!Obj)
		{
			RecordSkip(ObjectPath, TEXT("LoadObject returned null"));
			continue;
		}
		UObjectRedirector* Redir = Cast<UObjectRedirector>(Obj);
		if (!Redir)
		{
			RecordSkip(ObjectPath,
				FString::Printf(TEXT("class is '%s'; expected UObjectRedirector"),
					*Obj->GetClass()->GetPathName()));
			continue;
		}
		Loaded.Add(Redir);
	}

	int32 Fixed = 0;

	if (!bDryRun && Loaded.Num() > 0)
	{
		FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
		FScopedTransaction Transaction(LOCTEXT("MCP_SoftRef_FixRedirectors", "Fix Redirectors"));
		// FixupReferencers internally walks each redirector's referencers, patches each reference
		// to point at DestinationObject, then deletes the redirector (via the
		// ERedirectFixupMode::DeleteFixedUpRedirectors flag). It does NOT return a count — we
		// report the input count as ``fixed`` since FixupReferencers either succeeds in dispatching
		// the whole set or asserts. Per-redirector failures during the walk are silent at this
		// layer (engine logs them internally).
		AssetToolsModule.Get().FixupReferencers(
			Loaded,
			/*bCheckoutDialogPrompt*/ false,
			ERedirectFixupMode::DeleteFixedUpRedirectors);
		Fixed = Loaded.Num();
	}
	else if (bDryRun)
	{
		// Dry run: report what WOULD be dispatched.
		Fixed = Loaded.Num();
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetNumberField(TEXT("fixed"), Fixed);
	Out->SetNumberField(TEXT("skipped"), Skipped);
	Out->SetArrayField(TEXT("errors"), ErrorArr);
	if (bDryRun) { Out->SetBoolField(TEXT("dry_run"), true); }
	return SR_MakeSuccessObj(Request, Out);
}

// ─── Registration ─────────────────────────────────────────────────────────────────────────────
void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	RegisterTool(TEXT("soft_ref.validate"),         &Tool_Validate,         /*Lane A*/ false);
	RegisterTool(TEXT("soft_ref.resolve"),          &Tool_Resolve,          /*Lane A*/ false);
	RegisterTool(TEXT("soft_ref.find_redirectors"), &Tool_FindRedirectors,  /*Lane A*/ false);
	RegisterTool(TEXT("soft_ref.fix_redirectors"),  &Tool_FixRedirectors,   /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("SoftRef surface registered: 4 soft_ref.* tools "
			 "(validate + resolve + find_redirectors + fix_redirectors), all Lane A"));
}

} // namespace FSoftRefTools

#include "MCPSurfaceRegistry.h"
MCP_REGISTER_SURFACE(SoftRefTools, &FSoftRefTools::Register)

#undef LOCTEXT_NAMESPACE
