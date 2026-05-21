// Copyright FatumGame. All Rights Reserved.

#include "StatsTools.h"

#include "MCPSurfaceRegistry.h"

#include "FMCPDispatchQueue.h"
#include "MCPToolHelpers.h"
#include "UnrealMCPBridge.h"

#include "Engine/Engine.h"
#include "HAL/PlatformMemory.h"
#include "HAL/UnrealMemory.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// Engine globals — forward-declared extern (defined in UnrealEngine.cpp).
extern ENGINE_API float GAverageFPS;
extern ENGINE_API float GAverageMS;

// STA_MakeError / STA_MakeSuccessObj removed in Phase 3 — see FMCPToolHelpers in MCPToolHelpers.h.

namespace FStatsTools
{

// ─── stats.get_engine — engine-wide frame/FPS/draw snapshot ─────────────────────────────────
FMCPResponse Tool_GetEngine(const FMCPRequest& Request)
{
	check(IsInGameThread());

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetNumberField(TEXT("avg_fps"),        static_cast<double>(GAverageFPS));
	Out->SetNumberField(TEXT("avg_ms"),         static_cast<double>(GAverageMS));
	Out->SetNumberField(TEXT("frame_counter"),  static_cast<double>(GFrameCounter));
	Out->SetNumberField(TEXT("frame_number_rendered_last"),
		static_cast<double>(GFrameNumberRenderThread));
	Out->SetBoolField(TEXT("is_editor"),        GIsEditor);
	Out->SetBoolField(TEXT("is_running_commandlet"), IsRunningCommandlet());

	// Engine subsystem state.
	if (GEngine)
	{
		Out->SetNumberField(TEXT("max_fps"),    static_cast<double>(GEngine->bSmoothFrameRate ? GEngine->SmoothedFrameRateRange.GetUpperBoundValue() : 0));
		Out->SetBoolField(TEXT("smooth_frame_rate"), GEngine->bSmoothFrameRate);
	}

	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── stats.get_memory — FPlatformMemory + UE allocator breakdown ────────────────────────────
FMCPResponse Tool_GetMemory(const FMCPRequest& Request)
{
	check(IsInGameThread());

	const FPlatformMemoryStats S = FPlatformMemory::GetStats();
	const FPlatformMemoryConstants& C = FPlatformMemory::GetConstants();

	TSharedRef<FJsonObject> PhysMem = MakeShared<FJsonObject>();
	PhysMem->SetNumberField(TEXT("total_mb"),     static_cast<double>(C.TotalPhysical)      / (1024.0 * 1024.0));
	PhysMem->SetNumberField(TEXT("available_mb"), static_cast<double>(S.AvailablePhysical)  / (1024.0 * 1024.0));
	PhysMem->SetNumberField(TEXT("used_mb"),      static_cast<double>(S.UsedPhysical)       / (1024.0 * 1024.0));
	PhysMem->SetNumberField(TEXT("peak_used_mb"), static_cast<double>(S.PeakUsedPhysical)   / (1024.0 * 1024.0));

	TSharedRef<FJsonObject> VirtMem = MakeShared<FJsonObject>();
	VirtMem->SetNumberField(TEXT("total_mb"),     static_cast<double>(C.TotalVirtual)       / (1024.0 * 1024.0));
	VirtMem->SetNumberField(TEXT("available_mb"), static_cast<double>(S.AvailableVirtual)   / (1024.0 * 1024.0));
	VirtMem->SetNumberField(TEXT("used_mb"),      static_cast<double>(S.UsedVirtual)        / (1024.0 * 1024.0));
	VirtMem->SetNumberField(TEXT("peak_used_mb"), static_cast<double>(S.PeakUsedVirtual)    / (1024.0 * 1024.0));

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetObjectField(TEXT("physical"), PhysMem);
	Out->SetObjectField(TEXT("virtual"),  VirtMem);
	Out->SetNumberField(TEXT("page_size_bytes"), static_cast<double>(C.PageSize));

	// UE allocator stats — FMemory has TotalUsedMemory style queries via GMalloc but they're
	// allocator-dependent. We expose what's universally available.
	if (GMalloc)
	{
		// GMalloc->Exec("MEM") prints a dump but doesn't return structured data; skip for now.
		Out->SetStringField(TEXT("allocator_name"), GMalloc->GetDescriptiveName());
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

	RegisterTool(TEXT("stats.get_engine"), &Tool_GetEngine, /*Lane A*/ false);
	RegisterTool(TEXT("stats.get_memory"), &Tool_GetMemory, /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("Stats surface registered: stats.get_engine + stats.get_memory (Lane A; editor-context safe)"));
}

} // namespace FStatsTools

MCP_REGISTER_SURFACE(StatsTools, &FStatsTools::Register)
