// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Opaque pagination cursor for ``asset.list`` / ``asset.search_by_*`` / ``asset.find_references``
 * / ``asset.find_dependents``.
 *
 * **Wire form:** base64-encoded compact JSON ``{ "h": <u64 filter_hash>, "p": "<last asset path>",
 * "t": <i32 total_known_snapshot> }``. Caller treats it as opaque; only the server inspects.
 *
 * **Pagination semantics (D2):** "keyset pagination". On the next call, the server re-queries the
 * underlying AR with the SAME filter, lex sorts the full result by ``ObjectPath`` (the cursor's
 * sort key), then skips entries with key ``<= last_asset_path`` and returns the next ``page_size``.
 *
 * This survives both inserts (new items between pages appear in their slot naturally) and deletes
 * (a deleted sentinel still has a stable comparison key — the search lands at the next-greater
 * surviving item). Filter-hash validation detects callers mutating the filter mid-pagination —
 * the cursor's embedded hash MUST match the current call's computed hash or the server returns
 * ``STALE_CURSOR`` (kMCPErrorStaleCursor = -32015).
 */
struct FMCPPageCursor
{
	/** Hash of the canonical filter form at issue time. */
	uint64 FilterHash = 0;

	/** Last item's stable sort key — the ObjectPath string. Empty for first page. */
	FString LastAssetPath;

	/** Best-effort snapshot of total_known at issue time; advisory only. */
	int32 TotalKnownSnapshot = 0;

	/** Returns true if no useful state (i.e. first-page request). */
	bool IsEmpty() const
	{
		return LastAssetPath.IsEmpty() && FilterHash == 0 && TotalKnownSnapshot == 0;
	}
};

/**
 * Opaque pagination cursor encode/decode + validation.
 *
 * Lane B-safe (no UObject access).
 */
namespace FMCPPageCursorUtils
{
	/**
	 * base64( compact JSON ). Empty input cursor → empty string (i.e. don't emit a token for the
	 * first page).
	 */
	UNREALMCPBRIDGE_API FString Encode(const FMCPPageCursor& Cursor);

	/**
	 * Decode a base64-wrapped JSON cursor. Returns true on success; on failure returns false and
	 * populates ``OutError`` with a human-readable reason. Empty input string → success with
	 * empty cursor (caller is asking for first page).
	 */
	UNREALMCPBRIDGE_API bool Decode(const FString& TokenWire, FMCPPageCursor& OutCursor, FString& OutError);

	/**
	 * Check the decoded cursor matches the expected filter hash. Returns true if compatible.
	 * Empty cursor (first page) always matches.
	 */
	UNREALMCPBRIDGE_API bool ValidateAgainstFilter(const FMCPPageCursor& Cursor, uint64 ExpectedFilterHash);
}
