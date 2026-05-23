// Copyright FatumGame. All Rights Reserved.

#include "EngineTools.h"

#include "MCPSurfaceRegistry.h"

#include "FMCPDispatchQueue.h"
#include "MCPJsonBuilder.h"
#include "MCPToolHelpers.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPWorldContext.h"

#include "CoreGlobals.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/EngineTypes.h"
#include "Engine/World.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformTime.h"
#include "HAL/UnrealMemory.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"
#include "Misc/Paths.h"
#include "UObject/GarbageCollection.h"
#include "UObject/UObjectGlobals.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	// ENG_ prefix per the unity-build symbol-collision convention. The four shared helpers
	// (StampIds / MakeError / MakeSuccessObj / RequireXxxField) live in FMCPToolHelpers — see
	// Phase 1 helper extraction (commit b2fd19d).
	constexpr int32 kENGErrorInternal = -32603;

	/** Bytes → megabytes (1024-based) as a double. */
	constexpr double ENG_BytesToMB(uint64 Bytes)
	{
		return static_cast<double>(Bytes) / (1024.0 * 1024.0);
	}

	/**
	 * Resolve "the world to summarise" for engine.get_info — PIE first, editor fallback. Returns
	 * null when GEditor is missing (commandlet context).
	 */
	UWorld* ENG_ResolveCurrentWorld()
	{
		if (FMCPWorldContext::IsPIEActive() && GEditor && GEditor->PlayWorld)
		{
			return GEditor->PlayWorld;
		}
		return FMCPWorldContext::GetEditorWorld();
	}
} // namespace

namespace FEngineTools
{

// ─── engine.get_info ───────────────────────────────────────────────────────────────────────────
//
// Args: {}
//
// Result: {
//   ue_version: { major: int, minor: int, patch: int, changelist: int, branch: string },
//   build_configuration: string,    // "Debug" / "DebugGame" / "Development" / "Shipping" / "Test"
//   target_type: string,            // "Editor" / "Game" / "Server" / "Client" / etc.
//   project_name: string,
//   project_dir: string,            // absolute path
//   engine_dir: string,             // absolute path
//   platform_name: string,          // "Windows" / "Linux" / "Mac" / etc.
//   is_editor: bool,                // GIsEditor
//   is_unattended: bool,            // FApp::IsUnattended
//   is_running_commandlet: bool,    // IsRunningCommandlet()
//   current_world: { name: string, type: string, is_pie: bool } | null
// }
//
// No PIE guard, no transactions, no MarkPackageDirty — pure introspection.
FMCPResponse Tool_GetInfo(const FMCPRequest& Request)
{
	// Lane B safe (Phase 4.2 promote 2026-05-22): FApp::* getters are thread-safe (read from
	// command-line-derived state set at process init). GIsEditor / IsRunningCommandlet are
	// init-time constants. GEngine pointer read is safe (set once on init, never reassigned).

	// === UE engine version ===
	const FEngineVersion& Version = FEngineVersion::Current();
	TSharedRef<FJsonObject> VersionObj = MakeShared<FJsonObject>();
	VersionObj->SetNumberField(TEXT("major"),      static_cast<double>(Version.GetMajor()));
	VersionObj->SetNumberField(TEXT("minor"),      static_cast<double>(Version.GetMinor()));
	VersionObj->SetNumberField(TEXT("patch"),      static_cast<double>(Version.GetPatch()));
	VersionObj->SetNumberField(TEXT("changelist"), static_cast<double>(Version.GetChangelist()));
	VersionObj->SetStringField(TEXT("branch"),     Version.GetBranch());

	// === Build config + target type via Core LexToString — UE 5.7 ships both. ===
	const FString BuildConfig = LexToString(FApp::GetBuildConfiguration());
	const FString TargetType  = LexToString(FApp::GetBuildTargetType());

	// === Project + engine paths via FApp + FPaths ===
	const FString ProjectName = FString(FApp::GetProjectName());
	// FPaths::ProjectDir / EngineDir return relative paths by default; convert to absolute for
	// the API surface so AI callers get unambiguous fully-qualified strings.
	const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	const FString EngineDir  = FPaths::ConvertRelativePathToFull(FPaths::EngineDir());

	// === Platform name — FPlatformProperties returns char*, wrap into FString explicitly. ===
	const FString PlatformName(ANSI_TO_TCHAR(FPlatformProperties::PlatformName()));

	// === Flags ===
	const bool bIsEditor             = GIsEditor;
	const bool bIsUnattended         = FApp::IsUnattended();
	const bool bIsRunningCommandlet  = IsRunningCommandlet();

	// === Current world summary (PIE preferred, editor fallback) ===
	TSharedPtr<FJsonObject> WorldObj;
	if (UWorld* World = ENG_ResolveCurrentWorld())
	{
		WorldObj = MakeShared<FJsonObject>();
		WorldObj->SetStringField(TEXT("name"),   World->GetName());
		WorldObj->SetStringField(TEXT("type"),   LexToString(World->WorldType));
		WorldObj->SetBoolField  (TEXT("is_pie"), World->WorldType == EWorldType::PIE);
	}

	// === Assemble ===
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetObjectField(TEXT("ue_version"),            VersionObj);
	Out->SetStringField(TEXT("build_configuration"),   BuildConfig);
	Out->SetStringField(TEXT("target_type"),           TargetType);
	Out->SetStringField(TEXT("project_name"),          ProjectName);
	Out->SetStringField(TEXT("project_dir"),           ProjectDir);
	Out->SetStringField(TEXT("engine_dir"),            EngineDir);
	Out->SetStringField(TEXT("platform_name"),         PlatformName);
	Out->SetBoolField  (TEXT("is_editor"),             bIsEditor);
	Out->SetBoolField  (TEXT("is_unattended"),         bIsUnattended);
	Out->SetBoolField  (TEXT("is_running_commandlet"), bIsRunningCommandlet);
	if (WorldObj.IsValid())
	{
		Out->SetObjectField(TEXT("current_world"), WorldObj);
	}
	else
	{
		// Explicit null is more informative than a missing field — callers can branch deterministically.
		Out->SetField(TEXT("current_world"), MakeShared<FJsonValueNull>());
	}

	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── engine.gc_collect ─────────────────────────────────────────────────────────────────────────
//
// Args: {
//   force?: bool                       (default true)  — true → ``CollectGarbage(KeepFlags, bPurge)``
//                                                          (synchronous, immediate sweep);
//                                                       false → ``GEngine->ForceGarbageCollection``
//                                                          (deferred — runs on next GC tick).
//   purge_object_references?: bool     (default true)  — bPerformFullPurge argument to CollectGarbage,
//                                                       OR bForcePurge argument to ForceGarbageCollection.
// }
//
// Result: {
//   collected: bool,                  // true when the synchronous path ran or the deferred kick succeeded
//   forced: bool,                     // mirror of args.force (informational)
//   purge_object_references: bool,    // mirror of resolved purge flag
//   prior_allocated_mb: number,       // FPlatformMemory::GetStats().UsedPhysical pre-call
//   new_allocated_mb: number,         // post-call (only meaningful for synchronous path; for deferred
//                                     // path this equals prior_allocated_mb since GC hasn't run yet)
//   freed_mb: number,                 // prior - new (can be negative if other threads allocated)
//   duration_seconds: number          // wall-clock duration of the CollectGarbage call (or deferred-
//                                     // kick metadata flip, which is microseconds)
// }
//
// No PIE guard — GC is valid in editor + PIE. CollectGarbage is the canonical UE entry; KeepFlags
// is GARBAGE_COLLECTION_KEEPFLAGS = (GIsEditor ? RF_Standalone : RF_NoFlags), so editor-rooted
// assets survive (matches Epic's standard pattern).
FMCPResponse Tool_GCCollect(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!GEngine)
	{
		return FMCPToolHelpers::MakeError(Request, kENGErrorInternal,
			TEXT("GEngine unavailable (commandlet / extremely-early init)"));
	}

	bool bForce = true;
	bool bPurgeObjectReferences = true;
	if (Request.Args.IsValid())
	{
		bool ReadFlag = false;
		if (Request.Args->TryGetBoolField(TEXT("force"), ReadFlag))
		{
			bForce = ReadFlag;
		}
		if (Request.Args->TryGetBoolField(TEXT("purge_object_references"), ReadFlag))
		{
			bPurgeObjectReferences = ReadFlag;
		}
	}

	const FPlatformMemoryStats PriorStats = FPlatformMemory::GetStats();
	const double StartTime = FPlatformTime::Seconds();

	bool bCollected = false;
	if (bForce)
	{
		// Synchronous, immediate sweep. CollectGarbage holds the GC lock; ANY thread blocked
		// waiting for GC will wake after this returns.
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, /*bPerformFullPurge*/ bPurgeObjectReferences);
		bCollected = true;
	}
	else
	{
		// Deferred — sets a flag that the engine consumes on next GC tick. The "duration" we
		// measure here is just the kick overhead; the actual sweep happens later.
		GEngine->ForceGarbageCollection(/*bFullPurge*/ bPurgeObjectReferences);
		bCollected = true;
	}

	const double EndTime = FPlatformTime::Seconds();
	const FPlatformMemoryStats NewStats = FPlatformMemory::GetStats();

	const double PriorMB = ENG_BytesToMB(PriorStats.UsedPhysical);
	const double NewMB   = ENG_BytesToMB(NewStats.UsedPhysical);
	const double FreedMB = PriorMB - NewMB;
	const double DurationSeconds = EndTime - StartTime;

	return FMCPJsonBuilder()
		.Bool(TEXT("collected"),              bCollected)
		.Bool(TEXT("forced"),                 bForce)
		.Bool(TEXT("purge_object_references"), bPurgeObjectReferences)
		.Num(TEXT("prior_allocated_mb"),     PriorMB)
		.Num(TEXT("new_allocated_mb"),       NewMB)
		.Num(TEXT("freed_mb"),               FreedMB)
		.Num(TEXT("duration_seconds"),       DurationSeconds)
		.BuildSuccess(Request);
}

// ─── engine.get_memory_snapshot ────────────────────────────────────────────────────────────────
//
// Args: {
//   include_breakdown?: bool    (default false) — when true, append per-allocator-specific stats
//                                                  (FPlatformMemoryStats::GetPlatformSpecificStats)
//                                                  into a ``breakdown`` object.
// }
//
// Result: {
//   used_physical_mb: number,
//   used_virtual_mb: number,
//   peak_used_physical_mb: number,
//   peak_used_virtual_mb: number,
//   available_physical_mb: number,
//   available_virtual_mb: number,
//   total_physical_mb: number,           // from FPlatformMemoryConstants (parent of stats struct)
//   total_virtual_mb: number,
//   page_size_bytes: number,
//   allocator_name: string,              // GMalloc->GetDescriptiveName (e.g. "binned3", "binned2", "mimalloc")
//   breakdown?: { <stat_name>: number_mb, ... }     // present only when args.include_breakdown=true
// }
//
// Read-only, no PIE guard. Distinct from the Phase 1 ``stats.get_memory`` tool in that this
// surface adds ``allocator_name`` at the top level and the optional ``breakdown`` object exposing
// platform-specific allocator stats (Windows: PageFaultCount/PageFile/etc.).
FMCPResponse Tool_GetMemorySnapshot(const FMCPRequest& Request)
{
	// Lane B safe (Phase 4.2 promote 2026-05-22): FPlatformMemory::{GetStats,GetConstants} +
	// FApp time getters are documented thread-safe (atomic snapshots).

	const FPlatformMemoryStats Stats = FPlatformMemory::GetStats();
	const FPlatformMemoryConstants& Constants = FPlatformMemory::GetConstants();

	bool bIncludeBreakdown = false;
	if (Request.Args.IsValid())
	{
		Request.Args->TryGetBoolField(TEXT("include_breakdown"), bIncludeBreakdown);
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetNumberField(TEXT("used_physical_mb"),       ENG_BytesToMB(Stats.UsedPhysical));
	Out->SetNumberField(TEXT("used_virtual_mb"),        ENG_BytesToMB(Stats.UsedVirtual));
	Out->SetNumberField(TEXT("peak_used_physical_mb"),  ENG_BytesToMB(Stats.PeakUsedPhysical));
	Out->SetNumberField(TEXT("peak_used_virtual_mb"),   ENG_BytesToMB(Stats.PeakUsedVirtual));
	Out->SetNumberField(TEXT("available_physical_mb"),  ENG_BytesToMB(Stats.AvailablePhysical));
	Out->SetNumberField(TEXT("available_virtual_mb"),   ENG_BytesToMB(Stats.AvailableVirtual));
	Out->SetNumberField(TEXT("total_physical_mb"),      ENG_BytesToMB(Constants.TotalPhysical));
	Out->SetNumberField(TEXT("total_virtual_mb"),       ENG_BytesToMB(Constants.TotalVirtual));
	Out->SetNumberField(TEXT("page_size_bytes"),        static_cast<double>(Constants.PageSize));

	// GMalloc may be null during extremely-early init; treat as soft failure (empty string) rather
	// than -32603, since the rest of the snapshot is still valid.
	const FString AllocatorName = (GMalloc != nullptr)
		? FString(GMalloc->GetDescriptiveName())
		: FString(TEXT("unknown"));
	Out->SetStringField(TEXT("allocator_name"), AllocatorName);

	if (bIncludeBreakdown)
	{
		TSharedRef<FJsonObject> BreakdownObj = MakeShared<FJsonObject>();
		// Platform-specific stats (Windows: VirtualMemoryAvailable, PageFile, etc.; Linux: RSS,
		// Buffers, etc.). Values are reported in bytes by the engine — surface as MB for
		// consistency with the rest of this tool.
		const TArray<FPlatformMemoryStats::FPlatformSpecificStat> PlatformStats = Stats.GetPlatformSpecificStats();
		for (const FPlatformMemoryStats::FPlatformSpecificStat& Stat : PlatformStats)
		{
			if (Stat.Name == nullptr) { continue; } // Defensive against malformed platform data.
			BreakdownObj->SetNumberField(FString(Stat.Name), ENG_BytesToMB(Stat.Value));
		}
		Out->SetObjectField(TEXT("breakdown"), BreakdownObj);
	}

	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	// Phase 4.2 (2026-05-22): get_info + get_memory_snapshot promoted to Lane B (atomic global +
	// thread-safe FPlatformMemory / FApp reads). gc_collect stays Lane A — GEngine->ForceGarbageCollection
	// MUST run on game thread (operates on the global UObject heap).
	RegisterTool(TEXT("engine.get_info"),             &Tool_GetInfo,            /*Lane B*/ true);
	RegisterTool(TEXT("engine.gc_collect"),           &Tool_GCCollect,          /*Lane A*/ false);
	RegisterTool(TEXT("engine.get_memory_snapshot"),  &Tool_GetMemorySnapshot,  /*Lane B*/ true);

	UE_LOG(LogMCP, Log,
		TEXT("Engine surface registered: 3 tools "
			 "(get_info + gc_collect + get_memory_snapshot), all Lane A"));
}

} // namespace FEngineTools

MCP_REGISTER_SURFACE(EngineTools, &FEngineTools::Register)
