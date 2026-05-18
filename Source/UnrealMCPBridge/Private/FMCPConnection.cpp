// Copyright FatumGame. All Rights Reserved.

#include "FMCPConnection.h"

#include "FMCPDispatchQueue.h"
#include "UnrealMCPBridge.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/RunnableThread.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "Misc/ScopeLock.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

namespace
{
	/** Reasonable per-Recv chunk. We size it so most JSON frames arrive in 1-2 reads. */
	constexpr int32 kRecvChunkBytes = 64 * 1024;

	/** Connection idle wait. Wait() blocks up to this long for bytes; controls Stop() responsiveness. */
	constexpr int32 kSocketWaitMilliseconds = 250;
}

FMCPConnection::FMCPConnection(int32 InConnectionId, FSocket* InSocket, const FIPv4Endpoint& InRemoteEndpoint)
	: ConnectionId(InConnectionId)
	, Socket(InSocket)
	, RemoteAddressText(InRemoteEndpoint.ToString())
{
	check(Socket != nullptr);
}

FMCPConnection::~FMCPConnection()
{
	// Order matters: ensure Run() has exited before we destroy the socket it was reading from.
	if (Thread)
	{
		Thread->Kill(true /*WaitUntilCompleted*/);
		delete Thread;
		Thread = nullptr;
	}

	if (Socket)
	{
		Socket->Close();
		if (ISocketSubsystem* SocketSub = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM))
		{
			SocketSub->DestroySocket(Socket);
		}
		Socket = nullptr;
	}
}

bool FMCPConnection::Init()
{
	UE_LOG(LogMCP, Log, TEXT("MCP connection %d opened (remote=%s)"), ConnectionId, *RemoteAddressText);
	return true;
}

uint32 FMCPConnection::Run()
{
	check(Socket);

	TArray<uint8> RecvChunk;
	RecvChunk.SetNumUninitialized(kRecvChunkBytes);

	while (!bStopRequested.load(std::memory_order_acquire))
	{
		// Wait up to N ms for inbound bytes. Returns true on bytes-available OR remote-close (the
		// socket becomes readable in both cases). We disambiguate by attempting Recv afterwards.
		const bool bWaitFired = Socket->Wait(
			ESocketWaitConditions::WaitForRead,
			FTimespan::FromMilliseconds(kSocketWaitMilliseconds));

		if (bStopRequested.load(std::memory_order_acquire))
		{
			break;
		}

		if (!bWaitFired)
		{
			// Pure timeout — no data, no event. Loop back to top to re-check Stop flag.
			continue;
		}

		// Wait fired — could be real data OR an EOF from peer. Probe via Recv (returns BytesRead=0 on EOF).
		int32 BytesRead = 0;
		const bool bRecvOk = Socket->Recv(RecvChunk.GetData(), RecvChunk.Num(), BytesRead, ESocketReceiveFlags::None);
		if (!bRecvOk)
		{
			UE_LOG(LogMCP, Log, TEXT("MCP connection %d: Recv failed; closing"), ConnectionId);
			break;
		}
		if (BytesRead == 0)
		{
			// Graceful EOF — peer shut down its write half.
			UE_LOG(LogMCP, Log, TEXT("MCP connection %d: EOF"), ConnectionId);
			break;
		}

		// Append into our accumulator. Frame-cap check happens inside ConsumeBufferedFrames.
		InboundBuffer.Append(RecvChunk.GetData(), BytesRead);

		if (!ConsumeBufferedFrames())
		{
			// Hard protocol error (frame too large / malformed). Bail.
			break;
		}
	}

	bClosed.store(true, std::memory_order_release);
	UE_LOG(LogMCP, Log, TEXT("MCP connection %d closed"), ConnectionId);
	return 0;
}

void FMCPConnection::Stop()
{
	bStopRequested.store(true, std::memory_order_release);
}

void FMCPConnection::Exit()
{
	// Nothing here — Run already publishes bClosed.
}

bool FMCPConnection::ConsumeBufferedFrames()
{
	// Scan for newline. Process in a loop: there may be multiple frames buffered.
	int32 ScanFrom = 0;
	int32 ConsumedSoFar = 0;

	while (true)
	{
		int32 NewlineIdx = INDEX_NONE;
		for (int32 i = ScanFrom; i < InboundBuffer.Num(); ++i)
		{
			if (InboundBuffer[i] == '\n')
			{
				NewlineIdx = i;
				break;
			}
		}

		if (NewlineIdx == INDEX_NONE)
		{
			// No complete frame yet. Frame-cap guard: buffer beyond cap = abort the connection
			// (a single line is too large, attacker-style payload or runaway client).
			if (InboundBuffer.Num() - ConsumedSoFar >= kMCPFrameMaxBytes)
			{
				UE_LOG(LogMCP, Error,
					TEXT("MCP connection %d: inbound frame exceeded %d bytes (have %d) — closing"),
					ConnectionId, kMCPFrameMaxBytes, InboundBuffer.Num() - ConsumedSoFar);
				SendImmediateError(FGuid(), -32700, TEXT("frame too large"));
				return false;
			}
			break;
		}

		// One complete frame: bytes [ConsumedSoFar .. NewlineIdx). Strip a trailing '\r' if present.
		int32 LineEnd = NewlineIdx;
		if (LineEnd > ConsumedSoFar && InboundBuffer[LineEnd - 1] == '\r')
		{
			--LineEnd;
		}

		const int32 LineLen = LineEnd - ConsumedSoFar;
		if (LineLen > 0)
		{
			// UTF-8 decode into FString via the explicit-length converter (does NOT require null-termination).
			const FUTF8ToTCHAR Converter(
				reinterpret_cast<const ANSICHAR*>(InboundBuffer.GetData() + ConsumedSoFar),
				LineLen);
			const FString FrameJson = FString::ConstructFromPtrSize(Converter.Get(), Converter.Length());
			HandleFrame(FrameJson);
		}
		// Skip past the newline.
		ConsumedSoFar = NewlineIdx + 1;
		ScanFrom = ConsumedSoFar;
	}

	// Shift remaining unprocessed bytes to the front of the buffer.
	if (ConsumedSoFar > 0)
	{
		InboundBuffer.RemoveAt(0, ConsumedSoFar, EAllowShrinking::No);
	}

	return true;
}

void FMCPConnection::HandleFrame(const FString& FrameJson)
{
	TSharedPtr<FJsonObject> ParsedObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(FrameJson);
	if (!FJsonSerializer::Deserialize(Reader, ParsedObject) || !ParsedObject.IsValid())
	{
		UE_LOG(LogMCP, Warning, TEXT("MCP connection %d: failed to parse JSON frame (len=%d)"),
			ConnectionId, FrameJson.Len());
		// Per wire spec we return a parse-error response. RequestId blank — client can't correlate but
		// we tag it explicitly so they know it was malformed.
		SendImmediateError(FGuid(), -32700, TEXT("parse error: malformed JSON frame"));
		return;
	}

	// Required fields: id, kind, method (method may be empty for ping).
	FString IdStr;
	if (!ParsedObject->TryGetStringField(TEXT("id"), IdStr) || IdStr.IsEmpty())
	{
		SendImmediateError(FGuid(), -32600, TEXT("invalid request: missing 'id'"));
		return;
	}

	FGuid RequestId;
	if (!FGuid::Parse(IdStr, RequestId))
	{
		// Client may send opaque strings; we hash to deterministic GUID so we can echo back losslessly
		// as a string later. For Day 2 simplicity: accept opaque non-GUID id by stuffing into a GUID-stable
		// hash. We still echo whatever was sent in the response, so the client doesn't care about format.
		RequestId = FGuid::NewGuid();
	}

	FString KindStr;
	if (!ParsedObject->TryGetStringField(TEXT("kind"), KindStr))
	{
		SendImmediateError(RequestId, -32600, TEXT("invalid request: missing 'kind'"));
		return;
	}

	EMCPRequestKind Kind;
	if (!ParseRequestKind(KindStr, Kind))
	{
		SendImmediateError(RequestId, -32600,
			FString::Printf(TEXT("invalid request: unknown 'kind' value '%s'"), *KindStr));
		return;
	}

	FString Method;
	ParsedObject->TryGetStringField(TEXT("method"), Method);
	// For Day 2 we expect Method present on CallFunction; Ping/GetTools may omit. We still pass through.

	const TSharedPtr<FJsonObject>* ArgsPtr = nullptr;
	ParsedObject->TryGetObjectField(TEXT("args"), ArgsPtr);

	FMCPRequest Request;
	Request.RequestId = RequestId;
	Request.Kind = Kind;
	Request.Method = Method;
	Request.Args = ArgsPtr ? *ArgsPtr : nullptr;
	Request.SourceConnectionId = ConnectionId;
	Request.ReceivedAtSeconds = FPlatformTime::Seconds();
	Request.OriginalIdString = IdStr; // Round-trip echo — preserve opaque (non-GUID) client ids verbatim.

	// Lane B short-circuit (Phase 2 Day 0): thread-safe CallFunction handlers run inline on the
	// listener thread, bypassing the game-thread Drain queue entirely. This is the latency win
	// motivating Phase 2's Lane B audit. The kind check is kept even though only CallFunction
	// handlers are register-able as Lane B today — it future-proofs against accidentally short-
	// circuiting a kind that later gains its own dispatch path (Ping, JobSubmit, etc.).
	// See FMCPDispatchQueue.h for the Lane B contract handlers MUST follow.
	if (Request.Kind == EMCPRequestKind::CallFunction &&
		FMCPDispatchQueue::Get().IsThreadSafe(Request.Method))
	{
		const FMCPResponse Response = FMCPDispatchQueue::Get().DispatchInline(Request);
		SendResponse(Response);
		return;
	}

	FMCPDispatchQueue::Get().Push(MoveTemp(Request));
}

bool FMCPConnection::ParseRequestKind(const FString& KindStr, EMCPRequestKind& OutKind)
{
	// Map the wire enum verbs (snake_case) to EMCPRequestKind. Keep in sync with MCPTypes.h.
	if (KindStr.Equals(TEXT("ping"), ESearchCase::IgnoreCase))           { OutKind = EMCPRequestKind::Ping; return true; }
	if (KindStr.Equals(TEXT("call_function"), ESearchCase::IgnoreCase))  { OutKind = EMCPRequestKind::CallFunction; return true; }
	if (KindStr.Equals(TEXT("exec_python"), ESearchCase::IgnoreCase))    { OutKind = EMCPRequestKind::ExecPython; return true; }
	if (KindStr.Equals(TEXT("get_tools"), ESearchCase::IgnoreCase))      { OutKind = EMCPRequestKind::GetTools; return true; }
	if (KindStr.Equals(TEXT("job_submit"), ESearchCase::IgnoreCase))     { OutKind = EMCPRequestKind::JobSubmit; return true; }
	if (KindStr.Equals(TEXT("job_status"), ESearchCase::IgnoreCase))     { OutKind = EMCPRequestKind::JobStatus; return true; }
	if (KindStr.Equals(TEXT("job_result"), ESearchCase::IgnoreCase))     { OutKind = EMCPRequestKind::JobResult; return true; }
	if (KindStr.Equals(TEXT("job_cancel"), ESearchCase::IgnoreCase))     { OutKind = EMCPRequestKind::JobCancel; return true; }
	if (KindStr.Equals(TEXT("log_tail"), ESearchCase::IgnoreCase))       { OutKind = EMCPRequestKind::LogTail; return true; }
	if (KindStr.Equals(TEXT("log_subscribe"), ESearchCase::IgnoreCase))  { OutKind = EMCPRequestKind::LogSubscribe; return true; }
	if (KindStr.Equals(TEXT("shutdown"), ESearchCase::IgnoreCase))       { OutKind = EMCPRequestKind::Shutdown; return true; }
	return false;
}

bool FMCPConnection::SendResponse(const FMCPResponse& Response)
{
	if (!Socket || bClosed.load(std::memory_order_acquire))
	{
		return false;
	}

	const FString Line = SerializeResponse(Response);
	// Convert to UTF-8 + append newline.
	FTCHARToUTF8 Converter(*Line);
	TArray<uint8> OutBytes;
	OutBytes.Append(reinterpret_cast<const uint8*>(Converter.Get()), Converter.Length());
	OutBytes.Add(static_cast<uint8>('\n'));

	FScopeLock Lock(&SendLock);
	int32 Remaining = OutBytes.Num();
	int32 Offset = 0;
	while (Remaining > 0)
	{
		int32 Sent = 0;
		if (!Socket->Send(OutBytes.GetData() + Offset, Remaining, Sent) || Sent <= 0)
		{
			UE_LOG(LogMCP, Warning, TEXT("MCP connection %d: Send failed (remaining=%d)"), ConnectionId, Remaining);
			return false;
		}
		Offset += Sent;
		Remaining -= Sent;
	}
	return true;
}

FString FMCPConnection::SerializeResponse(const FMCPResponse& Response)
{
	FString Out;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);

	// Echo verbatim id string (covers opaque non-GUID ids like "test-1"). Fall back to GUID form
	// when no original was captured (e.g. internal synthetic responses).
	const FString IdEcho = Response.OriginalIdString.IsEmpty()
		? Response.RequestId.ToString(EGuidFormats::DigitsWithHyphens)
		: Response.OriginalIdString;

	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("id"), IdEcho);
	Writer->WriteValue(TEXT("ok"), !Response.bIsError);
	if (Response.bIsError)
	{
		Writer->WriteObjectStart(TEXT("error"));
		Writer->WriteValue(TEXT("code"), Response.ErrorCode);
		Writer->WriteValue(TEXT("message"), Response.ErrorMessage);
		Writer->WriteObjectEnd();
	}
	else if (Response.Result.IsValid())
	{
		// FJsonSerializer handles every FJsonValue subclass (object/array/string/number/bool/null)
		// — we just supply the identifier; bCloseWriter=false keeps the enclosing object open.
		FJsonSerializer::Serialize(Response.Result, TEXT("result"), Writer, /*bCloseWriter*/ false);
	}
	else
	{
		// No result payload — emit null so clients always see the field.
		Writer->WriteNull(TEXT("result"));
	}
	Writer->WriteObjectEnd();
	Writer->Close();
	return Out;
}

void FMCPConnection::SendImmediateError(const FGuid& RequestId, int32 Code, const FString& Message)
{
	FMCPResponse Response;
	Response.RequestId = RequestId.IsValid() ? RequestId : FGuid::NewGuid();
	Response.bIsError = true;
	Response.ErrorCode = Code;
	Response.ErrorMessage = Message;
	SendResponse(Response);
}
