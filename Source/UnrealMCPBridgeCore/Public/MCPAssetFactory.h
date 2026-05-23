// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

class UPackage;

/**
 * Shared asset-creation helper, extracted in Wave Q3.
 *
 * Replaces the duplicated CreatePackage + NewObject + AssetRegistry-notify pattern across
 * 6 ``*.create_asset`` tools (~180 LOC of duplication):
 *
 *   - asset.create_data_asset     (AssetCompositeTools.cpp — Wave H, canonical reference)
 *   - input.create_input_action   (InputTools.cpp           — Wave N)
 *   - input.create_mapping_context(InputTools.cpp           — Wave N)
 *   - ai.bt.create_asset          (AIBehaviorTreeTools.cpp  — Wave P)
 *   - ai.bb.create_asset          (AIBlackboardTools.cpp    — Wave P)
 *   - ai.eqs.create_asset         (AIEQSTools.cpp           — Wave P)
 *
 * Each call site previously implemented the same 7-step ritual: path normalize → existence
 * check → CreatePackage + FullyLoad → NewObject<T> + null check → FAssetRegistryModule::
 * AssetCreated → MarkPackageDirty (via outer ``FMCPMutatorScope``) → response. Steps 1-5 are
 * identical; steps 6-7 are caller-owned (scope still belongs to the surface, and any
 * per-type configure happens between NewObject and DirtyPackage).
 *
 * **Contract:**
 *   - Caller owns the outer ``FMCPMutatorScope`` (PIE guard + transaction). This helper does
 *     NOT open one — but DOES require the calling thread to be game thread (``check`` asserts).
 *   - Caller calls ``Scope.DirtyPackage(Result.Package)`` AFTER any per-type configure step.
 *     The helper does NOT mark dirty itself so the caller can configure first and dirty once.
 *   - On failure, ``Result.Asset`` is nullptr and ``Result.ErrorCode`` / ``Result.ErrorMessage``
 *     are populated with stdlib MCP error codes ready to surface via ``MakeError``.
 *
 * **Error codes used:**
 *   -32010 InvalidPath  — path malformed or unknown mount
 *   -32014 PathInUse    — package already exists at the requested path
 *   -32603 Internal     — CreatePackage or NewObject returned null (rare; OOM or sandbox)
 *
 * **Thread-safety:** Lane A only (CreatePackage / NewObject / FAssetRegistryModule notify all
 * require game thread). Callers are already Lane A for asset mutation per project convention.
 */
struct UNREALMCPBRIDGECORE_API FMCPAssetCreateResult
{
	/** Non-null on success; cast to your concrete T (or use the templated Create<T> overload). */
	UObject* Asset = nullptr;

	/** Non-null on success; pass to ``Scope.DirtyPackage()`` after per-type configure. */
	UPackage* Package = nullptr;

	/** Canonical asset path (``/Game/Foo/Bar.Bar``) on success; empty on failure. */
	FString AssetPath;

	/** Stdlib MCP error code on failure; 0 on success. Maps to ``MakeError(Result.ErrorCode, ...)``. */
	int32 ErrorCode = 0;

	/** Human-readable message on failure; empty on success. */
	FString ErrorMessage;

	/** Convenience — true when Asset is non-null. */
	bool IsValid() const { return Asset != nullptr; }
};

namespace FMCPAssetFactory
{
	/**
	 * Create a new asset of ``Class`` at ``InAssetPath``. Returns a result struct — check
	 * ``Result.Asset`` (or ``IsValid()``) before using; on failure surface the error via
	 * ``MakeError(Result.ErrorCode, Result.ErrorMessage)``.
	 *
	 * The created UObject's outer is the newly-created UPackage. RF_Public | RF_Standalone |
	 * RF_Transactional flags are applied (the standard "saveable, transactable" set). Override
	 * via the ``ObjectFlags`` parameter if you need different semantics.
	 *
	 * **Validation order** (first failure wins):
	 *   1. Path normalize + game-or-plugin mount check → InvalidPath
	 *   2. Package existence check (``DoesPackageExist`` + ``FindObject``) → PathInUse
	 *   3. CreatePackage → Internal (very rare; OOM)
	 *   4. NewObject<Class> → Internal (rare; abstract class / sandbox)
	 *
	 * Caller pattern:
	 *
	 *     FMCPAssetCreateResult R = FMCPAssetFactory::Create(Path, UMyAsset::StaticClass());
	 *     if (!R.IsValid())
	 *     {
	 *         return FMCPToolHelpers::MakeError(Request, R.ErrorCode, R.ErrorMessage);
	 *     }
	 *     UMyAsset* MyAsset = Cast<UMyAsset>(R.Asset);
	 *     MyAsset->SomeField = ...;        // per-type configure
	 *     Scope.DirtyPackage(R.Package);   // single dirty after configure complete
	 *     return FMCPJsonBuilder().Str(TEXT("asset_path"), R.AssetPath).BuildSuccess(Request);
	 */
	UNREALMCPBRIDGECORE_API FMCPAssetCreateResult Create(
		const FString& InAssetPath,
		UClass* Class,
		EObjectFlags ObjectFlags = RF_Public | RF_Standalone | RF_Transactional);

	/**
	 * Templated convenience — equivalent to ``Create(InAssetPath, T::StaticClass(), Flags)``.
	 * The returned ``Result.Asset`` is still ``UObject*``; caller does ``Cast<T>(R.Asset)`` if
	 * it needs the concrete pointer (no virtual dispatch tax — Cast is a constexpr-ish check).
	 *
	 * Example:
	 *     FMCPAssetCreateResult R = FMCPAssetFactory::Create<UEnvQuery>(DestPath);
	 *     if (!R.IsValid()) return MakeError(R.ErrorCode, R.ErrorMessage);
	 */
	template<typename T>
	FMCPAssetCreateResult Create(
		const FString& InAssetPath,
		EObjectFlags ObjectFlags = RF_Public | RF_Standalone | RF_Transactional)
	{
		return Create(InAssetPath, T::StaticClass(), ObjectFlags);
	}
}
