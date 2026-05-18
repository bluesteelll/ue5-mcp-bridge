// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/ARFilter.h"

class FJsonObject;

/**
 * JSON ↔ ``FARFilter`` adapter + canonical-form hashing.
 *
 * **Wire shape** (D13 — inlined in ``asset.list`` and ``asset.search_by_class`` schemas in
 * Phase 2; promoted to a shared ``$ref`` component in Phase 5):
 *
 * ```jsonc
 * {
 *   "package_paths":              ["/Game/Foo", "/Game/Bar"],
 *   "package_names":              ["/Game/Foo/Asset1"],
 *   "object_paths":               ["/Game/Foo/Asset1.Asset1"],
 *   "class_paths":                ["/Script/Engine.StaticMesh"],
 *   "recursive_paths":            false,
 *   "recursive_classes":          false,
 *   "tags_and_values":            { "TagKey": "TagValue", "OtherTag": "" },
 *   "include_only_on_disk_assets": false
 * }
 * ```
 *
 * Empty / missing fields map to a default ``FARFilter`` field (empty array / false bool).
 *
 * **Canonicalisation** (D2): before hashing, every array field is sorted lexicographically
 * (case-insensitive for FNames), TagsAndValues entries are flattened to a sorted
 * ``[[key, value], ...]`` pair list. Two filters that differ only in array element order
 * produce the same ``FilterHash`` and thus share pagination tokens.
 *
 * The canonical-form string is NOT serialised back to wire — it's only consumed by
 * ``CityHash64`` internally inside ``ComputeFilterHash``.
 */
namespace FMCPARFilterParser
{
	/**
	 * Convert a JSON object (per the schema above) into ``FARFilter``. Returns true on success;
	 * on failure leaves ``OutFilter`` in a partially-built state and populates ``OutError``.
	 *
	 * Validation failures: non-array where array expected, non-object where object expected.
	 * Empty-input is valid — produces a default ``FARFilter`` (matches every asset).
	 */
	UNREALMCPBRIDGE_API bool Parse(const TSharedPtr<FJsonObject>& InJson,
		FARFilter& OutFilter, FString& OutError);

	/**
	 * Produce a 64-bit canonical hash of the filter, suitable for stamping into pagination
	 * cursors so the server can detect mid-pagination filter mutation. See header docstring for
	 * the canonicalisation rules.
	 *
	 * Stable across process boundaries (CityHash64 is deterministic).
	 */
	UNREALMCPBRIDGE_API uint64 ComputeFilterHash(const FARFilter& Filter);

	/**
	 * Build the canonical-form string for debugging / unit-test purposes. Same input → same
	 * output; round-trip not guaranteed (the canonical form is lossy — it discards the
	 * recursive_paths / recursive_classes booleans into the hash via prefix tokens). Not on the
	 * Phase 2 dispatch hot path.
	 */
	UNREALMCPBRIDGE_API FString BuildCanonicalString(const FARFilter& Filter);
}
