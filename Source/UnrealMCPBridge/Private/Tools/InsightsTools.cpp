// Copyright FatumGame. All Rights Reserved.

#include "InsightsTools.h"

#include "MCPSurfaceRegistry.h"

#include "FMCPDispatchQueue.h"
#include "MCPJsonBuilder.h"
#include "MCPToolHelpers.h"
#include "Utils/MCPPathSandbox.h"

#include "Containers/StringConv.h"
#include "Containers/StringFwd.h"
#include "HAL/CriticalSection.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/DateTime.h"
#include "Misc/Paths.h"
#include "ProfilingDebugging/TraceAuxiliary.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// Channel enumeration: use the global `UE::Trace::EnumerateChannels` from
// Trace/Trace.h which IS TRACELOG_API exported. The class method
// FChannel::EnumerateChannels is NOT exported (no TRACELOG_API qualifier) so
// calling it from outside the module yields LNK2019 — verified at link time.
#include "Trace/Trace.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// INS_ prefix per the unity-build symbol-collision convention.
	constexpr int32 kINSErrorInternal       = -32603;
	constexpr int32 kINSErrorInvalidParams  = kMCPErrorInvalidParams;
	constexpr int32 kINSErrorPathEscape     = kMCPErrorPathEscape;
	constexpr int32 kINSErrorOperationFailed = kMCPErrorOperationFailed;

	// Engine tracks IsConnected/Destination/Channels itself; we only track the
	// trace start time (engine doesn't expose it) so insights.get_status can report
	// elapsed_seconds + started_at_utc to the caller.
	struct FInsightsLocalState
	{
		double    StartedAtSeconds = 0.0;
		FDateTime StartedAtUtc     = FDateTime(0);
	};

	static FInsightsLocalState  GInsightsLocalState;
	static FCriticalSection     GInsightsLocalStateLock;

	// Normalise the "channels" arg. Accepts either:
	//   - string: "cpu,gpu,frame"  (comma or semicolon separators)
	//   - array:  ["cpu","gpu","frame"]
	// Tokens are lowercased + trimmed. Returns false on any malformed token.
	bool INS_ParseChannels(
		const TSharedPtr<FJsonObject>& Args,
		FString& OutChannelsForEngine,
		TArray<FString>& OutChannelsForResponse,
		FString& OutErr)
	{
		// Default channel set if no input provided.
		const FString kDefaultChannels = TEXT("cpu,gpu,frame,bookmark");

		if (!Args.IsValid())
		{
			OutChannelsForEngine = kDefaultChannels;
			kDefaultChannels.ParseIntoArray(OutChannelsForResponse, TEXT(","), /*bCullEmpty=*/true);
			return true;
		}

		TArray<FString> Tokens;

		// Try array form first.
		const TArray<TSharedPtr<FJsonValue>>* ArrPtr = nullptr;
		if (Args->TryGetArrayField(TEXT("channels"), ArrPtr) && ArrPtr)
		{
			for (const TSharedPtr<FJsonValue>& V : *ArrPtr)
			{
				if (!V.IsValid() || V->Type != EJson::String)
				{
					OutErr = TEXT("channels[] must contain only strings");
					return false;
				}
				Tokens.Add(V->AsString());
			}
		}
		else
		{
			// Fall back to string form.
			FString Raw;
			if (!Args->TryGetStringField(TEXT("channels"), Raw) || Raw.IsEmpty())
			{
				// Field missing or empty → default set.
				OutChannelsForEngine = kDefaultChannels;
				kDefaultChannels.ParseIntoArray(OutChannelsForResponse, TEXT(","), /*bCullEmpty=*/true);
				return true;
			}
			// Accept both ',' and ';' as separators.
			Raw.Replace(TEXT(";"), TEXT(",")).ParseIntoArray(Tokens, TEXT(","), /*bCullEmpty=*/true);
		}

		// Validate + normalise each token.
		FString Built;
		for (FString Tok : Tokens)
		{
			Tok = Tok.TrimStartAndEnd().ToLower();
			if (Tok.IsEmpty())
			{
				continue;
			}
			// Anti-injection: channels are lowercase ASCII + digits + underscore.
			for (TCHAR Ch : Tok)
			{
				if (!(((Ch >= 'a') && (Ch <= 'z')) || ((Ch >= '0') && (Ch <= '9')) || (Ch == '_')))
				{
					OutErr = FString::Printf(TEXT("channel name '%s' contains illegal character"), *Tok);
					return false;
				}
			}
			OutChannelsForResponse.Add(Tok);
			if (!Built.IsEmpty())
			{
				Built.AppendChar(TEXT(','));
			}
			Built.Append(Tok);
		}

		if (Built.IsEmpty())
		{
			// All tokens were empty after trim — use defaults rather than error.
			OutChannelsForEngine = kDefaultChannels;
			kDefaultChannels.ParseIntoArray(OutChannelsForResponse, TEXT(","), /*bCullEmpty=*/true);
			return true;
		}

		OutChannelsForEngine = MoveTemp(Built);
		return true;
	}

	// Compose the engine-side active channels list by reading FTraceAuxiliary's
	// own state. Returns the parsed token array for JSON output.
	TArray<FString> INS_QueryActiveChannels()
	{
		TStringBuilder<512> Builder;
		FTraceAuxiliary::GetActiveChannelsString(Builder);
		const FString Joined(Builder.ToString());
		TArray<FString> Tokens;
		if (!Joined.IsEmpty())
		{
			Joined.ParseIntoArray(Tokens, TEXT(","), /*bCullEmpty=*/true);
			for (FString& T : Tokens)
			{
				T = T.TrimStartAndEnd();
			}
		}
		return Tokens;
	}
} // namespace

namespace FInsightsTools
{

// --- insights.start_capture -------------------------------------------------------------------
//
// Args:    { channels?: "cpu,gpu,frame" | ["cpu","gpu","frame"],
//            output_path?: "Saved/Profiling/MCP.utrace" }
// Result:  { path, channels[], started_at_utc }
//
// Default channels: "cpu,gpu,frame,bookmark". Default output_path:
// <Saved>/Profiling/Traces/MCP_<UTC>.utrace (auto-created dir).
//
// Engine `FTraceAuxiliary::Start` returns false if a trace is already running OR
// if the connection can't be made — we surface either as OperationFailed (-32058).
FMCPResponse Tool_StartCapture(const FMCPRequest& Request)
{
	check(IsInGameThread());

	// 1. Parse channels.
	FString ChannelsForEngine;
	TArray<FString> ChannelsForResponse;
	FString ParseErr;
	if (!INS_ParseChannels(Request.Args, ChannelsForEngine, ChannelsForResponse, ParseErr))
	{
		return FMCPToolHelpers::MakeError(Request, kINSErrorInvalidParams, ParseErr);
	}

	// 2. Resolve output_path. If caller omitted, generate a unique default under
	// <Saved>/Profiling/Traces/MCP_<UTC>.utrace.
	FString AbsPath;
	FString OutputPathRaw;
	if (Request.Args.IsValid())
	{
		Request.Args->TryGetStringField(TEXT("output_path"), OutputPathRaw);
	}

	if (OutputPathRaw.IsEmpty())
	{
		const FString DefaultDir = FPaths::ProjectSavedDir() / TEXT("Profiling/Traces/");
		IFileManager::Get().MakeDirectory(*DefaultDir, /*Tree=*/true);
		// ISO 8601 has colons; sanitize for cross-platform filenames.
		FString TimeTag = FDateTime::UtcNow().ToIso8601();
		TimeTag.ReplaceInline(TEXT(":"), TEXT("-"));
		AbsPath = DefaultDir / FString::Printf(TEXT("MCP_%s.utrace"), *TimeTag);
	}
	else
	{
		FString SandboxErr;
		if (!FMCPPathSandbox::Resolve(OutputPathRaw, AbsPath, SandboxErr))
		{
			return FMCPToolHelpers::MakeError(Request, kINSErrorPathEscape,
				FString::Printf(TEXT("output_path '%s' rejected: %s"), *OutputPathRaw, *SandboxErr));
		}
		// Ensure parent directory exists.
		const FString ParentDir = FPaths::GetPath(AbsPath);
		if (!ParentDir.IsEmpty())
		{
			IFileManager::Get().MakeDirectory(*ParentDir, /*Tree=*/true);
		}
	}

	// 3. Start the trace via the engine API. `bNoWorkerThread` was deprecated in 5.7,
	// so we leave it default (false). `bTruncateFile` ensures we overwrite a stale
	// file at the same path rather than failing.
	FTraceAuxiliary::FOptions Opts;
	Opts.bTruncateFile = true;

	const bool bOk = FTraceAuxiliary::Start(
		FTraceAuxiliary::EConnectionType::File,
		*AbsPath,
		*ChannelsForEngine,
		&Opts,
		LogMCP);

	if (!bOk)
	{
		// Either already tracing, or sink couldn't be opened.
		const bool bAlreadyConnected = FTraceAuxiliary::IsConnected();
		return FMCPToolHelpers::MakeError(Request, kINSErrorOperationFailed,
			bAlreadyConnected
				? FString::Printf(TEXT("trace already active at '%s'"), *FTraceAuxiliary::GetTraceDestinationString())
				: FString::Printf(TEXT("FTraceAuxiliary::Start returned false for '%s'"), *AbsPath));
	}

	// 4. Record local start time.
	const FDateTime UtcNow = FDateTime::UtcNow();
	const double NowSec = FPlatformTime::Seconds();
	{
		FScopeLock Lock(&GInsightsLocalStateLock);
		GInsightsLocalState.StartedAtSeconds = NowSec;
		GInsightsLocalState.StartedAtUtc     = UtcNow;
	}

	// 5. Build response.
	FMCPJsonArrayBuilder ChanArr;
	for (const FString& C : ChannelsForResponse)
	{
		ChanArr.AddString(C);
	}

	// Read back the engine's actual destination — it may massage the path (e.g.
	// canonicalise separators on Windows).
	const FString EngineDest = FTraceAuxiliary::GetTraceDestinationString();

	return FMCPJsonBuilder()
		.Str(TEXT("path"), EngineDest.IsEmpty() ? AbsPath : EngineDest)
		.Arr(TEXT("channels"), ChanArr.ToValueArray())
		.Str(TEXT("started_at_utc"), UtcNow.ToIso8601())
		.BuildSuccess(Request);
}

// --- insights.stop_capture --------------------------------------------------------------------
//
// Args:    {}
// Result:  { path, file_size_bytes, elapsed_seconds }
//
// Engine `FTraceAuxiliary::Stop` returns false when no trace is currently active
// — surfaced as OperationFailed (-32058). On success the trace file is flushed
// synchronously, so file_size_bytes is meaningful immediately.
FMCPResponse Tool_StopCapture(const FMCPRequest& Request)
{
	check(IsInGameThread());

	// Capture pre-stop state since FTraceAuxiliary::Stop() clears the engine's
	// internal destination/connection.
	const FString PreStopDestination = FTraceAuxiliary::GetTraceDestinationString();

	double StartedAtSeconds = 0.0;
	{
		FScopeLock Lock(&GInsightsLocalStateLock);
		StartedAtSeconds = GInsightsLocalState.StartedAtSeconds;
	}

	const bool bOk = FTraceAuxiliary::Stop();
	if (!bOk)
	{
		return FMCPToolHelpers::MakeError(Request, kINSErrorOperationFailed,
			TEXT("no trace is currently active (FTraceAuxiliary::Stop returned false)"));
	}

	// Clear local start time.
	{
		FScopeLock Lock(&GInsightsLocalStateLock);
		GInsightsLocalState.StartedAtSeconds = 0.0;
		GInsightsLocalState.StartedAtUtc     = FDateTime(0);
	}

	const double Elapsed = (StartedAtSeconds > 0.0)
		? (FPlatformTime::Seconds() - StartedAtSeconds)
		: 0.0;

	int64 FileSize = 0;
	if (!PreStopDestination.IsEmpty())
	{
		FileSize = IFileManager::Get().FileSize(*PreStopDestination);
		if (FileSize == INDEX_NONE)
		{
			FileSize = 0;
		}
	}

	return FMCPJsonBuilder()
		.Str(TEXT("path"), PreStopDestination)
		.Int(TEXT("file_size_bytes"), FileSize)
		.Num(TEXT("elapsed_seconds"), Elapsed)
		.BuildSuccess(Request);
}

// --- insights.get_status ----------------------------------------------------------------------
//
// Args:    {}
// Result (tracing): { is_tracing: true, current_path, active_channels[], elapsed_seconds, started_at_utc }
// Result (idle):    { is_tracing: false }
//
// Read-only. Reads engine state via FTraceAuxiliary directly (no self-state desync).
FMCPResponse Tool_GetStatus(const FMCPRequest& Request)
{
	check(IsInGameThread());

	const bool bIsTracing = FTraceAuxiliary::IsConnected();

	if (!bIsTracing)
	{
		return FMCPJsonBuilder()
			.Bool(TEXT("is_tracing"), false)
			.BuildSuccess(Request);
	}

	const FString CurrentPath = FTraceAuxiliary::GetTraceDestinationString();
	const TArray<FString> Channels = INS_QueryActiveChannels();

	double StartedAtSeconds = 0.0;
	FDateTime StartedAtUtc(0);
	{
		FScopeLock Lock(&GInsightsLocalStateLock);
		StartedAtSeconds = GInsightsLocalState.StartedAtSeconds;
		StartedAtUtc     = GInsightsLocalState.StartedAtUtc;
	}

	const double Elapsed = (StartedAtSeconds > 0.0)
		? (FPlatformTime::Seconds() - StartedAtSeconds)
		: 0.0;

	FMCPJsonArrayBuilder ChanArr;
	for (const FString& C : Channels)
	{
		ChanArr.AddString(C);
	}

	FMCPJsonBuilder B;
	B.Bool(TEXT("is_tracing"), true)
		.Str(TEXT("current_path"), CurrentPath)
		.Arr(TEXT("active_channels"), ChanArr.ToValueArray())
		.Num(TEXT("elapsed_seconds"), Elapsed);
	// started_at_utc only included if we have a sensible value (trace started by us).
	if (StartedAtUtc.GetTicks() != 0)
	{
		B.Str(TEXT("started_at_utc"), StartedAtUtc.ToIso8601());
	}
	return B.BuildSuccess(Request);
}

// --- insights.list_channels -------------------------------------------------------------------
//
// Args:    {}
// Result:  { channels: [{ name, description, is_enabled, is_read_only }], total_count }
//
// Enumerates the live set via `UE::Trace::EnumerateChannels` (free function in
// Trace/Trace.h — TRACELOG_API exported). The class method
// FChannel::EnumerateChannels exists but is NOT TRACELOG_API, so the free
// function is the only viable entry point from outside the TraceLog module.
// Channel registration is dynamic — modules announce channels at load time — so
// the returned list reflects the current process state.
FMCPResponse Tool_ListChannels(const FMCPRequest& Request)
{
	check(IsInGameThread());

	// Accumulator passed through the C-style callback.
	struct FAcc
	{
		TArray<TSharedPtr<FJsonValue>> Items;
	};
	FAcc Acc;

	UE::Trace::EnumerateChannels(
		[](const UE::Trace::FChannelInfo& Info, void* User) -> bool
		{
			FAcc* A = static_cast<FAcc*>(User);
			TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("name"),         Info.Name ? FString(ANSI_TO_TCHAR(Info.Name)) : FString());
			Obj->SetStringField(TEXT("description"),  Info.Desc ? FString(ANSI_TO_TCHAR(Info.Desc)) : FString());
			Obj->SetNumberField(TEXT("id"),           static_cast<double>(Info.Id));
			Obj->SetBoolField(TEXT("is_enabled"),     Info.bIsEnabled);
			Obj->SetBoolField(TEXT("is_read_only"),   Info.bIsReadOnly);
			A->Items.Add(MakeShared<FJsonValueObject>(Obj));
			return true; // continue
		},
		&Acc);

	const int32 Count = Acc.Items.Num();
	return FMCPJsonBuilder()
		.Arr(TEXT("channels"), MoveTemp(Acc.Items))
		.Int(TEXT("total_count"), Count)
		.BuildSuccess(Request);
}

void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	// All Lane A. Lane B promotion deferred to reviewer audit:
	//   - get_status / list_channels are pure reads, candidates for Lane B with
	//     defense-in-depth lock (already in place for local state). FChannel
	//     enumeration thread-safety is unverified.
	//   - start_capture / stop_capture call into the engine — Lane A only.
	RegisterTool(TEXT("insights.start_capture"), &Tool_StartCapture, /*Lane A*/ false);
	RegisterTool(TEXT("insights.stop_capture"),  &Tool_StopCapture,  /*Lane A*/ false);
	RegisterTool(TEXT("insights.get_status"),    &Tool_GetStatus,    /*Lane A*/ false);
	RegisterTool(TEXT("insights.list_channels"), &Tool_ListChannels, /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("Insights surface registered: insights.{start_capture, stop_capture, get_status, list_channels} (Lane A)"));
}

} // namespace FInsightsTools

MCP_REGISTER_SURFACE(InsightsTools, &FInsightsTools::Register)

#undef LOCTEXT_NAMESPACE
