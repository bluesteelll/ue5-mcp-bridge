// Copyright FatumGame. All Rights Reserved.

#include "MCPClassResolver.h"

#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectIterator.h"

namespace FMCPClassResolver
{
	/** Strip a known prefix from a class name (case-insensitive). Returns Name unchanged if no prefix matches. */
	static FString StripPrefix(const FString& Name, const TCHAR* PrefixToStrip)
	{
		if (!PrefixToStrip || !*PrefixToStrip) { return Name; }
		FString Stripped = Name;
		Stripped.RemoveFromStart(PrefixToStrip, ESearchCase::IgnoreCase);
		return Stripped;
	}

	UClass* Resolve(const FString& Identifier, const FMCPClassResolveOptions& Options, FString& OutError)
	{
		// 1. Empty check (shared across all modes).
		if (Identifier.IsEmpty())
		{
			OutError = TEXT("identifier is empty");
			return nullptr;
		}

		// 2. Path detection — leading '/' marks a class path.
		const bool bLooksLikePath = Identifier.StartsWith(TEXT("/"));

		if (bLooksLikePath)
		{
			// 2a. Defensive validation (paths must use forward slashes; reject backslash typos).
			if (Options.bRequirePathPrefix && Identifier.Contains(TEXT("\\")))
			{
				OutError = FString::Printf(
					TEXT("class path '%s' must use forward slashes only (no backslashes)"), *Identifier);
				return nullptr;
			}

			// 2b. LoadClass + optional _C fallback for Blueprint-generated classes.
			UClass* Resolved = LoadClass<UObject>(nullptr, *Identifier);
			if (!Resolved && Options.bTryClassSuffix)
			{
				const FString WithC = Identifier.EndsWith(TEXT("_C"))
					? Identifier
					: (Identifier + TEXT("_C"));
				Resolved = LoadClass<UObject>(nullptr, *WithC);
			}
			// LoadObject<UClass> fallback (some paths reach the UClass UObject but not via LoadClass<>).
			if (!Resolved)
			{
				Resolved = LoadObject<UClass>(nullptr, *Identifier);
			}
			if (!Resolved && Options.bTryClassSuffix)
			{
				const FString WithC = Identifier.EndsWith(TEXT("_C"))
					? Identifier
					: (Identifier + TEXT("_C"));
				Resolved = LoadObject<UClass>(nullptr, *WithC);
			}

			if (!Resolved)
			{
				OutError = FString::Printf(
					TEXT("class '%s' could not be loaded%s"),
					*Identifier,
					Options.bTryClassSuffix ? TEXT(" (also tried _C suffix)") : TEXT(""));
				return nullptr;
			}

			// 2c. Subclass descent check (when BaseClass provided).
			if (Options.BaseClass && !Resolved->IsChildOf(Options.BaseClass))
			{
				const UClass* Super = Resolved->GetSuperClass();
				OutError = FString::Printf(
					TEXT("class '%s' is not a subclass of '%s' (got family '%s')"),
					*Resolved->GetPathName(),
					*Options.BaseClass->GetPathName(),
					Super ? *Super->GetPathName() : TEXT("?"));
				return nullptr;
			}

			// 2d. Abstract/deprecated rejection (optional).
			if (Options.bRejectAbstract && Resolved->HasAnyClassFlags(CLASS_Abstract))
			{
				OutError = FString::Printf(
					TEXT("class '%s' is abstract — cannot instantiate"), *Resolved->GetPathName());
				return nullptr;
			}
			if (Options.bRejectDeprecated &&
				Resolved->HasAnyClassFlags(CLASS_Deprecated | CLASS_NewerVersionExists))
			{
				OutError = FString::Printf(
					TEXT("class '%s' is deprecated"), *Resolved->GetPathName());
				return nullptr;
			}

			return Resolved;
		}

		// 3. Short-name mode (only when bAllowShortName=true; BaseClass must be set).
		if (Options.bAllowShortName)
		{
			if (!Options.BaseClass)
			{
				OutError = TEXT("short-name resolution requires BaseClass to be set (iterator scan needs a descent filter)");
				return nullptr;
			}

			for (TObjectIterator<UClass> It; It; ++It)
			{
				UClass* C = *It;
				if (!C || C == Options.BaseClass || !C->IsChildOf(Options.BaseClass)) { continue; }
				if (Options.bRejectAbstract && C->HasAnyClassFlags(CLASS_Abstract)) { continue; }
				if (Options.bRejectDeprecated &&
					C->HasAnyClassFlags(CLASS_Deprecated | CLASS_NewerVersionExists)) { continue; }
				if (Options.bSkipHideDropdown && C->HasMetaData(TEXT("HideDropdown"))) { continue; }

				const FString DisplayName = C->GetMetaData(TEXT("DisplayName"));
				const FString Name        = C->GetName();
				const FString Stripped    = StripPrefix(Name, Options.ShortNameStripPrefix);

				if (Identifier.Equals(DisplayName, ESearchCase::IgnoreCase) ||
					Identifier.Equals(Stripped,    ESearchCase::IgnoreCase) ||
					Identifier.Equals(Name,        ESearchCase::IgnoreCase))
				{
					return C;
				}
			}

			OutError = FString::Printf(
				TEXT("class '%s' not found; use full path '/Script/Module.ClassName' or short name (DisplayName / stripped class name)"),
				*Identifier);
			return nullptr;
		}

		// 4. Path-required mode (default) — identifier didn't start with '/'.
		OutError = FString::Printf(
			TEXT("class path '%s' must start with '/' (use '/Script/Module.ClassName' for native or '/Game/Path/BP_X' for Blueprint)"),
			*Identifier);
		return nullptr;
	}

	UClass* ResolveStrict(const FString& Path, UClass* BaseClass, FString& OutError)
	{
		FMCPClassResolveOptions Opts;
		Opts.BaseClass = BaseClass;
		// All other defaults: bTryClassSuffix=true, bRejectAbstract=true, bRejectDeprecated=true,
		// bRequirePathPrefix=true. Matches AIBT/AIEQS_ResolveSubclassOf exactly.
		return Resolve(Path, Opts, OutError);
	}

	UClass* ResolveLenient(const FString& Path, UClass* BaseClass, FString& OutError)
	{
		FMCPClassResolveOptions Opts;
		Opts.BaseClass            = BaseClass;
		Opts.bRejectAbstract      = false;  // BPG accepted abstract for cast pins / generic class refs
		Opts.bRequirePathPrefix   = false;  // BPG didn't validate leading '/' (legacy tolerance)
		return Resolve(Path, Opts, OutError);
	}
}
