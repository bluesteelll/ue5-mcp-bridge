// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

/**
 * Tier 2 type marshalling — generic FProperty reflection exposed as C++ dispatch handlers.
 *
 * Phase 1 Day 4-5 work (see D:/tmp/mcp_unreal_blueprint_v2_patch.md §6.1). Lives in C++ because
 * `unreal.UStruct.get_properties()` doesn't exist in the Python bindings (verified empirically —
 * see D:/tmp/mcp_unreal_spike_results.md). The four entry points below provide everything Python
 * tools need to walk arbitrary UObject schemas:
 *
 *   - marshall.list_properties(object_path)               → [{name,type,flags}, ...]
 *   - marshall.read_property (object_path, property_path) → JSON value
 *   - marshall.write_property(object_path, property_path, value) → {ok, error?} (transacted)
 *   - marshall.describe_struct(struct_type_path)          → {fields: [{name,type,flags,offset}]}
 *
 * Threading:
 *   ALL handlers run on the game thread via FMCPDispatchQueue::Drain (OnEndFrame). LoadObject,
 *   FindObject<UScriptStruct>, FScopedTransaction, and ImportText_Direct/ExportTextItem_Direct
 *   all require game-thread context. Do NOT call these from a worker thread.
 *
 * Property-path traversal grammar (Day 4-5):
 *   Dotted segments only — e.g. ``RootComponent.RelativeLocation.X``.
 *   - Object refs (FObjectProperty) → unwrap inner UObject* and recurse.
 *   - Struct refs (FStructProperty) → recurse into the struct's UStruct* with a struct-pointer container.
 *   - Terminal leaf → emit JSON via the type-specific extractor.
 *   Array-index syntax (`Array[3]`) is OUT OF SCOPE for Day 4-5 — deferred to later.
 *
 * Symmetry with Tier 1 (Python marshall.py):
 *   The C++ side emits the same ``{"_kind":"Vector",...}`` discriminator dicts for the well-known
 *   structs FVector / FRotator / FTransform / FLinearColor / FQuat / FName / FSoftObjectPath /
 *   UObject reference. Unknown structs fall back to a generic recursive walk.
 *
 * Failure surfaces (all return FMCPResponse{bIsError=true}):
 *   -32602 InvalidParams         missing/wrong-type field in Request.Args
 *   -32004 ObjectNotFound        LoadObject returned null
 *   -32005 PropertyNotFound      FindPropertyByName failed on a segment
 *   -32006 PropertyTypeMismatch  expected struct/object property but got something else, OR
 *                                ImportText_Direct rejected the supplied value text
 *   -32007 PropertyAccessDenied  write blocked by CPF_BlueprintReadOnly / CPF_EditConst
 */
class FMCPMarshalling
{
public:
	/** Walk every top-level FProperty of the resolved UObject's class and return summary metadata. */
	static FMCPResponse ListProperties(const FMCPRequest& Request);

	/** Resolve `property_path` and return its current value as a JSON-encodable FJsonValue. */
	static FMCPResponse ReadProperty(const FMCPRequest& Request);

	/**
	 * Resolve `property_path` and overwrite its value with `value`. Wrapped in an FScopedTransaction
	 * so the mutation participates in Ctrl+Z. Marks the owning UObject dirty via Modify().
	 */
	static FMCPResponse WriteProperty(const FMCPRequest& Request);

	/** Resolve `struct_type_path` to a UScriptStruct and return its field schema. */
	static FMCPResponse DescribeStruct(const FMCPRequest& Request);
};
