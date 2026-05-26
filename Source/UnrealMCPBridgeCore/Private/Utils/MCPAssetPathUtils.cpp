// Copyright FatumGame. All Rights Reserved.

#include "Utils/MCPAssetPathUtils.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Misc/PackageName.h"
#include "UObject/SoftObjectPath.h"

namespace
{
	/**
	 * Strip leading/trailing whitespace, drop any trailing '/'. Cheap helper; reused inside
	 * ``Normalize`` for both the input string and any leaf-suffix split.
	 */
	FString TrimAndClean(const FString& In)
	{
		FString Out = In;
		Out.TrimStartAndEndInline();
		// Drop trailing slash only — we deliberately keep the leading slash that marks the
		// mount-point. ``/Game/`` becomes ``/Game`` (a folder query); ``/`` itself is rejected as
		// malformed later because no mount is bare-``/``.
		while (Out.EndsWith(TEXT("/"), ESearchCase::CaseSensitive))
		{
			Out.LeftChopInline(1, EAllowShrinking::No);
		}
		return Out;
	}

	/**
	 * True if ``Path`` starts with a known content mount as returned by
	 * ``FPackageName::QueryRootContentPaths``. Allocates a fresh array on every call (call site
	 * is in editor-time tool dispatch — not hot enough to warrant a static cache; mount-points
	 * can change at runtime via plugin (un)load).
	 */
	bool IsKnownMountPoint(const FString& Path)
	{
		// Special mount: /Script/ is the always-present class-path namespace. FPackageName's mount
		// table tracks content roots, NOT script paths — so we whitelist it explicitly.
		if (Path.StartsWith(TEXT("/Script/"), ESearchCase::CaseSensitive) ||
			Path.Equals(TEXT("/Script"), ESearchCase::CaseSensitive))
		{
			return true;
		}

		// Also allow /Memory/ (in-memory transient packages — used by some editor utilities).
		if (Path.StartsWith(TEXT("/Memory/"), ESearchCase::CaseSensitive) ||
			Path.Equals(TEXT("/Memory"), ESearchCase::CaseSensitive))
		{
			return true;
		}

		TArray<FString> Mounts;
		FPackageName::QueryRootContentPaths(Mounts, /*bIncludeReadOnlyRoots*/ true,
			/*bWithoutLeadingSlashes*/ false, /*bWithoutTrailingSlashes*/ false);
		for (const FString& Mount : Mounts)
		{
			// Mount returned with both leading and trailing slash, e.g. "/Game/". Strip the
			// trailing slash and check both "/Game" exactly AND "/Game/..." prefix to handle the
			// edge case where the caller passes the bare mount point.
			FString MountTrimmed = Mount;
			while (MountTrimmed.EndsWith(TEXT("/"), ESearchCase::CaseSensitive))
			{
				MountTrimmed.LeftChopInline(1, EAllowShrinking::No);
			}
			if (Path.Equals(MountTrimmed, ESearchCase::CaseSensitive))
			{
				return true;
			}
			if (Path.StartsWith(MountTrimmed + TEXT("/"), ESearchCase::CaseSensitive))
			{
				return true;
			}
		}
		return false;
	}
}

namespace FMCPAssetPathUtils
{

FString Normalize(const FString& InPath)
{
	FString Path = TrimAndClean(InPath);
	if (Path.IsEmpty())
	{
		return {};
	}

	// Wave R+1 hardening (2026-05-24): reject control characters and path length overflow.
	// Discovered via stress test T5: paths with embedded NUL bytes (\x00) and 200+ char paths
	// were being accepted by create_blueprint, then passing through to engine asset creation
	// where they'd silently truncate / corrupt asset registry state. Reject EARLY at the
	// validation layer so every surface inherits the defence (~65 surfaces protected by 1 fix).
	for (TCHAR Ch : Path)
	{
		// Block control codes (NUL through US, DEL) — none belong in a UE asset path. The
		// inclusive upper bound 0x1F catches null + bell + escape + every C0 control. 0x7F is
		// DEL. Tab/newline/CR fall in this range too — also illegal in asset paths.
		if (Ch < 0x20 || Ch == 0x7F)
		{
			return {};
		}
	}

	// Windows MAX_PATH = 260. Asset paths get suffixed with ``.uasset`` (8 chars), and may be
	// further extended by tool wrappers (e.g. ``/Saved/...thumbnail.png`` paths). Cap at 240 to
	// leave headroom; UE asset names rarely exceed 64 chars in practice, so 240 is generous.
	if (Path.Len() > 240)
	{
		return {};
	}

	// Backslash rejected. Windows-style paths must not reach the asset-registry — they confuse
	// FPackageName's split logic which assumes UE's forward-slash convention.
	if (Path.Contains(TEXT("\\"), ESearchCase::CaseSensitive))
	{
		return {};
	}

	// Relative-path escape. ``/Game/../Foo`` could resolve outside the project mount and any
	// downstream cb.delete / cb.export would betray the sandbox.
	if (Path.Contains(TEXT(".."), ESearchCase::CaseSensitive))
	{
		return {};
	}

	// Wave S+18 (2026-05-26): empty path segments crash FName/FSoftObjectPath internals.
	// Discovered via B4 path-traversal stress: ``/Game//_X`` (double-slash) sent to
	// asset.get_property → FMCPReflection::ResolveObjectPath → FindObject → internal FName
	// construction died with editor crash, no Lane B recovery. Reject any "//" anywhere in
	// path (the leading "/" is the only legal start; doubled internal slashes are never
	// meaningful in UE asset paths).
	if (Path.Contains(TEXT("//"), ESearchCase::CaseSensitive))
	{
		return {};
	}

	// Wave S+19 (2026-05-26): "/./" relative-current-dir segment. While "/Game/./Foo" resolves
	// to "/Game/Foo" by Unix path semantics, the dot-segment is a documented bypass pattern for
	// naive substring-based defences and creates hostile-pattern variants the asset registry
	// silently accepts. Reject any "/./" anywhere; the leading "/" plus normal package segments
	// are all UE expects.
	if (Path.Contains(TEXT("/./"), ESearchCase::CaseSensitive))
	{
		return {};
	}

	// Wave S+19: URL-encoded reserved characters. ``%2E`` (.) and ``%2F`` (/) can sneak past
	// substring-based ".." / "//" guards while still being silently decoded by some downstream
	// path-canonicalisation code. UE itself does NOT URL-decode asset paths, so a literal
	// "%2E%2E" stays in the package name and resolves to a non-existent asset — but it's a clear
	// hostile intent marker. Reject case-insensitively.
	if (Path.Contains(TEXT("%2E"), ESearchCase::IgnoreCase) ||
		Path.Contains(TEXT("%2F"), ESearchCase::IgnoreCase))
	{
		return {};
	}

	// Must start with a forward slash — the universal UE mount-point marker.
	if (!Path.StartsWith(TEXT("/"), ESearchCase::CaseSensitive))
	{
		return {};
	}

	// Drop any trailing ``.LeafName`` segment so we always emit the package-name form. Sub-object
	// suffixes like ``Bar.Bar:SubObject`` are preserved by checking for ``:`` first — if present we
	// leave the path alone (the caller wanted the sub-object form for object-path APIs).
	if (!Path.Contains(TEXT(":"), ESearchCase::CaseSensitive))
	{
		int32 LastDot = INDEX_NONE;
		if (Path.FindLastChar(TEXT('.'), LastDot))
		{
			int32 LastSlash = INDEX_NONE;
			Path.FindLastChar(TEXT('/'), LastSlash);
			// Only strip a leaf suffix when the dot is AFTER the final slash (i.e. it's the
			// ``.LeafName`` part, not a dot inside a folder name — which UE convention forbids
			// anyway but we don't want to silently truncate user input).
			if (LastDot > LastSlash)
			{
				Path.LeftInline(LastDot, EAllowShrinking::No);
			}
		}
	}

	if (!IsKnownMountPoint(Path))
	{
		return {};
	}

	return Path;
}

bool IsValidGameOrPlugin(const FString& NormalizedPath)
{
	if (NormalizedPath.IsEmpty())
	{
		return false;
	}
	return IsKnownMountPoint(NormalizedPath);
}

bool IsWriteableMountPoint(const FString& NormalizedPath)
{
	if (NormalizedPath.IsEmpty())
	{
		return false;
	}

	// Hard-deny engine-owned namespaces. These are never user-writeable regardless of mount
	// registration state. Paths matching exactly the mount name (e.g. "/Engine") or having
	// the mount-name prefix (e.g. "/Engine/Foo") are both blocked.
	const TCHAR* const ReadOnlyPrefixes[] = {
		TEXT("/Engine/"),
		TEXT("/Script/"),
		TEXT("/Memory/"),
	};
	for (const TCHAR* Prefix : ReadOnlyPrefixes)
	{
		if (NormalizedPath.StartsWith(Prefix, ESearchCase::CaseSensitive))
		{
			return false;
		}
		// Bare mount-name match (no trailing component)
		FString Bare(Prefix);
		Bare.LeftChopInline(1, EAllowShrinking::No);  // drop trailing '/'
		if (NormalizedPath.Equals(Bare, ESearchCase::CaseSensitive))
		{
			return false;
		}
	}

	// Pull writable-only content paths from UE. bIncludeReadOnlyRoots=false excludes
	// /Engine + any plugin marked read-only. /Game is always writable in editor.
	TArray<FString> Mounts;
	FPackageName::QueryRootContentPaths(Mounts, /*bIncludeReadOnlyRoots*/ false,
		/*bWithoutLeadingSlashes*/ false, /*bWithoutTrailingSlashes*/ false);
	for (const FString& Mount : Mounts)
	{
		FString MountTrimmed = Mount;
		while (MountTrimmed.EndsWith(TEXT("/"), ESearchCase::CaseSensitive))
		{
			MountTrimmed.LeftChopInline(1, EAllowShrinking::No);
		}
		if (NormalizedPath.Equals(MountTrimmed, ESearchCase::CaseSensitive))
		{
			return true;
		}
		if (NormalizedPath.StartsWith(MountTrimmed + TEXT("/"), ESearchCase::CaseSensitive))
		{
			return true;
		}
	}
	return false;
}

FString ToPackageName(const FString& NormalizedPath)
{
	// Normalize already strips the leaf suffix — this is the canonical package-name form.
	return NormalizedPath;
}

FString ToObjectPath(const FString& NormalizedPath)
{
	if (NormalizedPath.IsEmpty())
	{
		return {};
	}

	// If the path already looks like an object path (contains ':' for sub-object, OR ends in
	// ``.LeafName`` already) leave it alone. Note that Normalize strips trailing ``.LeafName`` so
	// the only way the path still has a dot here is via the ``:`` sub-object form.
	if (NormalizedPath.Contains(TEXT(":"), ESearchCase::CaseSensitive))
	{
		return NormalizedPath;
	}

	// Extract the leaf (everything after the final slash) and append as ``.Leaf``.
	int32 LastSlash = INDEX_NONE;
	if (!NormalizedPath.FindLastChar(TEXT('/'), LastSlash))
	{
		// No slash — malformed; return empty so caller errors out.
		return {};
	}
	const FString Leaf = NormalizedPath.Mid(LastSlash + 1);
	if (Leaf.IsEmpty())
	{
		return {};
	}
	return NormalizedPath + TEXT(".") + Leaf;
}

bool ResolveAssetData(const FString& AnyPath, FAssetData& OutData)
{
	const FString Normalized = Normalize(AnyPath);
	if (Normalized.IsEmpty())
	{
		return false;
	}
	IAssetRegistry& IAR = FAssetRegistryModule::GetRegistry();

	// The FSoftObjectPath overload accepts both ``/Game/Foo/Bar`` and ``/Game/Foo/Bar.Bar`` forms,
	// but is documented to prefer the full object-path form. Build the object form explicitly so
	// behaviour matches across the package-vs-object input variants.
	const FString ObjectPath = ToObjectPath(Normalized);
	const FSoftObjectPath Soft(ObjectPath);
	const FAssetData Data = IAR.GetAssetByObjectPath(Soft, /*bIncludeOnlyOnDiskAssets*/ false);
	if (!Data.IsValid())
	{
		return false;
	}
	OutData = Data;
	return true;
}

} // namespace FMCPAssetPathUtils
