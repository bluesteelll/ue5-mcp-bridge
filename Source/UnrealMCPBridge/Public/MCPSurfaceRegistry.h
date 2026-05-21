// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FMCPDispatchQueue;

/**
 * Process-global auto-registration registry for MCP tool surfaces.
 *
 * Before this refactor, every tool surface had to be manually registered in
 * ``UnrealMCPBridge.cpp::RegisterDefaultDispatchHandlers`` — both an ``#include``
 * for the surface's header AND an explicit ``FXxxTools::Register(Queue, Names);``
 * call. This file was the single coordination point for ALL surfaces, which made
 * it the bottleneck for parallel-agent waves: any two agents shipping surfaces
 * concurrently would conflict on this file, forcing sequential merge.
 *
 * The registry replaces that pattern. Each tool surface .cpp file places a SINGLE
 * line at translation-unit scope:
 *
 *   MCP_REGISTER_SURFACE(MyTools, &FMyTools::Register)
 *
 * The macro emits an anonymous-namespace struct whose constructor runs at static
 * initialization time and adds the surface's ``Register`` function pointer to a
 * process-global list. ``UnrealMCPBridge.cpp`` then iterates the list ONCE during
 * ``RegisterDefaultDispatchHandlers`` and calls each entry — no per-surface code
 * in the bridge file.
 *
 * **Net effect**: agents shipping new surfaces touch only their own
 * ``XxxTools.{h,cpp}`` + ``D:/tmp/test_wave_*.py`` — true file-disjoint
 * parallelism. The bridge file is touched once, by the orchestrator, for cross-
 * surface concerns (Build.cs deps that benefit multiple surfaces, etc.).
 *
 * **Thread safety**: ``Add`` is called only from static initializers (single-
 * threaded). ``RegisterAll`` is called only once from
 * ``RegisterDefaultDispatchHandlers`` on the game thread at module startup.
 * ``GetEntries`` is read-only and only intended for diagnostics. No locking
 * needed.
 *
 * **Static-init order**: irrelevant. ``Get()`` is a Meyers singleton (function-
 * local static), constructed on first access. C++11+ guarantees thread-safe
 * initialization. Multiple TUs adding to the same registry is unordered but
 * convergent — every surface that gets linked into the module gets registered.
 *
 * **Dead-stripping concern**: a TU containing only a static initializer can be
 * eliminated by some linkers if no symbol is referenced from elsewhere. UE
 * modules avoid this because the build system compiles every .cpp under the
 * module's source directory and the linker sees them all as defined-symbol
 * carriers. If a future build configuration breaks this assumption, add a
 * dummy ``ENGINE_API extern int32`` symbol to each surface .cpp and reference
 * it from the bridge file. Not currently needed.
 */
class UNREALMCPBRIDGE_API FMCPSurfaceRegistry
{
public:
	using FRegisterFn = void (*)(FMCPDispatchQueue&, TArray<FString>&);

	struct FEntry
	{
		const TCHAR* SurfaceName;
		FRegisterFn  Fn;
	};

	/** Meyers singleton — thread-safe init, no static-init-order dependency. */
	static FMCPSurfaceRegistry& Get();

	/**
	 * Called from each surface .cpp's MCP_REGISTER_SURFACE static initializer.
	 * NOT intended for direct invocation — use the macro.
	 */
	void Add(const TCHAR* InSurfaceName, FRegisterFn InFn);

	/**
	 * Invoked once from UnrealMCPBridge.cpp::RegisterDefaultDispatchHandlers at
	 * module startup. Iterates every auto-registered surface and forwards to its
	 * ``Register`` function with the shared dispatch queue and method-name accumulator.
	 *
	 * After all auto-register entries fire, ``OutMethodNames`` contains the union
	 * of every surface's registered method names. The bridge logs that total.
	 */
	void RegisterAll(FMCPDispatchQueue& Queue, TArray<FString>& OutMethodNames);

	/** For diagnostics (tools.list_surfaces, future). */
	const TArray<FEntry>& GetEntries() const { return Entries; }

private:
	TArray<FEntry> Entries;
};

/**
 * Static-initializer-driven surface registration. Place ONE line at TU scope at
 * the end of any tool surface .cpp:
 *
 *   MCP_REGISTER_SURFACE(MyTools, &FMyTools::Register)
 *
 * The first argument is the surface name (used purely for diagnostics — show up
 * in logs and future tools.list_surfaces output). The second is a function
 * pointer matching ``FMCPSurfaceRegistry::FRegisterFn`` — i.e. the surface
 * namespace's ``Register(FMCPDispatchQueue&, TArray<FString>&)`` function.
 *
 * Anonymous namespace + per-macro struct name avoids any symbol collision risk
 * even under unity builds.
 */
#define MCP_REGISTER_SURFACE(SurfaceName, RegisterFn)                                  \
	namespace                                                                          \
	{                                                                                  \
		struct F##SurfaceName##_MCPAutoRegistrar                                       \
		{                                                                              \
			F##SurfaceName##_MCPAutoRegistrar()                                        \
			{                                                                          \
				FMCPSurfaceRegistry::Get().Add(TEXT(#SurfaceName), (RegisterFn));      \
			}                                                                          \
		};                                                                             \
		static F##SurfaceName##_MCPAutoRegistrar G##SurfaceName##_MCPAutoRegistrar;    \
	}
