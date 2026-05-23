// Copyright FatumGame. All Rights Reserved.

#include "ActorSpatialTools.h"

#include "MCPSurfaceRegistry.h"

#include "FMCPDispatchQueue.h"
#include "MCPJsonBuilder.h"
#include "MCPToolHelpers.h"
#include "Utils/MCPWorldContext.h"

#include "ConvexVolume.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "Engine/World.h"
#include "EngineUtils.h"        // TActorIterator
#include "GameFramework/Actor.h"
#include "LevelEditor.h"
#include "LevelEditorViewport.h"
#include "Math/PerspectiveMatrix.h"
#include "Math/RotationTranslationMatrix.h"
#include "Modules/ModuleManager.h"
#include "SLevelViewport.h"
#include "UnrealClient.h"
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

// --- actor.frustum_query ----------------------------------------------------------------------
//
// Args:    { camera_location?: [x,y,z], camera_rotation?: [pitch,yaw,roll],
//            fov_degrees?: number, near_clip?: number, far_clip?: number, aspect_ratio?: number,
//            viewport_index?: int, class_filter?: string, limit?: int }
// Result:  { actors: [{ path, name, class, location, distance_from_camera, intersection }],
//            total_matched: int, truncated: bool,
//            camera: { location, rotation, fov_degrees, near_clip, far_clip, aspect_ratio, source },
//            world: "editor" | "pie" }
//
// Read-only — no PIE guard. Queries actors whose component bounding box intersects the camera
// frustum. Uses ``GetViewFrustumBounds`` for canonical 6-plane setup (avoids the 5.x reverse-Z
// perspective matrix gotchas — manual frustum construction historically a source of off-by-one
// bugs, see CLAUDE.md / wave_m_critique.md C5).
//
// Camera-param resolution (per critique Q5 — viewport-defaults cascade):
//   1. Explicit field in args     → use as-is
//   2. ``viewport_index`` valid   → read viewport's ViewLocation/Rotation/ViewFOV/NearClip/FarClip
//   3. Default cascade viewport   → same as (2) using SHOT_FindBestDefaultViewport equivalent
//   4. Final fallback constants   → loc=[0,0,100], rot=[0,90,0], fov=90, near=10, far=100000
//
// ``aspect_ratio`` defaults to viewport_size.X / viewport_size.Y when a viewport is in use; else
// to 1.7777 (16:9). ``intersection`` is "fully_inside" or "partially_clipped" via the bOutFullyContained
// out-param of FConvexVolume::IntersectBox.
//
// ``distance_from_camera`` uses Actor->GetActorLocation() pivot (per critique N3 — matches what
// the outliner shows; NOT the bbox center).
FMCPResponse Tool_FrustumQuery(const FMCPRequest& Request)
{
	check(IsInGameThread());

	UWorld* World = ASPL_ResolveWorld();
	if (!World)
	{
		return FMCPToolHelpers::MakeError(Request, kASPLErrorInternal,
			TEXT("no editor world available"));
	}

	// ── Resolve viewport for fallback defaults (optional). ────────────────────────────────────
	// Cascade matches the SHOT_FindBestDefaultViewport pattern (currently active perspective vp →
	// last-key vp → module first-active → any non-null realised vp).
	FLevelEditorViewportClient* ViewportClient = nullptr;
	int32 ExplicitViewportIndex = -1;
	if (Request.Args.IsValid())
	{
		double IndexRaw = -1.0;
		if (Request.Args->TryGetNumberField(TEXT("viewport_index"), IndexRaw))
		{
			ExplicitViewportIndex = static_cast<int32>(IndexRaw);
		}
	}
	if (GEditor)
	{
		const TArray<FLevelEditorViewportClient*>& Clients = GEditor->GetLevelViewportClients();
		if (ExplicitViewportIndex >= 0)
		{
			if (ExplicitViewportIndex < Clients.Num()
				&& Clients[ExplicitViewportIndex]
				&& Clients[ExplicitViewportIndex]->Viewport)
			{
				ViewportClient = Clients[ExplicitViewportIndex];
			}
			// Else leave null — we'll fall through to constants. Bad explicit index is silent
			// (matches the spec's "viewport_index? only used if camera_* omitted" hint).
		}
		else
		{
			if (GCurrentLevelEditingViewportClient)
			{
				ViewportClient = GCurrentLevelEditingViewportClient;
			}
			else if (GLastKeyLevelEditingViewportClient)
			{
				ViewportClient = GLastKeyLevelEditingViewportClient;
			}
			else if (FLevelEditorModule* LE =
				FModuleManager::GetModulePtr<FLevelEditorModule>("LevelEditor"))
			{
				TSharedPtr<SLevelViewport> ActiveVP = LE->GetFirstActiveLevelViewport();
				if (ActiveVP.IsValid())
				{
					ViewportClient = &ActiveVP->GetLevelViewportClient();
				}
			}
			if (!ViewportClient)
			{
				for (FLevelEditorViewportClient* VC : Clients)
				{
					if (VC && VC->IsPerspective() && VC->Viewport
						&& VC->Viewport->GetSizeXY().X > 0 && VC->Viewport->GetSizeXY().Y > 0)
					{
						ViewportClient = VC;
						break;
					}
				}
			}
		}
	}

	// ── Defaults from viewport (or hardcoded fallback constants). ─────────────────────────────
	FVector CameraLoc(0.0, 0.0, 100.0);
	FRotator CameraRot(0.0, 90.0, 0.0);
	float Fov = 90.0f;
	float NearClip = 10.0f;
	float FarClip = 100000.0f;
	float AspectRatio = 1.7777f;
	bool bAnyCameraFieldProvided = false;
	bool bUsedViewportDefaults = false;

	if (ViewportClient && ViewportClient->Viewport)
	{
		CameraLoc = ViewportClient->GetViewLocation();
		CameraRot = ViewportClient->GetViewRotation();
		Fov       = ViewportClient->ViewFOV;
		const float VPNear = ViewportClient->GetNearClipPlane();
		if (VPNear > 0.0f) { NearClip = VPNear; }
		const float VPFar = ViewportClient->GetFarClipPlaneOverride();
		if (VPFar > 0.0f) { FarClip = VPFar; }
		const FIntPoint VPSize = ViewportClient->Viewport->GetSizeXY();
		if (VPSize.X > 0 && VPSize.Y > 0)
		{
			AspectRatio = static_cast<float>(VPSize.X) / static_cast<float>(VPSize.Y);
		}
		bUsedViewportDefaults = true;
	}

	// ── Override per-field from args (any single field optional). ─────────────────────────────
	if (Request.Args.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* LocArr = nullptr;
		if (Request.Args->TryGetArrayField(TEXT("camera_location"), LocArr) && LocArr && LocArr->Num() == 3)
		{
			double X = 0.0, Y = 0.0, Z = 0.0;
			if ((*LocArr)[0]->TryGetNumber(X) && (*LocArr)[1]->TryGetNumber(Y) && (*LocArr)[2]->TryGetNumber(Z))
			{
				CameraLoc = FVector(X, Y, Z);
				bAnyCameraFieldProvided = true;
			}
		}
		const TArray<TSharedPtr<FJsonValue>>* RotArr = nullptr;
		if (Request.Args->TryGetArrayField(TEXT("camera_rotation"), RotArr) && RotArr && RotArr->Num() == 3)
		{
			double P = 0.0, Y = 0.0, R = 0.0;
			if ((*RotArr)[0]->TryGetNumber(P) && (*RotArr)[1]->TryGetNumber(Y) && (*RotArr)[2]->TryGetNumber(R))
			{
				CameraRot = FRotator(P, Y, R);
				bAnyCameraFieldProvided = true;
			}
		}
		double Raw = 0.0;
		if (Request.Args->TryGetNumberField(TEXT("fov_degrees"), Raw))
		{
			Fov = static_cast<float>(Raw);
			bAnyCameraFieldProvided = true;
		}
		if (Request.Args->TryGetNumberField(TEXT("near_clip"), Raw))
		{
			NearClip = static_cast<float>(Raw);
			bAnyCameraFieldProvided = true;
		}
		if (Request.Args->TryGetNumberField(TEXT("far_clip"), Raw))
		{
			FarClip = static_cast<float>(Raw);
			bAnyCameraFieldProvided = true;
		}
		if (Request.Args->TryGetNumberField(TEXT("aspect_ratio"), Raw))
		{
			AspectRatio = static_cast<float>(Raw);
			bAnyCameraFieldProvided = true;
		}
	}

	// ── Validate ranges. ──────────────────────────────────────────────────────────────────────
	if (Fov <= 0.0f || Fov >= 180.0f)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			FString::Printf(TEXT("'fov_degrees' must be in (0, 180); got %g"), Fov));
	}
	if (NearClip <= 0.0f)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			FString::Printf(TEXT("'near_clip' must be > 0; got %g"), NearClip));
	}
	if (FarClip <= NearClip)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			FString::Printf(TEXT("'far_clip' (%g) must be greater than 'near_clip' (%g)"), FarClip, NearClip));
	}
	if (AspectRatio <= 0.0f)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			FString::Printf(TEXT("'aspect_ratio' must be > 0; got %g"), AspectRatio));
	}

	FMCPResponse Err;
	UClass* ClassFilter = nullptr;
	if (!ASPL_ResolveClassFilter(Request, ClassFilter, Err)) return Err;
	UClass* IteratorClass = ClassFilter ? ClassFilter : AActor::StaticClass();

	int32 Limit = 200;
	if (Request.Args.IsValid()) { Request.Args->TryGetNumberField(TEXT("limit"), Limit); }
	Limit = FMath::Clamp(Limit, 1, 1000);

	// ── Build view-projection matrix → 6 frustum planes. ──────────────────────────────────────
	// View matrix follows UE's canonical pattern (FInverseRotationMatrix * FTranslationMatrix +
	// the axis-flip "RHS→view" matrix that maps world XYZ to view ZXY). See
	// FSceneView::SetupViewRectUniformBufferParameters / GameplayStatics.cpp:3293.
	const FMatrix ViewRotationMatrix = FInverseRotationMatrix(CameraRot) * FMatrix(
		FPlane(0, 0, 1, 0),
		FPlane(1, 0, 0, 0),
		FPlane(0, 1, 0, 0),
		FPlane(0, 0, 0, 1));
	const FMatrix ViewMatrix = FTranslationMatrix(-CameraLoc) * ViewRotationMatrix;

	// FReversedZPerspectiveMatrix(HalfFOVrad, AspectRatio, MultFOVY=1, NearZ) builds the standard
	// UE 5.x reverse-Z infinite-far perspective matrix. We pass NearClip only — FarClip is implicit
	// (infinite); per critique C5 the FConvexVolume::GetViewFrustumBounds handles this correctly
	// when we additionally bound the far plane via the 3-arg GetViewFrustumBounds overload below.
	const float HalfFovRadians = FMath::DegreesToRadians(Fov * 0.5f);
	const FMatrix ProjectionMatrix = FReversedZPerspectiveMatrix(
		HalfFovRadians, AspectRatio, 1.0f, NearClip);
	const FMatrix ViewProjectionMatrix = ViewMatrix * ProjectionMatrix;

	// Explicitly bound far plane to user-requested FarClip. Without it the frustum extends to
	// infinity — fine for default clip but defeats the point of a custom far_clip arg.
	// View-space far plane: normal = +Z (looking down +X in view space rotates to +Z post axis-flip),
	// distance = -FarClip. Plane in world space requires inverse transform — easier to use the
	// 3-arg overload that accepts an explicit FPlane in world space.
	const FVector CameraForward = CameraRot.Vector();   // unit world-space forward
	const FPlane FarPlaneWS(CameraLoc + CameraForward * FarClip, -CameraForward);

	FConvexVolume Frustum;
	GetViewFrustumBounds(Frustum, ViewProjectionMatrix, FarPlaneWS, /*bOverrideFarPlane*/ true, /*bUseNearPlane*/ true);

	// ── Iterate actors and test against frustum. ──────────────────────────────────────────────
	struct FFrustumActorHit
	{
		AActor* Actor;
		double  DistanceFromCamera;
		bool    bFullyInside;
	};
	TArray<FFrustumActorHit> Hits;

	for (TActorIterator<AActor> It(World, IteratorClass); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor || !IsValid(Actor)) { continue; }
		const FBox Bounds = Actor->GetComponentsBoundingBox(/*bNonColliding*/ false);
		if (!Bounds.IsValid) { continue; }

		bool bFullyContained = false;
		const bool bIntersects = Frustum.IntersectBox(Bounds.GetCenter(), Bounds.GetExtent(), bFullyContained);
		if (!bIntersects) { continue; }

		const double Dist = FVector::Dist(Actor->GetActorLocation(), CameraLoc);
		Hits.Add({ Actor, Dist, bFullyContained });
	}

	// Sort by distance ascending — closest-first is the most useful ordering for "what's in view".
	Hits.Sort([](const FFrustumActorHit& A, const FFrustumActorHit& B)
	{
		return A.DistanceFromCamera < B.DistanceFromCamera;
	});

	const int32 TotalMatched = Hits.Num();
	const int32 LimitedNum = FMath::Min(TotalMatched, Limit);

	FMCPJsonArrayBuilder Items;
	for (int32 i = 0; i < LimitedNum; ++i)
	{
		const FFrustumActorHit& H = Hits[i];
		TSharedRef<FJsonObject> Obj = ASPL_BuildActorEntry(H.Actor);
		Obj->SetNumberField(TEXT("distance_from_camera"), H.DistanceFromCamera);
		Obj->SetStringField(TEXT("intersection"),
			H.bFullyInside ? TEXT("fully_inside") : TEXT("partially_clipped"));
		Items.AddValue(MakeShared<FJsonValueObject>(Obj));
	}

	// Build response camera sub-object (echo what we actually used for the frustum).
	TSharedRef<FJsonObject> CameraObj = MakeShared<FJsonObject>();
	CameraObj->SetArrayField(TEXT("location"), {
		MakeShared<FJsonValueNumber>(CameraLoc.X),
		MakeShared<FJsonValueNumber>(CameraLoc.Y),
		MakeShared<FJsonValueNumber>(CameraLoc.Z),
	});
	CameraObj->SetArrayField(TEXT("rotation"), {
		MakeShared<FJsonValueNumber>(CameraRot.Pitch),
		MakeShared<FJsonValueNumber>(CameraRot.Yaw),
		MakeShared<FJsonValueNumber>(CameraRot.Roll),
	});
	CameraObj->SetNumberField(TEXT("fov_degrees"), Fov);
	CameraObj->SetNumberField(TEXT("near_clip"), NearClip);
	CameraObj->SetNumberField(TEXT("far_clip"), FarClip);
	CameraObj->SetNumberField(TEXT("aspect_ratio"), AspectRatio);
	CameraObj->SetStringField(TEXT("source"),
		bAnyCameraFieldProvided ? TEXT("user_provided")
		: (bUsedViewportDefaults ? TEXT("viewport_default") : TEXT("constants")));

	const bool bIsPIE = FMCPWorldContext::IsPIEActive() && (World == GEditor->PlayWorld);

	return FMCPJsonBuilder()
		.Arr(TEXT("actors"), Items.ToValueArray())
		.Int(TEXT("total_matched"), TotalMatched)
		.Bool(TEXT("truncated"), TotalMatched > Limit)
		.ObjectShared(TEXT("camera"), CameraObj)
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

	// All Lane A — TActorIterator walks ULevel::Actors which is GT-only by Engine contract.
	// frustum_query additionally touches GEditor->GetLevelViewportClients() (GT-only) and
	// FViewport->GetSizeXY() during the viewport-defaults cascade.
	RegisterTool(TEXT("actor.box_query"),     &Tool_BoxQuery,     /*Lane A*/ false);
	RegisterTool(TEXT("actor.sphere_query"),  &Tool_SphereQuery,  /*Lane A*/ false);
	RegisterTool(TEXT("actor.frustum_query"), &Tool_FrustumQuery, /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("Actor-spatial surface registered: box_query + sphere_query + frustum_query (Lane A)"));
}

} // namespace FActorSpatialTools

MCP_REGISTER_SURFACE(ActorSpatialTools, &FActorSpatialTools::Register)

#undef LOCTEXT_NAMESPACE
