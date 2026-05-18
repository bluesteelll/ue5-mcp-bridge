// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"

class FJsonObject;
class FJsonValue;

/**
 * Phase 4 — FEdGraphPinType ↔ JSON round-trip.
 *
 * Every BP-surface tool that touches a variable, function parameter, or graph-pin type uses this
 * single converter so the on-the-wire shape is identical across ``bp.list_variables``,
 * ``bp.get_variable``, ``bp.list_functions``, ``bp.get_function``, ``bp.list_nodes_in_function``,
 * and (Days 6-10) ``bp.add_variable`` / ``bp.add_function``.
 *
 * **Wire shape (D3 — round-trips losslessly).**
 *
 * .. code-block:: jsonc
 *
 *     {
 *       "category":               "Boolean"                // PC_* enum string (see CategoryToWire)
 *       "subcategory":            "" | "double" | "self"   // PC_Real subtype + PSC_* hints
 *       "subcategory_object_path": null | "/Script/Engine.Actor"   // Class/Struct/Enum target
 *       "is_array":               false,
 *       "is_set":                 false,
 *       "is_map":                 false,
 *       "is_reference":           false,                   // bIsReference (function params)
 *       "is_const":               false,                   // bIsConst (function params)
 *       "is_weak_pointer":        false,                   // bIsWeakPointer (object refs)
 *       "value_type":             null                      // present + non-null when is_map=true;
 *                                                          // nested shape: { category, subcategory,
 *                                                          // subcategory_object_path, is_weak_pointer }
 *     }
 *
 * **Category strings (D4: every PC_* in UE 5.7 covered or fail-fast).**
 *
 * +-------------------+-----------------------------------------------------------+
 * | Wire string       | Source PC_* constant (``UEdGraphSchema_K2``)              |
 * +-------------------+-----------------------------------------------------------+
 * | "Boolean"         | PC_Boolean                                                |
 * | "Byte"            | PC_Byte                                                   |
 * | "Int"             | PC_Int                                                    |
 * | "Int64"           | PC_Int64                                                  |
 * | "Real"            | PC_Real (subcategory="double" or "float")                 |
 * | "String"          | PC_String                                                 |
 * | "Name"            | PC_Name                                                   |
 * | "Text"            | PC_Text                                                   |
 * | "Object"          | PC_Object                                                 |
 * | "Class"           | PC_Class                                                  |
 * | "SoftObject"      | PC_SoftObject                                             |
 * | "SoftClass"       | PC_SoftClass                                              |
 * | "Interface"       | PC_Interface                                              |
 * | "Struct"          | PC_Struct (subcategory_object_path → UScriptStruct)       |
 * | "Enum"            | PC_Enum (subcategory_object_path → UEnum)                 |
 * | "Wildcard"        | PC_Wildcard                                               |
 * | "Delegate"        | PC_Delegate (subcategory_object_path → UFunction sig)     |
 * | "MCDelegate"      | PC_MCDelegate (subcategory_object_path → UFunction sig)   |
 * | "FieldPath"       | PC_FieldPath                                              |
 * | "Exec"            | PC_Exec (read-only — execution pins on K2 nodes)          |
 * +-------------------+-----------------------------------------------------------+
 *
 * **Unsupported category → -32032 PinTypeUnsupported (D4).** Tool callers MUST propagate the
 * ``OutErrorCode`` they receive from ``ToJson`` / ``FromJson`` rather than coercing to a lossy
 * fallback. Forward-compat: any future PC_Verse or new container category appears as -32032
 * immediately, with the category name in the message — caller knows what to file/skip.
 *
 * **Container types.** ``ContainerType`` (None / Array / Set / Map) collapses into three booleans
 * for wire simplicity. Map's value type lives in ``PinValueType`` (FEdGraphTerminalType) and is
 * emitted as a NESTED pin-type object under ``value_type``. The nested object omits container flags
 * (Map keys and values cannot themselves be containers in UE 5.7).
 *
 * **subcategory_object_path resolution.**
 *   - ``ToJson`` reads ``PinSubCategoryObject->GetPathName()`` when valid; emits null otherwise.
 *   - ``FromJson`` calls ``LoadObject<UObject>(nullptr, *Path)``. Unresolvable paths surface as
 *     -32032 with "subcategory_object_path '...' could not be resolved" (intentional — Object/Class
 *     pin types without a concrete subcategory class are degenerate and the caller likely typoed).
 *
 * **Threading.** ``ToJson`` is safe on any thread (read-only — no LoadObject, just GetPathName
 * on already-resolved object pointers). ``FromJson`` MUST run on the game thread (LoadObject).
 *
 * **Lane B status.** Read-only ToJson can safely run from Lane B; FromJson cannot. Phase 4 ships
 * every tool Lane A so this isn't exercised today, but the contract is preserved for future demote.
 */
namespace FMCPPinTypeUtils
{
	/**
	 * Serialise ``PinType`` to its canonical JSON object form.
	 *
	 * Returns a populated FJsonObject on success. On unsupported PC_* category sets
	 * ``OutErrorCode = kMCPErrorPinTypeUnsupported`` (-32032) + populates ``OutError`` with a
	 * diagnostic message and returns nullptr. Caller surfaces the error code through its own
	 * response helper (e.g. ``BP_MakeError(Req, kMCPErrorPinTypeUnsupported, OutError)``).
	 *
	 * For pin types with PinSubCategoryObject set, the path is read via ``GetPathName`` — no
	 * load required (the object is already resolved in the source pin type).
	 */
	UNREALMCPBRIDGE_API TSharedPtr<FJsonObject> ToJson(
		const FEdGraphPinType& PinType,
		int32& OutErrorCode,
		FString& OutError);

	/**
	 * Parse a JSON pin-type object back into an ``FEdGraphPinType``.
	 *
	 * Validates the ``category`` field exists and maps to a known PC_* constant. For Object/Class/
	 * Struct/Enum/SoftObject/SoftClass/Interface/Delegate categories, ``subcategory_object_path``
	 * is required and resolved via ``LoadObject<UObject>`` (with retry of ``+_C`` suffix for class
	 * paths missing the suffix).
	 *
	 * Returns true on success and populates ``OutPinType``. On failure returns false and sets
	 * ``OutErrorCode`` + ``OutError``. Error code is always ``kMCPErrorPinTypeUnsupported``
	 * (-32032) for unknown categories OR ``kMCPErrorObjectNotFound`` (-32004) for unresolvable
	 * subcategory paths.
	 */
	UNREALMCPBRIDGE_API bool FromJson(
		const TSharedPtr<FJsonObject>& Obj,
		FEdGraphPinType& OutPinType,
		int32& OutErrorCode,
		FString& OutError);

	/**
	 * Return the canonical wire-string for an FName PinCategory (e.g. ``PC_Boolean`` → ``"Boolean"``).
	 * Returns empty string on unknown category — caller treats as -32032.
	 *
	 * Exposed publicly so tools that build their own pin-type-adjacent JSON (e.g.
	 * ``bp.list_nodes_in_function`` walking K2 graph pins) can re-use the same mapping table
	 * without duplicating the switch.
	 */
	UNREALMCPBRIDGE_API FString CategoryToWire(const FName& PinCategory);

	/**
	 * Inverse of ``CategoryToWire``. Returns ``NAME_None`` on unknown wire string.
	 */
	UNREALMCPBRIDGE_API FName WireToCategory(const FString& Wire);
}
