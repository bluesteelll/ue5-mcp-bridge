// Copyright FatumGame. All Rights Reserved.

#include "FMCPDay7Handlers.h"

#include "FMCPDispatchQueue.h"
#include "FMCPJobRegistry.h"
#include "FMCPLogStream.h"
#include "FMCPPythonEval.h"
#include "UnrealMCPBridge.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "IPythonScriptPlugin.h"
#include "Misc/Base64.h"
#include "Misc/Guid.h"
#include "Misc/ScopeLock.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "PythonScriptTypes.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	// Same JSON-RPC error codes used elsewhere in the bridge. Day7_ prefix on the constants and
	// helpers below is required (NOT cosmetic) because UBT's unity-build groups multiple .cpp
	// files into a single TU, and anonymous namespaces do NOT isolate symbols across files that
	// end up in the same TU. FMCPPythonEval.cpp defines identical names — without the prefix,
	// MSVC reports C2374/C2086/C2084 redefinition errors whenever the two files share a unity.
	// Same rationale as FMCPMarshalling.cpp's MCP_MakeError / MCP_MakeSuccess. See also the
	// comment on Day7_MakeError below.
	constexpr int32 kDay7ErrorInvalidParams  = -32602;
	constexpr int32 kDay7ErrorObjectNotFound = -32004;
	constexpr int32 kDay7ErrorInternal       = -32603;

	void StampResponseId(const FMCPRequest& Request, FMCPResponse& Response)
	{
		Response.RequestId = Request.RequestId;
		Response.OriginalIdString = Request.OriginalIdString;
	}

	// NOTE: cannot name these `MakeError` / `MakeSuccess` — UE's global `MakeError` template
	// (`Templates/ValueOrError.h`) wins overload resolution and produces TValueOrError_ErrorProxy
	// instead of FMCPResponse. Day7_ prefix disambiguates (anonymous-namespace alone is
	// insufficient because the global template participates in unqualified lookup). Same
	// rationale as FMCPMarshalling.cpp's MCP_MakeError / MCP_MakeSuccess.
	FMCPResponse Day7_MakeError(const FMCPRequest& Request, int32 Code, const FString& Message)
	{
		FMCPResponse R;
		StampResponseId(Request, R);
		R.bIsError = true;
		R.ErrorCode = Code;
		R.ErrorMessage = Message;
		return R;
	}

	FMCPResponse Day7_MakeSuccess(const FMCPRequest& Request, TSharedPtr<FJsonObject> Result)
	{
		FMCPResponse R;
		StampResponseId(Request, R);
		R.bIsError = false;
		R.Result = MakeShared<FJsonValueObject>(MoveTemp(Result));
		return R;
	}

	/** Compact-serialise a JSON object. Null/empty becomes "{}". Day7_ prefix per the unity-
	 *  build symbol-collision note above. */
	FString Day7_JsonObjectToCompactString(const TSharedPtr<FJsonObject>& Object)
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

	/** Parse a JSON value (any top-level type). Returns null on failure. Day7_ prefix per the
	 *  unity-build symbol-collision note above. */
	TSharedPtr<FJsonValue> Day7_ParseJsonValue(const FString& Json)
	{
		TSharedPtr<FJsonValue> Value;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		if (!FJsonSerializer::Deserialize(Reader, Value))
		{
			return nullptr;
		}
		return Value;
	}

	/** GUID parsing accepting both DigitsWithHyphens and Digits formats. */
	bool TryParseJobId(const FString& Raw, FGuid& Out)
	{
		return FGuid::ParseExact(Raw, EGuidFormats::DigitsWithHyphens, Out)
			|| FGuid::Parse(Raw, Out);
	}

	/** Build the {id, state, ...} dict shared by job.status and job.list_active entries. */
	TSharedRef<FJsonObject> StatusSnapshotToJson(const FMCPJobRegistry::FStatusSnapshot& Snap)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("id"), Snap.Id.ToString(EGuidFormats::DigitsWithHyphens));
		Obj->SetStringField(TEXT("description"), Snap.Description);
		Obj->SetStringField(TEXT("state"), LexJobState(Snap.State));
		Obj->SetNumberField(TEXT("progress"), Snap.Progress);
		Obj->SetBoolField(TEXT("cancel_requested"), Snap.bCancelRequested);
		Obj->SetBoolField(TEXT("game_thread"), Snap.bGameThreadRequired);
		Obj->SetNumberField(TEXT("submitted_at"), Snap.SubmittedAt);
		Obj->SetNumberField(TEXT("started_at"), Snap.StartedAt);
		Obj->SetNumberField(TEXT("finished_at"), Snap.FinishedAt);
		if (!Snap.ErrorMessage.IsEmpty())
		{
			Obj->SetStringField(TEXT("message"), Snap.ErrorMessage);
		}
		return Obj;
	}

	/** Verbosity → wire string. Matches LogTimes::PerLogEntry conventions. */
	const TCHAR* LexVerbosity(ELogVerbosity::Type V)
	{
		switch (V & ELogVerbosity::VerbosityMask)
		{
			case ELogVerbosity::Fatal:        return TEXT("Fatal");
			case ELogVerbosity::Error:        return TEXT("Error");
			case ELogVerbosity::Warning:      return TEXT("Warning");
			case ELogVerbosity::Display:      return TEXT("Display");
			case ELogVerbosity::Log:          return TEXT("Log");
			case ELogVerbosity::Verbose:      return TEXT("Verbose");
			case ELogVerbosity::VeryVerbose:  return TEXT("VeryVerbose");
			default:                          return TEXT("Unknown");
		}
	}

	TSharedRef<FJsonObject> LogEntryToJson(const FMCPLogEntry& E)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		// ISO 8601 in UTC — matches the Iso8601 format constant used by FDateTime::ToIso8601().
		Obj->SetStringField(TEXT("timestamp"), E.Timestamp.ToIso8601());
		Obj->SetStringField(TEXT("category"), E.Category.ToString());
		Obj->SetStringField(TEXT("verbosity"), LexVerbosity(E.Verbosity));
		Obj->SetStringField(TEXT("message"), E.Message);
		return Obj;
	}

	/**
	 * Build a job body that invokes a Python tool. Captured method + args by value so the body
	 * is self-contained — the FMCPRequest the submitter sent has long since been responded to.
	 *
	 * The body runs the same wrapper script CallPythonTool builds, but it's done inline here so
	 * we can return a TSharedPtr<FJsonValue> directly instead of going through FMCPResponse.
	 */
	FMCPJobRegistry::FBody MakePythonToolJobBody(FString Method, TSharedPtr<FJsonObject> Args)
	{
		// Snapshot args into a string so the body lambda doesn't capture a TSharedPtr (which would
		// need extra care for thread safety if Args is mutated upstream).
		const FString ArgsJson = Day7_JsonObjectToCompactString(Args);

		return [Method = MoveTemp(Method), ArgsJson](FMCPJob& Job) -> TSharedPtr<FJsonValue>
		{
			// Synthesise a transient FMCPRequest just for CallPythonTool.
			FMCPRequest Synth;
			Synth.Kind = EMCPRequestKind::CallFunction;
			Synth.Method = Method;
			Synth.OriginalIdString = TEXT("(job)");
			if (!ArgsJson.IsEmpty() && ArgsJson != TEXT("{}"))
			{
				TSharedPtr<FJsonValue> Parsed = Day7_ParseJsonValue(ArgsJson);
				if (Parsed.IsValid() && Parsed->Type == EJson::Object)
				{
					Synth.Args = Parsed->AsObject();
				}
			}

			// Honour cancel-before-call.
			if (Job.bCancelRequested.load(std::memory_order_acquire))
			{
				return nullptr; // → Cancelled (ErrorMessage stays empty)
			}

			// Bump progress so polling clients see SOMETHING change.
			Job.Progress.store(0.5f, std::memory_order_release);

			const FMCPResponse Resp = FMCPPythonEval::CallPythonTool(Synth);
			if (Resp.bIsError)
			{
				Job.ErrorMessage = FString::Printf(TEXT("[code=%d] %s"), Resp.ErrorCode, *Resp.ErrorMessage);
				return nullptr;
			}

			Job.Progress.store(1.0f, std::memory_order_release);
			return Resp.Result; // may be null if tool returned null — interpreted as Failed
		};
	}
} // namespace

// ============================================================================================
// job.* handlers
// ============================================================================================

FMCPResponse FMCPDay7Handlers::JobSubmit(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!Request.Args.IsValid())
	{
		return Day7_MakeError(Request, kDay7ErrorInvalidParams, TEXT("job.submit requires args.method (string)"));
	}
	FString Method;
	if (!Request.Args->TryGetStringField(TEXT("method"), Method) || Method.IsEmpty())
	{
		return Day7_MakeError(Request, kDay7ErrorInvalidParams, TEXT("job.submit: args.method (non-empty string) required"));
	}

	FString Description;
	Request.Args->TryGetStringField(TEXT("description"), Description);
	if (Description.IsEmpty())
	{
		Description = FString::Printf(TEXT("python tool: %s"), *Method);
	}

	bool bGameThreadRequired = true; // default — most useful tools touch editor state
	Request.Args->TryGetBoolField(TEXT("game_thread"), bGameThreadRequired);

	// Inner args object — optional. Pass through to the synthesised CallPythonTool.
	TSharedPtr<FJsonObject> InnerArgs;
	const TSharedPtr<FJsonObject>* InnerArgsPtr = nullptr;
	if (Request.Args->TryGetObjectField(TEXT("args"), InnerArgsPtr) && InnerArgsPtr && (*InnerArgsPtr).IsValid())
	{
		InnerArgs = *InnerArgsPtr;
	}
	else
	{
		InnerArgs = MakeShared<FJsonObject>();
	}

	FMCPJobRegistry::FBody Body = MakePythonToolJobBody(Method, InnerArgs);
	const FGuid JobId = FMCPJobRegistry::Get().SubmitJob(Description, MoveTemp(Body), bGameThreadRequired);
	if (!JobId.IsValid())
	{
		return Day7_MakeError(Request, kDay7ErrorInternal, TEXT("job registry refused submit (shutdown?)"));
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("job_id"), JobId.ToString(EGuidFormats::DigitsWithHyphens));
	return Day7_MakeSuccess(Request, Result);
}

FMCPResponse FMCPDay7Handlers::JobStatus(const FMCPRequest& Request)
{
	// HOTFIX 2 (2026-05): promoted to Lane B. Body only does (a) string parse of job_id and
	// (b) FMCPJobRegistry::GetStatus which takes FScopeLock(JobsLock) and reads
	// std::atomic fields — no UObject access, no AR enumeration, no GWorld. Required to break
	// the composite-deadlock cycle where a Python composite that runs on the game thread inside
	// CallPythonTool tries to dispatch_internal('job.status', ...) and blocks on socket.recv()
	// — if job.status were Lane A, the request would queue back to the same game thread that's
	// blocked on the socket, deadlocking until the 60s TCP timeout. Lane B lets the listener
	// thread answer immediately so the game thread can continue processing the job-pool
	// AsyncTask(GameThread) callbacks that drive bGameThreadRequired=true jobs to completion.

	if (!Request.Args.IsValid())
	{
		return Day7_MakeError(Request, kDay7ErrorInvalidParams, TEXT("job.status requires args.job_id (string)"));
	}
	FString IdStr;
	if (!Request.Args->TryGetStringField(TEXT("job_id"), IdStr) || IdStr.IsEmpty())
	{
		return Day7_MakeError(Request, kDay7ErrorInvalidParams, TEXT("job.status: args.job_id (non-empty string) required"));
	}
	FGuid Id;
	if (!TryParseJobId(IdStr, Id))
	{
		return Day7_MakeError(Request, kDay7ErrorInvalidParams, FString::Printf(TEXT("job.status: malformed job_id '%s'"), *IdStr));
	}

	FMCPJobRegistry::FStatusSnapshot Snap;
	if (!FMCPJobRegistry::Get().GetStatus(Id, Snap))
	{
		return Day7_MakeError(Request, kDay7ErrorObjectNotFound, FString::Printf(TEXT("job not found: %s"), *IdStr));
	}

	return Day7_MakeSuccess(Request, StatusSnapshotToJson(Snap));
}

FMCPResponse FMCPDay7Handlers::JobResult(const FMCPRequest& Request)
{
	// HOTFIX 2 (2026-05): promoted to Lane B. Same rationale as JobStatus — composite tools call
	// this via dispatch_internal (TCP loopback) from inside CallPythonTool on the game thread,
	// so Lane A would deadlock. Body uses only thread-safe FMCPJobRegistry methods
	// (GetStatus / GetResult — both FScopeLock(JobsLock)-guarded). wait_timeout_s honoured via
	// FPlatformProcess::Sleep on the listener thread itself — clamped to 5s (existing) so a
	// stuck job can't permanently lock the connection; other connections have their own threads
	// and remain responsive. Callers SHOULD pass wait_timeout_s=0 and poll externally.

	if (!Request.Args.IsValid())
	{
		return Day7_MakeError(Request, kDay7ErrorInvalidParams, TEXT("job.result requires args.job_id (string)"));
	}
	FString IdStr;
	if (!Request.Args->TryGetStringField(TEXT("job_id"), IdStr) || IdStr.IsEmpty())
	{
		return Day7_MakeError(Request, kDay7ErrorInvalidParams, TEXT("job.result: args.job_id (non-empty string) required"));
	}
	FGuid Id;
	if (!TryParseJobId(IdStr, Id))
	{
		return Day7_MakeError(Request, kDay7ErrorInvalidParams, FString::Printf(TEXT("job.result: malformed job_id '%s'"), *IdStr));
	}

	double WaitTimeoutS = 0.0;
	if (Request.Args->HasField(TEXT("wait_timeout_s")))
	{
		Request.Args->TryGetNumberField(TEXT("wait_timeout_s"), WaitTimeoutS);
		// Sanity clamp — keep game-thread stall bounded. Real long-waits should poll externally.
		WaitTimeoutS = FMath::Clamp(WaitTimeoutS, 0.0, 5.0);
	}

	const double Deadline = FPlatformTime::Seconds() + WaitTimeoutS;
	FMCPJobRegistry::FStatusSnapshot Snap;
	while (true)
	{
		if (!FMCPJobRegistry::Get().GetStatus(Id, Snap))
		{
			return Day7_MakeError(Request, kDay7ErrorObjectNotFound, FString::Printf(TEXT("job not found: %s"), *IdStr));
		}
		const bool bTerminal =
			Snap.State == EMCPJobState::Succeeded
			|| Snap.State == EMCPJobState::Failed
			|| Snap.State == EMCPJobState::Cancelled;
		if (bTerminal)
		{
			break;
		}
		if (FPlatformTime::Seconds() >= Deadline)
		{
			break;
		}
		// 5 ms poll — short enough that we don't burn cycles, long enough that we don't thrash.
		FPlatformProcess::Sleep(0.005f);
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("id"), Snap.Id.ToString(EGuidFormats::DigitsWithHyphens));
	Result->SetStringField(TEXT("state"), LexJobState(Snap.State));

	switch (Snap.State)
	{
		case EMCPJobState::Succeeded:
		{
			Result->SetBoolField(TEXT("ok"), true);
			const TSharedPtr<FJsonValue> Inner = FMCPJobRegistry::Get().GetResult(Id);
			if (Inner.IsValid())
			{
				Result->SetField(TEXT("result"), Inner);
			}
			break;
		}
		case EMCPJobState::Failed:
		{
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), Snap.ErrorMessage);
			break;
		}
		case EMCPJobState::Cancelled:
		{
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetBoolField(TEXT("cancelled"), true);
			if (!Snap.ErrorMessage.IsEmpty())
			{
				Result->SetStringField(TEXT("message"), Snap.ErrorMessage);
			}
			break;
		}
		default:
		{
			// Still pending / running after wait.
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetBoolField(TEXT("pending"), true);
			Result->SetNumberField(TEXT("progress"), Snap.Progress);
			break;
		}
	}

	return Day7_MakeSuccess(Request, Result);
}

FMCPResponse FMCPDay7Handlers::JobCancel(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!Request.Args.IsValid())
	{
		return Day7_MakeError(Request, kDay7ErrorInvalidParams, TEXT("job.cancel requires args.job_id (string)"));
	}
	FString IdStr;
	if (!Request.Args->TryGetStringField(TEXT("job_id"), IdStr) || IdStr.IsEmpty())
	{
		return Day7_MakeError(Request, kDay7ErrorInvalidParams, TEXT("job.cancel: args.job_id (non-empty string) required"));
	}
	FGuid Id;
	if (!TryParseJobId(IdStr, Id))
	{
		return Day7_MakeError(Request, kDay7ErrorInvalidParams, FString::Printf(TEXT("job.cancel: malformed job_id '%s'"), *IdStr));
	}

	const bool bAccepted = FMCPJobRegistry::Get().RequestCancel(Id);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("accepted"), bAccepted);
	Result->SetStringField(TEXT("id"), Id.ToString(EGuidFormats::DigitsWithHyphens));
	return Day7_MakeSuccess(Request, Result);
}

FMCPResponse FMCPDay7Handlers::JobListActive(const FMCPRequest& Request)
{
	check(IsInGameThread());

	const TArray<FMCPJobRegistry::FStatusSnapshot> Active = FMCPJobRegistry::Get().GetActive();

	TArray<TSharedPtr<FJsonValue>> Items;
	Items.Reserve(Active.Num());
	for (const FMCPJobRegistry::FStatusSnapshot& Snap : Active)
	{
		Items.Add(MakeShared<FJsonValueObject>(StatusSnapshotToJson(Snap)));
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("jobs"), Items);
	Result->SetNumberField(TEXT("total_tracked"), FMCPJobRegistry::Get().GetTrackedJobCount());
	return Day7_MakeSuccess(Request, Result);
}

// ============================================================================================
// log.* handlers
// ============================================================================================

FMCPResponse FMCPDay7Handlers::LogTail(const FMCPRequest& Request)
{
	check(IsInGameThread());

	int32 Lines = 200;
	FString Category;
	if (Request.Args.IsValid())
	{
		Request.Args->TryGetNumberField(TEXT("lines"), Lines);
		Request.Args->TryGetStringField(TEXT("category"), Category);
	}
	// Clamp to ring depth — silently. Returning fewer than asked is normal once we hit the cap.
	Lines = FMath::Clamp(Lines, 1, FMCPLogStream::kMaxEntries);

	const FName CategoryName(Category);
	const TArray<FMCPLogEntry> Entries = FMCPLogStream::Get().GetLastN(
		Lines, Category.IsEmpty() ? nullptr : &CategoryName);

	TArray<TSharedPtr<FJsonValue>> EntryJsonValues;
	EntryJsonValues.Reserve(Entries.Num());
	for (const FMCPLogEntry& E : Entries)
	{
		EntryJsonValues.Add(MakeShared<FJsonValueObject>(LogEntryToJson(E)));
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("entries"), EntryJsonValues);
	Result->SetNumberField(TEXT("total_observed"), static_cast<double>(FMCPLogStream::Get().GetTotalObserved()));
	Result->SetNumberField(TEXT("ring_capacity"), FMCPLogStream::kMaxEntries);
	Result->SetNumberField(TEXT("ring_count"), FMCPLogStream::Get().GetEntryCount());
	if (!Category.IsEmpty())
	{
		Result->SetStringField(TEXT("category_filter"), Category);
	}
	return Day7_MakeSuccess(Request, Result);
}

FMCPResponse FMCPDay7Handlers::LogSubscribe(const FMCPRequest& Request)
{
	check(IsInGameThread());

	// Phase 1 ack stub. Returning a structured response so clients can detect the deferred-feature
	// surface — actual push streaming arrives in Phase 2 (needs WebSocket or framed bidirectional
	// JSON-RPC; current single-request-single-response protocol cannot push).
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("subscribed"), true);
	Result->SetStringField(TEXT("note"),
		TEXT("phase-1 ack stub; push streaming arrives in phase 2 (poll log.tail in the meantime)"));

	FString Category;
	if (Request.Args.IsValid())
	{
		Request.Args->TryGetStringField(TEXT("category"), Category);
	}
	if (!Category.IsEmpty())
	{
		Result->SetStringField(TEXT("category"), Category);
	}
	return Day7_MakeSuccess(Request, Result);
}

FMCPResponse FMCPDay7Handlers::LogSearch(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!Request.Args.IsValid())
	{
		return Day7_MakeError(Request, kDay7ErrorInvalidParams, TEXT("log.search requires args.pattern (regex string)"));
	}
	FString Pattern;
	if (!Request.Args->TryGetStringField(TEXT("pattern"), Pattern) || Pattern.IsEmpty())
	{
		return Day7_MakeError(Request, kDay7ErrorInvalidParams, TEXT("log.search: args.pattern (non-empty regex) required"));
	}
	int32 MaxResults = 100;
	Request.Args->TryGetNumberField(TEXT("max_results"), MaxResults);
	MaxResults = FMath::Clamp(MaxResults, 1, FMCPLogStream::kMaxEntries);

	const TArray<FMCPLogEntry> Matches = FMCPLogStream::Get().Search(Pattern, MaxResults);

	TArray<TSharedPtr<FJsonValue>> EntryJsonValues;
	EntryJsonValues.Reserve(Matches.Num());
	for (const FMCPLogEntry& E : Matches)
	{
		EntryJsonValues.Add(MakeShared<FJsonValueObject>(LogEntryToJson(E)));
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("entries"), EntryJsonValues);
	Result->SetNumberField(TEXT("total_scanned"), FMCPLogStream::Get().GetEntryCount());
	Result->SetStringField(TEXT("pattern"), Pattern);
	return Day7_MakeSuccess(Request, Result);
}

// ============================================================================================
// tools.list — combined Python + C++ enumeration
// ============================================================================================

namespace
{
	/**
	 * Query Python registry for the full tool meta. Returns null on Python-not-ready or wrapper
	 * error. Same marker-bracketed protocol used elsewhere in FMCPPythonEval.
	 */
	TSharedPtr<FJsonObject> QueryPythonToolsMeta()
	{
		if (!FMCPPythonEval::IsPythonReady())
		{
			return nullptr;
		}

		// Reuse the same marker convention; we don't import the markers from FMCPPythonEval to
		// keep this file's dependency surface small — they're a stable wire protocol.
		const FString StartMarker = TEXT("__MCP_RESULT_START__");
		const FString EndMarker   = TEXT("__MCP_RESULT_END__");

		// Build a Python script that strips the registry entries down to the wire-shape the
		// blueprint specifies (drop the 'fn' callable + 'module' name; keep schemas + flags).
		const FString Script = FString::Printf(
			TEXT("import json, unreal\n")
			TEXT("from MCPTools.registry import get_all_tools\n")
			TEXT("_out = {}\n")
			TEXT("for _name, _meta in get_all_tools().items():\n")
			TEXT("    _out[_name] = {\n")
			TEXT("        'schema_in':     _meta.get('schema_in', {}),\n")
			TEXT("        'schema_out':    _meta.get('schema_out', {}),\n")
			TEXT("        'thread_safe':   bool(_meta.get('thread_safe', False)),\n")
			TEXT("        'failure_modes': _meta.get('failure_modes', []),\n")
			TEXT("    }\n")
			TEXT("unreal.log('%s' + json.dumps(_out) + '%s')\n"),
			*StartMarker, *EndMarker);

		FPythonCommandEx Cmd;
		Cmd.Command = Script;
		Cmd.ExecutionMode = EPythonCommandExecutionMode::ExecuteFile;
		Cmd.FileExecutionScope = EPythonFileExecutionScope::Private;
		Cmd.Flags = EPythonCommandFlags::None;

		const bool bOk = IPythonScriptPlugin::Get()->ExecPythonCommandEx(Cmd);
		if (!bOk)
		{
			UE_LOG(LogMCP, Warning, TEXT("tools.list: Python registry query failed | %s"), *Cmd.CommandResult);
			return nullptr;
		}

		// Scan LogOutput back-to-front for the marker — same logic as FMCPPythonEval::ExtractMarkerPayload.
		FString PayloadJson;
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
			PayloadJson = Line.Mid(PayloadStart, EndIdx - PayloadStart);
			break;
		}

		if (PayloadJson.IsEmpty())
		{
			UE_LOG(LogMCP, Warning, TEXT("tools.list: Python wrapper emitted no marker"));
			return nullptr;
		}

		TSharedPtr<FJsonValue> Parsed = Day7_ParseJsonValue(PayloadJson);
		if (!Parsed.IsValid() || Parsed->Type != EJson::Object)
		{
			UE_LOG(LogMCP, Warning, TEXT("tools.list: Python wrapper result was not a JSON object | %s"), *PayloadJson);
			return nullptr;
		}
		return Parsed->AsObject();
	}
} // namespace

FMCPResponse FMCPDay7Handlers::ToolsList(const FMCPRequest& Request)
{
	check(IsInGameThread());

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();

	// C++ handler enumeration — always available; safe even pre-Python-init.
	{
		const TArray<FString> Methods = FMCPDispatchQueue::Get().GetRegisteredMethodNames();
		TArray<TSharedPtr<FJsonValue>> CppArray;
		CppArray.Reserve(Methods.Num());
		// Sort for deterministic output — easier to diff between editor sessions.
		TArray<FString> Sorted = Methods;
		Sorted.Sort();
		for (const FString& M : Sorted)
		{
			CppArray.Add(MakeShared<FJsonValueString>(M));
		}
		Result->SetArrayField(TEXT("cpp_handlers"), CppArray);
	}

	// Python tool meta — may degrade to {} if Python isn't initialised.
	TSharedPtr<FJsonObject> PythonMeta = QueryPythonToolsMeta();
	if (!PythonMeta.IsValid())
	{
		PythonMeta = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("python_ready"), false);
	}
	else
	{
		Result->SetBoolField(TEXT("python_ready"), true);
	}
	Result->SetObjectField(TEXT("python_tools"), PythonMeta);

	return Day7_MakeSuccess(Request, Result);
}
