// Copyright FatumGame. All Rights Reserved.

#include "SourceControlCompositeTools.h"

#include "FMCPDispatchQueue.h"
#include "FMCPJobRegistry.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPPathSandbox.h"

#include "ISourceControlModule.h"
#include "ISourceControlProvider.h"
#include "ISourceControlState.h"
#include "SourceControlOperations.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Internationalization/Text.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	// SCC_ prefix per the unity-build symbol-collision pattern.
	constexpr int32 kSCCErrorInvalidParams = -32602;
	constexpr int32 kSCCErrorInternal      = -32603;

	void SCC_StampIds(const FMCPRequest& Request, FMCPResponse& Response)
	{
		Response.RequestId = Request.RequestId;
		Response.OriginalIdString = Request.OriginalIdString;
	}

	FMCPResponse SCC_MakeError(const FMCPRequest& Request, int32 Code, const FString& Message)
	{
		FMCPResponse R;
		SCC_StampIds(Request, R);
		R.bIsError = true;
		R.ErrorCode = Code;
		R.ErrorMessage = Message;
		return R;
	}

	FMCPResponse SCC_MakeSuccessObj(const FMCPRequest& Request, TSharedPtr<FJsonObject> Result)
	{
		FMCPResponse R;
		SCC_StampIds(Request, R);
		R.bIsError = false;
		R.Result = MakeShared<FJsonValueObject>(MoveTemp(Result));
		return R;
	}

	/**
	 * Mirror of FSourceControlTools::SCT_ResolveSCPath — duplicated locally to avoid exporting an
	 * internal helper through the Tools header. Resolution rules identical to the sync sc.* tools.
	 * Returns true on success — populates OutAbsPath. Returns false on failure — populates OutError
	 * + OutErrorCode (caller surfaces).
	 */
	bool SCC_ResolveSCPath(const FString& InRawPath, FString& OutAbsPath, FString& OutError,
		int32& OutErrorCode)
	{
		OutAbsPath.Reset();
		OutError.Reset();
		OutErrorCode = 0;

		if (InRawPath.IsEmpty())
		{
			OutError = TEXT("file_path is empty");
			OutErrorCode = kMCPErrorInvalidPath;
			return false;
		}

		if (InRawPath.StartsWith(TEXT("/")) && FPackageName::IsValidLongPackageName(InRawPath))
		{
			FString FilenameOnDisk;
			if (!FPackageName::TryConvertLongPackageNameToFilename(InRawPath, FilenameOnDisk))
			{
				OutError = FString::Printf(
					TEXT("package path '%s' could not be mapped to a disk filename"),
					*InRawPath);
				OutErrorCode = kMCPErrorInvalidPath;
				return false;
			}
			const FString UAsset = FilenameOnDisk + TEXT(".uasset");
			const FString UMap   = FilenameOnDisk + TEXT(".umap");
			if (IFileManager::Get().FileExists(*UAsset))      { FilenameOnDisk = UAsset; }
			else if (IFileManager::Get().FileExists(*UMap))   { FilenameOnDisk = UMap;   }
			else                                              { FilenameOnDisk = UAsset; }

			FString Sandboxed;
			FString SandboxErr;
			if (!FMCPPathSandbox::Resolve(FilenameOnDisk, Sandboxed, SandboxErr))
			{
				OutError = SandboxErr;
				OutErrorCode = kMCPErrorPathEscape;
				return false;
			}
			OutAbsPath = Sandboxed;
			return true;
		}

		FString Sandboxed;
		FString SandboxErr;
		if (!FMCPPathSandbox::Resolve(InRawPath, Sandboxed, SandboxErr))
		{
			OutError = SandboxErr;
			OutErrorCode = kMCPErrorPathEscape;
			return false;
		}
		OutAbsPath = Sandboxed;
		return true;
	}
} // namespace

namespace FSourceControlCompositeTools
{

// ─── sc._submit_internal (Lane B → SubmitJob → game-thread body) ──────────────────────────────
//
// Validates inputs synchronously on the listener thread (path resolution + provider availability
// check), then submits a job that performs the actual ``Provider.Execute(FCheckIn)`` on the game
// thread. AI client receives ``{job_id}`` and polls externally.
//
// Args:
//   - file_paths:  [string]   (required, non-empty) — disk paths or /Game/... package paths
//   - description: string     (required, non-empty) — commit message / changelist description
//
// Inner result (after job.result on Succeeded):
//   {
//     "submitted":    bool,                    // true if FCheckIn returned Succeeded
//     "changelist":   string,                  // changelist number/CL/Git SHA; empty if N/A
//     "conflicts":    [string],                // files in conflict state post-submit (need resolve)
//     "duration_ms":  float,                   // wall-clock time for the FCheckIn RPC
//     "provider":     string,                  // provider name (Git, Perforce, etc.)
//     "error":        string (optional)        // failure message if submitted=false
//   }
//
// Error codes (Lane B sync surface):
//   -32602 InvalidParams                          missing/empty file_paths or description
//   -32010 InvalidPath / -32013 PathEscape        per-path resolution failures
//   -32016 JobSubmitFailed                        registry initialization failed
//   -32045 SourceControlProviderUnavailable       provider not enabled / not available
//
// Provider re-check is INSIDE the body (state can transition between submit + execution).
FMCPResponse Tool_SubmitInternal(const FMCPRequest& Request)
{
	if (!Request.Args.IsValid())
	{
		return SCC_MakeError(Request, kSCCErrorInvalidParams, TEXT("missing args object"));
	}

	// ─── Validate description ──────────────────────────────────────────────────────────────────
	FString Description;
	if (!Request.Args->TryGetStringField(TEXT("description"), Description) || Description.IsEmpty())
	{
		return SCC_MakeError(Request, kSCCErrorInvalidParams,
			TEXT("missing required string field 'description' (commit message)"));
	}

	// ─── Validate + resolve file_paths ─────────────────────────────────────────────────────────
	const TArray<TSharedPtr<FJsonValue>>* PathsArr = nullptr;
	if (!Request.Args->TryGetArrayField(TEXT("file_paths"), PathsArr) || !PathsArr ||
		PathsArr->Num() == 0)
	{
		return SCC_MakeError(Request, kSCCErrorInvalidParams,
			TEXT("missing or empty required array field 'file_paths'"));
	}

	TArray<FString> ResolvedPaths;
	ResolvedPaths.Reserve(PathsArr->Num());
	for (int32 i = 0; i < PathsArr->Num(); ++i)
	{
		FString Raw;
		if (!(*PathsArr)[i].IsValid() || !(*PathsArr)[i]->TryGetString(Raw) || Raw.IsEmpty())
		{
			return SCC_MakeError(Request, kSCCErrorInvalidParams,
				FString::Printf(TEXT("'file_paths'[%d] is not a non-empty string"), i));
		}
		FString AbsPath;
		FString ResolveErr;
		int32 ResolveCode = 0;
		if (!SCC_ResolveSCPath(Raw, AbsPath, ResolveErr, ResolveCode))
		{
			return SCC_MakeError(Request, ResolveCode,
				FString::Printf(TEXT("'file_paths'[%d]='%s': %s"), i, *Raw, *ResolveErr));
		}
		ResolvedPaths.Add(MoveTemp(AbsPath));
	}

	// ─── Submit-time provider availability fail-fast (still re-checked in body) ────────────────
	{
		ISourceControlModule& Module = ISourceControlModule::Get();
		if (!Module.IsEnabled())
		{
			return SCC_MakeError(Request, kMCPErrorSourceControlProviderUnavailable,
				TEXT("source control provider not configured "
					 "(ISourceControlModule::Get().IsEnabled() == false)"));
		}
		if (!Module.GetProvider().IsAvailable())
		{
			return SCC_MakeError(Request, kMCPErrorSourceControlProviderUnavailable,
				FString::Printf(TEXT("source control provider '%s' is enabled but not available"),
					*Module.GetProvider().GetName().ToString()));
		}
	}

	// ─── Submit job ────────────────────────────────────────────────────────────────────────────
	const FGuid JobIdGuid = FMCPJobRegistry::Get().SubmitJob(
		TEXT("sc._submit_internal"),
		[PathsCap = MoveTemp(ResolvedPaths), DescCap = MoveTemp(Description)]
		(FMCPJob& Job) -> TSharedPtr<FJsonValue>
		{
			// Provider re-check — state can transition between submit + execution.
			ISourceControlModule& Module = ISourceControlModule::Get();
			if (!Module.IsEnabled())
			{
				Job.ErrorMessage = TEXT("source control provider not configured "
					"(disabled between submit and execution)");
				return nullptr;
			}
			ISourceControlProvider& Provider = Module.GetProvider();
			if (!Provider.IsAvailable())
			{
				Job.ErrorMessage = FString::Printf(
					TEXT("source control provider '%s' became unavailable between submit and execution"),
					*Provider.GetName().ToString());
				return nullptr;
			}

			Job.Description = FString::Printf(
				TEXT("sc.submit: %d file(s) — '%s'"), PathsCap.Num(), *DescCap);

			const double StartTime = FPlatformTime::Seconds();

			// Build FCheckIn op with description. Provider RPC happens inside Execute.
			const TSharedRef<FCheckIn, ESPMode::ThreadSafe> CheckInOp =
				ISourceControlOperation::Create<FCheckIn>();
			CheckInOp->SetDescription(FText::FromString(DescCap));

			const ECommandResult::Type ExecResult = Provider.Execute(
				CheckInOp, PathsCap, EConcurrency::Synchronous);
			const double DurationMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;

			Job.Progress.store(1.0f, std::memory_order_release);

			// Post-submit state lookup — surface conflicts to caller.
			TArray<FSourceControlStateRef> States;
			Provider.GetState(PathsCap, States, EStateCacheUsage::Use);
			TArray<TSharedPtr<FJsonValue>> ConflictArr;
			for (const FSourceControlStateRef& State : States)
			{
				if (State->IsConflicted())
				{
					ConflictArr.Add(MakeShared<FJsonValueString>(State->GetFilename()));
				}
			}

			// SuccessMessage on FCheckIn typically contains "Submitted CL 12345" or equivalent.
			// We extract changelist by taking the success message text verbatim — caller can parse.
			FString Changelist;
			const FText SuccessMsg = CheckInOp->GetSuccessMessage();
			if (!SuccessMsg.IsEmpty())
			{
				// Best-effort: extract trailing numeric or hash if present, else echo the text.
				Changelist = SuccessMsg.ToString();
			}

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetBoolField(TEXT("submitted"),    ExecResult == ECommandResult::Succeeded);
			Out->SetStringField(TEXT("changelist"), Changelist);
			Out->SetArrayField(TEXT("conflicts"),   ConflictArr);
			Out->SetNumberField(TEXT("duration_ms"), DurationMs);
			Out->SetStringField(TEXT("provider"),   Provider.GetName().ToString());
			if (ExecResult != ECommandResult::Succeeded)
			{
				Out->SetStringField(TEXT("error"),
					FString::Printf(TEXT("FCheckIn returned %s on provider '%s'"),
						ExecResult == ECommandResult::Failed ? TEXT("Failed") : TEXT("Cancelled"),
						*Provider.GetName().ToString()));
			}
			return MakeShared<FJsonValueObject>(Out);
		},
		/*bGameThreadRequired*/ true);

	if (!JobIdGuid.IsValid())
	{
		return SCC_MakeError(Request, kMCPErrorJobSubmitFailed,
			TEXT("FMCPJobRegistry::SubmitJob refused (shutdown?)"));
	}

	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("job_id"), JobIdGuid.ToString(EGuidFormats::DigitsWithHyphens));
	return SCC_MakeSuccessObj(Request, Out);
}

// ─── Registration ────────────────────────────────────────────────────────────────────────────
//
// MUST be Lane B — the Python composite calls this via dispatch_internal (TCP loopback) from
// inside FMCPPythonEval::CallPythonTool on the game thread. Lane A would queue back to the same
// game thread that's blocked on socket.recv() → 60s deadlock until socket timeout. Same rationale
// as the Phase 2-5 internal composite handlers (LevelComposite, BlueprintComposite, etc.).
void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	// Day 3: sc.submit backing internal (async composite).
	RegisterTool(TEXT("sc._submit_internal"), &Tool_SubmitInternal, /*Lane B*/ true);

	UE_LOG(LogMCP, Log,
		TEXT("Phase 6 Chunk A (Source Control composites): registered 1 internal composite handler ")
		TEXT("(sc._submit_internal, Lane B); Python wrapper sc.submit in phase6_composites.py"));
}

} // namespace FSourceControlCompositeTools
