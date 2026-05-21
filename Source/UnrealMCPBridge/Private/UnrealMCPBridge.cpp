// Copyright FatumGame. All Rights Reserved.

#include "UnrealMCPBridge.h"

#include "FMCPDay7Handlers.h"
#include "FMCPDispatchQueue.h"
#include "FMCPJobRegistry.h"
#include "FMCPLogStream.h"
#include "FMCPMarshalling.h"
#include "FMCPPythonBootstrap.h"
#include "FMCPPythonEval.h"
#include "FMCPServer.h"
#include "MCPTypes.h"
#include "Tools/ActorTools.h"
#include "Tools/AnimBlueprintTools.h"
#include "Tools/AnimTools.h"
#include "Tools/AssetCompositeTools.h"
#include "Tools/AssetRegistryTools.h"
#include "Tools/AudioTools.h"
#include "Tools/BlueprintComponentTools.h"
#include "Tools/BlueprintCompositeTools.h"
#include "Tools/BlueprintGraphTools.h"
#include "Tools/BlueprintTools.h"
#include "Tools/ComponentTools.h"
#include "Tools/ConfigTools.h"
#include "Tools/ContentBrowserTools.h"
#include "Tools/CookTools.h"
#include "Tools/CurveTools.h"
#include "Tools/DataTableTools.h"
#include "Tools/DataValidationTools.h"
#include "Tools/DebugTools.h"
#include "Tools/EditorTools.h"
#include "Tools/EngineTools.h"
#include "Tools/FolderTools.h"
#include "Tools/GameplayTagTools.h"
#include "Tools/HierarchyTools.h"
#include "Tools/InputTools.h"
#include "Tools/LevelCompositeTools.h"
#include "Tools/LevelStreamingTools.h"
#include "Tools/LevelTools.h"
#include "Tools/LiveCodingTools.h"
#include "Tools/LogTools.h"
#include "Tools/MaterialTools.h"
#include "Tools/MeshTools.h"
#include "Tools/NavMeshTools.h"
#include "Tools/NiagaraTools.h"
#include "Tools/PhysicsTools.h"
#include "Tools/PIETools.h"
#include "Tools/RenderTools.h"
#include "Tools/ScreenshotTools.h"
#include "Tools/SequencerTools.h"
#include "Tools/SourceControlCompositeTools.h"

// Wave I refactor 2026-05: auto-registration registry. Wave I+ surfaces use this
// instead of being listed individually here.
#include "MCPSurfaceRegistry.h"
#include "Tools/SourceControlTools.h"
#include "Tools/StatsTools.h"
#include "Tools/SubsystemTools.h"
#include "Tools/TestCompositeTools.h"
#include "Tools/TestTools.h"
#include "Tools/TextureTools.h"
#include "Tools/ThumbnailTools.h"
#include "Tools/TransformTools.h"
#include "Tools/UFunctionTools.h"
#include "Tools/UMGTools.h"
#include "Tools/ViewportTools.h"
#include "Tools/WorldPartitionTools.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/CoreDelegates.h"

DEFINE_LOG_CATEGORY(LogMCP);

void FUnrealMCPBridgeModule::StartupModule()
{
	UE_LOG(LogMCP, Log, TEXT("MCP bridge module starting (Phase 1 Day 7 — async jobs + log streaming + tools.list)"));

	// 1. Open the TCP listener. Failure here does not abort module load; we log + continue so the
	//    user can MCP.RestartListener after fixing the port conflict.
	FString StartErr;
	if (!FMCPServer::Get().Start(kMCPDefaultPort, StartErr))
	{
		UE_LOG(LogMCP, Warning,
			TEXT("MCP bridge listener failed to start: %s. Use 'MCP.RestartListener' after resolving the conflict."),
			*StartErr);
	}

	// 2. Attach the GLog ring-buffer capture BEFORE we log anything else from this point on, so
	//    the buffer reflects the full module-startup history. AddOutputDevice is idempotent
	//    (see FMCPLogStream::Attach).
	FMCPLogStream::Get().Attach();

	// 3. Register Python eval bridge handlers. These self-check IsPythonInitialized() and return
	//    structured -32603 errors if Python isn't up, so installing eagerly (before Python init)
	//    is safe — early requests just get a deterministic "python not initialised" surface.
	//
	//    Dispatch precedence is:
	//        (kind=ExecPython?) → ExecPythonHandler
	//        else (Method ∈ C++ handler map?) → C++ handler  (marshall.*, job.*, log.*, tools.list)
	//        else (UnknownMethodFallback installed?) → Python registry lookup
	//        else → -32601 method not found
	RegisterDefaultDispatchHandlers();

	// 4. Hook the OnEndFrame drain.
	OnEndFrameHandle = FCoreDelegates::OnEndFrame.AddRaw(this, &FUnrealMCPBridgeModule::OnEndFrame);

	// 5. Python sys.path bootstrap — fires immediately if Python is already up, else queues.
	FMCPPythonBootstrap::RegisterPythonInitCallback();

	bStarted = true;
	UE_LOG(LogMCP, Log, TEXT("MCP bridge module ready (listener=%s port=%d, log_stream=%s)"),
		FMCPServer::Get().IsListening() ? TEXT("RUNNING") : TEXT("STOPPED"),
		FMCPServer::Get().GetListenPort(),
		FMCPLogStream::Get().GetEntryCount() > 0 ? TEXT("ATTACHED") : TEXT("ATTACHED_EMPTY"));
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

	// Stop accepting new socket traffic FIRST so no fresh job.submit calls land while we're tearing
	// the registry down.
	FMCPServer::Get().Stop();

	// Cancel any in-flight async jobs + destroy the worker pool. Bodies that don't honour cancel
	// will still run to completion — pool->Destroy() blocks. That's intentional (forced abort of
	// in-progress Python could corrupt the interpreter).
	FMCPJobRegistry::Get().Shutdown();

	// Detach log capture LAST so any teardown logs from Job registry / server get into the ring
	// for the final flush (cosmetic — by now nobody can query log.tail anyway).
	FMCPLogStream::Get().Detach();

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
	auto RegisterHandler = [this](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe = false)
	{
		FMCPDispatchQueue::Get().RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		RegisteredMethodNames.Add(MethodName);
	};
	RegisterHandler(TEXT("marshall.list_properties"), &FMCPMarshalling::ListProperties);
	RegisterHandler(TEXT("marshall.read_property"),   &FMCPMarshalling::ReadProperty);
	RegisterHandler(TEXT("marshall.write_property"),  &FMCPMarshalling::WriteProperty);
	RegisterHandler(TEXT("marshall.describe_struct"), &FMCPMarshalling::DescribeStruct);

	// Day 7: async job system (5 handlers). Bodies dispatch to a separate worker pool — the
	// handlers themselves are synchronous and bounded.
	// HOTFIX 2 (2026-05): job.status + job.result registered Lane B so Python composites that
	// submit jobs from inside the game thread (via dispatch_internal TCP loopback) can poll
	// without deadlocking the game thread on socket.recv. Both handlers only read FMCPJobRegistry
	// state under its internal lock — see the Lane B promotion notes in FMCPDay7Handlers.cpp.
	// job.submit / job.cancel / job.list_active remain Lane A: submit creates a job (writes the
	// jobs map under the same lock — safe in isolation, but conservatively Lane A because the
	// SubmitJob path also calls EnsureInitialized which spins up the worker pool the first time
	// and that pool init should run on the game thread for predictability). list_active is rarely
	// hot-pathed by composites.
	RegisterHandler(TEXT("job.submit"),      &FMCPDay7Handlers::JobSubmit);
	RegisterHandler(TEXT("job.status"),      &FMCPDay7Handlers::JobStatus,      /*bThreadSafe*/ true);
	RegisterHandler(TEXT("job.result"),      &FMCPDay7Handlers::JobResult,      /*bThreadSafe*/ true);
	RegisterHandler(TEXT("job.cancel"),      &FMCPDay7Handlers::JobCancel);
	RegisterHandler(TEXT("job.list_active"), &FMCPDay7Handlers::JobListActive);

	// Day 7: log streaming (3 handlers). FMCPLogStream is attached to GLog at module startup so
	// every log line since then is queryable via these handlers.
	RegisterHandler(TEXT("log.tail"),      &FMCPDay7Handlers::LogTail);
	RegisterHandler(TEXT("log.subscribe"), &FMCPDay7Handlers::LogSubscribe);
	RegisterHandler(TEXT("log.search"),    &FMCPDay7Handlers::LogSearch);

	// Day 7: first-class tools.list verb. Replaces mcp_server's exec_python ferry — returns
	// {python_tools: {meta...}, cpp_handlers: [names]} in a single round-trip.
	RegisterHandler(TEXT("tools.list"), &FMCPDay7Handlers::ToolsList);

	// Wave I refactor 2026-05 — auto-registered surfaces via FMCPSurfaceRegistry.
	// Surfaces declare themselves via MCP_REGISTER_SURFACE() at TU scope in their own .cpp
	// (Wave I onward — package / soft_ref / collision / mat_inst / sequencer_ext / landscape
	// + every future wave). This eliminates UnrealMCPBridge.cpp as the per-surface coordination
	// bottleneck — parallel-agent waves can ship N surfaces concurrently without any conflict
	// on this file. The registry iterates entries and forwards each surface's Register() with
	// the shared dispatch queue + accumulator. See MCPSurfaceRegistry.h for the full rationale.
	FMCPSurfaceRegistry::Get().RegisterAll(FMCPDispatchQueue::Get(), RegisteredMethodNames);

	UE_LOG(LogMCP, Log,
		TEXT("Registered dispatch handlers: kind=ExecPython, unknown-method-fallback, ")
		TEXT("marshall.* + job.* + log.* + tools.list + 63 auto-registered surfaces via FMCPSurfaceRegistry"));
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
