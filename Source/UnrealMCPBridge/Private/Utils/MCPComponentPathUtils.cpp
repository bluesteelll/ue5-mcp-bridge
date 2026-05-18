// Copyright FatumGame. All Rights Reserved.

#include "MCPComponentPathUtils.h"

#include "MCPActorPathUtils.h"

#include "Components/ActorComponent.h"
#include "GameFramework/Actor.h"

namespace FMCPComponentPathUtils
{

bool ParseComponentPath(const FString& Raw, FComponentPathParts& OutParts, FString& OutError)
{
	OutParts = FComponentPathParts();

	FString Trimmed = Raw;
	Trimmed.TrimStartAndEndInline();
	if (Trimmed.IsEmpty())
	{
		OutError = TEXT("component path is empty");
		return false;
	}

	int32 LastSlashIdx = INDEX_NONE;
	if (!Trimmed.FindLastChar(TEXT('/'), LastSlashIdx))
	{
		OutError = FString::Printf(
			TEXT("component path '%s' must contain '/<component_name>' suffix"), *Raw);
		return false;
	}

	// Both halves must be non-empty.
	OutParts.ActorPathRaw = Trimmed.Left(LastSlashIdx);
	OutParts.ComponentName = Trimmed.Mid(LastSlashIdx + 1);

	if (OutParts.ActorPathRaw.IsEmpty())
	{
		OutError = FString::Printf(TEXT("component path '%s' has empty actor segment"), *Raw);
		return false;
	}
	if (OutParts.ComponentName.IsEmpty())
	{
		OutError = FString::Printf(TEXT("component path '%s' has empty component segment"), *Raw);
		return false;
	}

	return true;
}

FString BuildComponentPath(const UActorComponent* Component)
{
	if (!Component)
	{
		return FString();
	}
	const AActor* Owner = Component->GetOwner();
	if (!Owner)
	{
		return FString();
	}
	const FString ActorPath = FMCPActorPathUtils::BuildActorPath(Owner);
	if (ActorPath.IsEmpty())
	{
		return FString();
	}
	return ActorPath + TEXT("/") + Component->GetFName().ToString();
}

UActorComponent* ResolveComponent(
	const FString& Raw,
	bool bRejectPIE,
	bool& OutAmbiguous,
	FString& OutAmbiguityHint,
	FString& OutError)
{
	check(IsInGameThread());
	OutAmbiguous = false;
	OutAmbiguityHint.Reset();

	FComponentPathParts Parts;
	if (!ParseComponentPath(Raw, Parts, OutError))
	{
		return nullptr;
	}

	bool bActorAmbiguous = false;
	FString ActorAmbiguityHint;
	FString ActorError;
	AActor* Owner = FMCPActorPathUtils::ResolveActor(
		Parts.ActorPathRaw, bRejectPIE, bActorAmbiguous, ActorAmbiguityHint, ActorError);
	if (!Owner)
	{
		// Propagate the actor-side error verbatim — caller surfaces under kMCPErrorObjectNotFound
		// (actor scope), distinct from the component-scope kMCPErrorComponentNotFound. Tools route
		// the message based on whether they got a component-resolve null or an actor-resolve null.
		OutError = ActorError;
		return nullptr;
	}

	// Collect all components whose internal FName matches.
	const FName Target(*Parts.ComponentName);
	TArray<UActorComponent*> Matches;
	Owner->GetComponents(Matches);
	for (int32 i = Matches.Num() - 1; i >= 0; --i)
	{
		if (!Matches[i] || Matches[i]->GetFName() != Target)
		{
			Matches.RemoveAtSwap(i, EAllowShrinking::No);
		}
	}

	if (Matches.Num() == 0)
	{
		OutError = FString::Printf(
			TEXT("component '%s' not found on actor '%s'"),
			*Parts.ComponentName, *Owner->GetName());
		return nullptr;
	}
	if (Matches.Num() == 1)
	{
		return Matches[0];
	}

	// >1: ambiguous. Compose hint of class names (FName-matched components SHOULD be rare —
	// usually only happens with SCS rename collisions).
	OutAmbiguous = true;
	constexpr int32 kMaxHintEntries = 16;
	const int32 HintCount = FMath::Min(Matches.Num(), kMaxHintEntries);
	TArray<FString> Hints;
	Hints.Reserve(HintCount);
	for (int32 i = 0; i < HintCount; ++i)
	{
		Hints.Add(Matches[i]->GetClass()->GetName());
	}
	OutAmbiguityHint = FString::Join(Hints, TEXT(";"));
	if (Matches.Num() > kMaxHintEntries)
	{
		OutAmbiguityHint.Append(FString::Printf(TEXT(";... (+%d more)"),
			Matches.Num() - kMaxHintEntries));
	}
	OutError = FString::Printf(
		TEXT("component name '%s' on actor '%s' is ambiguous — %d matches by class: %s"),
		*Parts.ComponentName, *Owner->GetName(), Matches.Num(), *OutAmbiguityHint);
	return nullptr;
}

} // namespace FMCPComponentPathUtils
