// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Wave I Surface 2 — Soft-reference resolution + redirector maintenance surface. 4 user-visible
 * tools, all Lane A.
 *
 * Tool roster:
 *   soft_ref.validate           — well-formedness + existence check on a single FSoftObjectPath.
 *                                  Returns { valid_syntax, target_exists, target_class? }. Uses
 *                                  IAssetRegistry::GetAssetByObjectPath (single-point query, no
 *                                  AR enumeration). Read-only, no PIE guard.
 *   soft_ref.resolve            — resolve a soft path to its hard object path; when ``force_load=true``,
 *                                  calls FSoftObjectPath::TryLoad so the target is brought into memory
 *                                  if not already loaded. Returns { resolved_path?, was_loaded,
 *                                  target_class? }. Read-only on the AR; force_load DOES touch UObject
 *                                  loading, but no asset MUTATION occurs. No PIE guard.
 *   soft_ref.find_redirectors   — paginated UObjectRedirector enumeration via IAssetRegistry::GetAssets.
 *                                  Optional ``path_prefix`` narrows the AR scan; default = recursive
 *                                  across all mounts (matches cb.fix_redirectors semantics on the
 *                                  filter side). Per-entry { redirector_path, target_path }. The
 *                                  ``target_path`` is read by loading the UObjectRedirector and
 *                                  inspecting DestinationObject — UE has no AR tag for redirector
 *                                  target. Standard FMCPPageCursor over ObjectPath. Read-only, no PIE
 *                                  guard.
 *   soft_ref.fix_redirectors    — IAssetTools::FixupReferencers driven by either an explicit list
 *                                  (``redirector_paths``) OR a path_prefix sweep. Caller MUST supply
 *                                  one or the other (both empty → -32602). PIE-guarded mutator;
 *                                  ``dry_run=true`` bypasses the PIE guard and reports what WOULD
 *                                  be fixed without touching anything. Wrapped in FScopedTransaction.
 *                                  Returns { fixed, skipped, errors: [{ path, reason }] }.
 *
 * **Read tools (validate/resolve/find_redirectors) bypass PIE guard.** ``fix_redirectors`` refuses
 * during PIE with -32027 + frozen kMCPMessagePIEActive text, UNLESS ``dry_run=true``.
 *
 * **Soft-path syntax contract.** Accepts ANY string convertible to FSoftObjectPath — the engine's
 * own parser handles object-path-with-class (``/Game/Foo/Bar.Bar``), package-name-leaf-implicit
 * (``/Game/Foo/Bar``), sub-objects (``/Game/Foo/Bar.Bar:Sub``), and even the legacy
 * ``"ClassName'/Game/Foo.Bar'"`` string form via FSoftObjectPath::operator=. We DO NOT pre-normalise
 * via FMCPAssetPathUtils::Normalize because soft paths legitimately carry the leaf suffix the
 * normaliser would strip; instead we report well-formedness via FSoftObjectPath::IsValid.
 *
 * **Redirector-target resolution.** UObjectRedirector::DestinationObject is the link target. We load
 * each redirector on demand (LoadObject is idempotent — returns the cached UObject if already loaded)
 * to read the pointer. Asset-registry tags do NOT include the redirector destination — only Phase 2's
 * cb.fix_redirectors path also pays this load cost. Caller can mitigate by narrowing ``path_prefix``.
 *
 * **fix_redirectors semantics.** ``FixupReferencers`` walks every package that REFERS to each input
 * redirector, mutates each reference to point at the redirector's DestinationObject, then DELETES
 * the redirector itself (ERedirectFixupMode::DeleteFixedUpRedirectors). Both behaviours mirror Phase
 * 2's cb.fix_redirectors. Errors are per-redirector (load failure, cast failure) and collected into
 * the ``errors`` array — the top-level response is still success unless ALL inputs failed.
 *
 * **Error codes (all reused from existing range — no new codes):**
 *   -32004 ObjectNotFound        soft ref target not found / redirector path doesn't resolve
 *   -32010 InvalidPath           malformed soft_path (FSoftObjectPath::IsValid returned false)
 *   -32027 PIEActive             fix_redirectors editor-world mutator during PIE (dry_run bypasses)
 *   -32602 InvalidParams         missing args, OR fix_redirectors called with both args empty
 *   -32603 InternalError         AR query / asset-tools module load failure
 */
namespace FSoftRefTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	// ─── Wave I Surface 2: Soft-reference tools ──────────────────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_Validate(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_Resolve(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_FindRedirectors(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_FixRedirectors(const FMCPRequest& Request);
}
