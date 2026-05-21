// Copyright FatumGame. All Rights Reserved.

#include "MCPSurfaceRegistry.h"

#include "FMCPDispatchQueue.h"
#include "UnrealMCPBridge.h" // LogMCP

FMCPSurfaceRegistry& FMCPSurfaceRegistry::Get()
{
	// Meyers singleton — constructed on first access. C++11+ guarantees the static-
	// local initialization is thread-safe (the runtime takes a one-time lock around
	// the constructor call), and no static-init-order dependency exists between this
	// singleton and any caller — by the time MCP_REGISTER_SURFACE macros fire (each
	// surface .cpp's static init), this function is callable and the singleton
	// materialises lazily.
	static FMCPSurfaceRegistry Instance;
	return Instance;
}

void FMCPSurfaceRegistry::Add(const TCHAR* InSurfaceName, FRegisterFn InFn)
{
	check(InSurfaceName != nullptr);
	check(InFn != nullptr);
	Entries.Add({ InSurfaceName, InFn });
}

void FMCPSurfaceRegistry::RegisterAll(FMCPDispatchQueue& Queue, TArray<FString>& OutMethodNames)
{
	// Iteration order matches static-initialization order across TUs, which is
	// unspecified by the C++ standard. That's fine for the MCP bridge — each
	// surface registers methods under disjoint namespaces (data_table.*, mat.*,
	// physics.*, etc.) so order has no observable effect on the registry's
	// final contents.
	const int32 StartingMethodCount = OutMethodNames.Num();
	for (const FEntry& E : Entries)
	{
		const int32 PriorCount = OutMethodNames.Num();
		E.Fn(Queue, OutMethodNames);
		const int32 Added = OutMethodNames.Num() - PriorCount;
		UE_LOG(LogMCP, Verbose,
			TEXT("FMCPSurfaceRegistry: %s -> %d new method(s) (%d total in surface call)"),
			E.SurfaceName, Added, OutMethodNames.Num() - StartingMethodCount);
	}
	UE_LOG(LogMCP, Log,
		TEXT("FMCPSurfaceRegistry: %d auto-registered surface(s), %d new method(s) added"),
		Entries.Num(), OutMethodNames.Num() - StartingMethodCount);
}
