// Copyright FatumGame. All Rights Reserved.

#include "UnrealMCPBridge.h"

DEFINE_LOG_CATEGORY(LogMCP);

void FUnrealMCPBridgeModule::StartupModule()
{
	UE_LOG(LogMCP, Log,
		TEXT("MCP bridge module loaded, listener=NotStarted (Phase 1 scaffold)"));
}

void FUnrealMCPBridgeModule::ShutdownModule()
{
	UE_LOG(LogMCP, Log, TEXT("MCP bridge module shutdown"));
}

IMPLEMENT_MODULE(FUnrealMCPBridgeModule, UnrealMCPBridge)
