// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Wave I Surface 1 — UPackage save / inspect / dependency surface. 5 user-visible tools, all Lane A.
 *
 * Tool roster:
 *   package.save              — Save a single loaded UPackage to disk. PIE-guarded mutator.
 *                               Returns { saved, was_dirty, file_path }. Refuses with -32004
 *                               when the package is not currently loaded (a save of a non-
 *                               loaded package is a no-op by definition; we surface it as a
 *                               hard error rather than silently succeed).
 *   package.save_all          — Save every loaded UPackage matching the filter (default:
 *                               only_dirty=true, max=500). PIE-guarded mutator. Skips
 *                               transient packages and the /Engine/Transient sentinel.
 *                               Returns { saved, failed, failures: [{ package_path, reason }] }.
 *                               Cap is a soft guardrail — caller can raise via max_packages.
 *   package.list_dirty        — Enumerate every loaded dirty UPackage. Read-only, no PIE
 *                               guard. Returns { dirty_packages: [{ path, asset_count,
 *                               transient }], total_dirty }. Useful as a pre-check before
 *                               save_all (lets the caller preview what would be saved).
 *   package.get_dependencies  — IAssetRegistry::GetDependencies wrapper. Read-only, no PIE
 *                               guard. Returns { dependencies: [{ path, dep_type }],
 *                               total }. dep_type is "Hard" | "Soft" | "SearchableName"
 *                               (mirrors asset.find_dependents classification).
 *                               include_hard/include_soft filter the result; recursive
 *                               walks the graph BFS-style with a 10k visited-cap.
 *   package.get_referencers   — Inverse of get_dependencies — who points AT this package.
 *                               Read-only, no PIE guard. Same response shape as
 *                               get_dependencies (dep_type classification preserved). No
 *                               recursive flag (single-hop only — recursive referencers can
 *                               explode combinatorially on common engine assets like default
 *                               materials).
 *
 * **Read tools (list_dirty / get_dependencies / get_referencers) bypass PIE guard.**
 * Mutators (save / save_all) refuse during PIE with -32027 + frozen kMCPMessagePIEActive text.
 *
 * **Save semantics.** ``package.save`` calls UPackage::SavePackage with the package's existing
 * filename (derived via FPackageName::LongPackageNameToFilename). The function does NOT call
 * the file dialog or the engine's interactive prompt — these are commandlet-style saves. The
 * was_dirty flag in the response reports the pre-save state so callers can detect "no-op"
 * saves (was_dirty=false → file rewritten with identical bytes but the operation succeeded).
 *
 * **Transient skip.** ``save_all`` skips packages with RF_Transient flag set OR any package
 * whose name starts with ``/Engine/Transient`` (the canonical anonymous-package mount). PIE
 * play worlds live under ``/Temp/UEDPIE_<instance>_<orig>`` and would be skipped by the
 * PIE guard anyway — we don't need an explicit /Temp/UEDPIE filter.
 *
 * **Dependency classification.** Reuses ``UE::AssetRegistry::EDependencyCategory::Package``
 * + the ``Hard`` / ``NotHard`` query flags. SearchableName dependencies are surfaced when
 * include_searchable_names is not present (default behavior: Package category only).
 *
 * **Error codes (all reused from existing range — no new codes):**
 *   -32004 ObjectNotFound        package not loaded (save) / not in registry (deps)
 *   -32010 InvalidPath           malformed package_path
 *   -32012 OverlyBroadQuery      recursive get_dependencies visited > 10k packages
 *   -32027 PIEActive             editor-world mutator during PIE
 *   -32602 InvalidParams         missing required args
 *   -32603 InternalError         no filename derivable / SavePackage returned false
 */
namespace FPackageTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	// ─── Wave I Surface 1: Package tools ────────────────────────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_Save(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_SaveAll(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_ListDirty(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_GetDependencies(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_GetReferencers(const FMCPRequest& Request);
}
