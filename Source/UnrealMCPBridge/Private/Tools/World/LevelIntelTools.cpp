// Copyright FatumGame. All Rights Reserved.

#include "LevelIntelTools.h"

#include "MCPSurfaceRegistry.h"

#include "FMCPDispatchQueue.h"
#include "MCPJsonBuilder.h"
#include "MCPToolHelpers.h"
#include "Utils/MCPWorldContext.h"

#include "Components/ActorComponent.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"          // TActorIterator
#include "GameFramework/Actor.h"
#include "UObject/UObjectGlobals.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// LVI_ prefix per the unity-build symbol-collision convention.
	constexpr int32 kLVIErrorInternal = -32603;

	UWorld* LVI_ResolveWorld()
	{
		if (!GEditor) return nullptr;
		if (FMCPWorldContext::IsPIEActive() && GEditor->PlayWorld) return GEditor->PlayWorld;
		return GEditor->GetEditorWorldContext().World();
	}
} // namespace

namespace FLevelIntelTools
{

// --- level.actor_summary ----------------------------------------------------------------------
//
// Args:    { top_n_classes?: int (default 20, clamp [1, 1000]) }
// Result:  { total_actors: int, total_classes: int,
//            classes: [{ class: string, count: int }],          // top N, desc by count
//            bounds_min: [x, y, z], bounds_max: [x, y, z],     // world AABB of all actor pivots
//            world: "editor" | "pie" }
//
// Read-only. Walks the current world's actor iterator once, accumulates class counts + bounds.
// Useful for "what's in this level" intelligence — show top contributors to actor budget,
// detect anomalies (10x more spawners than expected, etc.).
//
// bounds_min / bounds_max are computed over GetActorLocation (pivot), not GetActorBounds —
// matches box_query / sphere_query semantics. Returns origin-zero bounds for empty levels.
FMCPResponse Tool_ActorSummary(const FMCPRequest& Request)
{
	check(IsInGameThread());

	UWorld* World = LVI_ResolveWorld();
	if (!World)
	{
		return FMCPToolHelpers::MakeError(Request, kLVIErrorInternal, TEXT("no editor world available"));
	}

	int32 TopN = 20;
	if (Request.Args.IsValid()) { Request.Args->TryGetNumberField(TEXT("top_n_classes"), TopN); }
	TopN = FMath::Clamp(TopN, 1, 1000);

	TMap<FString, int32> CountsByClass;
	FVector BoundsMin( FLT_MAX,  FLT_MAX,  FLT_MAX);
	FVector BoundsMax(-FLT_MAX, -FLT_MAX, -FLT_MAX);
	int32 TotalActors = 0;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor || !IsValid(Actor)) { continue; }
		++TotalActors;
		const FString ClassPath = Actor->GetClass()->GetPathName();
		CountsByClass.FindOrAdd(ClassPath)++;
		const FVector Loc = Actor->GetActorLocation();
		BoundsMin = FVector(FMath::Min(BoundsMin.X, Loc.X), FMath::Min(BoundsMin.Y, Loc.Y), FMath::Min(BoundsMin.Z, Loc.Z));
		BoundsMax = FVector(FMath::Max(BoundsMax.X, Loc.X), FMath::Max(BoundsMax.Y, Loc.Y), FMath::Max(BoundsMax.Z, Loc.Z));
	}

	// Empty level guard — emit zeros rather than the FLT_MAX sentinels.
	if (TotalActors == 0)
	{
		BoundsMin = FVector::ZeroVector;
		BoundsMax = FVector::ZeroVector;
	}

	// Sort classes desc by count, then asc by name (stable tiebreaker).
	TArray<TPair<FString, int32>> Sorted;
	Sorted.Reserve(CountsByClass.Num());
	for (const auto& Pair : CountsByClass) { Sorted.Emplace(Pair.Key, Pair.Value); }
	Sorted.Sort([](const TPair<FString, int32>& A, const TPair<FString, int32>& B)
	{
		if (A.Value != B.Value) return A.Value > B.Value;
		return A.Key < B.Key;
	});

	FMCPJsonArrayBuilder ClassEntries;
	const int32 EmitCount = FMath::Min(TopN, Sorted.Num());
	for (int32 i = 0; i < EmitCount; ++i)
	{
		const TPair<FString, int32>& Entry = Sorted[i];
		ClassEntries.AddObject([&](FMCPJsonBuilder& B)
		{
			B.Str(TEXT("class"), Entry.Key).Int(TEXT("count"), Entry.Value);
		});
	}

	const bool bIsPIE = FMCPWorldContext::IsPIEActive() && (World == GEditor->PlayWorld);
	return FMCPJsonBuilder()
		.Int(TEXT("total_actors"), TotalActors)
		.Int(TEXT("total_classes"), CountsByClass.Num())
		.Arr(TEXT("classes"), ClassEntries.ToValueArray())
		.Arr(TEXT("bounds_min"), {
			MakeShared<FJsonValueNumber>(BoundsMin.X),
			MakeShared<FJsonValueNumber>(BoundsMin.Y),
			MakeShared<FJsonValueNumber>(BoundsMin.Z),
		})
		.Arr(TEXT("bounds_max"), {
			MakeShared<FJsonValueNumber>(BoundsMax.X),
			MakeShared<FJsonValueNumber>(BoundsMax.Y),
			MakeShared<FJsonValueNumber>(BoundsMax.Z),
		})
		.Str(TEXT("world"), bIsPIE ? TEXT("pie") : TEXT("editor"))
		.BuildSuccess(Request);
}

// --- level.find_actors_with_component ----------------------------------------------------------
//
// Args:    { component_class: string, limit?: int, class_filter?: string }
// Result:  { actors: [{ path, name, class, component_count: int }],
//            total_matched: int, truncated: bool, world: "editor" | "pie" }
//
// Iterates current world; for each actor (filtered by class_filter if supplied), counts
// components of type component_class. Emits actors with at least one matching component.
//
// component_class accepts /Script/Engine.X or BP class paths; resolves via LoadClass<UActorComponent>.
// Non-UActorComponent or unknown → -32011 WrongClass.
FMCPResponse Tool_FindActorsWithComponent(const FMCPRequest& Request)
{
	check(IsInGameThread());

	UWorld* World = LVI_ResolveWorld();
	if (!World)
	{
		return FMCPToolHelpers::MakeError(Request, kLVIErrorInternal, TEXT("no editor world available"));
	}

	FMCPResponse Err;
	FString ComponentClassPath;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("component_class"), ComponentClassPath, Err)) return Err;

	UClass* ComponentClass = LoadClass<UActorComponent>(nullptr, *ComponentClassPath);
	if (!ComponentClass)
	{
		ComponentClass = FindObject<UClass>(nullptr, *ComponentClassPath);
	}
	if (!ComponentClass || !ComponentClass->IsChildOf(UActorComponent::StaticClass()))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorWrongClass,
			FString::Printf(TEXT("component_class '%s' is not a valid UActorComponent subclass"), *ComponentClassPath));
	}

	// Optional class_filter for actor side.
	UClass* ActorClassFilter = nullptr;
	if (Request.Args.IsValid())
	{
		FString ActorClassPath;
		if (Request.Args->TryGetStringField(TEXT("class_filter"), ActorClassPath) && !ActorClassPath.IsEmpty())
		{
			ActorClassFilter = LoadClass<AActor>(nullptr, *ActorClassPath);
			if (!ActorClassFilter) { ActorClassFilter = FindObject<UClass>(nullptr, *ActorClassPath); }
			if (!ActorClassFilter || !ActorClassFilter->IsChildOf(AActor::StaticClass()))
			{
				return FMCPToolHelpers::MakeError(Request, kMCPErrorWrongClass,
					FString::Printf(TEXT("class_filter '%s' is not a valid AActor subclass"), *ActorClassPath));
			}
		}
	}

	int32 Limit = 500;
	if (Request.Args.IsValid()) { Request.Args->TryGetNumberField(TEXT("limit"), Limit); }
	Limit = FMath::Clamp(Limit, 1, 10000);

	UClass* IteratorClass = ActorClassFilter ? ActorClassFilter : AActor::StaticClass();

	FMCPJsonArrayBuilder Items;
	int32 TotalMatched = 0;
	for (TActorIterator<AActor> It(World, IteratorClass); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor || !IsValid(Actor)) { continue; }

		// Count components of the target class via GetComponents.
		TArray<UActorComponent*> Matching;
		Actor->GetComponents(ComponentClass, Matching, /*bIncludeFromChildActors*/ false);
		if (Matching.Num() == 0) { continue; }

		++TotalMatched;
		if (Items.Num() < Limit)
		{
			Items.AddObject([&](FMCPJsonBuilder& B)
			{
				B.Str(TEXT("path"), Actor->GetPathName())
				 .Str(TEXT("name"), Actor->GetActorLabel(false))
				 .Str(TEXT("class"), Actor->GetClass()->GetPathName())
				 .Int(TEXT("component_count"), Matching.Num());
			});
		}
	}

	const bool bIsPIE = FMCPWorldContext::IsPIEActive() && (World == GEditor->PlayWorld);
	return FMCPJsonBuilder()
		.Arr(TEXT("actors"), Items.ToValueArray())
		.Int(TEXT("total_matched"), TotalMatched)
		.Bool(TEXT("truncated"), TotalMatched > Limit)
		.Str(TEXT("world"), bIsPIE ? TEXT("pie") : TEXT("editor"))
		.BuildSuccess(Request);
}

void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	// Both Lane A — TActorIterator + GetComponents walk UObject globals (GT-only).
	RegisterTool(TEXT("level.actor_summary"),               &Tool_ActorSummary,             /*Lane A*/ false);
	RegisterTool(TEXT("level.find_actors_with_component"),  &Tool_FindActorsWithComponent,  /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("LevelIntel surface registered: level.actor_summary + level.find_actors_with_component (Lane A)"));
}

} // namespace FLevelIntelTools

MCP_REGISTER_SURFACE(LevelIntelTools, &FLevelIntelTools::Register)

#undef LOCTEXT_NAMESPACE
