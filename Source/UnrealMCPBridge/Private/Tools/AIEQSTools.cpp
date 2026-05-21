// Copyright FatumGame. All Rights Reserved.

#include "AIEQSTools.h"

#include "FMCPDispatchQueue.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPActorPathUtils.h"
#include "Utils/MCPAssetPathUtils.h"
#include "Utils/MCPPageCursor.h"

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
#include "UObject/UObjectGlobals.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// AIEQS_ prefix per the unity-build symbol-collision convention. The plugin uses unity builds so
	// anonymous-namespace helpers MUST be uniquely prefixed across every Tools/*.cpp.
	constexpr int32 kAIEQSErrorInvalidParams   = -32602;
	constexpr int32 kAIEQSErrorInternal        = -32603;
	constexpr int32 kAIEQSErrorOperationFailed = -32015; // Brief reuses the StaleCursor slot here.

	void AIEQS_StampIds(const FMCPRequest& Request, FMCPResponse& Response)
	{
		Response.RequestId = Request.RequestId;
		Response.OriginalIdString = Request.OriginalIdString;
	}

	FMCPResponse AIEQS_MakeError(const FMCPRequest& Request, int32 Code, const FString& Message)
	{
		FMCPResponse R;
		AIEQS_StampIds(Request, R);
		R.bIsError = true;
		R.ErrorCode = Code;
		R.ErrorMessage = Message;
		return R;
	}

	FMCPResponse AIEQS_MakeSuccessObj(const FMCPRequest& Request, TSharedPtr<FJsonObject> Result)
	{
		FMCPResponse R;
		AIEQS_StampIds(Request, R);
		R.bIsError = false;
		R.Result = MakeShared<FJsonValueObject>(MoveTemp(Result));
		return R;
	}

	bool AIEQS_RequireStringField(const FMCPRequest& Request, const TCHAR* FieldName,
		FString& OutValue, FMCPResponse& OutError)
	{
		if (!Request.Args.IsValid())
		{
			OutError = AIEQS_MakeError(Request, kAIEQSErrorInvalidParams, TEXT("missing args object"));
			return false;
		}
		if (!Request.Args->TryGetStringField(FieldName, OutValue) || OutValue.IsEmpty())
		{
			OutError = AIEQS_MakeError(Request, kAIEQSErrorInvalidParams,
				FString::Printf(TEXT("missing required string field '%s'"), FieldName));
			return false;
		}
		return true;
	}

	/**
	 * Load a UEnvQuery by path. Returns null on failure with OutErrorCode/OutError populated.
	 * Mirrors the DataTableTools / CurveTools loader shape — first tries the normalised path as-is
	 * (works for "/Game/Foo/Bar" + "/Game/Foo/Bar.Bar"), then falls back to the object-path variant
	 * if the first attempt fails.
	 */
	UEnvQuery* AIEQS_LoadQueryByPath(const FString& Path, int32& OutErrorCode, FString& OutError)
	{
		if (Path.IsEmpty())
		{
			OutErrorCode = kMCPErrorInvalidPath;
			OutError = TEXT("query_path is empty");
			return nullptr;
		}
		const FString Normalised = FMCPAssetPathUtils::Normalize(Path);
		if (Normalised.IsEmpty() || !FMCPAssetPathUtils::IsValidGameOrPlugin(Normalised))
		{
			OutErrorCode = kMCPErrorInvalidPath;
			OutError = FString::Printf(TEXT("query_path '%s' malformed or unknown mount"), *Path);
			return nullptr;
		}
		UObject* Loaded = LoadObject<UObject>(nullptr, *Normalised);
		if (!Loaded)
		{
			const FString ObjPath = FMCPAssetPathUtils::ToObjectPath(Normalised);
			if (!ObjPath.IsEmpty() && ObjPath != Normalised)
			{
				Loaded = LoadObject<UObject>(nullptr, *ObjPath);
			}
		}
		if (!Loaded)
		{
			OutErrorCode = kMCPErrorObjectNotFound;
			OutError = FString::Printf(TEXT("EQS query '%s' not loadable"), *Path);
			return nullptr;
		}
		UEnvQuery* Query = Cast<UEnvQuery>(Loaded);
		if (!Query)
		{
			OutErrorCode = kMCPErrorWrongClass;
			OutError = FString::Printf(TEXT("'%s' is class '%s'; expected UEnvQuery"),
				*Path, *Loaded->GetClass()->GetPathName());
			return nullptr;
		}
		return Query;
	}
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
			return AIEQS_MakeError(Request, kAIEQSErrorInvalidParams,
				FString::Printf(TEXT("invalid page_token: %s"), *DecodeErr));
		}
		if (!FMCPPageCursorUtils::ValidateAgainstFilter(InCursor, FilterHash))
		{
			return AIEQS_MakeError(Request, kMCPErrorStaleCursor,
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

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetArrayField(TEXT("queries"), QueryArr);
	Out->SetNumberField(TEXT("total_known"), Assets.Num());

	if (EndIdx < Assets.Num() && EndIdx > 0)
	{
		FMCPPageCursor OutCursor;
		OutCursor.FilterHash = FilterHash;
		OutCursor.LastAssetPath = Assets[EndIdx - 1].GetSoftObjectPath().ToString();
		OutCursor.TotalKnownSnapshot = Assets.Num();
		Out->SetStringField(TEXT("next_page_token"), FMCPPageCursorUtils::Encode(OutCursor));
	}

	return AIEQS_MakeSuccessObj(Request, Out);
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
	if (!AIEQS_RequireStringField(Request, TEXT("query_path"), QueryPath, Err)) { return Err; }

	int32 LoadErrCode = 0;
	FString LoadErrMsg;
	UEnvQuery* Query = AIEQS_LoadQueryByPath(QueryPath, LoadErrCode, LoadErrMsg);
	if (!Query) { return AIEQS_MakeError(Request, LoadErrCode, LoadErrMsg); }

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

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("query_path"), QueryPath);
	Out->SetNumberField(TEXT("options_count"), Options.Num());
	Out->SetArrayField(TEXT("options"), OptionArr);
	return AIEQS_MakeSuccessObj(Request, Out);
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
	if (!AIEQS_RequireStringField(Request, TEXT("query_path"),         QueryPath,   Err)) { return Err; }
	if (!AIEQS_RequireStringField(Request, TEXT("querier_actor_path"), QuerierPath, Err)) { return Err; }

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
		return AIEQS_MakeError(Request, kAIEQSErrorInvalidParams,
			FString::Printf(TEXT("unknown mode '%s'; expected 'single_best' or 'all_matching'"), *ModeStr));
	}

	// Load query asset.
	int32 QueryLoadErrCode = 0;
	FString QueryLoadErrMsg;
	UEnvQuery* Query = AIEQS_LoadQueryByPath(QueryPath, QueryLoadErrCode, QueryLoadErrMsg);
	if (!Query) { return AIEQS_MakeError(Request, QueryLoadErrCode, QueryLoadErrMsg); }

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
		return AIEQS_MakeError(Request, kMCPErrorObjectNotFound, Msg);
	}

	// Get the EQS manager for the querier's world. Editor-world querier → editor manager;
	// PIE-world querier → PIE manager. Either may be null if the world has no AISystem (e.g.
	// commandlet / certain custom configurations) — surface as InternalError.
	UWorld* QuerierWorld = Querier->GetWorld();
	if (!QuerierWorld)
	{
		return AIEQS_MakeError(Request, kAIEQSErrorInternal,
			FString::Printf(TEXT("querier '%s' has no UWorld"), *QuerierPath));
	}
	UEnvQueryManager* EQM = UEnvQueryManager::GetCurrent(QuerierWorld);
	if (!EQM)
	{
		return AIEQS_MakeError(Request, kAIEQSErrorInternal,
			FString::Printf(TEXT("UEnvQueryManager not available for querier's world '%s'"),
				*QuerierWorld->GetName()));
	}

	// Build the request + execute synchronously.
	FEnvQueryRequest EQSRequest(Query, Querier);
	TSharedPtr<FEnvQueryResult> Result = EQM->RunInstantQuery(EQSRequest, Mode);

	if (!Result.IsValid())
	{
		return AIEQS_MakeError(Request, kAIEQSErrorInternal,
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
	// kAIEQSErrorOperationFailed with the status string in the message so the caller can
	// disambiguate. Processing should never appear from an instant query (it's a synchronous
	// terminal call) but treat it as Failed-equivalent for defensiveness.
	if (!Result->IsSuccessful())
	{
		return AIEQS_MakeError(Request, kAIEQSErrorOperationFailed,
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
		return AIEQS_MakeError(Request, kAIEQSErrorInternal,
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

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("status"), StatusStr);
	Out->SetStringField(TEXT("mode"), ModeStr);
	Out->SetNumberField(TEXT("items_count"), Locations.Num());
	Out->SetArrayField(TEXT("results"), ResultArr);
	Out->SetStringField(TEXT("query_path"), QueryPath);
	Out->SetStringField(TEXT("querier_actor_path"), QuerierPath);
	return AIEQS_MakeSuccessObj(Request, Out);
}

// --- Registration ----------------------------------------------------------------------------
void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	RegisterTool(TEXT("ai.eqs.list_queries"),   &Tool_ListQueries,  /*Lane A*/ false);
	RegisterTool(TEXT("ai.eqs.get_query_info"), &Tool_GetQueryInfo, /*Lane A*/ false);
	RegisterTool(TEXT("ai.eqs.run_query"),      &Tool_RunQuery,     /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("AIEQS surface registered: 3 ai.eqs.* tools "
			 "(list_queries + get_query_info + run_query), all Lane A"));
}

} // namespace FAIEQSTools

// Wave I refactor 2026-05: auto-registration via FMCPSurfaceRegistry replaces the
// manual include + Register call in UnrealMCPBridge.cpp.
#include "MCPSurfaceRegistry.h"
MCP_REGISTER_SURFACE(AIEQSTools, &FAIEQSTools::Register)

#undef LOCTEXT_NAMESPACE
