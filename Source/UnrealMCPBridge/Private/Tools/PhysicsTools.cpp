// Copyright FatumGame. All Rights Reserved.

#include "PhysicsTools.h"

#include "MCPSurfaceRegistry.h"

#include "FMCPDispatchQueue.h"
#include "MCPToolHelpers.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPActorPathUtils.h"
#include "Utils/MCPComponentPathUtils.h"
#include "Utils/MCPWorldContext.h"

#include "CollisionQueryParams.h"
#include "CollisionShape.h"
#include "Components/ActorComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/HitResult.h"
#include "Engine/OverlapResult.h"
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
	// PHY_ prefix per the unity-build symbol-collision pattern. StampIds / MakeError / MakeSuccessObj
	// migrated to FMCPToolHelpers in Phase 3 (Group G3); only the surface-local error-code aliases
	// live here. physics.* tools are deliberately PIE-friendly (line_trace/sweep_capsule/overlap_test
	// are read-only; apply_impulse/set_simulation/set_velocity are runtime mutators that target the
	// PIE world specifically) — no FMCPMutatorScope migration is required or wanted here.
	constexpr int32 kPHYErrorInvalidParams    = kMCPErrorInvalidParams;   // -32602
	constexpr int32 kPHYErrorInternal         = kMCPErrorInternal;        // -32603
	constexpr int32 kPHYErrorObjectNotFound   = kMCPErrorObjectNotFound;  // -32004
	constexpr int32 kPHYErrorWrongClass       = kMCPErrorWrongClass;      // -32011

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

	// ─── Component resolution (Wave G S1 writes) ────────────────────────────────────────────────

	/**
	 * Locate the target UPrimitiveComponent on an actor.
	 *
	 *   - Empty ``CompName``  → return Actor->GetRootComponent() cast to UPrimitiveComponent.
	 *     Returns null if the root is non-primitive (e.g. USceneComponent on a billboard actor).
	 *   - Non-empty           → iterate ``GetComponents(UPrimitiveComponent::StaticClass())`` and
	 *     match against ``UActorComponent::GetName()`` (the internal FName, matches the wire
	 *     ``component_name`` convention used elsewhere in MCP). Returns null on miss.
	 *
	 * ``bOutWrongClass`` is set true iff the supplied component name DID resolve to a UActorComponent
	 * but the cast to UPrimitiveComponent failed (e.g. a USceneComponent or UMovementComponent name
	 * was passed). Callers route this to -32011 WrongClass; a true "not found" leaves the flag false
	 * and is routed to -32004 ObjectNotFound.
	 */
	UPrimitiveComponent* PHY_GetTargetPrimitive(AActor* Actor, const FString& CompName, bool& bOutWrongClass)
	{
		check(Actor);
		bOutWrongClass = false;
		if (CompName.IsEmpty())
		{
			return Cast<UPrimitiveComponent>(Actor->GetRootComponent());
		}
		// Look across ALL UActorComponents so we can distinguish "name found but not primitive"
		// (-> kMCPErrorWrongClass) from "name not found at all" (-> kMCPErrorObjectNotFound).
		TArray<UActorComponent*> Components;
		Actor->GetComponents(UActorComponent::StaticClass(), Components);
		for (UActorComponent* C : Components)
		{
			if (!C) { continue; }
			if (C->GetName() == CompName)
			{
				if (UPrimitiveComponent* P = Cast<UPrimitiveComponent>(C)) { return P; }
				bOutWrongClass = true;
				return nullptr;
			}
		}
		return nullptr;
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
		return FMCPToolHelpers::MakeError(Request, kPHYErrorInvalidParams, TEXT("missing args object"));
	}

	FVector Start, End;
	FString ArgErr;
	if (!PHY_ReadVectorArray(Request.Args, TEXT("start"), Start, ArgErr))
	{
		return FMCPToolHelpers::MakeError(Request, kPHYErrorInvalidParams, ArgErr);
	}
	if (!PHY_ReadVectorArray(Request.Args, TEXT("end"), End, ArgErr))
	{
		return FMCPToolHelpers::MakeError(Request, kPHYErrorInvalidParams, ArgErr);
	}

	FString ChannelStr;
	Request.Args->TryGetStringField(TEXT("channel"), ChannelStr);
	ECollisionChannel Channel = ECC_Visibility;
	if (!PHY_ParseCollisionChannel(ChannelStr, Channel))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidCollisionChannel,
			FString::Printf(TEXT("channel '%s' not recognised; accepted: %s"),
				*ChannelStr, PHY_AcceptedChannelNames));
	}

	bool bMultiHit = false;
	Request.Args->TryGetBoolField(TEXT("multi_hit"), bMultiHit);

	UWorld* World = PHY_ResolveTraceWorld();
	if (!World)
	{
		return FMCPToolHelpers::MakeError(Request, kPHYErrorInternal,
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
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
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
		return FMCPToolHelpers::MakeError(Request, kPHYErrorInvalidParams, TEXT("missing args object"));
	}

	FVector Start, End;
	FString ArgErr;
	if (!PHY_ReadVectorArray(Request.Args, TEXT("start"), Start, ArgErr))
	{
		return FMCPToolHelpers::MakeError(Request, kPHYErrorInvalidParams, ArgErr);
	}
	if (!PHY_ReadVectorArray(Request.Args, TEXT("end"), End, ArgErr))
	{
		return FMCPToolHelpers::MakeError(Request, kPHYErrorInvalidParams, ArgErr);
	}

	// radius / half_height must be >= 1.0 per plan §C-Physics inputSchema "minimum: 1.0".
	double Radius = 0.0;
	if (!PHY_ReadClampedNumber(Request.Args, TEXT("radius"), 1.0, 100000.0, Radius, ArgErr))
	{
		return FMCPToolHelpers::MakeError(Request, kPHYErrorInvalidParams, ArgErr);
	}
	double HalfHeight = 0.0;
	if (!PHY_ReadClampedNumber(Request.Args, TEXT("half_height"), 1.0, 100000.0, HalfHeight, ArgErr))
	{
		return FMCPToolHelpers::MakeError(Request, kPHYErrorInvalidParams, ArgErr);
	}

	FVector RotationVec = FVector::ZeroVector;
	if (!PHY_ReadOptionalVectorArray(Request.Args, TEXT("rotation"), FVector::ZeroVector, RotationVec, ArgErr))
	{
		return FMCPToolHelpers::MakeError(Request, kPHYErrorInvalidParams, ArgErr);
	}
	const FRotator Rotation(RotationVec.X, RotationVec.Y, RotationVec.Z); // pitch, yaw, roll
	const FQuat RotationQuat = Rotation.Quaternion();

	FString ChannelStr;
	Request.Args->TryGetStringField(TEXT("channel"), ChannelStr);
	ECollisionChannel Channel = ECC_Visibility;
	if (!PHY_ParseCollisionChannel(ChannelStr, Channel))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidCollisionChannel,
			FString::Printf(TEXT("channel '%s' not recognised; accepted: %s"),
				*ChannelStr, PHY_AcceptedChannelNames));
	}

	bool bMultiHit = false;
	Request.Args->TryGetBoolField(TEXT("multi_hit"), bMultiHit);

	UWorld* World = PHY_ResolveTraceWorld();
	if (!World)
	{
		return FMCPToolHelpers::MakeError(Request, kPHYErrorInternal,
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
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── physics.apply_impulse ─────────────────────────────────────────────────────────────────────
//
// Args:
//   actor_path     : string                             required
//   impulse        : [x, y, z]                          required
//   world_or_local : "world"|"local" (default "world")
//   velocity_change: bool   (default false)             when true, impulse is treated as a
//                                                       velocity delta (cm/s) ignoring mass — wraps
//                                                       UPrimitiveComponent::AddImpulse's bVelChange
//   component_name : string (default "" → root primitive)
//
// Result:
//   {
//     applied        : true,
//     component_path : string,   // canonical "actor_path/component_name"
//     impulse        : [x,y,z],  // the (world-space) impulse vector that was applied
//     velocity_change: bool,     // echoed back
//     world          : string,
//     world_kind     : "PIE"|"Editor"|...
//   }
//
// Errors:
//   -32602 InvalidParams      missing/malformed args; invalid world_or_local
//   -32004 ObjectNotFound     actor_path doesn't resolve OR component_name not found OR root is non-primitive
//   -32011 WrongClass         component_name found but it's not a UPrimitiveComponent
//   -32603 Internal           no world (commandlet)
//
// Notes:
//   - NOT PIE-guarded. Operates on whichever world resolves (PIE > editor).
//   - No FScopedTransaction / MarkPackageDirty — physics is runtime state, not an undoable asset edit.
//   - For impulse to do anything visible the target must be ``IsSimulatingPhysics() == true``. We
//     do NOT check or auto-enable here; caller pairs with ``physics.set_simulation`` when needed.
FMCPResponse Tool_ApplyImpulse(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kPHYErrorInvalidParams, TEXT("missing args object"));
	}

	FString ActorPath;
	if (!Request.Args->TryGetStringField(TEXT("actor_path"), ActorPath) || ActorPath.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kPHYErrorInvalidParams, TEXT("missing required string field 'actor_path'"));
	}

	FVector ImpulseVec;
	FString ArgErr;
	if (!PHY_ReadVectorArray(Request.Args, TEXT("impulse"), ImpulseVec, ArgErr))
	{
		return FMCPToolHelpers::MakeError(Request, kPHYErrorInvalidParams, ArgErr);
	}

	FString WorldOrLocal = TEXT("world");
	Request.Args->TryGetStringField(TEXT("world_or_local"), WorldOrLocal);
	if (!WorldOrLocal.Equals(TEXT("world"), ESearchCase::IgnoreCase)
		&& !WorldOrLocal.Equals(TEXT("local"), ESearchCase::IgnoreCase))
	{
		return FMCPToolHelpers::MakeError(Request, kPHYErrorInvalidParams,
			FString::Printf(TEXT("'world_or_local' must be 'world' or 'local' (got '%s')"), *WorldOrLocal));
	}
	const bool bLocal = WorldOrLocal.Equals(TEXT("local"), ESearchCase::IgnoreCase);

	bool bVelocityChange = false;
	Request.Args->TryGetBoolField(TEXT("velocity_change"), bVelocityChange);

	FString CompName;
	Request.Args->TryGetStringField(TEXT("component_name"), CompName);

	UWorld* World = PHY_ResolveTraceWorld();
	if (!World)
	{
		return FMCPToolHelpers::MakeError(Request, kPHYErrorInternal,
			TEXT("no world available (GEditor missing OR no level loaded)"));
	}

	bool bAmbiguous = false;
	FString AmbiguityHint, ResolveErr;
	AActor* Actor = FMCPActorPathUtils::ResolveActor(
		ActorPath, /*bRejectPIE*/ false, bAmbiguous, AmbiguityHint, ResolveErr);
	if (!Actor)
	{
		return FMCPToolHelpers::MakeError(Request, kPHYErrorObjectNotFound,
			FString::Printf(TEXT("actor '%s' not found: %s"), *ActorPath, *ResolveErr));
	}

	bool bWrongClass = false;
	UPrimitiveComponent* Prim = PHY_GetTargetPrimitive(Actor, CompName, bWrongClass);
	if (!Prim)
	{
		if (bWrongClass)
		{
			return FMCPToolHelpers::MakeError(Request, kPHYErrorWrongClass,
				FString::Printf(TEXT("component '%s' on actor '%s' is not a UPrimitiveComponent"),
					*CompName, *ActorPath));
		}
		return FMCPToolHelpers::MakeError(Request, kPHYErrorObjectNotFound,
			CompName.IsEmpty()
				? FString::Printf(TEXT("actor '%s' has no UPrimitiveComponent root"), *ActorPath)
				: FString::Printf(TEXT("component '%s' not found on actor '%s'"), *CompName, *ActorPath));
	}

	FVector AppliedImpulse = ImpulseVec;
	if (bLocal)
	{
		AppliedImpulse = Prim->GetComponentTransform().TransformVector(ImpulseVec);
	}

	Prim->AddImpulse(AppliedImpulse, NAME_None, bVelocityChange);

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("applied"), true);
	Out->SetStringField(TEXT("component_path"), FMCPComponentPathUtils::BuildComponentPath(Prim));
	Out->SetArrayField(TEXT("impulse"), PHY_VectorToArray(AppliedImpulse));
	Out->SetBoolField(TEXT("velocity_change"), bVelocityChange);
	Out->SetStringField(TEXT("world"), World->GetOutermost() ? World->GetOutermost()->GetName() : FString());
	Out->SetStringField(TEXT("world_kind"), PHY_DescribeWorldKind(World));
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── physics.set_simulation ────────────────────────────────────────────────────────────────────
//
// Args:
//   actor_path     : string  required
//   simulate       : bool    required
//   component_name : string  (default "" → root primitive; ignored when recurse=true)
//   recurse        : bool    (default false) — when true, set SimulatePhysics on ALL primitive
//                            components of the actor (component_name ignored).
//
// Result:
//   recurse=false:
//   {
//     component_path     : string,
//     prior_simulating   : bool,
//     now_simulating     : bool,
//     world / world_kind : ...
//   }
//   recurse=true:
//   {
//     recursive          : true,
//     component_count    : int,
//     components         : [{ component_path, prior_simulating, now_simulating }, ...],
//     world / world_kind : ...
//   }
//
// Errors: -32602 / -32004 / -32011 / -32603 (same family as apply_impulse).
//
// Notes:
//   - "now_simulating" reads back ``IsSimulatingPhysics()`` AFTER the SetSimulatePhysics call so the
//     caller sees the actual resulting state (some primitives — e.g. static-body USceneComponent
//     subclasses — silently refuse the set). prior_simulating is the pre-call value.
//   - Recursive form iterates ALL UPrimitiveComponents on the actor (including non-scene-attached
//     ones, e.g. UInstancedStaticMeshComponent under a custom outer). NO undo / no transaction.
FMCPResponse Tool_SetSimulation(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kPHYErrorInvalidParams, TEXT("missing args object"));
	}

	FString ActorPath;
	if (!Request.Args->TryGetStringField(TEXT("actor_path"), ActorPath) || ActorPath.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kPHYErrorInvalidParams, TEXT("missing required string field 'actor_path'"));
	}

	bool bSimulate = false;
	if (!Request.Args->TryGetBoolField(TEXT("simulate"), bSimulate))
	{
		return FMCPToolHelpers::MakeError(Request, kPHYErrorInvalidParams, TEXT("missing required bool field 'simulate'"));
	}

	bool bRecurse = false;
	Request.Args->TryGetBoolField(TEXT("recurse"), bRecurse);

	FString CompName;
	Request.Args->TryGetStringField(TEXT("component_name"), CompName);

	UWorld* World = PHY_ResolveTraceWorld();
	if (!World)
	{
		return FMCPToolHelpers::MakeError(Request, kPHYErrorInternal,
			TEXT("no world available (GEditor missing OR no level loaded)"));
	}

	bool bAmbiguous = false;
	FString AmbiguityHint, ResolveErr;
	AActor* Actor = FMCPActorPathUtils::ResolveActor(
		ActorPath, /*bRejectPIE*/ false, bAmbiguous, AmbiguityHint, ResolveErr);
	if (!Actor)
	{
		return FMCPToolHelpers::MakeError(Request, kPHYErrorObjectNotFound,
			FString::Printf(TEXT("actor '%s' not found: %s"), *ActorPath, *ResolveErr));
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();

	if (bRecurse)
	{
		TArray<UPrimitiveComponent*> Children;
		Actor->GetComponents(UPrimitiveComponent::StaticClass(), Children);
		TArray<TSharedPtr<FJsonValue>> Entries;
		Entries.Reserve(Children.Num());
		for (UPrimitiveComponent* P : Children)
		{
			if (!P) { continue; }
			const bool bPrior = P->IsSimulatingPhysics();
			P->SetSimulatePhysics(bSimulate);
			const bool bNow = P->IsSimulatingPhysics();
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("component_path"), FMCPComponentPathUtils::BuildComponentPath(P));
			Entry->SetBoolField(TEXT("prior_simulating"), bPrior);
			Entry->SetBoolField(TEXT("now_simulating"), bNow);
			Entries.Add(MakeShared<FJsonValueObject>(Entry));
		}
		Out->SetBoolField(TEXT("recursive"), true);
		Out->SetNumberField(TEXT("component_count"), Entries.Num());
		Out->SetArrayField(TEXT("components"), Entries);
	}
	else
	{
		bool bWrongClass = false;
		UPrimitiveComponent* Prim = PHY_GetTargetPrimitive(Actor, CompName, bWrongClass);
		if (!Prim)
		{
			if (bWrongClass)
			{
				return FMCPToolHelpers::MakeError(Request, kPHYErrorWrongClass,
					FString::Printf(TEXT("component '%s' on actor '%s' is not a UPrimitiveComponent"),
						*CompName, *ActorPath));
			}
			return FMCPToolHelpers::MakeError(Request, kPHYErrorObjectNotFound,
				CompName.IsEmpty()
					? FString::Printf(TEXT("actor '%s' has no UPrimitiveComponent root"), *ActorPath)
					: FString::Printf(TEXT("component '%s' not found on actor '%s'"), *CompName, *ActorPath));
		}

		const bool bPrior = Prim->IsSimulatingPhysics();
		Prim->SetSimulatePhysics(bSimulate);
		const bool bNow = Prim->IsSimulatingPhysics();

		Out->SetBoolField(TEXT("recursive"), false);
		Out->SetStringField(TEXT("component_path"), FMCPComponentPathUtils::BuildComponentPath(Prim));
		Out->SetBoolField(TEXT("prior_simulating"), bPrior);
		Out->SetBoolField(TEXT("now_simulating"), bNow);
	}

	Out->SetStringField(TEXT("world"), World->GetOutermost() ? World->GetOutermost()->GetName() : FString());
	Out->SetStringField(TEXT("world_kind"), PHY_DescribeWorldKind(World));
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── physics.set_velocity ──────────────────────────────────────────────────────────────────────
//
// Args:
//   actor_path     : string  required
//   linear         : [x, y, z]                        optional
//   angular        : [x, y, z] in degrees/sec         optional
//   component_name : string  (default "" → root primitive)
//   world_or_local : "world"|"local" (default "world") — affects ``linear`` only;
//                                                       angular is always world-space (matches
//                                                       SetPhysicsAngularVelocityInDegrees API)
//
// At least ONE of ``linear`` / ``angular`` must be supplied. Both absent → -32602.
//
// Result:
//   {
//     component_path : string,
//     prior_linear   : [x,y,z], prior_angular : [x,y,z],
//     new_linear     : [x,y,z], new_angular   : [x,y,z],
//     world / world_kind : ...
//   }
//
// Errors: -32602 / -32004 / -32011 / -32603 (same family).
FMCPResponse Tool_SetVelocity(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kPHYErrorInvalidParams, TEXT("missing args object"));
	}

	FString ActorPath;
	if (!Request.Args->TryGetStringField(TEXT("actor_path"), ActorPath) || ActorPath.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kPHYErrorInvalidParams, TEXT("missing required string field 'actor_path'"));
	}

	const bool bHasLinear  = Request.Args->HasField(TEXT("linear"));
	const bool bHasAngular = Request.Args->HasField(TEXT("angular"));
	if (!bHasLinear && !bHasAngular)
	{
		return FMCPToolHelpers::MakeError(Request, kPHYErrorInvalidParams,
			TEXT("at least one of 'linear' or 'angular' must be supplied"));
	}

	FVector LinearVec = FVector::ZeroVector;
	FVector AngularVec = FVector::ZeroVector;
	FString ArgErr;
	if (bHasLinear)
	{
		if (!PHY_ReadVectorArray(Request.Args, TEXT("linear"), LinearVec, ArgErr))
		{
			return FMCPToolHelpers::MakeError(Request, kPHYErrorInvalidParams, ArgErr);
		}
	}
	if (bHasAngular)
	{
		if (!PHY_ReadVectorArray(Request.Args, TEXT("angular"), AngularVec, ArgErr))
		{
			return FMCPToolHelpers::MakeError(Request, kPHYErrorInvalidParams, ArgErr);
		}
	}

	FString WorldOrLocal = TEXT("world");
	Request.Args->TryGetStringField(TEXT("world_or_local"), WorldOrLocal);
	if (!WorldOrLocal.Equals(TEXT("world"), ESearchCase::IgnoreCase)
		&& !WorldOrLocal.Equals(TEXT("local"), ESearchCase::IgnoreCase))
	{
		return FMCPToolHelpers::MakeError(Request, kPHYErrorInvalidParams,
			FString::Printf(TEXT("'world_or_local' must be 'world' or 'local' (got '%s')"), *WorldOrLocal));
	}
	const bool bLocal = WorldOrLocal.Equals(TEXT("local"), ESearchCase::IgnoreCase);

	FString CompName;
	Request.Args->TryGetStringField(TEXT("component_name"), CompName);

	UWorld* World = PHY_ResolveTraceWorld();
	if (!World)
	{
		return FMCPToolHelpers::MakeError(Request, kPHYErrorInternal,
			TEXT("no world available (GEditor missing OR no level loaded)"));
	}

	bool bAmbiguous = false;
	FString AmbiguityHint, ResolveErr;
	AActor* Actor = FMCPActorPathUtils::ResolveActor(
		ActorPath, /*bRejectPIE*/ false, bAmbiguous, AmbiguityHint, ResolveErr);
	if (!Actor)
	{
		return FMCPToolHelpers::MakeError(Request, kPHYErrorObjectNotFound,
			FString::Printf(TEXT("actor '%s' not found: %s"), *ActorPath, *ResolveErr));
	}

	bool bWrongClass = false;
	UPrimitiveComponent* Prim = PHY_GetTargetPrimitive(Actor, CompName, bWrongClass);
	if (!Prim)
	{
		if (bWrongClass)
		{
			return FMCPToolHelpers::MakeError(Request, kPHYErrorWrongClass,
				FString::Printf(TEXT("component '%s' on actor '%s' is not a UPrimitiveComponent"),
					*CompName, *ActorPath));
		}
		return FMCPToolHelpers::MakeError(Request, kPHYErrorObjectNotFound,
			CompName.IsEmpty()
				? FString::Printf(TEXT("actor '%s' has no UPrimitiveComponent root"), *ActorPath)
				: FString::Printf(TEXT("component '%s' not found on actor '%s'"), *CompName, *ActorPath));
	}

	const FVector PriorLin = Prim->GetPhysicsLinearVelocity();
	const FVector PriorAng = Prim->GetPhysicsAngularVelocityInDegrees();

	if (bHasLinear)
	{
		FVector V = LinearVec;
		if (bLocal)
		{
			V = Prim->GetComponentTransform().TransformVector(LinearVec);
		}
		Prim->SetPhysicsLinearVelocity(V, /*bAddToCurrent*/ false);
	}
	if (bHasAngular)
	{
		// Angular is always world-space — SetPhysicsAngularVelocityInDegrees doesn't model a local
		// frame on the wire side, and rotating a (pitch,yaw,roll) deg/sec triplet through the
		// component transform doesn't have a single canonical meaning.
		Prim->SetPhysicsAngularVelocityInDegrees(AngularVec, /*bAddToCurrent*/ false);
	}

	const FVector NewLin = Prim->GetPhysicsLinearVelocity();
	const FVector NewAng = Prim->GetPhysicsAngularVelocityInDegrees();

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("component_path"), FMCPComponentPathUtils::BuildComponentPath(Prim));
	Out->SetArrayField(TEXT("prior_linear"),  PHY_VectorToArray(PriorLin));
	Out->SetArrayField(TEXT("prior_angular"), PHY_VectorToArray(PriorAng));
	Out->SetArrayField(TEXT("new_linear"),    PHY_VectorToArray(NewLin));
	Out->SetArrayField(TEXT("new_angular"),   PHY_VectorToArray(NewAng));
	Out->SetStringField(TEXT("world"), World->GetOutermost() ? World->GetOutermost()->GetName() : FString());
	Out->SetStringField(TEXT("world_kind"), PHY_DescribeWorldKind(World));
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── physics.overlap_test ──────────────────────────────────────────────────────────────────────
//
// Args:
//   location       : [x, y, z]                        required
//   radius         : number                           required, > 0
//   channel        : string (default "WorldStatic")
//   ignore_actors  : array<string> (default [])
//
// Result:
//   {
//     world / world_kind : ...,
//     ignored_count : int,
//     hit_count     : int,
//     hits          : [
//       { actor_guid, actor_path, component, location, blocking }, ...
//     ]
//   }
//
// Errors:
//   -32602 InvalidParams              missing/malformed args, radius <= 0
//   -32041 InvalidCollisionChannel    channel string unknown
//   -32603 Internal                   no world
//
// Notes:
//   - Sphere only — for box/capsule overlaps use the dedicated sweep tools.
//   - ``location`` in FOverlapResult JSON comes from ``Component->GetComponentLocation()`` (overlaps
//     don't carry a single impact point the way hits do). It's a "where is the overlapping body's
//     anchor" hint, not an intersection point.
//   - **Channel default differs from line_trace/sweep_capsule.** Overlap callers typically want
//     "what static geometry am I touching at this point" — Visibility is rarely the answer because
//     UE meshes default to NoCollision on the Visibility channel. WorldStatic is the natural default
//     and is documented in the channel-list message.
FMCPResponse Tool_OverlapTest(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kPHYErrorInvalidParams, TEXT("missing args object"));
	}

	FVector Location;
	FString ArgErr;
	if (!PHY_ReadVectorArray(Request.Args, TEXT("location"), Location, ArgErr))
	{
		return FMCPToolHelpers::MakeError(Request, kPHYErrorInvalidParams, ArgErr);
	}

	double Radius = 0.0;
	if (!PHY_ReadClampedNumber(Request.Args, TEXT("radius"), 0.001, 1000000.0, Radius, ArgErr))
	{
		return FMCPToolHelpers::MakeError(Request, kPHYErrorInvalidParams, ArgErr);
	}

	// Channel default = "WorldStatic" (NOT Visibility — see header comment). Empty string also OK,
	// PHY_ParseCollisionChannel maps empty to Visibility, but for overlap we want WorldStatic.
	FString ChannelStr = TEXT("WorldStatic");
	Request.Args->TryGetStringField(TEXT("channel"), ChannelStr);
	if (ChannelStr.IsEmpty()) { ChannelStr = TEXT("WorldStatic"); }
	ECollisionChannel Channel = ECC_WorldStatic;
	if (!PHY_ParseCollisionChannel(ChannelStr, Channel))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidCollisionChannel,
			FString::Printf(TEXT("channel '%s' not recognised; accepted: %s"),
				*ChannelStr, PHY_AcceptedChannelNames));
	}

	UWorld* World = PHY_ResolveTraceWorld();
	if (!World)
	{
		return FMCPToolHelpers::MakeError(Request, kPHYErrorInternal,
			TEXT("no world available (GEditor missing OR no level loaded)"));
	}

	FCollisionQueryParams Params(SCENE_QUERY_STAT(MCPOverlap), /*bTraceComplex*/ false);
	const int32 IgnoredCount = PHY_AddIgnoredActors(Request.Args, Params);

	TArray<FOverlapResult> Overlaps;
	World->OverlapMultiByChannel(
		Overlaps,
		Location,
		FQuat::Identity,
		Channel,
		FCollisionShape::MakeSphere(static_cast<float>(Radius)),
		Params);

	TArray<TSharedPtr<FJsonValue>> HitsArr;
	HitsArr.Reserve(Overlaps.Num());
	for (const FOverlapResult& O : Overlaps)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		AActor* HitActor = O.GetActor();
		if (HitActor)
		{
			const FGuid Guid = HitActor->GetActorGuid();
			Obj->SetStringField(TEXT("actor_guid"),
				Guid.IsValid() ? Guid.ToString(EGuidFormats::DigitsWithHyphens) : FString());
			Obj->SetStringField(TEXT("actor_path"), FMCPActorPathUtils::BuildActorPath(HitActor));
		}
		else
		{
			Obj->SetField(TEXT("actor_guid"), MakeShared<FJsonValueNull>());
			Obj->SetField(TEXT("actor_path"), MakeShared<FJsonValueNull>());
		}

		UPrimitiveComponent* HitComp = O.GetComponent();
		if (HitComp)
		{
			Obj->SetStringField(TEXT("component"), HitComp->GetName());
			Obj->SetArrayField(TEXT("location"), PHY_VectorToArray(HitComp->GetComponentLocation()));
		}
		else
		{
			Obj->SetField(TEXT("component"), MakeShared<FJsonValueNull>());
			Obj->SetField(TEXT("location"),  MakeShared<FJsonValueNull>());
		}

		Obj->SetBoolField(TEXT("blocking"), O.bBlockingHit != 0);
		HitsArr.Add(MakeShared<FJsonValueObject>(Obj));
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("world"), World->GetOutermost() ? World->GetOutermost()->GetName() : FString());
	Out->SetStringField(TEXT("world_kind"), PHY_DescribeWorldKind(World));
	Out->SetNumberField(TEXT("ignored_count"), IgnoredCount);
	Out->SetNumberField(TEXT("hit_count"), HitsArr.Num());
	Out->SetArrayField(TEXT("hits"), HitsArr);
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── Registration ──────────────────────────────────────────────────────────────────────────────
void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	// Phase 5 Chunk C — read-only traces.
	RegisterTool(TEXT("physics.line_trace"),    &Tool_LineTrace,    /*Lane A*/ false);
	RegisterTool(TEXT("physics.sweep_capsule"), &Tool_SweepCapsule, /*Lane A*/ false);

	// Wave G Surface 1 — runtime mutation (impulse / sim toggle / velocity / overlap).
	RegisterTool(TEXT("physics.apply_impulse"),  &Tool_ApplyImpulse,  /*Lane A*/ false);
	RegisterTool(TEXT("physics.set_simulation"), &Tool_SetSimulation, /*Lane A*/ false);
	RegisterTool(TEXT("physics.set_velocity"),   &Tool_SetVelocity,   /*Lane A*/ false);
	RegisterTool(TEXT("physics.overlap_test"),   &Tool_OverlapTest,   /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("Phase 5 Chunk C + Wave G S1 (Physics): registered 6 physics.* handlers (2 traces + 4 writes, all Lane A)"));
}

} // namespace FPhysicsTools

#undef LOCTEXT_NAMESPACE

MCP_REGISTER_SURFACE(PhysicsTools, &FPhysicsTools::Register)
