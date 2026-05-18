// Copyright FatumGame. All Rights Reserved.

#include "MCPWorldContext.h"

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

	// Collapse backslashes (Windows path leakage) and drop trailing slashes.
	FString Out = Raw.Replace(TEXT("\\"), TEXT("/"));
	while (Out.Len() > 1 && Out.EndsWith(TEXT("/")))
	{
		Out.LeftChopInline(1);
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
