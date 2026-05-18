// Copyright FatumGame. All Rights Reserved.

#include "PhysicsTools.h"

#include "FMCPDispatchQueue.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPActorPathUtils.h"
#include "Utils/MCPWorldContext.h"

#include "CollisionQueryParams.h"
#include "CollisionShape.h"
#include "Components/PrimitiveComponent.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/HitResult.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "UObject/Package.h"
#include "WorldCollision.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// PHY_ prefix per the unity-build symbol-collision pattern.
	constexpr int32 kPHYErrorInvalidParams = -32602;
	constexpr int32 kPHYErrorInternal      = -32603;

	void PHY_StampIds(const FMCPRequest& Request, FMCPResponse& Response)
	{
		Response.RequestId = Request.RequestId;
		Response.OriginalIdString = Request.OriginalIdString;
	}

	FMCPResponse PHY_MakeError(const FMCPRequest& Request, int32 Code, const FString& Message)
	{
		FMCPResponse R;
		PHY_StampIds(Request, R);
		R.bIsError = true;
		R.ErrorCode = Code;
		R.ErrorMessage = Message;
		return R;
	}

	FMCPResponse PHY_MakeSuccessObj(const FMCPRequest& Request, TSharedPtr<FJsonObject> Result)
	{
		FMCPResponse R;
		PHY_StampIds(Request, R);
		R.bIsError = false;
		R.Result = MakeShared<FJsonValueObject>(MoveTemp(Result));
		return R;
	}

	// ─── arg parsing helpers ─────────────────────────────────────────────────────────────────────

	bool PHY_ReadVectorArray(
		const TSharedPtr<FJsonObject>& Args,
		const TCHAR* FieldName,
		FVector& OutV,
		FString& OutError)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Args.IsValid() || !Args->TryGetArrayField(FieldName, Arr) || !Arr)
		{
			OutError = FString::Printf(TEXT("missing required array field '%s'"), FieldName);
			return false;
		}
		if (Arr->Num() != 3)
		{
			OutError = FString::Printf(
				TEXT("'%s' must be [x,y,z] (3 numbers); got %d entries"), FieldName, Arr->Num());
			return false;
		}
		double X = 0.0, Y = 0.0, Z = 0.0;
		if (!(*Arr)[0]->TryGetNumber(X)
			|| !(*Arr)[1]->TryGetNumber(Y)
			|| !(*Arr)[2]->TryGetNumber(Z))
		{
			OutError = FString::Printf(TEXT("'%s' entries must all be numbers"), FieldName);
			return false;
		}
		OutV = FVector(X, Y, Z);
		return true;
	}

	bool PHY_ReadOptionalVectorArray(
		const TSharedPtr<FJsonObject>& Args,
		const TCHAR* FieldName,
		const FVector& DefaultValue,
		FVector& OutV,
		FString& OutError)
	{
		// Not present → default. Present-but-malformed → error.
		if (!Args.IsValid() || !Args->HasField(FieldName))
		{
			OutV = DefaultValue;
			return true;
		}
		return PHY_ReadVectorArray(Args, FieldName, OutV, OutError);
	}

	bool PHY_ReadClampedNumber(
		const TSharedPtr<FJsonObject>& Args,
		const TCHAR* FieldName,
		double Min,
		double Max,
		double& OutValue,
		FString& OutError)
	{
		if (!Args.IsValid() || !Args->TryGetNumberField(FieldName, OutValue))
		{
			OutError = FString::Printf(TEXT("missing required number field '%s'"), FieldName);
			return false;
		}
		if (OutValue < Min || OutValue > Max)
		{
			OutError = FString::Printf(
				TEXT("'%s' %g out of range [%g, %g]"), FieldName, OutValue, Min, Max);
			return false;
		}
		return true;
	}

	// ─── World resolution ───────────────────────────────────────────────────────────────────────

	/**
	 * Resolve the world to trace against. PIE first (matches "trace the world the user is
	 * watching"), editor world fallback. Returns null only when GEditor itself is missing
	 * (commandlet / cooker) — caller surfaces -32603.
	 */
	UWorld* PHY_ResolveTraceWorld()
	{
		check(IsInGameThread());
		if (GEditor && GEditor->PlayWorld)
		{
			return GEditor->PlayWorld;
		}
		return FMCPWorldContext::GetEditorWorld();
	}

	/** "PIE" or "Editor" — for telemetry logging only. */
	const TCHAR* PHY_DescribeWorldKind(const UWorld* World)
	{
		if (!World) { return TEXT("None"); }
		switch (World->WorldType)
		{
		case EWorldType::Editor:    return TEXT("Editor");
		case EWorldType::PIE:       return TEXT("PIE");
		case EWorldType::Game:      return TEXT("Game");
		case EWorldType::EditorPreview: return TEXT("EditorPreview");
		case EWorldType::GamePreview:   return TEXT("GamePreview");
		default:                    return TEXT("Other");
		}
	}

	// ─── Collision channel parsing ───────────────────────────────────────────────────────────────

	/** Canonical accepted channel names — kept in sync with the message in -32041 errors. */
	const TCHAR* const PHY_AcceptedChannelNames =
		TEXT("Visibility, Camera, WorldStatic, WorldDynamic, Pawn, PhysicsBody, Vehicle, Destructible");

	bool PHY_ParseCollisionChannel(const FString& ChannelStr, ECollisionChannel& OutChannel)
	{
		// Case-insensitive match. Default Visibility when empty.
		if (ChannelStr.IsEmpty() || ChannelStr.Equals(TEXT("Visibility"), ESearchCase::IgnoreCase))
		{
			OutChannel = ECC_Visibility;
			return true;
		}
		if (ChannelStr.Equals(TEXT("Camera"), ESearchCase::IgnoreCase))
		{
			OutChannel = ECC_Camera;
			return true;
		}
		if (ChannelStr.Equals(TEXT("WorldStatic"), ESearchCase::IgnoreCase))
		{
			OutChannel = ECC_WorldStatic;
			return true;
		}
		if (ChannelStr.Equals(TEXT("WorldDynamic"), ESearchCase::IgnoreCase))
		{
			OutChannel = ECC_WorldDynamic;
			return true;
		}
		if (ChannelStr.Equals(TEXT("Pawn"), ESearchCase::IgnoreCase))
		{
			OutChannel = ECC_Pawn;
			return true;
		}
		if (ChannelStr.Equals(TEXT("PhysicsBody"), ESearchCase::IgnoreCase))
		{
			OutChannel = ECC_PhysicsBody;
			return true;
		}
		if (ChannelStr.Equals(TEXT("Vehicle"), ESearchCase::IgnoreCase))
		{
			OutChannel = ECC_Vehicle;
			return true;
		}
		if (ChannelStr.Equals(TEXT("Destructible"), ESearchCase::IgnoreCase))
		{
			OutChannel = ECC_Destructible;
			return true;
		}
		return false;
	}

	// ─── Ignore-actors helper ────────────────────────────────────────────────────────────────────

	/**
	 * Resolve each entry in the ``ignore_actors`` JSON array via FMCPActorPathUtils and add to
	 * ``Params.AddIgnoredActor``. Unresolved entries are SKIPPED with a Verbose log — partial
	 * resolution doesn't fail the trace (matches the "best-effort filter" convention). Returns
	 * the count successfully ignored.
	 */
	int32 PHY_AddIgnoredActors(
		const TSharedPtr<FJsonObject>& Args,
		FCollisionQueryParams& Params)
	{
		if (!Args.IsValid()) { return 0; }
		const TArray<TSharedPtr<FJsonValue>>* IgnoreArr = nullptr;
		if (!Args->TryGetArrayField(TEXT("ignore_actors"), IgnoreArr) || !IgnoreArr)
		{
			return 0;
		}
		int32 IgnoredCount = 0;
		for (const TSharedPtr<FJsonValue>& Val : *IgnoreArr)
		{
			FString IdStr;
			if (!Val.IsValid() || !Val->TryGetString(IdStr) || IdStr.IsEmpty())
			{
				UE_LOG(LogMCP, Verbose, TEXT("physics trace: ignore_actors entry empty/non-string, skipping"));
				continue;
			}
			bool bAmbig = false;
			FString AmbigHint, ResolveErr;
			AActor* Actor = FMCPActorPathUtils::ResolveActor(
				IdStr, /*bRejectPIE*/ false, bAmbig, AmbigHint, ResolveErr);
			if (Actor)
			{
				Params.AddIgnoredActor(Actor);
				++IgnoredCount;
			}
			else
			{
				UE_LOG(LogMCP, Verbose,
					TEXT("physics trace: ignore_actor '%s' did not resolve (%s); skipping"),
					*IdStr, *ResolveErr);
			}
		}
		return IgnoredCount;
	}

	// ─── FHitResult → JSON ───────────────────────────────────────────────────────────────────────

	TArray<TSharedPtr<FJsonValue>> PHY_VectorToArray(const FVector& V)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Add(MakeShared<FJsonValueNumber>(V.X));
		Arr.Add(MakeShared<FJsonValueNumber>(V.Y));
		Arr.Add(MakeShared<FJsonValueNumber>(V.Z));
		return Arr;
	}

	TSharedRef<FJsonObject> PHY_BuildHitJson(const FHitResult& Hit)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();

		// Actor identity — emit both guid (may be empty for spawned PIE actors) and canonical path.
		AActor* Actor = Hit.GetActor();
		if (Actor)
		{
			const FGuid Guid = Actor->GetActorGuid();
			Obj->SetStringField(TEXT("actor_guid"),
				Guid.IsValid() ? Guid.ToString(EGuidFormats::DigitsWithHyphens) : FString());
			Obj->SetStringField(TEXT("actor_path"),
				FMCPActorPathUtils::BuildActorPath(Actor));
		}
		else
		{
			Obj->SetField(TEXT("actor_guid"), MakeShared<FJsonValueNull>());
			Obj->SetField(TEXT("actor_path"), MakeShared<FJsonValueNull>());
		}

		// Component name (not full path; the actor_path is the full handle).
		UPrimitiveComponent* Comp = Hit.GetComponent();
		if (Comp)
		{
			Obj->SetStringField(TEXT("component"), Comp->GetName());
		}
		else
		{
			Obj->SetField(TEXT("component"), MakeShared<FJsonValueNull>());
		}

		Obj->SetArrayField(TEXT("location"), PHY_VectorToArray(Hit.ImpactPoint));
		Obj->SetArrayField(TEXT("normal"),   PHY_VectorToArray(Hit.ImpactNormal));
		Obj->SetNumberField(TEXT("distance"), static_cast<double>(Hit.Distance));

		// Physical material (requires bReturnPhysicalMaterial on the trace params).
		if (UPhysicalMaterial* PhysMat = Hit.PhysMaterial.Get())
		{
			Obj->SetStringField(TEXT("phys_mat"), PhysMat->GetPathName());
		}
		else
		{
			Obj->SetField(TEXT("phys_mat"), MakeShared<FJsonValueNull>());
		}

		// Bone (skeletal mesh) — non-empty only on skel meshes.
		Obj->SetStringField(TEXT("bone_name"), Hit.BoneName.IsNone() ? FString() : Hit.BoneName.ToString());

		// Blocking flag — useful when multi_hit=true caller wants to know which entry was the
		// "blocking hit" vs an "overlap" hit.
		Obj->SetBoolField(TEXT("blocking"), Hit.bBlockingHit);

		return Obj;
	}

	/**
	 * Convert TArray<FHitResult> → JSON array, applying the multi-hit filter:
	 *   - multi_hit=true  → emit ALL entries (caller wants the full chain through overlaps)
	 *   - multi_hit=false → emit only the first BLOCKING hit (single-hit semantics)
	 *
	 * Returns the JSON array AND the bool ``hit`` (true iff at least one blocking hit present in
	 * the input regardless of multi_hit).
	 */
	void PHY_BuildHitsArray(
		const TArray<FHitResult>& InHits,
		bool bMultiHit,
		TArray<TSharedPtr<FJsonValue>>& OutArr,
		bool& bOutAnyBlocking)
	{
		bOutAnyBlocking = false;
		for (const FHitResult& H : InHits)
		{
			if (H.bBlockingHit) { bOutAnyBlocking = true; }
		}

		if (bMultiHit)
		{
			OutArr.Reserve(InHits.Num());
			for (const FHitResult& H : InHits)
			{
				OutArr.Add(MakeShared<FJsonValueObject>(PHY_BuildHitJson(H)));
			}
			return;
		}

		// Single-hit: first blocking hit only.
		for (const FHitResult& H : InHits)
		{
			if (H.bBlockingHit)
			{
				OutArr.Add(MakeShared<FJsonValueObject>(PHY_BuildHitJson(H)));
				return;
			}
		}
	}
} // namespace

namespace FPhysicsTools
{

// ─── physics.line_trace ────────────────────────────────────────────────────────────────────────
//
// Args:
//   start          : [x, y, z]                          required
//   end            : [x, y, z]                          required
//   channel        : string (default "Visibility")
//   ignore_actors  : array<string> (default [])
//   multi_hit      : bool (default false)
//
// Result:
//   {
//     world: string,                                    // package path of the world traced
//     world_kind: "Editor"|"PIE"|...,
//     ignored_count: int,                               // how many ignore_actors resolved
//     hit: bool,                                        // any blocking hit
//     hits: [
//       { actor_guid, actor_path, component, location, normal, distance,
//         phys_mat, bone_name, blocking }, ...
//     ]
//   }
//
// Errors:
//   -32602 InvalidParams              missing start/end / malformed
//   -32041 InvalidCollisionChannel    channel string unknown
//   -32603 Internal                   no world (commandlet / no level loaded)
//
// **Important for FatumGame:** This is the UE Chaos physics system — does NOT hit Jolt/Barrage
// bodies. Flecs/Barrage entities (projectiles, debris, ISM-rendered items) are invisible to this
// trace. Future flecs.* trace tools will be needed for Jolt queries.
FMCPResponse Tool_LineTrace(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!Request.Args.IsValid())
	{
		return PHY_MakeError(Request, kPHYErrorInvalidParams, TEXT("missing args object"));
	}

	FVector Start, End;
	FString ArgErr;
	if (!PHY_ReadVectorArray(Request.Args, TEXT("start"), Start, ArgErr))
	{
		return PHY_MakeError(Request, kPHYErrorInvalidParams, ArgErr);
	}
	if (!PHY_ReadVectorArray(Request.Args, TEXT("end"), End, ArgErr))
	{
		return PHY_MakeError(Request, kPHYErrorInvalidParams, ArgErr);
	}

	FString ChannelStr;
	Request.Args->TryGetStringField(TEXT("channel"), ChannelStr);
	ECollisionChannel Channel = ECC_Visibility;
	if (!PHY_ParseCollisionChannel(ChannelStr, Channel))
	{
		return PHY_MakeError(Request, kMCPErrorInvalidCollisionChannel,
			FString::Printf(TEXT("channel '%s' not recognised; accepted: %s"),
				*ChannelStr, PHY_AcceptedChannelNames));
	}

	bool bMultiHit = false;
	Request.Args->TryGetBoolField(TEXT("multi_hit"), bMultiHit);

	UWorld* World = PHY_ResolveTraceWorld();
	if (!World)
	{
		return PHY_MakeError(Request, kPHYErrorInternal,
			TEXT("no world available for trace (GEditor missing OR no level loaded)"));
	}

	FCollisionQueryParams Params(SCENE_QUERY_STAT(MCPLineTrace), /*bTraceComplex*/ false);
	Params.bReturnPhysicalMaterial = true;
	const int32 IgnoredCount = PHY_AddIgnoredActors(Request.Args, Params);

	TArray<FHitResult> Hits;
	const bool bAnyHit = World->LineTraceMultiByChannel(Hits, Start, End, Channel, Params);
	(void)bAnyHit; // We rely on per-hit bBlockingHit, not the bool return (overlap hits don't set it).

	TArray<TSharedPtr<FJsonValue>> HitsArr;
	bool bAnyBlocking = false;
	PHY_BuildHitsArray(Hits, bMultiHit, HitsArr, bAnyBlocking);

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("world"), World->GetOutermost() ? World->GetOutermost()->GetName() : FString());
	Out->SetStringField(TEXT("world_kind"), PHY_DescribeWorldKind(World));
	Out->SetNumberField(TEXT("ignored_count"), IgnoredCount);
	Out->SetBoolField(TEXT("hit"), bAnyBlocking);
	Out->SetArrayField(TEXT("hits"), HitsArr);
	return PHY_MakeSuccessObj(Request, Out);
}

// ─── physics.sweep_capsule ─────────────────────────────────────────────────────────────────────
//
// Args:
//   start         : [x, y, z]                          required
//   end           : [x, y, z]                          required
//   radius        : number > 0                         required
//   half_height   : number > 0                         required
//   rotation      : [pitch, yaw, roll] (default [0,0,0])
//   channel       : string (default "Visibility")
//   ignore_actors : array<string> (default [])
//   multi_hit     : bool (default false)
//
// Result: same shape as physics.line_trace + the input shape echoed back for clarity.
//
// Errors:
//   -32602 InvalidParams              missing start/end OR radius/half_height not present
//   -32041 InvalidCollisionChannel    channel string unknown
//   -32603 Internal                   no world OR radius/half_height non-positive after read
//
// Capsule shape: FCollisionShape::MakeCapsule(radius, half_height). Rotation is applied as an
// FQuat-from-FRotator (pitch, yaw, roll). For a sphere sweep, pass half_height==radius.
FMCPResponse Tool_SweepCapsule(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!Request.Args.IsValid())
	{
		return PHY_MakeError(Request, kPHYErrorInvalidParams, TEXT("missing args object"));
	}

	FVector Start, End;
	FString ArgErr;
	if (!PHY_ReadVectorArray(Request.Args, TEXT("start"), Start, ArgErr))
	{
		return PHY_MakeError(Request, kPHYErrorInvalidParams, ArgErr);
	}
	if (!PHY_ReadVectorArray(Request.Args, TEXT("end"), End, ArgErr))
	{
		return PHY_MakeError(Request, kPHYErrorInvalidParams, ArgErr);
	}

	// radius / half_height must be >= 1.0 per plan §C-Physics inputSchema "minimum: 1.0".
	double Radius = 0.0;
	if (!PHY_ReadClampedNumber(Request.Args, TEXT("radius"), 1.0, 100000.0, Radius, ArgErr))
	{
		return PHY_MakeError(Request, kPHYErrorInvalidParams, ArgErr);
	}
	double HalfHeight = 0.0;
	if (!PHY_ReadClampedNumber(Request.Args, TEXT("half_height"), 1.0, 100000.0, HalfHeight, ArgErr))
	{
		return PHY_MakeError(Request, kPHYErrorInvalidParams, ArgErr);
	}

	FVector RotationVec = FVector::ZeroVector;
	if (!PHY_ReadOptionalVectorArray(Request.Args, TEXT("rotation"), FVector::ZeroVector, RotationVec, ArgErr))
	{
		return PHY_MakeError(Request, kPHYErrorInvalidParams, ArgErr);
	}
	const FRotator Rotation(RotationVec.X, RotationVec.Y, RotationVec.Z); // pitch, yaw, roll
	const FQuat RotationQuat = Rotation.Quaternion();

	FString ChannelStr;
	Request.Args->TryGetStringField(TEXT("channel"), ChannelStr);
	ECollisionChannel Channel = ECC_Visibility;
	if (!PHY_ParseCollisionChannel(ChannelStr, Channel))
	{
		return PHY_MakeError(Request, kMCPErrorInvalidCollisionChannel,
			FString::Printf(TEXT("channel '%s' not recognised; accepted: %s"),
				*ChannelStr, PHY_AcceptedChannelNames));
	}

	bool bMultiHit = false;
	Request.Args->TryGetBoolField(TEXT("multi_hit"), bMultiHit);

	UWorld* World = PHY_ResolveTraceWorld();
	if (!World)
	{
		return PHY_MakeError(Request, kPHYErrorInternal,
			TEXT("no world available for sweep (GEditor missing OR no level loaded)"));
	}

	FCollisionQueryParams Params(SCENE_QUERY_STAT(MCPSweepCapsule), /*bTraceComplex*/ false);
	Params.bReturnPhysicalMaterial = true;
	const int32 IgnoredCount = PHY_AddIgnoredActors(Request.Args, Params);

	const FCollisionShape Shape = FCollisionShape::MakeCapsule(
		static_cast<float>(Radius), static_cast<float>(HalfHeight));

	TArray<FHitResult> Hits;
	const bool bAnyHit = World->SweepMultiByChannel(
		Hits, Start, End, RotationQuat, Channel, Shape, Params);
	(void)bAnyHit;

	TArray<TSharedPtr<FJsonValue>> HitsArr;
	bool bAnyBlocking = false;
	PHY_BuildHitsArray(Hits, bMultiHit, HitsArr, bAnyBlocking);

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("world"), World->GetOutermost() ? World->GetOutermost()->GetName() : FString());
	Out->SetStringField(TEXT("world_kind"), PHY_DescribeWorldKind(World));
	Out->SetNumberField(TEXT("ignored_count"), IgnoredCount);
	Out->SetBoolField(TEXT("hit"), bAnyBlocking);
	Out->SetArrayField(TEXT("hits"), HitsArr);
	return PHY_MakeSuccessObj(Request, Out);
}

// ─── Registration ──────────────────────────────────────────────────────────────────────────────
void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	RegisterTool(TEXT("physics.line_trace"),    &Tool_LineTrace,    /*Lane A*/ false);
	RegisterTool(TEXT("physics.sweep_capsule"), &Tool_SweepCapsule, /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("Phase 5 Chunk C (Physics): registered 2 physics.* handlers (line_trace + sweep_capsule, all Lane A)"));
}

} // namespace FPhysicsTools

#undef LOCTEXT_NAMESPACE
