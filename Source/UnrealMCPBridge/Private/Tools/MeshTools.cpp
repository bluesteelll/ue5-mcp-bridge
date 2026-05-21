// Copyright FatumGame. All Rights Reserved.

#include "MeshTools.h"

#include "MCPSurfaceRegistry.h"

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
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshSourceData.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PerPlatformProperties.h"
#include "StaticMeshResources.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// MSH_ prefix per unity-build symbol-collision convention.
	constexpr int32 kMSHErrorInternal = -32603;

	/** Convert FVector → 3-element JSON array of doubles. */
	TSharedRef<FJsonValueArray> MSH_Vec3ToJson(const FVector& V)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Reserve(3);
		Arr.Add(MakeShared<FJsonValueNumber>(V.X));
		Arr.Add(MakeShared<FJsonValueNumber>(V.Y));
		Arr.Add(MakeShared<FJsonValueNumber>(V.Z));
		return MakeShared<FJsonValueArray>(Arr);
	}
} // namespace

namespace FMeshTools
{

// ─── mesh.list ────────────────────────────────────────────────────────────────────────────────
//
// Args:    { path_prefix?: string, page_size?: int (default 100, clamp [1,1000]), page_token?: string }
// Result:  { meshes: [{ asset_path, name }], total_known, next_page_token? }
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
	Filter.ClassPaths.Add(UStaticMesh::StaticClass()->GetClassPathName());
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

	TArray<TSharedPtr<FJsonValue>> MeshArr;
	const int32 EndIdx = FMath::Min(StartIdx + PageSize, Assets.Num());
	MeshArr.Reserve(EndIdx - StartIdx);
	for (int32 i = StartIdx; i < EndIdx; ++i)
	{
		const FAssetData& A = Assets[i];
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("asset_path"), A.GetSoftObjectPath().ToString());
		Obj->SetStringField(TEXT("name"), A.AssetName.ToString());
		MeshArr.Add(MakeShared<FJsonValueObject>(Obj));
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetArrayField(TEXT("meshes"), MeshArr);
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

// ─── mesh.get_info ────────────────────────────────────────────────────────────────────────────
//
// Args:    { mesh_path: string }
// Result:  { asset_path, bounds: { origin: [x,y,z], box_extent: [x,y,z], sphere_radius: float },
//            lod_count: int, material_slots: [{ slot_index, slot_name, material_path }],
//            vertex_count: int (LOD 0; 0 if no RenderData), triangle_count: int (LOD 0; 0 if no RenderData),
//            source_model_count: int }
//
// Read-only — no PIE guard, no transaction.
FMCPResponse Tool_GetInfo(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString MeshPath;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("mesh_path"), MeshPath, Err)) { return Err; }

	int32 LoadErrCode = 0;
	FString LoadErrMsg;
	UStaticMesh* Mesh = FMCPAssetLoader::Load<UStaticMesh>(MeshPath, LoadErrCode, LoadErrMsg);
	if (!Mesh) { return FMCPToolHelpers::MakeError(Request, LoadErrCode, LoadErrMsg); }

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("asset_path"), Mesh->GetPathName());

	// Bounds.
	const FBoxSphereBounds Bounds = Mesh->GetBounds();
	TSharedRef<FJsonObject> BoundsObj = MakeShared<FJsonObject>();
	BoundsObj->SetField(TEXT("origin"),     MSH_Vec3ToJson(Bounds.Origin));
	BoundsObj->SetField(TEXT("box_extent"), MSH_Vec3ToJson(Bounds.BoxExtent));
	BoundsObj->SetNumberField(TEXT("sphere_radius"), Bounds.SphereRadius);
	Out->SetObjectField(TEXT("bounds"), BoundsObj);

	// LOD + vertex/triangle counts — null RenderData (placeholder / not-yet-cooked meshes) is
	// non-fatal; emit zeros + empty arrays so callers can still see slots + source-model info.
	int32 LODCount = 0;
	int32 VertexCount = 0;
	int32 TriangleCount = 0;
	if (const FStaticMeshRenderData* RD = Mesh->GetRenderData())
	{
		LODCount = RD->LODResources.Num();
		if (LODCount > 0)
		{
			const FStaticMeshLODResources& LOD0 = RD->LODResources[0];
			VertexCount   = LOD0.GetNumVertices();
			TriangleCount = LOD0.GetNumTriangles();
		}
	}
	Out->SetNumberField(TEXT("lod_count"),       LODCount);
	Out->SetNumberField(TEXT("vertex_count"),    VertexCount);
	Out->SetNumberField(TEXT("triangle_count"),  TriangleCount);

	// Source models — editor-time count, distinct from render LODs.
	Out->SetNumberField(TEXT("source_model_count"), Mesh->GetNumSourceModels());

	// Material slots.
	const TArray<FStaticMaterial>& Mats = Mesh->GetStaticMaterials();
	TArray<TSharedPtr<FJsonValue>> SlotArr;
	SlotArr.Reserve(Mats.Num());
	for (int32 i = 0; i < Mats.Num(); ++i)
	{
		const FStaticMaterial& Slot = Mats[i];
		TSharedRef<FJsonObject> SlotObj = MakeShared<FJsonObject>();
		SlotObj->SetNumberField(TEXT("slot_index"), i);
		SlotObj->SetStringField(TEXT("slot_name"),  Slot.MaterialSlotName.ToString());
		// Slot may be unset — emit empty string rather than crashing.
		SlotObj->SetStringField(TEXT("material_path"),
			Slot.MaterialInterface ? Slot.MaterialInterface->GetPathName() : FString());
		SlotArr.Add(MakeShared<FJsonValueObject>(SlotObj));
	}
	Out->SetArrayField(TEXT("material_slots"), SlotArr);

	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── mesh.list_lods ───────────────────────────────────────────────────────────────────────────
//
// Args:    { mesh_path: string }
// Result:  { asset_path, lods: [{ lod_index, vertex_count, triangle_count, screen_size }] }
//
// screen_size pulled from FStaticMeshSourceModel::ScreenSize.Default (PerPlatform float). Indices
// beyond GetNumSourceModels() (rare — happens when render LOD count > source model count after
// auto-LOD-generation strips source data) report screen_size = -1.
FMCPResponse Tool_ListLODs(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString MeshPath;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("mesh_path"), MeshPath, Err)) { return Err; }

	int32 LoadErrCode = 0;
	FString LoadErrMsg;
	UStaticMesh* Mesh = FMCPAssetLoader::Load<UStaticMesh>(MeshPath, LoadErrCode, LoadErrMsg);
	if (!Mesh) { return FMCPToolHelpers::MakeError(Request, LoadErrCode, LoadErrMsg); }

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("asset_path"), Mesh->GetPathName());

	TArray<TSharedPtr<FJsonValue>> LODArr;
	if (const FStaticMeshRenderData* RD = Mesh->GetRenderData())
	{
		const int32 NumLODs = RD->LODResources.Num();
		const int32 NumSourceModels = Mesh->GetNumSourceModels();
		LODArr.Reserve(NumLODs);
		for (int32 i = 0; i < NumLODs; ++i)
		{
			const FStaticMeshLODResources& LOD = RD->LODResources[i];
			TSharedRef<FJsonObject> LODObj = MakeShared<FJsonObject>();
			LODObj->SetNumberField(TEXT("lod_index"),      i);
			LODObj->SetNumberField(TEXT("vertex_count"),   LOD.GetNumVertices());
			LODObj->SetNumberField(TEXT("triangle_count"), LOD.GetNumTriangles());
			const float Screen = (i < NumSourceModels) ? Mesh->GetSourceModel(i).ScreenSize.Default : -1.0f;
			LODObj->SetNumberField(TEXT("screen_size"), Screen);
			LODArr.Add(MakeShared<FJsonValueObject>(LODObj));
		}
	}
	Out->SetArrayField(TEXT("lods"), LODArr);

	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── mesh.set_material_slot ───────────────────────────────────────────────────────────────────
//
// Args:    { mesh_path: string, slot_index: int, material_path: string }
// Result:  { slot_index, prior_material, new_material, slot_name, slot_count }
//
// PIE-guarded write — refuses during PIE with -32027. FScopedTransaction wraps the mutation,
// MarkPackageDirty fires after.
FMCPResponse Tool_SetMaterialSlot(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_Mesh_SetMaterialSlot", "Set StaticMesh Material Slot"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString MeshPath, MaterialPath;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("mesh_path"),     MeshPath,     Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("material_path"), MaterialPath, Err)) { return Err; }

	int32 SlotIndex = -1;
	if (!Request.Args->TryGetNumberField(TEXT("slot_index"), SlotIndex))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			TEXT("mesh.set_material_slot requires args.slot_index (integer, non-negative)"));
	}
	if (SlotIndex < 0)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			FString::Printf(TEXT("slot_index %d must be >= 0"), SlotIndex));
	}

	int32 LoadErrCode = 0;
	FString LoadErrMsg;
	UStaticMesh* Mesh = FMCPAssetLoader::Load<UStaticMesh>(MeshPath, LoadErrCode, LoadErrMsg);
	if (!Mesh) { return FMCPToolHelpers::MakeError(Request, LoadErrCode, LoadErrMsg); }

	const int32 SlotCount = Mesh->GetStaticMaterials().Num();
	if (SlotIndex >= SlotCount)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPropertyIndexOOB,
			FString::Printf(TEXT("slot_index %d out of range; mesh '%s' has %d material slot(s)"),
				SlotIndex, *MeshPath, SlotCount));
	}

	UMaterialInterface* NewMat = FMCPAssetLoader::Load<UMaterialInterface>(MaterialPath, LoadErrCode, LoadErrMsg);
	if (!NewMat) { return FMCPToolHelpers::MakeError(Request, LoadErrCode, LoadErrMsg); }

	// Snapshot prior values for the response BEFORE mutation.
	const FStaticMaterial& PriorSlot = Mesh->GetStaticMaterials()[SlotIndex];
	const FString PriorMaterialPath = PriorSlot.MaterialInterface
		? PriorSlot.MaterialInterface->GetPathName() : FString();
	const FString SlotName = PriorSlot.MaterialSlotName.ToString();

	Mesh->Modify();
	Mesh->SetMaterial(SlotIndex, NewMat);

	Scope.DirtyPackage(Mesh->GetOutermost());

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetNumberField(TEXT("slot_index"),     SlotIndex);
	Out->SetStringField(TEXT("slot_name"),      SlotName);
	Out->SetStringField(TEXT("prior_material"), PriorMaterialPath);
	Out->SetStringField(TEXT("new_material"),   NewMat->GetPathName());
	Out->SetNumberField(TEXT("slot_count"),     SlotCount);
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── mesh.duplicate ───────────────────────────────────────────────────────────────────────────
//
// Args:    { source_mesh_path: string, dest_path: string, save?: bool }
// Result:  { created, asset_path, saved, source_path }
//
// Manual DuplicateObject<UStaticMesh> into a fresh package + AssetRegistry::AssetCreated. DO NOT
// route through IAssetTools::DuplicateAsset — it opens the Static Mesh editor and stalls the game
// thread (same lesson as Wave C creator tools — see SequencerTools::Tool_CreateSequence comment).
// PathInUse check covers BOTH on-disk persistence AND in-memory transient objects (so a freshly-
// duplicated-but-not-saved mesh blocks subsequent duplicates of the same dest_path).
FMCPResponse Tool_Duplicate(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_Mesh_Duplicate", "Duplicate Static Mesh"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString SourceRaw, DestRaw;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("source_mesh_path"), SourceRaw, Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("dest_path"),        DestRaw,   Err)) { return Err; }

	int32 LoadErrCode = 0;
	FString LoadErrMsg;
	UStaticMesh* Source = FMCPAssetLoader::Load<UStaticMesh>(SourceRaw, LoadErrCode, LoadErrMsg);
	if (!Source) { return FMCPToolHelpers::MakeError(Request, LoadErrCode, LoadErrMsg); }

	const FString DestNorm = FMCPAssetPathUtils::Normalize(DestRaw);
	if (DestNorm.IsEmpty() || !FMCPAssetPathUtils::IsValidGameOrPlugin(DestNorm))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidPath,
			FString::Printf(TEXT("dest_path '%s' malformed or unknown mount"), *DestRaw));
	}

	const FString PackagePath = FPaths::GetPath(DestNorm);
	const FString AssetName   = FPaths::GetBaseFilename(DestNorm);

	// PathInUse: cover both on-disk and in-memory (matches sequencer.create_sequence pattern —
	// double-create without this check silently overwrites the prior UObject).
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
		return FMCPToolHelpers::MakeError(Request, kMSHErrorInternal,
			FString::Printf(TEXT("CreatePackage returned null for '%s'"), *PackageName));
	}
	DestPkg->FullyLoad();

	UStaticMesh* NewMesh = DuplicateObject<UStaticMesh>(Source, DestPkg, *AssetName);
	if (!NewMesh)
	{
		return FMCPToolHelpers::MakeError(Request, kMSHErrorInternal,
			FString::Printf(TEXT("DuplicateObject<UStaticMesh> returned null for '%s' -> '%s'"),
				*SourceRaw, *DestNorm));
	}
	// DuplicateObject doesn't propagate the standalone/public flags by default — set them so the
	// new asset persists on save and is GC-rooted in the package.
	NewMesh->SetFlags(RF_Public | RF_Standalone | RF_Transactional);

	FAssetRegistryModule::AssetCreated(NewMesh);
	Scope.DirtyPackage(DestPkg);

	bool bSaveRequested = false, bSavedOk = false;
	Request.Args->TryGetBoolField(TEXT("save"), bSaveRequested);
	if (bSaveRequested)
	{
		if (UEditorAssetSubsystem* EAS = GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr)
		{
			bSavedOk = EAS->SaveLoadedAsset(NewMesh, /*bOnlyIfIsDirty*/ true);
		}
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("created"), true);
	Out->SetStringField(TEXT("asset_path"), NewMesh->GetPathName());
	Out->SetStringField(TEXT("source_path"), Source->GetPathName());
	Out->SetBoolField(TEXT("saved"), bSavedOk);
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

	RegisterTool(TEXT("mesh.list"),               &Tool_List,             /*Lane A*/ false);
	RegisterTool(TEXT("mesh.get_info"),           &Tool_GetInfo,          /*Lane A*/ false);
	RegisterTool(TEXT("mesh.list_lods"),          &Tool_ListLODs,         /*Lane A*/ false);
	RegisterTool(TEXT("mesh.set_material_slot"),  &Tool_SetMaterialSlot,  /*Lane A*/ false);
	RegisterTool(TEXT("mesh.duplicate"),          &Tool_Duplicate,        /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("Mesh surface registered: 5 mesh.* tools "
			 "(list + get_info + list_lods + set_material_slot + duplicate), all Lane A"));
}

} // namespace FMeshTools

#undef LOCTEXT_NAMESPACE

MCP_REGISTER_SURFACE(MeshTools, &FMeshTools::Register)
