// Copyright FatumGame. All Rights Reserved.

#include "StatTools.h"

#include "MCPSurfaceRegistry.h"

#include "FMCPDispatchQueue.h"
#include "MCPJsonBuilder.h"
#include "MCPToolHelpers.h"
#include "Utils/MCPPathSandbox.h"

#include "Containers/Ticker.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "HAL/CriticalSection.h"
#include "HAL/FileManager.h"
#include "Misc/App.h"
#include "Misc/CoreDelegates.h"
#include "Misc/CString.h"
#include "Misc/Paths.h"
#include "Misc/OutputDeviceNull.h"

// Render/frame timer globals — `extern RENDERCORE_API uint32` per RenderTimer.h.
#include "RenderTimer.h"

// RHI draw-call counters — `extern RHI_API int32` per RHIStats.h.
#include "RHIStats.h"

// Modern GPU frame cycles accessor (GGPUFrameTime is deprecated 5.6).
#include "DynamicRHI.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// STA_ prefix per the unity-build symbol-collision convention.
	constexpr int32 kSTAErrorInternal       = -32603;
	constexpr int32 kSTAErrorInvalidParams  = kMCPErrorInvalidParams;
	constexpr int32 kSTAErrorPathEscape     = kMCPErrorPathEscape;
	constexpr int32 kSTAErrorOperationFailed = kMCPErrorOperationFailed;

	// Wave L+1 (2026-05-23) Lane B promotion: list_groups returns a hardcoded constant array. The
	// lock is defense-in-depth against future code paths that might mutate kKnownStatGroups (which
	// is constexpr today). Held for the duration of the array iteration only.
	static FCriticalSection gStatTablesLock;

	// Anti-injection: stat names are an FParse::Token followed by case-insensitive
	// command-name compare in StatsCommand.cpp. We constrain to alphanumeric +
	// underscore to keep arbitrary text out of GEngine->Exec.
	bool STA_IsValidStatName(const FString& Name)
	{
		if (Name.IsEmpty()) { return false; }
		const TCHAR First = Name[0];
		if (!(((First >= 'A') && (First <= 'Z')) || ((First >= 'a') && (First <= 'z')) || (First == '_')))
		{
			return false;
		}
		for (int32 i = 1; i < Name.Len(); ++i)
		{
			const TCHAR Ch = Name[i];
			if (!(((Ch >= 'A') && (Ch <= 'Z')) ||
				  ((Ch >= 'a') && (Ch <= 'z')) ||
				  ((Ch >= '0') && (Ch <= '9')) ||
				  (Ch == '_')))
			{
				return false;
			}
		}
		return true;
	}

	// Pick the editor world for GEngine->Exec command routing. Falls back to PIE
	// world if PIE is active. Returns null only in commandlet/cooker contexts.
	UWorld* STA_GetExecWorld()
	{
		if (!GEditor) { return nullptr; }
		// Prefer the PIE world when active so stat overlays affect the active session.
		if (GEditor->PlayWorld) { return GEditor->PlayWorld; }
		return GEditor->GetEditorWorldContext().World();
	}

	// Canonical UE 5.7 stat group names from
	// Runtime/Core/Public/Stats/GlobalStats.inl plus the Engine-module groups
	// commonly toggled by AI workflows. Engine doesn't expose runtime enumeration
	// (FStatGroupEnableManager has no public iteration API; FStatsThreadState::Groups
	// requires stats-thread access and races against the stat collector). Keeping
	// the list aligned with the engine .inl plus well-known additions is good
	// enough for AI introspection — the per-stat command interface (`stat <name>`)
	// accepts any name; the list is purely for discovery.
	const TCHAR* const kKnownStatGroups[] = {
		// From GlobalStats.inl (Core module canonical set):
		TEXT("STATGROUP_AI"),
		TEXT("STATGROUP_Anim"),
		TEXT("STATGROUP_AsyncIO"),
		TEXT("STATGROUP_Audio"),
		TEXT("STATGROUP_BeamParticles"),
		TEXT("STATGROUP_CPUStalls"),
		TEXT("STATGROUP_Canvas"),
		TEXT("STATGROUP_Character"),
		TEXT("STATGROUP_Collision"),
		TEXT("STATGROUP_CollisionTags"),
		TEXT("STATGROUP_CollisionVerbose"),
		TEXT("STATGROUP_D3D11RHI"),
		TEXT("STATGROUP_DDC"),
		TEXT("STATGROUP_Default"),
		TEXT("STATGROUP_Engine"),
		TEXT("STATGROUP_FPSChart"),
		TEXT("STATGROUP_GPUParticles"),
		TEXT("STATGROUP_Game"),
		TEXT("STATGROUP_GPUDEFRAG"),
		TEXT("STATGROUP_InitViews"),
		TEXT("STATGROUP_Landscape"),
		TEXT("STATGROUP_LightRendering"),
		TEXT("STATGROUP_LoadTime"),
		TEXT("STATGROUP_LoadTimeClass"),
		TEXT("STATGROUP_LoadTimeClassCount"),
		TEXT("STATGROUP_LoadTimeVerbose"),
		TEXT("STATGROUP_Media"),
		TEXT("STATGROUP_MemoryAllocator"),
		TEXT("STATGROUP_MemoryPlatform"),
		TEXT("STATGROUP_MemoryStaticMesh"),
		TEXT("STATGROUP_Memory"),
		TEXT("STATGROUP_MeshParticles"),
		TEXT("STATGROUP_MetalRHI"),
		TEXT("STATGROUP_AGXRHI"),
		TEXT("STATGROUP_MorphTarget"),
		TEXT("STATGROUP_Navigation"),
		TEXT("STATGROUP_Net"),
		TEXT("STATGROUP_Packet"),
		TEXT("STATGROUP_Object"),
		TEXT("STATGROUP_ObjectVerbose"),
		TEXT("STATGROUP_OpenGLRHI"),
		TEXT("STATGROUP_PakFile"),
		TEXT("STATGROUP_ParticleMem"),
		TEXT("STATGROUP_Particles"),
		TEXT("STATGROUP_Physics"),
		TEXT("STATGROUP_Platform"),
		TEXT("STATGROUP_Profiler"),
		TEXT("STATGROUP_Quick"),
		TEXT("STATGROUP_RHI"),
		TEXT("STATGROUP_RDG"),
		TEXT("STATGROUP_RenderThreadProcessing"),
		TEXT("STATGROUP_RenderTargetPool"),
		TEXT("STATGROUP_RenderScaling"),
		TEXT("STATGROUP_SceneMemory"),
		TEXT("STATGROUP_SceneRendering"),
		TEXT("STATGROUP_SceneUpdate"),
		TEXT("STATGROUP_ServerCPU"),
		TEXT("STATGROUP_MapBuildData"),
		TEXT("STATGROUP_ShaderCompiling"),
		TEXT("STATGROUP_Shaders"),
		TEXT("STATGROUP_ShadowRendering"),
		TEXT("STATGROUP_StatSystem"),
		TEXT("STATGROUP_StreamingOverview"),
		TEXT("STATGROUP_StreamingDetails"),
		TEXT("STATGROUP_Streaming"),
		TEXT("STATGROUP_TargetPlatform"),
		TEXT("STATGROUP_Text"),
		TEXT("STATGROUP_ThreadPoolAsyncTasks"),
		TEXT("STATGROUP_Threading"),
		TEXT("STATGROUP_Threads"),
		TEXT("STATGROUP_Tickables"),
		TEXT("STATGROUP_TrailParticles"),
		TEXT("STATGROUP_UI"),
		TEXT("STATGROUP_UObjects"),
		TEXT("STATGROUP_User"),
		// Common Engine / Renderer additions outside the Core .inl:
		TEXT("STATGROUP_FPS"),
		TEXT("STATGROUP_Unit"),
		TEXT("STATGROUP_Slate"),
		TEXT("STATGROUP_Niagara"),
		TEXT("STATGROUP_GPU"),
		TEXT("STATGROUP_Detailed"),
		TEXT("STATGROUP_StatNamedEvents"),
		TEXT("STATGROUP_TaskGraphTasks"),
		TEXT("STATGROUP_SceneRenderingDetailed"),
		TEXT("STATGROUP_PostProcessing"),
		TEXT("STATGROUP_TextureStreaming"),
		TEXT("STATGROUP_VirtualTextureMemory"),
		TEXT("STATGROUP_GPUBuffer"),
		TEXT("STATGROUP_World"),
	};
} // namespace

namespace FStatTools
{

// --- stat.list_groups -------------------------------------------------------------------------
//
// Args:    {}
// Result:  { groups: [{ name }], total_count }
//
// Engine doesn't expose runtime group enumeration publicly (FStatGroupEnableManager
// has no iteration API; FStatsThreadState::Groups requires stats-thread access).
// We return a hardcoded canonical set built from Runtime/Core/Public/Stats/GlobalStats.inl
// plus well-known Engine/Renderer additions. The `stat <name>` toggle command
// accepts ANY name regardless of presence in this list — the list is purely for
// discovery / autocomplete affordance.
// Wave L+1: Lane B — kKnownStatGroups is a constexpr TCHAR* array (read-only constant data).
// gStatTablesLock guards iteration as defense-in-depth.
FMCPResponse Tool_ListGroups(const FMCPRequest& Request)
{
	FScopeLock Lock(&gStatTablesLock);

	FMCPJsonArrayBuilder Groups;
	for (const TCHAR* Name : kKnownStatGroups)
	{
		Groups.AddObject([Name](FMCPJsonBuilder& B)
		{
			B.Str(TEXT("name"), FString(Name));
		});
	}

	return FMCPJsonBuilder()
		.Arr(TEXT("groups"), Groups.ToValueArray())
		.Int(TEXT("total_count"), UE_ARRAY_COUNT(kKnownStatGroups))
		.BuildSuccess(Request);
}

// --- stat.toggle ------------------------------------------------------------------------------
//
// Args:    { stat_name: string }
// Result:  { stat_name, command_executed }
//
// Toggle-only — UE 5.7 `stat <name>` doesn't support `-enable`/`-disable`
// suffixes uniformly (the engine parses tokens via FParse::Command which only
// recognises a few well-known modifiers). To force a specific state, call
// repeatedly and inspect the editor overlay.
FMCPResponse Tool_Toggle(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString StatName;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("stat_name"), StatName, Err))
	{
		return Err;
	}

	StatName = StatName.TrimStartAndEnd();
	if (!STA_IsValidStatName(StatName))
	{
		return FMCPToolHelpers::MakeError(Request, kSTAErrorInvalidParams,
			FString::Printf(TEXT("stat_name '%s' must match ^[A-Za-z_][A-Za-z0-9_]*$"), *StatName));
	}

	if (!GEngine)
	{
		return FMCPToolHelpers::MakeError(Request, kSTAErrorInternal, TEXT("GEngine is null"));
	}

	UWorld* World = STA_GetExecWorld();
	const FString Cmd = FString::Printf(TEXT("stat %s"), *StatName);

	// stat commands always return true even when the name is unknown — UE
	// accepts the toggle and queues the registration. We don't surface failures
	// here unless GEngine->Exec itself returns false.
	FOutputDeviceNull NullDev;
	const bool bOk = GEngine->Exec(World, *Cmd, NullDev);
	if (!bOk)
	{
		return FMCPToolHelpers::MakeError(Request, kSTAErrorOperationFailed,
			FString::Printf(TEXT("GEngine->Exec('%s') returned false"), *Cmd));
	}

	return FMCPJsonBuilder()
		.Str(TEXT("stat_name"), StatName)
		.Str(TEXT("command_executed"), Cmd)
		.BuildSuccess(Request);
}

// --- stat.get_unit_values ---------------------------------------------------------------------
//
// Args:    {}
// Result:  { fps, frame_time_ms, game_thread_ms, render_thread_ms, gpu_ms,
//            draw_calls, triangles, frame_counter }
//
// Read-only snapshot of per-frame counters. Degraded fields are omitted (gpu_ms
// = 0 when no RHI is initialised; draw_calls/triangles read RHI globals).
// Values are last-frame state — call once per frame to track over time.
FMCPResponse Tool_GetUnitValues(const FMCPRequest& Request)
{
	check(IsInGameThread());

	const double DeltaSec = FApp::GetDeltaTime();
	const double Fps      = (DeltaSec > 0.0) ? (1.0 / DeltaSec) : 0.0;
	const double FrameMs  = DeltaSec * 1000.0;

	// FPlatformTime::ToMilliseconds expects raw cycle counts.
	const double GameThreadMs   = FPlatformTime::ToMilliseconds(GGameThreadTime);
	const double RenderThreadMs = FPlatformTime::ToMilliseconds(GRenderThreadTime);
	const double GpuMs          = FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles(0));

	// RHI draw-call counters are double-buffered against the render-thread
	// frame counter. Reading the "other" slot avoids a tear with the in-flight
	// frame the render thread is writing.
	const int32 RenderFrameIdx  = static_cast<int32>(GFrameCounterRenderThread & 1u);
	const int32 ReadSlot        = (RenderFrameIdx + 1) & 1; // the slot the render thread JUST finished
	const int32 DrawCalls       = GNumDrawCallsRHI[ReadSlot];
	const int32 Triangles       = GNumPrimitivesDrawnRHI[ReadSlot];

	return FMCPJsonBuilder()
		.Num(TEXT("fps"),              Fps)
		.Num(TEXT("frame_time_ms"),    FrameMs)
		.Num(TEXT("game_thread_ms"),   GameThreadMs)
		.Num(TEXT("render_thread_ms"), RenderThreadMs)
		.Num(TEXT("gpu_ms"),           GpuMs)
		.Int(TEXT("draw_calls"),       DrawCalls)
		.Int(TEXT("triangles"),        Triangles)
		.Int(TEXT("frame_counter"),    static_cast<int64>(GFrameCounter))
		.BuildSuccess(Request);
}

// --- stat.dump_to_file ------------------------------------------------------------------------
//
// Args:    { output_path?: "Saved/Profiling/MCP.ue4stats", frames?: int [1, 10000] }
// Result:  { path, frames_requested, note }
//
// Engine `stat startfile <path>` begins a multi-frame capture; we schedule a
// matching `stat stopfile` after the requested frame count via FTSTicker (which
// ticks on the game thread by UE convention — safe to call GEngine->Exec from
// inside it).
//
// The response returns immediately — the file appears on disk after ~N frames
// (N/60 seconds at 60 fps). Callers poll their own filesystem for the path.
//
// Concurrent calls overwrite each other (only one stat capture at a time). The
// default output path includes an ISO-UTC timestamp so concurrent default-path
// calls produce uniquely-named files at least at second granularity.
FMCPResponse Tool_DumpToFile(const FMCPRequest& Request)
{
	check(IsInGameThread());

	// 1. Frames param (optional).
	int32 Frames = 60;
	if (Request.Args.IsValid())
	{
		Request.Args->TryGetNumberField(TEXT("frames"), Frames);
	}
	Frames = FMath::Clamp(Frames, 1, 10000);

	// 2. Resolve output path. Default lives under <Saved>/Profiling/UnrealStats/.
	FString AbsPath;
	FString RawPath;
	if (Request.Args.IsValid())
	{
		Request.Args->TryGetStringField(TEXT("output_path"), RawPath);
	}

	if (RawPath.IsEmpty())
	{
		const FString DefaultDir = FPaths::ProjectSavedDir() / TEXT("Profiling/UnrealStats/");
		IFileManager::Get().MakeDirectory(*DefaultDir, /*Tree=*/true);
		FString TimeTag = FDateTime::UtcNow().ToIso8601();
		TimeTag.ReplaceInline(TEXT(":"), TEXT("-"));
		AbsPath = DefaultDir / FString::Printf(TEXT("MCP_%s.ue4stats"), *TimeTag);
	}
	else
	{
		FString SandboxErr;
		if (!FMCPPathSandbox::Resolve(RawPath, AbsPath, SandboxErr))
		{
			return FMCPToolHelpers::MakeError(Request, kSTAErrorPathEscape,
				FString::Printf(TEXT("output_path '%s' rejected: %s"), *RawPath, *SandboxErr));
		}
		const FString ParentDir = FPaths::GetPath(AbsPath);
		if (!ParentDir.IsEmpty())
		{
			IFileManager::Get().MakeDirectory(*ParentDir, /*Tree=*/true);
		}
	}

	if (!GEngine)
	{
		return FMCPToolHelpers::MakeError(Request, kSTAErrorInternal, TEXT("GEngine is null"));
	}

	UWorld* World = STA_GetExecWorld();

	// 3. Kick off the capture. `stat startfile <path>` (case-insensitive command
	// match in StatsCommand.cpp); path is a token, so unquoted paths with spaces
	// would lose tokens. We assume sandbox paths are space-free in the standard
	// FatumGame layout.
	const FString StartCmd = FString::Printf(TEXT("stat startfile %s"), *AbsPath);
	FOutputDeviceNull NullDev;
	const bool bStartOk = GEngine->Exec(World, *StartCmd, NullDev);
	if (!bStartOk)
	{
		return FMCPToolHelpers::MakeError(Request, kSTAErrorOperationFailed,
			FString::Printf(TEXT("GEngine->Exec('%s') returned false"), *StartCmd));
	}

	// 4. Schedule the stop command N frames hence. FTSTicker fires on the game
	// thread (per UE convention); we count frames by triggering once per tick
	// and decrementing. Delay 0 means "next frame".
	int32 FramesRemaining = Frames;
	FTSTicker::GetCoreTicker().AddTicker(
		TEXT("MCP_StatDumpStopTimer"),
		0.0f,
		[FramesRemaining](float /*DeltaTime*/) mutable -> bool
		{
			--FramesRemaining;
			if (FramesRemaining <= 0)
			{
				if (GEngine)
				{
					FOutputDeviceNull NullDevInner;
					GEngine->Exec(nullptr, TEXT("stat stopfile"), NullDevInner);
				}
				return false; // stop ticking
			}
			return true; // continue
		});

	return FMCPJsonBuilder()
		.Str(TEXT("path"), AbsPath)
		.Int(TEXT("frames_requested"), Frames)
		.Str(TEXT("note"), FString::Printf(
			TEXT("Capture started. File will be finalized after %d frames elapse (~%.2fs at 60fps)."),
			Frames, Frames / 60.0))
		.BuildSuccess(Request);
}

// --- stat.is_enabled --------------------------------------------------------------------------
//
// Args:    { stat_name: string (required, regex `^[A-Za-z][A-Za-z0-9_]*$`) }
// Result:  { stat_name, is_enabled, best_effort }
//
// **Uses FCoreDelegates::StatCheckEnabled — the same delegate the engine itself broadcasts to
// inspect viewport stat state (see StatsCommand.cpp:2174 / GameViewportClient.cpp:4471). Each
// viewport client (game viewport + editor viewports) registers a handler in its ctor; the
// delegate populates `bCurrent` for the active-processing viewport and OR-accumulates `bOthers`
// for the rest. We return the disjunction — true if ANY viewport currently shows the stat.**
//
// Name handling: the engine stores names verbatim as the user passes them to `stat <name>` (e.g.,
// `stat fps` → "fps" in EnabledStats). We strip an optional leading "STATGROUP_" (case-insensitive)
// to canonicalise — toggling via the official command never includes that prefix.
//
// Threading: the broadcast subscribers (UGameViewportClient / FEditorViewportClient) call
// IsStatEnabled() which checks EnabledStats.Contains(); both registration and lookup happen on
// the game thread. Run on Lane A to keep alignment.
//
// `best_effort` semantics:
//   - false: the delegate has at least one bound subscriber AND returned a definitive result.
//   - true:  delegate has no subscribers (no GameViewport / no editor viewport active —
//     rare in -nullrhi commandlet contexts) → caller should treat as advisory.
FMCPResponse Tool_IsEnabled(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString StatName;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("stat_name"), StatName, Err))
	{
		return Err;
	}

	StatName = StatName.TrimStartAndEnd();
	if (!STA_IsValidStatName(StatName))
	{
		return FMCPToolHelpers::MakeError(Request, kSTAErrorInvalidParams,
			FString::Printf(TEXT("stat_name '%s' must match ^[A-Za-z_][A-Za-z0-9_]*$"), *StatName));
	}

	if (!GEngine)
	{
		return FMCPToolHelpers::MakeError(Request, kSTAErrorInternal, TEXT("GEngine is null"));
	}

	// Strip optional leading STATGROUP_ prefix (case-insensitive). The viewport's EnabledStats
	// set never contains the prefixed form — `stat fps` adds "fps", not "STATGROUP_fps".
	FString Lookup = StatName;
	if (Lookup.StartsWith(TEXT("STATGROUP_"), ESearchCase::IgnoreCase))
	{
		Lookup.RightChopInline(10, EAllowShrinking::No);
	}

	const bool bHasSubscribers = FCoreDelegates::StatCheckEnabled.IsBound();
	bool bCurrent = false;
	bool bOthers  = false;
	FCoreDelegates::StatCheckEnabled.Broadcast(*Lookup, bCurrent, bOthers);

	const bool bIsEnabled  = bCurrent || bOthers;
	const bool bBestEffort = !bHasSubscribers;

	return FMCPJsonBuilder()
		.Str(TEXT("stat_name"), StatName)
		.Bool(TEXT("is_enabled"), bIsEnabled)
		.Bool(TEXT("best_effort"), bBestEffort)
		.BuildSuccess(Request);
}

void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	// Wave L+1 (2026-05-23): stat.list_groups promoted to Lane B — returns a constexpr TCHAR*[]
	// constant set, no engine state touched. gStatTablesLock is defense-in-depth only.
	//
	// get_unit_values reads extern globals (GGameThreadTime / GRenderThreadTime / RHIGetGPUFrameCycles
	// / GNumDrawCallsRHI). Render-thread writers may tear 64-bit values on 32-bit platforms — kept
	// Lane A.
	// toggle / dump_to_file / is_enabled all call into engine state (GEngine->Exec or
	// FCoreDelegates::StatCheckEnabled broadcasting to viewport clients) — Lane A only.
	RegisterTool(TEXT("stat.list_groups"),     &Tool_ListGroups,    /*Lane B*/ true);
	RegisterTool(TEXT("stat.toggle"),          &Tool_Toggle,        /*Lane A*/ false);
	RegisterTool(TEXT("stat.get_unit_values"), &Tool_GetUnitValues, /*Lane A*/ false);
	RegisterTool(TEXT("stat.dump_to_file"),    &Tool_DumpToFile,    /*Lane A*/ false);
	RegisterTool(TEXT("stat.is_enabled"),      &Tool_IsEnabled,     /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("Stat surface registered: stat.{list_groups, toggle, get_unit_values, dump_to_file, "
			 "is_enabled} (4 Lane A + 1 Lane B)"));
}

} // namespace FStatTools

MCP_REGISTER_SURFACE(StatTools, &FStatTools::Register)

#undef LOCTEXT_NAMESPACE
