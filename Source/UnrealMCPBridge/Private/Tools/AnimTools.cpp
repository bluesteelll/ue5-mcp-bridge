// Copyright FatumGame. All Rights Reserved.

#include "AnimTools.h"

#include "MCPSurfaceRegistry.h"

#include "FMCPDispatchQueue.h"
#include "MCPAssetLoader.h"
#include "MCPJsonBuilder.h"
#include "MCPMutatorScope.h"
#include "MCPToolHelpers.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPAssetPathUtils.h"
#include "Utils/MCPPageCursor.h"

#include "Animation/AnimCompositeBase.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimTypes.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Editor.h"
#include "Factories/AnimMontageFactory.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// ANM_ prefix per the unity-build symbol-collision pattern.
	constexpr int32 kANMErrorInternal      = -32603;
} // namespace

namespace FAnimTools
{

// ─── anim.list_sequences ──────────────────────────────────────────────────────────────────────
//
// Args:    { path_prefix?: string (e.g. "/Game/Characters"), page_size?: int (default 100, clamp [1,1000]),
//            page_token?: string }
// Result:  { sequences: [{ asset_path, name, sequence_length, frame_rate, skeleton_path }],
//            next_page_token?, total_known }
FMCPResponse Tool_ListSequences(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString PathPrefix;
	if (Request.Args.IsValid()) { Request.Args->TryGetStringField(TEXT("path_prefix"), PathPrefix); }

	int32 PageSize = 100;
	if (Request.Args.IsValid()) { Request.Args->TryGetNumberField(TEXT("page_size"), PageSize); }
	PageSize = FMath::Clamp(PageSize, 1, 1000);

	FString PageToken;
	if (Request.Args.IsValid()) { Request.Args->TryGetStringField(TEXT("page_token"), PageToken); }

	// Build FilterHash so cursor staleness is detectable.
	const uint32 FilterHash = GetTypeHash(PathPrefix);

	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FARFilter Filter;
	Filter.ClassPaths.Add(UAnimSequence::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = false;
	Filter.bRecursivePaths   = true;
	if (!PathPrefix.IsEmpty())
	{
		Filter.PackagePaths.Add(*PathPrefix);
	}
	TArray<FAssetData> Assets;
	AR.GetAssets(Filter, Assets);

	// Stable sort by ObjectPath.
	Assets.Sort([](const FAssetData& A, const FAssetData& B)
	{
		return A.GetSoftObjectPath().ToString() < B.GetSoftObjectPath().ToString();
	});

	// Decode cursor.
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
		// Skip past LastAssetPath.
		while (StartIdx < Assets.Num() &&
		       Assets[StartIdx].GetSoftObjectPath().ToString() <= InCursor.LastAssetPath)
		{
			++StartIdx;
		}
	}

	TArray<TSharedPtr<FJsonValue>> SeqArr;
	const int32 EndIdx = FMath::Min(StartIdx + PageSize, Assets.Num());
	for (int32 i = StartIdx; i < EndIdx; ++i)
	{
		const FAssetData& A = Assets[i];
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("asset_path"), A.GetSoftObjectPath().ToString());
		Obj->SetStringField(TEXT("name"), A.AssetName.ToString());

		// Load to fetch length + frame rate + skeleton.  Light-touch — single-asset LoadObject is fast.
		UAnimSequence* Seq = Cast<UAnimSequence>(A.GetAsset());
		if (Seq)
		{
			Obj->SetNumberField(TEXT("sequence_length"), Seq->GetPlayLength());
			Obj->SetNumberField(TEXT("frame_rate_decimal"), Seq->GetSamplingFrameRate().AsDecimal());
			if (USkeleton* Sk = Seq->GetSkeleton())
			{
				Obj->SetStringField(TEXT("skeleton_path"), Sk->GetPathName());
			}
		}
		SeqArr.Add(MakeShared<FJsonValueObject>(Obj));
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetArrayField(TEXT("sequences"), SeqArr);
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

// ─── anim.create_montage ──────────────────────────────────────────────────────────────────────
//
// Args:    { dest_path: string, source_sequence_path: string, target_skeleton?: string, save?: bool }
// Result:  { created, asset_path, saved, skeleton_path, source_length }
//
// Replicates UAnimMontageFactory::FactoryCreateNew inline:
//   1. NewObject<UAnimMontage>
//   2. SetSkeleton (from source sequence)
//   3. AddSlot("DefaultSlot") + push a FAnimSegment for the source
//   4. EnsureStartingSection (FCompositeSection "Default" at t=0)
//
// Errors: standard + -32054 SkeletonMismatch.
FMCPResponse Tool_CreateMontage(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_CreateMontage", "Create Anim Montage"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString DestPathRaw, SourceSeqPath;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("dest_path"),            DestPathRaw,  Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("source_sequence_path"), SourceSeqPath, Err)) { return Err; }

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

	int32 LoadErrCode = 0;
	FString LoadErrMsg;
	UAnimSequence* SourceSeq = FMCPAssetLoader::Load<UAnimSequence>(SourceSeqPath, LoadErrCode, LoadErrMsg);
	if (!SourceSeq) { return FMCPToolHelpers::MakeError(Request, LoadErrCode, LoadErrMsg); }

	USkeleton* Skeleton = SourceSeq->GetSkeleton();
	if (!Skeleton)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorSkeletonMismatch,
			FString::Printf(TEXT("source_sequence '%s' has no skeleton bound"), *SourceSeqPath));
	}

	// Optional target_skeleton constraint check.
	FString TargetSkeletonPath;
	if (Request.Args->TryGetStringField(TEXT("target_skeleton"), TargetSkeletonPath) && !TargetSkeletonPath.IsEmpty())
	{
		USkeleton* TargetSk = LoadObject<USkeleton>(nullptr, *TargetSkeletonPath);
		if (!TargetSk)
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
				FString::Printf(TEXT("target_skeleton '%s' not loadable"), *TargetSkeletonPath));
		}
		if (TargetSk != Skeleton)
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorSkeletonMismatch,
				FString::Printf(TEXT("target_skeleton '%s' does not match source's skeleton '%s'"),
					*TargetSkeletonPath, *Skeleton->GetPathName()));
		}
	}

	const FString PackageName = PackagePath + TEXT("/") + AssetName;
	UPackage* MontagePkg = CreatePackage(*PackageName);
	if (!MontagePkg)
	{
		return FMCPToolHelpers::MakeError(Request, kANMErrorInternal,
			FString::Printf(TEXT("CreatePackage returned null for '%s'"), *PackageName));
	}
	MontagePkg->FullyLoad();

	UAnimMontage* Montage = NewObject<UAnimMontage>(
		MontagePkg, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
	if (!Montage)
	{
		return FMCPToolHelpers::MakeError(Request, kANMErrorInternal,
			FString::Printf(TEXT("NewObject<UAnimMontage> returned null for %s"), *DestPathNorm));
	}

	Montage->SetSkeleton(Skeleton);

	// Build default slot + first segment referencing the source.
	FSlotAnimationTrack& NewSlot = Montage->AddSlot(FAnimSlotGroup::DefaultSlotName);
	FAnimSegment NewSegment;
	NewSegment.SetAnimReference(SourceSeq, /*bInitialize*/ true);
	NewSegment.StartPos      = 0.0f;
	NewSegment.AnimStartTime = 0.0f;
	NewSegment.AnimEndTime   = SourceSeq->GetPlayLength();
	NewSegment.AnimPlayRate  = 1.0f;
	NewSegment.LoopingCount  = 1;
	NewSlot.AnimTrack.AnimSegments.Add(NewSegment);

	// Sync montage length with the source — montage's overall length is the slot's max end position.
	Montage->SetCompositeLength(SourceSeq->GetPlayLength());

	// Add the default starting section (the factory does the same — it's required for the editor
	// to consider the montage usable).
	UAnimMontageFactory::EnsureStartingSection(Montage);

	FAssetRegistryModule::AssetCreated(Montage);
	Scope.DirtyPackage(MontagePkg);

	bool bSaveRequested = false, bSavedOk = false;
	Request.Args->TryGetBoolField(TEXT("save"), bSaveRequested);
	if (bSaveRequested)
	{
		if (UEditorAssetSubsystem* EAS = GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr)
		{
			bSavedOk = EAS->SaveLoadedAsset(Montage, /*bOnlyIfIsDirty*/ true);
		}
	}

	return FMCPJsonBuilder()
		.Bool(TEXT("created"),       true)
		.Str (TEXT("asset_path"),    Montage->GetPathName())
		.Str (TEXT("skeleton_path"), Skeleton->GetPathName())
		.Num (TEXT("source_length"), SourceSeq->GetPlayLength())
		.Bool(TEXT("saved"),         bSavedOk)
		.BuildSuccess(Request);
}

// ─── anim.add_section ─────────────────────────────────────────────────────────────────────────
//
// Args:    { montage_path: string, section_name: string, start_time: number }
// Result:  { added, section_index, section_count }
FMCPResponse Tool_AddSection(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_AddMontageSection", "Add Montage Section"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString MontagePath, SectionName;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("montage_path"), MontagePath, Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("section_name"), SectionName, Err)) { return Err; }

	double StartTime = 0.0;
	if (!Request.Args->TryGetNumberField(TEXT("start_time"), StartTime))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			TEXT("anim.add_section requires args.start_time (seconds, non-negative)"));
	}
	if (StartTime < 0.0)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			FString::Printf(TEXT("start_time %.3f must be >= 0"), StartTime));
	}

	int32 LoadErrCode = 0;
	FString LoadErrMsg;
	UAnimMontage* Montage = FMCPAssetLoader::Load<UAnimMontage>(MontagePath, LoadErrCode, LoadErrMsg);
	if (!Montage) { return FMCPToolHelpers::MakeError(Request, LoadErrCode, LoadErrMsg); }

	Montage->Modify();

	FCompositeSection NewSection;
	NewSection.SectionName = FName(*SectionName);
	// FAnimLinkableElement::Link sets both the absolute time AND the protected LinkValue field.
	NewSection.Link(Montage, static_cast<float>(StartTime));

	Montage->CompositeSections.Add(NewSection);

	Scope.DirtyPackage(Montage->GetOutermost());

	return FMCPJsonBuilder()
		.Bool(TEXT("added"),         true)
		.Int (TEXT("section_index"), Montage->CompositeSections.Num() - 1)
		.Int (TEXT("section_count"), Montage->CompositeSections.Num())
		.BuildSuccess(Request);
}

// ─── anim.add_notify ──────────────────────────────────────────────────────────────────────────
//
// Args:    { montage_path: string, notify_name: string, time: number,
//            duration?: number (>0 = state notify), notify_track_name?: string }
// Result:  { added, notify_index, total_notifies, track_index }
FMCPResponse Tool_AddNotify(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_AddMontageNotify", "Add Montage Notify"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString MontagePath, NotifyName;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("montage_path"), MontagePath, Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("notify_name"), NotifyName, Err)) { return Err; }

	double Time = 0.0;
	if (!Request.Args->TryGetNumberField(TEXT("time"), Time))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			TEXT("anim.add_notify requires args.time (seconds, non-negative)"));
	}
	if (Time < 0.0)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			FString::Printf(TEXT("time %.3f must be >= 0"), Time));
	}

	double Duration = 0.0;
	Request.Args->TryGetNumberField(TEXT("duration"), Duration);

	FString TrackName;
	Request.Args->TryGetStringField(TEXT("notify_track_name"), TrackName);

	int32 LoadErrCode = 0;
	FString LoadErrMsg;
	UAnimMontage* Montage = FMCPAssetLoader::Load<UAnimMontage>(MontagePath, LoadErrCode, LoadErrMsg);
	if (!Montage) { return FMCPToolHelpers::MakeError(Request, LoadErrCode, LoadErrMsg); }

	Montage->Modify();

	// Resolve notify track — default to first track (auto-create "Default" if none exist).
	int32 TrackIndex = 0;
	if (!TrackName.IsEmpty())
	{
		bool bFound = false;
		for (int32 i = 0; i < Montage->AnimNotifyTracks.Num(); ++i)
		{
			if (Montage->AnimNotifyTracks[i].TrackName == FName(*TrackName))
			{
				TrackIndex = i;
				bFound = true;
				break;
			}
		}
		if (!bFound)
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorNotifyTrackNotFound,
				FString::Printf(TEXT("notify_track '%s' not found on montage '%s' (available count: %d)"),
					*TrackName, *MontagePath, Montage->AnimNotifyTracks.Num()));
		}
	}
	else if (Montage->AnimNotifyTracks.Num() == 0)
	{
		FAnimNotifyTrack DefaultTrack;
		DefaultTrack.TrackName = TEXT("Default");
		DefaultTrack.TrackColor = FLinearColor::White;
		Montage->AnimNotifyTracks.Add(DefaultTrack);
	}

	FAnimNotifyEvent NotifyEvent;
	NotifyEvent.NotifyName        = FName(*NotifyName);
	NotifyEvent.TriggerTimeOffset = 0.0f;
	NotifyEvent.EndTriggerTimeOffset = 0.0f;
	NotifyEvent.TrackIndex        = TrackIndex;
	// Link() is the public setter for the protected LinkValue + LinkMethod fields.
	NotifyEvent.Link(Montage, static_cast<float>(Time));
	if (Duration > 0.0)
	{
		NotifyEvent.SetDuration(static_cast<float>(Duration));
	}

	Montage->Notifies.Add(NotifyEvent);
	Montage->RefreshCacheData();

	Scope.DirtyPackage(Montage->GetOutermost());

	return FMCPJsonBuilder()
		.Bool(TEXT("added"),          true)
		.Int (TEXT("notify_index"),   Montage->Notifies.Num() - 1)
		.Int (TEXT("total_notifies"), Montage->Notifies.Num())
		.Int (TEXT("track_index"),    TrackIndex)
		.BuildSuccess(Request);
}

// ─── anim.set_blend_mode ──────────────────────────────────────────────────────────────────────
//
// Args:    { montage_path: string, blend_in_time?: number, blend_out_time?: number }
// Result:  { prior_blend_in, prior_blend_out, new_blend_in, new_blend_out }
FMCPResponse Tool_SetBlendMode(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_SetMontageBlend", "Set Montage Blend"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString MontagePath;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("montage_path"), MontagePath, Err)) { return Err; }

	int32 LoadErrCode = 0;
	FString LoadErrMsg;
	UAnimMontage* Montage = FMCPAssetLoader::Load<UAnimMontage>(MontagePath, LoadErrCode, LoadErrMsg);
	if (!Montage) { return FMCPToolHelpers::MakeError(Request, LoadErrCode, LoadErrMsg); }

	const float PriorIn  = Montage->BlendIn.GetBlendTime();
	const float PriorOut = Montage->BlendOut.GetBlendTime();

	double BlendInTime  = PriorIn;
	double BlendOutTime = PriorOut;
	const bool bHasIn  = Request.Args->TryGetNumberField(TEXT("blend_in_time"),  BlendInTime);
	const bool bHasOut = Request.Args->TryGetNumberField(TEXT("blend_out_time"), BlendOutTime);

	if (!bHasIn && !bHasOut)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			TEXT("anim.set_blend_mode requires at least one of blend_in_time / blend_out_time"));
	}
	if (BlendInTime < 0.0 || BlendOutTime < 0.0)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			TEXT("blend times must be >= 0"));
	}

	Montage->Modify();

	if (bHasIn)  { Montage->BlendIn.SetBlendTime(static_cast<float>(BlendInTime));  }
	if (bHasOut) { Montage->BlendOut.SetBlendTime(static_cast<float>(BlendOutTime)); }

	Scope.DirtyPackage(Montage->GetOutermost());

	return FMCPJsonBuilder()
		.Num(TEXT("prior_blend_in"),  PriorIn)
		.Num(TEXT("prior_blend_out"), PriorOut)
		.Num(TEXT("new_blend_in"),    Montage->BlendIn.GetBlendTime())
		.Num(TEXT("new_blend_out"),   Montage->BlendOut.GetBlendTime())
		.BuildSuccess(Request);
}

// ─── Registration ──────────────────────────────────────────────────────────────────────────────
void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	RegisterTool(TEXT("anim.list_sequences"),  &Tool_ListSequences, /*Lane A*/ false);
	RegisterTool(TEXT("anim.create_montage"),  &Tool_CreateMontage, /*Lane A*/ false);
	RegisterTool(TEXT("anim.add_section"),     &Tool_AddSection,    /*Lane A*/ false);
	RegisterTool(TEXT("anim.add_notify"),      &Tool_AddNotify,     /*Lane A*/ false);
	RegisterTool(TEXT("anim.set_blend_mode"),  &Tool_SetBlendMode,  /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("Animation surface registered: 5 anim.* tools "
			 "(list_sequences + create_montage + add_section + add_notify + set_blend_mode), all Lane A"));
}

} // namespace FAnimTools

#undef LOCTEXT_NAMESPACE

MCP_REGISTER_SURFACE(AnimTools, &FAnimTools::Register)
