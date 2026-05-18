// Copyright FatumGame. All Rights Reserved.

#include "FMCPPythonEval.h"

#include "UnrealMCPBridge.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "IPythonScriptPlugin.h"
#include "Misc/Base64.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "PythonScriptTypes.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	/**
	 * Wrapper-script error codes — kept in sync with the Python side
	 * (Content/Python/MCPTools/registry.py) so both sides agree.
	 *
	 *   -32601  Method not found              (tool name unknown to the registry)
	 *   -32602  Invalid params                (tool arg dict could not be JSON-decoded)
	 *   -32002  Server-defined: tool raised   (Python exception inside the tool body)
	 *   -32603  Internal error                (wrapper script itself failed — protocol bug)
	 */
	constexpr int32 kErrorMethodNotFound = -32601;
	constexpr int32 kErrorInvalidParams  = -32602;
	constexpr int32 kErrorToolException  = -32002;
	constexpr int32 kErrorInternal       = -32603;

	/**
	 * Serialise a TSharedPtr<FJsonObject> (possibly null) to a compact JSON string.
	 * Null / empty-object → "{}". Used to encode Args before base64-wrapping for injection.
	 */
	FString JsonObjectToCompactString(const TSharedPtr<FJsonObject>& Object)
	{
		if (!Object.IsValid())
		{
			return TEXT("{}");
		}

		FString Out;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		if (!FJsonSerializer::Serialize(Object.ToSharedRef(), Writer))
		{
			return TEXT("{}");
		}
		return Out;
	}

	/**
	 * Parse a JSON document (any top-level value) into a TSharedPtr<FJsonValue>.
	 * Returns null on failure — caller decides how to surface the error.
	 */
	TSharedPtr<FJsonValue> ParseJsonValue(const FString& Json)
	{
		TSharedPtr<FJsonValue> Value;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		if (!FJsonSerializer::Deserialize(Reader, Value))
		{
			return nullptr;
		}
		return Value;
	}

	/**
	 * Scan FPythonCommandEx::LogOutput for the marker pair and return the JSON payload between them.
	 * Returns empty string when not found. The marker is emitted by the wrapper script via
	 * unreal.log(...) which routes through GetPythonLogCapture → LogOutput[] as type=Info.
	 *
	 * We scan from the back because the wrapper script emits the marker as its LAST log call —
	 * earlier entries (e.g. user debug prints inside the tool) won't be mistaken for it.
	 */
	FString ExtractMarkerPayload(const FPythonCommandEx& Cmd, const FString& StartMarker, const FString& EndMarker)
	{
		for (int32 i = Cmd.LogOutput.Num() - 1; i >= 0; --i)
		{
			const FString& Line = Cmd.LogOutput[i].Output;
			const int32 StartIdx = Line.Find(StartMarker);
			if (StartIdx == INDEX_NONE)
			{
				continue;
			}
			const int32 PayloadStart = StartIdx + StartMarker.Len();
			const int32 EndIdx = Line.Find(EndMarker, ESearchCase::CaseSensitive, ESearchDir::FromStart, PayloadStart);
			if (EndIdx == INDEX_NONE)
			{
				continue;
			}
			return Line.Mid(PayloadStart, EndIdx - PayloadStart);
		}
		return FString();
	}

	/**
	 * Mirror Python LogOutput entries into LogMCP. Errors → Warning (so a single grep surfaces them),
	 * Python warnings → Log, info → Verbose (kept off by default — opt in via LogMCP Verbose).
	 *
	 * The Python plugin already routes these to LogPython at the appropriate level, but those
	 * lines are spread across log categories; mirroring under LogMCP keeps bridge debugging in
	 * one place.
	 */
	void LogPythonOutput(const FPythonCommandEx& Cmd, const FString& Context)
	{
		for (const FPythonLogOutputEntry& Entry : Cmd.LogOutput)
		{
			switch (Entry.Type)
			{
				case EPythonLogOutputType::Error:
					UE_LOG(LogMCP, Warning, TEXT("[Python:%s] ERROR %s"), *Context, *Entry.Output);
					break;
				case EPythonLogOutputType::Warning:
					UE_LOG(LogMCP, Log, TEXT("[Python:%s] WARN  %s"), *Context, *Entry.Output);
					break;
				case EPythonLogOutputType::Info:
				default:
					UE_LOG(LogMCP, Verbose, TEXT("[Python:%s] INFO  %s"), *Context, *Entry.Output);
					break;
			}
		}
	}
}

const TCHAR* FMCPPythonEval::GetResultMarkerStart()
{
	return TEXT("__MCP_RESULT_START__");
}

const TCHAR* FMCPPythonEval::GetResultMarkerEnd()
{
	return TEXT("__MCP_RESULT_END__");
}

bool FMCPPythonEval::IsPythonReady()
{
	IPythonScriptPlugin* Plugin = IPythonScriptPlugin::Get();
	return Plugin && Plugin->IsPythonInitialized();
}

void FMCPPythonEval::StampResponseId(const FMCPRequest& Request, FMCPResponse& Response)
{
	Response.RequestId = Request.RequestId;
	Response.OriginalIdString = Request.OriginalIdString;
}

FMCPResponse FMCPPythonEval::MakeError(const FMCPRequest& Request, int32 Code, const FString& Message)
{
	FMCPResponse Response;
	StampResponseId(Request, Response);
	Response.bIsError = true;
	Response.ErrorCode = Code;
	Response.ErrorMessage = Message;
	return Response;
}

FMCPResponse FMCPPythonEval::EvalExpression(const FMCPRequest& Request, const FString& PythonExpression)
{
	check(IsInGameThread());

	if (!IsPythonReady())
	{
		UE_LOG(LogMCP, Warning, TEXT("EvalExpression: Python not initialised"));
		return MakeError(Request, kErrorInternal, TEXT("python not initialised"));
	}

	if (PythonExpression.IsEmpty())
	{
		return MakeError(Request, kErrorInvalidParams, TEXT("exec_python: empty expression"));
	}

	FPythonCommandEx Cmd;
	Cmd.Command = PythonExpression;
	Cmd.ExecutionMode = EPythonCommandExecutionMode::EvaluateStatement;
	Cmd.FileExecutionScope = EPythonFileExecutionScope::Private;
	Cmd.Flags = EPythonCommandFlags::None;

	const bool bOk = IPythonScriptPlugin::Get()->ExecPythonCommandEx(Cmd);
	LogPythonOutput(Cmd, TEXT("ExecPython"));

	if (!bOk)
	{
		// CommandResult on failure contains the Python exception trace string.
		const FString Trace = Cmd.CommandResult;
		UE_LOG(LogMCP, Warning, TEXT("EvalExpression: ExecPythonCommandEx failed | expr='%s' | trace=%s"),
			*PythonExpression, *Trace);
		// Return only the first line to the client — the full trace can be voluminous.
		FString FirstLine = Trace;
		int32 NewlineIdx;
		if (Trace.FindChar('\n', NewlineIdx))
		{
			FirstLine = Trace.Left(NewlineIdx).TrimEnd();
		}
		const FString ClientMsg = FirstLine.IsEmpty() ? FString(TEXT("python eval failed")) : FirstLine;
		return MakeError(Request, kErrorInternal, ClientMsg);
	}

	// On success, CommandResult holds the repr() of the evaluated expression as a string.
	// We surface it as a JSON string result so the client can read it; arbitrary Python types
	// don't always round-trip through JSON cleanly, but repr() is always a printable string.
	FMCPResponse Response;
	StampResponseId(Request, Response);
	Response.bIsError = false;

	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("repr"), Cmd.CommandResult);
	Response.Result = MakeShared<FJsonValueObject>(Payload);
	return Response;
}

FMCPResponse FMCPPythonEval::CallPythonTool(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!IsPythonReady())
	{
		UE_LOG(LogMCP, Warning, TEXT("CallPythonTool: Python not initialised (method='%s')"), *Request.Method);
		return MakeError(Request, kErrorInternal, TEXT("python not initialised"));
	}

	if (Request.Method.IsEmpty())
	{
		return MakeError(Request, kErrorInvalidParams, TEXT("call_function: empty method"));
	}

	// Encode args + method as base64-of-UTF-8-JSON. Base64 alphabet (A-Z a-z 0-9 + / =) is
	// safe inside any Python string literal — no need to worry about quote / backslash escapes.
	const FString ArgsJson = JsonObjectToCompactString(Request.Args);
	const FString ArgsB64  = FBase64::Encode(ArgsJson);
	const FString MethodB64 = FBase64::Encode(Request.Method);

	// Build the wrapper script. EXECUTE_FILE mode supports multi-statement bodies. The script:
	//   1. base64-decodes method + args from the embedded literals,
	//   2. looks the tool up in the registry,
	//   3. calls it (catching exceptions) and serialises the result via json.dumps,
	//   4. emits __MCP_RESULT_START__<json>__MCP_RESULT_END__ via unreal.log so we can scrape it
	//      from FPythonCommandEx::LogOutput on the C++ side.
	//
	// NOTE: the markers MUST appear in a SINGLE unreal.log call so they land in one LogOutput
	// entry — otherwise our extractor would have to stitch consecutive entries. unreal.log's
	// implementation forwards exactly one string per call to GetPythonLogCapture, so this works.
	const FString StartMarker(GetResultMarkerStart());
	const FString EndMarker(GetResultMarkerEnd());

	// Argument order matters — Printf consumes args left-to-right, matching the %s slots in script:
	//   first  %s : _method   base64 → MethodB64
	//   second %s : _args     base64 → ArgsB64
	//   three %d  : error codes for params / not-found / tool-exception
	//   final two %s : start + end markers
	const FString Wrapper = FString::Printf(
		TEXT("import base64, json, traceback\n")
		TEXT("import unreal\n")
		TEXT("from MCPTools.registry import get_tool\n")
		TEXT("_method = base64.b64decode('%s').decode('utf-8')\n")
		TEXT("_args_json = base64.b64decode('%s').decode('utf-8')\n")
		TEXT("try:\n")
		TEXT("    _args = json.loads(_args_json) if _args_json else {}\n")
		TEXT("except Exception as _e:\n")
		TEXT("    _payload = {'_mcp_error': {'code': %d, 'message': 'invalid params json: ' + str(_e)}}\n")
		TEXT("else:\n")
		TEXT("    _tool = get_tool(_method)\n")
		TEXT("    if _tool is None:\n")
		TEXT("        _payload = {'_mcp_error': {'code': %d, 'message': 'tool not found: ' + _method}}\n")
		TEXT("    else:\n")
		TEXT("        try:\n")
		TEXT("            _result = _tool['fn'](_args)\n")
		TEXT("            _payload = {'_mcp_ok': _result}\n")
		TEXT("        except Exception as _e:\n")
		TEXT("            _payload = {'_mcp_error': {'code': %d, 'message': str(_e), 'traceback': traceback.format_exc()}}\n")
		TEXT("unreal.log('%s' + json.dumps(_payload) + '%s')\n"),
		*MethodB64,
		*ArgsB64,
		kErrorInvalidParams,
		kErrorMethodNotFound,
		kErrorToolException,
		*StartMarker,
		*EndMarker);

	FPythonCommandEx Cmd;
	Cmd.Command = Wrapper;
	Cmd.ExecutionMode = EPythonCommandExecutionMode::ExecuteFile;
	Cmd.FileExecutionScope = EPythonFileExecutionScope::Private;
	Cmd.Flags = EPythonCommandFlags::None;

	const bool bOk = IPythonScriptPlugin::Get()->ExecPythonCommandEx(Cmd);
	LogPythonOutput(Cmd, FString::Printf(TEXT("CallTool:%s"), *Request.Method));

	if (!bOk)
	{
		// Wrapper itself crashed before reaching the marker emit — protocol-level failure.
		const FString Trace = Cmd.CommandResult;
		UE_LOG(LogMCP, Warning, TEXT("CallPythonTool: wrapper script failed | method='%s' | trace=%s"),
			*Request.Method, *Trace);
		FString FirstLine = Trace;
		int32 NewlineIdx;
		if (Trace.FindChar('\n', NewlineIdx))
		{
			FirstLine = Trace.Left(NewlineIdx).TrimEnd();
		}
		const FString ClientMsg = FirstLine.IsEmpty() ? FString(TEXT("python wrapper failed")) : FirstLine;
		return MakeError(Request, kErrorInternal, ClientMsg);
	}

	const FString PayloadJson = ExtractMarkerPayload(Cmd, StartMarker, EndMarker);
	if (PayloadJson.IsEmpty())
	{
		UE_LOG(LogMCP, Warning, TEXT("CallPythonTool: marker missing from Python output | method='%s'"),
			*Request.Method);
		return MakeError(Request, kErrorInternal, TEXT("python wrapper produced no result marker"));
	}

	TSharedPtr<FJsonValue> PayloadValue = ParseJsonValue(PayloadJson);
	if (!PayloadValue.IsValid() || PayloadValue->Type != EJson::Object)
	{
		UE_LOG(LogMCP, Warning, TEXT("CallPythonTool: marker payload not a JSON object | method='%s' | payload=%s"),
			*Request.Method, *PayloadJson);
		return MakeError(Request, kErrorInternal, TEXT("python wrapper result was not valid JSON"));
	}

	const TSharedPtr<FJsonObject>& PayloadObj = PayloadValue->AsObject();

	// Error envelope: {"_mcp_error": {"code": ..., "message": ..., "traceback": ...}}
	const TSharedPtr<FJsonObject>* ErrorObjPtr = nullptr;
	if (PayloadObj->TryGetObjectField(TEXT("_mcp_error"), ErrorObjPtr) && ErrorObjPtr && (*ErrorObjPtr).IsValid())
	{
		int32 ErrorCode = kErrorInternal;
		FString ErrorMessage;
		(*ErrorObjPtr)->TryGetNumberField(TEXT("code"), ErrorCode);
		(*ErrorObjPtr)->TryGetStringField(TEXT("message"), ErrorMessage);

		FString Traceback;
		if ((*ErrorObjPtr)->TryGetStringField(TEXT("traceback"), Traceback) && !Traceback.IsEmpty())
		{
			// Verbose so it isn't spammy under normal conditions but is grep-able after the fact.
			UE_LOG(LogMCP, Warning, TEXT("CallPythonTool: tool '%s' raised:\n%s"), *Request.Method, *Traceback);
		}
		if (ErrorMessage.IsEmpty())
		{
			ErrorMessage = TEXT("python tool error");
		}
		return MakeError(Request, ErrorCode, ErrorMessage);
	}

	// Success envelope: {"_mcp_ok": <any-json-value>}
	const TSharedPtr<FJsonValue> OkValue = PayloadObj->TryGetField(TEXT("_mcp_ok"));
	if (!OkValue.IsValid())
	{
		UE_LOG(LogMCP, Warning, TEXT("CallPythonTool: payload missing _mcp_ok / _mcp_error | method='%s' | payload=%s"),
			*Request.Method, *PayloadJson);
		return MakeError(Request, kErrorInternal, TEXT("python wrapper result missing _mcp_ok / _mcp_error"));
	}

	FMCPResponse Response;
	StampResponseId(Request, Response);
	Response.bIsError = false;
	Response.Result = OkValue;
	return Response;
}

TArray<FString> FMCPPythonEval::GetRegisteredPythonToolNames()
{
	check(IsInGameThread());

	TArray<FString> Names;
	if (!IsPythonReady())
	{
		return Names;
	}

	const FString StartMarker(GetResultMarkerStart());
	const FString EndMarker(GetResultMarkerEnd());

	// Same marker idiom as CallPythonTool — keeps the C++ extractor uniform.
	const FString Script = FString::Printf(
		TEXT("import json, unreal\n")
		TEXT("from MCPTools.registry import get_all_tools\n")
		TEXT("_names = sorted(get_all_tools().keys())\n")
		TEXT("unreal.log('%s' + json.dumps(_names) + '%s')\n"),
		*StartMarker, *EndMarker);

	FPythonCommandEx Cmd;
	Cmd.Command = Script;
	Cmd.ExecutionMode = EPythonCommandExecutionMode::ExecuteFile;
	Cmd.FileExecutionScope = EPythonFileExecutionScope::Private;
	Cmd.Flags = EPythonCommandFlags::None;

	const bool bOk = IPythonScriptPlugin::Get()->ExecPythonCommandEx(Cmd);
	if (!bOk)
	{
		UE_LOG(LogMCP, Warning, TEXT("GetRegisteredPythonToolNames: ExecPythonCommandEx failed | %s"), *Cmd.CommandResult);
		return Names;
	}

	const FString PayloadJson = ExtractMarkerPayload(Cmd, StartMarker, EndMarker);
	if (PayloadJson.IsEmpty())
	{
		UE_LOG(LogMCP, Warning, TEXT("GetRegisteredPythonToolNames: marker missing"));
		return Names;
	}

	TSharedPtr<FJsonValue> ParsedValue = ParseJsonValue(PayloadJson);
	if (!ParsedValue.IsValid() || ParsedValue->Type != EJson::Array)
	{
		UE_LOG(LogMCP, Warning, TEXT("GetRegisteredPythonToolNames: payload was not an array | payload=%s"), *PayloadJson);
		return Names;
	}

	for (const TSharedPtr<FJsonValue>& Entry : ParsedValue->AsArray())
	{
		FString Name;
		if (Entry.IsValid() && Entry->TryGetString(Name))
		{
			Names.Add(MoveTemp(Name));
		}
	}
	return Names;
}
