// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MCPTypes.h"
#include "MCPToolHelpers.h"

/**
 * Fluent builder for `FJsonObject` results. Replaces ~40% of the 2 065 manual `SetXxxField` calls
 * across 63 tool surfaces with a more readable chain syntax.
 *
 * Usage:
 *     return FMCPJsonBuilder()
 *         .Str(TEXT("path"), AssetPath)
 *         .Num(TEXT("count"), Count)
 *         .Bool(TEXT("loaded"), bLoaded)
 *         .Arr(TEXT("rows"), MoveTemp(Rows))
 *         .BuildSuccess(Request);
 *
 * Composing nested objects:
 *     FMCPJsonBuilder()
 *         .Str(TEXT("name"), Name)
 *         .Object(TEXT("transform"), [&](FMCPJsonBuilder& Sub) {
 *             Sub.Num(TEXT("x"), Loc.X)
 *                .Num(TEXT("y"), Loc.Y)
 *                .Num(TEXT("z"), Loc.Z);
 *         })
 *         .BuildSuccess(Request);
 *
 * **Thread-safety:** No global state — safe to use from any thread that's already safe to build
 * FJsonObject on (i.e. same as raw FJsonObject usage; Lane B-safe).
 */
class FMCPJsonBuilder
{
public:
	explicit FMCPJsonBuilder() : Obj(MakeShared<FJsonObject>()) {}

	/** Wrap an existing object (advanced: when caller has a partially-built object already). */
	explicit FMCPJsonBuilder(TSharedRef<FJsonObject> InExisting) : Obj(InExisting) {}

	// ----- Scalars -----

	FMCPJsonBuilder& Str(const TCHAR* Key, const FString& Value)
	{
		Obj->SetStringField(Key, Value);
		return *this;
	}

	FMCPJsonBuilder& Num(const TCHAR* Key, double Value)
	{
		Obj->SetNumberField(Key, Value);
		return *this;
	}

	FMCPJsonBuilder& Int(const TCHAR* Key, int64 Value)
	{
		Obj->SetNumberField(Key, static_cast<double>(Value));
		return *this;
	}

	FMCPJsonBuilder& Bool(const TCHAR* Key, bool Value)
	{
		Obj->SetBoolField(Key, Value);
		return *this;
	}

	FMCPJsonBuilder& Null(const TCHAR* Key)
	{
		Obj->SetField(Key, MakeShared<FJsonValueNull>());
		return *this;
	}

	// ----- Optional setters (skip if empty/zero/false) -----

	/** Set only if `Value` is non-empty. Useful for omitting unused optional output fields. */
	FMCPJsonBuilder& OptStr(const TCHAR* Key, const FString& Value)
	{
		if (!Value.IsEmpty())
		{
			Obj->SetStringField(Key, Value);
		}
		return *this;
	}

	/** Set only if `bCondition` is true. */
	FMCPJsonBuilder& If(bool bCondition, TFunctionRef<void(FMCPJsonBuilder&)> Action)
	{
		if (bCondition)
		{
			Action(*this);
		}
		return *this;
	}

	// ----- Composites -----

	FMCPJsonBuilder& Arr(const TCHAR* Key, TArray<TSharedPtr<FJsonValue>> Value)
	{
		Obj->SetArrayField(Key, MoveTemp(Value));
		return *this;
	}

	/** Build a nested object via lambda. */
	template<typename TLambda>
	FMCPJsonBuilder& Object(const TCHAR* Key, TLambda&& Build)
	{
		FMCPJsonBuilder Sub;
		Build(Sub);
		Obj->SetObjectField(Key, Sub.Obj);
		return *this;
	}

	/** Set an already-built FJsonObject as a sub-field. */
	FMCPJsonBuilder& ObjectShared(const TCHAR* Key, TSharedRef<FJsonObject> Existing)
	{
		Obj->SetObjectField(Key, Existing);
		return *this;
	}

	/** Set a raw FJsonValue (escape hatch for callers that already have one). */
	FMCPJsonBuilder& Field(const TCHAR* Key, TSharedRef<FJsonValue> Value)
	{
		Obj->SetField(Key, Value);
		return *this;
	}

	// ----- Terminal: produce final FMCPResponse -----

	/**
	 * Build a success response wrapping the accumulated object. Stamps the request id +
	 * original-id-string for echo via `FMCPToolHelpers::MakeSuccessObj`.
	 */
	FMCPResponse BuildSuccess(const FMCPRequest& Request) const
	{
		return FMCPToolHelpers::MakeSuccessObj(Request, Obj);
	}

	/** Return the underlying shared-ref for callers that need the raw object. */
	TSharedRef<FJsonObject> ToShared() const { return Obj; }

	/** Return as TSharedPtr (for legacy interop with code that uses TSharedPtr<FJsonObject>). */
	TSharedPtr<FJsonObject> ToSharedPtr() const { return Obj; }

private:
	TSharedRef<FJsonObject> Obj;
};

/**
 * Helper for building array-of-objects payloads. Replaces the common pattern of
 *
 *     TArray<TSharedPtr<FJsonValue>> Items;
 *     for (auto& X : Source) {
 *         TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
 *         Item->SetStringField(...);
 *         ...
 *         Items.Add(MakeShared<FJsonValueObject>(Item));
 *     }
 *
 * with:
 *
 *     FMCPJsonArrayBuilder Items;
 *     for (auto& X : Source) {
 *         Items.AddObject([&](FMCPJsonBuilder& B) { B.Str(...).Num(...); });
 *     }
 *     Items.ToValueArray() // -> TArray<TSharedPtr<FJsonValue>>
 */
class FMCPJsonArrayBuilder
{
public:
	FMCPJsonArrayBuilder() = default;

	template<typename TLambda>
	FMCPJsonArrayBuilder& AddObject(TLambda&& Build)
	{
		FMCPJsonBuilder Sub;
		Build(Sub);
		Values.Add(MakeShared<FJsonValueObject>(Sub.ToShared()));
		return *this;
	}

	FMCPJsonArrayBuilder& AddString(const FString& Value)
	{
		Values.Add(MakeShared<FJsonValueString>(Value));
		return *this;
	}

	FMCPJsonArrayBuilder& AddNumber(double Value)
	{
		Values.Add(MakeShared<FJsonValueNumber>(Value));
		return *this;
	}

	FMCPJsonArrayBuilder& AddBool(bool Value)
	{
		Values.Add(MakeShared<FJsonValueBoolean>(Value));
		return *this;
	}

	/** Append a pre-built FJsonValue (escape hatch). */
	FMCPJsonArrayBuilder& AddValue(TSharedRef<FJsonValue> Value)
	{
		Values.Add(Value);
		return *this;
	}

	int32 Num() const { return Values.Num(); }

	/** Move out the underlying array — `*this` is dead after this call. */
	TArray<TSharedPtr<FJsonValue>> ToValueArray() { return MoveTemp(Values); }

private:
	TArray<TSharedPtr<FJsonValue>> Values;
};
