// Copyright FatumGame. All Rights Reserved.

#include "ActorSpatialTools.h"

#include "MCPSurfaceRegistry.h"

#include "FMCPDispatchQueue.h"
#include "MCPJsonBuilder.h"
#include "MCPToolHelpers.h"
#include "Utils/MCPWorldContext.h"

#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"        // TActorIterator
#include "GameFramework/Actor.h"
#include "UObject/UObjectGlobals.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// ASPL_ prefix per the unity-build symbol-collision convention.
	constexpr int32 kASPLErrorInternal = -32603;

	// Resolve the world to query. Editor world unless PIE is active (in which case PIE world
	// to mirror what the player would see). Returns nullptr if no world available.
	UWorld* ASPL_ResolveWorld()
	{
		if (!GEditor)
		{
			return nullptr;
		}
		if (FMCPWorldContext::IsPIEActive() && GEditor->PlayWorld)
		{
			return GEditor->PlayWorld;
		}
		return GEditor->GetEditorWorldContext().World();
	}

	// Resolve optional class_filter argument. Returns:
	//   - SUCCESS with non-null UClass on valid AActor subclass
	//   - SUCCESS with null UClass when class_filter is absent/empty (= "any actor class")
	//   - FAILURE with -32011 WrongClass when class_filter is malformed/non-AActor
	bool ASPL_ResolveClassFilter(const FMCPRequest& Request, UClass*& OutClass, FMCPResponse& OutError)
	{
		OutClass = nullptr;
		if (!Request.Args.IsValid())
		{
			return true; // no args object → no filter, ok
		}

		FString ClassFilter;
		if (!Request.Args->TryGetStringField(TEXT("class_filter"), ClassFilter) || ClassFilter.IsEmpty())
		{
			return true; // absent → no filter
		}

		UClass* Loaded = LoadClass<AActor>(nullptr, *ClassFilter);
		if (!Loaded)
		{
			// Try as a bare class path (e.g. "/Script/Engine.StaticMeshActor")
			Loaded = FindObject<UClass>(nullptr, *ClassFilter);
		}
		if (!Loaded || !Loaded->IsChildOf(AActor::StaticClass()))
		{
			OutError = FMCPToolHelpers::MakeError(Request, kMCPErrorWrongClass,
				FString::Printf(TEXT("class_filter '%s' is not a valid AActor subclass"), *ClassFilter));
			return false;
		}
		OutClass = Loaded;
		return true;
	}

	// Read a required FVector from a JSON array field [x, y, z]. Sets OutErr on failure.
	bool ASPL_RequireVector3(const FMCPRequest& Request, const TCHAR* FieldName, FVector& OutVec, FMCPResponse& OutErr)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!FMCPToolHelpers::RequireArrayField(Request, FieldName, Arr, OutErr))
		{
			return false;
		}
		if (Arr->Num() != 3)
		{
			OutErr = FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
				FString::Printf(TEXT("'%s' must be [x, y, z] (got %d elements)"), FieldName, Arr->Num()));
			return false;
		}
		double X = 0.0, Y = 0.0, Z = 0.0;
		if (!(*Arr)[0]->TryGetNumber(X) || !(*Arr)[1]->TryGetNumber(Y) || !(*Arr)[2]->TryGetNumber(Z))
		{
			OutErr = FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
				FString::Printf(TEXT("'%s' elements must be numbers"), FieldName));
			return false;
		}
		OutVec = FVector(X, Y, Z);
		return true;
	}

	// Build a per-actor JSON entry: { path, name, class, location: [x,y,z] }.
	TSharedRef<FJsonObject> ASPL_BuildActorEntry(AActor* Actor)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("path"), Actor->GetPathName());
		Obj->SetStringField(TEXT("name"), Actor->GetActorLabel(/*bCreateIfNone*/ false));
		Obj->SetStringField(TEXT("class"), Actor->GetClass()->GetPathName());
		const FVector Loc = Actor->GetActorLocation();
		TArray<TSharedPtr<FJsonValue>> LocArr = {
			MakeShared<FJsonValueNumber>(Loc.X),
			MakeShared<FJsonValueNumber>(Loc.Y),
			MakeShared<FJsonValueNumber>(Loc.Z),
		};
		Obj->SetArrayField(TEXT("location"), LocArr);
		return Obj;
	}
} // namespace

namespace FActorSpatialTools
{

// --- actor.box_query --------------------------------------------------------------------------
//
// Args:    { min: [x, y, z], max: [x, y, z], class_filter?: string, limit?: int }
// Result:  { actors: [{ path, name, class, location: [x, y, z] }],
//            total_matched: int,           // count before limit truncation
//            truncated: bool,              // was result limited
//            world: "editor" | "pie" }
//
// Read-only — no PIE guard. AABB containment check on AActor::GetActorLocation (each actor's
// pivot point), NOT the actor's rendered bounds. For "find actors whose physical extent overlaps
// this box", use `physics.overlap_test` instead.
//
// class_filter accepts:
//   - "/Script/Engine.StaticMeshActor" — full class path
//   - "BP_Enemy_C" — looked up via LoadClass<AActor>
// Filter must resolve to a UClass that IsChildOf(AActor); otherwise -32011 WrongClass.
//
// Default limit=500; clamp [1, 10000]. Matches the conventions of asset.*/actor.* enumeration tools.
FMCPResponse Tool_BoxQuery(const FMCPRequest& Request)
{
	check(IsInGameThread());

	UWorld* World = ASPL_ResolveWorld();
	if (!World)
	{
		return FMCPToolHelpers::MakeError(Request, kASPLErrorInternal,
			TEXT("no editor world available"));
	}

	FMCPResponse Err;
	FVector Min, Max;
	if (!ASPL_RequireVector3(Request, TEXT("min"), Min, Err)) return Err;
	if (!ASPL_RequireVector3(Request, TEXT("max"), Max, Err)) return Err;

	// Normalize: callers may pass min/max in either order; sort component-wise.
	const FVector BoxMin(FMath::Min(Min.X, Max.X), FMath::Min(Min.Y, Max.Y), FMath::Min(Min.Z, Max.Z));
	const FVector BoxMax(FMath::Max(Min.X, Max.X), FMath::Max(Min.Y, Max.Y), FMath::Max(Min.Z, Max.Z));

	UClass* ClassFilter = nullptr;
	if (!ASPL_ResolveClassFilter(Request, ClassFilter, Err)) return Err;
	UClass* IteratorClass = ClassFilter ? ClassFilter : AActor::StaticClass();

	int32 Limit = 500;
	if (Request.Args.IsValid()) { Request.Args->TryGetNumberField(TEXT("limit"), Limit); }
	Limit = FMath::Clamp(Limit, 1, 10000);

	FMCPJsonArrayBuilder Items;
	int32 TotalMatched = 0;
	for (TActorIterator<AActor> It(World, IteratorClass); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor || !IsValid(Actor)) { continue; }
		const FVector Loc = Actor->GetActorLocation();
		if (Loc.X < BoxMin.X || Loc.X > BoxMax.X) { continue; }
		if (Loc.Y < BoxMin.Y || Loc.Y > BoxMax.Y) { continue; }
		if (Loc.Z < BoxMin.Z || Loc.Z > BoxMax.Z) { continue; }
		++TotalMatched;
		if (Items.Num() < Limit)
		{
			Items.AddValue(MakeShared<FJsonValueObject>(ASPL_BuildActorEntry(Actor)));
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

// --- actor.sphere_query -----------------------------------------------------------------------
//
// Args:    { center: [x, y, z], radius: number, class_filter?: string, limit?: int }
// Result:  { actors: [{ path, name, class, location: [x, y, z] }],
//            total_matched: int, truncated: bool, world: "editor" | "pie" }
//
// Read-only — no PIE guard. Distance check from center using AActor::GetActorLocation.
// radius must be > 0; squared distance comparison used internally.
//
// Same class_filter / limit semantics as box_query.
FMCPResponse Tool_SphereQuery(const FMCPRequest& Request)
{
	check(IsInGameThread());

	UWorld* World = ASPL_ResolveWorld();
	if (!World)
	{
		return FMCPToolHelpers::MakeError(Request, kASPLErrorInternal,
			TEXT("no editor world available"));
	}

	FMCPResponse Err;
	FVector Center;
	if (!ASPL_RequireVector3(Request, TEXT("center"), Center, Err)) return Err;

	double Radius = 0.0;
	if (!FMCPToolHelpers::RequireNumberField(Request, TEXT("radius"), Radius, Err)) return Err;
	if (Radius <= 0.0)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			FString::Printf(TEXT("'radius' must be > 0 (got %f)"), Radius));
	}
	const double RadiusSq = Radius * Radius;

	UClass* ClassFilter = nullptr;
	if (!ASPL_ResolveClassFilter(Request, ClassFilter, Err)) return Err;
	UClass* IteratorClass = ClassFilter ? ClassFilter : AActor::StaticClass();

	int32 Limit = 500;
	if (Request.Args.IsValid()) { Request.Args->TryGetNumberField(TEXT("limit"), Limit); }
	Limit = FMath::Clamp(Limit, 1, 10000);

	FMCPJsonArrayBuilder Items;
	int32 TotalMatched = 0;
	for (TActorIterator<AActor> It(World, IteratorClass); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor || !IsValid(Actor)) { continue; }
		const FVector Loc = Actor->GetActorLocation();
		const double DistSq = FVector::DistSquared(Loc, Center);
		if (DistSq > RadiusSq) { continue; }
		++TotalMatched;
		if (Items.Num() < Limit)
		{
			Items.AddValue(MakeShared<FJsonValueObject>(ASPL_BuildActorEntry(Actor)));
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

	// Both Lane A — TActorIterator walks ULevel::Actors which is GT-only by Engine contract.
	RegisterTool(TEXT("actor.box_query"),    &Tool_BoxQuery,    /*Lane A*/ false);
	RegisterTool(TEXT("actor.sphere_query"), &Tool_SphereQuery, /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("Actor-spatial surface registered: actor.box_query + actor.sphere_query (Lane A)"));
}

} // namespace FActorSpatialTools

MCP_REGISTER_SURFACE(ActorSpatialTools, &FActorSpatialTools::Register)

#undef LOCTEXT_NAMESPACE
