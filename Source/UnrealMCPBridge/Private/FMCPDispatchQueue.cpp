// Copyright FatumGame. All Rights Reserved.

#include "FMCPDispatchQueue.h"

#include "FMCPServer.h"
#include "UnrealMCPBridge.h"

FMCPDispatchQueue& FMCPDispatchQueue::Get()
{
	static FMCPDispatchQueue Instance;
	return Instance;
}

void FMCPDispatchQueue::Push(FMCPRequest&& Request)
{
	EnqueuedCount.fetch_add(1, std::memory_order_relaxed);
	InboundQueue.Enqueue(MoveTemp(Request));
}

void FMCPDispatchQueue::RegisterHandler(const FString& Method, FHandler&& Handler, bool bThreadSafe)
{
	check(!Method.IsEmpty());

	FScopeLock Lock(&HandlersLock);
	if (Handlers.Contains(Method))
	{
		UE_LOG(LogMCP, Warning, TEXT("Dispatch handler '%s' replaced (was registered, now overwritten)"), *Method);
	}
	// Add/replace BOTH maps under the same lock so a concurrent IsThreadSafe / DispatchInline
	// on the listener thread never observes a handler entry without its thread-safety flag (or
	// vice versa). The two maps share a single key set by construction.
	Handlers.Add(Method, MoveTemp(Handler));
	MethodThreadSafety.Add(Method, bThreadSafe);
}

void FMCPDispatchQueue::UnregisterHandler(const FString& Method)
{
	FScopeLock Lock(&HandlersLock);
	Handlers.Remove(Method);
	MethodThreadSafety.Remove(Method);
}

bool FMCPDispatchQueue::IsThreadSafe(const FString& Method) const
{
	FScopeLock Lock(&HandlersLock);
	// FindRef returns the value-typed default (false) when Method isn't present, which is exactly
	// the "not Lane B" semantic we want for both unregistered methods and Lane A handlers.
	return MethodThreadSafety.FindRef(Method);
}

FMCPResponse FMCPDispatchQueue::DispatchInline(const FMCPRequest& Request) const
{
	// Mirror Drain's lock-then-copy-then-invoke pattern so a long-running Lane B handler does NOT
	// serialise with concurrent RegisterHandler calls from module bring-up. We deliberately do
	// NOT inspect ExecPythonHandler / UnknownMethodFallback here — Lane B is restricted to
	// explicitly-registered Method handlers; ExecPython and the Python fallback are game-thread-only
	// for safety (Python interp lock + UObject access).
	FHandler HandlerCopy;
	bool bHandlerFound = false;
	{
		FScopeLock Lock(&HandlersLock);
		if (const FHandler* Found = Handlers.Find(Request.Method))
		{
			HandlerCopy = *Found;
			bHandlerFound = true;
		}
	}

	FMCPResponse Response;
	Response.RequestId = Request.RequestId;
	Response.OriginalIdString = Request.OriginalIdString;

	if (!bHandlerFound)
	{
		// Lane B caller (FMCPConnection) only enters this path when IsThreadSafe(Method)==true, so
		// in practice the handler is virtually always present. The race window is
		// concurrent-UnregisterHandler between IsThreadSafe and Find — vanishingly rare in
		// production (handlers are registered once at module startup) but we surface it cleanly.
		UE_LOG(LogMCP, Warning,
			TEXT("Dispatch (inline): no handler for method '%s' (conn=%d, id=%s) — racing unregister?"),
			*Request.Method, Request.SourceConnectionId, *Request.OriginalIdString);
		return MakeMethodNotFoundError(Request);
	}

	Response = HandlerCopy(Request);
	// Defensive re-stamp — a buggy handler that mutates response ids cannot desync the round-trip.
	Response.RequestId = Request.RequestId;
	Response.OriginalIdString = Request.OriginalIdString;
	DispatchedCount.fetch_add(1, std::memory_order_relaxed);
	return Response;
}

void FMCPDispatchQueue::SetUnknownMethodFallback(FHandler&& Fallback)
{
	FScopeLock Lock(&HandlersLock);
	if (UnknownMethodFallback)
	{
		UE_LOG(LogMCP, Verbose, TEXT("Dispatch: unknown-method fallback replaced"));
	}
	UnknownMethodFallback = MoveTemp(Fallback);
}

void FMCPDispatchQueue::SetExecPythonHandler(FHandler&& Handler)
{
	FScopeLock Lock(&HandlersLock);
	if (ExecPythonHandler)
	{
		UE_LOG(LogMCP, Verbose, TEXT("Dispatch: exec_python handler replaced"));
	}
	ExecPythonHandler = MoveTemp(Handler);
}

TArray<FString> FMCPDispatchQueue::GetRegisteredMethodNames() const
{
	TArray<FString> Names;
	FScopeLock Lock(&HandlersLock);
	Names.Reserve(Handlers.Num());
	for (const TPair<FString, FHandler>& Pair : Handlers)
	{
		Names.Add(Pair.Key);
	}
	return Names;
}

void FMCPDispatchQueue::Drain()
{
	check(IsInGameThread());

	FMCPRequest Request;
	while (InboundQueue.Dequeue(Request))
	{
		// Resolve all candidate handlers under lock, then drop the lock so the handler body
		// (which may be long-running, e.g. Python eval) does NOT serialise with concurrent
		// RegisterHandler calls from the listener thread.
		FHandler HandlerCopy;
		FHandler FallbackCopy;
		FHandler ExecPythonCopy;
		bool bHandlerFound = false;
		{
			FScopeLock Lock(&HandlersLock);
			if (Request.Kind == EMCPRequestKind::ExecPython)
			{
				if (ExecPythonHandler)
				{
					ExecPythonCopy = ExecPythonHandler;
				}
			}
			else if (const FHandler* Found = Handlers.Find(Request.Method))
			{
				HandlerCopy = *Found;
				bHandlerFound = true;
			}
			else if (UnknownMethodFallback)
			{
				FallbackCopy = UnknownMethodFallback;
			}
		}

		FMCPResponse Response;
		Response.RequestId = Request.RequestId;
		Response.OriginalIdString = Request.OriginalIdString;

		if (ExecPythonCopy)
		{
			// kind=ExecPython routes directly to the registered Python evaluator. Method is
			// irrelevant here — the source is in Request.Args->"expression".
			Response = ExecPythonCopy(Request);
			Response.RequestId = Request.RequestId;
			Response.OriginalIdString = Request.OriginalIdString;
			DispatchedCount.fetch_add(1, std::memory_order_relaxed);
		}
		else if (Request.Kind == EMCPRequestKind::ExecPython)
		{
			// Client asked for ExecPython but the handler isn't installed (Python not available).
			Response.bIsError = true;
			Response.ErrorCode = -32603;
			Response.ErrorMessage = TEXT("exec_python handler not installed (python unavailable?)");
			UE_LOG(LogMCP, Warning, TEXT("Dispatch: exec_python handler missing (conn=%d, id=%s)"),
				Request.SourceConnectionId, *Request.OriginalIdString);
		}
		else if (bHandlerFound)
		{
			Response = HandlerCopy(Request);
			// Defensive — handler body MUST preserve ids; we re-stamp both fields so a buggy
			// handler can't desync the response with the originating request.
			Response.RequestId = Request.RequestId;
			Response.OriginalIdString = Request.OriginalIdString;
			DispatchedCount.fetch_add(1, std::memory_order_relaxed);
		}
		else if (FallbackCopy)
		{
			// Fallback is currently FMCPPythonEval::CallPythonTool — routes to Python registry.
			// We treat a successful fallback dispatch the same as a C++ handler (counter bumps).
			Response = FallbackCopy(Request);
			Response.RequestId = Request.RequestId;
			Response.OriginalIdString = Request.OriginalIdString;
			DispatchedCount.fetch_add(1, std::memory_order_relaxed);
		}
		else
		{
			Response = MakeMethodNotFoundError(Request);
			UE_LOG(LogMCP, Warning, TEXT("Dispatch: no handler for method '%s' (conn=%d, id=%s)"),
				*Request.Method, Request.SourceConnectionId, *Request.OriginalIdString);
		}

		// Route response back to the originating connection. -1 means "no source" (synthetic / test).
		if (Request.SourceConnectionId != INDEX_NONE)
		{
			FMCPServer::Get().SendResponse(Request.SourceConnectionId, Response);
		}
	}
}

FMCPResponse FMCPDispatchQueue::MakeMethodNotFoundError(const FMCPRequest& Request)
{
	FMCPResponse Response;
	Response.RequestId = Request.RequestId;
	Response.OriginalIdString = Request.OriginalIdString;
	Response.bIsError = true;
	Response.ErrorCode = -32601; // JSON-RPC: Method not found
	Response.ErrorMessage = FString::Printf(TEXT("method not found: %s"), *Request.Method);
	return Response;
}
