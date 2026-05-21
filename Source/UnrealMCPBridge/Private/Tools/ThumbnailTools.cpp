// Copyright FatumGame. All Rights Reserved.

#include "ThumbnailTools.h"

#include "FMCPDispatchQueue.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPAssetPathUtils.h"
#include "Utils/MCPPathSandbox.h"

#include "AssetRegistry/AssetData.h"
#include "Editor.h"
#include "HAL/FileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/ObjectThumbnail.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ObjectTools.h"
#include "ScopedTransaction.h"
#include "ThumbnailRendering/ThumbnailManager.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// THUMB_ prefix per the unity-build symbol-collision convention.
	constexpr int32 kTHUMBErrorInvalidParams = -32602;
	constexpr int32 kTHUMBErrorInternal      = -32603;

	// Size bounds for batch_generate match asset.get_thumbnail_to_disk (16..2048).
	constexpr int32 kTHUMBSizeMin = 16;
	constexpr int32 kTHUMBSizeMax = 2048;
	constexpr int32 kTHUMBSizeDefault = 256;

	// Hard cap on the asset_paths array per call. 256 keeps a single round-trip bounded against
	// an accidental project-wide thumbnail-rebuild (each Render is ~5-50 ms on a typical asset).
	constexpr int32 kTHUMBMaxBatchAssets = 256;

	void THUMB_StampIds(const FMCPRequest& Request, FMCPResponse& Response)
	{
		Response.RequestId = Request.RequestId;
		Response.OriginalIdString = Request.OriginalIdString;
	}

	FMCPResponse THUMB_MakeError(const FMCPRequest& Request, int32 Code, const FString& Message)
	{
		FMCPResponse R;
		THUMB_StampIds(Request, R);
		R.bIsError = true;
		R.ErrorCode = Code;
		R.ErrorMessage = Message;
		return R;
	}

	FMCPResponse THUMB_MakeSuccessObj(const FMCPRequest& Request, TSharedPtr<FJsonObject> Result)
	{
		FMCPResponse R;
		THUMB_StampIds(Request, R);
		R.bIsError = false;
		R.Result = MakeShared<FJsonValueObject>(MoveTemp(Result));
		return R;
	}

	/**
	 * Resolve a single asset path argument to a loaded UObject*. Returns nullptr on any failure
	 * (path malformed, no AR entry, LoadObject failure) and populates OutReason with a short
	 * description (forwarded into batch_generate's failures[] array).
	 *
	 * MUST RUN ON GAME THREAD — LoadObject is GT-only.
	 */
	UObject* THUMB_ResolveAssetForRender(const FString& InAssetPath, FString& OutReason)
	{
		OutReason.Reset();

		const FString Normalized = FMCPAssetPathUtils::Normalize(InAssetPath);
		if (Normalized.IsEmpty())
		{
			OutReason = FString::Printf(TEXT("malformed asset path '%s'"), *InAssetPath);
			return nullptr;
		}

		const FString ObjectPath = FMCPAssetPathUtils::ToObjectPath(Normalized);

		// Cheap FindObject for already-loaded assets first — keeps a project-wide batch cheap when
		// most assets are already in memory (typical editor session).
		UObject* Asset = FindObject<UObject>(nullptr, *ObjectPath);
		if (Asset == nullptr)
		{
			Asset = LoadObject<UObject>(nullptr, *ObjectPath);
		}
		if (Asset == nullptr)
		{
			OutReason = FString::Printf(TEXT("could not load object '%s'"), *ObjectPath);
			return nullptr;
		}
		return Asset;
	}

	/**
	 * Sanitise an asset's leaf name into a filesystem-safe stem. Strips path separators, control
	 * chars, and any character outside [A-Za-z0-9_.-]. Empty input → "thumbnail" fallback.
	 */
	FString THUMB_SanitiseLeafForFilename(const FString& InLeafName)
	{
		if (InLeafName.IsEmpty())
		{
			return TEXT("thumbnail");
		}
		FString Out;
		Out.Reserve(InLeafName.Len());
		for (TCHAR C : InLeafName)
		{
			const bool bSafe = (C >= TEXT('A') && C <= TEXT('Z'))
				|| (C >= TEXT('a') && C <= TEXT('z'))
				|| (C >= TEXT('0') && C <= TEXT('9'))
				|| C == TEXT('_') || C == TEXT('.') || C == TEXT('-');
			Out.AppendChar(bSafe ? C : TEXT('_'));
		}
		return Out.IsEmpty() ? FString(TEXT("thumbnail")) : Out;
	}

	/**
	 * Encode raw RGBA8 (Width*Height*4 bytes, BGRA layout per FObjectThumbnail convention) to PNG
	 * or JPG bytes. Returns false with empty output on encode failure.
	 */
	bool THUMB_EncodeImage(const TArray<uint8>& RGBA8, int32 Width, int32 Height, bool bJpg,
		TArray64<uint8>& OutBytes)
	{
		OutBytes.Reset();
		if (RGBA8.Num() != Width * Height * 4)
		{
			return false;
		}
		const FImageView Src(
			reinterpret_cast<const FColor*>(const_cast<uint8*>(RGBA8.GetData())),
			Width, Height, EGammaSpace::sRGB);
		const TCHAR* Format = bJpg ? TEXT("jpg") : TEXT("png");
		const int32 Quality = bJpg ? 85 : 0;
		return FImageUtils::CompressImage(OutBytes, Format, Src, Quality) && OutBytes.Num() > 0;
	}
}

namespace FThumbnailTools
{

// ─── thumbnail.batch_generate ───────────────────────────────────────────────────────────────────
//
// Args:
//   asset_paths:      [string]  required, 1..kTHUMBMaxBatchAssets entries
//   output_directory: string    required, sandbox-resolved (under <ProjectDir|Saved|Intermediate|Engine>)
//   size:             int       optional, default kTHUMBSizeDefault, range [kTHUMBSizeMin, kTHUMBSizeMax]
//   format:           string    optional, "png" (default) | "jpg"
//
// Result:
//   generated:  int                                        Successful asset count
//   failed:     int                                        Failed asset count
//   files:      [{ asset_path, file_path }]                Per-success entries
//   failures:   [{ asset_path, reason }]                   Per-failure entries (paths missing, render
//                                                          empty, encode failure, etc.)
//
// Lane A. NO PIE guard (writes to disk-sandbox, never mutates the asset's package).
FMCPResponse Tool_ThumbnailBatchGenerate(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!Request.Args.IsValid())
	{
		return THUMB_MakeError(Request, kTHUMBErrorInvalidParams, TEXT("missing args object"));
	}

	// asset_paths[] — required, non-empty, bounded by kTHUMBMaxBatchAssets.
	const TArray<TSharedPtr<FJsonValue>>* PathsArr = nullptr;
	if (!Request.Args->TryGetArrayField(TEXT("asset_paths"), PathsArr) || !PathsArr)
	{
		return THUMB_MakeError(Request, kTHUMBErrorInvalidParams,
			TEXT("missing required array field 'asset_paths'"));
	}
	if (PathsArr->Num() == 0)
	{
		return THUMB_MakeError(Request, kTHUMBErrorInvalidParams,
			TEXT("'asset_paths' must contain at least one entry"));
	}
	if (PathsArr->Num() > kTHUMBMaxBatchAssets)
	{
		return THUMB_MakeError(Request, kTHUMBErrorInvalidParams,
			FString::Printf(TEXT("'asset_paths' length %d exceeds cap %d"),
				PathsArr->Num(), kTHUMBMaxBatchAssets));
	}

	// output_directory — required, sandbox-resolved.
	FString OutDirRaw;
	if (!Request.Args->TryGetStringField(TEXT("output_directory"), OutDirRaw) || OutDirRaw.IsEmpty())
	{
		return THUMB_MakeError(Request, kTHUMBErrorInvalidParams,
			TEXT("missing required string field 'output_directory'"));
	}
	FString OutDirAbs;
	FString SandboxErr;
	if (!FMCPPathSandbox::Resolve(OutDirRaw, OutDirAbs, SandboxErr))
	{
		return THUMB_MakeError(Request, kMCPErrorPathEscape, SandboxErr);
	}

	// size — optional, [kTHUMBSizeMin, kTHUMBSizeMax].
	int32 Size = kTHUMBSizeDefault;
	Request.Args->TryGetNumberField(TEXT("size"), Size);
	if (Size < kTHUMBSizeMin || Size > kTHUMBSizeMax)
	{
		return THUMB_MakeError(Request, kTHUMBErrorInvalidParams,
			FString::Printf(TEXT("size %d outside [%d, %d]"), Size, kTHUMBSizeMin, kTHUMBSizeMax));
	}

	// format — optional, png|jpg(jpeg).
	FString Format = TEXT("png");
	Request.Args->TryGetStringField(TEXT("format"), Format);
	const bool bJpg = Format.Equals(TEXT("jpg"), ESearchCase::IgnoreCase)
		|| Format.Equals(TEXT("jpeg"), ESearchCase::IgnoreCase);
	if (!bJpg && !Format.Equals(TEXT("png"), ESearchCase::IgnoreCase))
	{
		return THUMB_MakeError(Request, kTHUMBErrorInvalidParams,
			FString::Printf(TEXT("format '%s' not in {png, jpg, jpeg}"), *Format));
	}
	const TCHAR* Ext = bJpg ? TEXT("jpg") : TEXT("png");

	// Ensure output directory exists before we start rendering — cheap up-front check (avoids
	// partial-success state where the first N renders succeed but disk writes fail).
	if (!IFileManager::Get().MakeDirectory(*OutDirAbs, /*Tree*/ true))
	{
		// Non-fatal if the directory already exists; MakeDirectory returns false in both cases.
		// We re-check via DirectoryExists to distinguish.
		if (!IFileManager::Get().DirectoryExists(*OutDirAbs))
		{
			return THUMB_MakeError(Request, kTHUMBErrorInternal,
				FString::Printf(TEXT("could not create output directory '%s'"), *OutDirAbs));
		}
	}

	TArray<TSharedPtr<FJsonValue>> FilesOut;
	TArray<TSharedPtr<FJsonValue>> FailuresOut;
	FilesOut.Reserve(PathsArr->Num());

	for (const TSharedPtr<FJsonValue>& V : *PathsArr)
	{
		if (!V.IsValid() || V->Type != EJson::String)
		{
			TSharedPtr<FJsonObject> Fail = MakeShared<FJsonObject>();
			Fail->SetStringField(TEXT("asset_path"), TEXT(""));
			Fail->SetStringField(TEXT("reason"), TEXT("entry is not a string"));
			FailuresOut.Add(MakeShared<FJsonValueObject>(Fail));
			continue;
		}
		const FString AssetPathIn = V->AsString();

		FString FailReason;
		UObject* Asset = THUMB_ResolveAssetForRender(AssetPathIn, FailReason);
		if (Asset == nullptr)
		{
			TSharedPtr<FJsonObject> Fail = MakeShared<FJsonObject>();
			Fail->SetStringField(TEXT("asset_path"), AssetPathIn);
			Fail->SetStringField(TEXT("reason"), FailReason);
			FailuresOut.Add(MakeShared<FJsonValueObject>(Fail));
			continue;
		}

		// Render via the same path as asset.get_thumbnail* — AlwaysFlush forces a render-thread
		// sync, populating RenderedThumbnail.AccessImageData with BGRA8 bytes.
		FObjectThumbnail Rendered;
		ThumbnailTools::RenderThumbnail(Asset, static_cast<uint32>(Size), static_cast<uint32>(Size),
			ThumbnailTools::EThumbnailTextureFlushMode::AlwaysFlush,
			/*InRenderTargetResource*/ nullptr,
			&Rendered);

		if (Rendered.IsEmpty() || !Rendered.HasValidImageData())
		{
			TSharedPtr<FJsonObject> Fail = MakeShared<FJsonObject>();
			Fail->SetStringField(TEXT("asset_path"), AssetPathIn);
			Fail->SetStringField(TEXT("reason"),
				TEXT("RenderThumbnail returned empty (no specific renderer + no class-generic fallback)"));
			FailuresOut.Add(MakeShared<FJsonValueObject>(Fail));
			continue;
		}

		const TArray<uint8>& RGBA8 = Rendered.GetUncompressedImageData();
		const int32 Width  = Rendered.GetImageWidth();
		const int32 Height = Rendered.GetImageHeight();

		TArray64<uint8> Encoded;
		if (!THUMB_EncodeImage(RGBA8, Width, Height, bJpg, Encoded))
		{
			TSharedPtr<FJsonObject> Fail = MakeShared<FJsonObject>();
			Fail->SetStringField(TEXT("asset_path"), AssetPathIn);
			Fail->SetStringField(TEXT("reason"),
				FString::Printf(TEXT("%s encode failed (RGBA8 size %d for %dx%d)"),
					bJpg ? TEXT("JPG") : TEXT("PNG"), RGBA8.Num(), Width, Height));
			FailuresOut.Add(MakeShared<FJsonValueObject>(Fail));
			continue;
		}

		if (Encoded.Num() > INT32_MAX)
		{
			TSharedPtr<FJsonObject> Fail = MakeShared<FJsonObject>();
			Fail->SetStringField(TEXT("asset_path"), AssetPathIn);
			Fail->SetStringField(TEXT("reason"),
				TEXT("encoded thumbnail exceeds 2 GiB — refusing to write"));
			FailuresOut.Add(MakeShared<FJsonValueObject>(Fail));
			continue;
		}

		// Filename = sanitised asset leaf + ".<ext>". On collisions (same leaf in different
		// packages) we deliberately overwrite — the caller asked for a flat directory of
		// thumbnails; if they need distinct names they can pre-sanitise their paths.
		const FString LeafName = FPaths::GetBaseFilename(AssetPathIn);
		const FString SafeStem = THUMB_SanitiseLeafForFilename(LeafName);
		const FString FilePath = FPaths::Combine(OutDirAbs,
			FString::Printf(TEXT("%s.%s"), *SafeStem, Ext));

		TArray<uint8> WriteBuf;
		WriteBuf.Append(Encoded.GetData(), static_cast<int32>(Encoded.Num()));
		if (!FFileHelper::SaveArrayToFile(WriteBuf, *FilePath))
		{
			TSharedPtr<FJsonObject> Fail = MakeShared<FJsonObject>();
			Fail->SetStringField(TEXT("asset_path"), AssetPathIn);
			Fail->SetStringField(TEXT("reason"),
				FString::Printf(TEXT("could not write file '%s'"), *FilePath));
			FailuresOut.Add(MakeShared<FJsonValueObject>(Fail));
			continue;
		}

		TSharedPtr<FJsonObject> Ok = MakeShared<FJsonObject>();
		Ok->SetStringField(TEXT("asset_path"), AssetPathIn);
		Ok->SetStringField(TEXT("file_path"), FilePath);
		FilesOut.Add(MakeShared<FJsonValueObject>(Ok));
	}

	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetNumberField(TEXT("generated"), FilesOut.Num());
	Out->SetNumberField(TEXT("failed"), FailuresOut.Num());
	Out->SetArrayField(TEXT("files"), FilesOut);
	Out->SetArrayField(TEXT("failures"), FailuresOut);
	return THUMB_MakeSuccessObj(Request, Out);
}

// ─── thumbnail.clear_cache ──────────────────────────────────────────────────────────────────────
//
// Args:
//   asset_paths:      [string]  optional. Omitted/empty → walk every loaded package's ThumbnailMap.
//   force_regenerate: bool      optional, default false. When true, immediately re-render every
//                                cleared asset via RenderThumbnail(AlwaysFlush) so the cache
//                                repopulates with fresh data.
//
// Result:
//   cleared_count:     int    Total in-memory cache entries replaced with empty thumbnails.
//   regenerated_count: int    Number of those re-rendered when force_regenerate=true (0 otherwise).
//
// Lane A. NO PIE guard (cache touch is transient — no asset mutation, no MarkPackageDirty).
//
// **Implementation note.** ``ThumbnailTools::CacheEmptyThumbnail`` calls
// ``CacheThumbnail(name, &EmptyThumbnail, package)``, which REPLACES the existing map entry with
// an empty FObjectThumbnail. The next ``GetThumbnailForObject`` will see ``IsEmpty()==true`` +
// ``HasValidImageData()==false`` and re-render on demand. This is the canonical "invalidate
// cache" pathway in the engine (matches Editor's "Refresh Thumbnail" content-browser menu).
FMCPResponse Tool_ThumbnailClearCache(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!Request.Args.IsValid())
	{
		return THUMB_MakeError(Request, kTHUMBErrorInvalidParams, TEXT("missing args object"));
	}

	bool bForceRegenerate = false;
	Request.Args->TryGetBoolField(TEXT("force_regenerate"), bForceRegenerate);

	// Asset paths can be omitted → walk all loaded packages. When provided, target specific assets.
	const TArray<TSharedPtr<FJsonValue>>* PathsArr = nullptr;
	const bool bHasPaths = Request.Args->TryGetArrayField(TEXT("asset_paths"), PathsArr)
		&& PathsArr != nullptr && PathsArr->Num() > 0;

	// We accumulate (Package, ObjectFullName) pairs and assets-to-regenerate so we can do the
	// CacheEmptyThumbnail + (optional) RenderThumbnail in one pass without re-resolving assets.
	struct FClearTarget
	{
		UPackage* Package;
		FString   ObjectFullName;
		UObject*  Asset;             // nullptr when bHasPaths == false (no need to re-render
		                             // every loaded asset — only explicit paths get re-rendered).
	};
	TArray<FClearTarget> Targets;

	if (bHasPaths)
	{
		Targets.Reserve(PathsArr->Num());
		for (const TSharedPtr<FJsonValue>& V : *PathsArr)
		{
			if (!V.IsValid() || V->Type != EJson::String) { continue; }
			const FString AssetPathIn = V->AsString();

			FString FailReason;
			UObject* Asset = THUMB_ResolveAssetForRender(AssetPathIn, FailReason);
			if (Asset == nullptr)
			{
				// Skip silently — the contract is "clear what we can"; bad paths are caller's
				// problem. Returning per-failure detail would clutter the response; the tests
				// can hit -32004 by going through a strict resolution path (set_custom does this).
				continue;
			}
			UPackage* Pkg = Asset->GetOutermost();
			if (Pkg == nullptr) { continue; }
			Targets.Add({Pkg, Asset->GetFullName(), Asset});
		}
	}
	else
	{
		// No explicit list — enumerate every loaded package that has a ThumbnailMap and queue
		// every entry. We walk packages via TObjectIterator<UPackage> (cheap; package set is
		// O(thousands) on a typical editor session). We DO NOT enumerate UObjectHash for every
		// entry — the map keys are already-cached object full names; that's enough for
		// CacheEmptyThumbnail (which only needs the FName + UPackage*).
		for (TObjectIterator<UPackage> It; It; ++It)
		{
			UPackage* Pkg = *It;
			if (Pkg == nullptr || !Pkg->HasThumbnailMap()) { continue; }
			const FThumbnailMap& Map = Pkg->GetThumbnailMap();
			Targets.Reserve(Targets.Num() + Map.Num());
			for (const TPair<FName, FObjectThumbnail>& Pair : Map)
			{
				Targets.Add({Pkg, Pair.Key.ToString(), /*Asset*/ nullptr});
			}
		}
	}

	int32 ClearedCount = 0;
	int32 RegeneratedCount = 0;

	for (const FClearTarget& T : Targets)
	{
		ThumbnailTools::CacheEmptyThumbnail(T.ObjectFullName, T.Package);
		++ClearedCount;

		if (bForceRegenerate && T.Asset != nullptr)
		{
			FObjectThumbnail Rendered;
			ThumbnailTools::RenderThumbnail(T.Asset,
				static_cast<uint32>(ThumbnailTools::DefaultThumbnailSize),
				static_cast<uint32>(ThumbnailTools::DefaultThumbnailSize),
				ThumbnailTools::EThumbnailTextureFlushMode::AlwaysFlush,
				/*InRenderTargetResource*/ nullptr,
				&Rendered);
			if (!Rendered.IsEmpty() && Rendered.HasValidImageData())
			{
				ThumbnailTools::CacheThumbnail(T.ObjectFullName, &Rendered, T.Package);
				++RegeneratedCount;
			}
		}
	}

	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetNumberField(TEXT("cleared_count"), ClearedCount);
	Out->SetNumberField(TEXT("regenerated_count"), RegeneratedCount);
	return THUMB_MakeSuccessObj(Request, Out);
}

// ─── thumbnail.set_custom ───────────────────────────────────────────────────────────────────────
//
// Args:
//   asset_path: string   required
//   image_path: string   required, PNG/JPG on disk, sandbox-resolved
//
// Result:
//   set:               bool          true on success
//   image_resolution:  [width, height]  decoded source image dimensions (the cached thumbnail
//                                      retains these dims — NO downscale, the caller is responsible
//                                      for supplying an appropriately-sized image, typical 256x256)
//
// Lane A. PIE-guarded (mutates the asset's package — marks dirty for next SaveLoadedAsset).
// FScopedTransaction for undo. MarkPackageDirty.
FMCPResponse Tool_ThumbnailSetCustom(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (GEditor && GEditor->PlayWorld != nullptr)
	{
		return THUMB_MakeError(Request, kMCPErrorPIEActive, kMCPMessagePIEActive);
	}

	if (!Request.Args.IsValid())
	{
		return THUMB_MakeError(Request, kTHUMBErrorInvalidParams, TEXT("missing args object"));
	}

	// asset_path — required, must resolve to a loaded UObject.
	FString AssetPathIn;
	if (!Request.Args->TryGetStringField(TEXT("asset_path"), AssetPathIn) || AssetPathIn.IsEmpty())
	{
		return THUMB_MakeError(Request, kTHUMBErrorInvalidParams,
			TEXT("missing required string field 'asset_path'"));
	}
	FString AssetFailReason;
	UObject* Asset = THUMB_ResolveAssetForRender(AssetPathIn, AssetFailReason);
	if (Asset == nullptr)
	{
		return THUMB_MakeError(Request, kMCPErrorObjectNotFound, AssetFailReason);
	}

	// image_path — required, sandbox-resolved.
	FString ImagePathRaw;
	if (!Request.Args->TryGetStringField(TEXT("image_path"), ImagePathRaw) || ImagePathRaw.IsEmpty())
	{
		return THUMB_MakeError(Request, kTHUMBErrorInvalidParams,
			TEXT("missing required string field 'image_path'"));
	}
	FString ImagePathAbs;
	FString SandboxErr;
	if (!FMCPPathSandbox::Resolve(ImagePathRaw, ImagePathAbs, SandboxErr))
	{
		return THUMB_MakeError(Request, kMCPErrorPathEscape, SandboxErr);
	}
	if (!IFileManager::Get().FileExists(*ImagePathAbs))
	{
		return THUMB_MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("image file '%s' does not exist"), *ImagePathAbs));
	}

	// Read + decode the source image into BGRA8/sRGB via the central ImageWrapper API.
	TArray<uint8> CompressedBytes;
	if (!FFileHelper::LoadFileToArray(CompressedBytes, *ImagePathAbs) || CompressedBytes.Num() == 0)
	{
		return THUMB_MakeError(Request, kMCPErrorThumbnailRenderFailed,
			FString::Printf(TEXT("could not read image file '%s'"), *ImagePathAbs));
	}

	IImageWrapperModule& IWM =
		FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));

	FImage Decoded;
	if (!IWM.DecompressImage(CompressedBytes.GetData(), CompressedBytes.Num(), Decoded))
	{
		return THUMB_MakeError(Request, kMCPErrorThumbnailRenderFailed,
			FString::Printf(TEXT("ImageWrapper failed to decode '%s' "
				"(not a recognised PNG/JPG/EXR/HDR/TGA/BMP format?)"), *ImagePathAbs));
	}

	// Normalise to the format FObjectThumbnail expects: BGRA8/sRGB. ChangeFormat is a no-op when
	// already in that form; PNGs with RGB8 / grayscale / RGBA8 / etc. all get converted here.
	Decoded.ChangeFormat(ERawImageFormat::BGRA8, EGammaSpace::sRGB);

	const int32 Width  = Decoded.SizeX;
	const int32 Height = Decoded.SizeY;
	if (Width <= 0 || Height <= 0)
	{
		return THUMB_MakeError(Request, kMCPErrorThumbnailRenderFailed,
			FString::Printf(TEXT("decoded image '%s' has zero dimensions"), *ImagePathAbs));
	}

	// Build the FObjectThumbnail from the decoded image. SetImage(FImage&&) handles dimension +
	// data move in one call and sets the format flags correctly; SetCreatedAfterCustomThumbsEnabled
	// marks it as a "custom" thumbnail (engine uses this flag to skip the auto-regenerate path
	// in the content-browser refresh logic — without it, the next ThumbnailManager request would
	// overwrite our custom thumbnail with a freshly-rendered preview).
	FObjectThumbnail NewThumb;
	NewThumb.SetImage(MoveTemp(Decoded));
	NewThumb.SetCreatedAfterCustomThumbsEnabled();
	NewThumb.MarkAsDirty(); // signals the package serialiser to persist this thumbnail on save.

	UPackage* Pkg = Asset->GetOutermost();
	if (Pkg == nullptr)
	{
		return THUMB_MakeError(Request, kTHUMBErrorInternal,
			FString::Printf(TEXT("asset '%s' has no outer package"), *Asset->GetFullName()));
	}

	const FScopedTransaction Transaction(LOCTEXT("MCPThumbnailSetCustom", "MCP: set custom thumbnail"));
	Pkg->Modify(); // transact the package so undo restores the previous thumbnail (if any).

	const FString ObjectFullName = Asset->GetFullName();
	FObjectThumbnail* Cached =
		ThumbnailTools::CacheThumbnail(ObjectFullName, &NewThumb, Pkg);
	if (Cached == nullptr)
	{
		// Should not happen — CacheThumbnail only returns nullptr when ObjectFullName is empty
		// or Package is nullptr, both of which we've already validated.
		return THUMB_MakeError(Request, kTHUMBErrorInternal,
			FString::Printf(TEXT("ThumbnailTools::CacheThumbnail returned null for '%s'"),
				*ObjectFullName));
	}

	Pkg->MarkPackageDirty();

	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("set"), true);
	TArray<TSharedPtr<FJsonValue>> ResolutionArr;
	ResolutionArr.Add(MakeShared<FJsonValueNumber>(Width));
	ResolutionArr.Add(MakeShared<FJsonValueNumber>(Height));
	Out->SetArrayField(TEXT("image_resolution"), ResolutionArr);
	return THUMB_MakeSuccessObj(Request, Out);
}

// ─── Registration ───────────────────────────────────────────────────────────────────────────────
void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	// All Lane A — RenderThumbnail enqueues to render thread, ThumbnailMap writes touch UPackage
	// (game-thread-only), MarkPackageDirty broadcasts editor delegates.
	RegisterTool(TEXT("thumbnail.batch_generate"), &Tool_ThumbnailBatchGenerate, /*Lane A*/ false);
	RegisterTool(TEXT("thumbnail.clear_cache"),    &Tool_ThumbnailClearCache,    /*Lane A*/ false);
	RegisterTool(TEXT("thumbnail.set_custom"),     &Tool_ThumbnailSetCustom,     /*Lane A*/ false);

	UE_LOG(LogMCP, Log, TEXT("Wave H Surface 5: registered 3 thumbnail.* handlers (all Lane A)"));
}

} // namespace FThumbnailTools

#undef LOCTEXT_NAMESPACE
