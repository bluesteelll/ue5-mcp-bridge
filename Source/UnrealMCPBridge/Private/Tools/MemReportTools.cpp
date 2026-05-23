// Copyright FatumGame. All Rights Reserved.

#include "MemReportTools.h"

#include "MCPSurfaceRegistry.h"

#include "FMCPDispatchQueue.h"
#include "MCPJsonBuilder.h"
#include "MCPToolHelpers.h"
#include "Utils/MCPPathSandbox.h"

#include "Editor.h"
#include "Engine/Engine.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "Misc/DateTime.h"
#include "Misc/OutputDeviceNull.h"
#include "Misc/Paths.h"
#include "UObject/GarbageCollection.h"
#include "UObject/UObjectArray.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// MR_ prefix per the unity-build symbol-collision convention.
	constexpr int32 kMRErrorInternal       = -32603;
	constexpr int32 kMRErrorInvalidParams  = kMCPErrorInvalidParams;
	constexpr int32 kMRErrorPathEscape     = kMCPErrorPathEscape;
	constexpr int32 kMRErrorOperationFailed = kMCPErrorOperationFailed;

	constexpr double kBytes2Mb = 1.0 / (1024.0 * 1024.0);

	// Wave L+1 (2026-05-23) Lane B promotion: get_quick_stats reads FPlatformMemory statics
	// (documented thread-safe — atomic OS counter reads) and GUObjectArray counter methods. The
	// lock is defense-in-depth — concurrent Lane A calls (e.g. memreport.dump triggering GC)
	// might briefly perturb the live UObject counts mid-iteration. Held for the snapshot only.
	static FCriticalSection gMemReportStatsLock;

	// Snapshot the .memreport files in the MemReport output dir so we can detect
	// which file the engine produces.
	void MR_SnapshotDir(const FString& Dir, TSet<FString>& OutFiles)
	{
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *Dir, TEXT("*.memreport"));
		OutFiles.Reset();
		for (const FString& F : Files)
		{
			OutFiles.Add(F);
		}
	}

	// Pick the editor world (or PIE world if active) for GEngine->Exec routing.
	UWorld* MR_GetExecWorld()
	{
		if (!GEditor) { return nullptr; }
		if (GEditor->PlayWorld) { return GEditor->PlayWorld; }
		return GEditor->GetEditorWorldContext().World();
	}
} // namespace

namespace FMemReportTools
{

// --- memreport.dump ---------------------------------------------------------------------------
//
// Args:    { output_path?: string, full?: bool, timeout_seconds?: int [1, 120] }
// Result:  { path, size_bytes, is_full, elapsed_seconds }
//
// Engine command `MemReport [-FULL] [NAME=...]` writes to
// <ProjectSavedDir>/Profiling/MemReports/<auto-name>.memreport. The command is
// deferred (added to DeferredCommands inside the engine) — runs at end-of-tick,
// then synchronously flushes the file.
//
// We watch the dir for a new file by snapshotting before + diffing every 100ms
// until a new file appears or timeout_seconds elapses. If client requested
// output_path, the file is moved post-detection (sandbox-validated path).
FMCPResponse Tool_Dump(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!GEngine)
	{
		return FMCPToolHelpers::MakeError(Request, kMRErrorInternal, TEXT("GEngine is null"));
	}

	bool bFull = false;
	int32 TimeoutSeconds = 30;
	if (Request.Args.IsValid())
	{
		Request.Args->TryGetBoolField(TEXT("full"), bFull);
		int32 ParsedTimeout = 30;
		if (Request.Args->TryGetNumberField(TEXT("timeout_seconds"), ParsedTimeout))
		{
			if (ParsedTimeout < 1 || ParsedTimeout > 120)
			{
				return FMCPToolHelpers::MakeError(Request, kMRErrorInvalidParams,
					FString::Printf(TEXT("timeout_seconds must be in [1, 120] (got %d)"), ParsedTimeout));
			}
			TimeoutSeconds = ParsedTimeout;
		}
	}

	// Output_path validation (early — fail before triggering expensive engine work).
	FString DesiredFinalPath;
	if (Request.Args.IsValid())
	{
		FString RawPath;
		if (Request.Args->TryGetStringField(TEXT("output_path"), RawPath) && !RawPath.IsEmpty())
		{
			FString SandboxErr;
			if (!FMCPPathSandbox::Resolve(RawPath, DesiredFinalPath, SandboxErr))
			{
				return FMCPToolHelpers::MakeError(Request, kMRErrorPathEscape,
					FString::Printf(TEXT("output_path '%s' rejected: %s"), *RawPath, *SandboxErr));
			}
		}
	}

	// 1. Resolve the engine's MemReports dir + snapshot existing files.
	const FString MemReportsDir = FPaths::ProjectSavedDir() / TEXT("Profiling/MemReports/");
	IFileManager::Get().MakeDirectory(*MemReportsDir, /*Tree=*/true);

	TSet<FString> PreFiles;
	MR_SnapshotDir(MemReportsDir, PreFiles);

	// 2. Trigger MemReport. The `MemReport` command defers to MemReportDeferred
	// inside the engine which runs at end-of-tick, flushes asyncs, GCs, then
	// writes the file.
	const FString MemCmd = bFull ? TEXT("MemReport -FULL") : TEXT("MemReport");
	UWorld* World = MR_GetExecWorld();
	FOutputDeviceNull NullDev;
	const bool bExecOk = GEngine->Exec(World, *MemCmd, NullDev);
	if (!bExecOk)
	{
		return FMCPToolHelpers::MakeError(Request, kMRErrorOperationFailed,
			FString::Printf(TEXT("GEngine->Exec('%s') returned false"), *MemCmd));
	}

	// 3. Poll the directory for the new file. MemReport is deferred so the file
	// won't appear on this tick; we sleep + re-scan. Game thread blocks for up
	// to timeout_seconds, then surfaces a clear error if nothing appeared.
	const double StartTime = FPlatformTime::Seconds();
	const double Deadline  = StartTime + static_cast<double>(TimeoutSeconds);
	FString NewFileName;
	while (FPlatformTime::Seconds() < Deadline)
	{
		FPlatformProcess::Sleep(0.1f);

		TArray<FString> FilesNow;
		IFileManager::Get().FindFiles(FilesNow, *MemReportsDir, TEXT("*.memreport"));

		// Pick the LATEST new file by alphabetical sort (engine names include a
		// timestamp so lexicographic ordering tracks creation time).
		FString CandidateLatest;
		for (const FString& F : FilesNow)
		{
			if (PreFiles.Contains(F)) { continue; }
			if (F > CandidateLatest)
			{
				CandidateLatest = F;
			}
		}
		if (!CandidateLatest.IsEmpty())
		{
			NewFileName = CandidateLatest;
			break;
		}
	}

	const double Elapsed = FPlatformTime::Seconds() - StartTime;

	if (NewFileName.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kMRErrorOperationFailed,
			FString::Printf(
				TEXT("MemReport file did not appear within %d seconds (engine busy or IO hang)"),
				TimeoutSeconds));
	}

	FString FinalPath = MemReportsDir / NewFileName;

	// 4. If client requested an alternate path, move the file there.
	if (!DesiredFinalPath.IsEmpty())
	{
		// Ensure the destination directory exists.
		const FString DestDir = FPaths::GetPath(DesiredFinalPath);
		if (!DestDir.IsEmpty())
		{
			IFileManager::Get().MakeDirectory(*DestDir, /*Tree=*/true);
		}
		if (IFileManager::Get().Move(*DesiredFinalPath, *FinalPath, /*bReplace=*/true))
		{
			FinalPath = DesiredFinalPath;
		}
		// On move failure: silently fall back to the original engine path —
		// caller still gets a valid file path in the response.
	}

	int64 Size = IFileManager::Get().FileSize(*FinalPath);
	if (Size == INDEX_NONE) { Size = 0; }

	return FMCPJsonBuilder()
		.Str(TEXT("path"), FinalPath)
		.Int(TEXT("size_bytes"), Size)
		.Bool(TEXT("is_full"), bFull)
		.Num(TEXT("elapsed_seconds"), Elapsed)
		.BuildSuccess(Request);
}

// --- memreport.get_quick_stats ----------------------------------------------------------------
//
// Args:    {}
// Result:  { used_physical_mb, available_physical_mb, peak_used_physical_mb,
//            total_physical_mb, used_virtual_mb, available_virtual_mb,
//            peak_used_virtual_mb, page_size_bytes,
//            live_uobject_slots, total_uobject_slots, seconds_since_last_gc }
//
// Lightweight memory snapshot — safe for high-frequency polling. FPlatformMemory
// queries are documented thread-safe (atomic OS counter reads).
// `live_uobject_slots` = GUObjectArray.GetObjectArrayNumMinusAvailable() — the
// rename per blueprint R8: this is the slot count minus the available-list
// estimate, NOT a strict "alive after mark" count.
// `seconds_since_last_gc` is clamped >= 0 (negative possible at startup before
// the first GC pass).
// Wave L+1: Lane B — FPlatformMemory::GetStats() is documented thread-safe (atomic OS counter
// reads), GUObjectArray::GetObjectArrayNum* are simple int reads, GetLastGCTime() reads
// GTimingInfo.LastGCTime (double). gMemReportStatsLock added as defense-in-depth.
FMCPResponse Tool_GetQuickStats(const FMCPRequest& Request)
{
	FScopeLock Lock(&gMemReportStatsLock);

	const FPlatformMemoryStats Stats  = FPlatformMemory::GetStats();
	const FPlatformMemoryConstants& C = FPlatformMemory::GetConstants();

	const int32 TotalObjs = GUObjectArray.GetObjectArrayNum();
	const int32 LiveObjs  = GUObjectArray.GetObjectArrayNumMinusAvailable();

	const double LastGCSec   = GetLastGCTime(); // 0 if GC never ran
	const double NowSec      = FApp::GetCurrentTime();
	const double SinceLastGc = (LastGCSec > 0.0)
		? FMath::Max(0.0, NowSec - LastGCSec)
		: 0.0;

	return FMCPJsonBuilder()
		.Num(TEXT("used_physical_mb"),       static_cast<double>(Stats.UsedPhysical)      * kBytes2Mb)
		.Num(TEXT("available_physical_mb"),  static_cast<double>(Stats.AvailablePhysical) * kBytes2Mb)
		.Num(TEXT("peak_used_physical_mb"),  static_cast<double>(Stats.PeakUsedPhysical)  * kBytes2Mb)
		.Num(TEXT("total_physical_mb"),      static_cast<double>(C.TotalPhysical)         * kBytes2Mb)
		.Num(TEXT("used_virtual_mb"),        static_cast<double>(Stats.UsedVirtual)       * kBytes2Mb)
		.Num(TEXT("available_virtual_mb"),   static_cast<double>(Stats.AvailableVirtual)  * kBytes2Mb)
		.Num(TEXT("peak_used_virtual_mb"),   static_cast<double>(Stats.PeakUsedVirtual)   * kBytes2Mb)
		.Int(TEXT("page_size_bytes"),        static_cast<int64>(C.PageSize))
		.Int(TEXT("live_uobject_slots"),     LiveObjs)
		.Int(TEXT("total_uobject_slots"),    TotalObjs)
		.Num(TEXT("seconds_since_last_gc"),  SinceLastGc)
		.BuildSuccess(Request);
}

void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	// Wave L+1 (2026-05-23): get_quick_stats promoted to Lane B with gMemReportStatsLock for
	// defense-in-depth. FPlatformMemory::GetStats() is documented thread-safe; GUObjectArray
	// counters are simple int reads; GetLastGCTime() is a double read. dump stays Lane A — blocks
	// the calling thread on FPlatformProcess::Sleep and invokes GEngine->Exec for "MemReport".
	RegisterTool(TEXT("memreport.dump"),            &Tool_Dump,          /*Lane A*/ false);
	RegisterTool(TEXT("memreport.get_quick_stats"), &Tool_GetQuickStats, /*Lane B*/ true);

	UE_LOG(LogMCP, Log,
		TEXT("MemReport surface registered: memreport.{dump, get_quick_stats} "
			 "(1 Lane A + 1 Lane B)"));
}

} // namespace FMemReportTools

MCP_REGISTER_SURFACE(MemReportTools, &FMemReportTools::Register)

#undef LOCTEXT_NAMESPACE
