// Copyright FatumGame. All Rights Reserved.

#include "MCPARFilterParser.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Hash/CityHash.h"
#include "UObject/TopLevelAssetPath.h"

namespace
{
	/**
	 * Pull a string array out of a JSON object field. Missing field → success with empty array.
	 * Wrong-type field → failure with descriptive error. Caller decides what to do with the
	 * extracted strings (FName / FTopLevelAssetPath / FSoftObjectPath).
	 */
	bool TryGetStringArray(const TSharedPtr<FJsonObject>& Json, const TCHAR* FieldName,
		TArray<FString>& OutStrings, FString& OutError)
	{
		OutStrings.Reset();
		if (!Json.IsValid())
		{
			return true;
		}
		const TArray<TSharedPtr<FJsonValue>>* JsonArrayPtr = nullptr;
		if (!Json->TryGetArrayField(FieldName, JsonArrayPtr))
		{
			// Field absent — treat as empty array.
			return true;
		}
		for (const TSharedPtr<FJsonValue>& Value : *JsonArrayPtr)
		{
			FString Item;
			if (!Value.IsValid() || !Value->TryGetString(Item))
			{
				OutError = FString::Printf(TEXT("filter.%s: expected array of strings, got mixed types"), FieldName);
				return false;
			}
			OutStrings.Add(MoveTemp(Item));
		}
		return true;
	}

	/**
	 * Sort an FName array lex-by-string-case-insensitive. Used for canonicalisation prior to
	 * hashing so reordered filter arrays produce identical hashes.
	 */
	void SortNamesCanonical(TArray<FName>& Names)
	{
		Names.Sort([](const FName& A, const FName& B)
		{
			return A.ToString().Compare(B.ToString(), ESearchCase::IgnoreCase) < 0;
		});
	}

	void SortStringsCanonical(TArray<FString>& Strings)
	{
		Strings.Sort([](const FString& A, const FString& B)
		{
			return A.Compare(B, ESearchCase::IgnoreCase) < 0;
		});
	}

	void SortTopLevelAssetPathsCanonical(TArray<FTopLevelAssetPath>& Paths)
	{
		Paths.Sort([](const FTopLevelAssetPath& A, const FTopLevelAssetPath& B)
		{
			return A.ToString().Compare(B.ToString(), ESearchCase::IgnoreCase) < 0;
		});
	}
}

namespace FMCPARFilterParser
{

bool Parse(const TSharedPtr<FJsonObject>& InJson, FARFilter& OutFilter, FString& OutError)
{
	// Default-constructed FARFilter matches everything — that's the well-defined "no filter"
	// behaviour, so an absent/empty input is valid.
	OutFilter = FARFilter();
	OutError.Reset();

	if (!InJson.IsValid())
	{
		return true;
	}

	// --- package_paths: TArray<FName> --------------------------------------------------------
	TArray<FString> Strs;
	if (!TryGetStringArray(InJson, TEXT("package_paths"), Strs, OutError)) { return false; }
	OutFilter.PackagePaths.Reserve(Strs.Num());
	for (const FString& S : Strs) { OutFilter.PackagePaths.Add(FName(*S)); }

	// --- package_names: TArray<FName> --------------------------------------------------------
	if (!TryGetStringArray(InJson, TEXT("package_names"), Strs, OutError)) { return false; }
	OutFilter.PackageNames.Reserve(Strs.Num());
	for (const FString& S : Strs) { OutFilter.PackageNames.Add(FName(*S)); }

	// --- object_paths: TArray<FSoftObjectPath> ------------------------------------------------
	if (!TryGetStringArray(InJson, TEXT("object_paths"), Strs, OutError)) { return false; }
	OutFilter.SoftObjectPaths.Reserve(Strs.Num());
	for (const FString& S : Strs) { OutFilter.SoftObjectPaths.Emplace(FSoftObjectPath(S)); }

	// --- class_paths: TArray<FTopLevelAssetPath> ----------------------------------------------
	if (!TryGetStringArray(InJson, TEXT("class_paths"), Strs, OutError)) { return false; }
	OutFilter.ClassPaths.Reserve(Strs.Num());
	for (const FString& S : Strs)
	{
		// FTopLevelAssetPath wants "/Script/Engine.StaticMesh" form. We accept either that or
		// "/Script/Engine/StaticMesh" by replacing the final '/' with '.' if no dot present.
		FString Normalised = S;
		if (!Normalised.Contains(TEXT("."), ESearchCase::CaseSensitive))
		{
			int32 LastSlash = INDEX_NONE;
			if (Normalised.FindLastChar(TEXT('/'), LastSlash) && LastSlash >= 0)
			{
				Normalised[LastSlash] = TEXT('.');
			}
		}
		FTopLevelAssetPath Top;
		if (!Top.TrySetPath(Normalised))
		{
			OutError = FString::Printf(TEXT("filter.class_paths: invalid class path '%s'"), *S);
			return false;
		}
		OutFilter.ClassPaths.Add(Top);
	}

	// --- recursive_paths / recursive_classes / include_only_on_disk_assets --------------------
	InJson->TryGetBoolField(TEXT("recursive_paths"), OutFilter.bRecursivePaths);
	InJson->TryGetBoolField(TEXT("recursive_classes"), OutFilter.bRecursiveClasses);
	InJson->TryGetBoolField(TEXT("include_only_on_disk_assets"), OutFilter.bIncludeOnlyOnDiskAssets);

	// --- tags_and_values: { key: value } → TMultiMap<FName, TOptional<FString>> ---------------
	const TSharedPtr<FJsonObject>* TagsObjPtr = nullptr;
	if (InJson->TryGetObjectField(TEXT("tags_and_values"), TagsObjPtr) && TagsObjPtr->IsValid())
	{
		for (const auto& Pair : (*TagsObjPtr)->Values)
		{
			FString Val;
			if (Pair.Value.IsValid() && Pair.Value->TryGetString(Val))
			{
				// Empty string = "any value" sentinel — encoded as TOptional unset to match
				// IAR.GetAssetsByTags behaviour. Non-empty → exact match.
				TOptional<FString> OptVal;
				if (!Val.IsEmpty())
				{
					OptVal = Val;
				}
				OutFilter.TagsAndValues.Add(FName(*Pair.Key), OptVal);
			}
			else
			{
				OutError = FString::Printf(TEXT("filter.tags_and_values['%s']: expected string value"), *Pair.Key);
				return false;
			}
		}
	}

	return true;
}

FString BuildCanonicalString(const FARFilter& Filter)
{
	// Copy & sort each array so the string form is order-independent.
	TArray<FName> PackagePathsSorted   = Filter.PackagePaths;   SortNamesCanonical(PackagePathsSorted);
	TArray<FName> PackageNamesSorted   = Filter.PackageNames;   SortNamesCanonical(PackageNamesSorted);
	TArray<FTopLevelAssetPath> ClassPathsSorted = Filter.ClassPaths;
	SortTopLevelAssetPathsCanonical(ClassPathsSorted);

	// SoftObjectPaths → flatten to strings then sort. Same canonicalisation contract.
	TArray<FString> ObjectPathsSorted;
	ObjectPathsSorted.Reserve(Filter.SoftObjectPaths.Num());
	for (const FSoftObjectPath& Soft : Filter.SoftObjectPaths)
	{
		ObjectPathsSorted.Add(Soft.ToString());
	}
	SortStringsCanonical(ObjectPathsSorted);

	// TagsAndValues canonicalisation — sort by FName lex case-insensitive, then serialize as
	// [[key,value], ...] pairs. NULL/missing values encode as empty string (the wire-shape
	// "any value" sentinel). Per critic advisory #1, this is the ONLY tags-canonicalisation
	// contract — DO NOT add value-side sorting (a single key may appear with multiple values via
	// TMultiMap; we preserve insertion order within a key by sorting ALL pairs lex on
	// concatenated "key=value").
	TArray<FString> TagPairsSorted;
	TagPairsSorted.Reserve(Filter.TagsAndValues.Num());
	for (const auto& Pair : Filter.TagsAndValues)
	{
		const FString KeyStr = Pair.Key.ToString();
		const FString ValStr = Pair.Value.IsSet() ? Pair.Value.GetValue() : FString();
		TagPairsSorted.Add(KeyStr + TEXT("=") + ValStr);
	}
	SortStringsCanonical(TagPairsSorted);

	TStringBuilder<256> Sb;
	Sb << TEXT("recursive_paths=") << (Filter.bRecursivePaths ? TEXT("1") : TEXT("0"));
	Sb << TEXT(";recursive_classes=") << (Filter.bRecursiveClasses ? TEXT("1") : TEXT("0"));
	Sb << TEXT(";include_only_on_disk_assets=") << (Filter.bIncludeOnlyOnDiskAssets ? TEXT("1") : TEXT("0"));

	Sb << TEXT(";package_paths=[");
	for (int32 i = 0; i < PackagePathsSorted.Num(); ++i)
	{
		if (i) Sb << TEXT(",");
		Sb << *PackagePathsSorted[i].ToString();
	}
	Sb << TEXT("]");

	Sb << TEXT(";package_names=[");
	for (int32 i = 0; i < PackageNamesSorted.Num(); ++i)
	{
		if (i) Sb << TEXT(",");
		Sb << *PackageNamesSorted[i].ToString();
	}
	Sb << TEXT("]");

	Sb << TEXT(";object_paths=[");
	for (int32 i = 0; i < ObjectPathsSorted.Num(); ++i)
	{
		if (i) Sb << TEXT(",");
		Sb << *ObjectPathsSorted[i];
	}
	Sb << TEXT("]");

	Sb << TEXT(";class_paths=[");
	for (int32 i = 0; i < ClassPathsSorted.Num(); ++i)
	{
		if (i) Sb << TEXT(",");
		Sb << *ClassPathsSorted[i].ToString();
	}
	Sb << TEXT("]");

	Sb << TEXT(";tags=[");
	for (int32 i = 0; i < TagPairsSorted.Num(); ++i)
	{
		if (i) Sb << TEXT(",");
		Sb << *TagPairsSorted[i];
	}
	Sb << TEXT("]");

	return FString(Sb);
}

uint64 ComputeFilterHash(const FARFilter& Filter)
{
	const FString Canonical = BuildCanonicalString(Filter);
	// Convert to UTF-8 byte view for a stable hash (CityHash64 operates on raw bytes; using
	// the TCHAR view would tie the hash to platform char width).
	const FTCHARToUTF8 Utf8(*Canonical);
	return CityHash64(reinterpret_cast<const char*>(Utf8.Get()), Utf8.Length());
}

} // namespace FMCPARFilterParser
