// Copyright FatumGame. All Rights Reserved.

#include "LandscapeTools.h"

#include "FMCPDispatchQueue.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPActorPathUtils.h"

#include "Editor.h"
#include "EngineUtils.h"                            // TActorIterator
#include "Engine/World.h"
#include "Landscape.h"                              // ALandscape (transitively brings LandscapeProxy.h)
#include "LandscapeComponent.h"
#include "LandscapeDataAccess.h"                    // LandscapeDataAccess::GetLocalHeight, LANDSCAPE_ZSCALE
#include "LandscapeEdit.h"                          // FLandscapeEditDataInterface
#include "LandscapeInfo.h"                          // ULandscapeInfo, FLandscapeInfoLayerSettings
#include "LandscapeLayerInfoObject.h"               // ULandscapeLayerInfoObject
#include "LandscapeUtils.h"                         // UE::Landscape::IsVisibilityLayer

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// LAND_ prefix per the unity-build symbol-collision convention.
	constexpr int32 kLANDErrorInvalidParams   = -32602;
	constexpr int32 kLANDErrorInternal        = -32603;
	constexpr int32 kLANDErrorObjectNotFound  = kMCPErrorObjectNotFound;  // -32004
	constexpr int32 kLANDErrorInvalidPath     = kMCPErrorInvalidPath;     // -32010
	constexpr int32 kLANDErrorWrongClass      = kMCPErrorWrongClass;      // -32011

	void LAND_StampIds(const FMCPRequest& Request, FMCPResponse& Response)
	{
		Response.RequestId = Request.RequestId;
		Response.OriginalIdString = Request.OriginalIdString;
	}

	FMCPResponse LAND_MakeError(const FMCPRequest& Request, int32 Code, const FString& Message)
	{
		FMCPResponse R;
		LAND_StampIds(Request, R);
		R.bIsError = true;
		R.ErrorCode = Code;
		R.ErrorMessage = Message;
		return R;
	}

	FMCPResponse LAND_MakeSuccessObj(const FMCPRequest& Request, TSharedPtr<FJsonObject> Result)
	{
		FMCPResponse R;
		LAND_StampIds(Request, R);
		R.bIsError = false;
		R.Result = MakeShared<FJsonValueObject>(MoveTemp(Result));
		return R;
	}

	bool LAND_RequireStringField(const FMCPRequest& Request, const TCHAR* FieldName,
		FString& OutValue, FMCPResponse& OutError)
	{
		if (!Request.Args.IsValid())
		{
			OutError = LAND_MakeError(Request, kLANDErrorInvalidParams, TEXT("missing args object"));
			return false;
		}
		if (!Request.Args->TryGetStringField(FieldName, OutValue) || OutValue.IsEmpty())
		{
			OutError = LAND_MakeError(Request, kLANDErrorInvalidParams,
				FString::Printf(TEXT("missing required string field '%s'"), FieldName));
			return false;
		}
		return true;
	}

	bool LAND_RequireNumberField(const FMCPRequest& Request, const TCHAR* FieldName,
		double& OutValue, FMCPResponse& OutError)
	{
		if (!Request.Args.IsValid())
		{
			OutError = LAND_MakeError(Request, kLANDErrorInvalidParams, TEXT("missing args object"));
			return false;
		}
		if (!Request.Args->TryGetNumberField(FieldName, OutValue))
		{
			OutError = LAND_MakeError(Request, kLANDErrorInvalidParams,
				FString::Printf(TEXT("missing required number field '%s'"), FieldName));
			return false;
		}
		return true;
	}

	/** Convert FVector → JSON [x,y,z] array. Mirrors NavMeshTools shape. */
	TArray<TSharedPtr<FJsonValue>> LAND_VectorToArray(const FVector& V)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Add(MakeShared<FJsonValueNumber>(V.X));
		Arr.Add(MakeShared<FJsonValueNumber>(V.Y));
		Arr.Add(MakeShared<FJsonValueNumber>(V.Z));
		return Arr;
	}

	/**
	 * Resolve an ALandscape by actor_path. Returns null + writes ``OutErr`` + ``OutErrCode`` on
	 * failure. Read-only path — permits PIE actors (bRejectPIE=false), though landscape inspection
	 * is meaningful only against the editor world (PIE's landscape is typically a duplicate proxy
	 * with no useful editor-side ULandscapeInfo).
	 */
	ALandscape* LAND_ResolveLandscape(const FString& ActorPath, int32& OutErrCode, FString& OutErr)
	{
		if (ActorPath.IsEmpty())
		{
			OutErrCode = kLANDErrorInvalidPath;
			OutErr = TEXT("landscape_path is empty");
			return nullptr;
		}

		bool bAmbig = false;
		FString AmbigHint;
		FString ResolveErr;
		AActor* Actor = FMCPActorPathUtils::ResolveActor(
			ActorPath, /*bRejectPIE*/ false, bAmbig, AmbigHint, ResolveErr);
		if (!Actor)
		{
			if (bAmbig)
			{
				OutErrCode = kLANDErrorObjectNotFound;
				OutErr = FString::Printf(
					TEXT("landscape_path '%s' is ambiguous; candidates: %s"),
					*ActorPath, *AmbigHint);
				return nullptr;
			}
			OutErrCode = kLANDErrorObjectNotFound;
			OutErr = FString::Printf(
				TEXT("landscape_path '%s' did not resolve: %s"),
				*ActorPath, *ResolveErr);
			return nullptr;
		}
		ALandscape* LS = Cast<ALandscape>(Actor);
		if (!LS)
		{
			OutErrCode = kLANDErrorWrongClass;
			OutErr = FString::Printf(
				TEXT("actor '%s' is not an ALandscape (got class '%s')"),
				*ActorPath, *Actor->GetClass()->GetName());
			return nullptr;
		}
		return LS;
	}

	/**
	 * Convert a world-XY position to landscape-local quad coordinates.
	 *
	 * ``InverseTransformPosition`` factors out scale, so LocalPos.{X,Y} is in unscaled actor space —
	 * which for the landscape IS quad space directly (one quad per unit). We floor so a query at
	 * sub-quad precision lands on the containing cell (matches heightmap texel granularity).
	 *
	 * Returns the (QuadX, QuadY) integer pair. Caller MUST gate the result via
	 * ``LAND_IsQuadInBounds`` before sampling — out-of-bounds quads return empty TMap data from
	 * FLandscapeEditDataInterface which would otherwise look the same as "in bounds but no data".
	 */
	FIntPoint LAND_WorldXYToQuad(const ALandscape* LS, double WorldX, double WorldY)
	{
		check(LS);
		// LandscapeActorToWorld() is the canonical landscape transform — for the main ALandscape
		// it equals GetActorTransform(); for streaming proxies it subtracts the SectionBase shift.
		// We only enumerate main ALandscape actors so the distinction is moot here, but using the
		// canonical accessor matches what the engine's own GetHeightAtLocation / overlap helpers do.
		const FTransform LSXform = LS->LandscapeActorToWorld();
		const FVector LocalPos = LSXform.InverseTransformPosition(FVector(WorldX, WorldY, 0.0));
		// InverseTransformPosition already factors out scale — LocalPos is in "unscaled actor space"
		// which for the landscape IS quad space directly (one quad per unit in unscaled local space).
		const int32 QuadX = FMath::FloorToInt(LocalPos.X);
		const int32 QuadY = FMath::FloorToInt(LocalPos.Y);
		return FIntPoint(QuadX, QuadY);
	}

	/** True iff (QuadX, QuadY) falls inside the landscape's component extent. */
	bool LAND_IsQuadInBounds(ULandscapeInfo* LSI, int32 QuadX, int32 QuadY)
	{
		check(LSI);
		int32 MinX = MAX_int32, MinY = MAX_int32, MaxX = MIN_int32, MaxY = MIN_int32;
		if (!LSI->GetLandscapeExtent(MinX, MinY, MaxX, MaxY))
		{
			// No registered components → no valid bounds.
			return false;
		}
		return (QuadX >= MinX && QuadX <= MaxX && QuadY >= MinY && QuadY <= MaxY);
	}
} // namespace

namespace FLandscapeTools
{

// ─── landscape.list ──────────────────────────────────────────────────────────────────────────────
//
// Args: (no args)
//
// Result:
//   {
//     landscapes: [
//       { actor_path, component_count, world_bounds: { origin: [x,y,z], extent: [x,y,z] } }, ...
//     ]
//   }
//
// Errors:
//   -32603 Internal    no editor world available (commandlet / no map loaded)
//
// Notes:
//   - Uses ``GEditor->GetEditorWorldContext().World()`` per brief — these tools are editor-only
//     inspection (the PIE-side landscape is a transient duplicate without useful editor metadata).
//   - ``TActorIterator<ALandscape>`` only yields the persistent ALandscape actor; landscape proxies
//     (ALandscapeStreamingProxy) in sublevels are NOT enumerated. This matches the outliner UX
//     where "the landscape" is the single main actor and proxies are component-level details.
//   - ``world_bounds`` is the aggregate AABB of all LandscapeComponents (``GetComponentsBoundingBox``).
//     For un-streamed landscapes this equals the full visual extent; for World Partition / streaming
//     setups it's the bounds of currently-loaded components only.
FMCPResponse Tool_List(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!GEditor)
	{
		return LAND_MakeError(Request, kLANDErrorInternal,
			TEXT("no GEditor available (commandlet / cooked build)"));
	}
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		return LAND_MakeError(Request, kLANDErrorInternal,
			TEXT("no editor world available (no map loaded)"));
	}

	TArray<TSharedPtr<FJsonValue>> LandscapesArr;
	for (TActorIterator<ALandscape> It(World); It; ++It)
	{
		ALandscape* LS = *It;
		if (!LS) { continue; }

		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("actor_path"), FMCPActorPathUtils::BuildActorPath(LS));
		Obj->SetNumberField(TEXT("component_count"),
			static_cast<double>(LS->LandscapeComponents.Num()));

		const FBox Bounds = LS->GetComponentsBoundingBox(/*bNonColliding*/ true);
		TSharedRef<FJsonObject> BoundsObj = MakeShared<FJsonObject>();
		const FVector Origin = Bounds.IsValid ? Bounds.GetCenter() : FVector::ZeroVector;
		const FVector Extent = Bounds.IsValid ? Bounds.GetExtent() : FVector::ZeroVector;
		BoundsObj->SetArrayField(TEXT("origin"), LAND_VectorToArray(Origin));
		BoundsObj->SetArrayField(TEXT("extent"), LAND_VectorToArray(Extent));
		Obj->SetObjectField(TEXT("world_bounds"), BoundsObj);

		LandscapesArr.Add(MakeShared<FJsonValueObject>(Obj));
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetArrayField(TEXT("landscapes"), LandscapesArr);
	return LAND_MakeSuccessObj(Request, Out);
}

// ─── landscape.get_info ──────────────────────────────────────────────────────────────────────────
//
// Args:
//   landscape_path : string    required — actor path of an ALandscape
//
// Result:
//   {
//     component_size_quads : int,
//     num_subsections      : int,
//     layer_infos          : [ { name: string, type: "Weight" | "Visibility" }, ... ],
//     min_z                : float,    // world-space, from aggregate bounds
//     max_z                : float,
//     total_components     : int
//   }
//
// Errors:
//   -32004 ObjectNotFound       landscape_path missing / not an actor
//   -32010 InvalidPath          landscape_path empty
//   -32011 WrongClass           resolved actor is not ALandscape
//   -32602 InvalidParams        missing landscape_path field
//   -32603 Internal             landscape has no ULandscapeInfo (un-registered / mid-load proxy)
//
// Notes:
//   - ``layer_infos`` skips entries with null ``LayerInfoObj`` — those are placeholder entries the
//     editor adds for material slots that haven't been bound to a layer info asset yet, and their
//     weights cannot be sampled via FLandscapeEditDataInterface.
//   - Layer type is determined by ``UE::Landscape::IsVisibilityLayer`` — landscapes can have at most
//     one visibility layer (used as a hole-mask via LANDSCAPE_VISIBILITY_THRESHOLD).
//   - min_z / max_z come from the aggregate components bounding box. For empty / unloaded landscapes
//     these will both be 0 (FBox::IsValid=false case). Use total_components=0 as the indicator.
FMCPResponse Tool_GetInfo(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString LandscapePath;
	FMCPResponse Err;
	if (!LAND_RequireStringField(Request, TEXT("landscape_path"), LandscapePath, Err)) { return Err; }

	int32 ResolveErrCode = 0;
	FString ResolveErr;
	ALandscape* LS = LAND_ResolveLandscape(LandscapePath, ResolveErrCode, ResolveErr);
	if (!LS) { return LAND_MakeError(Request, ResolveErrCode, ResolveErr); }

	ULandscapeInfo* LSI = LS->GetLandscapeInfo();
	if (!LSI)
	{
		return LAND_MakeError(Request, kLANDErrorInternal,
			FString::Printf(
				TEXT("landscape '%s' has no ULandscapeInfo (un-registered or mid-load proxy)"),
				*LandscapePath));
	}

	TArray<TSharedPtr<FJsonValue>> LayerArr;
	for (const FLandscapeInfoLayerSettings& LayerSettings : LSI->Layers)
	{
		// Skip placeholder entries with no backing LayerInfoObj — these can't be sampled and have
		// no meaningful type. The placeholder LayerName field IS populated, but exposing them would
		// mislead callers into thinking they can query weights.
		ULandscapeLayerInfoObject* LayerInfoObj = LayerSettings.LayerInfoObj;
		if (!LayerInfoObj) { continue; }

		TSharedRef<FJsonObject> LayerObj = MakeShared<FJsonObject>();
		LayerObj->SetStringField(TEXT("name"), LayerInfoObj->GetLayerName().ToString());
		LayerObj->SetStringField(TEXT("type"),
			UE::Landscape::IsVisibilityLayer(LayerInfoObj) ? TEXT("Visibility") : TEXT("Weight"));
		LayerArr.Add(MakeShared<FJsonValueObject>(LayerObj));
	}

	const FBox Bounds = LS->GetComponentsBoundingBox(/*bNonColliding*/ true);
	const double MinZ = Bounds.IsValid ? Bounds.Min.Z : 0.0;
	const double MaxZ = Bounds.IsValid ? Bounds.Max.Z : 0.0;

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetNumberField(TEXT("component_size_quads"), static_cast<double>(LSI->ComponentSizeQuads));
	Out->SetNumberField(TEXT("num_subsections"),      static_cast<double>(LSI->ComponentNumSubsections));
	Out->SetArrayField(TEXT("layer_infos"),           LayerArr);
	Out->SetNumberField(TEXT("min_z"),                MinZ);
	Out->SetNumberField(TEXT("max_z"),                MaxZ);
	Out->SetNumberField(TEXT("total_components"),     static_cast<double>(LS->LandscapeComponents.Num()));
	return LAND_MakeSuccessObj(Request, Out);
}

// ─── landscape.get_height_at ─────────────────────────────────────────────────────────────────────
//
// Args:
//   landscape_path : string    required
//   world_x        : float     required
//   world_y        : float     required
//
// Result:
//   {
//     height_z : float,        // world-space Z at the queried XY (0 when has_data=false)
//     has_data : bool,         // true when the XY falls inside the landscape's quad extent AND
//                              // FLandscapeEditDataInterface returned a heightmap sample
//     world_xy : [x, y]        // echo of the input XY (convenience for caller batches)
//   }
//
// Errors:
//   -32004 ObjectNotFound       landscape not resolvable
//   -32010 InvalidPath          empty landscape_path
//   -32011 WrongClass           actor isn't ALandscape
//   -32602 InvalidParams        missing world_x / world_y / landscape_path
//   -32603 Internal             no ULandscapeInfo
//
// Notes:
//   - Uses the SPARSE (TMap) variant of ``GetHeightData`` — it skips quads with no component, so
//     ``Heights.Num() == 0`` after the call cleanly signals "out of landscape coverage" even when
//     the quad index passed the extent check (sparse landscape with gaps).
//   - Height conversion: ``LandscapeDataAccess::GetLocalHeight(uint16)`` returns local-Z in the
//     landscape's actor space (centered around 0). Multiply by actor scale Z and add actor world Z
//     to get the world-space height.
FMCPResponse Tool_GetHeightAt(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString LandscapePath;
	FMCPResponse Err;
	if (!LAND_RequireStringField(Request, TEXT("landscape_path"), LandscapePath, Err)) { return Err; }

	double WorldX = 0.0, WorldY = 0.0;
	if (!LAND_RequireNumberField(Request, TEXT("world_x"), WorldX, Err)) { return Err; }
	if (!LAND_RequireNumberField(Request, TEXT("world_y"), WorldY, Err)) { return Err; }

	int32 ResolveErrCode = 0;
	FString ResolveErr;
	ALandscape* LS = LAND_ResolveLandscape(LandscapePath, ResolveErrCode, ResolveErr);
	if (!LS) { return LAND_MakeError(Request, ResolveErrCode, ResolveErr); }

	ULandscapeInfo* LSI = LS->GetLandscapeInfo();
	if (!LSI)
	{
		return LAND_MakeError(Request, kLANDErrorInternal,
			FString::Printf(
				TEXT("landscape '%s' has no ULandscapeInfo (un-registered or mid-load proxy)"),
				*LandscapePath));
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> WorldXYArr;
	WorldXYArr.Add(MakeShared<FJsonValueNumber>(WorldX));
	WorldXYArr.Add(MakeShared<FJsonValueNumber>(WorldY));
	Out->SetArrayField(TEXT("world_xy"), WorldXYArr);

	const FIntPoint Quad = LAND_WorldXYToQuad(LS, WorldX, WorldY);

	if (!LAND_IsQuadInBounds(LSI, Quad.X, Quad.Y))
	{
		Out->SetNumberField(TEXT("height_z"), 0.0);
		Out->SetBoolField(TEXT("has_data"),   false);
		return LAND_MakeSuccessObj(Request, Out);
	}

	// Sample a single-quad region via the sparse TMap variant. The Get* call may CLAMP the requested
	// region to the loaded extent — capture mutable copies so we can read back the actual range.
	int32 X1 = Quad.X, Y1 = Quad.Y, X2 = Quad.X, Y2 = Quad.Y;
	TMap<FIntPoint, uint16> Heights;
	FLandscapeEditDataInterface LandscapeEdit(LSI, /*bInUploadTextureChangesToGPU*/ false);
	LandscapeEdit.GetHeightData(X1, Y1, X2, Y2, Heights);

	const uint16* HeightPtr = Heights.Find(FIntPoint(Quad.X, Quad.Y));
	if (!HeightPtr)
	{
		// Quad was inside extent but no component data — typical for streaming-style landscapes with
		// unloaded tiles in the queried region.
		Out->SetNumberField(TEXT("height_z"), 0.0);
		Out->SetBoolField(TEXT("has_data"),   false);
		return LAND_MakeSuccessObj(Request, Out);
	}

	// Convert local-Z (centered around 0 in landscape actor space) → world-Z. Apply landscape's
	// canonical actor transform's scale.Z and translation.Z. Matches the engine's
	// ``Component->GetComponentToWorld().TransformPositionNoScale(FVector(0,0,LocalHeight))`` path
	// used in ALandscapeProxy::GetHeightAtLocation, modulo the proxy SectionBase nuance.
	const FTransform LSXform = LS->LandscapeActorToWorld();
	const FVector ActorScale = LSXform.GetScale3D();
	const FVector ActorLoc   = LSXform.GetLocation();
	const float   LocalZ     = LandscapeDataAccess::GetLocalHeight(*HeightPtr);
	const double  WorldZ     = static_cast<double>(LocalZ) * ActorScale.Z + ActorLoc.Z;

	Out->SetNumberField(TEXT("height_z"), WorldZ);
	Out->SetBoolField(TEXT("has_data"),   true);
	return LAND_MakeSuccessObj(Request, Out);
}

// ─── landscape.get_layer_weights ─────────────────────────────────────────────────────────────────
//
// Args:
//   landscape_path : string    required
//   world_x        : float     required
//   world_y        : float     required
//
// Result:
//   {
//     weights  : { "<layer_name>": float (0..1), ... },   // empty object when has_data=false
//     has_data : bool                                     // true iff at least one layer sample was returned
//   }
//
// Errors: same surface as ``landscape.get_height_at``.
//
// Notes:
//   - Iterates ``ULandscapeInfo::Layers`` calling ``GetWeightData`` (sparse TMap variant) per layer
//     with a single-quad region. Layers without a ``LayerInfoObj`` are skipped (no backing weight
//     texture to read).
//   - Per-layer weights are uint8 [0,255]; reported as float [0,1] = byte/255. Multiple layers can
//     report >0 weight for the same quad (blending), summing close to 1.0 for properly normalised
//     paint data.
//   - ``has_data`` is the OR of "at least one layer had data" — a quad inside extent but in an
//     unloaded tile returns has_data=false with empty weights.
FMCPResponse Tool_GetLayerWeights(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString LandscapePath;
	FMCPResponse Err;
	if (!LAND_RequireStringField(Request, TEXT("landscape_path"), LandscapePath, Err)) { return Err; }

	double WorldX = 0.0, WorldY = 0.0;
	if (!LAND_RequireNumberField(Request, TEXT("world_x"), WorldX, Err)) { return Err; }
	if (!LAND_RequireNumberField(Request, TEXT("world_y"), WorldY, Err)) { return Err; }

	int32 ResolveErrCode = 0;
	FString ResolveErr;
	ALandscape* LS = LAND_ResolveLandscape(LandscapePath, ResolveErrCode, ResolveErr);
	if (!LS) { return LAND_MakeError(Request, ResolveErrCode, ResolveErr); }

	ULandscapeInfo* LSI = LS->GetLandscapeInfo();
	if (!LSI)
	{
		return LAND_MakeError(Request, kLANDErrorInternal,
			FString::Printf(
				TEXT("landscape '%s' has no ULandscapeInfo (un-registered or mid-load proxy)"),
				*LandscapePath));
	}

	TSharedRef<FJsonObject> WeightsObj = MakeShared<FJsonObject>();
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();

	const FIntPoint Quad = LAND_WorldXYToQuad(LS, WorldX, WorldY);

	if (!LAND_IsQuadInBounds(LSI, Quad.X, Quad.Y))
	{
		Out->SetObjectField(TEXT("weights"), WeightsObj);
		Out->SetBoolField(TEXT("has_data"),  false);
		return LAND_MakeSuccessObj(Request, Out);
	}

	FLandscapeEditDataInterface LandscapeEdit(LSI, /*bInUploadTextureChangesToGPU*/ false);
	bool bAnyData = false;

	for (const FLandscapeInfoLayerSettings& LayerSettings : LSI->Layers)
	{
		ULandscapeLayerInfoObject* LayerInfoObj = LayerSettings.LayerInfoObj;
		if (!LayerInfoObj) { continue; }

		int32 X1 = Quad.X, Y1 = Quad.Y, X2 = Quad.X, Y2 = Quad.Y;
		TMap<FIntPoint, uint8> Weights;
		LandscapeEdit.GetWeightData(LayerInfoObj, X1, Y1, X2, Y2, Weights);

		const uint8* WeightPtr = Weights.Find(FIntPoint(Quad.X, Quad.Y));
		if (!WeightPtr) { continue; }

		bAnyData = true;
		const double Normalised = static_cast<double>(*WeightPtr) / 255.0;
		WeightsObj->SetNumberField(LayerInfoObj->GetLayerName().ToString(), Normalised);
	}

	Out->SetObjectField(TEXT("weights"), WeightsObj);
	Out->SetBoolField(TEXT("has_data"),  bAnyData);
	return LAND_MakeSuccessObj(Request, Out);
}

// ─── Registration ────────────────────────────────────────────────────────────────────────────────
void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	RegisterTool(TEXT("landscape.list"),              &Tool_List,             /*Lane A*/ false);
	RegisterTool(TEXT("landscape.get_info"),          &Tool_GetInfo,          /*Lane A*/ false);
	RegisterTool(TEXT("landscape.get_height_at"),     &Tool_GetHeightAt,      /*Lane A*/ false);
	RegisterTool(TEXT("landscape.get_layer_weights"), &Tool_GetLayerWeights,  /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("Wave I Surface 6 (Landscape): registered 4 landscape.* handlers "
			 "(list / get_info / get_height_at / get_layer_weights, all Lane A, all read-only)"));
}

} // namespace FLandscapeTools

#include "MCPSurfaceRegistry.h"
MCP_REGISTER_SURFACE(LandscapeTools, &FLandscapeTools::Register)

#undef LOCTEXT_NAMESPACE
