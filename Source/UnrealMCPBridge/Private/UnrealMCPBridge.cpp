// Copyright FatumGame. All Rights Reserved.

#include "UnrealMCPBridge.h"

#include "FMCPDispatchQueue.h"
#include "FMCPMarshalling.h"
#include "FMCPPythonBootstrap.h"
#include "FMCPPythonEval.h"
#include "FMCPServer.h"
#include "MCPTypes.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/CoreDelegates.h"

DEFINE_LOG_CATEGORY(LogMCP);

void FUnrealMCPBridgeModule::StartupModule()
{
	UE_LOG(LogMCP, Log, TEXT("MCP bridge module starting (Phase 1 Day 3 — Python tool dispatch)"));

	// 1. Open the TCP listener. Failure here does not abort module load; we log + continue so the
	//    user can MCP.RestartListener after fixing the port conflict.
	FString StartErr;
	if (!FMCPServer::Get().Start(kMCPDefaultPort, StartErr))
	{
		UE_LOG(LogMCP, Warning,
			TEXT("MCP bridge listener failed to start: %s. Use 'MCP.RestartListener' after resolving the conflict."),
			*StartErr);
	}

	// 2. Register Python eval bridge handlers. These self-check IsPythonInitialized() and return
	//    structured -32603 errors if Python isn't up, so installing eagerly (before Python init)
	//    is safe — early requests just get a deterministic "python not initialised" surface.
	//
	//    Day 2 had a hard-coded C++ editor.ping handler here; Day 3 deletes it because Python
	//    serves editor.ping via the @tool decorator in smoke_tools.py. Dispatch precedence is:
	//        (kind=ExecPython?) → ExecPythonHandler
	//        else (Method ∈ C++ handler map?) → C++ handler
	//        else (UnknownMethodFallback installed?) → Python registry lookup
	//        else → -32601 method not found
	RegisterDefaultDispatchHandlers();

	// 3. Hook the OnEndFrame drain.
	OnEndFrameHandle = FCoreDelegates::OnEndFrame.AddRaw(this, &FUnrealMCPBridgeModule::OnEndFrame);

	// 4. Python sys.path bootstrap — fires immediately if Python is already up, else queues.
	FMCPPythonBootstrap::RegisterPythonInitCallback();

	bStarted = true;
	UE_LOG(LogMCP, Log, TEXT("MCP bridge module ready (listener=%s port=%d)"),
		FMCPServer::Get().IsListening() ? TEXT("RUNNING") : TEXT("STOPPED"),
		FMCPServer::Get().GetListenPort());
}

void FUnrealMCPBridgeModule::ShutdownModule()
{
	UE_LOG(LogMCP, Log, TEXT("MCP bridge module shutting down"));

	if (OnEndFrameHandle.IsValid())
	{
		FCoreDelegates::OnEndFrame.Remove(OnEndFrameHandle);
		OnEndFrameHandle.Reset();
	}

	UnregisterDefaultDispatchHandlers();

	// Tear the listener down LAST so any in-flight response from a Drain caught before us can still
	// be sent (defensive — we already unregistered OnEndFrame so this is mostly cosmetic).
	FMCPServer::Get().Stop();

	bStarted = false;
}

bool FUnrealMCPBridgeModule::IsListening() const
{
	return FMCPServer::Get().IsListening();
}

int32 FUnrealMCPBridgeModule::GetListenPort() const
{
	return FMCPServer::Get().GetListenPort();
}

int64 FUnrealMCPBridgeModule::GetDispatchedRequestCount() const
{
	return FMCPDispatchQueue::Get().GetDispatchedCount();
}

void FUnrealMCPBridgeModule::OnEndFrame()
{
	// Pumps both: drains the dispatch queue (handlers run + responses sent), then GCs closed sockets.
	FMCPDispatchQueue::Get().Drain();
	FMCPServer::Get().GarbageCollectClosedConnections();
}

void FUnrealMCPBridgeModule::RegisterDefaultDispatchHandlers()
{
	// kind=ExecPython → arbitrary Python expression evaluator.
	// The wrapper reads Request.Args->"expression"; missing/empty produces -32602.
	FMCPDispatchQueue::Get().SetExecPythonHandler(
		[](const FMCPRequest& Req) -> FMCPResponse
		{
			FString Expression;
			if (Req.Args.IsValid())
			{
				Req.Args->TryGetStringField(TEXT("expression"), Expression);
			}
			if (Expression.IsEmpty())
			{
				FMCPResponse Err;
				Err.RequestId = Req.RequestId;
				Err.OriginalIdString = Req.OriginalIdString;
				Err.bIsError = true;
				Err.ErrorCode = -32602;
				Err.ErrorMessage = TEXT("exec_python requires args.expression (string)");
				return Err;
			}
			return FMCPPythonEval::EvalExpression(Req, Expression);
		});

	// Unknown-method fallback → Python registry. Once installed, ANY unmatched CallFunction
	// request gets routed to MCPTools.registry.get_tool(method). The handler itself emits a
	// JSON-RPC -32601 if the tool isn't registered Python-side either.
	FMCPDispatchQueue::Get().SetUnknownMethodFallback(
		[](const FMCPRequest& Req) -> FMCPResponse
		{
			return FMCPPythonEval::CallPythonTool(Req);
		});

	// Day 4-5: Tier 2 marshalling handlers. Registered as C++ handlers (not Python tools) for
	// two reasons: (a) FProperty reflection isn't exposed to Python, and (b) C++ dispatch is
	// ~2-3 ms faster per call (no wrapper script + base64 round-trip). These names take
	// precedence over the Python fallback for ``marshall.*`` — see dispatch order in
	// FMCPDispatchQueue::Drain. FHandler is a TFunction so we can pass the static method pointer
	// directly — TFunction's converting ctor wraps it.
	auto RegisterMarshall = [this](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler)
	{
		FMCPDispatchQueue::Get().RegisterHandler(MethodName, MoveTemp(Handler));
		RegisteredMethodNames.Add(MethodName);
	};
	RegisterMarshall(TEXT("marshall.list_properties"), &FMCPMarshalling::ListProperties);
	RegisterMarshall(TEXT("marshall.read_property"),   &FMCPMarshalling::ReadProperty);
	RegisterMarshall(TEXT("marshall.write_property"),  &FMCPMarshalling::WriteProperty);
	RegisterMarshall(TEXT("marshall.describe_struct"), &FMCPMarshalling::DescribeStruct);

	UE_LOG(LogMCP, Log,
		TEXT("Registered dispatch handlers: kind=ExecPython → FMCPPythonEval::EvalExpression, ")
		TEXT("unknown-method-fallback → FMCPPythonEval::CallPythonTool, ")
		TEXT("C++ handlers → marshall.list_properties / read_property / write_property / describe_struct"));
}

void FUnrealMCPBridgeModule::UnregisterDefaultDispatchHandlers()
{
	// Drop the Python eval handlers + any explicit C++ method handlers we accumulated.
	FMCPDispatchQueue::Get().SetExecPythonHandler({});
	FMCPDispatchQueue::Get().SetUnknownMethodFallback({});

	for (const FString& Method : RegisteredMethodNames)
	{
		FMCPDispatchQueue::Get().UnregisterHandler(Method);
	}
	RegisteredMethodNames.Reset();
}

IMPLEMENT_MODULE(FUnrealMCPBridgeModule, UnrealMCPBridge)
