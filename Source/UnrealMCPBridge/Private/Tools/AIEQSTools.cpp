// Copyright FatumGame. All Rights Reserved.

#include "AIEQSTools.h"

#include "FMCPDispatchQueue.h"
#include "MCPAssetLoader.h"
#include "MCPJsonBuilder.h"
#include "MCPMutatorScope.h"
#include "MCPToolHelpers.h"
#include "MCPClassResolver.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPActorPathUtils.h"
#include "Utils/MCPAssetPathUtils.h"
#include "Utils/MCPPageCursor.h"
#include "Utils/MCPReflection.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryGenerator.h"
#include "EnvironmentQuery/EnvQueryManager.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "EnvironmentQuery/EnvQueryTest.h"
#include "EnvironmentQuery/EnvQueryTypes.h"
#include "GameFramework/Actor.h"
#include "Engine/World.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// AIEQS_ prefix per the unity-build symbol-collision convention. The plugin uses unity builds so
	// anonymous-namespace helpers MUST be uniquely prefixed across every Tools/*.cpp.
	//
	// Phase 2 (Pair E, Wave J AI sub-2): generic StampIds / MakeError / MakeSuccessObj /
	// RequireStringField + AIEQS_LoadQueryByPath retired in favour of FMCPToolHelpers::Xxx +
	// FMCPAssetLoader::Load<UEnvQuery>. Only the surface-specific error constants stay (Phase 4
	// sweep target).
	constexpr int32 kAIEQSErrorInternal        = -32603;
	// kAIEQSErrorOperationFailed retired in Phase 4 — was -32015 (DOUBLE-MEANING bug with
	// canonical kMCPErrorStaleCursor pagination semantics). Migrated to canonical
	// kMCPErrorOperationFailed (-32058) per MCPTypes.h Phase 4 disambiguation.

	// ─── Wave P authoring helpers ──────────────────────────────────────────────────────────────────

	/**
	 * Resolve a class path string to a UClass*, verifying it descends from BaseClass.
	 * Tries the path verbatim first, then with a "_C" suffix (Blueprint generated class convention).
	 * Returns nullptr + populates OutError on any failure mode.
	 */
	// AIEQS_ResolveSubclassOf removed; replaced by FMCPClassResolver::Resolve (Wave Q2).

	// AIEQS_ApplyProperties removed; replaced by FMCPToolHelpers::ApplyJsonProperties (Wave Q1).
} // namespace

namespace FAIEQSTools
{

// --- ai.eqs.list_queries ---------------------------------------------------------------------
//
// Args:    { path_prefix?: string, page_size?: int (default 100, clamp [1, 1000]), page_token?: string }
// Result:  { queries: [{ asset_path, options_count }], total_known, next_page_token? }
//
// Read-only — no PIE guard. Asset registry enumeration of every UEnvQuery asset, paginated by
// ObjectPath via the standard FMCPPageCursor flow (matches data_table.list / curve.list).
//
// options_count requires loading the asset (no asset-registry tag carries it). Load is idempotent
// — the cost is paid only once per query asset and only for those in the current page slice. For
// projects with hundreds of unloaded EQS queries this still keeps the first-page latency bounded
// (page_size default 100, load cost is small for typical 1-3 option queries).
FMCPResponse Tool_ListQueries(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString PathPrefix;
	if (Request.Args.IsValid()) { Request.Args->TryGetStringField(TEXT("path_prefix"), PathPrefix); }

	int32 PageSize = 100;
	if (Request.Args.IsValid()) { Request.Args->TryGetNumberField(TEXT("page_size"), PageSize); }
	PageSize = FMath::Clamp(PageSize, 1, 1000);

	FString PageToken;
	if (Request.Args.IsValid()) { Request.Args->TryGetStringField(TEXT("page_token"), PageToken); }

	// FilterHash so cursor staleness is detectable across pages.
	const uint32 FilterHash = GetTypeHash(PathPrefix);

	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FARFilter Filter;
	Filter.ClassPaths.Add(UEnvQuery::StaticClass()->GetClassPathName());
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

	TArray<TSharedPtr<FJsonValue>> QueryArr;
	const int32 EndIdx = FMath::Min(StartIdx + PageSize, Assets.Num());
	QueryArr.Reserve(EndIdx - StartIdx);
	for (int32 i = StartIdx; i < EndIdx; ++i)
	{
		const FAssetData& A = Assets[i];
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("asset_path"), A.GetSoftObjectPath().ToString());

		// options_count needs the asset loaded — no AR tag for it. GetAsset is idempotent and
		// returns the already-loaded UObject on subsequent calls (no double-load cost).
		int32 OptionsCount = 0;
		if (UEnvQuery* Query = Cast<UEnvQuery>(A.GetAsset()))
		{
			OptionsCount = Query->GetOptions().Num();
		}
		Obj->SetNumberField(TEXT("options_count"), OptionsCount);
		QueryArr.Add(MakeShared<FJsonValueObject>(Obj));
	}

	const int32 TotalKnown = Assets.Num();
	const bool bHasNextPage = (EndIdx < TotalKnown && EndIdx > 0);
	return FMCPJsonBuilder()
		.Arr(TEXT("queries"), MoveTemp(QueryArr))
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

// --- ai.eqs.get_query_info -------------------------------------------------------------------
//
// Args:    { query_path: string }
// Result:  { query_path, options_count, options: [{ option_name, generator_class,
//            tests: [{ test_class, weight }] }] }
//
// Read-only — no PIE guard. Loads the asset on demand. Each option's generator + per-test class
// names are read via GetClass()->GetName() (short C++ name, e.g. "EnvQueryGenerator_OnCircle"
// without "U" prefix or "_C" suffix — matches editor display names for AI agent legibility).
//
// Test weight is read from ``ScoringFactor.DefaultValue`` — the FAIDataProviderFloatValue's
// editor-set constant. Tests with data-provider-driven scoring (e.g. blackboard-bound) will
// report the DefaultValue fallback rather than the runtime-bound value; the runtime value isn't
// known until the test is bound to a query instance, and this is a STATIC inspection tool.
//
// option_name uses the option's GetDescriptionTitle() text — that's what the EQS editor displays
// in the option list. For unnamed/legacy options it falls back to the generator class name.
FMCPResponse Tool_GetQueryInfo(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString QueryPath;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("query_path"), QueryPath, Err)) { return Err; }

	int32 LoadErrCode = 0;
	FString LoadErrMsg;
	UEnvQuery* Query = FMCPAssetLoader::Load<UEnvQuery>(QueryPath, LoadErrCode, LoadErrMsg);
	if (!Query) { return FMCPToolHelpers::MakeError(Request, LoadErrCode, LoadErrMsg); }

	const TArray<UEnvQueryOption*>& Options = Query->GetOptions();
	TArray<TSharedPtr<FJsonValue>> OptionArr;
	OptionArr.Reserve(Options.Num());

	for (int32 OptIdx = 0; OptIdx < Options.Num(); ++OptIdx)
	{
		const UEnvQueryOption* Option = Options[OptIdx];
		TSharedRef<FJsonObject> OptObj = MakeShared<FJsonObject>();

		if (!Option)
		{
			// Stale/null option — surface a placeholder rather than dropping silently so the caller
			// can detect a misconfigured asset.
			OptObj->SetStringField(TEXT("option_name"), FString::Printf(TEXT("(null option %d)"), OptIdx));
			OptObj->SetStringField(TEXT("generator_class"), TEXT("(null)"));
			OptObj->SetArrayField(TEXT("tests"), TArray<TSharedPtr<FJsonValue>>());
			OptionArr.Add(MakeShared<FJsonValueObject>(OptObj));
			continue;
		}

		const UEnvQueryGenerator* Generator = Option->Generator;
		const FString GenClass = Generator
			? Generator->GetClass()->GetName()
			: FString(TEXT("(null)"));

		// GetDescriptionTitle is the EQS editor's display label for the option. Falls back to the
		// generator class name when empty (legacy options without explicit description).
		FString OptName = Option->GetDescriptionTitle().ToString();
		if (OptName.IsEmpty())
		{
			OptName = GenClass;
		}

		OptObj->SetStringField(TEXT("option_name"), OptName);
		OptObj->SetStringField(TEXT("generator_class"), GenClass);

		TArray<TSharedPtr<FJsonValue>> TestArr;
		TestArr.Reserve(Option->Tests.Num());
		for (const UEnvQueryTest* Test : Option->Tests)
		{
			TSharedRef<FJsonObject> TestObj = MakeShared<FJsonObject>();
			if (Test)
			{
				TestObj->SetStringField(TEXT("test_class"), Test->GetClass()->GetName());
				// ScoringFactor.DefaultValue is the editor-set constant. GetValue() requires the
				// test to be bound to a query instance (provider binding) — we're a static
				// inspection tool, so the DefaultValue fallback is the correct read here.
				TestObj->SetNumberField(TEXT("weight"), Test->ScoringFactor.DefaultValue);
			}
			else
			{
				TestObj->SetStringField(TEXT("test_class"), TEXT("(null)"));
				TestObj->SetNumberField(TEXT("weight"), 0.0);
			}
			TestArr.Add(MakeShared<FJsonValueObject>(TestObj));
		}
		OptObj->SetArrayField(TEXT("tests"), TestArr);

		OptionArr.Add(MakeShared<FJsonValueObject>(OptObj));
	}

	const int32 OptionsCount = Options.Num();
	return FMCPJsonBuilder()
		.Str(TEXT("query_path"), QueryPath)
		.Num(TEXT("options_count"), OptionsCount)
		.Arr(TEXT("options"), MoveTemp(OptionArr))
		.BuildSuccess(Request);
}

// --- ai.eqs.run_query ------------------------------------------------------------------------
//
// Args:    { query_path: string, querier_actor_path: string,
//            mode?: "single_best" | "all_matching" (default "single_best") }
// Result:  { status: "Success"|"Failed"|"Aborted"|"OwnerLost"|"MissingParam",
//            mode, items_count, results: [{ location: [x,y,z], score, actor_path? }] }
//
// Runtime tool — PIE-safe. ``UEnvQueryManager::RunInstantQuery`` synchronously runs the query on
// the calling thread bypassing the time-sliced async pipeline. Epic flags this method as
// debug/testing-only — we ARE the debug tool, so the use is intentional. Production gameplay
// code should use the async ``RunQuery`` path instead.
//
// Querier resolution accepts editor-world OR PIE-world actors (bRejectPIE=false) — the querier's
// UWorld feeds GetCurrent(World), so PIE queries run against the PIE EQS manager and editor
// queries against the editor manager — no cross-world contamination.
//
// Items returning null actor (location-only item types — Points / Directions) emit only the
// location field. Items returning a non-null actor emit both location AND actor_path.
FMCPResponse Tool_RunQuery(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString QueryPath, QuerierPath;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("query_path"),         QueryPath,   Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("querier_actor_path"), QuerierPath, Err)) { return Err; }

	FString ModeStr = TEXT("single_best");
	if (Request.Args.IsValid())
	{
		Request.Args->TryGetStringField(TEXT("mode"), ModeStr);
	}
	EEnvQueryRunMode::Type Mode;
	if (ModeStr == TEXT("single_best"))
	{
		Mode = EEnvQueryRunMode::SingleResult;
	}
	else if (ModeStr == TEXT("all_matching"))
	{
		Mode = EEnvQueryRunMode::AllMatching;
	}
	else
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			FString::Printf(TEXT("unknown mode '%s'; expected 'single_best' or 'all_matching'"), *ModeStr));
	}

	// Load query asset.
	int32 QueryLoadErrCode = 0;
	FString QueryLoadErrMsg;
	UEnvQuery* Query = FMCPAssetLoader::Load<UEnvQuery>(QueryPath, QueryLoadErrCode, QueryLoadErrMsg);
	if (!Query) { return FMCPToolHelpers::MakeError(Request, QueryLoadErrCode, QueryLoadErrMsg); }

	// Resolve querier actor — bRejectPIE=false so PIE actors are addressable.
	bool bAmbiguous = false;
	FString AmbiguityHint;
	FString ResolveErr;
	AActor* Querier = FMCPActorPathUtils::ResolveActor(
		QuerierPath, /*bRejectPIE*/ false, bAmbiguous, AmbiguityHint, ResolveErr);
	if (!Querier)
	{
		FString Msg = FString::Printf(TEXT("querier_actor_path '%s' not resolvable: %s"),
			*QuerierPath, *ResolveErr);
		if (bAmbiguous && !AmbiguityHint.IsEmpty())
		{
			Msg.Append(FString::Printf(TEXT(" (ambiguous candidates: %s)"), *AmbiguityHint));
		}
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound, Msg);
	}

	// Get the EQS manager for the querier's world. Editor-world querier → editor manager;
	// PIE-world querier → PIE manager. Either may be null if the world has no AISystem (e.g.
	// commandlet / certain custom configurations) — surface as InternalError.
	UWorld* QuerierWorld = Querier->GetWorld();
	if (!QuerierWorld)
	{
		return FMCPToolHelpers::MakeError(Request, kAIEQSErrorInternal,
			FString::Printf(TEXT("querier '%s' has no UWorld"), *QuerierPath));
	}
	UEnvQueryManager* EQM = UEnvQueryManager::GetCurrent(QuerierWorld);
	if (!EQM)
	{
		return FMCPToolHelpers::MakeError(Request, kAIEQSErrorInternal,
			FString::Printf(TEXT("UEnvQueryManager not available for querier's world '%s'"),
				*QuerierWorld->GetName()));
	}

	// Build the request + execute synchronously.
	FEnvQueryRequest EQSRequest(Query, Querier);
	TSharedPtr<FEnvQueryResult> Result = EQM->RunInstantQuery(EQSRequest, Mode);

	if (!Result.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kAIEQSErrorInternal,
			TEXT("RunInstantQuery returned null result pointer"));
	}

	const EEnvQueryStatus::Type RawStatus = Result->GetRawStatus();
	const TCHAR* StatusStr = TEXT("Unknown");
	switch (RawStatus)
	{
		case EEnvQueryStatus::Processing:   StatusStr = TEXT("Processing");   break;
		case EEnvQueryStatus::Success:      StatusStr = TEXT("Success");      break;
		case EEnvQueryStatus::Failed:       StatusStr = TEXT("Failed");       break;
		case EEnvQueryStatus::Aborted:      StatusStr = TEXT("Aborted");      break;
		case EEnvQueryStatus::OwnerLost:    StatusStr = TEXT("OwnerLost");    break;
		case EEnvQueryStatus::MissingParam: StatusStr = TEXT("MissingParam"); break;
		default: break;
	}

	// Aborted / Failed / OwnerLost / MissingParam are query-level failures — surface as
	// kMCPErrorOperationFailed with the status string in the message so the caller can
	// disambiguate. Processing should never appear from an instant query (it's a synchronous
	// terminal call) but treat it as Failed-equivalent for defensiveness.
	if (!Result->IsSuccessful())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorOperationFailed,
			FString::Printf(TEXT("EQS query '%s' returned status '%s'"), *QueryPath, StatusStr));
	}

	// Successful query — extract locations + actors. GetAllAsLocations + GetAllAsActors keep null
	// entries (per Epic's "note that this function does not strip out the null-actors") so we can
	// match index-to-index between the two arrays.
	TArray<FVector> Locations;
	Result->GetAllAsLocations(Locations);
	TArray<AActor*> Actors;
	Result->GetAllAsActors(Actors);

	// Sanity: both arrays must have the same length (one entry per Items[]). If they diverge it's
	// an engine-level invariant break — surface as InternalError rather than producing junk results.
	if (Locations.Num() != Result->Items.Num())
	{
		return FMCPToolHelpers::MakeError(Request, kAIEQSErrorInternal,
			FString::Printf(TEXT("EQS result invariant break: %d items, %d locations"),
				Result->Items.Num(), Locations.Num()));
	}

	TArray<TSharedPtr<FJsonValue>> ResultArr;
	ResultArr.Reserve(Locations.Num());
	for (int32 i = 0; i < Locations.Num(); ++i)
	{
		TSharedRef<FJsonObject> ItemObj = MakeShared<FJsonObject>();
		const FVector& Loc = Locations[i];

		TArray<TSharedPtr<FJsonValue>> LocArr;
		LocArr.Reserve(3);
		LocArr.Add(MakeShared<FJsonValueNumber>(Loc.X));
		LocArr.Add(MakeShared<FJsonValueNumber>(Loc.Y));
		LocArr.Add(MakeShared<FJsonValueNumber>(Loc.Z));
		ItemObj->SetArrayField(TEXT("location"), LocArr);

		ItemObj->SetNumberField(TEXT("score"), Result->GetItemScore(i));

		// Actor item types emit a non-null actor here; location-only item types (Points/Directions)
		// return null. We only emit actor_path when it's actually populated to keep the wire
		// shape minimal for non-actor queries.
		if (Actors.IsValidIndex(i) && Actors[i])
		{
			const FString ActorPath = FMCPActorPathUtils::BuildActorPath(Actors[i]);
			if (!ActorPath.IsEmpty())
			{
				ItemObj->SetStringField(TEXT("actor_path"), ActorPath);
			}
		}

		ResultArr.Add(MakeShared<FJsonValueObject>(ItemObj));
	}

	const int32 ItemsCount = Locations.Num();
	return FMCPJsonBuilder()
		.Str(TEXT("status"), StatusStr)
		.Str(TEXT("mode"), ModeStr)
		.Num(TEXT("items_count"), ItemsCount)
		.Arr(TEXT("results"), MoveTemp(ResultArr))
		.Str(TEXT("query_path"), QueryPath)
		.Str(TEXT("querier_actor_path"), QuerierPath)
		.BuildSuccess(Request);
}

// --- ai.eqs.create_asset (Wave P) ------------------------------------------------------------
//
// Args:    { path: string }
// Result:  { asset_path }
//
// Creates an empty UEnvQuery asset (Options[] empty) at the supplied path. Caller follows up with
// ai.eqs.add_generator / ai.eqs.add_test to populate. Saves via cb.save / asset.save_loaded
// (not auto-saved here).
//
// Errors:
//   -32010 InvalidPath      malformed path
//   -32014 PathInUse        path already exists
//   -32027 PIEActive
//   -32603 InternalError    CreatePackage / NewObject returned null
FMCPResponse Tool_CreateAsset(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_EQSCreateAsset", "Create EQS Asset"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString DestPathRaw;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("path"), DestPathRaw, Err)) { return Err; }

	const FString DestPathNorm = FMCPAssetPathUtils::Normalize(DestPathRaw);
	if (DestPathNorm.IsEmpty() || !FMCPAssetPathUtils::IsValidGameOrPlugin(DestPathNorm))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidPath,
			FString::Printf(TEXT("path '%s' malformed or unknown mount"), *DestPathRaw));
	}

	const FString PackagePath = FPaths::GetPath(DestPathNorm);
	const FString AssetName   = FPaths::GetBaseFilename(DestPathNorm);

	if (FPackageName::DoesPackageExist(DestPathNorm) ||
		FindObject<UObject>(nullptr, *(DestPathNorm + TEXT(".") + AssetName)) != nullptr)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPathInUse,
			FString::Printf(TEXT("path '%s' already exists"), *DestPathNorm));
	}

	UPackage* EQSPkg = CreatePackage(*DestPathNorm);
	if (!EQSPkg)
	{
		return FMCPToolHelpers::MakeError(Request, kAIEQSErrorInternal,
			FString::Printf(TEXT("CreatePackage returned null for '%s'"), *DestPathNorm));
	}
	EQSPkg->FullyLoad();

	UEnvQuery* Query = NewObject<UEnvQuery>(
		EQSPkg, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
	if (!Query)
	{
		return FMCPToolHelpers::MakeError(Request, kAIEQSErrorInternal,
			FString::Printf(TEXT("NewObject<UEnvQuery> returned null for '%s'"), *DestPathNorm));
	}

	FAssetRegistryModule::AssetCreated(Query);
	Scope.DirtyPackage(EQSPkg);

	return FMCPJsonBuilder()
		.Str(TEXT("asset_path"), Query->GetPathName())
		.BuildSuccess(Request);
}

// --- ai.eqs.add_generator (Wave P) -----------------------------------------------------------
//
// Args:    { eqs_path: string, generator_class: string, properties?: {...} }
// Result:  { added, option_index, generator_class, properties_applied, properties_skipped }
//
// Each generator lives inside a UEnvQueryOption (1:1) appended to UEnvQuery->Options. Tests are
// then added separately via ai.eqs.add_test using the returned option_index.
//
// Pattern: NewObject<UEnvQueryOption>(EQS) → NewObject<UEnvQueryGenerator>(Option, ResolvedClass)
//          → Option->Generator = Gen → ApplyProperties(Gen) → EQS->Options.Add(Option).
//
// Errors:
//   -32004 ObjectNotFound  eqs_path not loadable
//   -32011 WrongClass      generator_class is not a UEnvQueryGenerator subclass / abstract
//   -32020 ClassNotFound   generator_class did not resolve
//   -32023 InvalidClassPath  malformed class path
//   -32027 PIEActive
FMCPResponse Tool_AddGenerator(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_EQSAddGenerator", "Add EQS Generator"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString EQSPath, GeneratorClassPath;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("eqs_path"),        EQSPath,             Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("generator_class"), GeneratorClassPath,  Err)) { return Err; }

	int32 LoadErr = 0;
	FString LoadMsg;
	UEnvQuery* Query = FMCPAssetLoader::Load<UEnvQuery>(EQSPath, LoadErr, LoadMsg);
	if (!Query) { return FMCPToolHelpers::MakeError(Request, LoadErr, LoadMsg); }

	FString ResolveErr;
	UClass* GenClass = FMCPClassResolver::ResolveStrict(GeneratorClassPath, UEnvQueryGenerator::StaticClass(), ResolveErr);
	if (!GenClass)
	{
		// Distinguish family mismatch from "class not found" / "syntactically invalid". The string
		// "is not a subclass" is a stable marker.
		if (ResolveErr.Contains(TEXT("is not a subclass")))
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorWrongClass, ResolveErr);
		}
		if (ResolveErr.Contains(TEXT("must start with '/'")))
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidClassPath, ResolveErr);
		}
		return FMCPToolHelpers::MakeError(Request, kMCPErrorClassNotFound, ResolveErr);
	}

	// Build option + generator. Option is owned by the EQS asset (parent ensures GC keeps it alive);
	// generator is owned by the option (matches engine UEnvQuery serialization model — Options
	// serialize Generator as a sub-object).
	Query->Modify();

	UEnvQueryOption* NewOpt = NewObject<UEnvQueryOption>(
		Query, NAME_None, RF_Public | RF_Transactional);
	check(NewOpt);
	UEnvQueryGenerator* NewGen = NewObject<UEnvQueryGenerator>(
		NewOpt, GenClass, NAME_None, RF_Public | RF_Transactional);
	check(NewGen);
	NewOpt->Generator = NewGen;

	// Apply optional properties to the generator.
	TArray<FString> PropsApplied, PropsSkipped;
	const TSharedPtr<FJsonObject>* PropsObj = nullptr;
	if (Request.Args.IsValid() && Request.Args->TryGetObjectField(TEXT("properties"), PropsObj)
		&& PropsObj && (*PropsObj).IsValid())
	{
		FMCPToolHelpers::ApplyJsonProperties(NewGen, *PropsObj, PropsApplied, PropsSkipped);
	}

	const int32 OptionIdx = Query->GetOptionsMutable().Add(NewOpt);
	Scope.DirtyPackage(Query->GetPackage());

	FMCPJsonArrayBuilder AppliedArr;
	for (const FString& S : PropsApplied) { AppliedArr.AddString(S); }
	FMCPJsonArrayBuilder SkippedArr;
	for (const FString& S : PropsSkipped) { SkippedArr.AddString(S); }

	return FMCPJsonBuilder()
		.Bool(TEXT("added"),               true)
		.Int (TEXT("option_index"),        OptionIdx)
		.Str (TEXT("generator_class"),     GenClass->GetPathName())
		.Str (TEXT("eqs_path"),            Query->GetPathName())
		.Arr (TEXT("properties_applied"),  AppliedArr.ToValueArray())
		.Arr (TEXT("properties_skipped"),  SkippedArr.ToValueArray())
		.BuildSuccess(Request);
}

// --- ai.eqs.add_test (Wave P) -----------------------------------------------------------------
//
// Args:    { eqs_path: string, option_index: int, test_class: string, properties?: {...} }
// Result:  { added, test_index, test_class, properties_applied, properties_skipped }
//
// Appends a UEnvQueryTest to the specified option's Tests[]. Test ownership matches the engine
// pattern: Option owns each Test sub-object.
//
// Errors:
//   -32004 ObjectNotFound       eqs_path not loadable
//   -32011 WrongClass           test_class is not a UEnvQueryTest subclass
//   -32020 ClassNotFound        test_class did not resolve
//   -32023 InvalidClassPath     malformed test_class path
//   -32026 PropertyIndexOOB     option_index out of range
//   -32027 PIEActive
FMCPResponse Tool_AddTest(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_EQSAddTest", "Add EQS Test"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString EQSPath, TestClassPath;
	int32 OptionIndex = 0;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("eqs_path"),     EQSPath,        Err)) { return Err; }
	if (!FMCPToolHelpers::RequireIntField   (Request, TEXT("option_index"), OptionIndex,    Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("test_class"),   TestClassPath,  Err)) { return Err; }

	int32 LoadErr = 0;
	FString LoadMsg;
	UEnvQuery* Query = FMCPAssetLoader::Load<UEnvQuery>(EQSPath, LoadErr, LoadMsg);
	if (!Query) { return FMCPToolHelpers::MakeError(Request, LoadErr, LoadMsg); }

	const TArray<TObjectPtr<UEnvQueryOption>>& OptionsMut = Query->GetOptionsMutable();
	if (OptionIndex < 0 || OptionIndex >= OptionsMut.Num())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPropertyIndexOOB,
			FString::Printf(TEXT("option_index %d out of range [0, %d) for EQS '%s'"),
				OptionIndex, OptionsMut.Num(), *Query->GetPathName()));
	}
	UEnvQueryOption* Option = OptionsMut[OptionIndex];
	if (!Option)
	{
		// Null entry inside Options[] — corrupt asset; surface as InternalError so caller can
		// distinguish from "no such index".
		return FMCPToolHelpers::MakeError(Request, kAIEQSErrorInternal,
			FString::Printf(TEXT("EQS '%s' has null entry at Options[%d] (corrupt asset?)"),
				*Query->GetPathName(), OptionIndex));
	}

	FString ResolveErr;
	UClass* TestClass = FMCPClassResolver::ResolveStrict(TestClassPath, UEnvQueryTest::StaticClass(), ResolveErr);
	if (!TestClass)
	{
		if (ResolveErr.Contains(TEXT("is not a subclass")))
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorWrongClass, ResolveErr);
		}
		if (ResolveErr.Contains(TEXT("must start with '/'")))
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidClassPath, ResolveErr);
		}
		return FMCPToolHelpers::MakeError(Request, kMCPErrorClassNotFound, ResolveErr);
	}

	Query->Modify();
	Option->Modify();

	UEnvQueryTest* NewTest = NewObject<UEnvQueryTest>(
		Option, TestClass, NAME_None, RF_Public | RF_Transactional);
	check(NewTest);

	TArray<FString> PropsApplied, PropsSkipped;
	const TSharedPtr<FJsonObject>* PropsObj = nullptr;
	if (Request.Args.IsValid() && Request.Args->TryGetObjectField(TEXT("properties"), PropsObj)
		&& PropsObj && (*PropsObj).IsValid())
	{
		FMCPToolHelpers::ApplyJsonProperties(NewTest, *PropsObj, PropsApplied, PropsSkipped);
	}

	const int32 TestIdx = Option->Tests.Add(NewTest);
	Scope.DirtyPackage(Query->GetPackage());

	FMCPJsonArrayBuilder AppliedArr;
	for (const FString& S : PropsApplied) { AppliedArr.AddString(S); }
	FMCPJsonArrayBuilder SkippedArr;
	for (const FString& S : PropsSkipped) { SkippedArr.AddString(S); }

	return FMCPJsonBuilder()
		.Bool(TEXT("added"),               true)
		.Int (TEXT("test_index"),          TestIdx)
		.Str (TEXT("test_class"),          TestClass->GetPathName())
		.Int (TEXT("option_index"),        OptionIndex)
		.Str (TEXT("eqs_path"),            Query->GetPathName())
		.Arr (TEXT("properties_applied"),  AppliedArr.ToValueArray())
		.Arr (TEXT("properties_skipped"),  SkippedArr.ToValueArray())
		.BuildSuccess(Request);
}

// --- Registration ----------------------------------------------------------------------------
void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	RegisterTool(TEXT("ai.eqs.list_queries"),   &Tool_ListQueries,   /*Lane A*/ false);
	RegisterTool(TEXT("ai.eqs.get_query_info"), &Tool_GetQueryInfo,  /*Lane A*/ false);
	RegisterTool(TEXT("ai.eqs.run_query"),      &Tool_RunQuery,      /*Lane A*/ false);
	RegisterTool(TEXT("ai.eqs.create_asset"),   &Tool_CreateAsset,   /*Lane A*/ false);
	RegisterTool(TEXT("ai.eqs.add_generator"),  &Tool_AddGenerator,  /*Lane A*/ false);
	RegisterTool(TEXT("ai.eqs.add_test"),       &Tool_AddTest,       /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("AIEQS surface registered: 6 ai.eqs.* tools "
			 "(list_queries + get_query_info + run_query + create_asset + add_generator + add_test), all Lane A"));
}

} // namespace FAIEQSTools

// Wave I refactor 2026-05: auto-registration via FMCPSurfaceRegistry replaces the
// manual include + Register call in UnrealMCPBridge.cpp.
#include "MCPSurfaceRegistry.h"
MCP_REGISTER_SURFACE(AIEQSTools, &FAIEQSTools::Register)

#undef LOCTEXT_NAMESPACE
