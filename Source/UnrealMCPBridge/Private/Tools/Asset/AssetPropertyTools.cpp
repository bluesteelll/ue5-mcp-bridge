// Copyright FatumGame. All Rights Reserved.

#include "AssetPropertyTools.h"

#include "MCPSurfaceRegistry.h"

#include "FMCPDispatchQueue.h"
#include "MCPJsonBuilder.h"
#include "MCPToolHelpers.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPAssetPathUtils.h"
#include "Utils/MCPReflection.h"
#include "Utils/MCPWorldContext.h"

#include "UObject/UObjectHash.h"  // ForEachObjectWithOuter (Wave S — Package→inner descent)

#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// APT_ prefix per the unity-build symbol-collision pattern (matches ACT_/COMP_/etc).
	// Per-surface error constants kept for readability — same value as kMCPErrorInternal.
	constexpr int32 kAPTErrorInternal = -32603;

	/**
	 * Resolve the ``asset_path`` argument to a loaded UObject. Centralises the (require-field →
	 * ResolveObjectPath → null-check) shape across all 3 surface tools.
	 *
	 * Returns nullptr on failure with ``OutError`` populated; success returns the UObject and
	 * leaves ``OutError`` default-constructed. Caller MUST early-return ``OutError`` on null.
	 */
	UObject* APT_ResolveAssetOrError(const FMCPRequest& Request, FString& OutPath, FMCPResponse& OutError)
	{
		if (!FMCPToolHelpers::RequireStringField(Request, TEXT("asset_path"), OutPath, OutError))
		{
			return nullptr;
		}

		// Wave S fix (2026-05-24): when caller passes package-form path like /Game/Foo/Bar
		// (no .AssetName suffix), FMCPReflection::ResolveObjectPath's FindObject<UObject> may
		// return the UPackage instead of the inner asset. Downstream ResolvePropertyPath then
		// fails with "no property X on Package". Fix: auto-canonicalise to object-path form via
		// FMCPAssetPathUtils::ToObjectPath BEFORE resolve so the lookup always targets the inner
		// asset. Idempotent for paths that already include .AssetName suffix.
		//
		// S+18 (2026-05-26): Normalize-rejected paths used to fall through to raw — that defeated
		// the defence layer (Normalize rejects // / \\ / control-chars / .. / >240 chars). Hostile
		// inputs like /Game//_X were then sent into ResolveObjectPath → FindObject → FName::Init
		// crash. Switch to STRICT: if Normalize rejects, surface kMCPErrorInvalidPath instead of
		// proceeding with the raw input.
		const FString NormalizedPath = FMCPAssetPathUtils::Normalize(OutPath);
		if (NormalizedPath.IsEmpty())
		{
			OutError = FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidPath,
				FString::Printf(
					TEXT("asset_path '%s' rejected by path normalizer (empty segments, control chars, "
					     "relative escape, backslashes, or length > 240 are forbidden)"),
					*OutPath));
			return nullptr;
		}
		const FString ObjectPath = FMCPAssetPathUtils::ToObjectPath(NormalizedPath);

		UObject* Asset = FMCPReflection::ResolveObjectPath(ObjectPath);
		if (!Asset)
		{
			OutError = FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
				FString::Printf(TEXT("asset_path '%s' did not resolve to a loaded UObject"), *OutPath));
			return nullptr;
		}

		// Defensive: if we somehow still got a UPackage (e.g. path was /Memory/X with no inner
		// asset), descend to the first non-Package inner UObject. The asset is usually the package's
		// single primary child with name == package leaf.
		if (Asset->IsA<UPackage>())
		{
			UPackage* Pkg = Cast<UPackage>(Asset);
			UObject* Inner = nullptr;
			ForEachObjectWithOuter(Pkg, [&Inner](UObject* Obj)
			{
				if (!Inner && Obj && !Obj->IsA<UPackage>())
				{
					Inner = Obj;
				}
			}, /*bIncludeNestedObjects=*/false);
			if (Inner)
			{
				Asset = Inner;
			}
		}

		return Asset;
	}

	/**
	 * Recursive walk used by ``asset.list_properties`` to descend into FStructProperty sub-fields
	 * when ``max_depth > 0``. Each call appends one summary per property at the current level;
	 * recurses (decrementing RemainingDepth) into struct properties only.
	 *
	 * Object properties are NOT followed — that would mean loading + walking referenced UObjects
	 * (potentially infinite). Caller can issue a follow-up ``asset.list_properties`` on the resolved
	 * object path instead.
	 */
	void APT_AppendPropertySummaries(
		const UStruct* Container,
		EFieldIteratorFlags::SuperClassFlags InheritFlag,
		int32 RemainingDepth,
		TArray<TSharedPtr<FJsonValue>>& OutSummaries)
	{
		check(Container != nullptr);
		for (TFieldIterator<FProperty> It(Container, InheritFlag); It; ++It)
		{
			const FProperty* Prop = *It;
			OutSummaries.Add(MakeShared<FJsonValueObject>(FMCPReflection::MakePropertySummary(Prop)));

			if (RemainingDepth > 0)
			{
				if (const FStructProperty* StructProp = CastField<FStructProperty>(Prop))
				{
					if (StructProp->Struct)
					{
						// Sub-fields of a USTRUCT — always include "super" so nested struct hierarchy
						// surfaces parent struct fields too. Decrement depth so unbounded recursion
						// is impossible even for self-referential USTRUCTs (UE forbids these but
						// defense-first).
						APT_AppendPropertySummaries(
							StructProp->Struct,
							EFieldIteratorFlags::IncludeSuper,
							RemainingDepth - 1,
							OutSummaries);
					}
				}
			}
		}
	}
}

namespace FAssetPropertyTools
{

// ─── asset.get_property (read-only — works in PIE) ───────────────────────────────────────────
//
// Reads a single FProperty value off a UObject asset by dotted path. Delegates to
// FMCPReflection::ResolvePropertyPath + ReadPropertyValueAt — identical shape to actor.get_property
// minus the actor-resolve step (asset-resolve via FMCPReflection::ResolveObjectPath instead).
//
// Args:    { asset_path: string, property_path: string }
// Result:  { asset_path, property_path, property_type, value: <jsonified> }
FMCPResponse Tool_GetProperty(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString AssetPath;
	FMCPResponse PathErr;
	UObject* Asset = APT_ResolveAssetOrError(Request, AssetPath, PathErr);
	if (!Asset) { return PathErr; }

	FString PropertyPath;
	if (!Request.Args->TryGetStringField(TEXT("property_path"), PropertyPath) || PropertyPath.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			TEXT("missing required string field 'property_path'"));
	}

	UObject* Container = nullptr;
	void* LeafValuePtr = nullptr;
	FProperty* LeafProp = nullptr;
	int32 ErrCode = 0;
	FString ErrMsg;
	if (!FMCPReflection::ResolvePropertyPath(Asset, PropertyPath, Container, LeafValuePtr, LeafProp,
		ErrCode, ErrMsg))
	{
		return FMCPToolHelpers::MakeError(Request, ErrCode, ErrMsg);
	}

	TSharedPtr<FJsonValue> Value = FMCPReflection::ReadPropertyValueAt(LeafProp, LeafValuePtr);

	return FMCPJsonBuilder()
		.Str(TEXT("asset_path"),    Asset->GetPathName())
		.Str(TEXT("property_path"), PropertyPath)
		.Str(TEXT("property_type"), FMCPReflection::DescribePropertyType(LeafProp))
		.Field(TEXT("value"), Value.ToSharedRef())
		.BuildSuccess(Request);
}

// ─── asset.set_property (mutator — PIE-guarded) ──────────────────────────────────────────────
//
// Writes a single FProperty by dotted path on a loaded UObject asset. Implements the D7 contract
// inherited from actor.set_property (ActorTools.cpp:1534):
//   1. Inline PIE guard FIRST (FMCPWritePropertyScope below owns the transaction — wrapping in
//      FMCPMutatorScope here would nest a second transaction).
//   2. Resolve asset via FMCPReflection::ResolveObjectPath (handles /Game/Foo.Foo + _C suffix +
//      SoftObjectPath forms — same as actor.set_property's ACT_ResolveActorOrError minus the
//      AActor cast).
//   3. Resolve dotted property path via FMCPReflection::ResolvePropertyPath.
//   4. Edit-const gate (CPF_EditConst | CPF_DisableEditOnInstance) unless args.bypass_readonly=true.
//      CPF_BlueprintReadOnly intentionally NOT included — the bridge acts as editor surrogate, which
//      Details panel CAN write at design time even on BP-readonly fields (matches wave-2 testing
//      decision in actor.set_property line 1581-1584).
//   5. FMCPWritePropertyScope RAII (PreEditChange / Modify / FScopedTransaction; PostEditChange on
//      dtor). Pattern is syntactically un-skippable.
//   6. WritePropertyValueAt does the JSON-to-FProperty marshalling.
//   7. MarkPackageDirty on the asset's outermost package — distinct from FScopedTransaction (the
//      transaction handles undo/redo; MarkPackageDirty is what activates the "save" affordance in
//      the content browser / "save all dirty" menu items).
//   8. Re-read post-write for round-trip echo (so caller can verify mutation took effect).
//
// Args:    { asset_path: string, property_path: string, value: <jsonified>, bypass_readonly?: bool }
// Result:  { ok: true, asset_path, property_path, value: <re-read echo> }
FMCPResponse Tool_SetProperty(const FMCPRequest& Request)
{
	check(IsInGameThread());

	// FMCPWritePropertyScope below owns the transaction — using FMCPMutatorScope here would open
	// a second nested transaction. Keep the inline PIE guard.
	if (FMCPWorldContext::IsPIEActive())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPIEActive, kMCPMessagePIEActive);
	}

	FString AssetPath;
	FMCPResponse PathErr;
	UObject* Asset = APT_ResolveAssetOrError(Request, AssetPath, PathErr);
	if (!Asset) { return PathErr; }

	FString PropertyPath;
	if (!Request.Args->TryGetStringField(TEXT("property_path"), PropertyPath) || PropertyPath.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			TEXT("missing required string field 'property_path'"));
	}
	const TSharedPtr<FJsonValue> ValueField = Request.Args->TryGetField(TEXT("value"));
	if (!ValueField.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			TEXT("missing required field 'value'"));
	}

	UObject* Container = nullptr;
	void* LeafValuePtr = nullptr;
	FProperty* LeafProp = nullptr;
	int32 ErrCode = 0;
	FString ErrMsg;
	if (!FMCPReflection::ResolvePropertyPath(Asset, PropertyPath, Container, LeafValuePtr, LeafProp,
		ErrCode, ErrMsg))
	{
		return FMCPToolHelpers::MakeError(Request, ErrCode, ErrMsg);
	}

	// Step 1: edit-const gate FIRST (early return, no transaction). Mirrors actor.set_property —
	// CPF_BlueprintReadOnly intentionally OMITTED per wave-2 decision: bridge acts as editor
	// surrogate, Details panel CAN write at design time even when BP code cannot at runtime.
	// TryGetBoolField returns false on missing OR wrong type, leaving OutValue default (false).
	// Prior pattern used HasField + GetBoolField which crashes if field exists but wrong type
	// (e.g., caller passes `bypass_readonly: "yes"` as a string).
	bool bBypassReadOnly = false;
	if (Request.Args.IsValid())
	{
		Request.Args->TryGetBoolField(TEXT("bypass_readonly"), bBypassReadOnly);
	}
	const uint64 Flags = LeafProp->PropertyFlags;
	if (!bBypassReadOnly && (Flags & (CPF_EditConst | CPF_DisableEditOnInstance)))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPropertyAccessDenied,
			FString::Printf(
				TEXT("property '%s' is read-only (CPF flags=%llu); pass args.bypass_readonly=true to override"),
				*LeafProp->GetName(), static_cast<unsigned long long>(Flags)));
	}

	// Step 2: RAII scope owns Pre/Modify/Transaction; dtor fires PostEditChangeProperty.
	FString WriteErr;
	bool bWriteOk = false;
	{
		FMCPWritePropertyScope Scope(Container, LeafProp,
			LOCTEXT("AssetSetProperty", "MCP: set asset property"));
		bWriteOk = FMCPReflection::WritePropertyValueAt(LeafProp, LeafValuePtr, ValueField, Container, WriteErr);
	}
	if (!bWriteOk)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPropertyTypeMismatch,
			FString::Printf(TEXT("write rejected: %s"), *WriteErr));
	}

	// Step 3: mark the asset's outermost package dirty so the editor surfaces the unsaved change
	// (FScopedTransaction handles undo; MarkPackageDirty enables the save affordance).
	if (UPackage* Pkg = Asset->GetOutermost())
	{
		Pkg->MarkPackageDirty();
	}

	// Re-read post-write for round-trip echo.
	TSharedPtr<FJsonValue> EchoValue = FMCPReflection::ReadPropertyValueAt(LeafProp, LeafValuePtr);

	return FMCPJsonBuilder()
		.Bool(TEXT("ok"), true)
		.Str(TEXT("asset_path"),    Asset->GetPathName())
		.Str(TEXT("property_path"), PropertyPath)
		.Field(TEXT("value"), EchoValue.ToSharedRef())
		.BuildSuccess(Request);
}

// ─── asset.list_properties (read-only — works in PIE) ─────────────────────────────────────────
//
// Schema reflection on any loaded UObject asset. Walks the UClass via TFieldIterator and emits
// {name, type, flags, offset} summaries via FMCPReflection::MakePropertySummary (the shared helper
// used by component.list_properties_summary). Optionally recurses into FStructProperty sub-fields
// when max_depth > 0.
//
// Args:
//   - asset_path: string (required)
//   - include_inherited: bool (optional, default true) — true → IncludeSuper, false → ExcludeSuper
//   - max_depth: int (optional, default 0) — 0 = flat (top-level only); 1 = include struct sub-fields;
//                                            N = N levels deep into nested USTRUCTs. Object properties
//                                            are NEVER followed (avoid unbounded UObject graph walk —
//                                            caller can issue a follow-up call on the resolved path).
//
// Result:
//   { asset_path, asset_class, properties: [{name, type, flags, offset}, ...], total_count }
//
// Lane A. Read-only TFieldIterator + MakePropertySummary — could promote to Lane B in the future,
// but shipped defense-first as Lane A in Wave N.
FMCPResponse Tool_ListProperties(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString AssetPath;
	FMCPResponse PathErr;
	UObject* Asset = APT_ResolveAssetOrError(Request, AssetPath, PathErr);
	if (!Asset) { return PathErr; }

	// Optional include_inherited (default true).
	bool bIncludeInherited = true;
	if (Request.Args.IsValid() && Request.Args->HasField(TEXT("include_inherited")))
	{
		Request.Args->TryGetBoolField(TEXT("include_inherited"), bIncludeInherited);
	}
	const EFieldIteratorFlags::SuperClassFlags InheritFlag = bIncludeInherited
		? EFieldIteratorFlags::IncludeSuper
		: EFieldIteratorFlags::ExcludeSuper;

	// Optional max_depth (default 0 = flat). Clamp to a sane upper bound — 8 matches the property
	// path depth cap conceptually (kMCPErrorPropertyPathTooDeep is 16, but recursion is per-summary
	// and explodes quickly with nested structs, so we cap tighter here).
	int32 MaxDepth = 0;
	if (Request.Args.IsValid() && Request.Args->HasField(TEXT("max_depth")))
	{
		double D = 0.0;
		Request.Args->TryGetNumberField(TEXT("max_depth"), D);
		MaxDepth = FMath::Clamp(static_cast<int32>(D), 0, 8);
	}

	const UClass* AssetClass = Asset->GetClass();
	check(AssetClass != nullptr);

	TArray<TSharedPtr<FJsonValue>> Summaries;
	APT_AppendPropertySummaries(AssetClass, InheritFlag, MaxDepth, Summaries);
	const int32 TotalCount = Summaries.Num();

	return FMCPJsonBuilder()
		.Str(TEXT("asset_path"),  Asset->GetPathName())
		.Str(TEXT("asset_class"), AssetClass->GetName())
		.Arr(TEXT("properties"),  MoveTemp(Summaries))
		.Int(TEXT("total_count"), TotalCount)
		.BuildSuccess(Request);
}

// ─── Registration ──────────────────────────────────────────────────────────────────────────────
void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	RegisterTool(TEXT("asset.get_property"),    &Tool_GetProperty,    /*Lane A*/ false);
	RegisterTool(TEXT("asset.set_property"),    &Tool_SetProperty,    /*Lane A*/ false);
	RegisterTool(TEXT("asset.list_properties"), &Tool_ListProperties, /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("AssetProperty surface registered: 3 asset.* tools "
			 "(get_property + set_property + list_properties), all Lane A"));
}

} // namespace FAssetPropertyTools

#undef LOCTEXT_NAMESPACE

MCP_REGISTER_SURFACE(AssetPropertyTools, &FAssetPropertyTools::Register)
