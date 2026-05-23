// Copyright FatumGame. All Rights Reserved.

#include "DebugTools.h"

#include "MCPSurfaceRegistry.h"

#include "FMCPDispatchQueue.h"
#include "MCPJsonBuilder.h"
#include "MCPToolHelpers.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPWorldContext.h"

#include "DrawDebugHelpers.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Math/Color.h"
#include "Math/Quat.h"
#include "Math/Rotator.h"
#include "Math/Vector.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	constexpr int32 kDBGErrorInternal = -32603;

	// ─── World resolution ────────────────────────────────────────────────────────────────────────
	//
	// Mirrors PhysicsTools' approach: PIE world first (matches "draw in the window the user is
	// watching"), editor world fallback. Returns null only when GEditor itself is missing.

	UWorld* DBG_ResolveWorld()
	{
		check(IsInGameThread());
		if (GEditor && GEditor->PlayWorld)
		{
			return GEditor->PlayWorld;
		}
		return FMCPWorldContext::GetEditorWorld();
	}

	const TCHAR* DBG_WorldKindName(const UWorld* World)
	{
		if (!World) { return TEXT("none"); }
		return World->WorldType == EWorldType::PIE ? TEXT("pie") : TEXT("editor");
	}

	// ─── JSON argument parsing helpers ───────────────────────────────────────────────────────────

	/** Parse required [x,y,z] array. Returns false + populates OutError when missing or malformed. */
	bool DBG_ParseVector3(const TSharedPtr<FJsonObject>& Args, const TCHAR* Field, FVector& Out, FString& OutError)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Args.IsValid() || !Args->TryGetArrayField(Field, Arr) || !Arr)
		{
			OutError = FString::Printf(TEXT("missing required array field '%s' ([x,y,z])"), Field);
			return false;
		}
		if (Arr->Num() != 3)
		{
			OutError = FString::Printf(
				TEXT("'%s' must be [x,y,z] (3 numbers); got %d entries"), Field, Arr->Num());
			return false;
		}
		double X = 0.0, Y = 0.0, Z = 0.0;
		if (!(*Arr)[0]->TryGetNumber(X) || !(*Arr)[1]->TryGetNumber(Y) || !(*Arr)[2]->TryGetNumber(Z))
		{
			OutError = FString::Printf(TEXT("'%s' entries must all be numbers"), Field);
			return false;
		}
		Out = FVector(X, Y, Z);
		return true;
	}

	/** Optional [r,g,b] or [r,g,b,a] in 0..1 float range. Defaults to white if missing or wrong shape. */
	FLinearColor DBG_ParseColor(const TSharedPtr<FJsonObject>& Args, const TCHAR* Field = TEXT("color"))
	{
		if (!Args.IsValid()) { return FLinearColor::White; }
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Args->TryGetArrayField(Field, Arr) || !Arr || Arr->Num() < 3) { return FLinearColor::White; }
		FLinearColor C;
		C.R = static_cast<float>((*Arr)[0]->AsNumber());
		C.G = static_cast<float>((*Arr)[1]->AsNumber());
		C.B = static_cast<float>((*Arr)[2]->AsNumber());
		C.A = Arr->Num() >= 4 ? static_cast<float>((*Arr)[3]->AsNumber()) : 1.0f;
		return C;
	}

	/** Optional number with default. Returns default if field missing OR not a number. */
	double DBG_ReadNumberOr(const TSharedPtr<FJsonObject>& Args, const TCHAR* Field, double Default)
	{
		if (!Args.IsValid()) { return Default; }
		double Out = Default;
		return Args->TryGetNumberField(Field, Out) ? Out : Default;
	}

	/** Optional bool with default. Returns default if field missing. */
	bool DBG_ReadBoolOr(const TSharedPtr<FJsonObject>& Args, const TCHAR* Field, bool Default)
	{
		if (!Args.IsValid()) { return Default; }
		bool Out = Default;
		return Args->TryGetBoolField(Field, Out) ? Out : Default;
	}

	/** Build the standard success response: { drawn|cleared: true, world: "editor"|"pie" }. */
	FMCPResponse DBG_MakeDrawSuccess(const FMCPRequest& Request, const UWorld* World, const TCHAR* Verb = TEXT("drawn"))
	{
		return FMCPJsonBuilder()
			.Bool(Verb, true)
			.Str(TEXT("world"), DBG_WorldKindName(World))
			.BuildSuccess(Request);
	}
} // namespace

namespace FDebugTools
{

// ─── debug.draw_line ──────────────────────────────────────────────────────────────────────────
//
// Args:    { start: [x,y,z], end: [x,y,z], color?: [r,g,b,a] (0..1), thickness?: float,
//            lifetime?: float (-1 = single-frame OR persistent depending on `persistent`),
//            persistent?: bool }
// Result:  { drawn: true, world: "editor"|"pie" }
FMCPResponse Tool_DrawLine(const FMCPRequest& Request)
{
	check(IsInGameThread());

	UWorld* World = DBG_ResolveWorld();
	if (!World)
	{
		return FMCPToolHelpers::MakeError(Request, kDBGErrorInternal, TEXT("no world available (GEditor missing — commandlet/cooker?)"));
	}

	FVector Start, End;
	FString Err;
	if (!DBG_ParseVector3(Request.Args, TEXT("start"), Start, Err)) { return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams, Err); }
	if (!DBG_ParseVector3(Request.Args, TEXT("end"),   End,   Err)) { return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams, Err); }

	const FColor Color      = DBG_ParseColor(Request.Args).ToFColor(/*sRGB*/ true);
	const float Thickness   = static_cast<float>(DBG_ReadNumberOr(Request.Args, TEXT("thickness"), 0.0));
	const float Lifetime    = static_cast<float>(DBG_ReadNumberOr(Request.Args, TEXT("lifetime"),  -1.0));
	const bool bPersistent  = DBG_ReadBoolOr(Request.Args, TEXT("persistent"), false);

	DrawDebugLine(World, Start, End, Color, bPersistent, Lifetime, /*DepthPriority*/ 0, Thickness);

	return DBG_MakeDrawSuccess(Request, World);
}

// ─── debug.draw_sphere ────────────────────────────────────────────────────────────────────────
//
// Args:    { center: [x,y,z], radius: float, color?, segments?: int (default 16),
//            lifetime?, persistent?, thickness? }
FMCPResponse Tool_DrawSphere(const FMCPRequest& Request)
{
	check(IsInGameThread());

	UWorld* World = DBG_ResolveWorld();
	if (!World)
	{
		return FMCPToolHelpers::MakeError(Request, kDBGErrorInternal, TEXT("no world available"));
	}

	FVector Center;
	FString Err;
	if (!DBG_ParseVector3(Request.Args, TEXT("center"), Center, Err)) { return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams, Err); }

	double RadiusD = 0.0;
	if (!Request.Args.IsValid() || !Request.Args->TryGetNumberField(TEXT("radius"), RadiusD))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams, TEXT("missing required number field 'radius'"));
	}

	const float Radius      = static_cast<float>(RadiusD);
	const int32 Segments    = FMath::Clamp(static_cast<int32>(DBG_ReadNumberOr(Request.Args, TEXT("segments"), 16.0)), 4, 256);
	const FColor Color      = DBG_ParseColor(Request.Args).ToFColor(/*sRGB*/ true);
	const float Thickness   = static_cast<float>(DBG_ReadNumberOr(Request.Args, TEXT("thickness"), 0.0));
	const float Lifetime    = static_cast<float>(DBG_ReadNumberOr(Request.Args, TEXT("lifetime"),  -1.0));
	const bool bPersistent  = DBG_ReadBoolOr(Request.Args, TEXT("persistent"), false);

	DrawDebugSphere(World, Center, Radius, Segments, Color, bPersistent, Lifetime, /*DepthPriority*/ 0, Thickness);

	return DBG_MakeDrawSuccess(Request, World);
}

// ─── debug.draw_box ───────────────────────────────────────────────────────────────────────────
//
// Args:    { center: [x,y,z], extent: [x,y,z], rotation?: [pitch,yaw,roll] degrees,
//            color?, lifetime?, persistent?, thickness? }
FMCPResponse Tool_DrawBox(const FMCPRequest& Request)
{
	check(IsInGameThread());

	UWorld* World = DBG_ResolveWorld();
	if (!World)
	{
		return FMCPToolHelpers::MakeError(Request, kDBGErrorInternal, TEXT("no world available"));
	}

	FVector Center, Extent;
	FString Err;
	if (!DBG_ParseVector3(Request.Args, TEXT("center"), Center, Err)) { return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams, Err); }
	if (!DBG_ParseVector3(Request.Args, TEXT("extent"), Extent, Err)) { return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams, Err); }

	// Rotation is optional — default to identity (axis-aligned box).
	FQuat Rotation = FQuat::Identity;
	if (Request.Args.IsValid() && Request.Args->HasField(TEXT("rotation")))
	{
		FVector RotVec;
		if (!DBG_ParseVector3(Request.Args, TEXT("rotation"), RotVec, Err))
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams, Err);
		}
		// [pitch, yaw, roll] degrees → FRotator.
		Rotation = FRotator(static_cast<float>(RotVec.X), static_cast<float>(RotVec.Y), static_cast<float>(RotVec.Z)).Quaternion();
	}

	const FColor Color      = DBG_ParseColor(Request.Args).ToFColor(/*sRGB*/ true);
	const float Thickness   = static_cast<float>(DBG_ReadNumberOr(Request.Args, TEXT("thickness"), 0.0));
	const float Lifetime    = static_cast<float>(DBG_ReadNumberOr(Request.Args, TEXT("lifetime"),  -1.0));
	const bool bPersistent  = DBG_ReadBoolOr(Request.Args, TEXT("persistent"), false);

	DrawDebugBox(World, Center, Extent, Rotation, Color, bPersistent, Lifetime, /*DepthPriority*/ 0, Thickness);

	return DBG_MakeDrawSuccess(Request, World);
}

// ─── debug.draw_arrow ─────────────────────────────────────────────────────────────────────────
//
// Args:    { start: [x,y,z], end: [x,y,z], arrow_size?: float (default 40),
//            color?, lifetime?, persistent?, thickness? }
FMCPResponse Tool_DrawArrow(const FMCPRequest& Request)
{
	check(IsInGameThread());

	UWorld* World = DBG_ResolveWorld();
	if (!World)
	{
		return FMCPToolHelpers::MakeError(Request, kDBGErrorInternal, TEXT("no world available"));
	}

	FVector Start, End;
	FString Err;
	if (!DBG_ParseVector3(Request.Args, TEXT("start"), Start, Err)) { return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams, Err); }
	if (!DBG_ParseVector3(Request.Args, TEXT("end"),   End,   Err)) { return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams, Err); }

	const float ArrowSize   = static_cast<float>(DBG_ReadNumberOr(Request.Args, TEXT("arrow_size"), 40.0));
	const FColor Color      = DBG_ParseColor(Request.Args).ToFColor(/*sRGB*/ true);
	const float Thickness   = static_cast<float>(DBG_ReadNumberOr(Request.Args, TEXT("thickness"), 0.0));
	const float Lifetime    = static_cast<float>(DBG_ReadNumberOr(Request.Args, TEXT("lifetime"),  -1.0));
	const bool bPersistent  = DBG_ReadBoolOr(Request.Args, TEXT("persistent"), false);

	DrawDebugDirectionalArrow(World, Start, End, ArrowSize, Color, bPersistent, Lifetime, /*DepthPriority*/ 0, Thickness);

	return DBG_MakeDrawSuccess(Request, World);
}

// ─── debug.draw_text ──────────────────────────────────────────────────────────────────────────
//
// Args:    { location: [x,y,z], text: string, color?, lifetime?,
//            draw_shadow?: bool (default true), font_scale?: float (default 1.0) }
//
// Note: DrawDebugString uses a different signature than the other DrawDebug* functions — there
// is no `bPersistent` flag; instead, `Duration = -1.0` means "persistent". We expose `lifetime`
// (matching the other tools' naming) and forward it as Duration directly.
FMCPResponse Tool_DrawText(const FMCPRequest& Request)
{
	check(IsInGameThread());

	UWorld* World = DBG_ResolveWorld();
	if (!World)
	{
		return FMCPToolHelpers::MakeError(Request, kDBGErrorInternal, TEXT("no world available"));
	}

	FVector Location;
	FString Err;
	if (!DBG_ParseVector3(Request.Args, TEXT("location"), Location, Err)) { return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams, Err); }

	FString Text;
	if (!Request.Args.IsValid() || !Request.Args->TryGetStringField(TEXT("text"), Text))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams, TEXT("missing required string field 'text'"));
	}

	const FColor Color      = DBG_ParseColor(Request.Args).ToFColor(/*sRGB*/ true);
	const float Duration    = static_cast<float>(DBG_ReadNumberOr(Request.Args, TEXT("lifetime"), -1.0));
	const bool bDrawShadow  = DBG_ReadBoolOr(Request.Args, TEXT("draw_shadow"), true);
	const float FontScale   = static_cast<float>(DBG_ReadNumberOr(Request.Args, TEXT("font_scale"), 1.0));

	DrawDebugString(World, Location, Text, /*TestBaseActor*/ nullptr, Color, Duration, bDrawShadow, FontScale);

	return DBG_MakeDrawSuccess(Request, World);
}

// ─── debug.clear ──────────────────────────────────────────────────────────────────────────────
//
// Args:    (none)
// Result:  { cleared: true, world: "editor"|"pie" }
//
// Clears BOTH the persistent debug line batcher AND world-space debug strings. Per-frame
// (non-persistent) lines clear themselves on the next render tick — caller doesn't need to
// invoke `debug.clear` for those.
FMCPResponse Tool_Clear(const FMCPRequest& Request)
{
	check(IsInGameThread());

	UWorld* World = DBG_ResolveWorld();
	if (!World)
	{
		return FMCPToolHelpers::MakeError(Request, kDBGErrorInternal, TEXT("no world available"));
	}

	FlushPersistentDebugLines(World);
	FlushDebugStrings(World);

	return DBG_MakeDrawSuccess(Request, World, /*Verb*/ TEXT("cleared"));
}

void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	RegisterTool(TEXT("debug.draw_line"),   &Tool_DrawLine,   /*Lane A*/ false);
	RegisterTool(TEXT("debug.draw_sphere"), &Tool_DrawSphere, /*Lane A*/ false);
	RegisterTool(TEXT("debug.draw_box"),    &Tool_DrawBox,    /*Lane A*/ false);
	RegisterTool(TEXT("debug.draw_arrow"),  &Tool_DrawArrow,  /*Lane A*/ false);
	RegisterTool(TEXT("debug.draw_text"),   &Tool_DrawText,   /*Lane A*/ false);
	RegisterTool(TEXT("debug.clear"),       &Tool_Clear,      /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("Debug surface registered: 6 tools "
			 "(draw_line + draw_sphere + draw_box + draw_arrow + draw_text + clear), all Lane A"));
}

} // namespace FDebugTools

MCP_REGISTER_SURFACE(DebugTools, &FDebugTools::Register)
