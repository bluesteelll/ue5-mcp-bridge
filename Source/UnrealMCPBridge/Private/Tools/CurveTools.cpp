// Copyright FatumGame. All Rights Reserved.

#include "CurveTools.h"

#include "MCPSurfaceRegistry.h"

#include "FMCPDispatchQueue.h"
#include "MCPAssetLoader.h"
#include "MCPMutatorScope.h"
#include "MCPToolHelpers.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPPageCursor.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Curves/CurveBase.h"
#include "Curves/CurveFloat.h"
#include "Curves/CurveLinearColor.h"
#include "Curves/CurveVector.h"
#include "Curves/KeyHandle.h"
#include "Curves/RealCurve.h"
#include "Curves/RichCurve.h"
#include "Curves/SimpleCurve.h"
#include "Engine/CurveTable.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// CRV_ prefix per the unity-build symbol-collision convention. The four shared helpers
	// (StampIds / MakeError / MakeSuccessObj / RequireStringField) live in FMCPToolHelpers — see
	// Phase 1 helper extraction (commit b2fd19d). Polymorphic curve-family loader still lives
	// here because it accepts EITHER UCurveBase OR UCurveTable (FMCPAssetLoader::Load<T> is
	// single-class).
	constexpr int32 kCRVErrorInvalidParams = -32602;
	constexpr int32 kCRVErrorInternal      = -32603;

	/**
	 * Load a curve-family UObject (UCurveFloat / UCurveLinearColor / UCurveVector / UCurveTable)
	 * by path. Wraps FMCPAssetLoader::LoadRaw + the polymorphic curve-family gate (UCurveBase OR
	 * UCurveTable). Returns nullptr + OutError populated on any failure.
	 */
	UObject* CRV_LoadCurveByPath(const FString& Path, int32& OutErrorCode, FString& OutError)
	{
		UObject* Loaded = FMCPAssetLoader::LoadRaw(Path, OutErrorCode, OutError);
		if (!Loaded) { return nullptr; }

		// Curve-family gate. UCurveBase covers Float/LinearColor/Vector; UCurveTable is independent.
		const bool bIsCurveBase  = Loaded->IsA<UCurveBase>();
		const bool bIsCurveTable = Loaded->IsA<UCurveTable>();
		if (!bIsCurveBase && !bIsCurveTable)
		{
			OutErrorCode = kMCPErrorWrongClass;
			OutError = FString::Printf(
				TEXT("'%s' is class '%s'; expected UCurveFloat / UCurveLinearColor / UCurveVector / UCurveTable"),
				*Path, *Loaded->GetClass()->GetPathName());
			return nullptr;
		}
		return Loaded;
	}

	// ─── Interp-mode string ↔ enum conversion ──────────────────────────────────────────────────

	const TCHAR* CRV_InterpModeToString(ERichCurveInterpMode Mode)
	{
		switch (Mode)
		{
		case RCIM_Linear:   return TEXT("Linear");
		case RCIM_Constant: return TEXT("Constant");
		case RCIM_Cubic:    return TEXT("Cubic");
		case RCIM_None:     return TEXT("None");
		default:            return TEXT("None");
		}
	}

	/** Parse interp-mode string. Empty / unrecognized → bOutOk=false. */
	ERichCurveInterpMode CRV_InterpModeFromString(const FString& Str, bool& bOutOk)
	{
		bOutOk = true;
		if (Str.Equals(TEXT("Linear"),   ESearchCase::IgnoreCase)) { return RCIM_Linear;   }
		if (Str.Equals(TEXT("Constant"), ESearchCase::IgnoreCase)) { return RCIM_Constant; }
		if (Str.Equals(TEXT("Cubic"),    ESearchCase::IgnoreCase)) { return RCIM_Cubic;    }
		if (Str.Equals(TEXT("None"),     ESearchCase::IgnoreCase)) { return RCIM_None;     }
		// "Auto" and "User" both map to Cubic on the FRichCurve InterpMode axis — the actual
		// auto-vs-user distinction lives on FRichCurveKey::TangentMode. We accept these aliases
		// for convenience (caller is allowed to specify tangent_in/out independently) but treat
		// the interp_mode itself as Cubic.
		if (Str.Equals(TEXT("Auto"), ESearchCase::IgnoreCase)) { return RCIM_Cubic; }
		if (Str.Equals(TEXT("User"), ESearchCase::IgnoreCase)) { return RCIM_Cubic; }
		bOutOk = false;
		return RCIM_None;
	}

	// ─── Curve-class identification ────────────────────────────────────────────────────────────

	/** Wire-form class identifier returned in curve.list / curve.get_data responses. */
	const TCHAR* CRV_ClassNameWire(const UObject* Curve)
	{
		if (!Curve) { return TEXT(""); }
		if (Curve->IsA<UCurveFloat>())       { return TEXT("UCurveFloat");       }
		if (Curve->IsA<UCurveLinearColor>()) { return TEXT("UCurveLinearColor"); }
		if (Curve->IsA<UCurveVector>())      { return TEXT("UCurveVector");      }
		if (Curve->IsA<UCurveTable>())       { return TEXT("UCurveTable");       }
		return TEXT("");
	}

	/**
	 * Build the JSON for a single key from a FRichCurveKey. The ``channel`` field is conditionally
	 * added by the caller (multi-channel curves only); this helper emits the time/value/interp/tangents.
	 */
	TSharedRef<FJsonObject> CRV_KeyToJson(const FRichCurveKey& Key)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("time"),         static_cast<double>(Key.Time));
		Obj->SetNumberField(TEXT("value"),        static_cast<double>(Key.Value));
		Obj->SetStringField(TEXT("interp_mode"),  CRV_InterpModeToString(Key.InterpMode.GetValue()));
		Obj->SetNumberField(TEXT("tangent_in"),   static_cast<double>(Key.ArriveTangent));
		Obj->SetNumberField(TEXT("tangent_out"),  static_cast<double>(Key.LeaveTangent));
		return Obj;
	}

	/** Same for FSimpleCurveKey (no per-key tangents; one curve-wide InterpMode lives on the curve). */
	TSharedRef<FJsonObject> CRV_SimpleKeyToJson(const FSimpleCurveKey& Key, ERichCurveInterpMode CurveInterpMode)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("time"),         static_cast<double>(Key.Time));
		Obj->SetNumberField(TEXT("value"),        static_cast<double>(Key.Value));
		Obj->SetStringField(TEXT("interp_mode"),  CRV_InterpModeToString(CurveInterpMode));
		// FSimpleCurve has no tangents; emit zeros so wire shape stays consistent.
		Obj->SetNumberField(TEXT("tangent_in"),   0.0);
		Obj->SetNumberField(TEXT("tangent_out"),  0.0);
		return Obj;
	}

	/**
	 * Append every key of a FRichCurve to OutKeys. ``ChannelName`` is the optional per-key channel
	 * label written to each emitted object (empty = single-channel curve, no channel field added).
	 */
	void CRV_AppendRichCurveKeys(const FRichCurve& Curve, const FString& ChannelName,
		TArray<TSharedPtr<FJsonValue>>& OutKeys)
	{
		const TArray<FRichCurveKey>& Keys = Curve.GetConstRefOfKeys();
		for (const FRichCurveKey& Key : Keys)
		{
			TSharedRef<FJsonObject> Obj = CRV_KeyToJson(Key);
			if (!ChannelName.IsEmpty())
			{
				Obj->SetStringField(TEXT("channel"), ChannelName);
			}
			OutKeys.Add(MakeShared<FJsonValueObject>(Obj));
		}
	}

	/** Append every key of a FSimpleCurve. SimpleCurves have one InterpMode for the whole curve. */
	void CRV_AppendSimpleCurveKeys(const FSimpleCurve& Curve, const FString& ChannelName,
		TArray<TSharedPtr<FJsonValue>>& OutKeys)
	{
		const TArray<FSimpleCurveKey>& Keys = Curve.GetConstRefOfKeys();
		const ERichCurveInterpMode Mode = Curve.GetKeyInterpMode();
		for (const FSimpleCurveKey& Key : Keys)
		{
			TSharedRef<FJsonObject> Obj = CRV_SimpleKeyToJson(Key, Mode);
			if (!ChannelName.IsEmpty())
			{
				Obj->SetStringField(TEXT("channel"), ChannelName);
			}
			OutKeys.Add(MakeShared<FJsonValueObject>(Obj));
		}
	}

	/**
	 * Parse a single key descriptor from ``args.keys[N]`` (or the inline add_key fields). Required:
	 * time + value. Optional: interp_mode (default Cubic), tangent_in/out (default 0). Returns false
	 * on missing/malformed fields + sets OutError.
	 */
	bool CRV_ParseKeyDesc(const TSharedPtr<FJsonObject>& KeyObj, FRichCurveKey& OutKey,
		FString& OutChannel, FString& OutErrorMsg)
	{
		if (!KeyObj.IsValid())
		{
			OutErrorMsg = TEXT("key descriptor is not a JSON object");
			return false;
		}
		double Time = 0.0;
		if (!KeyObj->TryGetNumberField(TEXT("time"), Time))
		{
			OutErrorMsg = TEXT("key descriptor missing 'time' (number)");
			return false;
		}
		double Value = 0.0;
		if (!KeyObj->TryGetNumberField(TEXT("value"), Value))
		{
			OutErrorMsg = TEXT("key descriptor missing 'value' (number)");
			return false;
		}
		OutKey.Time  = static_cast<float>(Time);
		OutKey.Value = static_cast<float>(Value);

		// Interp mode — default Cubic per spec for both set_data fall-through and add_key default.
		FString InterpStr = TEXT("Cubic");
		KeyObj->TryGetStringField(TEXT("interp_mode"), InterpStr);
		bool bInterpOk = true;
		const ERichCurveInterpMode Mode = CRV_InterpModeFromString(InterpStr, bInterpOk);
		if (!bInterpOk)
		{
			OutErrorMsg = FString::Printf(
				TEXT("unknown interp_mode '%s' (expected Linear/Constant/Cubic/None/Auto/User)"),
				*InterpStr);
			return false;
		}
		OutKey.InterpMode = Mode;

		// Optional tangents.
		double TangentIn = 0.0;
		double TangentOut = 0.0;
		KeyObj->TryGetNumberField(TEXT("tangent_in"),  TangentIn);
		KeyObj->TryGetNumberField(TEXT("tangent_out"), TangentOut);
		OutKey.ArriveTangent = static_cast<float>(TangentIn);
		OutKey.LeaveTangent  = static_cast<float>(TangentOut);

		// Channel (multi-channel curves only — empty for single-channel).
		OutChannel.Reset();
		KeyObj->TryGetStringField(TEXT("channel"), OutChannel);

		return true;
	}

	/**
	 * Map a channel-name string to an index for a multi-channel curve. Returns -1 + sets
	 * OutErrorMsg on miss. Accepts case-insensitive R/G/B/A for UCurveLinearColor and X/Y/Z for
	 * UCurveVector, plus numeric "0".."3" for either.
	 */
	int32 CRV_ChannelNameToIndex(const FString& ChannelName, bool bIsColor, FString& OutErrorMsg)
	{
		if (ChannelName.IsEmpty())
		{
			OutErrorMsg = TEXT("multi-channel curve requires 'channel' field on each key");
			return -1;
		}

		// Numeric forms first.
		if (ChannelName.IsNumeric())
		{
			const int32 Idx = FCString::Atoi(*ChannelName);
			const int32 MaxIdx = bIsColor ? 3 : 2;
			if (Idx < 0 || Idx > MaxIdx)
			{
				OutErrorMsg = FString::Printf(TEXT("channel index %d out of range [0..%d]"), Idx, MaxIdx);
				return -1;
			}
			return Idx;
		}

		if (bIsColor)
		{
			if (ChannelName.Equals(TEXT("R"), ESearchCase::IgnoreCase)) { return 0; }
			if (ChannelName.Equals(TEXT("G"), ESearchCase::IgnoreCase)) { return 1; }
			if (ChannelName.Equals(TEXT("B"), ESearchCase::IgnoreCase)) { return 2; }
			if (ChannelName.Equals(TEXT("A"), ESearchCase::IgnoreCase)) { return 3; }
			OutErrorMsg = FString::Printf(TEXT("unknown LinearColor channel '%s' (expected R/G/B/A)"), *ChannelName);
			return -1;
		}
		// Vector
		if (ChannelName.Equals(TEXT("X"), ESearchCase::IgnoreCase)) { return 0; }
		if (ChannelName.Equals(TEXT("Y"), ESearchCase::IgnoreCase)) { return 1; }
		if (ChannelName.Equals(TEXT("Z"), ESearchCase::IgnoreCase)) { return 2; }
		OutErrorMsg = FString::Printf(TEXT("unknown Vector channel '%s' (expected X/Y/Z)"), *ChannelName);
		return -1;
	}

	const TCHAR* CRV_ChannelIndexToName(int32 Idx, bool bIsColor)
	{
		if (bIsColor)
		{
			switch (Idx)
			{
			case 0: return TEXT("R");
			case 1: return TEXT("G");
			case 2: return TEXT("B");
			case 3: return TEXT("A");
			default: return TEXT("");
			}
		}
		switch (Idx)
		{
		case 0: return TEXT("X");
		case 1: return TEXT("Y");
		case 2: return TEXT("Z");
		default: return TEXT("");
		}
	}

	/**
	 * Resolve the operating FRichCurve(s) on a loaded curve UObject. Caller picks single- vs
	 * multi-channel branch via OutNumChannels (1 for UCurveFloat / UCurveTable rich-row, 3/4 for
	 * UCurveVector / UCurveLinearColor). UCurveTable simple-row path returns nullptr (caller falls
	 * back to FSimpleCurve handling via CRV_GetSimpleCurveFromTable).
	 *
	 * RowName: empty for non-table curves; required for UCurveTable.
	 *
	 * Returns: array of FRichCurve* pointers (size = OutNumChannels), or null entries on miss.
	 */
	bool CRV_GetRichCurvesForWrite(UObject* Curve, const FString& RowName,
		TArray<FRichCurve*>& OutCurves, int32& OutErrorCode, FString& OutErrorMsg)
	{
		OutCurves.Reset();
		if (UCurveFloat* CF = Cast<UCurveFloat>(Curve))
		{
			OutCurves.Add(&CF->FloatCurve);
			return true;
		}
		if (UCurveLinearColor* CLC = Cast<UCurveLinearColor>(Curve))
		{
			for (int32 i = 0; i < 4; ++i) { OutCurves.Add(&CLC->FloatCurves[i]); }
			return true;
		}
		if (UCurveVector* CV = Cast<UCurveVector>(Curve))
		{
			for (int32 i = 0; i < 3; ++i) { OutCurves.Add(&CV->FloatCurves[i]); }
			return true;
		}
		if (UCurveTable* CT = Cast<UCurveTable>(Curve))
		{
			if (RowName.IsEmpty())
			{
				OutErrorCode = kCRVErrorInvalidParams;
				OutErrorMsg = TEXT("UCurveTable requires 'key' field (row name)");
				return false;
			}
			if (CT->GetCurveTableMode() == ECurveTableMode::SimpleCurves)
			{
				OutErrorCode = kCRVErrorInvalidParams;
				OutErrorMsg = TEXT("UCurveTable is in SimpleCurves mode — set_data/add_key not yet supported for simple-row tables");
				return false;
			}
			const FName RowFName(*RowName);
			const TMap<FName, FRichCurve*>& RichMap = CT->GetRichCurveRowMap();
			FRichCurve* const* Found = RichMap.Find(RowFName);
			if (!Found || !*Found)
			{
				OutErrorCode = kMCPErrorObjectNotFound;
				OutErrorMsg = FString::Printf(TEXT("row '%s' not found in CurveTable"), *RowName);
				return false;
			}
			OutCurves.Add(*Found);
			return true;
		}
		OutErrorCode = kCRVErrorInternal;
		OutErrorMsg = FString::Printf(TEXT("unhandled curve subclass '%s'"),
			*Curve->GetClass()->GetPathName());
		return false;
	}
} // namespace

namespace FCurveTools
{

// ─── curve.list ───────────────────────────────────────────────────────────────────────────────
//
// Args:    { path_prefix?: string, types?: ["UCurveFloat","UCurveLinearColor","UCurveVector","UCurveTable"]
//             (default all), page_size?: int (default 100, clamp [1,1000]), page_token?: string }
// Result:  { curves: [{ asset_path, curve_class }], total_known, next_page_token? }
//
// Read-only — no PIE guard. The ``types`` filter narrows the AR scan; absent or empty = all four.
FMCPResponse Tool_List(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString PathPrefix;
	if (Request.Args.IsValid()) { Request.Args->TryGetStringField(TEXT("path_prefix"), PathPrefix); }

	int32 PageSize = 100;
	if (Request.Args.IsValid()) { Request.Args->TryGetNumberField(TEXT("page_size"), PageSize); }
	PageSize = FMath::Clamp(PageSize, 1, 1000);

	FString PageToken;
	if (Request.Args.IsValid()) { Request.Args->TryGetStringField(TEXT("page_token"), PageToken); }

	// Build the class allow-set from args.types. Default = all four curve families.
	TSet<FString> TypeAllow;
	bool bExplicitTypes = false;
	if (Request.Args.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* TypesArr = nullptr;
		if (Request.Args->TryGetArrayField(TEXT("types"), TypesArr) && TypesArr)
		{
			for (const TSharedPtr<FJsonValue>& V : *TypesArr)
			{
				if (!V.IsValid()) { continue; }
				FString S;
				if (V->TryGetString(S) && !S.IsEmpty())
				{
					TypeAllow.Add(S);
					bExplicitTypes = true;
				}
			}
		}
	}
	auto AcceptType = [&TypeAllow, bExplicitTypes](const TCHAR* Name) -> bool
	{
		return !bExplicitTypes || TypeAllow.Contains(Name);
	};

	// FilterHash: prefix + sorted-types — staleness probe across pages.
	uint32 FilterHash = GetTypeHash(PathPrefix);
	{
		TArray<FString> Sorted = TypeAllow.Array();
		Sorted.Sort();
		for (const FString& T : Sorted) { FilterHash = HashCombine(FilterHash, GetTypeHash(T)); }
	}

	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FARFilter Filter;
	if (AcceptType(TEXT("UCurveFloat")))
	{
		Filter.ClassPaths.Add(UCurveFloat::StaticClass()->GetClassPathName());
	}
	if (AcceptType(TEXT("UCurveLinearColor")))
	{
		Filter.ClassPaths.Add(UCurveLinearColor::StaticClass()->GetClassPathName());
	}
	if (AcceptType(TEXT("UCurveVector")))
	{
		Filter.ClassPaths.Add(UCurveVector::StaticClass()->GetClassPathName());
	}
	if (AcceptType(TEXT("UCurveTable")))
	{
		Filter.ClassPaths.Add(UCurveTable::StaticClass()->GetClassPathName());
	}
	if (Filter.ClassPaths.Num() == 0)
	{
		// Caller passed an explicit ``types`` array with no recognised entries — empty result is
		// the correct response (not an error). Caller gets an empty curves[] + total_known=0.
		TSharedRef<FJsonObject> OutEmpty = MakeShared<FJsonObject>();
		OutEmpty->SetArrayField(TEXT("curves"), {});
		OutEmpty->SetNumberField(TEXT("total_known"), 0);
		return FMCPToolHelpers::MakeSuccessObj(Request, OutEmpty);
	}
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
			return FMCPToolHelpers::MakeError(Request, kCRVErrorInvalidParams,
				FString::Printf(TEXT("invalid page_token: %s"), *DecodeErr));
		}
		if (!FMCPPageCursorUtils::ValidateAgainstFilter(InCursor, FilterHash))
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorStaleCursor,
				TEXT("filter mutated between pages (path_prefix or types changed); restart pagination"));
		}
		while (StartIdx < Assets.Num() &&
			   Assets[StartIdx].GetSoftObjectPath().ToString() <= InCursor.LastAssetPath)
		{
			++StartIdx;
		}
	}

	TArray<TSharedPtr<FJsonValue>> CurveArr;
	const int32 EndIdx = FMath::Min(StartIdx + PageSize, Assets.Num());
	CurveArr.Reserve(EndIdx - StartIdx);
	for (int32 i = StartIdx; i < EndIdx; ++i)
	{
		const FAssetData& A = Assets[i];
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("asset_path"), A.GetSoftObjectPath().ToString());
		// AR class name is the wire-form curve_class (e.g. "/Script/Engine.CurveFloat"). Strip
		// to bare class name to match the args.types convention.
		const FString ClassPathStr = A.AssetClassPath.ToString();
		FString BareClassName = ClassPathStr;
		int32 DotIdx = INDEX_NONE;
		if (ClassPathStr.FindLastChar(TEXT('.'), DotIdx) && DotIdx + 1 < ClassPathStr.Len())
		{
			BareClassName = ClassPathStr.Mid(DotIdx + 1);
			// Prepend 'U' to match UE convention used in args.types.
			if (!BareClassName.StartsWith(TEXT("U")))
			{
				BareClassName = TEXT("U") + BareClassName;
			}
		}
		Obj->SetStringField(TEXT("curve_class"), BareClassName);
		CurveArr.Add(MakeShared<FJsonValueObject>(Obj));
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetArrayField(TEXT("curves"), CurveArr);
	Out->SetNumberField(TEXT("total_known"), Assets.Num());

	if (EndIdx < Assets.Num() && EndIdx > 0)
	{
		FMCPPageCursor OutCursor;
		OutCursor.FilterHash = FilterHash;
		OutCursor.LastAssetPath = Assets[EndIdx - 1].GetSoftObjectPath().ToString();
		OutCursor.TotalKnownSnapshot = Assets.Num();
		Out->SetStringField(TEXT("next_page_token"), FMCPPageCursorUtils::Encode(OutCursor));
	}

	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── curve.get_data ───────────────────────────────────────────────────────────────────────────
//
// Args:    { curve_path: string, key?: string (UCurveTable row name) }
// Result:  { curve_class, keys: [{ channel?, time, value, interp_mode, tangent_in, tangent_out }] }
//
// Read-only — no PIE guard. ``channel`` only appears for multi-channel curves (UCurveLinearColor
// → R/G/B/A; UCurveVector → X/Y/Z). UCurveTable + key (row name) emits the row's curve keys (no
// channel field; rows are single-channel). UCurveTable + missing key → -32602.
FMCPResponse Tool_GetData(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString CurvePath;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("curve_path"), CurvePath, Err)) { return Err; }

	FString RowName;
	if (Request.Args.IsValid()) { Request.Args->TryGetStringField(TEXT("key"), RowName); }

	int32 LoadErrCode = 0;
	FString LoadErrMsg;
	UObject* CurveObj = CRV_LoadCurveByPath(CurvePath, LoadErrCode, LoadErrMsg);
	if (!CurveObj) { return FMCPToolHelpers::MakeError(Request, LoadErrCode, LoadErrMsg); }

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("curve_class"), CRV_ClassNameWire(CurveObj));

	TArray<TSharedPtr<FJsonValue>> KeysArr;

	// UCurveFloat — single channel.
	if (UCurveFloat* CF = Cast<UCurveFloat>(CurveObj))
	{
		CRV_AppendRichCurveKeys(CF->FloatCurve, FString(), KeysArr);
	}
	// UCurveLinearColor — 4 channels (R/G/B/A).
	else if (UCurveLinearColor* CLC = Cast<UCurveLinearColor>(CurveObj))
	{
		for (int32 i = 0; i < 4; ++i)
		{
			CRV_AppendRichCurveKeys(CLC->FloatCurves[i], CRV_ChannelIndexToName(i, /*bIsColor*/ true), KeysArr);
		}
	}
	// UCurveVector — 3 channels (X/Y/Z).
	else if (UCurveVector* CV = Cast<UCurveVector>(CurveObj))
	{
		for (int32 i = 0; i < 3; ++i)
		{
			CRV_AppendRichCurveKeys(CV->FloatCurves[i], CRV_ChannelIndexToName(i, /*bIsColor*/ false), KeysArr);
		}
	}
	// UCurveTable — requires row name; supports both rich and simple curve rows.
	else if (UCurveTable* CT = Cast<UCurveTable>(CurveObj))
	{
		if (RowName.IsEmpty())
		{
			return FMCPToolHelpers::MakeError(Request, kCRVErrorInvalidParams,
				TEXT("UCurveTable requires 'key' field (row name); use marshall.list_properties or curve.list to discover rows"));
		}
		const FName RowFName(*RowName);
		const TMap<FName, FRealCurve*>& RowMap = CT->GetRowMap();
		FRealCurve* const* Found = RowMap.Find(RowFName);
		if (!Found || !*Found)
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
				FString::Printf(TEXT("row '%s' not found in CurveTable '%s'"), *RowName, *CurvePath));
		}
		// Branch on table mode for downcast safety.
		if (CT->GetCurveTableMode() == ECurveTableMode::SimpleCurves)
		{
			const FSimpleCurve* SC = static_cast<const FSimpleCurve*>(*Found);
			CRV_AppendSimpleCurveKeys(*SC, FString(), KeysArr);
		}
		else
		{
			// RichCurves (also covers Empty mode — RowMap will be empty so we won't reach here).
			const FRichCurve* RC = static_cast<const FRichCurve*>(*Found);
			CRV_AppendRichCurveKeys(*RC, FString(), KeysArr);
		}
		Out->SetStringField(TEXT("row_name"), RowName);
	}
	else
	{
		// Shouldn't happen — CRV_LoadCurveByPath gates the class family.
		return FMCPToolHelpers::MakeError(Request, kCRVErrorInternal,
			FString::Printf(TEXT("unhandled curve subclass '%s'"), *CurveObj->GetClass()->GetPathName()));
	}

	Out->SetArrayField(TEXT("keys"), KeysArr);
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── curve.set_data ───────────────────────────────────────────────────────────────────────────
//
// Args:    { curve_path: string, key?: string (UCurveTable row), keys: [{ time, value, interp_mode?,
//            tangent_in?, tangent_out?, channel? (multi-channel curves only) }] }
// Result:  { prior_key_count, new_key_count }
//
// PIE-guarded mutator. Replaces the ENTIRE key set (clear-then-add). FMCPMutatorScope wraps the
// PIE-guard + transaction + MarkPackageDirty. For multi-channel curves (UCurveLinearColor /
// UCurveVector) each input key MUST carry a ``channel`` field — keys missing channel route to
// channel 0 to preserve single-channel-input parity for UCurveFloat callers. UCurveTable requires
// ``key`` (row name).
FMCPResponse Tool_SetData(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_Curve_SetData", "Set Curve Data"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString CurvePath;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("curve_path"), CurvePath, Err)) { return Err; }

	// Required keys array.
	const TArray<TSharedPtr<FJsonValue>>* KeysArrPtr = nullptr;
	if (!Request.Args->TryGetArrayField(TEXT("keys"), KeysArrPtr) || !KeysArrPtr)
	{
		return FMCPToolHelpers::MakeError(Request, kCRVErrorInvalidParams,
			TEXT("curve.set_data requires args.keys (array of key descriptors)"));
	}
	const TArray<TSharedPtr<FJsonValue>>& InKeys = *KeysArrPtr;

	FString RowName;
	if (Request.Args.IsValid()) { Request.Args->TryGetStringField(TEXT("key"), RowName); }

	int32 LoadErrCode = 0;
	FString LoadErrMsg;
	UObject* CurveObj = CRV_LoadCurveByPath(CurvePath, LoadErrCode, LoadErrMsg);
	if (!CurveObj) { return FMCPToolHelpers::MakeError(Request, LoadErrCode, LoadErrMsg); }

	// Resolve operating FRichCurve(s).
	TArray<FRichCurve*> RichCurves;
	int32 ResolveErrCode = 0;
	FString ResolveErrMsg;
	if (!CRV_GetRichCurvesForWrite(CurveObj, RowName, RichCurves, ResolveErrCode, ResolveErrMsg))
	{
		return FMCPToolHelpers::MakeError(Request, ResolveErrCode, ResolveErrMsg);
	}
	check(RichCurves.Num() >= 1);
	const bool bIsMultiChannel = RichCurves.Num() > 1;
	const bool bIsColor        = CurveObj->IsA<UCurveLinearColor>();

	// Pre-parse + bucket all incoming keys per channel BEFORE mutating, so a bad descriptor doesn't
	// leave the curve partially modified.
	TArray<TArray<FRichCurveKey>> Buckets;
	Buckets.SetNum(RichCurves.Num());
	for (int32 KeyIdx = 0; KeyIdx < InKeys.Num(); ++KeyIdx)
	{
		const TSharedPtr<FJsonValue>& V = InKeys[KeyIdx];
		if (!V.IsValid() || V->Type != EJson::Object)
		{
			return FMCPToolHelpers::MakeError(Request, kCRVErrorInvalidParams,
				FString::Printf(TEXT("keys[%d] is not an object"), KeyIdx));
		}
		const TSharedPtr<FJsonObject>& Obj = V->AsObject();
		FRichCurveKey RCKey;
		FString ChannelName;
		FString ParseErr;
		if (!CRV_ParseKeyDesc(Obj, RCKey, ChannelName, ParseErr))
		{
			return FMCPToolHelpers::MakeError(Request, kCRVErrorInvalidParams,
				FString::Printf(TEXT("keys[%d]: %s"), KeyIdx, *ParseErr));
		}
		int32 ChannelIdx = 0;
		if (bIsMultiChannel)
		{
			FString ChannelErr;
			ChannelIdx = CRV_ChannelNameToIndex(ChannelName, bIsColor, ChannelErr);
			if (ChannelIdx < 0)
			{
				return FMCPToolHelpers::MakeError(Request, kCRVErrorInvalidParams,
					FString::Printf(TEXT("keys[%d]: %s"), KeyIdx, *ChannelErr));
			}
			check(ChannelIdx >= 0 && ChannelIdx < Buckets.Num());
		}
		Buckets[ChannelIdx].Add(RCKey);
	}

	// Count prior keys for the response.
	int32 PriorKeyCount = 0;
	for (FRichCurve* RC : RichCurves)
	{
		check(RC);
		PriorKeyCount += RC->GetConstRefOfKeys().Num();
	}

	CurveObj->Modify();

	int32 NewKeyCount = 0;
	for (int32 Ci = 0; Ci < RichCurves.Num(); ++Ci)
	{
		FRichCurve* RC = RichCurves[Ci];
		check(RC);
		RC->Reset();
		for (const FRichCurveKey& Desc : Buckets[Ci])
		{
			const FKeyHandle Handle = RC->AddKey(Desc.Time, Desc.Value);
			RC->SetKeyInterpMode(Handle, Desc.InterpMode.GetValue());
			// Tangents — only meaningful for Cubic; harmless to set on other modes (Linear/Constant
			// ignore them at eval time).
			FRichCurveKey& WrittenKey = RC->GetKey(Handle);
			WrittenKey.ArriveTangent = Desc.ArriveTangent;
			WrittenKey.LeaveTangent  = Desc.LeaveTangent;
		}
		NewKeyCount += RC->GetConstRefOfKeys().Num();
	}

	Scope.DirtyPackage(CurveObj->GetOutermost());

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetNumberField(TEXT("prior_key_count"), PriorKeyCount);
	Out->SetNumberField(TEXT("new_key_count"),   NewKeyCount);
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── curve.add_key ────────────────────────────────────────────────────────────────────────────
//
// Args:    { curve_path: string, key?: string (UCurveTable row), time: float, value: float,
//             interp_mode?: string (default "Cubic"), tangent_in?: float, tangent_out?: float,
//             channel?: string (multi-channel curves only — required) }
// Result:  { added: bool, was_replaced: bool, new_key_count }
//
// PIE-guarded mutator. If a key with the same time already exists, its value/interp/tangents are
// overwritten in-place (was_replaced=true) — uses FRichCurve::UpdateOrAddKey under the hood so the
// time-tolerance check matches Sequencer behaviour.
FMCPResponse Tool_AddKey(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_Curve_AddKey", "Add Curve Key"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString CurvePath;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("curve_path"), CurvePath, Err)) { return Err; }

	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kCRVErrorInvalidParams, TEXT("missing args object"));
	}

	double Time = 0.0;
	if (!Request.Args->TryGetNumberField(TEXT("time"), Time))
	{
		return FMCPToolHelpers::MakeError(Request, kCRVErrorInvalidParams, TEXT("missing required number field 'time'"));
	}
	double Value = 0.0;
	if (!Request.Args->TryGetNumberField(TEXT("value"), Value))
	{
		return FMCPToolHelpers::MakeError(Request, kCRVErrorInvalidParams, TEXT("missing required number field 'value'"));
	}

	FString InterpStr = TEXT("Cubic");
	Request.Args->TryGetStringField(TEXT("interp_mode"), InterpStr);
	bool bInterpOk = true;
	const ERichCurveInterpMode Mode = CRV_InterpModeFromString(InterpStr, bInterpOk);
	if (!bInterpOk)
	{
		return FMCPToolHelpers::MakeError(Request, kCRVErrorInvalidParams,
			FString::Printf(TEXT("unknown interp_mode '%s' (expected Linear/Constant/Cubic/None/Auto/User)"),
				*InterpStr));
	}

	double TangentIn = 0.0;
	double TangentOut = 0.0;
	Request.Args->TryGetNumberField(TEXT("tangent_in"),  TangentIn);
	Request.Args->TryGetNumberField(TEXT("tangent_out"), TangentOut);

	FString RowName;
	Request.Args->TryGetStringField(TEXT("key"), RowName);

	FString ChannelName;
	Request.Args->TryGetStringField(TEXT("channel"), ChannelName);

	int32 LoadErrCode = 0;
	FString LoadErrMsg;
	UObject* CurveObj = CRV_LoadCurveByPath(CurvePath, LoadErrCode, LoadErrMsg);
	if (!CurveObj) { return FMCPToolHelpers::MakeError(Request, LoadErrCode, LoadErrMsg); }

	TArray<FRichCurve*> RichCurves;
	int32 ResolveErrCode = 0;
	FString ResolveErrMsg;
	if (!CRV_GetRichCurvesForWrite(CurveObj, RowName, RichCurves, ResolveErrCode, ResolveErrMsg))
	{
		return FMCPToolHelpers::MakeError(Request, ResolveErrCode, ResolveErrMsg);
	}
	check(RichCurves.Num() >= 1);

	const bool bIsMultiChannel = RichCurves.Num() > 1;
	const bool bIsColor        = CurveObj->IsA<UCurveLinearColor>();
	int32 ChannelIdx = 0;
	if (bIsMultiChannel)
	{
		FString ChannelErr;
		ChannelIdx = CRV_ChannelNameToIndex(ChannelName, bIsColor, ChannelErr);
		if (ChannelIdx < 0)
		{
			return FMCPToolHelpers::MakeError(Request, kCRVErrorInvalidParams, ChannelErr);
		}
		check(ChannelIdx >= 0 && ChannelIdx < RichCurves.Num());
	}
	FRichCurve* TargetCurve = RichCurves[ChannelIdx];
	check(TargetCurve);

	const int32 PriorKeyCount = TargetCurve->GetConstRefOfKeys().Num();

	CurveObj->Modify();

	// UpdateOrAddKey uses UE_KINDA_SMALL_NUMBER as the time tolerance by default — matches engine
	// behaviour for "key at this time already exists" detection.
	const FKeyHandle Handle = TargetCurve->UpdateOrAddKey(
		static_cast<float>(Time), static_cast<float>(Value), /*bUnwindRotation*/ false);
	TargetCurve->SetKeyInterpMode(Handle, Mode);
	FRichCurveKey& WrittenKey = TargetCurve->GetKey(Handle);
	WrittenKey.ArriveTangent = static_cast<float>(TangentIn);
	WrittenKey.LeaveTangent  = static_cast<float>(TangentOut);

	const int32 NewKeyCount = TargetCurve->GetConstRefOfKeys().Num();
	const bool bWasReplaced = (NewKeyCount == PriorKeyCount);
	const bool bAdded       = !bWasReplaced;

	Scope.DirtyPackage(CurveObj->GetOutermost());

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField  (TEXT("added"),         bAdded);
	Out->SetBoolField  (TEXT("was_replaced"),  bWasReplaced);
	Out->SetNumberField(TEXT("new_key_count"), NewKeyCount);
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── Registration ─────────────────────────────────────────────────────────────────────────────
void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	RegisterTool(TEXT("curve.list"),     &Tool_List,    /*Lane A*/ false);
	RegisterTool(TEXT("curve.get_data"), &Tool_GetData, /*Lane A*/ false);
	RegisterTool(TEXT("curve.set_data"), &Tool_SetData, /*Lane A*/ false);
	RegisterTool(TEXT("curve.add_key"),  &Tool_AddKey,  /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("Curve surface registered: 4 curve.* tools "
			 "(list + get_data + set_data + add_key), all Lane A"));
}

} // namespace FCurveTools

#undef LOCTEXT_NAMESPACE

MCP_REGISTER_SURFACE(CurveTools, &FCurveTools::Register)
