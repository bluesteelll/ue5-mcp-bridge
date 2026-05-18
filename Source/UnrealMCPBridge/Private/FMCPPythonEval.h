// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

/**
 * Game-thread Python evaluator bridge for Phase 1 Day 3.
 *
 * Provides two entry points consumed by FMCPDispatchQueue::Drain:
 *
 *   1. EvalExpression — runs an arbitrary Python expression via EvaluateStatement mode and
 *      returns its repr() string as the response result. Used by kind=ExecPython.
 *
 *   2. CallPythonTool — generates a small Python wrapper script that looks up the tool in
 *      MCPTools.registry, calls it with the JSON-decoded args, and emits a marker-bracketed
 *      JSON result via unreal.log(). The C++ side scans FPythonCommandEx::LogOutput for the
 *      marker pair and parses the inner JSON. Used by kind=CallFunction when no C++ handler
 *      claims the method.
 *
 * Threading:
 *   ALL methods MUST be called from the game thread. IPythonScriptPlugin::ExecPythonCommandEx
 *   takes the GIL itself, but the surrounding plumbing assumes game-thread context (Python
 *   globals are torn down on editor shutdown from the game thread). Drain() runs OnEndFrame
 *   which satisfies this requirement.
 *
 * Failure surfaces (all return structured FMCPResponse{bIsError=true}):
 *   - Python not initialised      -> -32603 "python not initialised"
 *   - ExecPythonCommandEx returns false -> -32603 "python eval failed" + log entries appended
 *   - Tool name not registered    -> -32601 "tool not found"
 *   - Tool raised an exception    -> -32002 "<exception message>" (full traceback logged to LogMCP)
 *   - Marker missing from output  -> -32603 "python wrapper produced no result marker"
 *   - Marker payload not JSON     -> -32603 "python wrapper result was not valid JSON"
 */
class FMCPPythonEval
{
public:
	/** True iff IPythonScriptPlugin::IsPythonInitialized(). Cheap; safe to call every Drain iteration. */
	static bool IsPythonReady();

	/**
	 * Evaluate an arbitrary Python expression and return its repr() as a JSON string result.
	 * The expression MUST be a single statement (Python's eval grammar — no `import`, no `def`).
	 * For multi-statement scripts use a future kind=ExecPythonScript (Day 6+).
	 */
	static FMCPResponse EvalExpression(const FMCPRequest& Request, const FString& PythonExpression);

	/**
	 * Look up Request.Method in MCPTools.registry and invoke the registered Python callable with
	 * Request.Args (JSON-decoded into a Python dict). Marshalling is currently primitive — the
	 * Python tool receives the args dict as-is and returns any json.dumps-serialisable value.
	 *
	 * Tier-2 reflective marshalling (unreal struct round-trip) lands in Days 4-5.
	 */
	static FMCPResponse CallPythonTool(const FMCPRequest& Request);

	/**
	 * Query MCPTools.registry.get_all_tools() and return the list of registered Python tool names.
	 * Returns empty array if Python isn't ready or the call fails. Used by the MCP.Tools console
	 * command to enumerate Python-served methods alongside C++-registered handlers.
	 */
	static TArray<FString> GetRegisteredPythonToolNames();

private:
	/** Helper: stamp Response.RequestId / OriginalIdString from Request. Centralised so we don't forget. */
	static void StampResponseId(const FMCPRequest& Request, FMCPResponse& Response);

	/** Helper: build {bIsError=true, Code, Message} response with id stamped from Request. */
	static FMCPResponse MakeError(const FMCPRequest& Request, int32 Code, const FString& Message);

	/** Marker prefix used to bracket the JSON payload emitted by the Python wrapper script. */
	static const TCHAR* GetResultMarkerStart();
	/** Marker suffix used to bracket the JSON payload emitted by the Python wrapper script. */
	static const TCHAR* GetResultMarkerEnd();
};
