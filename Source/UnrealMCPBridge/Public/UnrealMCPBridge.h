// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"

DECLARE_LOG_CATEGORY_EXTERN(LogMCP, Log, All);

/**
 * Public module interface for the Unreal MCP Bridge.
 * Phase 1 Day 1: scaffold only. TCP listener / dispatch queue / Python pipeline
 * are introduced in subsequent Day-2..Day-5 work — see D:/tmp/mcp_unreal_blueprint_v2_patch.md.
 */
class IUnrealMCPBridgeModule : public IModuleInterface
{
public:
	/**
	 * Returns the loaded module singleton, loading it if necessary.
	 * Editor-only module — calling from non-editor builds will fail to load.
	 */
	static IUnrealMCPBridgeModule& Get()
	{
		return FModuleManager::LoadModuleChecked<IUnrealMCPBridgeModule>("UnrealMCPBridge");
	}

	static bool IsAvailable()
	{
		return FModuleManager::Get().IsModuleLoaded("UnrealMCPBridge");
	}

	/**
	 * Whether the bridge TCP listener has been started and is accepting connections.
	 * Phase 1 Day 1: always returns false (no listener yet).
	 */
	virtual bool IsListening() const = 0;
};

/**
 * Concrete module implementation. Phase 1 Day 1: empty Startup/Shutdown lifecycle
 * that simply registers the log category. No socket / Python work yet.
 */
class FUnrealMCPBridgeModule : public IUnrealMCPBridgeModule
{
public:
	// IModuleInterface
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	// IUnrealMCPBridgeModule
	virtual bool IsListening() const override { return false; }
};
