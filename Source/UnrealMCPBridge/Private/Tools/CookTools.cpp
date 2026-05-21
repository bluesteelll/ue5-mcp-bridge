// Copyright FatumGame. All Rights Reserved.

#include "CookTools.h"

#include "MCPSurfaceRegistry.h"

#include "FMCPDispatchQueue.h"
#include "FMCPJobRegistry.h"
#include "MCPJsonBuilder.h"
#include "MCPToolHelpers.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPAssetPathUtils.h"
#include "Utils/MCPPathSandbox.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/ITargetPlatform.h"
#include "Interfaces/ITargetPlatformManagerModule.h"
#include "Internationalization/Text.h"
#include "Misc/MonitoredProcess.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Modules/ModuleManager.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#include <atomic>

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// COOK_ prefix per the unity-build symbol-collision convention. The four shared helpers
	// (StampIds / MakeError / MakeSuccessObj / RequireStringField) live in FMCPToolHelpers — see
	// Phase 1 helper extraction (commit b2fd19d). All cook.* submitters are Lane A; cook.start's
	// async body runs on a worker thread but never touches FMCPMutatorScope (no GEditor / no PIE-
	// guard semantics for a separate-process cook subprocess).
	constexpr int32 kCOOKErrorInvalidParams = -32602;
	constexpr int32 kCOOKErrorInternal      = -32603;

	// validate_cookable hard cap — protects against accidental /Game-recursive walks on huge
	// projects. Default 5000 per the surface brief; absolute ceiling 100k matches data_validation's
	// upper bound (cook is even heavier per asset but the per-asset cost here is just flag-read).
	constexpr int32 kCOOKDefaultMaxAssets   = 5000;
	constexpr int32 kCOOKMaxAssetsHardCap   = 100000;

	// Bounded output ring for cook subprocess stdout/stderr capture. Cook can emit many MiB of
	// progress logs over hours; we keep only the LAST 1 MiB to surface in job result without
	// blowing the worker's memory.
	constexpr int32 kCOOKMaxOutputBytes     = 1 * 1024 * 1024;

	// Poll cadence inside the cook job body. Cook is a long-running process — we don't need
	// sub-second responsiveness for IsRunning/cancel polling. 500 ms balances cancel latency
	// against CPU overhead from the wakeup loop.
	constexpr double kCOOKPollIntervalSecs  = 0.5;

	/**
	 * Resolve the TargetPlatformManager module. Returns nullptr + populates OutError when the
	 * module isn't loadable (e.g. -nullrhi commandlet contexts that strip target platform support).
	 */
	ITargetPlatformManagerModule* COOK_GetTPM(FString& OutError)
	{
		ITargetPlatformManagerModule* TPM =
			FModuleManager::LoadModulePtr<ITargetPlatformManagerModule>(TEXT("TargetPlatform"));
		if (!TPM)
		{
			OutError = TEXT("TargetPlatform module is not loadable in this build configuration "
				"(commandlet / -nullrhi / cooker context may strip target platform support)");
			return nullptr;
		}
		return TPM;
	}

	/**
	 * Resolve the engine's UnrealEditor-Cmd.exe path. Returns empty string when the engine binary
	 * cannot be located (shouldn't happen in normal editor contexts but we surface a friendly
	 * error rather than crashing the subprocess launch).
	 *
	 * Platform path:
	 *   Win64    : <EngineDir>/Binaries/Win64/UnrealEditor-Cmd.exe
	 *   Mac      : <EngineDir>/Binaries/Mac/UnrealEditor-Cmd.app/Contents/MacOS/UnrealEditor-Cmd
	 *   Linux    : <EngineDir>/Binaries/Linux/UnrealEditor-Cmd
	 *
	 * Wave H S6 ships Win64-only resolution since the bridge is editor-only and our development
	 * platform is Windows. Cross-platform sniff added behind PLATFORM_* guards for future-proofing.
	 */
	FString COOK_GetEditorCmdPath()
	{
		const FString EngineDir = FPaths::EngineDir();
#if PLATFORM_WINDOWS
		const FString ExePath = FPaths::Combine(EngineDir, TEXT("Binaries/Win64/UnrealEditor-Cmd.exe"));
#elif PLATFORM_MAC
		const FString ExePath = FPaths::Combine(EngineDir,
			TEXT("Binaries/Mac/UnrealEditor-Cmd.app/Contents/MacOS/UnrealEditor-Cmd"));
#elif PLATFORM_LINUX
		const FString ExePath = FPaths::Combine(EngineDir, TEXT("Binaries/Linux/UnrealEditor-Cmd"));
#else
		const FString ExePath;
#endif
		return ExePath;
	}
} // namespace

namespace FCookTools
{

// ─── cook.list_platforms ──────────────────────────────────────────────────────────────────────
//
// Args:    (none)
// Result:  { platforms: [{ name, display_name, is_server, is_client, is_editor }],
//            total: int, active_count: int }
//
// Read-only — no PIE guard. Enumerates EVERY registered target platform (ITargetPlatformManager::
// GetTargetPlatforms()), not just the cooker-active subset. ``active_count`` reports how many of
// those are returned by GetActiveTargetPlatforms() (i.e. would be cooked by a default invocation
// without explicit -TargetPlatform). Per-entry fields:
//   - name           : ITargetPlatform::PlatformName() — canonical wire identifier passed to
//                       UnrealEditor-Cmd.exe -TargetPlatform=<X>.
//   - display_name   : ITargetPlatform::DisplayName().ToString() — UX-friendly label.
//   - is_server      : ITargetPlatform::IsServerOnly() — server-only build (no AV data).
//   - is_client      : ITargetPlatform::IsClientOnly() — client-only build (no server logic).
//   - is_editor      : ITargetPlatform::HasEditorOnlyData() — retains editor-only data (Editor
//                       targets only; cooked target platforms always return false).
//
// Lane A — TPM access is conventionally game-thread in this codebase (FModuleManager::LoadModulePtr
// is thread-safe but TPM internal state has no documented threading contract).
FMCPResponse Tool_CookListPlatforms(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString TPMErr;
	ITargetPlatformManagerModule* TPM = COOK_GetTPM(TPMErr);
	if (!TPM)
	{
		return FMCPToolHelpers::MakeError(Request, kCOOKErrorInternal, TPMErr);
	}

	const TArray<ITargetPlatform*>& All    = TPM->GetTargetPlatforms();
	const TArray<ITargetPlatform*>& Active = TPM->GetActiveTargetPlatforms();

	// Build a set of pointer identities for the active subset so we can O(1) test membership.
	TSet<const ITargetPlatform*> ActiveSet;
	ActiveSet.Reserve(Active.Num());
	for (const ITargetPlatform* P : Active) { ActiveSet.Add(P); }

	TArray<TSharedPtr<FJsonValue>> PlatformsArr;
	PlatformsArr.Reserve(All.Num());
	for (const ITargetPlatform* Platform : All)
	{
		if (!Platform) { continue; }

		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"),         Platform->PlatformName());
		Obj->SetStringField(TEXT("display_name"), Platform->DisplayName().ToString());
		Obj->SetBoolField  (TEXT("is_server"),    Platform->IsServerOnly());
		Obj->SetBoolField  (TEXT("is_client"),    Platform->IsClientOnly());
		Obj->SetBoolField  (TEXT("is_editor"),    Platform->HasEditorOnlyData());
		Obj->SetBoolField  (TEXT("is_active"),    ActiveSet.Contains(Platform));
		PlatformsArr.Add(MakeShared<FJsonValueObject>(Obj));
	}

	// Stable sort by name so the listing order is deterministic across calls. AI clients can match
	// by string regardless of TPM internal ordering.
	PlatformsArr.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
	{
		const TSharedPtr<FJsonObject>& AObj = A->AsObject();
		const TSharedPtr<FJsonObject>& BObj = B->AsObject();
		FString AName, BName;
		if (AObj.IsValid()) { AObj->TryGetStringField(TEXT("name"), AName); }
		if (BObj.IsValid()) { BObj->TryGetStringField(TEXT("name"), BName); }
		return AName < BName;
	});

	const int32 PlatformCount = PlatformsArr.Num();
	return FMCPJsonBuilder()
		.Arr(TEXT("platforms"), MoveTemp(PlatformsArr))
		.Num(TEXT("total"), PlatformCount)
		.Num(TEXT("active_count"), Active.Num())
		.BuildSuccess(Request);
}

// ─── cook.validate_cookable ───────────────────────────────────────────────────────────────────
//
// Args:    { platform_name: string,
//            asset_paths?: [string]  (default = ['/Game']),
//            max_assets?: int        (default = kCOOKDefaultMaxAssets, hard cap kCOOKMaxAssetsHardCap) }
// Result:  { platform_name: string,
//            cookable_count: int,
//            uncookable_count: int,
//            total_visited: int,
//            max_assets_reached: bool,
//            errors: [{ asset_path, reason }] }
//
// Walks every asset under the supplied asset_paths (or all of /Game when omitted), classifying
// each as cookable or non-cookable for the given target platform. ``errors`` lists ONLY the
// non-cookable entries (cookable ones are summarised by count).
//
// **Cookability heuristic.** We use a CHEAP three-step check (no actual cook attempt):
//   1. UPackage::HasAnyPackageFlags(PKG_EditorOnly) — editor-only packages are always uncookable.
//   2. Target->AllowsEditorObjects()      — if false AND package contains editor-only assets,
//                                            reject. (Most cook targets return false here.)
//   3. Target->IsServerOnly() vs asset class — server-only platforms reject obvious client
//                                               content (UTexture2D / USoundWave) per Epic's
//                                               cook-time policy. Heuristic — caller MUST run an
//                                               actual cook for authoritative classification.
//
// This is intentionally NOT a full ICookOnTheFlyServer / FCookOnTheSide simulation — those are
// hundreds of lines and require driving the cook pipeline. The goal here is a cheap dry-run
// signal that catches the common cases (editor-only assets, server-only mismatches) so callers
// can pre-flight a cook before incurring its multi-minute cost.
//
// Lane A — IAssetRegistry walks + UPackage flag reads are game-thread by codebase convention.
// NO PIE guard.
FMCPResponse Tool_CookValidateCookable(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kCOOKErrorInvalidParams, TEXT("missing args object"));
	}

	FString PlatformName;
	if (!Request.Args->TryGetStringField(TEXT("platform_name"), PlatformName) || PlatformName.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kCOOKErrorInvalidParams,
			TEXT("missing required string field 'platform_name'"));
	}

	int32 MaxAssets = kCOOKDefaultMaxAssets;
	Request.Args->TryGetNumberField(TEXT("max_assets"), MaxAssets);
	if (MaxAssets < 1 || MaxAssets > kCOOKMaxAssetsHardCap)
	{
		return FMCPToolHelpers::MakeError(Request, kCOOKErrorInvalidParams,
			FString::Printf(TEXT("max_assets=%d out of range [1, %d]"),
				MaxAssets, kCOOKMaxAssetsHardCap));
	}

	// Resolve target platform. Wrong name → -32004 ObjectNotFound (TPM treats unknown platforms
	// as if the asset doesn't exist).
	FString TPMErr;
	ITargetPlatformManagerModule* TPM = COOK_GetTPM(TPMErr);
	if (!TPM)
	{
		return FMCPToolHelpers::MakeError(Request, kCOOKErrorInternal, TPMErr);
	}
	ITargetPlatform* Target = TPM->FindTargetPlatform(PlatformName);
	if (!Target)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("target platform '%s' not registered (use cook.list_platforms "
				"to enumerate valid names)"), *PlatformName));
	}

	// Cache target gates once — these never change per-asset within a single call.
	const bool bTargetAllowsEditorObjects = Target->AllowsEditorObjects();
	const bool bTargetIsServerOnly        = Target->IsServerOnly();

	// Build the list of paths to walk. Empty/omitted → ["/Game"] as the default (matches the
	// brief). Each path must be a recognised mount and normalise cleanly.
	const TArray<TSharedPtr<FJsonValue>>* PathsArr = nullptr;
	TArray<FString> PackagePaths;
	if (Request.Args->TryGetArrayField(TEXT("asset_paths"), PathsArr) && PathsArr && PathsArr->Num() > 0)
	{
		PackagePaths.Reserve(PathsArr->Num());
		for (const TSharedPtr<FJsonValue>& V : *PathsArr)
		{
			if (!V.IsValid() || V->Type != EJson::String)
			{
				return FMCPToolHelpers::MakeError(Request, kCOOKErrorInvalidParams,
					TEXT("'asset_paths' entries must be strings"));
			}
			const FString Raw = V->AsString();
			const FString Normalised = FMCPAssetPathUtils::Normalize(Raw);
			if (Normalised.IsEmpty() || !FMCPAssetPathUtils::IsValidGameOrPlugin(Normalised))
			{
				return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidPath,
					FString::Printf(TEXT("path '%s' malformed or unknown mount"), *Raw));
			}
			PackagePaths.Add(Normalised);
		}
	}
	else
	{
		PackagePaths.Add(TEXT("/Game"));
	}

	// Walk the AR for the union of all requested paths. We collect into a single AssetData[]
	// so the max_assets cap is global across paths (not per-path).
	IAssetRegistry& AR =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FARFilter Filter;
	Filter.bRecursivePaths = true;
	for (const FString& P : PackagePaths)
	{
		Filter.PackagePaths.Add(*P);
	}
	TArray<FAssetData> Assets;
	AR.GetAssets(Filter, Assets);

	// Stable sort by SoftObjectPath so the errors[] order is deterministic across runs.
	Assets.Sort([](const FAssetData& A, const FAssetData& B)
	{
		return A.GetSoftObjectPath().ToString() < B.GetSoftObjectPath().ToString();
	});

	int32 CookableCount = 0;
	int32 UncookableCount = 0;
	const bool bMaxReached = Assets.Num() > MaxAssets;
	const int32 EffectiveMax = FMath::Min(Assets.Num(), MaxAssets);

	TArray<TSharedPtr<FJsonValue>> ErrorsArr;

	for (int32 i = 0; i < EffectiveMax; ++i)
	{
		const FAssetData& AD = Assets[i];
		const FString AssetPathStr = AD.GetSoftObjectPath().ToString();

		// Check 1: editor-only package flag. We don't need to load the asset for this — query the
		// AR's cached package flags directly. PKG_EditorOnly is an authoritative reject reason.
		const uint32 PackageFlags = AD.PackageFlags;
		if ((PackageFlags & PKG_EditorOnly) != 0)
		{
			++UncookableCount;
			TSharedRef<FJsonObject> ErrObj = MakeShared<FJsonObject>();
			ErrObj->SetStringField(TEXT("asset_path"), AssetPathStr);
			ErrObj->SetStringField(TEXT("reason"),
				TEXT("package has PKG_EditorOnly flag — editor-only assets are never cooked"));
			ErrorsArr.Add(MakeShared<FJsonValueObject>(ErrObj));
			continue;
		}

		// Check 2: editor-objects gate. Editor classes (Blueprint editor data, EditorOnly toolkits)
		// are filtered out for non-editor targets. We use the asset class as a proxy — full
		// resolution would require LoadObject + IsEditorOnlyClass walk which is too expensive for
		// a dry-run check.
		if (!bTargetAllowsEditorObjects)
		{
			const FName AssetClass = AD.AssetClassPath.GetAssetName();
			// Common editor-only class names that won't cook. Not exhaustive — a real cook
			// has full inheritance + interface info to test against. This is the cheap pass.
			static const FName Class_EditorTutorial(TEXT("EditorTutorial"));
			static const FName Class_EditorUtilityWidget(TEXT("EditorUtilityWidget"));
			static const FName Class_EditorUtilityBlueprint(TEXT("EditorUtilityBlueprint"));
			static const FName Class_AssetEditorSubsystem(TEXT("AssetEditorSubsystem"));
			if (AssetClass == Class_EditorTutorial ||
				AssetClass == Class_EditorUtilityWidget ||
				AssetClass == Class_EditorUtilityBlueprint ||
				AssetClass == Class_AssetEditorSubsystem)
			{
				++UncookableCount;
				TSharedRef<FJsonObject> ErrObj = MakeShared<FJsonObject>();
				ErrObj->SetStringField(TEXT("asset_path"), AssetPathStr);
				ErrObj->SetStringField(TEXT("reason"),
					FString::Printf(TEXT("class '%s' is editor-only and target platform '%s' "
						"does not allow editor objects"),
						*AssetClass.ToString(), *PlatformName));
				ErrorsArr.Add(MakeShared<FJsonValueObject>(ErrObj));
				continue;
			}
		}

		// Check 3: server-only target rejects obvious client-only AV data. Heuristic — cook
		// pipeline does the authoritative work via per-class IsServerOnly. We catch the common
		// case of dropping textures + sounds for a dedicated-server cook.
		if (bTargetIsServerOnly)
		{
			const FName AssetClass = AD.AssetClassPath.GetAssetName();
			static const FName Class_Texture2D(TEXT("Texture2D"));
			static const FName Class_TextureCube(TEXT("TextureCube"));
			static const FName Class_SoundWave(TEXT("SoundWave"));
			static const FName Class_SoundCue(TEXT("SoundCue"));
			static const FName Class_NiagaraSystem(TEXT("NiagaraSystem"));
			if (AssetClass == Class_Texture2D ||
				AssetClass == Class_TextureCube ||
				AssetClass == Class_SoundWave ||
				AssetClass == Class_SoundCue ||
				AssetClass == Class_NiagaraSystem)
			{
				++UncookableCount;
				TSharedRef<FJsonObject> ErrObj = MakeShared<FJsonObject>();
				ErrObj->SetStringField(TEXT("asset_path"), AssetPathStr);
				ErrObj->SetStringField(TEXT("reason"),
					FString::Printf(TEXT("class '%s' is client-side AV data and target platform "
						"'%s' is server-only"), *AssetClass.ToString(), *PlatformName));
				ErrorsArr.Add(MakeShared<FJsonValueObject>(ErrObj));
				continue;
			}
		}

		// Passed all heuristic checks → assumed cookable.
		++CookableCount;
	}

	return FMCPJsonBuilder()
		.Str(TEXT("platform_name"), PlatformName)
		.Num(TEXT("cookable_count"), CookableCount)
		.Num(TEXT("uncookable_count"), UncookableCount)
		.Num(TEXT("total_visited"), EffectiveMax)
		.Num(TEXT("total_known"), Assets.Num())
		.Bool(TEXT("max_assets_reached"), bMaxReached)
		.Arr(TEXT("errors"), MoveTemp(ErrorsArr))
		.BuildSuccess(Request);
}

// ─── cook.start ──────────────────────────────────────────────────────────────────────────────
//
// Args:    { platform_name: string,
//            output_directory: string,
//            additional_args?: string (default "") }
// Sync result: { job_id: string, started_at: float (FPlatformTime::Seconds when submitted) }
// Inner job.result schema:
//   {
//     "return_code":    int,         // 0 = success
//     "duration_secs":  float,
//     "cancelled":      bool,
//     "platform_name":  string,
//     "output_directory": string,
//     "command_line":   string,      // full command for diagnostic
//     "output_tail":    string       // last kCOOKMaxOutputBytes of stdout/stderr
//   }
//
// Submits an FMonitoredProcess wrapping UnrealEditor-Cmd.exe -run=Cook -TargetPlatform=<X>
// -CookOutputDir=<output> -unattended <additional_args>. The cook subprocess is INDEPENDENT of
// the current editor instance — they can coexist (different memory, different working dir under
// the same engine binary). Cooks routinely take minutes-to-hours.
//
// **Lane A submitter, worker-thread body.** The submitter resolves paths + builds the command line
// (game-thread-safe FPaths/FMCPPathSandbox calls), then SubmitJob with bGameThreadRequired=false
// so the body runs entirely on the worker pool. The body launches FMonitoredProcess (which spawns
// its own monitoring thread internally) and polls IsRunning() + Job.bCancelRequested in a 500ms
// loop. NO UObject access in the body — only FMonitoredProcess + FPlatformProcess::Sleep.
//
// **NO PIE guard.** Cook is a separate-process operation; PIE in the running editor doesn't
// affect it.
//
// **Cooperative cancel via Cancel(InKillTree=true).** job.cancel sets Job.bCancelRequested; the
// worker loop calls Process->Cancel(true) which terminates the cooker process tree. Cancel takes
// effect on the next poll tick (≤500ms). Output captured up to that point is included in the
// result.
FMCPResponse Tool_CookStart(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kCOOKErrorInvalidParams, TEXT("missing args object"));
	}

	FString PlatformName;
	if (!Request.Args->TryGetStringField(TEXT("platform_name"), PlatformName) || PlatformName.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kCOOKErrorInvalidParams,
			TEXT("missing required string field 'platform_name'"));
	}

	FString OutputDirRaw;
	if (!Request.Args->TryGetStringField(TEXT("output_directory"), OutputDirRaw) || OutputDirRaw.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kCOOKErrorInvalidParams,
			TEXT("missing required string field 'output_directory'"));
	}

	FString AdditionalArgs;
	Request.Args->TryGetStringField(TEXT("additional_args"), AdditionalArgs);

	// Sandbox-resolve the output directory. Cook outputs are bulky — they go under Saved/Cooked
	// by editor convention, which the sandbox allows. /tmp or arbitrary disk paths are rejected.
	FString OutputDirAbs;
	FString SandboxErr;
	if (!FMCPPathSandbox::Resolve(OutputDirRaw, OutputDirAbs, SandboxErr))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPathEscape, SandboxErr);
	}

	// Validate platform_name BEFORE submitting — we don't want a job_id for an obviously bad input.
	// TPM lookup is cheap; re-checked in the body would be wasted work.
	FString TPMErr;
	ITargetPlatformManagerModule* TPM = COOK_GetTPM(TPMErr);
	if (!TPM)
	{
		return FMCPToolHelpers::MakeError(Request, kCOOKErrorInternal, TPMErr);
	}
	ITargetPlatform* Target = TPM->FindTargetPlatform(PlatformName);
	if (!Target)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("target platform '%s' not registered (use cook.list_platforms "
				"to enumerate valid names)"), *PlatformName));
	}

	// Resolve the engine executable path. Failure here is an environment problem (someone moved
	// the engine binary) — surface as -32603 rather than a job_id.
	const FString EditorExe = COOK_GetEditorCmdPath();
	if (EditorExe.IsEmpty() || !IFileManager::Get().FileExists(*EditorExe))
	{
		return FMCPToolHelpers::MakeError(Request, kCOOKErrorInternal,
			FString::Printf(TEXT("UnrealEditor-Cmd executable not found at expected path '%s'; "
				"cook cannot launch"), *EditorExe));
	}

	// Resolve project file path. Empty means we're running without a project (commandlet) — cook
	// requires a project to know what content to walk.
	const FString ProjectFilePath = FPaths::GetProjectFilePath();
	if (ProjectFilePath.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kCOOKErrorInternal,
			TEXT("FPaths::GetProjectFilePath() is empty — cannot determine .uproject for cook"));
	}

	// Build the cook command line. -unattended suppresses interactive prompts; -nullrhi avoids
	// loading an actual RHI device in the cooker (cook doesn't render). Quoting follows the
	// engine's convention: project file path quoted, paths with spaces quoted via FString::Printf.
	const FString CookCommandLine = FString::Printf(
		TEXT("\"%s\" -run=Cook -TargetPlatform=%s -CookOutputDir=\"%s\" -unattended -nullrhi%s%s"),
		*ProjectFilePath,
		*PlatformName,
		*OutputDirAbs,
		AdditionalArgs.IsEmpty() ? TEXT("") : TEXT(" "),
		*AdditionalArgs);

	const double SubmittedAt = FPlatformTime::Seconds();

	// Capture-by-value into the body lambda so the worker thread owns its own copies. The body
	// does NOT capture Request — it's done with by the time SubmitJob returns.
	const FString EditorExeCap = EditorExe;
	const FString CookCmdCap   = CookCommandLine;
	const FString PlatNameCap  = PlatformName;
	const FString OutDirCap    = OutputDirAbs;

	const FGuid JobIdGuid = FMCPJobRegistry::Get().SubmitJob(
		FString::Printf(TEXT("cook.start: %s -> %s"), *PlatNameCap, *OutDirCap),
		[EditorExeCap, CookCmdCap, PlatNameCap, OutDirCap]
		(FMCPJob& Job) -> TSharedPtr<FJsonValue>
		{
			const double JobStartTime = FPlatformTime::Seconds();

			// Allocate the process. bHidden=true so we don't pop a console window for every cook;
			// bCreatePipes=true so we can capture stdout into our bounded ring.
			TSharedPtr<FMonitoredProcess> Process = MakeShared<FMonitoredProcess>(
				EditorExeCap, CookCmdCap, /*bHidden*/ true, /*bCreatePipes*/ true);

			// Output ring — accumulated into a single FString that we trim to the last
			// kCOOKMaxOutputBytes on every append. FCriticalSection guards both the trim and the
			// final read since OutputDelegate fires from FMonitoredProcess's own monitor thread.
			TSharedRef<FString> OutputRing = MakeShared<FString>();
			TSharedRef<FCriticalSection> OutputLock = MakeShared<FCriticalSection>();

			Process->OnOutput().BindLambda([OutputRing, OutputLock](const FString& Line)
			{
				FScopeLock Lock(&OutputLock.Get());
				OutputRing.Get().Append(Line);
				OutputRing.Get().AppendChar(TEXT('\n'));
				// Trim from the FRONT if we exceed the ring cap. Slack guard so we don't trim
				// every single character — wait until we're ~2x over before slicing.
				if (OutputRing.Get().Len() > kCOOKMaxOutputBytes * 2)
				{
					const int32 Excess = OutputRing.Get().Len() - kCOOKMaxOutputBytes;
					OutputRing.Get().RemoveAt(0, Excess, EAllowShrinking::No);
				}
			});

			// Launch — failure here typically means the exe couldn't be spawned (permissions /
			// engine moved). FMonitoredProcess returns false from Launch() in that case.
			if (!Process->Launch())
			{
				Job.ErrorMessage = FString::Printf(
					TEXT("FMonitoredProcess::Launch failed for command line: %s"),
					*CookCmdCap);
				return nullptr;
			}

			// Poll loop. Cooperative cancel — body checks Job.bCancelRequested every iteration
			// and forwards to Process->Cancel(true) which terminates the cooker process tree.
			bool bCancelled = false;
			while (Process->Update())
			{
				if (Job.bCancelRequested.load(std::memory_order_acquire))
				{
					Process->Cancel(/*InKillTree*/ true);
					bCancelled = true;
					// Wait for the process to fully exit before returning so we can capture the
					// final stdout flush. Bounded wait — 10 seconds — to avoid a permanent hang
					// if Cancel doesn't take effect (Windows orphaned subprocess corner case).
					const double CancelDeadline = FPlatformTime::Seconds() + 10.0;
					while (Process->Update() && FPlatformTime::Seconds() < CancelDeadline)
					{
						FPlatformProcess::Sleep(0.1f);
					}
					break;
				}

				// Progress hint — best-effort. Cook has no internal progress reporting we can
				// query from the parent process; we use wall-clock elapsed as a proxy normalised
				// against a 30-minute pessimistic estimate. UI uses this for spinner cadence.
				const double Elapsed = FPlatformTime::Seconds() - JobStartTime;
				Job.Progress.store(
					FMath::Clamp(static_cast<float>(Elapsed / (30.0 * 60.0)), 0.0f, 0.99f),
					std::memory_order_release);

				FPlatformProcess::Sleep(static_cast<float>(kCOOKPollIntervalSecs));
			}

			const double JobEndTime = FPlatformTime::Seconds();
			const int32 ReturnCode = Process->GetReturnCode();

			Job.Progress.store(1.0f, std::memory_order_release);

			// Honour cancel: even if process exited cleanly during/after the Cancel call, we
			// report cancelled=true so the AI can distinguish operator-initiated termination.
			if (bCancelled)
			{
				Job.ErrorMessage.Reset();
				// FMCPJobRegistry interprets null result + empty ErrorMessage + bCancelRequested
				// as Cancelled. Build a result payload anyway so the caller has the output_tail
				// for diagnostic.
				TSharedRef<FJsonObject> CancelOut = MakeShared<FJsonObject>();
				CancelOut->SetNumberField(TEXT("return_code"), ReturnCode);
				CancelOut->SetNumberField(TEXT("duration_secs"), JobEndTime - JobStartTime);
				CancelOut->SetBoolField(TEXT("cancelled"), true);
				CancelOut->SetStringField(TEXT("platform_name"), PlatNameCap);
				CancelOut->SetStringField(TEXT("output_directory"), OutDirCap);
				CancelOut->SetStringField(TEXT("command_line"), CookCmdCap);
				{
					FScopeLock Lock(&OutputLock.Get());
					CancelOut->SetStringField(TEXT("output_tail"), OutputRing.Get());
				}
				// Return null result to flip the registry to Cancelled — payload preserved in
				// ErrorMessage as a JSON-encoded hint (NOT ideal but matches LiveCoding pattern).
				FString CancelJson;
				const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&CancelJson);
				FJsonSerializer::Serialize(CancelOut, Writer);
				Job.ErrorMessage = FString::Printf(TEXT("cook cancelled by operator; tail=%s"),
					*CancelJson);
				return nullptr;
			}

			// Build the success result. ReturnCode != 0 is a cook FAILURE but the JOB succeeded
			// in the sense that it ran to completion — caller inspects return_code to discriminate.
			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetNumberField(TEXT("return_code"), ReturnCode);
			Out->SetNumberField(TEXT("duration_secs"), JobEndTime - JobStartTime);
			Out->SetBoolField(TEXT("cancelled"), false);
			Out->SetStringField(TEXT("platform_name"), PlatNameCap);
			Out->SetStringField(TEXT("output_directory"), OutDirCap);
			Out->SetStringField(TEXT("command_line"), CookCmdCap);
			{
				FScopeLock Lock(&OutputLock.Get());
				Out->SetStringField(TEXT("output_tail"), OutputRing.Get());
			}
			return MakeShared<FJsonValueObject>(Out);
		},
		/*bGameThreadRequired*/ false);

	if (!JobIdGuid.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorJobSubmitFailed,
			TEXT("FMCPJobRegistry::SubmitJob refused (shutdown?)"));
	}

	return FMCPJsonBuilder()
		.Str(TEXT("job_id"), JobIdGuid.ToString(EGuidFormats::DigitsWithHyphens))
		.Num(TEXT("started_at"), SubmittedAt)
		.Str(TEXT("command_line"), CookCommandLine)
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

	// All Lane A. cook.start submits a worker-thread job (bGameThreadRequired=false) — the
	// submitter itself touches TPM + FPaths + FMCPPathSandbox which are conventionally GT-called
	// in this codebase, hence Lane A for predictable serialization with other tool calls.
	RegisterTool(TEXT("cook.list_platforms"),    &Tool_CookListPlatforms,    /*Lane A*/ false);
	RegisterTool(TEXT("cook.validate_cookable"), &Tool_CookValidateCookable, /*Lane A*/ false);
	RegisterTool(TEXT("cook.start"),             &Tool_CookStart,            /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("Wave H Surface 6: registered 3 cook.* handlers (list_platforms + validate_cookable "
			 "+ start, all Lane A, NO PIE guard; cook.start submits a worker-thread "
			 "FMonitoredProcess job via FMCPJobRegistry)"));
}

} // namespace FCookTools

#undef LOCTEXT_NAMESPACE

MCP_REGISTER_SURFACE(CookTools, &FCookTools::Register)
