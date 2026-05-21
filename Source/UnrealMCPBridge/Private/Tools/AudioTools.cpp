// Copyright FatumGame. All Rights Reserved.

#include "AudioTools.h"

#include "MCPSurfaceRegistry.h"

#include "FMCPDispatchQueue.h"
#include "MCPAssetLoader.h"
#include "MCPMutatorScope.h"
#include "MCPToolHelpers.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPAssetPathUtils.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Editor.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundBase.h"
#include "Sound/SoundClass.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundMix.h"
#include "Sound/SoundNode.h"
#include "Sound/SoundNodeWavePlayer.h"
#include "Sound/SoundWave.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// AUD_ prefix per unity-build convention.
	constexpr int32 kAUDErrorInternal      = -32603;
} // namespace

namespace FAudioTools
{

// ─── audio.create_sound_cue ───────────────────────────────────────────────────────────────────
FMCPResponse Tool_CreateSoundCue(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_CreateSoundCue", "Create Sound Cue"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString DestPathRaw;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("dest_path"), DestPathRaw, Err)) { return Err; }

	const FString DestPathNorm = FMCPAssetPathUtils::Normalize(DestPathRaw);
	if (DestPathNorm.IsEmpty() || !FMCPAssetPathUtils::IsValidGameOrPlugin(DestPathNorm))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidPath,
			FString::Printf(TEXT("dest_path '%s' malformed or unknown mount"), *DestPathRaw));
	}

	const FString PackagePath = FPaths::GetPath(DestPathNorm);
	const FString AssetName   = FPaths::GetBaseFilename(DestPathNorm);

	if (FPackageName::DoesPackageExist(DestPathNorm) ||
	    FindObject<UObject>(nullptr, *(DestPathNorm + TEXT(".") + AssetName)) != nullptr)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPathInUse,
			FString::Printf(TEXT("dest_path '%s' already exists"), *DestPathNorm));
	}

	// Optional initial sound wave.
	USoundWave* SourceWave = nullptr;
	FString SourceWavePath;
	if (Request.Args->TryGetStringField(TEXT("source_wave_path"), SourceWavePath) && !SourceWavePath.IsEmpty())
	{
		int32 LoadErrCode = 0;
		FString LoadErrMsg;
		SourceWave = FMCPAssetLoader::Load<USoundWave>(SourceWavePath, LoadErrCode, LoadErrMsg);
		if (!SourceWave) { return FMCPToolHelpers::MakeError(Request, LoadErrCode, LoadErrMsg); }
	}

	const FString PackageName = PackagePath + TEXT("/") + AssetName;
	UPackage* CuePkg = CreatePackage(*PackageName);
	if (!CuePkg)
	{
		return FMCPToolHelpers::MakeError(Request, kAUDErrorInternal,
			FString::Printf(TEXT("CreatePackage returned null for '%s'"), *PackageName));
	}
	CuePkg->FullyLoad();

	USoundCue* Cue = NewObject<USoundCue>(CuePkg, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
	if (!Cue)
	{
		return FMCPToolHelpers::MakeError(Request, kAUDErrorInternal,
			FString::Printf(TEXT("NewObject<USoundCue> returned null for %s"), *DestPathNorm));
	}

	if (SourceWave)
	{
		// Construct a single USoundNodeWavePlayer pointing at the source wave and wire it as FirstNode.
		USoundNodeWavePlayer* WavePlayer = Cue->ConstructSoundNode<USoundNodeWavePlayer>();
		WavePlayer->SetSoundWave(SourceWave);
		Cue->FirstNode = WavePlayer;
		Cue->LinkGraphNodesFromSoundNodes();
	}

	FAssetRegistryModule::AssetCreated(Cue);
	Scope.DirtyPackage(CuePkg);

	bool bSaveRequested = false, bSavedOk = false;
	Request.Args->TryGetBoolField(TEXT("save"), bSaveRequested);
	if (bSaveRequested)
	{
		if (UEditorAssetSubsystem* EAS = GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr)
		{
			bSavedOk = EAS->SaveLoadedAsset(Cue, /*bOnlyIfIsDirty*/ true);
		}
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("created"), true);
	Out->SetStringField(TEXT("asset_path"), Cue->GetPathName());
	Out->SetBoolField(TEXT("has_source_wave"), SourceWave != nullptr);
	Out->SetBoolField(TEXT("saved"), bSavedOk);
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── audio.set_attenuation ────────────────────────────────────────────────────────────────────
//
// Args:    { sound_path: string, attenuation_path?: string (null/empty = clear) }
// Result:  { prior_attenuation, new_attenuation, sound_class }
FMCPResponse Tool_SetAttenuation(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_SetAttenuation", "Set Sound Attenuation"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString SoundPath;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("sound_path"), SoundPath, Err)) { return Err; }

	int32 LoadErrCode = 0;
	FString LoadErrMsg;
	USoundBase* Sound = FMCPAssetLoader::Load<USoundBase>(SoundPath, LoadErrCode, LoadErrMsg);
	if (!Sound) { return FMCPToolHelpers::MakeError(Request, LoadErrCode, LoadErrMsg); }

	// Optional attenuation_path — null/empty/missing → clear existing.
	FString AttenuationPath;
	USoundAttenuation* Attenuation = nullptr;
	if (Request.Args->TryGetStringField(TEXT("attenuation_path"), AttenuationPath) && !AttenuationPath.IsEmpty())
	{
		Attenuation = FMCPAssetLoader::Load<USoundAttenuation>(AttenuationPath, LoadErrCode, LoadErrMsg);
		if (!Attenuation) { return FMCPToolHelpers::MakeError(Request, LoadErrCode, LoadErrMsg); }
	}

	Sound->Modify();

	const FString PriorPath = Sound->AttenuationSettings
		? Sound->AttenuationSettings->GetPathName()
		: FString();

	Sound->AttenuationSettings = Attenuation;

	Scope.DirtyPackage(Sound->GetOutermost());

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("sound_class"), Sound->GetClass()->GetPathName());
	Out->SetStringField(TEXT("prior_attenuation"), PriorPath);
	Out->SetStringField(TEXT("new_attenuation"),
		Attenuation ? Attenuation->GetPathName() : FString());
	Out->SetBoolField(TEXT("cleared"), Attenuation == nullptr);
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── audio.list_mix_classes ───────────────────────────────────────────────────────────────────
//
// Args:    { path_prefix?: string }
// Result:  { sound_classes: [{ path, name }], sound_mixes: [{ path, name }] }
FMCPResponse Tool_ListMixClasses(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString PathPrefix;
	if (Request.Args.IsValid()) { Request.Args->TryGetStringField(TEXT("path_prefix"), PathPrefix); }

	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	auto QueryClass = [&](UClass* Cls) -> TArray<TSharedPtr<FJsonValue>>
	{
		FARFilter Filter;
		Filter.ClassPaths.Add(Cls->GetClassPathName());
		Filter.bRecursiveClasses = true;
		Filter.bRecursivePaths   = true;
		if (!PathPrefix.IsEmpty()) { Filter.PackagePaths.Add(*PathPrefix); }
		TArray<FAssetData> Assets;
		AR.GetAssets(Filter, Assets);

		Assets.Sort([](const FAssetData& A, const FAssetData& B)
		{
			return A.GetSoftObjectPath().ToString() < B.GetSoftObjectPath().ToString();
		});

		TArray<TSharedPtr<FJsonValue>> Result;
		Result.Reserve(Assets.Num());
		for (const FAssetData& A : Assets)
		{
			TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("path"), A.GetSoftObjectPath().ToString());
			Obj->SetStringField(TEXT("name"), A.AssetName.ToString());
			Obj->SetStringField(TEXT("class"), A.AssetClassPath.ToString());
			Result.Add(MakeShared<FJsonValueObject>(Obj));
		}
		return Result;
	};

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetArrayField(TEXT("sound_classes"), QueryClass(USoundClass::StaticClass()));
	Out->SetArrayField(TEXT("sound_mixes"),   QueryClass(USoundMix::StaticClass()));
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	RegisterTool(TEXT("audio.create_sound_cue"),  &Tool_CreateSoundCue,  /*Lane A*/ false);
	RegisterTool(TEXT("audio.set_attenuation"),   &Tool_SetAttenuation,  /*Lane A*/ false);
	RegisterTool(TEXT("audio.list_mix_classes"),  &Tool_ListMixClasses,  /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("Audio surface registered: 3 audio.* tools (create_sound_cue + set_attenuation + list_mix_classes), all Lane A"));
}

} // namespace FAudioTools

#undef LOCTEXT_NAMESPACE

MCP_REGISTER_SURFACE(AudioTools, &FAudioTools::Register)
