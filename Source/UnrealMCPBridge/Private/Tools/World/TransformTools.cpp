// Copyright FatumGame. All Rights Reserved.

#include "TransformTools.h"

#include "MCPSurfaceRegistry.h"

#include "FMCPDispatchQueue.h"
#include "MCPJsonBuilder.h"
#include "MCPMutatorScope.h"
#include "MCPToolHelpers.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPActorPathUtils.h"
#include "Utils/MCPWorldContext.h"

#include "CollisionQueryParams.h"
#include "Components/SceneComponent.h"
#include "Engine/HitResult.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "UObject/Package.h"
#include "WorldCollision.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// TFM_ prefix per the unity-build symbol-collision convention.
	constexpr int32 kTFMErrorInternal = -32603;

	// ─── arg parsing helpers ─────────────────────────────────────────────────────────────────────

	/**
	 * Read a non-empty array of strings. Caller maps the bool false to -32602 with OutError.
	 * Empty array (Num() == 0) is treated as missing — same -32602.
	 */
	bool TFM_ReadActorPaths(
		const TSharedPtr<FJsonObject>& Args,
		TArray<FString>& OutPaths,
		FString& OutError)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Args.IsValid() || !Args->TryGetArrayField(TEXT("actor_paths"), Arr) || !Arr)
		{
			OutError = TEXT("missing required array field 'actor_paths'");
			return false;
		}
		if (Arr->Num() == 0)
		{
			OutError = TEXT("'actor_paths' is empty — supply at least one actor path");
			return false;
		}
		OutPaths.Reset(Arr->Num());
		for (int32 i = 0; i < Arr->Num(); ++i)
		{
			FString S;
			if (!(*Arr)[i].IsValid() || !(*Arr)[i]->TryGetString(S) || S.IsEmpty())
			{
				OutError = FString::Printf(
					TEXT("'actor_paths[%d]' must be a non-empty string"), i);
				return false;
			}
			OutPaths.Add(MoveTemp(S));
		}
		return true;
	}

	/**
	 * Read an OPTIONAL [x, y, z] number array. Missing → bOutPresent=false; present-but-malformed
	 * → return false with OutError populated. Present-and-valid → OutV + bOutPresent=true.
	 */
	bool TFM_ReadOptionalVec3(
		const TSharedPtr<FJsonObject>& Args,
		const TCHAR* FieldName,
		FVector& OutV,
		bool& bOutPresent,
		FString& OutError)
	{
		bOutPresent = false;
		if (!Args.IsValid() || !Args->HasField(FieldName))
		{
			return true;
		}
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Args->TryGetArrayField(FieldName, Arr) || !Arr)
		{
			OutError = FString::Printf(TEXT("'%s' must be a [x,y,z] array"), FieldName);
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
		bOutPresent = true;
		return true;
	}

	/**
	 * Parse a collision-channel string. Accepts the subset the surface needs (Visibility,
	 * WorldStatic, WorldDynamic). Default Visibility on empty.
	 */
	bool TFM_ParseTraceChannel(const FString& ChannelStr, ECollisionChannel& OutChannel)
	{
		if (ChannelStr.IsEmpty() || ChannelStr.Equals(TEXT("Visibility"), ESearchCase::IgnoreCase))
		{
			OutChannel = ECC_Visibility;
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
		if (ChannelStr.Equals(TEXT("Camera"), ESearchCase::IgnoreCase))
		{
			OutChannel = ECC_Camera;
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
		return false;
	}

	// ─── shared post-write housekeeping ──────────────────────────────────────────────────────────

	/**
	 * Queue dirty for the actor's package — handles WorldPartition's one-file-per-actor (external
	 * package) AND the legacy in-level-package case (outermost). Mirrors the pattern from
	 * WP_SetActorRuntimeGrid. Queues through the supplied FMCPMutatorScope so the bulk-dirty pass
	 * happens once at scope-destruction (avoids per-actor MarkPackageDirty in a tight loop).
	 */
	void TFM_QueueActorPackageDirty(FMCPMutatorScope& Scope, AActor* Actor)
	{
		check(Actor);
		if (UPackage* ExternalPkg = Actor->GetExternalPackage())
		{
			Scope.DirtyPackage(ExternalPkg);
		}
		else
		{
			Scope.DirtyPackage(Actor->GetOutermost());
		}
	}

	TArray<TSharedPtr<FJsonValue>> TFM_VecToArray(const FVector& V)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Add(MakeShared<FJsonValueNumber>(V.X));
		Arr.Add(MakeShared<FJsonValueNumber>(V.Y));
		Arr.Add(MakeShared<FJsonValueNumber>(V.Z));
		return Arr;
	}

	TSharedRef<FJsonObject> TFM_MakeFailureEntry(const FString& ActorPath, const FString& Reason)
	{
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("actor_path"), ActorPath);
		Entry->SetStringField(TEXT("reason"), Reason);
		return Entry;
	}

	/**
	 * Resolve a single supplied path through FMCPActorPathUtils. Returns the actor or null +
	 * populates OutReason on failure. Skips already-pending-kill actors (treats them as
	 * not-found).
	 */
	AActor* TFM_ResolveActorOrReason(const FString& Path, FString& OutReason)
	{
		bool bAmbiguous = false;
		FString AmbiguityHint, ResolveErr;
		AActor* Actor = FMCPActorPathUtils::ResolveActor(
			Path, /*bRejectPIE*/ true, bAmbiguous, AmbiguityHint, ResolveErr);
		if (Actor)
		{
			return Actor;
		}
		if (bAmbiguous)
		{
			OutReason = FString::Printf(TEXT("ambiguous name; candidates: %s"), *AmbiguityHint);
		}
		else
		{
			OutReason = ResolveErr.IsEmpty() ? TEXT("not found") : ResolveErr;
		}
		return nullptr;
	}
} // namespace

namespace FTransformTools
{

// ─── transform.batch_set ──────────────────────────────────────────────────────────────────────
//
// Args:
//   actor_paths : [string]               required, non-empty
//   location    : [x, y, z]              optional
//   rotation    : [p, y, r]              optional (Euler degrees: pitch, yaw, roll)
//   scale       : [x, y, z]              optional
//   relative    : bool                   default false
//
// At least one of location / rotation / scale MUST be supplied — otherwise -32602.
//
// When relative=true, each supplied component is ADDED to the actor's current value
// (location: vector add; rotation: quat-combined via FRotator addition; scale: component-wise
// multiplied to preserve the "scale = identity element 1" intuition).
//
// Result:
//   { updated: int, failed: int,
//     failures: [{ actor_path, reason }, ...] }
//
// Top-level errors:
//   -32602 InvalidParams   missing args / actor_paths empty / no transform component supplied
//   -32027 PIEActive       editor-world mutator refused during PIE
//   -32004 ObjectNotFound  ZERO actors resolved (failures[] would contain everything otherwise)
FMCPResponse Tool_BatchSet(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_TransformBatchSet", "MCP: Batch Set Transform"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams, TEXT("missing args object"));
	}

	TArray<FString> Paths;
	FString ParseErr;
	if (!TFM_ReadActorPaths(Request.Args, Paths, ParseErr))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams, ParseErr);
	}

	FVector Loc, Rot, Scale;
	bool bHaveLoc = false, bHaveRot = false, bHaveScale = false;
	if (!TFM_ReadOptionalVec3(Request.Args, TEXT("location"), Loc, bHaveLoc, ParseErr))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams, ParseErr);
	}
	if (!TFM_ReadOptionalVec3(Request.Args, TEXT("rotation"), Rot, bHaveRot, ParseErr))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams, ParseErr);
	}
	if (!TFM_ReadOptionalVec3(Request.Args, TEXT("scale"), Scale, bHaveScale, ParseErr))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams, ParseErr);
	}

	if (!bHaveLoc && !bHaveRot && !bHaveScale)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			TEXT("transform.batch_set: at least one of 'location' / 'rotation' / 'scale' is required"));
	}

	bool bRelative = false;
	Request.Args->TryGetBoolField(TEXT("relative"), bRelative);

	int32 Updated = 0;
	TArray<TSharedPtr<FJsonValue>> Failures;
	Failures.Reserve(4);

	for (const FString& Path : Paths)
	{
		FString Reason;
		AActor* Actor = TFM_ResolveActorOrReason(Path, Reason);
		if (!Actor)
		{
			Failures.Add(MakeShared<FJsonValueObject>(TFM_MakeFailureEntry(Path, Reason)));
			continue;
		}
		if (!Actor->GetRootComponent())
		{
			Failures.Add(MakeShared<FJsonValueObject>(TFM_MakeFailureEntry(
				FMCPActorPathUtils::BuildActorPath(Actor),
				TEXT("no root component (cannot transform)"))));
			continue;
		}

		Actor->Modify();

		const FTransform Cur = Actor->GetActorTransform();
		FVector NewLoc   = Cur.GetLocation();
		FRotator NewRot  = Cur.GetRotation().Rotator();
		FVector NewScale = Cur.GetScale3D();

		if (bHaveLoc)
		{
			NewLoc = bRelative ? (NewLoc + Loc) : Loc;
		}
		if (bHaveRot)
		{
			const FRotator Delta(Rot.X, Rot.Y, Rot.Z);  // pitch, yaw, roll
			NewRot = bRelative ? (NewRot + Delta) : Delta;
		}
		if (bHaveScale)
		{
			if (bRelative)
			{
				NewScale = FVector(NewScale.X * Scale.X, NewScale.Y * Scale.Y, NewScale.Z * Scale.Z);
			}
			else
			{
				NewScale = Scale;
			}
		}

		const FTransform NewT(NewRot.Quaternion(), NewLoc, NewScale);
		Actor->SetActorTransform(NewT);

		TFM_QueueActorPackageDirty(Scope, Actor);
		++Updated;
	}

	// All resolution failures → top-level -32004. Otherwise partial success is fine.
	if (Updated == 0)
	{
		Scope.Abort();
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("transform.batch_set: 0/%d actors resolved; first reason: %s"),
				Paths.Num(),
				Failures.Num() > 0 ? *Failures[0]->AsObject()->GetStringField(TEXT("reason")) : TEXT("(empty)")));
	}

	const int32 FailuresNum = Failures.Num();
	return FMCPJsonBuilder()
		.Num(TEXT("updated"), Updated)
		.Num(TEXT("failed"),  FailuresNum)
		.Arr(TEXT("failures"), MoveTemp(Failures))
		.BuildSuccess(Request);
}

// ─── transform.snap_to_floor ──────────────────────────────────────────────────────────────────
//
// Args:
//   actor_paths        : [string]                  required, non-empty
//   max_trace_distance : number (default 100000)   cm, downward
//   trace_channel      : string (default "Visibility")
//
// Per actor: cast a downward ray from its current location. On hit → SetActorLocation(Hit.Location).
// Miss → enter into ``results`` with hit_actor=null. Always-hit + always-miss are both valid
// successes; the response carries snapped + missed counts.
//
// Result:
//   { snapped: int, missed: int,
//     results: [{ actor_path, prior_z, new_z, hit_actor?: string|null }, ...] }
//
// Top-level errors:
//   -32602 InvalidParams              missing args / actor_paths empty / max_trace_distance <= 0
//   -32041 InvalidCollisionChannel    trace_channel string unknown
//   -32027 PIEActive                  editor-world mutator refused during PIE
//   -32004 ObjectNotFound             ZERO actors resolved
//   -32603 Internal                   no editor world (commandlet / cooker)
FMCPResponse Tool_SnapToFloor(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_TransformSnapToFloor", "MCP: Snap Actors To Floor"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams, TEXT("missing args object"));
	}

	TArray<FString> Paths;
	FString ParseErr;
	if (!TFM_ReadActorPaths(Request.Args, Paths, ParseErr))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams, ParseErr);
	}

	double MaxDist = 100000.0;
	Request.Args->TryGetNumberField(TEXT("max_trace_distance"), MaxDist);
	if (MaxDist <= 0.0)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			TEXT("'max_trace_distance' must be > 0"));
	}

	FString ChannelStr;
	Request.Args->TryGetStringField(TEXT("trace_channel"), ChannelStr);
	ECollisionChannel Channel = ECC_Visibility;
	if (!TFM_ParseTraceChannel(ChannelStr, Channel))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidCollisionChannel,
			FString::Printf(TEXT("trace_channel '%s' not recognised; accepted: ")
				TEXT("Visibility, WorldStatic, WorldDynamic, Camera, Pawn, PhysicsBody"),
				*ChannelStr));
	}

	UWorld* World = FMCPWorldContext::GetEditorWorld();
	if (!World)
	{
		return FMCPToolHelpers::MakeError(Request, kTFMErrorInternal,
			TEXT("no editor world (GEditor missing or no level loaded)"));
	}

	int32 Snapped = 0;
	int32 Missed  = 0;
	TArray<TSharedPtr<FJsonValue>> Results;
	Results.Reserve(Paths.Num());

	int32 ResolutionFailures = 0;
	FString FirstResolutionReason;

	for (const FString& Path : Paths)
	{
		FString Reason;
		AActor* Actor = TFM_ResolveActorOrReason(Path, Reason);
		if (!Actor)
		{
			++ResolutionFailures;
			if (FirstResolutionReason.IsEmpty()) { FirstResolutionReason = Reason; }
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("actor_path"), Path);
			Entry->SetField(TEXT("prior_z"), MakeShared<FJsonValueNull>());
			Entry->SetField(TEXT("new_z"),   MakeShared<FJsonValueNull>());
			Entry->SetField(TEXT("hit_actor"), MakeShared<FJsonValueNull>());
			Entry->SetStringField(TEXT("error"), Reason);
			Results.Add(MakeShared<FJsonValueObject>(Entry));
			++Missed;
			continue;
		}
		if (!Actor->GetRootComponent())
		{
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("actor_path"), FMCPActorPathUtils::BuildActorPath(Actor));
			Entry->SetField(TEXT("prior_z"), MakeShared<FJsonValueNull>());
			Entry->SetField(TEXT("new_z"),   MakeShared<FJsonValueNull>());
			Entry->SetField(TEXT("hit_actor"), MakeShared<FJsonValueNull>());
			Entry->SetStringField(TEXT("error"), TEXT("no root component (cannot transform)"));
			Results.Add(MakeShared<FJsonValueObject>(Entry));
			++Missed;
			continue;
		}

		const FVector Start = Actor->GetActorLocation();
		const FVector End   = Start - FVector(0.0, 0.0, MaxDist);

		FCollisionQueryParams QP(SCENE_QUERY_STAT(MCPSnapToFloor), /*bTraceComplex*/ false);
		QP.AddIgnoredActor(Actor);

		FHitResult Hit;
		const bool bHit = World->LineTraceSingleByChannel(Hit, Start, End, Channel, QP);

		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("actor_path"), FMCPActorPathUtils::BuildActorPath(Actor));
		Entry->SetNumberField(TEXT("prior_z"), Start.Z);

		if (bHit)
		{
			Actor->Modify();
			Actor->SetActorLocation(Hit.Location);
			TFM_QueueActorPackageDirty(Scope, Actor);

			Entry->SetNumberField(TEXT("new_z"), Hit.Location.Z);
			if (AActor* HitActor = Hit.GetActor())
			{
				Entry->SetStringField(TEXT("hit_actor"),
					FMCPActorPathUtils::BuildActorPath(HitActor));
			}
			else
			{
				Entry->SetField(TEXT("hit_actor"), MakeShared<FJsonValueNull>());
			}
			++Snapped;
		}
		else
		{
			Entry->SetNumberField(TEXT("new_z"), Start.Z); // unchanged
			Entry->SetField(TEXT("hit_actor"), MakeShared<FJsonValueNull>());
			++Missed;
		}
		Results.Add(MakeShared<FJsonValueObject>(Entry));
	}

	// All paths failed to resolve (note: misses with successful resolution count as resolved).
	if (ResolutionFailures == Paths.Num())
	{
		Scope.Abort();
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("transform.snap_to_floor: 0/%d actors resolved; first reason: %s"),
				Paths.Num(),
				FirstResolutionReason.IsEmpty() ? TEXT("(empty)") : *FirstResolutionReason));
	}

	return FMCPJsonBuilder()
		.Num(TEXT("snapped"), Snapped)
		.Num(TEXT("missed"),  Missed)
		.Arr(TEXT("results"),  MoveTemp(Results))
		.BuildSuccess(Request);
}

// ─── transform.align ──────────────────────────────────────────────────────────────────────────
//
// Args:
//   actor_paths : [string]               required, non-empty
//   axis        : "X" | "Y" | "Z"        required (case-insensitive)
//   value       : number                 required when mode=="set"; ignored otherwise
//   mode        : "set" | "min" | "max" | "average"   required (case-insensitive)
//
// mode=set     : write supplied ``value`` to ``axis`` for every actor.
// mode=min/max : compute the min/max of all resolved actors' current axis values, apply to all.
// mode=average : arithmetic mean across all resolved actors' current axis values, apply to all.
//
// Other two axes are preserved per actor (we don't normalise X+Y when aligning on Z).
//
// Result:
//   { aligned: int, axis: "X"|"Y"|"Z", value: number, mode: string,
//     failed: int, failures: [{ actor_path, reason }, ...] }
//
// Top-level errors:
//   -32602 InvalidParams   missing args / actor_paths empty / unknown axis or mode /
//                          value missing for mode=set / 0 actors resolved for mode=min/max/average
//   -32027 PIEActive       editor-world mutator refused during PIE
//   -32004 ObjectNotFound  mode=set with 0 actors resolved (no aggregate to compute)
FMCPResponse Tool_Align(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_TransformAlign", "MCP: Align Actors"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams, TEXT("missing args object"));
	}

	TArray<FString> Paths;
	FString ParseErr;
	if (!TFM_ReadActorPaths(Request.Args, Paths, ParseErr))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams, ParseErr);
	}

	FString AxisStr;
	if (!Request.Args->TryGetStringField(TEXT("axis"), AxisStr) || AxisStr.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			TEXT("missing required string field 'axis' (expected 'X', 'Y', or 'Z')"));
	}
	int32 AxisIdx = -1;
	if      (AxisStr.Equals(TEXT("X"), ESearchCase::IgnoreCase)) { AxisIdx = 0; }
	else if (AxisStr.Equals(TEXT("Y"), ESearchCase::IgnoreCase)) { AxisIdx = 1; }
	else if (AxisStr.Equals(TEXT("Z"), ESearchCase::IgnoreCase)) { AxisIdx = 2; }
	else
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			FString::Printf(TEXT("axis '%s' not recognised; expected one of 'X', 'Y', 'Z'"), *AxisStr));
	}

	FString ModeStr;
	if (!Request.Args->TryGetStringField(TEXT("mode"), ModeStr) || ModeStr.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			TEXT("missing required string field 'mode' (expected 'set', 'min', 'max', or 'average')"));
	}

	enum class EMode : uint8 { Set, Min, Max, Average };
	EMode Mode;
	if      (ModeStr.Equals(TEXT("set"),     ESearchCase::IgnoreCase)) { Mode = EMode::Set; }
	else if (ModeStr.Equals(TEXT("min"),     ESearchCase::IgnoreCase)) { Mode = EMode::Min; }
	else if (ModeStr.Equals(TEXT("max"),     ESearchCase::IgnoreCase)) { Mode = EMode::Max; }
	else if (ModeStr.Equals(TEXT("average"), ESearchCase::IgnoreCase)) { Mode = EMode::Average; }
	else
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			FString::Printf(TEXT("mode '%s' not recognised; expected one of 'set', 'min', 'max', 'average'"),
				*ModeStr));
	}

	double SuppliedValue = 0.0;
	const bool bHaveValue = Request.Args->TryGetNumberField(TEXT("value"), SuppliedValue);
	if (Mode == EMode::Set && !bHaveValue)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			TEXT("transform.align mode='set' requires numeric field 'value'"));
	}

	// Pass 1 — resolve all actors and (for aggregate modes) collect current axis values.
	struct FResolved
	{
		AActor* Actor;
		double AxisValue;
	};
	TArray<FResolved> Resolved;
	Resolved.Reserve(Paths.Num());
	TArray<TSharedPtr<FJsonValue>> Failures;
	Failures.Reserve(4);

	for (const FString& Path : Paths)
	{
		FString Reason;
		AActor* Actor = TFM_ResolveActorOrReason(Path, Reason);
		if (!Actor)
		{
			Failures.Add(MakeShared<FJsonValueObject>(TFM_MakeFailureEntry(Path, Reason)));
			continue;
		}
		if (!Actor->GetRootComponent())
		{
			Failures.Add(MakeShared<FJsonValueObject>(TFM_MakeFailureEntry(
				FMCPActorPathUtils::BuildActorPath(Actor),
				TEXT("no root component (cannot transform)"))));
			continue;
		}
		const FVector Loc = Actor->GetActorLocation();
		Resolved.Add({Actor, Loc.Component(AxisIdx)});
	}

	if (Resolved.Num() == 0)
	{
		Scope.Abort();
		if (Mode == EMode::Set)
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
				FString::Printf(TEXT("transform.align: 0/%d actors resolved"), Paths.Num()));
		}
		// min/max/average: aggregating empty set is meaningless.
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			FString::Printf(
				TEXT("transform.align mode='%s': 0/%d actors resolved — cannot aggregate empty set"),
				*ModeStr, Paths.Num()));
	}

	// Pass 2 — compute the value to apply.
	double FinalValue = SuppliedValue;
	switch (Mode)
	{
	case EMode::Set:
		FinalValue = SuppliedValue;
		break;
	case EMode::Min:
		FinalValue = TNumericLimits<double>::Max();
		for (const FResolved& R : Resolved)
		{
			FinalValue = FMath::Min(FinalValue, R.AxisValue);
		}
		break;
	case EMode::Max:
		FinalValue = TNumericLimits<double>::Lowest();
		for (const FResolved& R : Resolved)
		{
			FinalValue = FMath::Max(FinalValue, R.AxisValue);
		}
		break;
	case EMode::Average:
	{
		double Sum = 0.0;
		for (const FResolved& R : Resolved) { Sum += R.AxisValue; }
		FinalValue = Sum / static_cast<double>(Resolved.Num());
		break;
	}
	}

	// Pass 3 — write to every resolved actor inside the single batch transaction.
	int32 Aligned = 0;
	for (const FResolved& R : Resolved)
	{
		R.Actor->Modify();
		FVector Loc = R.Actor->GetActorLocation();
		Loc.Component(AxisIdx) = FinalValue;
		R.Actor->SetActorLocation(Loc);
		TFM_QueueActorPackageDirty(Scope, R.Actor);
		++Aligned;
	}

	const int32 FailuresNum = Failures.Num();
	return FMCPJsonBuilder()
		.Num(TEXT("aligned"), Aligned)
		.Num(TEXT("failed"),  FailuresNum)
		.Str(TEXT("axis"),
			AxisIdx == 0 ? FString(TEXT("X")) : (AxisIdx == 1 ? FString(TEXT("Y")) : FString(TEXT("Z"))))
		.Num(TEXT("value"),   FinalValue)
		.Str(TEXT("mode"),    ModeStr.ToLower())
		.Arr(TEXT("failures"), MoveTemp(Failures))
		.BuildSuccess(Request);
}

// ─── Registration ─────────────────────────────────────────────────────────────────────────────
void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	RegisterTool(TEXT("transform.batch_set"),     &Tool_BatchSet,     /*Lane A*/ false);
	RegisterTool(TEXT("transform.snap_to_floor"), &Tool_SnapToFloor,  /*Lane A*/ false);
	RegisterTool(TEXT("transform.align"),         &Tool_Align,        /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("Wave D Surface 6 (Transform): registered 3 transform.* handlers ")
		TEXT("(batch_set + snap_to_floor + align, all Lane A, all PIE-guarded)"));
}

} // namespace FTransformTools

#undef LOCTEXT_NAMESPACE

MCP_REGISTER_SURFACE(TransformTools, &FTransformTools::Register)
