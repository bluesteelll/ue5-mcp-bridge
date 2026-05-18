// Copyright FatumGame. All Rights Reserved.

#include "MCPPageCursor.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Base64.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace FMCPPageCursorUtils
{

FString Encode(const FMCPPageCursor& Cursor)
{
	if (Cursor.IsEmpty())
	{
		return {};
	}

	// Compact JSON form: short field names (h / p / t) to minimise wire bytes since the cursor
	// rides every paginated response. Hash is serialised as decimal string to dodge the
	// FJsonWriter "treat number as double" precision loss for >2^53.
	const FString HashStr = LexToString(Cursor.FilterHash);

	FString Json;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("h"), HashStr);
	Writer->WriteValue(TEXT("p"), Cursor.LastAssetPath);
	Writer->WriteValue(TEXT("t"), Cursor.TotalKnownSnapshot);
	Writer->WriteObjectEnd();
	Writer->Close();

	// Base64-encode the JSON bytes so the cursor is URL-safe & opaque to callers.
	const FTCHARToUTF8 Utf8(*Json);
	return FBase64::Encode(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
}

bool Decode(const FString& TokenWire, FMCPPageCursor& OutCursor, FString& OutError)
{
	OutCursor = FMCPPageCursor();
	OutError.Reset();

	if (TokenWire.IsEmpty())
	{
		// First-page request — return empty cursor + success.
		return true;
	}

	// Base64 decode.
	TArray<uint8> JsonBytes;
	if (!FBase64::Decode(TokenWire, JsonBytes))
	{
		OutError = TEXT("page_token: base64 decode failed");
		return false;
	}
	// Convert UTF-8 → TCHAR via the explicit-length converter (input may be unterminated).
	const FUTF8ToTCHAR Conv(
		reinterpret_cast<const ANSICHAR*>(JsonBytes.GetData()), JsonBytes.Num());
	const FString JsonStr = FString::ConstructFromPtrSize(Conv.Get(), Conv.Length());

	TSharedPtr<FJsonObject> Obj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonStr);
	if (!FJsonSerializer::Deserialize(Reader, Obj) || !Obj.IsValid())
	{
		OutError = TEXT("page_token: JSON parse failed");
		return false;
	}

	// h: filter_hash as decimal string. Falls back to direct uint64 read if the encoder ever
	// switches to native int form.
	FString HashStr;
	if (Obj->TryGetStringField(TEXT("h"), HashStr))
	{
		// LexFromString sets to 0 on parse failure — we accept that as "0 hash" (matches the
		// IsEmpty() definition; degenerate but bounded).
		LexFromString(OutCursor.FilterHash, *HashStr);
	}
	else
	{
		// Number form fallback.
		double NumD = 0.0;
		if (Obj->TryGetNumberField(TEXT("h"), NumD))
		{
			OutCursor.FilterHash = static_cast<uint64>(NumD);
		}
	}

	Obj->TryGetStringField(TEXT("p"), OutCursor.LastAssetPath);

	int32 Total = 0;
	Obj->TryGetNumberField(TEXT("t"), Total);
	OutCursor.TotalKnownSnapshot = Total;

	return true;
}

bool ValidateAgainstFilter(const FMCPPageCursor& Cursor, uint64 ExpectedFilterHash)
{
	if (Cursor.IsEmpty())
	{
		// First-page request always valid.
		return true;
	}
	return Cursor.FilterHash == ExpectedFilterHash;
}

} // namespace FMCPPageCursorUtils
