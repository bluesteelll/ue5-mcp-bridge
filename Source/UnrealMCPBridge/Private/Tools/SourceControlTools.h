// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Phase 6 — Chunk A (Source Control). 5 user-visible synchronous tools, all Lane A.
 *
 * Tool roster (per Phase 6 plan §Category-A):
 *   sc.status        → list per-file SC states (revision, action, last-modified-by, etc.)
 *   sc.checkout      → check out files for edit (acquire write lock on locking providers)
 *   sc.diff          → unified diff text for text files; base64 chunks for binary
 *   sc.diff_binary   → forced binary-mode variant of sc.diff (skips text-detection)
 *   sc.revert        → discard local changes (destructive — gated by confirm_destructive=true)
 *
 * The 6th tool ``sc.submit`` is an ASYNC composite — its internal Lane-B submitter lives in
 * ``Tools/SourceControlCompositeTools.{h,cpp}`` and its Python wrapper in
 * ``Content/Python/MCPTools/tools/phase6_composites.py``.
 *
 * **All 5 sync tools are Lane A** (``bThreadSafe=false``). Reasons:
 *   - ``ISourceControlProvider`` state mutates (per-file state cache); Lane B would race the
 *     cache during ``Provider.Execute`` calls.
 *   - ``Execute(EConcurrency::Synchronous)`` blocks the calling thread for the provider RPC
 *     (Perforce/Git workspace command). On Git LFS this is typically a fork+exec of `git`,
 *     ~50-500ms; on Perforce, network round-trip ~100ms-2s. Game-thread block is acceptable
 *     because (a) per-call cost is bounded, (b) Lane B can't touch UObject state and we're
 *     using the provider's caching API.
 *   - Per Phase 6 plan §D10: "All `sc.*` reads Lane A initially (SC provider state is mutable;
 *     Phase 7 may revisit `sc.status` for Lane B)."
 *
 * **Provider-unavailable surface.** Every tool first checks
 * ``ISourceControlModule::Get().IsEnabled()`` and ``GetProvider().IsAvailable()`` — failure
 * returns -32045 SourceControlProviderUnavailable with a recovery hint pointing at editor SC
 * settings. The provider name (Git, Perforce, etc.) is echoed in error messages so callers
 * can debug provider-specific failures.
 *
 * **Path translation.** Caller passes file paths in either form:
 *   - Absolute disk path: ``C:/Project/Content/foo.uasset`` (sandboxed via FMCPPathSandbox)
 *   - UE package path: ``/Game/Foo/Bar`` → translated via FPackageName::TryConvertLongPackageNameToFilename
 *
 * SC providers want absolute disk paths; we normalise package paths to disk paths before
 * passing to ``Provider.Execute``. Sandbox enforcement is identical to Phase 2 cb.* tools
 * (project dir / saved / intermediate / engine — anything else → -32013 PathEscape).
 *
 * **Binary diff cap.** ``sc.diff`` (auto-detect) and ``sc.diff_binary`` (forced) cap binary
 * payloads at 32 MiB per side. Larger → -32017 InputTooLarge. This prevents accidental
 * embedding of 1+ GiB cooked assets in JSON responses; AI clients can fall back to
 * file-handle export via Phase 2 ``cb.export`` for large binaries.
 *
 * **Revert is destructive.** ``sc.revert`` requires ``confirm_destructive=true`` (D2-style
 * gate matching ``bp.reparent confirm_dangerous`` and ``cb.delete force``). Missing flag →
 * -32033 ReparentUnsafe (reused — "destructive operation requires confirmation").
 *
 * **PIE behaviour.** No PIE guard — SC operations are workspace-level and orthogonal to PIE
 * state. Even during PIE the editor can checkout/revert/submit files. (PIE shouldn't be
 * running while submitting changes is a workflow concern, not a correctness one.)
 *
 * **Build.cs.** Adds ``SourceControl`` private dep — every header used here lives in that
 * module's Public/ folder (ISourceControlModule, ISourceControlProvider, SourceControlOperations,
 * ISourceControlState, ISourceControlRevision).
 */
namespace FSourceControlTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	// ─── Category A: Source Control sync tools ──────────────────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_Status(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_Checkout(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_Diff(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_DiffBinary(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_Revert(const FMCPRequest& Request);
}
