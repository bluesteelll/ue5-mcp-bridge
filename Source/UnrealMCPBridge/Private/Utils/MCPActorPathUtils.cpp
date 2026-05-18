// Copyright FatumGame. All Rights Reserved.

#include "MCPActorPathUtils.h"

#include "MCPWorldContext.h"

#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "UObject/Package.h"

namespace FMCPActorPathUtils
{

bool ParseActorPath(const FString& Raw, FActorPathParts& OutParts, FString& OutError)
{
	OutParts = FActorPathParts();

	// Strip leading/trailing whitespace defensively — AI clients sometimes paste with newlines.
	FString Trimmed = Raw;
	Trimmed.TrimStartAndEndInline();
	if (Trimmed.IsEmpty())
	{
		OutError = TEXT("actor path is empty");
		return false;
	}
	if (Trimmed.Contains(TEXT("\\")))
	{
		OutError = FString::Printf(TEXT("actor path '%s' contains backslash"), *Raw);
		return false;
	}

	// Form 2: ``<map_path>::<actor_name>`` — split on the literal ``::``. Kept distinct from form 1
	// (UE uses ``:`` to separate sub-object paths within an outer, and we don't want to risk
	// confusing the two). AI agents that don't know the full sublevel path can use this form.
	int32 DoubleColonIdx = INDEX_NONE;
	if (Trimmed.FindLastChar(TEXT(':'), DoubleColonIdx)
		&& DoubleColonIdx > 0
		&& Trimmed[DoubleColonIdx - 1] == TEXT(':'))
	{
		OutParts.MapPath = Trimmed.Left(DoubleColonIdx - 1);
		OutParts.ActorName = Trimmed.Mid(DoubleColonIdx + 1);
		OutParts.bIsFullPath = true;
		if (OutParts.MapPath.IsEmpty() || OutParts.ActorName.IsEmpty())
		{
			OutError = FString::Printf(TEXT("malformed '<map>::<actor>' path '%s'"), *Raw);
			return false;
		}
		return true;
	}

	// Form 1: full canonical path ``/Game/Maps/X.X:PersistentLevel.<actor_fname>``.
	// We detect it by the leading ``/`` AND the presence of a ``:`` separator. Past the colon comes
	// ``<LevelName>.<ActorName>``; we keep only the last dot-separated token as the actor name.
	if (Trimmed.StartsWith(TEXT("/")) && Trimmed.Contains(TEXT(":")))
	{
		int32 ColonIdx = INDEX_NONE;
		Trimmed.FindChar(TEXT(':'), ColonIdx);
		check(ColonIdx > 0);
		FString MapPackage = Trimmed.Left(ColonIdx);
		FString AfterColon = Trimmed.Mid(ColonIdx + 1);

		// MapPackage may itself include ``.MapName`` suffix — strip it via NormaliseMapPath.
		MapPackage = FMCPWorldContext::NormaliseMapPath(MapPackage);

		// Take the last segment of AfterColon (split on '.') as the actor name.
		int32 LastDot = INDEX_NONE;
		AfterColon.FindLastChar(TEXT('.'), LastDot);
		const FString ActorName = (LastDot == INDEX_NONE) ? AfterColon : AfterColon.Mid(LastDot + 1);

		if (MapPackage.IsEmpty() || ActorName.IsEmpty())
		{
			OutError = FString::Printf(TEXT("malformed canonical actor path '%s'"), *Raw);
			return false;
		}

		OutParts.MapPath = MapPackage;
		OutParts.ActorName = ActorName;
		OutParts.bIsFullPath = true;
		return true;
	}

	// Form 3: bare name. Disallow slashes (would conflict with form 1 detection) and colons
	// (would conflict with form 2). Anything else is acceptable.
	if (Trimmed.Contains(TEXT("/")) || Trimmed.Contains(TEXT(":")))
	{
		OutError = FString::Printf(
			TEXT("actor path '%s' is malformed — bare-name form must not contain '/' or ':'"),
			*Raw);
		return false;
	}

	OutParts.MapPath.Reset();
	OutParts.ActorName = Trimmed;
	OutParts.bIsFullPath = false;
	return true;
}

FString BuildActorPath(const AActor* Actor)
{
	if (!Actor || !Actor->IsValidLowLevel())
	{
		return FString();
	}
	// UE's own GetPathName() is the canonical authority here — it emits
	// ``/Game/Maps/X.X:PersistentLevel.<actor_fname>`` exactly. Reimplementing it would risk
	// drift if Epic changes the format (e.g. external actors, OFPA persistent-level naming).
	return Actor->GetPathName();
}

namespace
{
	/** Find an actor by FName in a single level. Returns null if not present. */
	AActor* FindActorByFNameInLevel(ULevel* Level, const FString& ActorName)
	{
		if (!Level)
		{
			return nullptr;
		}
		const FName Target(*ActorName);
		for (AActor* Actor : Level->Actors)
		{
			if (!Actor)
			{
				continue;
			}
			if (Actor->GetFName() == Target)
			{
				return Actor;
			}
		}
		return nullptr;
	}

	/**
	 * Scan all loaded levels in ``World`` for actors whose FName matches ``ActorName``. Populates
	 * ``OutCandidates`` (size capped via OutCandidates.Num() check at call site, not here).
	 */
	void GatherActorsByBareNameInWorld(UWorld* World, const FString& ActorName, TArray<AActor*>& OutCandidates)
	{
		if (!World)
		{
			return;
		}
		const FName Target(*ActorName);
		for (ULevel* Level : World->GetLevels())
		{
			if (!Level)
			{
				continue;
			}
			for (AActor* Actor : Level->Actors)
			{
				if (Actor && Actor->GetFName() == Target)
				{
					OutCandidates.Add(Actor);
				}
			}
		}
	}
}

AActor* ResolveActor(
	const FString& Raw,
	bool bRejectPIE,
	bool& OutAmbiguous,
	FString& OutAmbiguityHint,
	FString& OutError)
{
	check(IsInGameThread());
	OutAmbiguous = false;
	OutAmbiguityHint.Reset();

	FActorPathParts Parts;
	if (!ParseActorPath(Raw, Parts, OutError))
	{
		return nullptr;
	}

	UWorld* EditorWorld = FMCPWorldContext::GetEditorWorld();
	if (!EditorWorld)
	{
		OutError = TEXT("no editor world available (GEditor missing)");
		return nullptr;
	}

	// When PIE is active and the caller did NOT opt out, also search the PIE world. This matches
	// the read-only-tools-see-PIE-actors rule.
	TArray<UWorld*, TInlineAllocator<2>> Worlds;
	Worlds.Add(EditorWorld);
	if (!bRejectPIE && FMCPWorldContext::IsPIEActive())
	{
		Worlds.Add(GEditor->PlayWorld);
	}

	// Form 1/2: explicit map path. Resolve level → find actor in level.
	if (Parts.bIsFullPath)
	{
		for (UWorld* W : Worlds)
		{
			ULevel* Level = FMCPWorldContext::ResolveLevelOrNull(W, Parts.MapPath);
			if (!Level)
			{
				continue;
			}
			if (AActor* Found = FindActorByFNameInLevel(Level, Parts.ActorName))
			{
				return Found;
			}
		}
		OutError = FString::Printf(
			TEXT("actor '%s' not found in level '%s' (full-path resolution failed)"),
			*Parts.ActorName, *Parts.MapPath);
		return nullptr;
	}

	// Form 3: bare name across loaded worlds. Multi-match → ambiguous.
	TArray<AActor*> Candidates;
	for (UWorld* W : Worlds)
	{
		GatherActorsByBareNameInWorld(W, Parts.ActorName, Candidates);
	}

	if (Candidates.Num() == 0)
	{
		OutError = FString::Printf(TEXT("actor '%s' not found in any loaded map"), *Parts.ActorName);
		return nullptr;
	}
	if (Candidates.Num() == 1)
	{
		return Candidates[0];
	}

	// Ambiguous — emit a bounded hint so the caller can surface a meaningful disambiguator.
	OutAmbiguous = true;
	constexpr int32 kMaxHintEntries = 16;
	const int32 HintCount = FMath::Min(Candidates.Num(), kMaxHintEntries);
	TArray<FString> Hints;
	Hints.Reserve(HintCount);
	for (int32 i = 0; i < HintCount; ++i)
	{
		Hints.Add(BuildActorPath(Candidates[i]));
	}
	OutAmbiguityHint = FString::Join(Hints, TEXT(";"));
	if (Candidates.Num() > kMaxHintEntries)
	{
		OutAmbiguityHint.Append(FString::Printf(TEXT(";... (+%d more)"),
			Candidates.Num() - kMaxHintEntries));
	}
	OutError = FString::Printf(
		TEXT("actor name '%s' is ambiguous — %d candidates; use the full ``<map>::<actor>`` form. Candidates: %s"),
		*Parts.ActorName, Candidates.Num(), *OutAmbiguityHint);
	return nullptr;
}

AActor* ResolveActorOrNull(const FString& Raw, bool bRejectPIE)
{
	bool bAmbig = false;
	FString HintUnused, ErrUnused;
	return ResolveActor(Raw, bRejectPIE, bAmbig, HintUnused, ErrUnused);
}

} // namespace FMCPActorPathUtils
