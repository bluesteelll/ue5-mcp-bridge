// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "HAL/CriticalSection.h"
#include "MCPTypes.h"
#include "Templates/Function.h"

/**
 * Game-thread dispatch queue for inbound MCP requests.
 *
 * **Producer:** TCP worker threads (FMCPConnection::Run) call Push().
 * **Consumer:** game thread (FCoreDelegates::OnEndFrame) calls Drain() — pops every pending request,
 * routes it through the precedence chain below, runs the selected handler synchronously, and
 * ships the response back to the originating connection via FMCPServer::SendResponse.
 *
 * Dispatch precedence (per request, evaluated in order):
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
 * Handlers run on the GAME THREAD. They can safely touch UObjects, GWorld, asset registry, etc.
 * Long-running work MUST be promoted to a job — synchronous handlers are expected to be < 5 ms
 * each (Python wrapper-invoke overhead is currently ~1-2 ms per CallPythonTool).
 */
class UNREALMCPBRIDGE_API FMCPDispatchQueue
{
public:
	/** Method dispatch signature: takes the parsed Args object (may be null), returns a populated response. */
	using FHandler = TFunction<FMCPResponse(const FMCPRequest&)>;

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
	 * Register a synchronous game-thread handler for the given dotted method name.
	 * Replaces any existing handler with the same name (last-writer-wins; logs a warning on overwrite).
	 * Thread-safe — guarded by HandlersLock so it can be called during module bring-up while the
	 * listener is already accepting.
	 */
	void RegisterHandler(const FString& Method, FHandler&& Handler);

	/** Remove a handler. No-op if not registered. */
	void UnregisterHandler(const FString& Method);

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

	/** Method → handler table. Read on game thread (Drain), written from any thread (RegisterHandler). */
	TMap<FString, FHandler> Handlers;

	/** Fallback invoked when Method has no entry in Handlers. Day 3: Python registry dispatch. */
	FHandler UnknownMethodFallback;

	/** Kind=ExecPython handler. Day 3: FMCPPythonEval::EvalExpression. */
	FHandler ExecPythonHandler;

	mutable FCriticalSection HandlersLock;

	std::atomic<int64> EnqueuedCount{0};
	std::atomic<int64> DispatchedCount{0};
};
