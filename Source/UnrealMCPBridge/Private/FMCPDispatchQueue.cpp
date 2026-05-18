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

void FMCPDispatchQueue::RegisterHandler(const FString& Method, FHandler&& Handler)
{
	check(!Method.IsEmpty());

	FScopeLock Lock(&HandlersLock);
	if (Handlers.Contains(Method))
	{
		UE_LOG(LogMCP, Warning, TEXT("Dispatch handler '%s' replaced (was registered, now overwritten)"), *Method);
	}
	Handlers.Add(Method, MoveTemp(Handler));
}

void FMCPDispatchQueue::UnregisterHandler(const FString& Method)
{
	FScopeLock Lock(&HandlersLock);
	Handlers.Remove(Method);
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
