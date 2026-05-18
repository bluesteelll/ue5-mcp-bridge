// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ScopedTransaction.h"

class FJsonObject;
class FJsonValue;
class FProperty;
class UObject;
class UScriptStruct;

/**
 * Generic FProperty reflection helpers. Extracted from FMCPMarshalling.cpp in Phase 3 Day 0 so
 * the property round-trip pipeline can be re-used by `actor.set_property`, `component.set_property`,
 * `level.set_world_settings`, and the composite batch tools shipping in Phase 3 (D7 in the v3 plan).
 *
 * Three layers:
 *
 *   1. **Path-walking primitives** — ``ResolveObjectPath`` / ``ResolveScriptStructPath`` /
 *      ``ResolvePropertyPath`` translate user-supplied strings into FProperty + value-pointer pairs.
 *
 *   2. **Value-pointer read/write** — ``ReadPropertyValueAt`` / ``WritePropertyValueAt`` operate on
 *      the (FProperty*, void*) pair that emerges from path traversal. Pure conversion: NO transactions,
 *      NO PreEditChange / Modify / PostEditChangeProperty (those are the caller's contract).
 *
 *   3. **Target/Property convenience overloads** — ``ReadPropertyValue`` / ``WritePropertyValue``
 *      take a top-level UObject + FProperty and apply ``ContainerPtrToValuePtr`` internally. Useful
 *      when the caller already knows the property is top-level on a known object (e.g. WorldSettings
 *      fields, or an FAssetData-resolved CDO).
 *
 * **Threading.** All helpers MUST run on the game thread. ``LoadObject``, ``FindObject<UScriptStruct>``,
 * ``ImportText_Direct`` / ``ExportTextItem_Direct``, and FScopedTransaction all require it.
 *
 * **JSON shape (round-trip-stable across read/write).**
 *   - FVector       → ``{"_kind":"Vector","x":...,"y":...,"z":...}``
 *   - FRotator      → ``{"_kind":"Rotator","pitch":...,"yaw":...,"roll":...}``
 *   - FQuat         → ``{"_kind":"Quat","x":...,"y":...,"z":...,"w":...}``
 *   - FTransform    → ``{"_kind":"Transform","translation":Vector,"rotation":Quat,"scale":Vector}``
 *   - FLinearColor  → ``{"_kind":"LinearColor","r":...,"g":...,"b":...,"a":...}``
 *   - FName         → ``{"_kind":"Name","value":"..."}``
 *   - FText         → ``{"_kind":"Text","value":"..."}``
 *   - SoftObjectPtr → ``{"_kind":"SoftObjectPath","value":"..."}``
 *   - Hard object   → ``{"_kind":"ObjectRef","path":"...","class":"..."}``
 *   - Enums         → ``{"_kind":"Enum","type":"...","value":N,"name":"..."}``
 *   - Unknown struct→ ``{"_kind":"Struct","_type":"...","<field>":..., ...}``
 *   - Map           → ``{"_kind":"Map","pairs":[{"key":...,"value":...}, ...]}``
 *   - Unsupported   → ``{"_kind":"Unsupported","type":"...","text":"<UE text-export>"}``
 *
 * **Path grammar (dotted-only for Day 0).** ``RootComponent.RelativeLocation.X`` traverses
 * UObject and UStruct refs. Array-index syntax (``Array[3]``) is OUT OF SCOPE; will be added in a
 * later phase when a tool needs it.
 *
 * **Failure surfaces (numeric error codes from MCPTypes.h).**
 *   -32004 ``kMCPErrorObjectNotFound``        — ResolveObjectPath returned null
 *   -32005 ``kMCPErrorPropertyNotFound``      — FindPropertyByName failed on a segment
 *   -32006 ``kMCPErrorPropertyTypeMismatch``  — intermediate path node was neither object nor struct,
 *                                              OR a write rejected the supplied JSON shape
 *   -32007 ``kMCPErrorPropertyAccessDenied``  — caller's responsibility to enforce (see
 *                                              ``FMCPWritePropertyScope`` doc), NOT raised here.
 */
namespace FMCPReflection
{
	// ─── Identifier resolution ─────────────────────────────────────────────────────────────────────

	/**
	 * Resolve an object path string to a UObject*.
	 *
	 * Accepts:
	 *   - Full asset path: ``/Game/Foo/Bar.Bar`` (with or without ``_C`` class suffix)
	 *   - Soft object path strings (resolved via TryLoad())
	 *   - Already-loaded transient objects (``/Engine/Transient.MyObj``)
	 *   - CDO paths (``/Script/Engine.Default__GameUserSettings``)
	 *
	 * Returns nullptr on failure. NEVER asserts.
	 */
	UNREALMCPBRIDGE_API UObject* ResolveObjectPath(const FString& Path);

	/** Lookup a UScriptStruct by its full path name (e.g. ``/Script/CoreUObject.Vector``). */
	UNREALMCPBRIDGE_API const UScriptStruct* ResolveScriptStructPath(const FString& Path);

	/**
	 * Stable, JSON-friendly type name for a property — uses ``FProperty::GetCPPType`` so the result
	 * is the source-form identifier (``FVector``, ``int32``, ``TArray<float>``).
	 */
	UNREALMCPBRIDGE_API FString DescribePropertyType(const FProperty* Prop);

	/** Build a ``{name, type, flags, offset}`` JSON summary of one FProperty. Used by list/describe. */
	UNREALMCPBRIDGE_API TSharedRef<FJsonObject> MakePropertySummary(const FProperty* Prop);

	// ─── Dotted-path traversal ─────────────────────────────────────────────────────────────────────

	/**
	 * Walk a dotted property path from ``RootTarget`` down to the terminal property.
	 *
	 * Object refs are auto-unwrapped — they become the new container (the inner UObject*).
	 * Struct refs descend into the struct's UStruct* with a void* container pointer.
	 * Any segment whose property doesn't exist → ``OutErrorCode = kMCPErrorPropertyNotFound``.
	 * Any non-leaf segment that isn't an object or struct → ``kMCPErrorPropertyTypeMismatch``.
	 *
	 * On success returns true and:
	 *   - ``OutContainer`` = the top-level UObject (same as RootTarget; surfaced for clarity, so
	 *     callers building an FMCPWritePropertyScope have an explicit place to read it from).
	 *   - ``OutContainerPtr`` = the writable byte address of the terminal value (i.e.
	 *     ``LeafProp->ContainerPtrToValuePtr<void>(immediate container)``).
	 *   - ``OutLeafProp`` = the terminal FProperty*.
	 *
	 * On failure returns false and populates ``OutErrorCode`` + ``OutError``.
	 */
	UNREALMCPBRIDGE_API bool ResolvePropertyPath(
		UObject* RootTarget,
		const FString& DottedPath,
		UObject*& OutContainer,
		void*& OutContainerPtr,
		FProperty*& OutLeafProp,
		int32& OutErrorCode,
		FString& OutError);

	// ─── Value-pointer read/write (universal primitives) ───────────────────────────────────────────

	/**
	 * Pure conversion: FProperty value → FJsonValue.
	 *
	 * ``ValuePtr`` MUST already point at the value memory (caller did ContainerPtrToValuePtr).
	 * NEVER returns null — unsupported properties emit ``{"_kind":"Unsupported", ...}``.
	 *
	 * **No side effects.** Safe to call on a non-mutating path (read_property dispatch handler).
	 */
	UNREALMCPBRIDGE_API TSharedPtr<FJsonValue> ReadPropertyValueAt(
		const FProperty* Prop,
		const void* ValuePtr);

	/**
	 * Pure conversion: FJsonValue → FProperty value at ``ValuePtr``.
	 *
	 * ``OwnerObject`` is forwarded to ``FProperty::ImportText_Direct`` for the text-fallback path
	 * (some properties need an outer to resolve relative paths inside text literals).
	 *
	 * **No transactions, no PreEditChange / Modify / PostEditChangeProperty.** Caller MUST wrap the
	 * call in an ``FMCPWritePropertyScope`` (see below) — the scope's RAII contract guarantees the
	 * 4-step edit pattern fires in the correct order even on early return.
	 *
	 * Returns false on type mismatch / parse failure; ``OutError`` carries a human-readable message
	 * the caller surfaces as ``kMCPErrorPropertyTypeMismatch`` (-32006).
	 */
	UNREALMCPBRIDGE_API bool WritePropertyValueAt(
		FProperty* Prop,
		void* ValuePtr,
		const TSharedPtr<FJsonValue>& Value,
		UObject* OwnerObject,
		FString& OutError);

	// ─── Target/Property convenience overloads (top-level properties only) ─────────────────────────

	/**
	 * Convenience over ``ReadPropertyValueAt`` — applies ``Prop->ContainerPtrToValuePtr<void>(Target)``
	 * internally. ONLY valid when ``Prop`` is a top-level property on ``Target``'s UClass; for nested
	 * paths use ``ResolvePropertyPath`` + ``ReadPropertyValueAt`` instead.
	 */
	UNREALMCPBRIDGE_API TSharedPtr<FJsonValue> ReadPropertyValue(
		const UObject* Target,
		const FProperty* Prop);

	/**
	 * Convenience over ``WritePropertyValueAt`` — applies ``Prop->ContainerPtrToValuePtr<void>(Target)``
	 * internally and forwards ``Target`` as the OwnerObject for text-import fallback. ONLY valid when
	 * ``Prop`` is a top-level property on ``Target``'s UClass.
	 *
	 * **Same contract as ``WritePropertyValueAt``** — caller wraps in ``FMCPWritePropertyScope``.
	 */
	UNREALMCPBRIDGE_API bool WritePropertyValue(
		UObject* Target,
		FProperty* Prop,
		const TSharedPtr<FJsonValue>& Value,
		FString& OutError);
}

/**
 * RAII helper owning the 4-step property-edit contract for a single write.
 *
 *   ctor: ``Target->PreEditChange(Prop) → Target->Modify() → FScopedTransaction(label)``
 *   dtor: ``Target->PostEditChangeProperty(FPropertyChangedEvent(Prop))``
 *
 * Mandatory call pattern at EVERY write site (refactored ``FMCPMarshalling::WriteProperty`` +
 * future ``actor.set_property`` / ``component.set_property`` / ``level.set_world_settings`` /
 * ``actor.batch_set_property``):
 *
 * .. code-block:: cpp
 *
 *     // 1. Edit-const gate FIRST (early return → no transaction opened, no Pre/Post imbalance)
 *     if (Prop->HasAnyPropertyFlags(CPF_EditConst | CPF_BlueprintReadOnly | CPF_DisableEditOnInstance))
 *     {
 *         return Error(kMCPErrorPropertyAccessDenied, ...);
 *     }
 *     // 2. Scope owns Pre/Modify/Transaction; dtor fires PostEditChangeProperty even on early-return
 *     {
 *         FMCPWritePropertyScope Scope(Target, Prop, LOCTEXT("SetProp", "MCP: set property"));
 *         FString WriteErr;
 *         if (!FMCPReflection::WritePropertyValueAt(Prop, ValuePtr, Val, Target, WriteErr))
 *         {
 *             return Error(kMCPErrorPropertyTypeMismatch, WriteErr);
 *         }
 *     }
 *     // 3. PostEditChangeProperty fires on Scope destructor as control leaves the block
 *
 * The 4-step contract is now syntactically un-skippable — Pre fires in ctor before any caller
 * code, Post fires in dtor regardless of how the block exits (normal, return, throw, early-out).
 *
 * **NEVER instantiate as a temporary.** ``FMCPWritePropertyScope(...)`` on its own line would
 * destruct immediately, firing Post before the write happens. Always bind to a named local:
 * ``FMCPWritePropertyScope Scope(...)``.
 *
 * Lifetime invariants checked via ``check()`` in the ctor.
 */
class UNREALMCPBRIDGE_API FMCPWritePropertyScope
{
public:
	FMCPWritePropertyScope(UObject* InTarget, FProperty* InProp, const FText& TransactionLabel);
	~FMCPWritePropertyScope();

	// Non-copyable, non-movable — the Pre/Post pair is tied to a single UObject*+FProperty*.
	FMCPWritePropertyScope(const FMCPWritePropertyScope&) = delete;
	FMCPWritePropertyScope& operator=(const FMCPWritePropertyScope&) = delete;
	FMCPWritePropertyScope(FMCPWritePropertyScope&&) = delete;
	FMCPWritePropertyScope& operator=(FMCPWritePropertyScope&&) = delete;

private:
	UObject* Target;
	FProperty* Prop;
	FScopedTransaction Transaction;
};
