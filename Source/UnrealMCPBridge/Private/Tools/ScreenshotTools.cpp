// Copyright FatumGame. All Rights Reserved.

#include "ScreenshotTools.h"

#include "FMCPDispatchQueue.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPActorPathUtils.h"
#include "Utils/MCPPathSandbox.h"
#include "Utils/MCPScreenshotUtils.h"

#include "Editor.h"
#include "EditorViewportClient.h"
#include "Engine/Engine.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "LevelEditor.h"
#include "LevelEditorViewport.h"
#include "Math/Box.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "SLevelViewport.h"
#include "UnrealClient.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	// SHOT_ prefix per the unity-build symbol-collision convention.
	constexpr int32 kSHOTErrorInvalidParams = -32602;
	constexpr int32 kSHOTErrorInternal      = -32603;

	// high_resolution caps. Multiplier ≥ 1.0 (anything below is a downscale — not a "hi-res"
	// operation; if the caller wants downscale they should use editor.viewport_screenshot_to_disk
	// directly). Upper bound 8.0× matches the hard ceiling we already enforce in
	// FMCPScreenshotUtils (post-multiplier dim ≤ 16384 px would exceed the kMaxResizeDim guard).
	constexpr float kSHOTHighResMultiplierMin = 1.0f;
	constexpr float kSHOTHighResMultiplierMax = 8.0f;
	constexpr float kSHOTHighResMultiplierDefault = 4.0f;

	// region_capture defaults — 1080p (Phase 5 plan default for disk screenshots).
	constexpr int32 kSHOTRegionDefaultW = 1920;
	constexpr int32 kSHOTRegionDefaultH = 1080;
	constexpr int32 kSHOTRegionMin      = 32;
	constexpr int32 kSHOTRegionMax      = 8192;
	constexpr float kSHOTRegionPaddingDefault = 100.0f;

	// diff defaults. Threshold is expressed as a fraction in [0, 1] — 0.05 = 5% differing
	// pixels = "identical". Per-channel noise tolerance defaults to 5 (JPEG artefact slack).
	constexpr float kSHOTDiffThresholdDefault = 0.05f;
	constexpr int32 kSHOTDiffChannelTolerance = 5;

	void SHOT_StampIds(const FMCPRequest& Request, FMCPResponse& Response)
	{
		Response.RequestId = Request.RequestId;
		Response.OriginalIdString = Request.OriginalIdString;
	}

	FMCPResponse SHOT_MakeError(const FMCPRequest& Request, int32 Code, const FString& Message)
	{
		FMCPResponse R;
		SHOT_StampIds(Request, R);
		R.bIsError = true;
		R.ErrorCode = Code;
		R.ErrorMessage = Message;
		return R;
	}

	FMCPResponse SHOT_MakeSuccessObj(const FMCPRequest& Request, TSharedPtr<FJsonObject> Result)
	{
		FMCPResponse R;
		SHOT_StampIds(Request, R);
		R.bIsError = false;
		R.Result = MakeShared<FJsonValueObject>(MoveTemp(Result));
		return R;
	}

	// ─── Viewport resolution helpers (mirror of EditorTools/ViewportTools) ─────────────────────

	/**
	 * Pick the best "default" level viewport client when ``viewport_index`` is NOT supplied.
	 * Cascade matches EditorTools' EDT_FindActiveLevelViewportClient:
	 *   1. GCurrentLevelEditingViewportClient  — hovered viewport.
	 *   2. GLastKeyLevelEditingViewportClient  — last keyboard-focused viewport.
	 *   3. FLevelEditorModule::GetFirstActiveLevelViewport — module-level resolved active tab.
	 *   4. First non-null, non-orthographic, RHI-realised client from
	 *      GEditor->GetLevelViewportClients() — defensive fallback for the case where the editor
	 *      just booted and none of the globals are populated yet.
	 *   5. Final fallback: first non-null client at any index (even unrealised — caller will
	 *      get a useful "viewport not realised" error then).
	 * Returns null only when no LevelEditor exists (commandlet).
	 */
	FLevelEditorViewportClient* SHOT_FindBestDefaultViewport()
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
		if (FLevelEditorModule* LevelEditor =
				FModuleManager::GetModulePtr<FLevelEditorModule>("LevelEditor"))
		{
			TSharedPtr<SLevelViewport> ActiveViewport = LevelEditor->GetFirstActiveLevelViewport();
			if (ActiveViewport.IsValid())
			{
				return &ActiveViewport->GetLevelViewportClient();
			}
		}
		if (GEditor)
		{
			const TArray<FLevelEditorViewportClient*>& Clients = GEditor->GetLevelViewportClients();
			// Prefer a perspective viewport with a realised RHI surface — orthographic editor
			// tabs frequently sit unrealised until the user explicitly clicks them.
			for (FLevelEditorViewportClient* VC : Clients)
			{
				if (VC && VC->IsPerspective() && VC->Viewport
					&& VC->Viewport->GetSizeXY().X > 0 && VC->Viewport->GetSizeXY().Y > 0)
				{
					return VC;
				}
			}
			// Last-resort: any non-null client (may still be unrealised; caller's burden).
			for (FLevelEditorViewportClient* VC : Clients)
			{
				if (VC) { return VC; }
			}
		}
		return nullptr;
	}

	/**
	 * Resolve a level viewport client. When ``viewport_index`` is supplied, indexes
	 * GEditor->GetLevelViewportClients() directly. When absent, uses SHOT_FindBestDefaultViewport's
	 * cascade so we transparently hit the user's currently active perspective viewport instead
	 * of an unrealised ortho tab at index 0.
	 *
	 * Returns nullptr + populates OutErr on failure.
	 */
	FLevelEditorViewportClient* SHOT_ResolveViewport(const FMCPRequest& Request, FMCPResponse& OutErr)
	{
		check(IsInGameThread());

		if (!GEditor)
		{
			OutErr = SHOT_MakeError(Request, kSHOTErrorInternal,
				TEXT("GEditor unavailable (commandlet?)"));
			return nullptr;
		}

		// Explicit viewport_index path.
		double IndexRaw = -1.0;
		const bool bHasIndex = Request.Args.IsValid()
			&& Request.Args->TryGetNumberField(TEXT("viewport_index"), IndexRaw);
		if (bHasIndex)
		{
			const int32 Index = static_cast<int32>(IndexRaw);
			const TArray<FLevelEditorViewportClient*>& Clients = GEditor->GetLevelViewportClients();
			if (Clients.Num() == 0)
			{
				OutErr = SHOT_MakeError(Request, kSHOTErrorInternal,
					TEXT("no level viewports available (open a level editor tab first)"));
				return nullptr;
			}
			if (Index < 0 || Index >= Clients.Num())
			{
				OutErr = SHOT_MakeError(Request, kSHOTErrorInvalidParams,
					FString::Printf(
						TEXT("viewport_index %d out of range [0, %d)"), Index, Clients.Num()));
				return nullptr;
			}
			FLevelEditorViewportClient* VC = Clients[Index];
			if (!VC || !VC->Viewport)
			{
				OutErr = SHOT_MakeError(Request, kSHOTErrorInternal,
					FString::Printf(TEXT("level viewport client at index %d is null/uninit"), Index));
				return nullptr;
			}
			return VC;
		}

		// Default cascade (no explicit index).
		FLevelEditorViewportClient* VC = SHOT_FindBestDefaultViewport();
		if (!VC || !VC->Viewport)
		{
			OutErr = SHOT_MakeError(Request, kSHOTErrorInternal,
				TEXT("no active level viewport (NO_VIEWPORT) — open a level editor tab first"));
			return nullptr;
		}
		return VC;
	}

	/**
	 * Resolve an output_path against the sandbox. Empty input → defaults to a Saved/UnrealMCP/
	 * screenshots/ generated path (matches Phase 5 family default).
	 */
	bool SHOT_ResolveOutputPath(
		const TSharedPtr<FJsonObject>& Args,
		const TCHAR* DefaultExtension,
		const TCHAR* DefaultPrefix,
		FString& OutAbsPath,
		int32& OutErrorCode,
		FString& OutErrorMessage)
	{
		FString PathRaw;
		if (Args.IsValid())
		{
			Args->TryGetStringField(TEXT("output_path"), PathRaw);
		}
		if (PathRaw.IsEmpty())
		{
			const FGuid Guid = FGuid::NewGuid();
			const FString SavedAbs = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir());
			PathRaw = FPaths::Combine(SavedAbs, TEXT("UnrealMCP"), TEXT("screenshots"),
				FString::Printf(TEXT("%s_%s.%s"),
					DefaultPrefix, *Guid.ToString(EGuidFormats::Digits), DefaultExtension));
		}

		FString SandboxErr;
		if (!FMCPPathSandbox::Resolve(PathRaw, OutAbsPath, SandboxErr))
		{
			OutErrorCode = kMCPErrorPathEscape;
			OutErrorMessage = SandboxErr;
			return false;
		}
		return true;
	}

	/**
	 * Pick the image format from the output_path's file extension. Defaults to PNG when the
	 * extension isn't recognised (lossless is the safe default for screenshots; JPG callers
	 * must explicitly use .jpg/.jpeg).
	 */
	FMCPScreenshotUtils::EImageFormat SHOT_FormatFromExtension(const FString& AbsPath)
	{
		const FString Ext = FPaths::GetExtension(AbsPath).ToLower();
		if (Ext == TEXT("jpg") || Ext == TEXT("jpeg"))
		{
			return FMCPScreenshotUtils::EImageFormat::JPG;
		}
		return FMCPScreenshotUtils::EImageFormat::PNG;
	}

	// ─── diff helpers ──────────────────────────────────────────────────────────────────────────

	/**
	 * Load + decode an image file at AbsPath into a normalised BGRA8/sRGB FImage. Returns
	 * false on file-read failure / decode failure / format-conversion failure, with a
	 * descriptive OutError. Lane B safe — pure filesystem + ImageWrapper module access; no
	 * UObject reads, no GT requirement.
	 */
	bool SHOT_LoadImageBGRA8(const FString& AbsPath, FImage& OutImage, FString& OutError)
	{
		TArray<uint8> CompressedBytes;
		if (!FFileHelper::LoadFileToArray(CompressedBytes, *AbsPath))
		{
			OutError = FString::Printf(TEXT("could not read file '%s'"), *AbsPath);
			return false;
		}
		if (CompressedBytes.Num() == 0)
		{
			OutError = FString::Printf(TEXT("file '%s' is empty"), *AbsPath);
			return false;
		}

		// LoadModuleChecked rather than Get — the ImageWrapper module is a Phase 2 dep already
		// loaded, but we may be running on a worker thread (Lane B). LoadModuleChecked is
		// thread-safe when the module is already loaded; first-load goes through the GT-only
		// module manager, which is why this tool requires ImageWrapper to have been touched
		// previously (it always has — every screenshot tool uses it).
		IImageWrapperModule& IWM =
			FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));

		if (!IWM.DecompressImage(CompressedBytes.GetData(), CompressedBytes.Num(), OutImage))
		{
			OutError = FString::Printf(
				TEXT("ImageWrapper failed to decode '%s' (not a recognised image format?)"),
				*AbsPath);
			return false;
		}

		// Normalise to BGRA8/sRGB so the pixel walk below is uniform regardless of source format
		// (PNG can be RGBA8 or RGB8 or grayscale; JPG is always RGB8). ChangeFormat is a no-op
		// when source is already BGRA8/sRGB.
		OutImage.ChangeFormat(ERawImageFormat::BGRA8, EGammaSpace::sRGB);
		return true;
	}

	/**
	 * Walk pixel arrays in lockstep, count differing pixels (per-channel abs-diff > tolerance
	 * on any of R/G/B). Returns count of differing pixels; writes the differing-pixel map into
	 * OutDifferingMask (one bool per pixel, parallel to PixelsA/PixelsB) when OutDifferingMask
	 * is non-null. Alpha intentionally NOT compared — screenshot alpha is forced opaque by the
	 * capture path.
	 */
	uint64 SHOT_CountDifferingPixels(
		TArrayView64<const FColor> PixelsA,
		TArrayView64<const FColor> PixelsB,
		int32 ChannelTolerance,
		TArray<uint8>* OutDifferingMask)
	{
		check(PixelsA.Num() == PixelsB.Num());

		uint64 DiffCount = 0;
		if (OutDifferingMask)
		{
			OutDifferingMask->Reset();
			OutDifferingMask->SetNumZeroed(PixelsA.Num());
		}

		for (int64 i = 0; i < PixelsA.Num(); ++i)
		{
			const FColor& A = PixelsA[i];
			const FColor& B = PixelsB[i];
			const int32 DR = FMath::Abs(static_cast<int32>(A.R) - static_cast<int32>(B.R));
			const int32 DG = FMath::Abs(static_cast<int32>(A.G) - static_cast<int32>(B.G));
			const int32 DB = FMath::Abs(static_cast<int32>(A.B) - static_cast<int32>(B.B));
			if (DR > ChannelTolerance || DG > ChannelTolerance || DB > ChannelTolerance)
			{
				++DiffCount;
				if (OutDifferingMask)
				{
					(*OutDifferingMask)[i] = 1;
				}
			}
		}
		return DiffCount;
	}

	/**
	 * Build a diff overlay image: image A's pixels desaturated to greyscale at half intensity,
	 * with semi-transparent red painted on top of every pixel where DifferingMask[i] == 1.
	 * Returns a fresh FImage in BGRA8/sRGB.
	 */
	void SHOT_BuildDiffOverlay(
		TArrayView64<const FColor> BaseA,
		const TArray<uint8>& DifferingMask,
		int32 Width,
		int32 Height,
		FImage& OutImage)
	{
		check(BaseA.Num() == static_cast<int64>(Width) * static_cast<int64>(Height));
		check(DifferingMask.Num() == BaseA.Num());

		OutImage.Init(Width, Height, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
		TArrayView64<FColor> OutPixels = OutImage.AsBGRA8();

		for (int64 i = 0; i < BaseA.Num(); ++i)
		{
			const FColor& Src = BaseA[i];
			// Standard luminance weights; halved so the overlay reads "shadowy" and the red is
			// vivid against it.
			const uint8 Lum = static_cast<uint8>(FMath::Clamp(
				(0.299f * Src.R + 0.587f * Src.G + 0.114f * Src.B) * 0.5f, 0.0f, 255.0f));
			if (DifferingMask[i] != 0)
			{
				// Semi-transparent red overlay (alpha ~0.7) blended against the grey base.
				const uint8 R = static_cast<uint8>(FMath::Min(255, static_cast<int32>(Lum) + 180));
				OutPixels[i] = FColor(0, 0, R, 255); // BGRA layout: B=0, G=0, R=brightened
			}
			else
			{
				OutPixels[i] = FColor(Lum, Lum, Lum, 255); // BGRA: B=G=R=lum
			}
		}
	}

	/**
	 * Encode an FImage (BGRA8) to PNG bytes and save to AbsPath. Lane B safe.
	 */
	bool SHOT_SaveImageAsPNG(const FImage& InImage, const FString& AbsPath, FString& OutError)
	{
		TArray64<uint8> Encoded;
		const FImageView View(
			const_cast<FColor*>(InImage.AsBGRA8().GetData()),
			InImage.SizeX, InImage.SizeY, EGammaSpace::sRGB);
		if (!FImageUtils::CompressImage(Encoded, TEXT("png"), View, /*Quality*/ 0))
		{
			OutError = TEXT("FImageUtils::CompressImage returned false (PNG encode)");
			return false;
		}
		if (Encoded.Num() > INT32_MAX)
		{
			OutError = FString::Printf(
				TEXT("encoded PNG %lld bytes exceeds 2 GiB cap"), Encoded.Num());
			return false;
		}

		const FString ParentDir = FPaths::GetPath(AbsPath);
		if (!ParentDir.IsEmpty())
		{
			IFileManager::Get().MakeDirectory(*ParentDir, /*Tree*/ true);
		}

		TArray<uint8> WriteBuf;
		WriteBuf.Append(Encoded.GetData(), static_cast<int32>(Encoded.Num()));
		if (!FFileHelper::SaveArrayToFile(WriteBuf, *AbsPath))
		{
			OutError = FString::Printf(TEXT("could not write '%s' to disk"), *AbsPath);
			return false;
		}
		return true;
	}
} // namespace

namespace FScreenshotTools
{

// ─── screenshot.high_resolution ────────────────────────────────────────────────────────────────
//
// Args:
//   viewport_index?         : int (default 0)
//   resolution_multiplier?  : float [1.0, 8.0] (default 4.0)
//   output_path?            : string (default = "<Saved>/UnrealMCP/screenshots/hires_<uuid>.png")
//
// Response: { saved_path: string, width: int, height: int, bytes: int, multiplier: float }
//
// Errors:
//   -32013 PathEscape      output_path outside sandbox
//   -32602 InvalidParams   multiplier out of [1.0, 8.0], viewport_index OOB
//   -32603 Internal        no viewport / capture / encode / write failed
//
// Pipeline: native viewport size × multiplier → CaptureViewport (Draw + ReadPixels +
// FImageUtils::ImageResize) → EncodeAndSaveToDisk. PNG/JPG selected from output path extension.
//
// Caveat — multiplier-resize is NOT engine HighResShot tile-rendering. The underlying scene
// render is at native viewport resolution and then upsampled (FImageUtils does sRGB-correct
// resize). Image fidelity above 2× matches an in-engine FXAA-pass screenshot more than a true
// supersample. The engine's HighResShot path that does multi-tile rendering writes via async
// FImageWriteTask and CANNOT return synchronously to our request/response model — see header
// for FHighResScreenshotConfig caveat detail.
FMCPResponse Tool_HighResolution(const FMCPRequest& Request)
{
	check(IsInGameThread());

	// Parse multiplier with clamping.
	float Multiplier = kSHOTHighResMultiplierDefault;
	if (Request.Args.IsValid())
	{
		double Raw = 0.0;
		if (Request.Args->TryGetNumberField(TEXT("resolution_multiplier"), Raw))
		{
			Multiplier = static_cast<float>(Raw);
		}
	}
	if (Multiplier < kSHOTHighResMultiplierMin || Multiplier > kSHOTHighResMultiplierMax)
	{
		return SHOT_MakeError(Request, kSHOTErrorInvalidParams,
			FString::Printf(
				TEXT("resolution_multiplier %g out of [%g, %g]"),
				Multiplier, kSHOTHighResMultiplierMin, kSHOTHighResMultiplierMax));
	}

	// Resolve output path FIRST (fail-fast on sandbox issues before touching engine state).
	FString AbsPath;
	int32 PathErrCode = 0;
	FString PathErrMsg;
	if (!SHOT_ResolveOutputPath(Request.Args, TEXT("png"), TEXT("hires"),
		AbsPath, PathErrCode, PathErrMsg))
	{
		return SHOT_MakeError(Request, PathErrCode, PathErrMsg);
	}

	// Resolve viewport (validates viewport_index).
	FMCPResponse ResolveErr;
	FLevelEditorViewportClient* VC = SHOT_ResolveViewport(Request, ResolveErr);
	if (!VC) { return ResolveErr; }

	FViewport* Viewport = VC->Viewport;
	const FIntPoint NativeSize = Viewport->GetSizeXY();
	if (NativeSize.X <= 0 || NativeSize.Y <= 0)
	{
		return SHOT_MakeError(Request, kSHOTErrorInternal,
			FString::Printf(TEXT("viewport not realised (size=%dx%d)"), NativeSize.X, NativeSize.Y));
	}

	const int32 TargetW = FMath::Min(
		static_cast<int32>(NativeSize.X * Multiplier), 16384);
	const int32 TargetH = FMath::Min(
		static_cast<int32>(NativeSize.Y * Multiplier), 16384);

	const FMCPScreenshotUtils::EImageFormat Format = SHOT_FormatFromExtension(AbsPath);

	// Capture at the target (multiplied) resolution.
	TArray<uint8> Pixels;
	int32 OutW = 0, OutH = 0;
	FString CaptureErr;
	if (!FMCPScreenshotUtils::CaptureViewport(
			Viewport, TargetW, TargetH, Pixels, OutW, OutH, CaptureErr))
	{
		return SHOT_MakeError(Request, kSHOTErrorInternal,
			FString::Printf(TEXT("high-res capture failed: %s"), *CaptureErr));
	}

	// Encode + save.
	int64 BytesWritten = 0;
	FString SaveErr;
	if (!FMCPScreenshotUtils::EncodeAndSaveToDisk(
			Pixels, OutW, OutH, Format, /*JpegQuality*/ 90, AbsPath, BytesWritten, SaveErr))
	{
		return SHOT_MakeError(Request, kSHOTErrorInternal,
			FString::Printf(TEXT("encode-and-save failed: %s"), *SaveErr));
	}

	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("saved_path"), AbsPath);
	Out->SetNumberField(TEXT("width"), OutW);
	Out->SetNumberField(TEXT("height"), OutH);
	Out->SetNumberField(TEXT("bytes"), static_cast<double>(BytesWritten));
	Out->SetNumberField(TEXT("multiplier"), Multiplier);
	Out->SetNumberField(TEXT("native_width"), NativeSize.X);
	Out->SetNumberField(TEXT("native_height"), NativeSize.Y);
	return SHOT_MakeSuccessObj(Request, Out);
}

// ─── screenshot.region_capture ─────────────────────────────────────────────────────────────────
//
// Args:
//   actor_path     : string (required) — bare name OR full soft-object path (see MCPActorPathUtils)
//   viewport_index?: int (default 0)
//   padding?       : float (default 100.0) — extra cm added to actor's AABB extent on each side
//   output_path?   : string (default = "<Saved>/UnrealMCP/screenshots/region_<uuid>.png")
//   resolution?    : [w, h] (default [1920, 1080]) — output dimensions
//
// Response: {
//   saved_path: string, width: int, height: int, bytes: int,
//   actor_path: string,
//   actor_bounds: { origin: [x,y,z], extent: [x,y,z] },
//   captured_resolution: [w, h]
// }
//
// Errors:
//   -32004 ObjectNotFound  actor_path doesn't resolve OR has no bounds
//   -32013 PathEscape      output_path outside sandbox
//   -32602 InvalidParams   resolution malformed / out of range, padding negative
//   -32603 Internal        no viewport / capture / encode / write failed
//
// Pipeline: snapshot camera → expand actor AABB by (extent + padding) → FocusViewportOnBox
// (bInstant=true) → CaptureViewport at supplied resolution → EncodeAndSaveToDisk → restore
// camera. Camera restore is best-effort (a stray Slate tick between save and restore could
// write a different value — extremely unlikely on the GT dispatch path).
FMCPResponse Tool_RegionCapture(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!Request.Args.IsValid())
	{
		return SHOT_MakeError(Request, kSHOTErrorInvalidParams, TEXT("missing args object"));
	}

	FString ActorPath;
	if (!Request.Args->TryGetStringField(TEXT("actor_path"), ActorPath) || ActorPath.IsEmpty())
	{
		return SHOT_MakeError(Request, kSHOTErrorInvalidParams,
			TEXT("missing required string field 'actor_path'"));
	}

	double PaddingRaw = kSHOTRegionPaddingDefault;
	Request.Args->TryGetNumberField(TEXT("padding"), PaddingRaw);
	if (PaddingRaw < 0.0)
	{
		return SHOT_MakeError(Request, kSHOTErrorInvalidParams,
			FString::Printf(TEXT("padding %g must be ≥ 0"), PaddingRaw));
	}
	const float Padding = static_cast<float>(PaddingRaw);

	// Parse optional [w, h] resolution.
	int32 ResW = kSHOTRegionDefaultW;
	int32 ResH = kSHOTRegionDefaultH;
	const TArray<TSharedPtr<FJsonValue>>* ResArr = nullptr;
	if (Request.Args->TryGetArrayField(TEXT("resolution"), ResArr) && ResArr)
	{
		if (ResArr->Num() != 2)
		{
			return SHOT_MakeError(Request, kSHOTErrorInvalidParams,
				FString::Printf(
					TEXT("'resolution' must be [w, h] (2 numbers); got %d entries"),
					ResArr->Num()));
		}
		double W = 0.0, H = 0.0;
		if (!(*ResArr)[0]->TryGetNumber(W) || !(*ResArr)[1]->TryGetNumber(H))
		{
			return SHOT_MakeError(Request, kSHOTErrorInvalidParams,
				TEXT("'resolution' entries must both be numbers"));
		}
		ResW = static_cast<int32>(W);
		ResH = static_cast<int32>(H);
		if (ResW < kSHOTRegionMin || ResW > kSHOTRegionMax
			|| ResH < kSHOTRegionMin || ResH > kSHOTRegionMax)
		{
			return SHOT_MakeError(Request, kSHOTErrorInvalidParams,
				FString::Printf(
					TEXT("'resolution' [%d, %d] outside [%d, %d]"),
					ResW, ResH, kSHOTRegionMin, kSHOTRegionMax));
		}
	}

	// Resolve actor — region_capture is read-only on the world, so bRejectPIE=false (capture
	// works during PIE too).
	bool bAmbig = false;
	FString AmbigHint, ResolveErrMsg;
	AActor* Actor = FMCPActorPathUtils::ResolveActor(ActorPath, /*bRejectPIE*/ false,
		bAmbig, AmbigHint, ResolveErrMsg);
	if (!Actor)
	{
		const FString Msg = bAmbig
			? FString::Printf(TEXT("actor '%s' ambiguous: %s"), *ActorPath, *AmbigHint)
			: FString::Printf(TEXT("actor '%s' not found: %s"), *ActorPath, *ResolveErrMsg);
		return SHOT_MakeError(Request, kMCPErrorObjectNotFound, Msg);
	}

	// Compute AABB. include nonColliding so visual-only meshes count (matches viewport.focus_on_actor).
	FBox Bounds = Actor->GetComponentsBoundingBox(/*bNonColliding*/ true);
	if (!Bounds.IsValid)
	{
		return SHOT_MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(
				TEXT("actor '%s' has no valid bounding box (no registered components with bounds)"),
				*ActorPath));
	}

	// Pre-padding actor bounds for the response (caller may want to verify framing).
	const FVector PreOrigin = Bounds.GetCenter();
	const FVector PreExtent = Bounds.GetExtent();

	// Expand by padding (cm) on each side. FocusViewportOnBox frames the whole box in view.
	if (Padding > 0.0f)
	{
		Bounds = Bounds.ExpandBy(FVector(Padding, Padding, Padding));
	}

	// Resolve viewport.
	FMCPResponse VCErr;
	FLevelEditorViewportClient* VC = SHOT_ResolveViewport(Request, VCErr);
	if (!VC) { return VCErr; }

	FViewport* Viewport = VC->Viewport;
	if (!Viewport)
	{
		return SHOT_MakeError(Request, kSHOTErrorInternal,
			TEXT("viewport client has no realised FViewport"));
	}

	// Snapshot prior camera state for restore — keeps the user's editor view intact after the
	// programmatic capture.
	const FVector PriorLocation = VC->GetViewLocation();
	const FRotator PriorRotation = VC->GetViewRotation();
	const float PriorFOV = VC->ViewFOV;

	// Resolve output path BEFORE the camera change — fail-fast on sandbox issues so we don't
	// disturb the editor view for nothing.
	FString AbsPath;
	int32 PathErrCode = 0;
	FString PathErrMsg;
	if (!SHOT_ResolveOutputPath(Request.Args, TEXT("png"), TEXT("region"),
		AbsPath, PathErrCode, PathErrMsg))
	{
		return SHOT_MakeError(Request, PathErrCode, PathErrMsg);
	}

	// Focus + invalidate. bInstant=true skips the editor's smooth-camera-tween animation so
	// the next Draw() inside CaptureViewport sees the new framing immediately.
	VC->FocusViewportOnBox(Bounds, /*bInstant*/ true);
	VC->Invalidate();

	const FMCPScreenshotUtils::EImageFormat Format = SHOT_FormatFromExtension(AbsPath);

	// Capture at the requested resolution. CaptureViewport itself calls Viewport->Draw() so we
	// don't need a second Draw here — the focus change above is picked up by that internal Draw.
	TArray<uint8> Pixels;
	int32 OutW = 0, OutH = 0;
	FString CaptureErr;
	if (!FMCPScreenshotUtils::CaptureViewport(
			Viewport, ResW, ResH, Pixels, OutW, OutH, CaptureErr))
	{
		// Restore camera before returning error — the user's view shouldn't be left stuck on
		// the actor we tried to frame.
		VC->SetViewLocation(PriorLocation);
		VC->SetViewRotation(PriorRotation);
		VC->ViewFOV = PriorFOV;
		VC->Invalidate();
		return SHOT_MakeError(Request, kSHOTErrorInternal,
			FString::Printf(TEXT("region capture failed: %s"), *CaptureErr));
	}

	// Encode + save.
	int64 BytesWritten = 0;
	FString SaveErr;
	if (!FMCPScreenshotUtils::EncodeAndSaveToDisk(
			Pixels, OutW, OutH, Format, /*JpegQuality*/ 90, AbsPath, BytesWritten, SaveErr))
	{
		VC->SetViewLocation(PriorLocation);
		VC->SetViewRotation(PriorRotation);
		VC->ViewFOV = PriorFOV;
		VC->Invalidate();
		return SHOT_MakeError(Request, kSHOTErrorInternal,
			FString::Printf(TEXT("encode-and-save failed: %s"), *SaveErr));
	}

	// Restore camera — caller's editor view returns to the prior state.
	VC->SetViewLocation(PriorLocation);
	VC->SetViewRotation(PriorRotation);
	VC->ViewFOV = PriorFOV;
	VC->Invalidate();

	// Build bounds sub-object for response.
	TSharedRef<FJsonObject> BoundsObj = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> OriginArr;
	OriginArr.Add(MakeShared<FJsonValueNumber>(PreOrigin.X));
	OriginArr.Add(MakeShared<FJsonValueNumber>(PreOrigin.Y));
	OriginArr.Add(MakeShared<FJsonValueNumber>(PreOrigin.Z));
	BoundsObj->SetArrayField(TEXT("origin"), OriginArr);
	TArray<TSharedPtr<FJsonValue>> ExtentArr;
	ExtentArr.Add(MakeShared<FJsonValueNumber>(PreExtent.X));
	ExtentArr.Add(MakeShared<FJsonValueNumber>(PreExtent.Y));
	ExtentArr.Add(MakeShared<FJsonValueNumber>(PreExtent.Z));
	BoundsObj->SetArrayField(TEXT("extent"), ExtentArr);

	TArray<TSharedPtr<FJsonValue>> ResArrOut;
	ResArrOut.Add(MakeShared<FJsonValueNumber>(OutW));
	ResArrOut.Add(MakeShared<FJsonValueNumber>(OutH));

	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("saved_path"), AbsPath);
	Out->SetNumberField(TEXT("width"), OutW);
	Out->SetNumberField(TEXT("height"), OutH);
	Out->SetNumberField(TEXT("bytes"), static_cast<double>(BytesWritten));
	Out->SetStringField(TEXT("actor_path"), Actor->GetPathName());
	Out->SetObjectField(TEXT("actor_bounds"), BoundsObj);
	Out->SetArrayField(TEXT("captured_resolution"), ResArrOut);
	return SHOT_MakeSuccessObj(Request, Out);
}

// ─── screenshot.diff ───────────────────────────────────────────────────────────────────────────
//
// Args:
//   image_a_path     : string (required) — sandboxed absolute path to PNG/JPG/BMP/etc.
//   image_b_path     : string (required) — same.
//   threshold?       : float [0.0, 1.0] (default 0.05) — max fraction of differing pixels for
//                      'identical' classification.
//   diff_output_path?: string — when present, write a desaturated-greyscale overlay of image_a
//                      with red-painted differing pixels to this path. Sandbox-checked.
//
// Response: {
//   identical: bool,
//   difference_pct: float,            // 0.0 .. 100.0
//   differing_pixels: int,
//   total_pixels: int,
//   image_a: { width, height },
//   image_b: { width, height },
//   diff_image_path?: string          // only when diff_output_path supplied AND write succeeded
// }
//
// Errors:
//   -32004 ObjectNotFound  one or both files missing
//   -32013 PathEscape      one of the supplied paths outside sandbox
//   -32602 InvalidParams   missing image_a_path/image_b_path, threshold out of [0, 1],
//                          dimensions mismatch (identical=false, returned WITH 100% diff)
//   -32603 Internal        decode / overlay-write failed
//
// Lane B safe: pure filesystem + ImageWrapper compute, no UObject access, no GT requirement.
// Registered with bThreadSafe=true so the dispatch queue runs this on the worker pool — frees
// the game thread to keep ticking the editor while a heavy diff churns.
//
// Dimension mismatch policy: when the two images have different sizes we return identical=false
// + difference_pct=100 + differing_pixels=total_pixels=0. We do NOT raise -32602 for this — the
// diff is well-defined (everything differs) and the caller wants to see "they don't match" not
// an error.
FMCPResponse Tool_Diff(const FMCPRequest& Request)
{
	// NO check(IsInGameThread()) — this runs on a worker thread.

	if (!Request.Args.IsValid())
	{
		return SHOT_MakeError(Request, kSHOTErrorInvalidParams, TEXT("missing args object"));
	}

	FString PathARaw, PathBRaw;
	if (!Request.Args->TryGetStringField(TEXT("image_a_path"), PathARaw) || PathARaw.IsEmpty())
	{
		return SHOT_MakeError(Request, kSHOTErrorInvalidParams,
			TEXT("missing required string field 'image_a_path'"));
	}
	if (!Request.Args->TryGetStringField(TEXT("image_b_path"), PathBRaw) || PathBRaw.IsEmpty())
	{
		return SHOT_MakeError(Request, kSHOTErrorInvalidParams,
			TEXT("missing required string field 'image_b_path'"));
	}

	double ThresholdRaw = kSHOTDiffThresholdDefault;
	Request.Args->TryGetNumberField(TEXT("threshold"), ThresholdRaw);
	if (ThresholdRaw < 0.0 || ThresholdRaw > 1.0)
	{
		return SHOT_MakeError(Request, kSHOTErrorInvalidParams,
			FString::Printf(TEXT("threshold %g out of [0.0, 1.0]"), ThresholdRaw));
	}
	const float Threshold = static_cast<float>(ThresholdRaw);

	// Optional diff overlay path.
	FString DiffOutRaw;
	bool bWantOverlay = false;
	if (Request.Args->TryGetStringField(TEXT("diff_output_path"), DiffOutRaw)
		&& !DiffOutRaw.IsEmpty())
	{
		bWantOverlay = true;
	}

	// Sandbox-resolve all paths up-front. PATH_ESCAPE before any file open is the safer order.
	FString AbsA, AbsB, AbsDiffOut;
	FString SandboxErr;
	if (!FMCPPathSandbox::Resolve(PathARaw, AbsA, SandboxErr))
	{
		return SHOT_MakeError(Request, kMCPErrorPathEscape,
			FString::Printf(TEXT("image_a_path: %s"), *SandboxErr));
	}
	if (!FMCPPathSandbox::Resolve(PathBRaw, AbsB, SandboxErr))
	{
		return SHOT_MakeError(Request, kMCPErrorPathEscape,
			FString::Printf(TEXT("image_b_path: %s"), *SandboxErr));
	}
	if (bWantOverlay && !FMCPPathSandbox::Resolve(DiffOutRaw, AbsDiffOut, SandboxErr))
	{
		return SHOT_MakeError(Request, kMCPErrorPathEscape,
			FString::Printf(TEXT("diff_output_path: %s"), *SandboxErr));
	}

	// Existence check (LoadFileToArray would fail anyway; pre-check gives the cleaner -32004
	// surface that callers expect for "file not found" vs ambiguous decode failure).
	if (!FPaths::FileExists(AbsA))
	{
		return SHOT_MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("image_a_path file not found: '%s'"), *AbsA));
	}
	if (!FPaths::FileExists(AbsB))
	{
		return SHOT_MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("image_b_path file not found: '%s'"), *AbsB));
	}

	// Decode both images.
	FImage ImageA, ImageB;
	FString DecodeErr;
	if (!SHOT_LoadImageBGRA8(AbsA, ImageA, DecodeErr))
	{
		return SHOT_MakeError(Request, kSHOTErrorInternal,
			FString::Printf(TEXT("image_a decode: %s"), *DecodeErr));
	}
	if (!SHOT_LoadImageBGRA8(AbsB, ImageB, DecodeErr))
	{
		return SHOT_MakeError(Request, kSHOTErrorInternal,
			FString::Printf(TEXT("image_b decode: %s"), *DecodeErr));
	}

	// Build per-image dimension sub-objects (always populated, even on dimension mismatch).
	TSharedRef<FJsonObject> ASizeObj = MakeShared<FJsonObject>();
	ASizeObj->SetNumberField(TEXT("width"), ImageA.SizeX);
	ASizeObj->SetNumberField(TEXT("height"), ImageA.SizeY);
	TSharedRef<FJsonObject> BSizeObj = MakeShared<FJsonObject>();
	BSizeObj->SetNumberField(TEXT("width"), ImageB.SizeX);
	BSizeObj->SetNumberField(TEXT("height"), ImageB.SizeY);

	// Dimension mismatch: well-defined "everything differs" result, not an error.
	if (ImageA.SizeX != ImageB.SizeX || ImageA.SizeY != ImageB.SizeY)
	{
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("identical"), false);
		Out->SetNumberField(TEXT("difference_pct"), 100.0);
		Out->SetNumberField(TEXT("differing_pixels"), 0);
		Out->SetNumberField(TEXT("total_pixels"), 0);
		Out->SetObjectField(TEXT("image_a"), ASizeObj);
		Out->SetObjectField(TEXT("image_b"), BSizeObj);
		Out->SetStringField(TEXT("note"), TEXT("dimension mismatch — diff is 100% by definition"));
		return SHOT_MakeSuccessObj(Request, Out);
	}

	TArrayView64<const FColor> PixelsA = ImageA.AsBGRA8();
	TArrayView64<const FColor> PixelsB = ImageB.AsBGRA8();
	const int64 TotalPixels = PixelsA.Num();
	check(TotalPixels == PixelsB.Num());

	// Run the per-pixel compare. Allocate the differing-mask only when the caller wants an overlay
	// (saves O(W*H) bytes on every diff call).
	TArray<uint8> DifferingMask;
	const uint64 DiffCount = SHOT_CountDifferingPixels(
		PixelsA, PixelsB, kSHOTDiffChannelTolerance,
		bWantOverlay ? &DifferingMask : nullptr);

	const float DiffPct = TotalPixels > 0
		? (static_cast<float>(DiffCount) / static_cast<float>(TotalPixels)) * 100.0f
		: 0.0f;
	const bool bIdentical = (DiffCount == 0) || (DiffPct <= Threshold * 100.0f);

	// Optionally render + save the overlay.
	FString DiffWritePath;
	bool bOverlayWritten = false;
	if (bWantOverlay)
	{
		FImage Overlay;
		SHOT_BuildDiffOverlay(PixelsA, DifferingMask, ImageA.SizeX, ImageA.SizeY, Overlay);
		FString WriteErr;
		if (SHOT_SaveImageAsPNG(Overlay, AbsDiffOut, WriteErr))
		{
			DiffWritePath = AbsDiffOut;
			bOverlayWritten = true;
		}
		else
		{
			// Don't fail the whole diff just because the overlay couldn't be written — report
			// the diff result + a warning. The caller's machine-readable success branch is the
			// {identical, difference_pct} pair which is already known by this point.
			UE_LOG(LogMCP, Warning, TEXT("screenshot.diff overlay write failed: %s"), *WriteErr);
		}
	}

	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("identical"), bIdentical);
	Out->SetNumberField(TEXT("difference_pct"), DiffPct);
	Out->SetNumberField(TEXT("differing_pixels"), static_cast<double>(DiffCount));
	Out->SetNumberField(TEXT("total_pixels"), static_cast<double>(TotalPixels));
	Out->SetObjectField(TEXT("image_a"), ASizeObj);
	Out->SetObjectField(TEXT("image_b"), BSizeObj);
	Out->SetNumberField(TEXT("threshold"), Threshold);
	if (bOverlayWritten)
	{
		Out->SetStringField(TEXT("diff_image_path"), DiffWritePath);
	}
	return SHOT_MakeSuccessObj(Request, Out);
}

// ─── Registration ──────────────────────────────────────────────────────────────────────────────
void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	// high_resolution + region_capture — Lane A (viewport access).
	RegisterTool(TEXT("screenshot.high_resolution"), &Tool_HighResolution, /*Lane A*/ false);
	RegisterTool(TEXT("screenshot.region_capture"),  &Tool_RegionCapture,  /*Lane A*/ false);
	// diff — Lane B (pure image compute; no UObject/world access).
	RegisterTool(TEXT("screenshot.diff"),            &Tool_Diff,           /*Lane B*/ true);

	UE_LOG(LogMCP, Log,
		TEXT("Screenshot extended surface registered: 3 screenshot.* tools "
			 "(high_resolution + region_capture Lane A; diff Lane B), no PIE guard"));
}

} // namespace FScreenshotTools
