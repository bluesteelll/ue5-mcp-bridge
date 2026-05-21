// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"
#include "Utils/MCPAssetPathUtils.h"
#include "UObject/UObjectGlobals.h"

/**
 * Shared path → LoadObject<T> → cast helper. Replaces ~80 inline call sites across 30+ tool
 * surfaces, each of which did this in 20-30 LOC of boilerplate.
 *
 * Usage:
 *     int32 ErrorCode = 0;
 *     FString ErrorMessage;
 *     UDataTable* Table = FMCPAssetLoader::Load<UDataTable>(InPath, ErrorCode, ErrorMessage);
 *     if (!Table) {
 *         return FMCPToolHelpers::MakeError(Request, ErrorCode, ErrorMessage);
 *     }
 *
 * Error codes set on failure:
 *   - kMCPErrorInvalidPath (-32010) — empty path, malformed (backslash / .. / wrong mount)
 *   - kMCPErrorObjectNotFound (-32004) — load returned nullptr after both normalised + object-path forms
 *   - kMCPErrorWrongClass (-32011) — load succeeded but Cast<T> failed (wrong asset class)
 *
 * **Thread-safety:** NOT Lane B safe — calls LoadObject which touches UObject globals. Same constraint
 * as the existing per-surface XX_LoadXxxByPath functions.
 */

namespace FMCPAssetLoader
{
	template<typename T>
	T* Load(const FString& InPath, int32& OutErrorCode, FString& OutErrorMessage)
	{
		OutErrorCode = 0;
		OutErrorMessage.Reset();

		if (InPath.IsEmpty())
		{
			OutErrorCode = kMCPErrorInvalidPath;
			OutErrorMessage = TEXT("path is empty");
			return nullptr;
		}

		const FString Normalised = FMCPAssetPathUtils::Normalize(InPath);
		if (Normalised.IsEmpty() || !FMCPAssetPathUtils::IsValidGameOrPlugin(Normalised))
		{
			OutErrorCode = kMCPErrorInvalidPath;
			OutErrorMessage = FString::Printf(TEXT("path '%s' malformed or unknown mount"), *InPath);
			return nullptr;
		}

		UObject* Loaded = LoadObject<UObject>(nullptr, *Normalised);
		if (!Loaded)
		{
			const FString ObjPath = FMCPAssetPathUtils::ToObjectPath(Normalised);
			if (!ObjPath.IsEmpty() && ObjPath != Normalised)
			{
				Loaded = LoadObject<UObject>(nullptr, *ObjPath);
			}
		}
		if (!Loaded)
		{
			OutErrorCode = kMCPErrorObjectNotFound;
			OutErrorMessage = FString::Printf(TEXT("'%s' not loadable"), *InPath);
			return nullptr;
		}

		T* Typed = Cast<T>(Loaded);
		if (!Typed)
		{
			OutErrorCode = kMCPErrorWrongClass;
			OutErrorMessage = FString::Printf(TEXT("'%s' is '%s'; expected %s"),
				*InPath, *Loaded->GetClass()->GetPathName(), *T::StaticClass()->GetName());
			return nullptr;
		}

		return Typed;
	}

	/**
	 * Non-templated variant that returns the raw UObject* — useful for surfaces that need to check
	 * the loaded class against a runtime UClass* rather than a compile-time type. Sets OutErrorCode
	 * to kMCPErrorInvalidPath / kMCPErrorObjectNotFound, never kMCPErrorWrongClass (caller does
	 * their own class check).
	 */
	inline UObject* LoadRaw(const FString& InPath, int32& OutErrorCode, FString& OutErrorMessage)
	{
		OutErrorCode = 0;
		OutErrorMessage.Reset();

		if (InPath.IsEmpty())
		{
			OutErrorCode = kMCPErrorInvalidPath;
			OutErrorMessage = TEXT("path is empty");
			return nullptr;
		}

		const FString Normalised = FMCPAssetPathUtils::Normalize(InPath);
		if (Normalised.IsEmpty() || !FMCPAssetPathUtils::IsValidGameOrPlugin(Normalised))
		{
			OutErrorCode = kMCPErrorInvalidPath;
			OutErrorMessage = FString::Printf(TEXT("path '%s' malformed or unknown mount"), *InPath);
			return nullptr;
		}

		UObject* Loaded = LoadObject<UObject>(nullptr, *Normalised);
		if (!Loaded)
		{
			const FString ObjPath = FMCPAssetPathUtils::ToObjectPath(Normalised);
			if (!ObjPath.IsEmpty() && ObjPath != Normalised)
			{
				Loaded = LoadObject<UObject>(nullptr, *ObjPath);
			}
		}
		if (!Loaded)
		{
			OutErrorCode = kMCPErrorObjectNotFound;
			OutErrorMessage = FString::Printf(TEXT("'%s' not loadable"), *InPath);
			return nullptr;
		}

		return Loaded;
	}
}
