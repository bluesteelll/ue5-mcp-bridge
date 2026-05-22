// Copyright FatumGame. All Rights Reserved.

#include "RenderTargetTools.h"

#include "MCPSurfaceRegistry.h"

#include "FMCPDispatchQueue.h"
#include "MCPAssetLoader.h"
#include "MCPJsonBuilder.h"
#include "MCPToolHelpers.h"
#include "Utils/MCPPathSandbox.h"

#include "Engine/TextureRenderTarget2D.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Base64.h"
#include "RenderingThread.h"
#include "TextureResource.h"
#include "UObject/UObjectGlobals.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// RTT_ prefix per the unity-build symbol-collision convention.
	constexpr int32 kRTTErrorInternal           = -32603;
	constexpr int32 kRTTErrorInputTooLarge      = kMCPErrorInputTooLarge;       // -32017
	constexpr int32 kRTTErrorThumbnailRenderFail = kMCPErrorThumbnailRenderFailed; // -32018

	// Hard cap: 4096*4096 = 16.7M pixels * 4 bytes (BGRA) = 64 MB raw + ~30 MB PNG. Bigger
	// would risk OOM on memory-constrained editor configs.
	constexpr int64 kMaxPixelCount = 4096LL * 4096LL;

	const TCHAR* RTT_PixelFormatToString(EPixelFormat Format)
	{
		switch (Format)
		{
			case PF_B8G8R8A8:     return TEXT("BGRA8");
			case PF_R8G8B8A8:     return TEXT("RGBA8");
			case PF_FloatRGBA:    return TEXT("FloatRGBA");
			case PF_FloatRGB:     return TEXT("FloatRGB");
			case PF_R32_FLOAT:    return TEXT("R32F");
			case PF_DepthStencil: return TEXT("DepthStencil");
			case PF_G8:           return TEXT("G8");
			case PF_R16F:         return TEXT("R16F");
			default:              return TEXT("Other");
		}
	}

	const TCHAR* RTT_RenderTargetFormatToString(ETextureRenderTargetFormat Format)
	{
		switch (Format)
		{
			case RTF_R8:      return TEXT("R8");
			case RTF_RG8:     return TEXT("RG8");
			case RTF_RGBA8:   return TEXT("RGBA8");
			case RTF_RGBA8_SRGB: return TEXT("RGBA8_sRGB");
			case RTF_R16f:    return TEXT("R16f");
			case RTF_RG16f:   return TEXT("RG16f");
			case RTF_RGBA16f: return TEXT("RGBA16f");
			case RTF_R32f:    return TEXT("R32f");
			case RTF_RG32f:   return TEXT("RG32f");
			case RTF_RGBA32f: return TEXT("RGBA32f");
			case RTF_RGB10A2: return TEXT("RGB10A2");
			default:          return TEXT("Other");
		}
	}
} // namespace

namespace FRenderTargetTools
{

// --- render_target.get_info -------------------------------------------------------------------
//
// Args:    { render_target_path: string }
// Result:  { path, size_x, size_y, format, render_target_format,
//            num_mips, srgb, has_gpu_resource, address_x, address_y, clear_color: [r,g,b,a] }
//
// Read-only — no PIE guard. Loads the asset and inspects its UPROPERTY metadata. Does NOT
// touch the GPU resource; use render_target.dump for that.
FMCPResponse Tool_GetInfo(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString RTPath;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("render_target_path"), RTPath, Err)) return Err;

	int32 LoadErrCode = 0;
	FString LoadErrMsg;
	UTextureRenderTarget2D* RT = FMCPAssetLoader::Load<UTextureRenderTarget2D>(RTPath, LoadErrCode, LoadErrMsg);
	if (!RT) return FMCPToolHelpers::MakeError(Request, LoadErrCode, LoadErrMsg);

	const FLinearColor ClearColor = RT->ClearColor;
	const bool bHasGpuResource = (RT->GameThread_GetRenderTargetResource() != nullptr);

	return FMCPJsonBuilder()
		.Str(TEXT("path"), RTPath)
		.Int(TEXT("size_x"), RT->SizeX)
		.Int(TEXT("size_y"), RT->SizeY)
		.Str(TEXT("format"), RTT_PixelFormatToString(RT->GetFormat()))
		.Str(TEXT("render_target_format"), RTT_RenderTargetFormatToString(RT->RenderTargetFormat))
		.Int(TEXT("num_mips"), RT->GetNumMips())
		.Bool(TEXT("srgb"), RT->SRGB)
		.Bool(TEXT("has_gpu_resource"), bHasGpuResource)
		.Int(TEXT("address_x"), static_cast<int32>(RT->AddressX))
		.Int(TEXT("address_y"), static_cast<int32>(RT->AddressY))
		.Arr(TEXT("clear_color"), {
			MakeShared<FJsonValueNumber>(ClearColor.R),
			MakeShared<FJsonValueNumber>(ClearColor.G),
			MakeShared<FJsonValueNumber>(ClearColor.B),
			MakeShared<FJsonValueNumber>(ClearColor.A),
		})
		.BuildSuccess(Request);
}

// --- render_target.dump -----------------------------------------------------------------------
//
// Args:    { render_target_path: string,
//            output_path?: string,           // disk path; if absent → inline base64
//            format?: "png" | "exr"           // default "png" (BGRA→RGBA8 PNG via FImageUtils) }
// Result (output_path set):   { saved_path, size_x, size_y, format, bytes_written }
// Result (no output_path):    { encoded_png_b64, size_x, size_y, format, bytes_encoded }
//
// Flushes rendering commands before reading pixel buffer (mandatory for GPU readback).
// Hard cap 4096x4096 = 16.7M pixels — bigger → -32017 InputTooLarge.
//
// PNG path uses FImageUtils::CompressImageArray (BGRA8 input, lossless PNG output).
// EXR path uses FImageUtils::CompressImage with EImageFormat::EXR for HDR float targets.
//
// Inline-base64 result is intended for small thumbnails (≤ 1MB encoded). Anything larger should
// use output_path to avoid blowing up the JSON-RPC response frame size.
FMCPResponse Tool_Dump(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString RTPath;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("render_target_path"), RTPath, Err)) return Err;

	int32 LoadErrCode = 0;
	FString LoadErrMsg;
	UTextureRenderTarget2D* RT = FMCPAssetLoader::Load<UTextureRenderTarget2D>(RTPath, LoadErrCode, LoadErrMsg);
	if (!RT) return FMCPToolHelpers::MakeError(Request, LoadErrCode, LoadErrMsg);

	// Size cap.
	const int64 PixelCount = static_cast<int64>(RT->SizeX) * static_cast<int64>(RT->SizeY);
	if (PixelCount > kMaxPixelCount)
	{
		return FMCPToolHelpers::MakeError(Request, kRTTErrorInputTooLarge,
			FString::Printf(TEXT("render target %dx%d (%lld pixels) exceeds the 4096x4096 cap"),
				RT->SizeX, RT->SizeY, PixelCount));
	}

	// Format selection.
	FString FormatStr = TEXT("png");
	if (Request.Args.IsValid()) { Request.Args->TryGetStringField(TEXT("format"), FormatStr); }
	FormatStr = FormatStr.ToLower();
	if (FormatStr != TEXT("png") && FormatStr != TEXT("exr"))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			FString::Printf(TEXT("format must be 'png' or 'exr' (got '%s')"), *FormatStr));
	}

	// Render-thread sync: ensure all pending render commands have completed so the GPU buffer is stable.
	FlushRenderingCommands();

	FTextureRenderTargetResource* RTRes = RT->GameThread_GetRenderTargetResource();
	if (!RTRes)
	{
		return FMCPToolHelpers::MakeError(Request, kRTTErrorThumbnailRenderFail,
			TEXT("render target has no GPU-side resource — has it ever been rendered to?"));
	}

	// PNG path — BGRA8 readback + FImageUtils::CompressImageArray.
	// FImageUtils encode APIs require FDefaultAllocator64 (TArray64) for the output buffer to
	// support >2 GiB image payloads. Our 64 MB cap ensures we stay well under int32 range, but
	// the API contract demands the 64-bit allocator.
	TArray64<uint8> EncodedBytes;
	if (FormatStr == TEXT("png"))
	{
		TArray<FColor> Bitmap;
		FReadSurfaceDataFlags ReadPixelFlags(RCM_UNorm);
		if (!RTRes->ReadPixels(Bitmap, ReadPixelFlags))
		{
			return FMCPToolHelpers::MakeError(Request, kRTTErrorThumbnailRenderFail,
				TEXT("ReadPixels(BGRA8) failed"));
		}
		FImageUtils::PNGCompressImageArray(RT->SizeX, RT->SizeY, Bitmap, EncodedBytes);
	}
	else // "exr"
	{
		// EXR readback — float HDR pipeline. Format must be an Float-family target.
		TArray<FLinearColor> Bitmap;
		FReadSurfaceDataFlags ReadPixelFlags(RCM_MinMax);
		if (!RTRes->ReadLinearColorPixels(Bitmap, ReadPixelFlags))
		{
			return FMCPToolHelpers::MakeError(Request, kRTTErrorThumbnailRenderFail,
				TEXT("ReadLinearColorPixels(float) failed — target may not be an HDR format"));
		}
		FImageView ImgView(Bitmap.GetData(), RT->SizeX, RT->SizeY, ERawImageFormat::RGBA32F);
		if (!FImageUtils::CompressImage(EncodedBytes, TEXT("exr"), ImgView))
		{
			return FMCPToolHelpers::MakeError(Request, kRTTErrorThumbnailRenderFail,
				TEXT("EXR encode failed"));
		}
	}

	if (EncodedBytes.Num() == 0)
	{
		return FMCPToolHelpers::MakeError(Request, kRTTErrorThumbnailRenderFail,
			TEXT("encode produced 0 bytes"));
	}

	// Output mode A: disk path.
	FString OutputPath;
	if (Request.Args.IsValid()) { Request.Args->TryGetStringField(TEXT("output_path"), OutputPath); }
	if (!OutputPath.IsEmpty())
	{
		// Sandbox check — output must be inside the project / saved / intermediate trees.
		FString CanonicalPath, SandboxErr;
		if (!FMCPPathSandbox::Resolve(OutputPath, CanonicalPath, SandboxErr))
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorPathEscape,
				FString::Printf(TEXT("output_path '%s' rejected: %s"), *OutputPath, *SandboxErr));
		}
		// FFileHelper::SaveArrayToFile expects TArray<uint8> (32-bit allocator). Copy from our
		// TArray64 — payload is capped at <64MB so the copy is bounded and fits within int32.
		TArray<uint8> EncodedBytes32(EncodedBytes.GetData(), static_cast<int32>(EncodedBytes.Num()));
		if (!FFileHelper::SaveArrayToFile(EncodedBytes32, *CanonicalPath))
		{
			return FMCPToolHelpers::MakeError(Request, kRTTErrorThumbnailRenderFail,
				FString::Printf(TEXT("failed to write %lld bytes to '%s'"), EncodedBytes.Num(), *CanonicalPath));
		}
		return FMCPJsonBuilder()
			.Str(TEXT("saved_path"), CanonicalPath)
			.Int(TEXT("size_x"), RT->SizeX)
			.Int(TEXT("size_y"), RT->SizeY)
			.Str(TEXT("format"), FormatStr)
			.Int(TEXT("bytes_written"), EncodedBytes.Num())
			.BuildSuccess(Request);
	}

	// Output mode B: inline base64 (caps at the FMCPFrameMaxBytes / 2 = 32 MiB net of base64 inflation).
	if (EncodedBytes.Num() > 24 * 1024 * 1024)
	{
		return FMCPToolHelpers::MakeError(Request, kRTTErrorInputTooLarge,
			FString::Printf(TEXT("encoded payload %d bytes exceeds 24 MiB inline cap; pass output_path"),
				EncodedBytes.Num()));
	}
	const FString B64 = FBase64::Encode(EncodedBytes.GetData(), static_cast<int32>(EncodedBytes.Num()));
	return FMCPJsonBuilder()
		.Str(TEXT("encoded_png_b64"), B64)
		.Int(TEXT("size_x"), RT->SizeX)
		.Int(TEXT("size_y"), RT->SizeY)
		.Str(TEXT("format"), FormatStr)
		.Int(TEXT("bytes_encoded"), EncodedBytes.Num())
		.BuildSuccess(Request);
}

void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	// Both Lane A — LoadObject + FlushRenderingCommands + GPU readback are GT-only.
	RegisterTool(TEXT("render_target.get_info"), &Tool_GetInfo, /*Lane A*/ false);
	RegisterTool(TEXT("render_target.dump"),     &Tool_Dump,    /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("RenderTarget surface registered: render_target.get_info + render_target.dump (Lane A)"));
}

} // namespace FRenderTargetTools

MCP_REGISTER_SURFACE(RenderTargetTools, &FRenderTargetTools::Register)

#undef LOCTEXT_NAMESPACE
