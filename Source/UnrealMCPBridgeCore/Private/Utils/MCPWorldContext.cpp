// Copyright FatumGame. All Rights Reserved.

#include "Utils/MCPWorldContext.h"

#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "UObject/Package.h"

namespace FMCPWorldContext
{

UWorld* GetEditorWorld()
{
	check(IsInGameThread());

	// GEditor can be null in commandlets / cooker / -game builds. Phase 3 tools refuse to run in
	// those contexts — null world is a hard error surfaced by the caller (kMCPErrorObjectNotFound
	// / kMCPErrorLevelNotFound depending on the tool). We deliberately do NOT fall back to
	// GEngine->GetWorldContexts() — Phase 3 is editor-only by design.
	if (!GEditor)
	{
		return nullptr;
	}
	return GEditor->GetEditorWorldContext().World();
}

bool IsPIEActive()
{
	check(IsInGameThread());
	return GEditor != nullptr && GEditor->PlayWorld != nullptr;
}

FString NormaliseMapPath(const FString& Raw)
{
	if (Raw.IsEmpty())
	{
		return FString();
	}

	// Wave S+19 (2026-05-26) hardening — bring NormaliseMapPath to parity with
	// FMCPAssetPathUtils::Normalize so level.* surfaces (level.duplicate / level.load /
	// level.create / level.save / level.unload / actor-path map-component resolution / etc.)
	// inherit the same crash-prevention guards. Discovered via B4 path-traversal sweep: backslash,
	// empty segments, control chars, and URL-encoded sequences were all silently accepted by
	// NormaliseMapPath (lenient: backslash WAS replaced with forward-slash, others uncheck'd),
	// allowing hostile-pattern variants through level mutators. The strict guards below match
	// FMCPAssetPathUtils::Normalize byte-for-byte.

	FString Out = Raw;
	Out.TrimStartAndEndInline();

	// Length cap (Windows MAX_PATH headroom). Same threshold as Normalize.
	if (Out.Len() > 240)
	{
		return FString();
	}

	// Reject control characters and DEL. None belong in a UE asset path — they corrupt FName
	// internals and asset registry serialisation.
	for (TCHAR Ch : Out)
	{
		if (Ch < 0x20 || Ch == 0x7F)
		{
			return FString();
		}
	}

	// Reject backslashes (NO LONGER auto-replaced — see Wave S+19 hardening rationale above).
	// Backslash in a UE asset path is always a code-smell or hostile probe.
	if (Out.Contains(TEXT("\\"), ESearchCase::CaseSensitive))
	{
		return FString();
	}

	// Drop trailing slashes (e.g. "/Game/Maps/" → "/Game/Maps").
	while (Out.Len() > 1 && Out.EndsWith(TEXT("/")))
	{
		Out.LeftChopInline(1);
	}

	// Reject relative-path escape and other hostile path-traversal patterns. ``..`` could escape
	// the project sandbox; ``//`` (double slash) crashes FName/FSoftObjectPath internals (S+18);
	// ``/./`` and URL-encoded ``%2E`` / ``%2F`` are bypass markers for naive substring guards.
	if (Out.Contains(TEXT(".."), ESearchCase::CaseSensitive))
	{
		return FString();
	}
	if (Out.Contains(TEXT("//"), ESearchCase::CaseSensitive))
	{
		return FString();
	}
	if (Out.Contains(TEXT("/./"), ESearchCase::CaseSensitive))
	{
		return FString();
	}
	if (Out.Contains(TEXT("%2E"), ESearchCase::IgnoreCase) ||
		Out.Contains(TEXT("%2F"), ESearchCase::IgnoreCase))
	{
		return FString();
	}

	// Strip the optional ``.AssetName`` suffix — both ``/Game/Maps/X`` and ``/Game/Maps/X.X``
	// must canonicalise to the package path ``/Game/Maps/X``. Splitting at the last ``.``
	// before any ``/`` after it is sufficient because asset names cannot contain ``/``.
	int32 LastSlashIdx = INDEX_NONE;
	Out.FindLastChar(TEXT('/'), LastSlashIdx);
	int32 LastDotIdx = INDEX_NONE;
	Out.FindLastChar(TEXT('.'), LastDotIdx);
	if (LastDotIdx != INDEX_NONE && LastDotIdx > LastSlashIdx)
	{
		Out.LeftInline(LastDotIdx, EAllowShrinking::No);
	}

	// Must start with ``/`` (mount point form). Phase 3 doesn't accept disk paths or relative.
	if (Out.IsEmpty() || Out[0] != TEXT('/'))
	{
		return FString();
	}

	return Out;
}

ULevel* ResolveLevelByMapPath(
	UWorld* World,
	const FString& MapPath,
	bool bRejectPartitioned,
	bool& bOutWPRejected)
{
	check(IsInGameThread());
	bOutWPRejected = false;

	if (!World)
	{
		return nullptr;
	}

	// WP rejection FIRST — partitioned worlds expose a different streaming model and we hard-reject
	// here so callers don't accidentally treat their findings as authoritative. Caller surfaces
	// kMCPErrorWorldPartitionNotSupported (-32029) before any further validation.
	if (bRejectPartitioned && World->IsPartitionedWorld())
	{
		bOutWPRejected = true;
		return nullptr;
	}

	const FString Norm = NormaliseMapPath(MapPath);
	if (Norm.IsEmpty())
	{
		return nullptr;
	}

	// Persistent level first — short-circuit because it's the common case.
	if (ULevel* Persistent = World->PersistentLevel)
	{
		if (UPackage* Pkg = Persistent->GetOutermost())
		{
			if (Pkg->GetName().Equals(Norm, ESearchCase::IgnoreCase))
			{
				return Persistent;
			}
		}
	}

	// Walk sublevels.
	for (ULevel* Level : World->GetLevels())
	{
		if (!Level)
		{
			continue;
		}
		if (UPackage* Pkg = Level->GetOutermost())
		{
			if (Pkg->GetName().Equals(Norm, ESearchCase::IgnoreCase))
			{
				return Level;
			}
		}
	}

	return nullptr;
}

ULevel* ResolveLevelOrNull(UWorld* World, const FString& MapPath)
{
	bool bWPRejectedUnused = false;
	return ResolveLevelByMapPath(World, MapPath, /*bRejectPartitioned*/ false, bWPRejectedUnused);
}

FString GetWorldPackagePath(UWorld* World)
{
	check(IsInGameThread());
	check(World != nullptr);

	if (UPackage* Pkg = World->GetOutermost())
	{
		return Pkg->GetName();
	}
	return FString();
}

} // namespace FMCPWorldContext
