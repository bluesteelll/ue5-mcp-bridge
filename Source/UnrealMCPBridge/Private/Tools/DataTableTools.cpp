// Copyright FatumGame. All Rights Reserved.

#include "DataTableTools.h"

#include "MCPSurfaceRegistry.h"

#include "FMCPDispatchQueue.h"
#include "MCPAssetLoader.h"
#include "MCPMutatorScope.h"
#include "MCPToolHelpers.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPPageCursor.h"
#include "Utils/MCPReflection.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "DataTableEditorUtils.h"
#include "Engine/DataTable.h"
#include "HAL/UnrealMemory.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// DT_ prefix per the unity-build symbol-collision convention. The four shared helpers
	// (StampIds / MakeError / MakeSuccessObj / RequireStringField) and the per-surface asset
	// loader live in FMCPToolHelpers / FMCPAssetLoader — see Phase 1 helper extraction
	// (commits b2fd19d + 8e5384c).
	constexpr int32 kDTErrorInvalidParams = -32602;
	constexpr int32 kDTErrorInternal      = -32603;

	/**
	 * Build a JSON object holding every UPROPERTY on ``RowStruct`` read from the row memory at
	 * ``RowData``. Mirrors the field-iteration loop used by GameplayTagTools / marshall.read_property
	 * — uses FMCPReflection::ReadPropertyValueAt for each property so the round-trip JSON shape
	 * matches every other reflection-backed tool in the bridge.
	 */
	TSharedRef<FJsonObject> DT_BuildRowValueObject(const UScriptStruct* RowStruct, const uint8* RowData)
	{
		check(RowStruct);
		check(RowData);
		TSharedRef<FJsonObject> ValuesObj = MakeShared<FJsonObject>();
		for (TFieldIterator<FProperty> It(RowStruct); It; ++It)
		{
			const FProperty* Prop = *It;
			if (!Prop) { continue; }
			const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(RowData);
			TSharedPtr<FJsonValue> JsonVal = FMCPReflection::ReadPropertyValueAt(Prop, ValuePtr);
			if (JsonVal.IsValid())
			{
				ValuesObj->SetField(Prop->GetName(), JsonVal);
			}
		}
		return ValuesObj;
	}
} // namespace

namespace FDataTableTools
{

// ─── data_table.list ──────────────────────────────────────────────────────────────────────────
//
// Args:    { path_prefix?: string, page_size?: int (default 100, clamp [1,1000]), page_token?: string }
// Result:  { data_tables: [{ asset_path, name, row_struct_path, row_count }],
//            total_known, next_page_token? }
//
// Read-only — no PIE guard. Row count is fetched by loading the asset (cheap for small DTs;
// FAssetData asset registry tags do NOT include row count, only RowStructure path).
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

	// FilterHash so cursor staleness is detectable across pages.
	const uint32 FilterHash = GetTypeHash(PathPrefix);

	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FARFilter Filter;
	Filter.ClassPaths.Add(UDataTable::StaticClass()->GetClassPathName());
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
			return FMCPToolHelpers::MakeError(Request, kDTErrorInvalidParams,
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

	static const FName RowStructureTag(TEXT("RowStructure"));

	TArray<TSharedPtr<FJsonValue>> TableArr;
	const int32 EndIdx = FMath::Min(StartIdx + PageSize, Assets.Num());
	TableArr.Reserve(EndIdx - StartIdx);
	for (int32 i = StartIdx; i < EndIdx; ++i)
	{
		const FAssetData& A = Assets[i];
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("asset_path"), A.GetSoftObjectPath().ToString());
		Obj->SetStringField(TEXT("name"), A.AssetName.ToString());

		// Try the AR tag first — avoids loading the table just to read the RowStruct path.
		FString RowStructTagValue;
		if (A.GetTagValue(RowStructureTag, RowStructTagValue) && !RowStructTagValue.IsEmpty())
		{
			Obj->SetStringField(TEXT("row_struct_path"), RowStructTagValue);
		}

		// Row count requires actually loading the table — there's no AR tag for it. Load is
		// idempotent (returns the existing object on subsequent calls) so callers paginating
		// pay the load cost once per table.
		int32 RowCount = 0;
		if (UDataTable* DT = Cast<UDataTable>(A.GetAsset()))
		{
			RowCount = DT->GetRowMap().Num();
			// Backfill row_struct_path if the AR tag was absent (e.g. old asset, dirty in editor).
			if (!Obj->HasField(TEXT("row_struct_path")))
			{
				if (const UScriptStruct* RowStruct = DT->GetRowStruct())
				{
					Obj->SetStringField(TEXT("row_struct_path"), RowStruct->GetPathName());
				}
			}
		}
		Obj->SetNumberField(TEXT("row_count"), RowCount);
		TableArr.Add(MakeShared<FJsonValueObject>(Obj));
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetArrayField(TEXT("data_tables"), TableArr);
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

// ─── data_table.get_rows ──────────────────────────────────────────────────────────────────────
//
// Args:    { data_table_path: string, row_name_filter?: string (substring match),
//            page_size?: int (default 50, clamp [1, 500]), page_token?: string }
// Result:  { row_struct_path, rows: [{ row_name, values: { field_name: typed_json_value, ... } }],
//            total_known, next_page_token? }
//
// Read-only — no PIE guard. Row iteration order = TMap iteration order (implementation-defined,
// but stable within a single edit session — the underlying TMap doesn't re-hash on read).
FMCPResponse Tool_GetRows(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString DataTablePath;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("data_table_path"), DataTablePath, Err)) { return Err; }

	FString RowNameFilter;
	if (Request.Args.IsValid())
	{
		Request.Args->TryGetStringField(TEXT("row_name_filter"), RowNameFilter);
	}

	int32 PageSize = 50;
	if (Request.Args.IsValid()) { Request.Args->TryGetNumberField(TEXT("page_size"), PageSize); }
	PageSize = FMath::Clamp(PageSize, 1, 500);

	FString PageToken;
	if (Request.Args.IsValid()) { Request.Args->TryGetStringField(TEXT("page_token"), PageToken); }

	int32 LoadErrCode = 0;
	FString LoadErrMsg;
	UDataTable* DT = FMCPAssetLoader::Load<UDataTable>(DataTablePath, LoadErrCode, LoadErrMsg);
	if (!DT) { return FMCPToolHelpers::MakeError(Request, LoadErrCode, LoadErrMsg); }

	const UScriptStruct* RowStruct = DT->GetRowStruct();
	if (!RowStruct)
	{
		return FMCPToolHelpers::MakeError(Request, kDTErrorInternal,
			FString::Printf(TEXT("DataTable '%s' has no row struct (mis-configured asset)"),
				*DataTablePath));
	}

	// Snapshot row names in sorted order so pagination has a stable key. Filter by substring
	// here BEFORE pagination so total_known reflects the filtered count.
	const TMap<FName, uint8*>& RowMap = DT->GetRowMap();
	TArray<FName> RowNames;
	RowNames.Reserve(RowMap.Num());
	for (const auto& Pair : RowMap)
	{
		if (!RowNameFilter.IsEmpty())
		{
			const FString NameStr = Pair.Key.ToString();
			if (!NameStr.Contains(RowNameFilter)) { continue; }
		}
		RowNames.Add(Pair.Key);
	}
	RowNames.Sort([](const FName& A, const FName& B)
	{
		return A.LexicalLess(B);
	});

	// Decode cursor (simple string-keyed offset; no filter-hash guard — row mutation IS expected
	// between pages and we want survival across inserts/deletes the same way the AR cursor handles it).
	int32 StartIdx = 0;
	if (!PageToken.IsEmpty())
	{
		while (StartIdx < RowNames.Num() && RowNames[StartIdx].ToString() <= PageToken)
		{
			++StartIdx;
		}
	}

	TArray<TSharedPtr<FJsonValue>> RowArr;
	const int32 EndIdx = FMath::Min(StartIdx + PageSize, RowNames.Num());
	RowArr.Reserve(EndIdx - StartIdx);
	for (int32 i = StartIdx; i < EndIdx; ++i)
	{
		const FName& RowName = RowNames[i];
		uint8* const* RowDataPtr = RowMap.Find(RowName);
		if (!RowDataPtr || !*RowDataPtr) { continue; }
		TSharedRef<FJsonObject> RowObj = MakeShared<FJsonObject>();
		RowObj->SetStringField(TEXT("row_name"), RowName.ToString());
		RowObj->SetObjectField(TEXT("values"), DT_BuildRowValueObject(RowStruct, *RowDataPtr));
		RowArr.Add(MakeShared<FJsonValueObject>(RowObj));
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("row_struct_path"), RowStruct->GetPathName());
	Out->SetArrayField(TEXT("rows"), RowArr);
	Out->SetNumberField(TEXT("total_known"), RowNames.Num());
	if (EndIdx < RowNames.Num() && EndIdx > 0)
	{
		Out->SetStringField(TEXT("next_page_token"), RowNames[EndIdx - 1].ToString());
	}
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── data_table.set_row ───────────────────────────────────────────────────────────────────────
//
// Args:    { data_table_path: string, row_name: string, values: object (field_name -> JSON value),
//            create_if_missing?: bool (default false) }
// Result:  { written: bool, was_created: bool, fields_updated: int, fields_skipped: int,
//            row_name, row_struct_path }
//
// PIE-guarded mutator. FMCPMutatorScope wraps PIE-guard + transaction. Per the standard 4-step contract
// for property edits, but we DON'T use FMCPWritePropertyScope here because the target is row
// memory inside the table (not a UPROPERTY ON the UDataTable UObject) — Pre/PostEditChangeProperty
// don't fire usefully for row internals. We use UDataTable::Modify + HandleDataTableChanged
// (which fires the engine's OnDataTableChanged multicast delegate) + MarkPackageDirty instead.
//
// Unknown field names in ``values`` are silently skipped (consistent with the engine's default
// bIgnoreExtraFields=true behaviour on import); they're counted into fields_skipped so callers
// can detect typos.
FMCPResponse Tool_SetRow(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_DataTable_SetRow", "Set DataTable Row"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString DataTablePath, RowNameStr;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("data_table_path"), DataTablePath, Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("row_name"),        RowNameStr,   Err)) { return Err; }

	const TSharedPtr<FJsonObject>* ValuesObjPtr = nullptr;
	if (!Request.Args->TryGetObjectField(TEXT("values"), ValuesObjPtr) || !ValuesObjPtr || !ValuesObjPtr->IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kDTErrorInvalidParams,
			TEXT("data_table.set_row requires args.values (object of field_name -> JSON value)"));
	}
	const TSharedPtr<FJsonObject>& ValuesObj = *ValuesObjPtr;

	bool bCreateIfMissing = false;
	Request.Args->TryGetBoolField(TEXT("create_if_missing"), bCreateIfMissing);

	int32 LoadErrCode = 0;
	FString LoadErrMsg;
	UDataTable* DT = FMCPAssetLoader::Load<UDataTable>(DataTablePath, LoadErrCode, LoadErrMsg);
	if (!DT) { return FMCPToolHelpers::MakeError(Request, LoadErrCode, LoadErrMsg); }

	UScriptStruct* RowStruct = const_cast<UScriptStruct*>(DT->GetRowStruct());
	if (!RowStruct)
	{
		return FMCPToolHelpers::MakeError(Request, kDTErrorInternal,
			FString::Printf(TEXT("DataTable '%s' has no row struct (mis-configured asset)"),
				*DataTablePath));
	}

	const FName RowName(*RowNameStr);
	uint8* const* ExistingRowPtr = DT->GetRowMap().Find(RowName);
	const bool bRowExists = (ExistingRowPtr != nullptr && *ExistingRowPtr != nullptr);

	if (!bRowExists && !bCreateIfMissing)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("row '%s' not found on DataTable '%s'; pass create_if_missing=true to create"),
				*RowNameStr, *DataTablePath));
	}

	DT->Modify();

	uint8* RowData = nullptr;
	bool bWasCreated = false;
	if (bRowExists)
	{
		RowData = *ExistingRowPtr;
	}
	else
	{
		// FDataTableEditorUtils::AddRow is the engine's canonical path — handles row memory
		// allocation via FMemory::Malloc + RowStruct::InitializeStruct + AddRowInternal +
		// HandleDataTableChanged + Pre/Post broadcast + its own nested FScopedTransaction.
		// Returns the new uint8* row data pointer directly; nullptr only on the early-out
		// conditions we've already screened (null DT, NAME_None row, already-present row,
		// null RowStruct).
		RowData = FDataTableEditorUtils::AddRow(DT, RowName);
		if (!RowData)
		{
			Scope.Abort();
			return FMCPToolHelpers::MakeError(Request, kDTErrorInternal,
				FString::Printf(TEXT("FDataTableEditorUtils::AddRow returned null for row '%s' on '%s'"),
					*RowNameStr, *DataTablePath));
		}
		bWasCreated = true;
	}
	check(RowData != nullptr);

	// Apply each provided field; unknown fields silently skipped (count for caller diagnostic).
	int32 FieldsUpdated = 0;
	int32 FieldsSkipped = 0;
	for (const auto& FieldPair : ValuesObj->Values)
	{
		const FString& FieldName = FieldPair.Key;
		FProperty* FieldProp = RowStruct->FindPropertyByName(FName(*FieldName));
		if (!FieldProp)
		{
			++FieldsSkipped;
			continue;
		}
		void* FieldValuePtr = FieldProp->ContainerPtrToValuePtr<void>(RowData);
		FString WriteErr;
		if (FMCPReflection::WritePropertyValueAt(FieldProp, FieldValuePtr, FieldPair.Value, DT, WriteErr))
		{
			++FieldsUpdated;
		}
		else
		{
			++FieldsSkipped;
			UE_LOG(LogMCP, Verbose,
				TEXT("data_table.set_row: field '%s' on row '%s' write rejected: %s"),
				*FieldName, *RowNameStr, *WriteErr);
		}
	}

	// Fire the engine's data-table-changed delegate so editor row viewers refresh + dependent
	// systems re-cache. Pass the specific row name so per-row listeners can target updates.
	DT->HandleDataTableChanged(RowName);

	Scope.DirtyPackage(DT->GetOutermost());

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("written"), true);
	Out->SetBoolField(TEXT("was_created"), bWasCreated);
	Out->SetNumberField(TEXT("fields_updated"), FieldsUpdated);
	Out->SetNumberField(TEXT("fields_skipped"), FieldsSkipped);
	Out->SetStringField(TEXT("row_name"), RowNameStr);
	Out->SetStringField(TEXT("row_struct_path"), RowStruct->GetPathName());
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── data_table.delete_row ────────────────────────────────────────────────────────────────────
//
// Args:    { data_table_path: string, row_name: string }
// Result:  { deleted: bool, row_existed: bool, row_name, remaining_row_count }
//
// PIE-guarded mutator. No-op (deleted=false, row_existed=false) when the row doesn't exist —
// callers can use this for idempotent delete-or-noop without separately probing the table first.
FMCPResponse Tool_DeleteRow(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_DataTable_DeleteRow", "Delete DataTable Row"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString DataTablePath, RowNameStr;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("data_table_path"), DataTablePath, Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("row_name"),        RowNameStr,   Err)) { return Err; }

	int32 LoadErrCode = 0;
	FString LoadErrMsg;
	UDataTable* DT = FMCPAssetLoader::Load<UDataTable>(DataTablePath, LoadErrCode, LoadErrMsg);
	if (!DT) { return FMCPToolHelpers::MakeError(Request, LoadErrCode, LoadErrMsg); }

	const FName RowName(*RowNameStr);
	const bool bRowExisted = (DT->GetRowMap().Find(RowName) != nullptr);

	if (!bRowExisted)
	{
		// Idempotent no-op — abort the transaction so undo history isn't littered with empty ops.
		Scope.Abort();
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("deleted"), false);
		Out->SetBoolField(TEXT("row_existed"), false);
		Out->SetStringField(TEXT("row_name"), RowNameStr);
		Out->SetNumberField(TEXT("remaining_row_count"), DT->GetRowMap().Num());
		return FMCPToolHelpers::MakeSuccessObj(Request, Out);
	}

	// FDataTableEditorUtils::RemoveRow handles its own nested FScopedTransaction +
	// BroadcastPreChange + DataTable->Modify + DestroyStruct + FMemory::Free +
	// BroadcastPostChange. Returns true iff the row existed and was successfully removed.
	const bool bRemoved = FDataTableEditorUtils::RemoveRow(DT, RowName);

	// Fire HandleDataTableChanged explicitly with the specific row name — RemoveRow's broadcast
	// path posts EDataTableChangeInfo::RowList (collection-level signal) but NOT the per-row
	// HandleDataTableChanged hook that downstream caches listen on. Pair with MarkPackageDirty.
	if (bRemoved) { DT->HandleDataTableChanged(RowName); }

	Scope.DirtyPackage(DT->GetOutermost());

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("deleted"), bRemoved);
	Out->SetBoolField(TEXT("row_existed"), true);
	Out->SetStringField(TEXT("row_name"), RowNameStr);
	Out->SetNumberField(TEXT("remaining_row_count"), DT->GetRowMap().Num());
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

	RegisterTool(TEXT("data_table.list"),        &Tool_List,       /*Lane A*/ false);
	RegisterTool(TEXT("data_table.get_rows"),    &Tool_GetRows,    /*Lane A*/ false);
	RegisterTool(TEXT("data_table.set_row"),     &Tool_SetRow,     /*Lane A*/ false);
	RegisterTool(TEXT("data_table.delete_row"),  &Tool_DeleteRow,  /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("DataTable surface registered: 4 data_table.* tools "
			 "(list + get_rows + set_row + delete_row), all Lane A"));
}

} // namespace FDataTableTools

#undef LOCTEXT_NAMESPACE

MCP_REGISTER_SURFACE(DataTableTools, &FDataTableTools::Register)
