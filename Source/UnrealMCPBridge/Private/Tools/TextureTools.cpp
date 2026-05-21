// Copyright FatumGame. All Rights Reserved.

#include "TextureTools.h"

#include "FMCPDispatchQueue.h"
#include "MCPAssetLoader.h"
#include "MCPMutatorScope.h"
#include "MCPToolHelpers.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPAssetPathUtils.h"
#include "Utils/MCPPageCursor.h"
#include "Utils/MCPWorldContext.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Editor.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureDefines.h"
#include "Engine/TextureLODSettings.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PixelFormat.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// TEX_ prefix per unity-build symbol-collision convention (MakeError/MakeSuccess shapes recur
	// across sibling tool TUs in the unity build — keep prefixes per surface).
	constexpr int32 kTEXErrorInternal = -32603;

	/**
	 * String → TextureCompressionSettings. Whitelist mirrors the brief's enum table. Returns
	 * TC_MAX on miss (caller treats as -32602 InvalidParams) — TC_MAX is the sentinel sentinel.
	 *
	 * Note: TC_EditorIcon and TC_Displacementmap exist in the engine but are deprecated /
	 * non-recommended (deprecated for displacement; editor-only for icons) — exposing them
	 * would only invite confusion. Skipped intentionally.
	 */
	TextureCompressionSettings TEX_ParseCompression(const FString& Name)
	{
		if (Name == TEXT("TC_Default"))               { return TC_Default; }
		if (Name == TEXT("TC_Normalmap"))             { return TC_Normalmap; }
		if (Name == TEXT("TC_Masks"))                 { return TC_Masks; }
		if (Name == TEXT("TC_Grayscale"))             { return TC_Grayscale; }
		if (Name == TEXT("TC_HDR"))                   { return TC_HDR; }
		if (Name == TEXT("TC_Alpha"))                 { return TC_Alpha; }
		if (Name == TEXT("TC_BC7"))                   { return TC_BC7; }
		if (Name == TEXT("TC_HalfFloat"))             { return TC_HalfFloat; }
		if (Name == TEXT("TC_LQ"))                    { return TC_LQ; }
		if (Name == TEXT("TC_VectorDisplacementmap")) { return TC_VectorDisplacementmap; }
		if (Name == TEXT("TC_DistanceFieldFont"))     { return TC_DistanceFieldFont; }
		return TC_MAX;
	}

	/** TextureCompressionSettings → canonical "TC_*" string for response payloads. */
	const TCHAR* TEX_CompressionToString(TextureCompressionSettings Value)
	{
		switch (Value)
		{
		case TC_Default:                return TEXT("TC_Default");
		case TC_Normalmap:              return TEXT("TC_Normalmap");
		case TC_Masks:                  return TEXT("TC_Masks");
		case TC_Grayscale:              return TEXT("TC_Grayscale");
		case TC_Displacementmap:        return TEXT("TC_Displacementmap");
		case TC_VectorDisplacementmap:  return TEXT("TC_VectorDisplacementmap");
		case TC_HDR:                    return TEXT("TC_HDR");
		case TC_EditorIcon:             return TEXT("TC_EditorIcon");
		case TC_Alpha:                  return TEXT("TC_Alpha");
		case TC_DistanceFieldFont:      return TEXT("TC_DistanceFieldFont");
		case TC_HDR_Compressed:         return TEXT("TC_HDR_Compressed");
		case TC_BC7:                    return TEXT("TC_BC7");
		case TC_HalfFloat:              return TEXT("TC_HalfFloat");
		case TC_LQ:                     return TEXT("TC_LQ");
		case TC_EncodedReflectionCapture: return TEXT("TC_EncodedReflectionCapture");
		case TC_SingleFloat:            return TEXT("TC_SingleFloat");
		case TC_HDR_F32:                return TEXT("TC_HDR_F32");
		default:                        return TEXT("TC_Unknown");
		}
	}

	/** TextureAddress → string. */
	const TCHAR* TEX_AddressToString(TextureAddress Value)
	{
		switch (Value)
		{
		case TA_Wrap:   return TEXT("TA_Wrap");
		case TA_Clamp:  return TEXT("TA_Clamp");
		case TA_Mirror: return TEXT("TA_Mirror");
		default:        return TEXT("TA_Unknown");
		}
	}

	/**
	 * TextureGroup → canonical TEXTUREGROUP_* string via UTextureLODSettings::GetTextureGroupNames().
	 * That helper returns the same ordered array UE itself uses (FOREACH_ENUM_TEXTUREGROUP). Falls
	 * back to "TEXTUREGROUP_Unknown_<n>" if the index isn't in range (defensive — should never
	 * trigger in practice but keeps the response well-formed).
	 */
	FString TEX_LODGroupToString(TextureGroup Value)
	{
		const int32 Idx = static_cast<int32>(Value);
		const TArray<FString> GroupNames = UTextureLODSettings::GetTextureGroupNames();
		if (GroupNames.IsValidIndex(Idx)) { return GroupNames[Idx]; }
		return FString::Printf(TEXT("TEXTUREGROUP_Unknown_%d"), Idx);
	}
} // namespace

namespace FTextureTools
{

// ─── texture.list ─────────────────────────────────────────────────────────────────────────────
//
// Args:    { path_prefix?: string, page_size?: int (default 100, clamp [1,1000]), page_token?: string }
// Result:  { textures: [{ asset_path, name }], total_known, next_page_token? }
//
// Mirror of mesh.list — same FARFilter + FMCPPageCursor pattern, swap UStaticMesh for UTexture2D.
FMCPResponse Tool_List(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString PathPrefix;
	if (Request.Args.IsValid()) { Request.Args->TryGetStringField(TEXT("path_prefix"), PathPrefix); }

	int32 PageSize = 100;
	if (Request.Args.IsValid()) { Request.Args->TryGetNumberField(TEXT("page_size"), PageSize); }
	PageSize = FMath::Clamp(PageSize, 1, 1000);

	FString PageToken;
	if (Request.Args.IsValid()) { Request.Args->TryGetStringField(TEXT("page_token"), PageToken); }

	// FilterHash so cursor staleness is detectable across pages.
	const uint32 FilterHash = GetTypeHash(PathPrefix);

	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FARFilter Filter;
	Filter.ClassPaths.Add(UTexture2D::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = false;
	Filter.bRecursivePaths   = true;
	if (!PathPrefix.IsEmpty())
	{
		Filter.PackagePaths.Add(*PathPrefix);
	}
	TArray<FAssetData> Assets;
	AR.GetAssets(Filter, Assets);

	// Stable sort by ObjectPath (keyset pagination sort key).
	Assets.Sort([](const FAssetData& A, const FAssetData& B)
	{
		return A.GetSoftObjectPath().ToString() < B.GetSoftObjectPath().ToString();
	});

	// Decode + validate cursor.
	int32 StartIdx = 0;
	FMCPPageCursor InCursor;
	if (!PageToken.IsEmpty())
	{
		FString DecodeErr;
		if (!FMCPPageCursorUtils::Decode(PageToken, InCursor, DecodeErr))
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
				FString::Printf(TEXT("invalid page_token: %s"), *DecodeErr));
		}
		if (!FMCPPageCursorUtils::ValidateAgainstFilter(InCursor, FilterHash))
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorStaleCursor,
				TEXT("filter mutated between pages (path_prefix changed); restart pagination"));
		}
		while (StartIdx < Assets.Num() &&
		       Assets[StartIdx].GetSoftObjectPath().ToString() <= InCursor.LastAssetPath)
		{
			++StartIdx;
		}
	}

	TArray<TSharedPtr<FJsonValue>> TexArr;
	const int32 EndIdx = FMath::Min(StartIdx + PageSize, Assets.Num());
	TexArr.Reserve(EndIdx - StartIdx);
	for (int32 i = StartIdx; i < EndIdx; ++i)
	{
		const FAssetData& A = Assets[i];
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("asset_path"), A.GetSoftObjectPath().ToString());
		Obj->SetStringField(TEXT("name"), A.AssetName.ToString());
		TexArr.Add(MakeShared<FJsonValueObject>(Obj));
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetArrayField(TEXT("textures"), TexArr);
	Out->SetNumberField(TEXT("total_known"), Assets.Num());

	if (EndIdx < Assets.Num() && EndIdx > 0)
	{
		FMCPPageCursor OutCursor;
		OutCursor.FilterHash = FilterHash;
		OutCursor.LastAssetPath = Assets[EndIdx - 1].GetSoftObjectPath().ToString();
		OutCursor.TotalKnownSnapshot = Assets.Num();
		Out->SetStringField(TEXT("next_page_token"), FMCPPageCursorUtils::Encode(OutCursor));
	}

	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── texture.get_info ─────────────────────────────────────────────────────────────────────────
//
// Args:    { texture_path: string }
// Result:  { asset_path, size: [w, h], pixel_format: string, compression: string, mip_count: int,
//            srgb: bool, lod_group: string, lod_bias: int, never_stream: bool,
//            address_x: string, address_y: string }
//
// Read-only — no PIE guard, no transaction. GetSizeX/Y use the *current* texture state which
// reflects ImportedSize/Source; this differs from Source.GetSizeX (which always reads source data)
// but matches what the renderer actually sees.
FMCPResponse Tool_GetInfo(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString TexPath;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("texture_path"), TexPath, Err)) { return Err; }

	int32 LoadErrCode = 0;
	FString LoadErrMsg;
	UTexture2D* Tex = FMCPAssetLoader::Load<UTexture2D>(TexPath, LoadErrCode, LoadErrMsg);
	if (!Tex) { return FMCPToolHelpers::MakeError(Request, LoadErrCode, LoadErrMsg); }

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("asset_path"), Tex->GetPathName());

	// Size — [W, H] as JSON array.
	{
		TArray<TSharedPtr<FJsonValue>> Size;
		Size.Reserve(2);
		Size.Add(MakeShared<FJsonValueNumber>(Tex->GetSizeX()));
		Size.Add(MakeShared<FJsonValueNumber>(Tex->GetSizeY()));
		Out->SetArrayField(TEXT("size"), Size);
	}

	Out->SetStringField(TEXT("pixel_format"), GetPixelFormatString(Tex->GetPixelFormat()));
	Out->SetStringField(TEXT("compression"),  TEX_CompressionToString(Tex->CompressionSettings));
	Out->SetNumberField(TEXT("mip_count"),    Tex->GetNumMips());
	Out->SetBoolField  (TEXT("srgb"),         Tex->SRGB != 0);
	Out->SetStringField(TEXT("lod_group"),    TEX_LODGroupToString(Tex->LODGroup));
	Out->SetNumberField(TEXT("lod_bias"),     Tex->LODBias);
	Out->SetBoolField  (TEXT("never_stream"), Tex->NeverStream != 0);
	Out->SetStringField(TEXT("address_x"),    TEX_AddressToString(Tex->GetTextureAddressX()));
	Out->SetStringField(TEXT("address_y"),    TEX_AddressToString(Tex->GetTextureAddressY()));

	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── texture.set_compression ──────────────────────────────────────────────────────────────────
//
// Args:    { texture_path: string, compression_settings: string, update_resource?: bool (default true) }
// Result:  { asset_path, prior_compression, new_compression, update_resource_called }
//
// PIE-guarded write — refuses during PIE with -32027. FScopedTransaction wraps the mutation,
// MarkPackageDirty fires after. update_resource (default true) triggers Texture->UpdateResource()
// so subsequent reads see the new encoding immediately; callers performing batch edits can pass
// false to defer (must call texture.set_compression again with update_resource=true on the LAST
// write or call asset.save_asset to commit).
FMCPResponse Tool_SetCompression(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_Texture_SetCompression", "Set Texture Compression"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString TexPath, CompStr;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("texture_path"),         TexPath, Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("compression_settings"), CompStr, Err)) { return Err; }

	const TextureCompressionSettings NewComp = TEX_ParseCompression(CompStr);
	if (NewComp == TC_MAX)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			FString::Printf(TEXT("compression_settings '%s' unrecognised; valid: TC_Default, "
				"TC_Normalmap, TC_Masks, TC_Grayscale, TC_HDR, TC_Alpha, TC_BC7, TC_HalfFloat, "
				"TC_LQ, TC_VectorDisplacementmap, TC_DistanceFieldFont"), *CompStr));
	}

	bool bUpdateResource = true;
	if (Request.Args.IsValid()) { Request.Args->TryGetBoolField(TEXT("update_resource"), bUpdateResource); }

	int32 LoadErrCode = 0;
	FString LoadErrMsg;
	UTexture2D* Tex = FMCPAssetLoader::Load<UTexture2D>(TexPath, LoadErrCode, LoadErrMsg);
	if (!Tex) { return FMCPToolHelpers::MakeError(Request, LoadErrCode, LoadErrMsg); }

	const TextureCompressionSettings PriorComp = Tex->CompressionSettings;

	Tex->Modify();
	Tex->CompressionSettings = NewComp;
	if (bUpdateResource)
	{
		// PreEditChange/PostEditChange would be the textbook path but UpdateResource() alone is
		// sufficient for compression-setting changes — TextureFactory/EditorFactories.cpp uses the
		// same direct-assignment + UpdateResource pattern for batch re-compression. Pre/PostEdit
		// triggers full DDC rebuild + reimport-source-changed delegate which we don't need.
		Tex->UpdateResource();
	}

	Scope.DirtyPackage(Tex->GetOutermost());

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("asset_path"),            Tex->GetPathName());
	Out->SetStringField(TEXT("prior_compression"),     TEX_CompressionToString(PriorComp));
	Out->SetStringField(TEXT("new_compression"),       TEX_CompressionToString(NewComp));
	Out->SetBoolField  (TEXT("update_resource_called"), bUpdateResource);
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── texture.generate_solid_color ─────────────────────────────────────────────────────────────
//
// Args:    { dest_path: string, color: [r, g, b, a] (0..1 floats), size?: int (default 4, valid 1..1024),
//            save?: bool }
// Result:  { created, asset_path, saved, size }
//
// Procedurally generates a solid-color UTexture2D asset. Uses the manual NewObject + Source.Init
// pattern (NOT FImageUtils::CreateTexture2D — that path leaves Source data empty which makes the
// asset un-rebuildable; Source.Init stamps real BGRA8 source bytes so the texture survives a DDC
// wipe and re-import). PIE-guarded. PathInUse check covers BOTH on-disk persistence AND in-memory
// transient objects so a freshly-generated-but-unsaved texture blocks subsequent overwrites of
// the same dest_path.
//
// Color floats are clamped to [0,1] then converted to 8-bit BGRA (matching TSF_BGRA8 layout —
// blue/green/red/alpha byte order in source bytes, NOT FColor's logical RGBA layout). Size must
// be in [1, 1024]; out-of-range yields -32602. SRGB defaults to true so designers can pick
// gamma-space colors that match the editor color-picker (TC_Default also gates sRGB to true so
// flipping later via texture.set_compression doesn't invalidate the source data).
FMCPResponse Tool_GenerateSolidColor(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_Texture_GenerateSolid", "Generate Solid-Color Texture"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString DestRaw;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("dest_path"), DestRaw, Err)) { return Err; }

	const TArray<TSharedPtr<FJsonValue>>* ColorArr = nullptr;
	if (!Request.Args.IsValid() ||
	    !Request.Args->TryGetArrayField(TEXT("color"), ColorArr) || !ColorArr || ColorArr->Num() != 4)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			TEXT("texture.generate_solid_color requires args.color = [r, g, b, a] (4 floats in [0,1])"));
	}
	auto ClampFloat = [](double V) -> uint8
	{
		const double Clamped = FMath::Clamp(V, 0.0, 1.0);
		return static_cast<uint8>(FMath::RoundToInt(Clamped * 255.0));
	};
	const uint8 R = ClampFloat((*ColorArr)[0]->AsNumber());
	const uint8 G = ClampFloat((*ColorArr)[1]->AsNumber());
	const uint8 B = ClampFloat((*ColorArr)[2]->AsNumber());
	const uint8 A = ClampFloat((*ColorArr)[3]->AsNumber());

	int32 Size = 4;
	if (Request.Args.IsValid()) { Request.Args->TryGetNumberField(TEXT("size"), Size); }
	if (Size < 1 || Size > 1024)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			FString::Printf(TEXT("size %d out of range; must be in [1, 1024]"), Size));
	}

	const FString DestNorm = FMCPAssetPathUtils::Normalize(DestRaw);
	if (DestNorm.IsEmpty() || !FMCPAssetPathUtils::IsValidGameOrPlugin(DestNorm))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidPath,
			FString::Printf(TEXT("dest_path '%s' malformed or unknown mount"), *DestRaw));
	}
	const FString PackagePath = FPaths::GetPath(DestNorm);
	const FString AssetName   = FPaths::GetBaseFilename(DestNorm);

	// PathInUse: cover both on-disk and in-memory (same lesson as sequencer.create_sequence /
	// mesh.duplicate — a freshly-created-but-unsaved asset reads as "not present" on disk and a
	// double-create silently overwrites the prior UObject otherwise).
	if (FPackageName::DoesPackageExist(DestNorm) ||
	    FindObject<UObject>(nullptr, *(DestNorm + TEXT(".") + AssetName)) != nullptr)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPathInUse,
			FString::Printf(TEXT("dest_path '%s' already exists (on disk or in-memory)"), *DestNorm));
	}

	const FString PackageName = PackagePath + TEXT("/") + AssetName;
	UPackage* DestPkg = CreatePackage(*PackageName);
	if (!DestPkg)
	{
		return FMCPToolHelpers::MakeError(Request, kTEXErrorInternal,
			FString::Printf(TEXT("CreatePackage returned null for '%s'"), *PackageName));
	}
	DestPkg->FullyLoad();

	FScopedTransaction Transaction(LOCTEXT("MCP_Texture_GenerateSolid", "Generate Solid-Color Texture"));

	UTexture2D* NewTex = NewObject<UTexture2D>(
		DestPkg, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
	if (!NewTex)
	{
		return FMCPToolHelpers::MakeError(Request, kTEXErrorInternal,
			FString::Printf(TEXT("UTexture2D NewObject returned null for '%s'"), *DestNorm));
	}

	// TSF_BGRA8 byte layout — B, G, R, A per pixel. Fill buffer BEFORE Source.Init so we can pass
	// the data pointer in one shot (avoids Lock/Unlock churn for the simple solid-fill case).
	const int32 PixelCount = Size * Size;
	TArray<uint8> Pixels;
	Pixels.SetNumUninitialized(PixelCount * 4);
	for (int32 i = 0; i < PixelCount; ++i)
	{
		Pixels[i * 4 + 0] = B;
		Pixels[i * 4 + 1] = G;
		Pixels[i * 4 + 2] = R;
		Pixels[i * 4 + 3] = A;
	}

	NewTex->Source.Init(Size, Size, /*NumSlices*/ 1, /*NumMips*/ 1, TSF_BGRA8, Pixels.GetData());

	// Default to sRGB true + TC_Default — matches the engine's standard "color texture" preset so
	// the resulting texture renders identically to a designer-imported PNG with the same color.
	NewTex->SRGB = true;
	NewTex->CompressionSettings = TC_Default;
	NewTex->MipGenSettings = TMGS_NoMipmaps;  // 4×4 default — mipmaps are pure overhead at this size.

	NewTex->UpdateResource();

	FAssetRegistryModule::AssetCreated(NewTex);
	DestPkg->MarkPackageDirty();

	bool bSaveRequested = false, bSavedOk = false;
	if (Request.Args.IsValid()) { Request.Args->TryGetBoolField(TEXT("save"), bSaveRequested); }
	if (bSaveRequested)
	{
		if (UEditorAssetSubsystem* EAS = GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr)
		{
			bSavedOk = EAS->SaveLoadedAsset(NewTex, /*bOnlyIfIsDirty*/ true);
		}
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField  (TEXT("created"),    true);
	Out->SetStringField(TEXT("asset_path"), NewTex->GetPathName());
	Out->SetBoolField  (TEXT("saved"),      bSavedOk);
	Out->SetNumberField(TEXT("size"),       Size);
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

	RegisterTool(TEXT("texture.list"),                 &Tool_List,                /*Lane A*/ false);
	RegisterTool(TEXT("texture.get_info"),             &Tool_GetInfo,             /*Lane A*/ false);
	RegisterTool(TEXT("texture.set_compression"),      &Tool_SetCompression,      /*Lane A*/ false);
	RegisterTool(TEXT("texture.generate_solid_color"), &Tool_GenerateSolidColor,  /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("Texture surface registered: 4 texture.* tools "
			 "(list + get_info + set_compression + generate_solid_color), all Lane A"));
}

} // namespace FTextureTools

#undef LOCTEXT_NAMESPACE
