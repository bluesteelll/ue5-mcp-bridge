// Copyright FatumGame. All Rights Reserved.

#include "ViewportTools.h"

#include "MCPSurfaceRegistry.h"

#include "FMCPDispatchQueue.h"
#include "MCPToolHelpers.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPActorPathUtils.h"

#include "Editor.h"
#include "EditorViewportClient.h"
#include "Editor/UnrealEdTypes.h"
#include "Engine/Engine.h"
#include "GameFramework/Actor.h"
#include "LevelEditorViewport.h"
#include "Math/Box.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	// VPT_ prefix per the unity-build symbol-collision convention.
	constexpr int32 kVPTErrorInternal = -32603;

	/** Convert FVector to JSON [x, y, z] array. */
	TArray<TSharedPtr<FJsonValue>> VPT_VectorToArray(const FVector& V)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Add(MakeShared<FJsonValueNumber>(V.X));
		Arr.Add(MakeShared<FJsonValueNumber>(V.Y));
		Arr.Add(MakeShared<FJsonValueNumber>(V.Z));
		return Arr;
	}

	/** Convert FRotator to JSON [pitch, yaw, roll] array. */
	TArray<TSharedPtr<FJsonValue>> VPT_RotatorToArray(const FRotator& R)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Add(MakeShared<FJsonValueNumber>(R.Pitch));
		Arr.Add(MakeShared<FJsonValueNumber>(R.Yaw));
		Arr.Add(MakeShared<FJsonValueNumber>(R.Roll));
		return Arr;
	}

	/** Read a JSON [x, y, z] array into FVector. Returns false on missing/malformed. */
	bool VPT_ReadVectorArray(
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

	/**
	 * Translate ELevelViewportType to a stable wire string. We map the public LVT_ enum to
	 * descriptive english names; aliases (LVT_OrthoTop = LVT_OrthoXY, etc.) collapse to the
	 * underlying value so the wire surface stays unambiguous.
	 */
	const TCHAR* VPT_ViewportTypeToString(ELevelViewportType Type)
	{
		switch (Type)
		{
		case LVT_Perspective:      return TEXT("perspective");
		case LVT_OrthoXY:          return TEXT("orthographic_top");     // alias LVT_OrthoTop
		case LVT_OrthoXZ:          return TEXT("orthographic_left");    // alias LVT_OrthoLeft (XZ plane = side view from +Y)
		case LVT_OrthoYZ:          return TEXT("orthographic_back");    // alias LVT_OrthoBack  (YZ plane)
		case LVT_OrthoFreelook:    return TEXT("orthographic_freelook");
		case LVT_OrthoNegativeXY:  return TEXT("orthographic_bottom");  // alias LVT_OrthoBottom
		case LVT_OrthoNegativeXZ:  return TEXT("orthographic_right");   // alias LVT_OrthoRight
		case LVT_OrthoNegativeYZ:  return TEXT("orthographic_front");   // alias LVT_OrthoFront
		case LVT_None:             return TEXT("none");
		default:                   return TEXT("unknown");
		}
	}

	/**
	 * Build a JSON object describing one viewport's camera state. Used by both list and get_camera
	 * to guarantee the per-viewport schema is identical across endpoints.
	 */
	TSharedRef<FJsonObject> VPT_BuildViewportEntry(int32 Index, FLevelEditorViewportClient* VC)
	{
		check(VC != nullptr);

		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("viewport_index"), Index);
		Obj->SetStringField(TEXT("viewport_type"), VPT_ViewportTypeToString(VC->ViewportType));
		// "Active" mirrors the editor's hovered/focused viewport (set by mouse enter / key input).
		// We don't rely on the SLevelViewport widget — VC->bIsRealtime is unrelated; the correct
		// signal is "this client is the current global one". The two globals together cover the
		// usual hover-or-last-key-input semantics.
		const bool bIsActive =
			(VC == GCurrentLevelEditingViewportClient)
			|| (VC == GLastKeyLevelEditingViewportClient);
		Obj->SetBoolField(TEXT("is_active"), bIsActive);

		Obj->SetArrayField(TEXT("camera_location"), VPT_VectorToArray(VC->GetViewLocation()));
		Obj->SetArrayField(TEXT("camera_rotation"), VPT_RotatorToArray(VC->GetViewRotation()));
		Obj->SetNumberField(TEXT("fov"), VC->ViewFOV);
		if (VC->IsOrtho())
		{
			// Ortho zoom is a separate numeric handle (not an angle). Surface it conditionally so
			// downstream callers can round-trip it back through set_camera in a future revision.
			Obj->SetNumberField(TEXT("orthographic_zoom"), VC->GetOrthoZoom());
		}
		return Obj;
	}

	/**
	 * Read viewport_index from args with default 0. Returns false on present-but-OOB or
	 * present-but-malformed. Populates ``OutErr`` for caller surfacing.
	 *
	 * ``ViewportCount`` is checked against — we surface PropertyIndexOOB (-32026) for OOB to
	 * match the existing semantic family (out-of-range index on a list-shaped resource).
	 */
	bool VPT_ResolveViewportIndex(
		const FMCPRequest& Request,
		int32 ViewportCount,
		int32& OutIndex,
		FMCPResponse& OutErr)
	{
		OutIndex = 0;
		if (Request.Args.IsValid())
		{
			double Raw = 0.0;
			if (Request.Args->TryGetNumberField(TEXT("viewport_index"), Raw))
			{
				OutIndex = static_cast<int32>(Raw);
			}
		}
		if (OutIndex < 0 || OutIndex >= ViewportCount)
		{
			OutErr = FMCPToolHelpers::MakeError(Request, kMCPErrorPropertyIndexOOB,
				FString::Printf(
					TEXT("viewport_index %d out of range [0, %d)"),
					OutIndex, ViewportCount));
			return false;
		}
		return true;
	}

	/**
	 * Resolve a viewport client by index against GEditor->GetLevelViewportClients(). Returns
	 * null + populates OutErr on failure (no editor / no viewports / OOB). Callers that want
	 * the count separately should call GEditor->GetLevelViewportClients() directly.
	 */
	FLevelEditorViewportClient* VPT_ResolveViewport(const FMCPRequest& Request, FMCPResponse& OutErr)
	{
		check(IsInGameThread());

		if (!GEditor)
		{
			OutErr = FMCPToolHelpers::MakeError(Request, kVPTErrorInternal,
				TEXT("GEditor unavailable (commandlet?)"));
			return nullptr;
		}
		const TArray<FLevelEditorViewportClient*>& Clients = GEditor->GetLevelViewportClients();
		if (Clients.Num() == 0)
		{
			OutErr = FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
				TEXT("no level viewports available (open a level editor tab first)"));
			return nullptr;
		}

		int32 Index = 0;
		if (!VPT_ResolveViewportIndex(Request, Clients.Num(), Index, OutErr))
		{
			return nullptr;
		}

		FLevelEditorViewportClient* VC = Clients[Index];
		if (!VC)
		{
			OutErr = FMCPToolHelpers::MakeError(Request, kVPTErrorInternal,
				FString::Printf(TEXT("level viewport client at index %d is null"), Index));
			return nullptr;
		}
		return VC;
	}

	/** Snapshot {location, rotation, fov} into a JSON object — for the prior/new diff blocks. */
	TSharedRef<FJsonObject> VPT_SnapshotCamera(FLevelEditorViewportClient* VC)
	{
		check(VC != nullptr);
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetArrayField(TEXT("location"), VPT_VectorToArray(VC->GetViewLocation()));
		Obj->SetArrayField(TEXT("rotation"), VPT_RotatorToArray(VC->GetViewRotation()));
		Obj->SetNumberField(TEXT("fov"), VC->ViewFOV);
		return Obj;
	}

	int32 VPT_FindViewportIndex(FLevelEditorViewportClient* VC)
	{
		if (!GEditor || !VC) { return INDEX_NONE; }
		const TArray<FLevelEditorViewportClient*>& Clients = GEditor->GetLevelViewportClients();
		return Clients.IndexOfByKey(VC);
	}
} // namespace

namespace FViewportTools
{

// ─── viewport.list ─────────────────────────────────────────────────────────────────────────────
//
// Args: {}
// Result: {
//   viewports: [{ viewport_index, viewport_type, is_active, camera_location, camera_rotation,
//                 fov, orthographic_zoom? }],
//   count: int
// }
FMCPResponse Tool_List(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!GEditor)
	{
		return FMCPToolHelpers::MakeError(Request, kVPTErrorInternal,
			TEXT("GEditor unavailable (commandlet?)"));
	}

	const TArray<FLevelEditorViewportClient*>& Clients = GEditor->GetLevelViewportClients();
	if (Clients.Num() == 0)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			TEXT("no level viewports available (open a level editor tab first)"));
	}

	TArray<TSharedPtr<FJsonValue>> Arr;
	Arr.Reserve(Clients.Num());
	for (int32 Idx = 0; Idx < Clients.Num(); ++Idx)
	{
		FLevelEditorViewportClient* VC = Clients[Idx];
		if (!VC) { continue; } // Defensive — null slot in the array would be a UE bug.
		Arr.Add(MakeShared<FJsonValueObject>(VPT_BuildViewportEntry(Idx, VC)));
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetArrayField(TEXT("viewports"), Arr);
	Out->SetNumberField(TEXT("count"), Arr.Num());
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── viewport.get_camera ───────────────────────────────────────────────────────────────────────
//
// Args: { viewport_index?: int (default 0) }
// Result: { viewport_index, viewport_type, camera_location, camera_rotation, fov,
//           is_active, orthographic_zoom? }
FMCPResponse Tool_GetCamera(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPResponse Err;
	FLevelEditorViewportClient* VC = VPT_ResolveViewport(Request, Err);
	if (!VC) { return Err; }

	const int32 Index = VPT_FindViewportIndex(VC);
	TSharedRef<FJsonObject> Out = VPT_BuildViewportEntry(Index, VC);
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── viewport.set_camera ───────────────────────────────────────────────────────────────────────
//
// Args: {
//   viewport_index?: int (default 0),
//   location?: [x, y, z],
//   rotation?: [pitch, yaw, roll],
//   fov?: number  (range (1.0, 175.0))
// }
//
// Result: {
//   viewport_index,
//   prior: { location, rotation, fov },
//   new:   { location, rotation, fov }
// }
//
// At least one of location/rotation/fov MUST be present; absent everything → -32602
// (no-op would be misleading — caller should use viewport.get_camera for read-only queries).
FMCPResponse Tool_SetCamera(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams, TEXT("missing args object"));
	}

	// Parse optional fields up-front to fail-fast on malformed input BEFORE we resolve the
	// viewport (cheaper diagnostic when the index also happens to be invalid).
	const TArray<TSharedPtr<FJsonValue>>* LocArrPtr = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* RotArrPtr = nullptr;
	const bool bHasLocation = Request.Args->TryGetArrayField(TEXT("location"), LocArrPtr) && LocArrPtr;
	const bool bHasRotation = Request.Args->TryGetArrayField(TEXT("rotation"), RotArrPtr) && RotArrPtr;
	double FovRaw = 0.0;
	const bool bHasFov = Request.Args->TryGetNumberField(TEXT("fov"), FovRaw);

	if (!bHasLocation && !bHasRotation && !bHasFov)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			TEXT("viewport.set_camera requires at least one of: location, rotation, fov"));
	}

	FVector NewLocation = FVector::ZeroVector;
	FString Err;
	if (bHasLocation && !VPT_ReadVectorArray(Request.Args, TEXT("location"), NewLocation, Err))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams, Err);
	}

	FRotator NewRotation = FRotator::ZeroRotator;
	if (bHasRotation)
	{
		FVector RotVec = FVector::ZeroVector;
		if (!VPT_ReadVectorArray(Request.Args, TEXT("rotation"), RotVec, Err))
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams, Err);
		}
		NewRotation = FRotator(RotVec.X, RotVec.Y, RotVec.Z); // pitch, yaw, roll
	}

	if (bHasFov && (FovRaw < 1.0 || FovRaw > 175.0))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			FString::Printf(TEXT("fov %g out of range (1.0, 175.0)"), FovRaw));
	}

	// Resolve viewport AFTER param validation.
	FMCPResponse ResolveErr;
	FLevelEditorViewportClient* VC = VPT_ResolveViewport(Request, ResolveErr);
	if (!VC) { return ResolveErr; }

	// Snapshot prior state BEFORE mutating so the response can carry a round-tripable diff
	// for callers that want to undo the change manually (viewport state isn't on the undo stack).
	TSharedRef<FJsonObject> PriorObj = VPT_SnapshotCamera(VC);

	if (bHasLocation) { VC->SetViewLocation(NewLocation); }
	if (bHasRotation) { VC->SetViewRotation(NewRotation); }
	if (bHasFov)      { VC->ViewFOV = static_cast<float>(FovRaw); }
	VC->Invalidate(); // Force immediate redraw with the new view.

	TSharedRef<FJsonObject> NewObj = VPT_SnapshotCamera(VC);

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetNumberField(TEXT("viewport_index"), VPT_FindViewportIndex(VC));
	Out->SetObjectField(TEXT("prior"), PriorObj);
	Out->SetObjectField(TEXT("new"),   NewObj);
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── viewport.focus_on_actor ───────────────────────────────────────────────────────────────────
//
// Args: {
//   actor_path: string,
//   viewport_index?: int (default 0),
//   padding?: number (default 1.0)  // RESERVED — UE's FocusViewportOnBox doesn't accept padding;
//                                   // we expand the bounding box by (padding-1.0)*extent on each
//                                   // side to approximate the requested zoom-out factor.
// }
//
// Result: {
//   actor_path, viewport_index,
//   new_camera_location, new_camera_rotation
// }
//
// Uses FLevelEditorViewportClient::FocusViewportOnBox(Bounds, /*bInstant*/ true) — instant=true
// so the caller's subsequent get_camera sees the new state immediately rather than waiting for
// the editor's per-tick interpolation to finish.
FMCPResponse Tool_FocusOnActor(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams, TEXT("missing args object"));
	}

	FString ActorPath;
	if (!Request.Args->TryGetStringField(TEXT("actor_path"), ActorPath) || ActorPath.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			TEXT("missing required string field 'actor_path'"));
	}

	double PaddingRaw = 1.0;
	Request.Args->TryGetNumberField(TEXT("padding"), PaddingRaw);
	const float Padding = FMath::Max(static_cast<float>(PaddingRaw), 1.0f);

	bool bAmbiguous = false;
	FString AmbiguityHint, ResolveErrMsg;
	AActor* Actor = FMCPActorPathUtils::ResolveActor(ActorPath, /*bRejectPIE*/ false,
		bAmbiguous, AmbiguityHint, ResolveErrMsg);
	if (!Actor)
	{
		const FString Msg = bAmbiguous
			? FString::Printf(TEXT("actor '%s' ambiguous: %s"), *ActorPath, *AmbiguityHint)
			: FString::Printf(TEXT("actor '%s' not found: %s"), *ActorPath, *ResolveErrMsg);
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound, Msg);
	}

	FMCPResponse ResolveErr;
	FLevelEditorViewportClient* VC = VPT_ResolveViewport(Request, ResolveErr);
	if (!VC) { return ResolveErr; }

	// Get tight bounds from the actor's registered components, including non-collision geometry
	// (true = include nonColliding so visual-only meshes count toward framing).
	FBox Bounds = Actor->GetComponentsBoundingBox(/*bNonColliding*/ true);
	if (!Bounds.IsValid)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(
				TEXT("actor '%s' has no valid bounding box (no registered components with bounds)"),
				*ActorPath));
	}

	// Expand box by (padding - 1.0)*Extent on each side. padding=1.0 = no expansion (tight fit);
	// 1.5 = 50% extra room around the actor.
	if (Padding > 1.0f)
	{
		const FVector ExpansionExtent = Bounds.GetExtent() * (Padding - 1.0f);
		Bounds = Bounds.ExpandBy(ExpansionExtent);
	}

	VC->FocusViewportOnBox(Bounds, /*bInstant*/ true);
	VC->Invalidate();

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("actor_path"), Actor->GetPathName());
	Out->SetNumberField(TEXT("viewport_index"), VPT_FindViewportIndex(VC));
	Out->SetArrayField(TEXT("new_camera_location"), VPT_VectorToArray(VC->GetViewLocation()));
	Out->SetArrayField(TEXT("new_camera_rotation"), VPT_RotatorToArray(VC->GetViewRotation()));
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	RegisterTool(TEXT("viewport.list"),            &Tool_List,         /*Lane A*/ false);
	RegisterTool(TEXT("viewport.get_camera"),      &Tool_GetCamera,    /*Lane A*/ false);
	RegisterTool(TEXT("viewport.set_camera"),      &Tool_SetCamera,    /*Lane A*/ false);
	RegisterTool(TEXT("viewport.focus_on_actor"),  &Tool_FocusOnActor, /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("Viewport surface registered: 4 tools "
			 "(list + get_camera + set_camera + focus_on_actor), all Lane A"));
}

} // namespace FViewportTools

MCP_REGISTER_SURFACE(ViewportTools, &FViewportTools::Register)
