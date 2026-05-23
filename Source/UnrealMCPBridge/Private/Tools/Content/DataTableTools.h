// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Wave H Surface 1 — UDataTable tabular game data surface. 4 user-visible tools, all Lane A.
 *
 * Tool roster:
 *   data_table.list        — paginated UDataTable enumeration via IAssetRegistry::GetAssets,
 *                            path_prefix-filtered. Per-entry: { asset_path, row_struct_path,
 *                            row_count }. Standard FMCPPageCursor keyed by ObjectPath with
 *                            filter hash including path_prefix. Read-only, no PIE guard.
 *   data_table.get_rows    — enumerate rows of a single table, paginated by row name. Each row:
 *                            { row_name, values: { field_name: typed_json_value, ... } }.
 *                            Field values marshalled through FMCPReflection::ReadPropertyValueAt
 *                            (standard JSON shape — Vector/Rotator/Quat/Transform/ObjectRef/
 *                            Enum/Struct/Map/etc.). Optional ``row_name_filter`` matches rows
 *                            whose name string contains the substring (case-sensitive).
 *                            Read-only, no PIE guard.
 *   data_table.set_row     — update an existing row, or create one when ``create_if_missing=true``.
 *                            For each provided field, looks up FProperty on the RowStruct and
 *                            writes via FMCPReflection::WritePropertyValueAt. Unknown fields are
 *                            silently skipped (consistent with engine import-with-extra-fields
 *                            default). PIE-guarded mutator with FScopedTransaction + HandleDataTableChanged
 *                            + MarkPackageDirty.
 *   data_table.delete_row  — UDataTable::RemoveRow. No-op when the row doesn't exist (returns
 *                            deleted=false, row_existed=false). PIE-guarded mutator with
 *                            FScopedTransaction + HandleDataTableChanged + MarkPackageDirty.
 *
 * **Read tools (list/get_rows) bypass PIE guard.** Mutators (set_row/delete_row) refuse during
 * PIE with -32027 + frozen kMCPMessagePIEActive text.
 *
 * **Row struct contract.** Each DataTable's RowStruct may be any UScriptStruct. We do NOT
 * require derivation from FTableRowBase — the engine's
 * ``UDataTable::AddRow(FName, const uint8*, const UScriptStruct*)`` overload accepts arbitrary
 * row structs. Unknown UScriptStruct's are still supported as long as their fields can be
 * marshalled via the standard FMCPReflection round-trip.
 *
 * **Pagination semantics.** ``data_table.list`` uses the standard FMCPPageCursor over
 * ObjectPath (matches mesh.list / anim.list_sequences / texture.list shape). ``data_table.get_rows``
 * does its own simple offset-from-string pagination keyed on the row's FName string (the row map
 * iteration order is implementation-defined but stable within a single edit session — sufficient
 * for tabular data which is typically dozens to low thousands of rows).
 *
 * **Error codes (all reused from existing range — no new codes):**
 *   -32004 ObjectNotFound        DataTable / row name not loadable / missing
 *   -32010 InvalidPath           malformed data_table_path
 *   -32011 WrongClass            asset isn't UDataTable
 *   -32027 PIEActive             editor-world mutator during PIE
 *   -32602 InvalidParams         missing required args
 *   -32603 InternalError         DataTable has no row struct (mis-configured asset)
 */
namespace FDataTableTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	// ─── Wave H Surface 1: DataTable tools ──────────────────────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_List(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_GetRows(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_SetRow(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_DeleteRow(const FMCPRequest& Request);
}
