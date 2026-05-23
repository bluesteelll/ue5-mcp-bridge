// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Shared class-identifier → UClass* resolver, extracted in Wave Q2.
 *
 * Replaces 4 per-surface helpers (~140 LOC of duplication across 11 call sites):
 *   - AIBT_ResolveSubclassOf   (AIBehaviorTreeTools.cpp — 4 sites)
 *   - AIEQS_ResolveSubclassOf  (AIEQSTools.cpp           — 2 sites)
 *   - BPG_ResolveClass         (BlueprintGraphTools.cpp  — 1 site)
 *   - INP_ResolveSubclass      (InputTools.cpp           — 4 sites, short-name support)
 *
 * **Three modes** covered by one ``Resolve()`` + options struct:
 *
 *   1. **Strict path** (AIBT/AIEQS default):  ``/Script/Module.Class`` only, ``_C`` fallback,
 *      abstract+deprecated rejected, ``IsChildOf(BaseClass)`` enforced. Use when the caller
 *      expects an instantiable concrete subclass (Behavior Tree node, EQS generator/test).
 *
 *   2. **Lenient path** (BPG): same as strict but accepts abstract classes. Use when the
 *      caller only needs a class *reference* (e.g. ``UK2Node_DynamicCast::TargetType`` —
 *      a cast pin can target an abstract base like ``AActor``).
 *
 *   3. **Short name** (INP): accepts non-path identifiers like ``"Hold"`` / ``"DeadZone"`` by
 *      iterating ``TObjectIterator<UClass>`` and matching on DisplayName / stripped Name /
 *      bare Name. Used for trigger/modifier discovery where AI callers prefer human-readable
 *      names over full ``/Script/EnhancedInput.InputTriggerHold`` paths.
 *
 * **Thread-safety:** safe to call from Lane B worker threads — uses only ``LoadClass`` /
 * ``LoadObject`` / ``TObjectIterator`` / ``UClass::HasMetaData`` which are documented thread-safe
 * for read access. The class objects themselves are immortal (root-set) so the returned pointer
 * is stable for the program lifetime.
 *
 * **Path validation:** when ``Options.bRequirePathPrefix`` is true (default), a leading ``'/'``
 * is required and backslashes are rejected — same defensive shape as the original AIBT/AIEQS
 * helpers. Short-name mode (``bAllowShortName=true``) disables this check.
 */
struct UNREALMCPBRIDGECORE_API FMCPClassResolveOptions
{
	/** Optional descent filter. Null = no IsChildOf check. */
	UClass* BaseClass = nullptr;

	/** Retry ``LoadClass`` with ``_C`` suffix for Blueprint-generated classes (default true). */
	bool bTryClassSuffix = true;

	/** Reject ``CLASS_Abstract``. Set false for type-only references (e.g. cast TargetType). */
	bool bRejectAbstract = true;

	/** Reject ``CLASS_Deprecated`` AND ``CLASS_NewerVersionExists``. */
	bool bRejectDeprecated = true;

	/**
	 * Require Identifier to start with ``'/'`` and reject backslashes. Defensive validation
	 * shared by AIBT/AIEQS — protects against typos like Windows-style paths.
	 * Automatically disabled when ``bAllowShortName=true``.
	 */
	bool bRequirePathPrefix = true;

	/**
	 * Accept short-name identifiers (no leading ``'/'``). Triggers a ``TObjectIterator<UClass>``
	 * scan filtered by ``BaseClass``; matches against (in priority): MetaData ``DisplayName`` →
	 * stripped name → bare class name. Case-insensitive.
	 *
	 * NOTE: short-name resolution requires BaseClass to be set (otherwise the iterator scan
	 * has no descent filter and would be O(N) over EVERY UClass in the process).
	 */
	bool bAllowShortName = false;

	/**
	 * Optional prefix to strip from class name for short-name comparison.
	 * Example: ``StripPrefix=TEXT("InputTrigger")`` makes ``"UInputTriggerHold"`` match
	 * identifier ``"Hold"`` via the stripped form ``"Hold"``.
	 * Ignored unless ``bAllowShortName=true``.
	 */
	const TCHAR* ShortNameStripPrefix = nullptr;

	/**
	 * Skip classes carrying the ``HideDropdown`` meta tag (engine convention for classes
	 * that shouldn't appear in editor dropdowns — e.g. ``UInputTriggerChordBlocker`` which
	 * is auto-spawned by the chord system).
	 * Ignored unless ``bAllowShortName=true`` (path mode lets the caller decide).
	 */
	bool bSkipHideDropdown = true;
};

namespace FMCPClassResolver
{
	/**
	 * Resolve ``Identifier`` to a ``UClass*`` per ``Options``. Returns nullptr on failure with
	 * ``OutError`` populated; never asserts on bad input.
	 *
	 * Behaviour matrix:
	 *
	 *   - Identifier empty                           → "identifier is empty"
	 *   - bRequirePathPrefix && no leading '/'       → "must start with '/'"  (unless short-name mode)
	 *   - Path starts with '/' → LoadClass (+ _C fallback if bTryClassSuffix) → fail if null
	 *   - Path resolved → IsChildOf(BaseClass) check → fail if not
	 *   - Path resolved → abstract/deprecated checks per options → fail if rejected
	 *   - bAllowShortName && non-path → iterate derived classes (BaseClass required), match
	 *     on DisplayName → StrippedName → Name (case-insensitive)
	 *   - No match found → "not found; use full path or short name" diagnostic
	 *
	 * Caller pattern:
	 *     FString Err;
	 *     UClass* Cls = FMCPClassResolver::Resolve(Identifier,
	 *         {.BaseClass = UMyBase::StaticClass()}, Err);
	 *     if (!Cls) return MakeError(kMCPErrorClassNotFound, Err);
	 */
	UNREALMCPBRIDGECORE_API UClass* Resolve(
		const FString& Identifier,
		const FMCPClassResolveOptions& Options,
		FString& OutError);

	/**
	 * Strict convenience overload — path-only, ``_C`` fallback, abstract+deprecated rejected,
	 * subclass check, forward-slash validation. Matches the prior AIBT/AIEQS_ResolveSubclassOf
	 * behaviour exactly. Use for "give me an instantiable concrete subclass".
	 */
	UNREALMCPBRIDGECORE_API UClass* ResolveStrict(
		const FString& Path,
		UClass* BaseClass,
		FString& OutError);

	/**
	 * Lenient convenience overload — path-only, ``_C`` fallback, subclass check, but accepts
	 * abstract classes (cast TargetType / generic ClassRef pins can target ``AActor`` etc.).
	 * Also drops the forward-slash validation requirement. Matches prior BPG_ResolveClass behaviour.
	 */
	UNREALMCPBRIDGECORE_API UClass* ResolveLenient(
		const FString& Path,
		UClass* BaseClass,
		FString& OutError);
}
