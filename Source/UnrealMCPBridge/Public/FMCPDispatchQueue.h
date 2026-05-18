// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "HAL/CriticalSection.h"
#include "MCPTypes.h"
#include "Templates/Function.h"

/**
 * Dispatch queue for inbound MCP requests.
 *
 * **Producer:** TCP worker threads (FMCPConnection::Run) call Push() (Lane A) OR call DispatchInline()
 * directly when the method is registered as Lane B (Phase 2 Day 0).
 *
 * **Consumer (Lane A):** game thread (FCoreDelegates::OnEndFrame) calls Drain() — pops every pending
 * request, routes it through the precedence chain below, runs the selected handler synchronously,
 * and ships the response back to the originating connection via FMCPServer::SendResponse.
 *
 * **Consumer (Lane B):** the TCP listener thread itself invokes DispatchInline() for handlers
 * registered with `bThreadSafe=true`, then sends the response directly on the connection. The
 * game-thread Drain queue is bypassed entirely. See the Lane B contract block in the public
 * section below for the rules Lane B handlers MUST follow.
 *
 * Dispatch precedence (Lane A, per request, evaluated in order):
 *   1. Request.Kind == EMCPRequestKind::ExecPython → ExecPythonHandler (Day 3: FMCPPythonEval::EvalExpression).
 *   2. CallFunction with Method in Handlers map  → that explicit C++ handler.
 *   3. CallFunction otherwise, UnknownMethodFallback installed → fallback (Day 3: Python registry).
 *   4. Else                                       → JSON-RPC -32601 method-not-found error.
 *
 * Phase 1 Day 3 scope:
 * - Python @tool dispatch via the unknown-method fallback (MCPTools.registry.get_tool).
 * - kind=ExecPython evaluates arbitrary single-statement Python expressions.
 * - No async / job system yet (Day 5+).
 *
 * Lane A handlers run on the GAME THREAD. They can safely touch UObjects, GWorld, asset registry, etc.
 * Long-running work MUST be promoted to a job — synchronous handlers are expected to be < 5 ms
 * each (Python wrapper-invoke overhead is currently ~1-2 ms per CallPythonTool).
 */
class UNREALMCPBRIDGE_API FMCPDispatchQueue
{
public:
	/** Method dispatch signature: takes the parsed Args object (may be null), returns a populated response. */
	using FHandler = TFunction<FMCPResponse(const FMCPRequest&)>;

	// ─── Lane B contract (Phase 2 Day 0) ────────────────────────────────────────────────────────
	//
	// Handlers registered with `bThreadSafe=true` (Lane B) run on the TCP listener thread that
	// received the request, NOT on the game thread. This bypasses the OnEndFrame Drain queue and
	// removes the per-call ~16ms tick-quantization latency, but imposes strict rules:
	//
	// Lane B handlers MUST NOT:
	//   - Touch UObjects (no LoadObject, no FindObject, no GetClass()/GetOuter() walks)
	//   - Touch GWorld, GEngine, or any UEngineSubsystem / UEditorSubsystem
	//   - Modify any state (no Set*, no AR mutations, no editor commands, no UPackage::SetDirty)
	//   - Allocate persistent UObjects or call NewObject
	//   - Hold the GC lock or interact with FUObjectGlobals
	//
	// Lane B handlers MAY:
	//   - Call IAssetRegistry::Get()->GetAssets, GetReferencers, GetDependencies, GetSubPaths,
	//     IsLoadingAssets, GetAssetByObjectPath, GetAssetsByTags, GetAssetsByTagValues, GetAssetsByPath
	//     (the AR read API is documented thread-safe since UE 5.0)
	//   - Call FPackageName::DoesPackageExist (filesystem-only)
	//   - Call IFileManager::FileSize
	//   - Perform pure math / string / JSON serialization
	//
	// If a tool's Lane B audit (lane_b_spike.py) fails — sporadic asserts, crashes, or any
	// non-NOT_REGISTERED failure across 1000 hot-loop iterations — DEMOTE the tool by passing
	// `bThreadSafe=false` to RegisterHandler. The infrastructure stays in place; only the per-tool
	// flag changes.
	//
	// Phase 1 callers (marshall.*, job.*, log.*, tools.list) continue to use the default
	// `bThreadSafe=false` overload — they remain Lane A and run on the game thread unchanged.
	//
	// **Known UE 5.7 limitation (2026-05):** AssetRegistry read APIs (`GetAssets`, `GetReferencers`,
	// etc.) assert `IsInGameThread()` internally when enumerating in-memory assets via
	// `GetAssetRegistryTags()` — Epic's own comment at `AssetRegistry.cpp:2906` reads
	// "Enumerating in-memory assets can only be done on the game thread or in the loader,
	// there are too many GetAssetRegistryTags() still not thread-safe." Lane B handlers using AR
	// enumeration queries crash on the TCP listener thread. **Phase 2 ships ALL AR tools as
	// Lane A (`bThreadSafe=false`) until this is resolved.** Single-point AR queries
	// (`GetAssetByObjectPath`) appear safe; only the enumerating bulk-query path triggers the
	// assert. Future workaround: passing `Filter.bIncludeOnlyOnDiskAssets=true` to `GetAssets()`
	// may bypass the path that hits the assert. Phase 3+ should retry Lane B with that mitigation
	// after stand-alone verification. The Lane B router (`IsThreadSafe` + `DispatchInline` +
	// `FMCPConnection` short-circuit) remains fully functional — the demotion is per-tool, not
	// infrastructural.

	/** Singleton accessor. The instance lives for the lifetime of the bridge module. */
	static FMCPDispatchQueue& Get();

	/**
	 * Producer-side: enqueue a request from any thread (typically a TCP worker).
	 * The request is moved into the queue. Caller MUST have set RequestId / SourceConnectionId
	 * before calling — Drain() relies on both for response routing.
	 */
	void Push(FMCPRequest&& Request);

	/**
	 * Game-thread sink: pop ALL pending requests, dispatch each by Method, and send responses.
	 * Called from FCoreDelegates::OnEndFrame. If a handler throws/asserts the caller is responsible
	 * for translating to an error response — currently we just rely on UE's assertion handler.
	 */
	void Drain();

	/**
	 * Register a synchronous handler for the given dotted method name.
	 * Replaces any existing handler with the same name (last-writer-wins; logs a warning on overwrite).
	 * Thread-safe — guarded by HandlersLock so it can be called during module bring-up while the
	 * listener is already accepting.
	 *
	 * @param Method        Dotted method name (e.g. "asset.exists"). Must be non-empty.
	 * @param Handler       Body invoked with the parsed request. Returns the response payload.
	 * @param bThreadSafe   When true, the handler is eligible for Lane B execution on the TCP
	 *                      listener thread (bypasses the game-thread Drain queue). See the Lane B
	 *                      contract block above for the rules the handler MUST follow. Default
	 *                      false preserves Phase 1 behavior — handler always runs on game thread
	 *                      via Drain().
	 */
	void RegisterHandler(const FString& Method, FHandler&& Handler, bool bThreadSafe = false);

	/** Remove a handler. No-op if not registered. Also clears the per-method thread-safety flag. */
	void UnregisterHandler(const FString& Method);

	/**
	 * Returns true iff Method was registered with bThreadSafe=true (Lane B). Returns false for
	 * unregistered methods. Thread-safe (read under HandlersLock). Called by FMCPConnection on
	 * the listener thread to decide between inline dispatch and game-thread enqueue.
	 */
	bool IsThreadSafe(const FString& Method) const;

	/**
	 * Synchronous dispatch on the calling thread. Used by FMCPConnection to invoke Lane B
	 * handlers inline on the listener thread, bypassing the game-thread Drain queue entirely.
	 *
	 * Behavior mirrors a single iteration of Drain()'s body for a CallFunction request:
	 *   - Look up Handlers[Method] under HandlersLock, copy the handler out, release the lock
	 *     before invoking (so a long-running handler does not block concurrent RegisterHandler
	 *     calls from other threads).
	 *   - Invoke the handler, re-stamp RequestId / OriginalIdString on the response.
	 *   - Bump DispatchedCount exactly like Drain does (telemetry parity).
	 *   - If no handler is registered, return a -32601 method-not-found error.
	 *
	 * The caller is responsible for sending the response back to the client. ExecPython / the
	 * unknown-method fallback are NOT invoked from here — they remain game-thread-only via Drain.
	 */
	FMCPResponse DispatchInline(const FMCPRequest& Request) const;

	/**
	 * Install a fallback invoked by Drain() when no C++ handler claims Method for a CallFunction
	 * request.
	 *
	 * Day 3 use: the bridge module installs FMCPPythonEval::CallPythonTool here so unknown
	 * methods are routed to the Python registry before we emit -32601. Pass a
	 * default-constructed FHandler to clear the fallback. Only one fallback may be installed
	 * at a time; re-registering replaces (logged at Verbose). Thread-safe — guarded by
	 * HandlersLock.
	 */
	void SetUnknownMethodFallback(FHandler&& Fallback);

	/**
	 * Install the kind=ExecPython handler. Invoked by Drain() for requests where
	 * Request.Kind == EMCPRequestKind::ExecPython BEFORE the Method-based handler-map lookup.
	 *
	 * The handler receives the raw FMCPRequest — it reads the Python source from
	 * Request.Args->"expression" and returns the eval result. Day 3 binds
	 * FMCPPythonEval::EvalExpression here.
	 *
	 * Pass a default-constructed FHandler to clear. Re-registering replaces (logged at Verbose).
	 * Thread-safe.
	 */
	void SetExecPythonHandler(FHandler&& Handler);

	/** Diagnostic counter — total requests successfully dispatched since process start. */
	int64 GetDispatchedCount() const { return DispatchedCount.load(std::memory_order_relaxed); }

	/** Diagnostic counter — total requests enqueued (including unknown methods). */
	int64 GetEnqueuedCount() const { return EnqueuedCount.load(std::memory_order_relaxed); }

	/** Snapshot copy of currently-registered C++ method names. Allocates — call on demand only. */
	TArray<FString> GetRegisteredMethodNames() const;

private:
	FMCPDispatchQueue() = default;
	~FMCPDispatchQueue() = default;
	FMCPDispatchQueue(const FMCPDispatchQueue&) = delete;
	FMCPDispatchQueue& operator=(const FMCPDispatchQueue&) = delete;

	/** Build a `method not found` error response. Caller sets RequestId. */
	static FMCPResponse MakeMethodNotFoundError(const FMCPRequest& Request);

	TQueue<FMCPRequest, EQueueMode::Mpsc> InboundQueue;

	/** Method → handler table. Read on game thread (Drain) AND listener thread (DispatchInline);
	 *  written from any thread (RegisterHandler). All accesses guarded by HandlersLock. */
	TMap<FString, FHandler> Handlers;

	/**
	 * Method → Lane B eligibility. Same key set as Handlers (entry added/removed in lockstep
	 * inside RegisterHandler / UnregisterHandler). Read by FMCPConnection on the listener thread
	 * via IsThreadSafe() to decide between Lane B inline dispatch and Lane A Push().
	 */
	TMap<FString, bool> MethodThreadSafety;

	/** Fallback invoked when Method has no entry in Handlers. Day 3: Python registry dispatch. */
	FHandler UnknownMethodFallback;

	/** Kind=ExecPython handler. Day 3: FMCPPythonEval::EvalExpression. */
	FHandler ExecPythonHandler;

	mutable FCriticalSection HandlersLock;

	// `mutable` because DispatchInline() is const (it's a pure dispatch query from the caller's
	// perspective) but still needs to bump the diagnostic counter for parity with Drain. The
	// counter is observable only through GetDispatchedCount() — not part of the public state.
	std::atomic<int64> EnqueuedCount{0};
	mutable std::atomic<int64> DispatchedCount{0};
};
