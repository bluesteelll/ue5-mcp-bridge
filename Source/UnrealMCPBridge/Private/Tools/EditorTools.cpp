// Copyright FatumGame. All Rights Reserved.

#include "EditorTools.h"

#include "FMCPDispatchQueue.h"
#include "MCPToolHelpers.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPActorPathUtils.h"
#include "Utils/MCPPathSandbox.h"
#include "Utils/MCPScreenshotUtils.h"
#include "Utils/MCPWorldContext.h"

#include "Containers/Ticker.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/Selection.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "LevelEditor.h"
#include "LevelEditorViewport.h"
#include "Misc/Base64.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "SLevelViewport.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "UnrealClient.h"
#include "UObject/Package.h"

#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// EDT_ prefix per the unity-build symbol-collision pattern. StampIds / MakeError / MakeSuccessObj
	// migrated to FMCPToolHelpers in Phase 3 (Group G3); only the surface-local error-code aliases,
	// screenshot dimension caps, and the EDT_MakePIENotActiveError shim (still used by pie.* path)
	// live here. editor.* tools are read-mostly + transient editor-state mutators (camera, selection,
	// notifications) — no FScopedTransaction / package-dirty footprint, so FMCPMutatorScope migration
	// is intentionally skipped.
	constexpr int32 kEDTErrorInvalidParams = kMCPErrorInvalidParams; // -32602
	constexpr int32 kEDTErrorInternal      = kMCPErrorInternal;      // -32603

	/** Match Phase 2 asset.batch_metadata's batch cap — large selections also stress Details panel. */
	constexpr int32 kEDTMaxSelectionSize = 200;

	/** Default editor viewport screenshot resolution per Phase 5 plan §C6. */
	constexpr int32 kEDTMemScreenshotDefault   = 768;
	constexpr int32 kEDTMemScreenshotMin       = 32;
	constexpr int32 kEDTMemScreenshotMax       = 2048;

	/** Disk variant defaults — large enough for a 1080p frame; max matches engine's HighRes guard. */
	constexpr int32 kEDTDiskScreenshotDefaultW = 1920;
	constexpr int32 kEDTDiskScreenshotDefaultH = 1080;
	constexpr int32 kEDTDiskScreenshotMin      = 32;
	constexpr int32 kEDTDiskScreenshotMax      = 8192;

	FMCPResponse EDT_MakePIENotActiveError(const FMCPRequest& Request)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPIENotActive, kMCPMessagePIENotActive);
	}

	// ─── Active viewport resolution ─────────────────────────────────────────────────────────────

	/**
	 * Resolve the focused level viewport client. Search order:
	 *   1. ``GCurrentLevelEditingViewportClient``  — viewport the user is currently hovering / interacting with.
	 *   2. ``GLastKeyLevelEditingViewportClient``  — viewport that most recently received key input
	 *      (typical fallback when the user moves the mouse off into the Details panel).
	 *   3. ``FLevelEditorModule::GetFirstActiveLevelViewport()`` — module-level resolved viewport
	 *      based on the active tab; final fallback if the user has no viewport hovered/focused.
	 *
	 * Returns null when no level editor tab exists (commandlet mode / cooked build) OR when all
	 * three paths failed. Callers should surface kEDTErrorInternal with "no level viewport found"
	 * (the Phase 5 plan's NO_VIEWPORT failure mode maps to -32603 since we don't have a dedicated
	 * error code for this case).
	 */
	FLevelEditorViewportClient* EDT_FindActiveLevelViewportClient()
	{
		check(IsInGameThread());

		if (GCurrentLevelEditingViewportClient)
		{
			return GCurrentLevelEditingViewportClient;
		}
		if (GLastKeyLevelEditingViewportClient)
		{
			return GLastKeyLevelEditingViewportClient;
		}

		// Final fallback — module API. Will be null in commandlet/cooked-game contexts.
		if (FLevelEditorModule* LevelEditor = FModuleManager::GetModulePtr<FLevelEditorModule>("LevelEditor"))
		{
			TSharedPtr<SLevelViewport> ActiveViewport = LevelEditor->GetFirstActiveLevelViewport();
			if (ActiveViewport.IsValid())
			{
				return &ActiveViewport->GetLevelViewportClient();
			}
		}
		return nullptr;
	}

	/** Resolve the FViewport from the active level viewport client, or null. */
	FViewport* EDT_FindActiveLevelViewport()
	{
		FLevelEditorViewportClient* Client = EDT_FindActiveLevelViewportClient();
		return Client ? Client->Viewport : nullptr;
	}

	/**
	 * Resolve the PIE game viewport's FViewport. Walks PIE world contexts and returns the first
	 * non-server world's ``GameViewport->Viewport``. Returns null when PIE isn't running OR no
	 * client world is available OR no GameViewport is attached (rare; PIE startup race).
	 */
	FViewport* EDT_FindPIEGameViewport()
	{
		check(IsInGameThread());
		if (!GEngine)
		{
			return nullptr;
		}
		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			if (Ctx.WorldType == EWorldType::PIE && !Ctx.RunAsDedicated)
			{
				UWorld* World = Ctx.World();
				if (!World)
				{
					continue;
				}
				UGameViewportClient* GVC = World->GetGameViewport();
				if (GVC && GVC->Viewport)
				{
					return GVC->Viewport;
				}
			}
		}
		return nullptr;
	}

	// ─── Wire IO helpers ────────────────────────────────────────────────────────────────────────

	/**
	 * Read an integer field from JSON with clamping to ``[Min, Max]``. ``DefaultValue`` is used
	 * when the field is absent. Returns false on present-but-out-of-range with descriptive error.
	 */
	bool EDT_ReadClampedInt(
		const TSharedPtr<FJsonObject>& Args,
		const TCHAR* FieldName,
		int32 DefaultValue,
		int32 Min,
		int32 Max,
		int32& OutValue,
		FString& OutError)
	{
		OutValue = DefaultValue;
		if (!Args.IsValid())
		{
			return true;
		}
		double Raw = 0.0;
		if (!Args->TryGetNumberField(FieldName, Raw))
		{
			return true; // not present → keep default
		}
		const int32 Value = static_cast<int32>(Raw);
		if (Value < Min || Value > Max)
		{
			OutError = FString::Printf(
				TEXT("'%s' must be in [%d, %d]; got %d"), FieldName, Min, Max, Value);
			return false;
		}
		OutValue = Value;
		return true;
	}

	/** Read a JSON [x, y, z] array into FVector. Returns false on missing/malformed with error. */
	bool EDT_ReadVectorArray(
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

	/** Convert FVector to JSON [x, y, z] array. */
	TArray<TSharedPtr<FJsonValue>> EDT_VectorToArray(const FVector& V)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Add(MakeShared<FJsonValueNumber>(V.X));
		Arr.Add(MakeShared<FJsonValueNumber>(V.Y));
		Arr.Add(MakeShared<FJsonValueNumber>(V.Z));
		return Arr;
	}

	/** Convert FRotator to JSON [pitch, yaw, roll] array. */
	TArray<TSharedPtr<FJsonValue>> EDT_RotatorToArray(const FRotator& R)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Add(MakeShared<FJsonValueNumber>(R.Pitch));
		Arr.Add(MakeShared<FJsonValueNumber>(R.Yaw));
		Arr.Add(MakeShared<FJsonValueNumber>(R.Roll));
		return Arr;
	}

	/**
	 * Build an ActorSummary JSON object matching the Phase 3 ``$ref: ActorSummary`` schema.
	 * Returns a minimal subset (path/guid/class/label) — full transforms aren't relevant to
	 * a selection list and would inflate response size.
	 */
	TSharedRef<FJsonObject> EDT_BuildActorSummary(const AActor* Actor)
	{
		check(Actor != nullptr);
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("path"), FMCPActorPathUtils::BuildActorPath(Actor));
		Obj->SetStringField(TEXT("name"), Actor->GetName());
		Obj->SetStringField(TEXT("label"), Actor->GetActorLabel());
		Obj->SetStringField(TEXT("class"), Actor->GetClass()->GetPathName());
		const FGuid Guid = Actor->GetActorGuid();
		Obj->SetStringField(TEXT("actor_guid"),
			Guid.IsValid() ? Guid.ToString(EGuidFormats::DigitsWithHyphens) : FString());
		return Obj;
	}
} // namespace

namespace FEditorTools
{

// ─── editor.viewport_screenshot ────────────────────────────────────────────────────────────────
//
// Args:
//   width  : int [32, 2048] (default 768)
//   height : int [32, 2048] (default 768)
//   format : "png" | "jpg" (default "png")
//
// Response: { base64: string, mime: "image/png"|"image/jpeg", width: int, height: int }
//
// Errors:
//   -32602 InvalidParams  width/height out of range OR format unrecognised
//   -32603 Internal       no level viewport found OR capture/encode failed
//
// Output size: PNG of 2048x2048 ≈ 8-16 MiB → base64 ≈ 11-22 MiB → well under the 64 MiB frame cap.
// Jpg @ Q=85 will be much smaller (~500 KiB-1 MiB) but loses fidelity.
FMCPResponse Tool_ViewportScreenshot(const FMCPRequest& Request)
{
	check(IsInGameThread());

	// Parse width/height with clamping.
	int32 Width = kEDTMemScreenshotDefault;
	int32 Height = kEDTMemScreenshotDefault;
	FString Err;
	if (!EDT_ReadClampedInt(Request.Args, TEXT("width"), kEDTMemScreenshotDefault,
		kEDTMemScreenshotMin, kEDTMemScreenshotMax, Width, Err))
	{
		return FMCPToolHelpers::MakeError(Request, kEDTErrorInvalidParams, Err);
	}
	if (!EDT_ReadClampedInt(Request.Args, TEXT("height"), kEDTMemScreenshotDefault,
		kEDTMemScreenshotMin, kEDTMemScreenshotMax, Height, Err))
	{
		return FMCPToolHelpers::MakeError(Request, kEDTErrorInvalidParams, Err);
	}

	// Parse format.
	FString FormatRaw;
	if (Request.Args.IsValid())
	{
		Request.Args->TryGetStringField(TEXT("format"), FormatRaw);
	}
	FMCPScreenshotUtils::EImageFormat Format = FMCPScreenshotUtils::EImageFormat::PNG;
	if (!FMCPScreenshotUtils::ParseFormat(FormatRaw, Format, Err))
	{
		return FMCPToolHelpers::MakeError(Request, kEDTErrorInvalidParams, Err);
	}

	// Resolve active viewport.
	FViewport* Viewport = EDT_FindActiveLevelViewport();
	if (!Viewport)
	{
		return FMCPToolHelpers::MakeError(Request, kEDTErrorInternal,
			TEXT("no active level viewport (NO_VIEWPORT) — open a level editor tab"));
	}

	// Capture.
	TArray<uint8> Pixels;
	int32 OutW = 0, OutH = 0;
	FString CaptureErr;
	if (!FMCPScreenshotUtils::CaptureViewport(Viewport, Width, Height, Pixels, OutW, OutH, CaptureErr))
	{
		return FMCPToolHelpers::MakeError(Request, kEDTErrorInternal,
			FString::Printf(TEXT("viewport capture failed: %s"), *CaptureErr));
	}

	// Encode (PNG ignores quality; JPG default 85).
	TArray64<uint8> Encoded;
	FString EncodeErr;
	if (!FMCPScreenshotUtils::EncodeImage(Pixels, OutW, OutH, Format, /*JpegQuality*/ 85,
		Encoded, EncodeErr))
	{
		return FMCPToolHelpers::MakeError(Request, kEDTErrorInternal,
			FString::Printf(TEXT("image encode failed: %s"), *EncodeErr));
	}

	// Base64 wrap. UE's FBase64::Encode takes int32; PNG of 2048x2048 fits well under that limit.
	if (Encoded.Num() > INT32_MAX)
	{
		return FMCPToolHelpers::MakeError(Request, kEDTErrorInternal,
			FString::Printf(TEXT("encoded image %lld bytes exceeds 2 GiB — use editor.viewport_screenshot_to_disk"),
				Encoded.Num()));
	}
	const FString Base64 = FBase64::Encode(
		reinterpret_cast<const uint8*>(Encoded.GetData()), static_cast<int32>(Encoded.Num()));

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("base64"), Base64);
	Out->SetStringField(TEXT("mime"),
		Format == FMCPScreenshotUtils::EImageFormat::JPG ? TEXT("image/jpeg") : TEXT("image/png"));
	Out->SetNumberField(TEXT("width"), OutW);
	Out->SetNumberField(TEXT("height"), OutH);
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── editor.viewport_screenshot_to_disk ────────────────────────────────────────────────────────
//
// Args:
//   path    : string (default = "<Saved>/UnrealMCP/screenshots/<uuid>.<ext>")
//   width   : int [32, 8192] (default 1920)
//   height  : int [32, 8192] (default 1080)
//   format  : "png" | "jpg" (default "png")
//   quality : int [0, 100] (default 85, JPG only)
//
// Response: { path: string, bytes: int, width: int, height: int }
//
// Errors:
//   -32602 InvalidParams  param out of range / format unrecognised
//   -32013 PathEscape     resolved path outside sandbox whitelist
//   -32603 Internal       no viewport / capture / encode / write failed
FMCPResponse Tool_ViewportScreenshotToDisk(const FMCPRequest& Request)
{
	check(IsInGameThread());

	// Parse width/height with clamping.
	int32 Width = kEDTDiskScreenshotDefaultW;
	int32 Height = kEDTDiskScreenshotDefaultH;
	FString Err;
	if (!EDT_ReadClampedInt(Request.Args, TEXT("width"), kEDTDiskScreenshotDefaultW,
		kEDTDiskScreenshotMin, kEDTDiskScreenshotMax, Width, Err))
	{
		return FMCPToolHelpers::MakeError(Request, kEDTErrorInvalidParams, Err);
	}
	if (!EDT_ReadClampedInt(Request.Args, TEXT("height"), kEDTDiskScreenshotDefaultH,
		kEDTDiskScreenshotMin, kEDTDiskScreenshotMax, Height, Err))
	{
		return FMCPToolHelpers::MakeError(Request, kEDTErrorInvalidParams, Err);
	}

	// Parse format.
	FString FormatRaw;
	if (Request.Args.IsValid())
	{
		Request.Args->TryGetStringField(TEXT("format"), FormatRaw);
	}
	FMCPScreenshotUtils::EImageFormat Format = FMCPScreenshotUtils::EImageFormat::PNG;
	if (!FMCPScreenshotUtils::ParseFormat(FormatRaw, Format, Err))
	{
		return FMCPToolHelpers::MakeError(Request, kEDTErrorInvalidParams, Err);
	}

	// Parse quality (JPG only — silently accepted for PNG but ignored).
	int32 Quality = 85;
	if (!EDT_ReadClampedInt(Request.Args, TEXT("quality"), 85, 0, 100, Quality, Err))
	{
		return FMCPToolHelpers::MakeError(Request, kEDTErrorInvalidParams, Err);
	}

	// Resolve path — default if missing.
	FString PathRaw;
	if (Request.Args.IsValid())
	{
		Request.Args->TryGetStringField(TEXT("path"), PathRaw);
	}
	if (PathRaw.IsEmpty())
	{
		// FPaths::ProjectSavedDir() is UE-relative (../../../FatumGame/Saved/) — sandbox correctly
		// rejects `..` segments, so canonicalise to absolute BEFORE the Combine.
		const TCHAR* Ext = (Format == FMCPScreenshotUtils::EImageFormat::JPG) ? TEXT("jpg") : TEXT("png");
		const FGuid Guid = FGuid::NewGuid();
		const FString SavedAbs = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir());
		PathRaw = FPaths::Combine(SavedAbs, TEXT("UnrealMCP"),
			TEXT("screenshots"), FString::Printf(TEXT("%s.%s"), *Guid.ToString(EGuidFormats::Digits), Ext));
	}

	FString AbsPath;
	FString SandboxErr;
	if (!FMCPPathSandbox::Resolve(PathRaw, AbsPath, SandboxErr))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPathEscape, SandboxErr);
	}

	// Resolve active viewport.
	FViewport* Viewport = EDT_FindActiveLevelViewport();
	if (!Viewport)
	{
		return FMCPToolHelpers::MakeError(Request, kEDTErrorInternal,
			TEXT("no active level viewport (NO_VIEWPORT) — open a level editor tab"));
	}

	// Capture.
	TArray<uint8> Pixels;
	int32 OutW = 0, OutH = 0;
	FString CaptureErr;
	if (!FMCPScreenshotUtils::CaptureViewport(Viewport, Width, Height, Pixels, OutW, OutH, CaptureErr))
	{
		return FMCPToolHelpers::MakeError(Request, kEDTErrorInternal,
			FString::Printf(TEXT("viewport capture failed: %s"), *CaptureErr));
	}

	// Encode + save.
	int64 BytesWritten = 0;
	FString SaveErr;
	if (!FMCPScreenshotUtils::EncodeAndSaveToDisk(Pixels, OutW, OutH, Format, Quality,
		AbsPath, BytesWritten, SaveErr))
	{
		return FMCPToolHelpers::MakeError(Request, kEDTErrorInternal,
			FString::Printf(TEXT("encode-and-save failed: %s"), *SaveErr));
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("path"), AbsPath);
	Out->SetNumberField(TEXT("bytes"), static_cast<double>(BytesWritten));
	Out->SetNumberField(TEXT("width"), OutW);
	Out->SetNumberField(TEXT("height"), OutH);
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── pie.screenshot_to_disk ────────────────────────────────────────────────────────────────────
//
// Args:
//   path   : string (default = "<Saved>/UnrealMCP/screenshots/pie_<uuid>.png")
//   width  : int [32, 8192] (default 1920)
//   height : int [32, 8192] (default 1080)
//
// Response: { path: string, bytes: int, world: string }
//
// Errors:
//   -32602 InvalidParams   width/height out of range
//   -32038 PIENotActive    PIE not running (PIE_NOT_RUNNING per plan)
//   -32013 PathEscape      path outside sandbox
//   -32603 Internal        no PIE game viewport / capture / encode / write failed
//
// Note: this is in the pie.* namespace because the tool is meaningless without PIE — keeping it
// with the rest of the pie.* tools (Chunk A) gives a coherent wire surface.
FMCPResponse Tool_PIEScreenshotToDisk(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!FMCPWorldContext::IsPIEActive())
	{
		return EDT_MakePIENotActiveError(Request);
	}

	int32 Width = kEDTDiskScreenshotDefaultW;
	int32 Height = kEDTDiskScreenshotDefaultH;
	FString Err;
	if (!EDT_ReadClampedInt(Request.Args, TEXT("width"), kEDTDiskScreenshotDefaultW,
		kEDTDiskScreenshotMin, kEDTDiskScreenshotMax, Width, Err))
	{
		return FMCPToolHelpers::MakeError(Request, kEDTErrorInvalidParams, Err);
	}
	if (!EDT_ReadClampedInt(Request.Args, TEXT("height"), kEDTDiskScreenshotDefaultH,
		kEDTDiskScreenshotMin, kEDTDiskScreenshotMax, Height, Err))
	{
		return FMCPToolHelpers::MakeError(Request, kEDTErrorInvalidParams, Err);
	}

	// Default path: include "pie_" prefix to differentiate from editor screenshots.
	FString PathRaw;
	if (Request.Args.IsValid())
	{
		Request.Args->TryGetStringField(TEXT("path"), PathRaw);
	}
	if (PathRaw.IsEmpty())
	{
		// Canonicalise UE-relative ProjectSavedDir to absolute (sandbox blocks `..` segments).
		const FGuid Guid = FGuid::NewGuid();
		const FString SavedAbs = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir());
		PathRaw = FPaths::Combine(SavedAbs, TEXT("UnrealMCP"),
			TEXT("screenshots"), FString::Printf(TEXT("pie_%s.png"), *Guid.ToString(EGuidFormats::Digits)));
	}

	FString AbsPath;
	FString SandboxErr;
	if (!FMCPPathSandbox::Resolve(PathRaw, AbsPath, SandboxErr))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPathEscape, SandboxErr);
	}

	// Resolve PIE viewport + capture the world path BEFORE the capture (so we report exactly which
	// world this came from, in the rare case multiple PIE worlds exist concurrently).
	FViewport* Viewport = EDT_FindPIEGameViewport();
	if (!Viewport)
	{
		return FMCPToolHelpers::MakeError(Request, kEDTErrorInternal,
			TEXT("no PIE game viewport available (PIE may still be initialising)"));
	}
	FString WorldPath;
	for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
	{
		if (Ctx.WorldType == EWorldType::PIE && !Ctx.RunAsDedicated && Ctx.World())
		{
			UGameViewportClient* GVC = Ctx.World()->GetGameViewport();
			if (GVC && GVC->Viewport == Viewport)
			{
				WorldPath = Ctx.World()->GetOutermost() ? Ctx.World()->GetOutermost()->GetName() : FString();
				break;
			}
		}
	}

	TArray<uint8> Pixels;
	int32 OutW = 0, OutH = 0;
	FString CaptureErr;
	if (!FMCPScreenshotUtils::CaptureViewport(Viewport, Width, Height, Pixels, OutW, OutH, CaptureErr))
	{
		return FMCPToolHelpers::MakeError(Request, kEDTErrorInternal,
			FString::Printf(TEXT("PIE viewport capture failed: %s"), *CaptureErr));
	}

	// PIE always writes PNG (lossless; PIE captures are typically used for diagnostics, not archival).
	int64 BytesWritten = 0;
	FString SaveErr;
	if (!FMCPScreenshotUtils::EncodeAndSaveToDisk(Pixels, OutW, OutH,
		FMCPScreenshotUtils::EImageFormat::PNG, /*JpegQuality*/ 0,
		AbsPath, BytesWritten, SaveErr))
	{
		return FMCPToolHelpers::MakeError(Request, kEDTErrorInternal,
			FString::Printf(TEXT("encode-and-save failed: %s"), *SaveErr));
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("path"), AbsPath);
	Out->SetNumberField(TEXT("bytes"), static_cast<double>(BytesWritten));
	Out->SetStringField(TEXT("world"), WorldPath);
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── editor.get_camera ─────────────────────────────────────────────────────────────────────────
//
// Args: {}
// Response: {
//   location:    [x, y, z],
//   rotation:    [pitch, yaw, roll],
//   fov:         number,
//   ortho_width: number | null  (only non-null when viewport is orthographic)
// }
//
// Errors:
//   -32603 Internal  no level viewport found
FMCPResponse Tool_GetCamera(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FLevelEditorViewportClient* Client = EDT_FindActiveLevelViewportClient();
	if (!Client)
	{
		return FMCPToolHelpers::MakeError(Request, kEDTErrorInternal,
			TEXT("no active level viewport (NO_VIEWPORT) — open a level editor tab"));
	}

	const FVector Location = Client->GetViewLocation();
	const FRotator Rotation = Client->GetViewRotation();
	const float FOV = Client->ViewFOV;
	const bool bOrtho = Client->IsOrtho();

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetArrayField(TEXT("location"), EDT_VectorToArray(Location));
	Out->SetArrayField(TEXT("rotation"), EDT_RotatorToArray(Rotation));
	Out->SetNumberField(TEXT("fov"), FOV);
	if (bOrtho)
	{
		// OrthoZoom isn't strictly "ortho_width" in cm/units — it's the zoom factor. We expose it
		// under that field name to match the plan's wire schema; downstream callers treat it as
		// an opaque numeric handle for set_camera round-tripping (we don't yet expose set_camera
		// ortho zoom write, but the read surface is forward-compatible).
		Out->SetNumberField(TEXT("ortho_width"), Client->GetOrthoZoom());
	}
	else
	{
		Out->SetField(TEXT("ortho_width"), MakeShared<FJsonValueNull>());
	}
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── editor.set_camera ─────────────────────────────────────────────────────────────────────────
//
// Args:
//   location : [x, y, z]                  required
//   rotation : [pitch, yaw, roll]         required
//   fov      : number in (1.0, 175.0)     optional
//
// Response: { set: true }
//
// Errors:
//   -32602 InvalidParams  missing location/rotation OR fov out of range
//   -32603 Internal       no level viewport
//
// FOV is applied via FEditorViewportClient::ViewFOV (direct field write). When omitted, the
// existing FOV is preserved. Location/rotation set via SetViewLocation/SetViewRotation which
// also invalidate the viewport so the change is visible without a manual redraw.
FMCPResponse Tool_SetCamera(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kEDTErrorInvalidParams, TEXT("missing args object"));
	}

	FVector Location = FVector::ZeroVector;
	FString Err;
	if (!EDT_ReadVectorArray(Request.Args, TEXT("location"), Location, Err))
	{
		return FMCPToolHelpers::MakeError(Request, kEDTErrorInvalidParams, Err);
	}
	FVector RotationVec = FVector::ZeroVector;
	if (!EDT_ReadVectorArray(Request.Args, TEXT("rotation"), RotationVec, Err))
	{
		return FMCPToolHelpers::MakeError(Request, kEDTErrorInvalidParams, Err);
	}
	const FRotator Rotation(RotationVec.X, RotationVec.Y, RotationVec.Z); // pitch, yaw, roll

	double FOV = -1.0;
	const bool bHasFov = Request.Args->TryGetNumberField(TEXT("fov"), FOV);
	if (bHasFov && (FOV < 1.0 || FOV > 175.0))
	{
		return FMCPToolHelpers::MakeError(Request, kEDTErrorInvalidParams,
			FString::Printf(TEXT("fov %g out of range (1.0, 175.0)"), FOV));
	}

	FLevelEditorViewportClient* Client = EDT_FindActiveLevelViewportClient();
	if (!Client)
	{
		return FMCPToolHelpers::MakeError(Request, kEDTErrorInternal,
			TEXT("no active level viewport (NO_VIEWPORT) — open a level editor tab"));
	}

	Client->SetViewLocation(Location);
	Client->SetViewRotation(Rotation);
	if (bHasFov)
	{
		Client->ViewFOV = static_cast<float>(FOV);
	}
	// Invalidate so the editor presents the new view this frame; otherwise the change waits for
	// the next natural redraw trigger.
	Client->Invalidate();

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("set"), true);
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── editor.get_selection ──────────────────────────────────────────────────────────────────────
//
// Args: {}
// Response: {
//   actors:     [ActorSummary, ...],
//   components: [{ name: string, owner_actor_guid: string }, ...]
// }
//
// Errors: none expected (empty selection returns empty arrays)
//
// USelection enumeration via GEditor->GetSelectedActors / GetSelectedComponents. Component
// summary is minimal (name + owning actor's guid) since callers usually want components for
// follow-up component.* calls which take the owner-path form.
FMCPResponse Tool_GetSelection(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!GEditor)
	{
		return FMCPToolHelpers::MakeError(Request, kEDTErrorInternal, TEXT("GEditor unavailable (commandlet?)"));
	}

	// Enumerate actors.
	TArray<TSharedPtr<FJsonValue>> ActorsArr;
	if (USelection* ActorSel = GEditor->GetSelectedActors())
	{
		const int32 Count = ActorSel->Num();
		ActorsArr.Reserve(Count);
		for (int32 Idx = 0; Idx < Count; ++Idx)
		{
			UObject* Obj = ActorSel->GetSelectedObject(Idx);
			if (AActor* Actor = Cast<AActor>(Obj))
			{
				ActorsArr.Add(MakeShared<FJsonValueObject>(EDT_BuildActorSummary(Actor)));
			}
		}
	}

	// Enumerate components.
	TArray<TSharedPtr<FJsonValue>> ComponentsArr;
	if (USelection* CompSel = GEditor->GetSelectedComponents())
	{
		const int32 Count = CompSel->Num();
		ComponentsArr.Reserve(Count);
		for (int32 Idx = 0; Idx < Count; ++Idx)
		{
			UObject* Obj = CompSel->GetSelectedObject(Idx);
			if (UActorComponent* Comp = Cast<UActorComponent>(Obj))
			{
				TSharedPtr<FJsonObject> CompObj = MakeShared<FJsonObject>();
				CompObj->SetStringField(TEXT("name"), Comp->GetName());
				CompObj->SetStringField(TEXT("class"), Comp->GetClass()->GetPathName());
				if (AActor* Owner = Comp->GetOwner())
				{
					const FGuid Guid = Owner->GetActorGuid();
					CompObj->SetStringField(TEXT("owner_actor_guid"),
						Guid.IsValid() ? Guid.ToString(EGuidFormats::DigitsWithHyphens) : FString());
					CompObj->SetStringField(TEXT("owner_actor_path"),
						FMCPActorPathUtils::BuildActorPath(Owner));
				}
				else
				{
					CompObj->SetStringField(TEXT("owner_actor_guid"), FString());
					CompObj->SetStringField(TEXT("owner_actor_path"), FString());
				}
				ComponentsArr.Add(MakeShared<FJsonValueObject>(CompObj));
			}
		}
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetArrayField(TEXT("actors"), ActorsArr);
	Out->SetArrayField(TEXT("components"), ComponentsArr);
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── editor.set_selection ──────────────────────────────────────────────────────────────────────
//
// Args:
//   actor_ids : array<string> [0, 200]   required (empty array clears selection when append=false)
//   append    : bool (default false)     when false, replaces prior selection; when true, adds
//
// Response: { selected_count: int }       count of actors AFTER selection change
//
// Errors:
//   -32602 InvalidParams    missing actor_ids OR malformed entry
//   -32017 InputTooLarge    actor_ids.length > 200
//   -32603 Internal         UEditorActorSubsystem unavailable
//   -32004 ObjectNotFound   one or more actor_ids did not resolve — message lists the failed IDs
//
// Resolution uses ResolveActor(bRejectPIE=false) — set_selection is editor-world only by API
// contract (EditorActorSubsystem excludes PIE actors from its selection internally), but
// passing false here lets resolve return helpful "actor lives in PIE" messaging.
FMCPResponse Tool_SetSelection(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kEDTErrorInvalidParams, TEXT("missing args object"));
	}

	const TArray<TSharedPtr<FJsonValue>>* IdsArr = nullptr;
	if (!Request.Args->TryGetArrayField(TEXT("actor_ids"), IdsArr) || !IdsArr)
	{
		return FMCPToolHelpers::MakeError(Request, kEDTErrorInvalidParams,
			TEXT("missing required array field 'actor_ids'"));
	}
	if (IdsArr->Num() > kEDTMaxSelectionSize)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInputTooLarge,
			FString::Printf(TEXT("actor_ids.length=%d exceeds cap of %d"),
				IdsArr->Num(), kEDTMaxSelectionSize));
	}

	bool bAppend = false;
	Request.Args->TryGetBoolField(TEXT("append"), bAppend);

	UEditorActorSubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UEditorActorSubsystem>() : nullptr;
	if (!Subsystem)
	{
		return FMCPToolHelpers::MakeError(Request, kEDTErrorInternal,
			TEXT("UEditorActorSubsystem unavailable"));
	}

	// Resolve each id; accumulate unresolved IDs for the error response.
	TArray<AActor*> Resolved;
	Resolved.Reserve(IdsArr->Num());
	TArray<FString> Unresolved;
	for (const TSharedPtr<FJsonValue>& Val : *IdsArr)
	{
		FString IdStr;
		if (!Val.IsValid() || !Val->TryGetString(IdStr) || IdStr.IsEmpty())
		{
			return FMCPToolHelpers::MakeError(Request, kEDTErrorInvalidParams,
				TEXT("actor_ids entries must be non-empty strings"));
		}
		bool bAmbig = false;
		FString AmbigHint;
		FString ResolveErr;
		AActor* Actor = FMCPActorPathUtils::ResolveActor(IdStr,
			/*bRejectPIE*/ false, bAmbig, AmbigHint, ResolveErr);
		if (Actor)
		{
			Resolved.Add(Actor);
		}
		else
		{
			Unresolved.Add(IdStr);
		}
	}
	if (Unresolved.Num() > 0)
	{
		// Bound the listed-ids count in the message so a 200-entry failure doesn't blow the wire.
		const int32 ListCap = FMath::Min(8, Unresolved.Num());
		FString List;
		for (int32 i = 0; i < ListCap; ++i)
		{
			if (!List.IsEmpty()) { List += TEXT(", "); }
			List += FString::Printf(TEXT("'%s'"), *Unresolved[i]);
		}
		if (Unresolved.Num() > ListCap)
		{
			List += FString::Printf(TEXT(" (and %d more)"), Unresolved.Num() - ListCap);
		}
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("could not resolve %d of %d actor_ids: %s"),
				Unresolved.Num(), IdsArr->Num(), *List));
	}

	// If append, prepend the current selection.
	if (bAppend)
	{
		if (USelection* CurSel = GEditor->GetSelectedActors())
		{
			const int32 CurCount = CurSel->Num();
			for (int32 Idx = 0; Idx < CurCount; ++Idx)
			{
				if (AActor* Actor = Cast<AActor>(CurSel->GetSelectedObject(Idx)))
				{
					// Deduplicate — Resolved may already contain this actor (caller-passed).
					Resolved.AddUnique(Actor);
				}
			}
		}
	}

	Subsystem->SetSelectedLevelActors(Resolved);

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	// Report what the subsystem actually selected (it may have filtered out PIE/invalid actors
	// internally — we report the post-state count, not just Resolved.Num()).
	const int32 PostCount = GEditor->GetSelectedActors() ? GEditor->GetSelectedActors()->Num() : 0;
	Out->SetNumberField(TEXT("selected_count"), PostCount);
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── editor.show_message ───────────────────────────────────────────────────────────────────────
//
// Args:
//   text     : string (1..500 chars)              required
//   level    : "info" | "success" | "warning" | "error"  default "info"
//   duration : float [0.5, 30.0] seconds          default 5.0
//
// Response: { shown: true }
//
// Errors:
//   -32602 InvalidParams  text missing/empty/too long OR level unrecognised OR duration OOB
//
// Implementation: FSlateNotificationManager::Get().AddNotification(Info). Level maps to
// SNotificationItem::ECompletionState — note that "info" stays at CS_None which produces no
// icon (UE renders a plain toast). Use "success"/"error" for visible status icons.
FMCPResponse Tool_ShowMessage(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kEDTErrorInvalidParams, TEXT("missing args object"));
	}

	FString Text;
	if (!Request.Args->TryGetStringField(TEXT("text"), Text) || Text.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kEDTErrorInvalidParams,
			TEXT("missing required non-empty string field 'text'"));
	}
	if (Text.Len() > 500)
	{
		return FMCPToolHelpers::MakeError(Request, kEDTErrorInvalidParams,
			FString::Printf(TEXT("'text' length %d exceeds 500-char cap"), Text.Len()));
	}

	FString Level = TEXT("info");
	Request.Args->TryGetStringField(TEXT("level"), Level);
	SNotificationItem::ECompletionState State = SNotificationItem::CS_None;
	bool bUseIcons = true;
	if (Level.Equals(TEXT("info"), ESearchCase::IgnoreCase))
	{
		State = SNotificationItem::CS_None;
		bUseIcons = false; // info has no icon by convention
	}
	else if (Level.Equals(TEXT("success"), ESearchCase::IgnoreCase))
	{
		State = SNotificationItem::CS_Success;
	}
	else if (Level.Equals(TEXT("warning"), ESearchCase::IgnoreCase))
	{
		// SNotificationItem has no native "warning" state; reuse CS_Pending which renders an
		// in-progress/cautionary icon (the throbber-style) — closest equivalent.
		State = SNotificationItem::CS_Pending;
	}
	else if (Level.Equals(TEXT("error"), ESearchCase::IgnoreCase))
	{
		State = SNotificationItem::CS_Fail;
	}
	else
	{
		return FMCPToolHelpers::MakeError(Request, kEDTErrorInvalidParams,
			FString::Printf(TEXT("level '%s' invalid — accepted: info | success | warning | error"),
				*Level));
	}

	double Duration = 5.0;
	if (Request.Args->TryGetNumberField(TEXT("duration"), Duration))
	{
		if (Duration < 0.5 || Duration > 30.0)
		{
			return FMCPToolHelpers::MakeError(Request, kEDTErrorInvalidParams,
				FString::Printf(TEXT("duration %g out of range [0.5, 30.0]"), Duration));
		}
	}

	FNotificationInfo Info(FText::FromString(Text));
	Info.ExpireDuration = static_cast<float>(Duration);
	Info.bUseSuccessFailIcons = bUseIcons;
	Info.bUseThrobber = (State == SNotificationItem::CS_Pending); // warnings spin until expiry
	Info.bFireAndForget = true;
	TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info);
	if (Item.IsValid() && State != SNotificationItem::CS_None)
	{
		Item->SetCompletionState(State);
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("shown"), true);
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── editor.current_world ──────────────────────────────────────────────────────────────────────
//
// Args: {}
// Response: {
//   world_path: string,   // e.g. "/Game/Maps/MyLevel"
//   world_name: string,   // e.g. "MyLevel"
//   pie_active: bool      // mirrors pie.is_running.running
// }
//
// Errors:
//   -32603 Internal  no editor world (GEditor missing OR rare level-switch race)
//
// Always reports the EDITOR world (NOT the PIE world). pie_active is a separate field so callers
// can decide whether to follow up with pie.* tools or editor.* tools.
FMCPResponse Tool_CurrentWorld(const FMCPRequest& Request)
{
	check(IsInGameThread());

	UWorld* World = FMCPWorldContext::GetEditorWorld();
	if (!World)
	{
		return FMCPToolHelpers::MakeError(Request, kEDTErrorInternal,
			TEXT("no editor world available (NO_WORLD) — GEditor missing or level switch in progress"));
	}

	const FString PackagePath = World->GetOutermost() ? World->GetOutermost()->GetName() : FString();
	const FString WorldName = World->GetName();

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("world_path"), PackagePath);
	Out->SetStringField(TEXT("world_name"), WorldName);
	Out->SetBoolField(TEXT("pie_active"), FMCPWorldContext::IsPIEActive());
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── editor.tick_once ──────────────────────────────────────────────────────────────────────────
//
// Args: {}
// Response: { ticked: true }
//
// Errors:
//   -32603 Internal  GEditor unavailable
//
// Advances the global FTSTicker (timers, async-task progress) by ONE 60 Hz frame. Does NOT call
// GEditor->Tick() — that's reserved for the engine's own per-frame loop and asserts on
// EDynamicResolutionStateEvent::BeginFrame being unique-per-frame. Calling GEditor->Tick from a
// Lane A dispatch handler runs MID-frame (inside the engine's natural tick), violating that
// invariant and immediately crashing with appError ("UnrealEngine.cpp:13962").
//
// This implementation uses the safer FTSTicker path: gameplay timers, async load completions,
// per-frame callbacks, and the bridge's own OnEndFrame drain all advance. The engine's own
// per-frame work (rendering, animation, physics) does NOT — that still requires a natural tick.
// Primary use case: headless / commandlet scenarios where the editor is otherwise idle and
// queued FTSTicker work needs a nudge to progress.
FMCPResponse Tool_TickOnce(const FMCPRequest& Request)
{
	check(IsInGameThread());

	// One 60Hz frame. Caller has no way to specify dt — keeping the input schema strict (per plan).
	constexpr float TickDt = 1.0f / 60.0f;
	FTSTicker::GetCoreTicker().Tick(TickDt);

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("ticked"), true);
	Out->SetNumberField(TEXT("delta_seconds"), TickDt);
	Out->SetStringField(TEXT("note"),
		TEXT("FTSTicker advanced; GEditor->Tick is unsafe mid-frame and is NOT called here. "
			 "Engine render/anim/physics ticks still require a natural editor frame."));
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

	// Screenshot family.
	RegisterTool(TEXT("editor.viewport_screenshot"),         &Tool_ViewportScreenshot,        /*Lane A*/ false);
	RegisterTool(TEXT("editor.viewport_screenshot_to_disk"), &Tool_ViewportScreenshotToDisk,  /*Lane A*/ false);
	RegisterTool(TEXT("pie.screenshot_to_disk"),             &Tool_PIEScreenshotToDisk,       /*Lane A*/ false);

	// Camera.
	RegisterTool(TEXT("editor.get_camera"),                  &Tool_GetCamera,                 /*Lane A*/ false);
	RegisterTool(TEXT("editor.set_camera"),                  &Tool_SetCamera,                 /*Lane A*/ false);

	// Selection.
	RegisterTool(TEXT("editor.get_selection"),               &Tool_GetSelection,              /*Lane A*/ false);
	RegisterTool(TEXT("editor.set_selection"),               &Tool_SetSelection,              /*Lane A*/ false);

	// Misc utility.
	RegisterTool(TEXT("editor.show_message"),                &Tool_ShowMessage,               /*Lane A*/ false);
	RegisterTool(TEXT("editor.current_world"),               &Tool_CurrentWorld,              /*Lane A*/ false);
	RegisterTool(TEXT("editor.tick_once"),                   &Tool_TickOnce,                  /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("Phase 5 Chunk B: registered 10 editor utility handlers (screenshot ×3 + camera ×2 + selection ×2 + misc ×3, all Lane A)"));
}

} // namespace FEditorTools

#undef LOCTEXT_NAMESPACE
