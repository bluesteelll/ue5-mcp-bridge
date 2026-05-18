// Copyright FatumGame. All Rights Reserved.

#include "FMCPDispatchQueue.h"
#include "FMCPPythonEval.h"
#include "FMCPServer.h"
#include "MCPTypes.h"
#include "UnrealMCPBridge.h"

#include "HAL/IConsoleManager.h"
#include "Misc/OutputDevice.h"

namespace
{
	void HandleMCPStatus(FOutputDevice& Out)
	{
		FMCPServer& Server = FMCPServer::Get();
		FMCPDispatchQueue& Queue = FMCPDispatchQueue::Get();

		Out.Logf(TEXT("[MCP] listener=%s port=%d connections=%d total_accepted=%d enqueued=%lld dispatched=%lld python=%s"),
			Server.IsListening() ? TEXT("RUNNING") : TEXT("STOPPED"),
			Server.GetListenPort(),
			Server.GetConnectionCount(),
			Server.GetTotalAcceptedConnections(),
			static_cast<long long>(Queue.GetEnqueuedCount()),
			static_cast<long long>(Queue.GetDispatchedCount()),
			FMCPPythonEval::IsPythonReady() ? TEXT("READY") : TEXT("NOT_READY"));
	}

	void HandleMCPRestartListener(FOutputDevice& Out)
	{
		FMCPServer& Server = FMCPServer::Get();
		const int32 PreviousPort = Server.GetListenPort();
		const int32 RestartPort = PreviousPort > 0 ? PreviousPort : kMCPDefaultPort;

		Out.Logf(TEXT("[MCP] Restarting listener on port %d ..."), RestartPort);
		Server.Stop();

		FString Err;
		if (Server.Start(RestartPort, Err))
		{
			Out.Logf(TEXT("[MCP] Listener restarted on port %d"), RestartPort);
		}
		else
		{
			Out.Logf(ELogVerbosity::Error, TEXT("[MCP] Listener restart failed: %s"), *Err);
		}
	}

	void HandleMCPTools(FOutputDevice& Out)
	{
		// C++ handler names are stable + cheap to enumerate. Python tool names require a Python
		// roundtrip — skipped (with a notice) if Python isn't initialised yet.
		const TArray<FString> CppMethods = FMCPDispatchQueue::Get().GetRegisteredMethodNames();
		Out.Logf(TEXT("[MCP] Registered C++ method handlers (%d):"), CppMethods.Num());
		if (CppMethods.Num() == 0)
		{
			Out.Logf(TEXT("    (none — all tools served by Python registry fallback)"));
		}
		else
		{
			for (const FString& Name : CppMethods)
			{
				Out.Logf(TEXT("    cpp  %s"), *Name);
			}
		}

		if (!FMCPPythonEval::IsPythonReady())
		{
			Out.Logf(TEXT("[MCP] Python tools: NOT_READY (Python not yet initialised — try again after editor startup completes)"));
			return;
		}

		const TArray<FString> PythonTools = FMCPPythonEval::GetRegisteredPythonToolNames();
		Out.Logf(TEXT("[MCP] Registered Python @tool entries (%d):"), PythonTools.Num());
		if (PythonTools.Num() == 0)
		{
			Out.Logf(TEXT("    (none — verify MCPTools.tools.smoke_tools imported during bootstrap)"));
		}
		else
		{
			for (const FString& Name : PythonTools)
			{
				Out.Logf(TEXT("    py   %s"), *Name);
			}
		}
	}

	// Use the output-device variants so output reliably appears in the console panel.
	static FAutoConsoleCommandWithOutputDevice GMCPStatusCmd(
		TEXT("MCP.Status"),
		TEXT("Reports MCP bridge listener state: port, connection count, dispatched request count, Python readiness."),
		FConsoleCommandWithOutputDeviceDelegate::CreateStatic(&HandleMCPStatus));

	static FAutoConsoleCommandWithOutputDevice GMCPRestartListenerCmd(
		TEXT("MCP.RestartListener"),
		TEXT("Stops then restarts the MCP TCP listener on its currently-bound port (or default)."),
		FConsoleCommandWithOutputDeviceDelegate::CreateStatic(&HandleMCPRestartListener));

	static FAutoConsoleCommandWithOutputDevice GMCPToolsCmd(
		TEXT("MCP.Tools"),
		TEXT("Lists all dispatch targets — C++ handler-map entries plus Python @tool registry entries."),
		FConsoleCommandWithOutputDeviceDelegate::CreateStatic(&HandleMCPTools));
}
