// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Guid.h"

/** Default TCP port for the MCP bridge listener (loopback only). */
inline constexpr int32 kMCPDefaultPort = 30020;

/**
 * Per-line frame cap (bytes). Raised from v1's 8 MiB to 64 MiB per blueprint v2 §C6 — large
 * screenshot/thumbnail payloads exceed the older limit even after base64 wrapping. Any single
 * inbound line larger than this aborts the connection (DoS guard).
 */
inline constexpr int32 kMCPFrameMaxBytes = 64 * 1024 * 1024;

/**
 * Server-defined error codes (JSON-RPC -32000..-32099 range). Day 4-5 marshalling layer adds 4
 * codes around generic property reflection — kept here so Python tools and tests can refer to the
 * same numeric constants. Mirrored in `MCPTools/marshall.py` constants where appropriate.
 *
 *   -32004 ObjectNotFound         LoadObject(path) returned null
 *   -32005 PropertyNotFound       FindPropertyByName on a path segment failed
 *   -32006 PropertyTypeMismatch   Property exists but cast (e.g. expected FStructProperty) failed
 *                                 OR ImportText_Direct could not parse the supplied JSON value
 *   -32007 PropertyAccessDenied   Property hit a CPF_EditorOnly / CPF_BlueprintReadOnly guard at
 *                                 runtime (write-side only)
 */
inline constexpr int32 kMCPErrorObjectNotFound       = -32004;
inline constexpr int32 kMCPErrorPropertyNotFound     = -32005;
inline constexpr int32 kMCPErrorPropertyTypeMismatch = -32006;
inline constexpr int32 kMCPErrorPropertyAccessDenied = -32007;

/**
 * Phase 2 — Assets + Content Browser surface (30 tools across `asset.*` / `cb.*`).
 *
 *   -32010 InvalidPath            Path is empty, contains backslashes, contains `..`, OR is not a
 *                                 recognized mount point (/Game//Engine//Plugins/<name>//Script/...).
 *                                 Used by every tool that takes an asset/folder path argument.
 *   -32011 WrongClass             Provided class_path doesn't resolve to a UClass. Used by
 *                                 asset.search_by_class.
 *   -32012 OverlyBroadQuery       Caller asked for a recursive /Game scan with no narrowing filters
 *                                 (asset.list / asset.search_by_class) OR exceeded the 500-redirector
 *                                 hard cap (cb.fix_redirectors) OR exceeded the 10k visited-set in
 *                                 the recursive ref/dep walk OR exceeded the 5000-asset hard cap on
 *                                 asset.find_unused / asset.find_broken_references / asset._size_report_internal.
 *   -32013 PathEscape             Disk path argument (cb.export dest_file, cb.import source_file,
 *                                 asset.get_thumbnail_to_disk output_path) resolves outside the
 *                                 sandbox whitelist (project / saved / intermediate / engine).
 *   -32014 PathInUse              cb.rename / cb.duplicate target path already exists.
 *   -32015 StaleCursor            asset.list / asset.find_references / asset.find_dependents /
 *                                 asset.search_by_* page_token's embedded filter_hash doesn't match
 *                                 the current call's filter — caller changed the filter mid-pagination.
 *   -32016 JobSubmitFailed        FMCPJobRegistry::SubmitJob failed (registry not initialized).
 *                                 Used by cb.save_all_dirty, cb.bulk_import, asset.batch_metadata_async.
 *   -32017 InputTooLarge          paths array exceeds the synchronous-batch limit (asset.batch_metadata
 *                                 cap = 200).
 *   -32018 ThumbnailRenderFailed  asset.get_thumbnail / asset.get_thumbnail_to_disk could not render
 *                                 the thumbnail bitmap (load failure, RenderThumbnail returned empty
 *                                 result, PNG/JPG encode failed, output write failed, or encoded
 *                                 payload exceeded 2 GiB limit).
 */
inline constexpr int32 kMCPErrorInvalidPath              = -32010;
inline constexpr int32 kMCPErrorWrongClass               = -32011;
inline constexpr int32 kMCPErrorOverlyBroadQuery         = -32012;
inline constexpr int32 kMCPErrorPathEscape               = -32013;
inline constexpr int32 kMCPErrorPathInUse                = -32014;
inline constexpr int32 kMCPErrorStaleCursor              = -32015;
inline constexpr int32 kMCPErrorJobSubmitFailed          = -32016;
inline constexpr int32 kMCPErrorInputTooLarge            = -32017;
inline constexpr int32 kMCPErrorThumbnailRenderFailed    = -32018;

/**
 * Distinguishes the high-level kind of a wire request.
 *
 * Phase 1 baseline — values map 1:1 to the protocol verbs described in the
 * v2 blueprint (D:/tmp/mcp_unreal_blueprint_v2_patch.md §3). Additional kinds
 * (live coding, transactions, etc.) will be appended in later phases; existing
 * values MUST stay numerically stable so wire-format remains backward compatible.
 */
enum class EMCPRequestKind : uint8
{
	Ping,
	CallFunction,
	ExecPython,
	GetTools,
	JobSubmit,
	JobStatus,
	JobResult,
	JobCancel,
	LogTail,
	LogSubscribe,
	Shutdown
};

/**
 * In-process request POD. Built by the TCP listener after JSON parsing and handed
 * off to the dispatch queue. Phase 1 Day 1: type declared only; no producers yet.
 *
 * Args ownership: the shared pointer is the canonical home for parsed JSON during
 * the request's lifetime. Do not retain raw pointers into Args past completion.
 */
struct FMCPRequest
{
	/** Unique request id assigned by the server (correlates with FMCPResponse::RequestId). */
	FGuid RequestId;

	/** High-level kind dispatched on by the worker. */
	EMCPRequestKind Kind = EMCPRequestKind::Ping;

	/** For CallFunction / GetTools / JobSubmit etc., the method/tool name. */
	FString Method;

	/** Parsed args object. May be null for kinds that carry no payload (Ping, GetTools). */
	TSharedPtr<FJsonObject> Args;

	/** Opaque listener-side connection id; used for routing async responses. */
	int32 SourceConnectionId = INDEX_NONE;

	/** Wall-clock seconds when the listener accepted this request (for timeouts/metrics). */
	double ReceivedAtSeconds = 0.0;

	/**
	 * Verbatim "id" string from the wire frame. Preserved so we can echo back exactly what the
	 * client sent — even if it wasn't GUID-shaped (e.g. "test-1"). RequestId above is synthesised
	 * from this when it's not parseable as a GUID; OriginalIdString is the source of truth for
	 * round-trip correlation.
	 */
	FString OriginalIdString;
};

/**
 * In-process response POD. Built by the dispatcher; serialised back to JSON by the listener.
 *
 * Error semantics:
 * - bIsError=true MUST be paired with non-empty ErrorMessage and a non-zero ErrorCode.
 * - ErrorCode follows JSON-RPC 2.0 conventions (-32000..-32099 = server-defined errors).
 */
struct FMCPResponse
{
	/** Echo of FMCPRequest::RequestId for correlation. */
	FGuid RequestId;

	/**
	 * Verbatim "id" string copied from FMCPRequest::OriginalIdString. Serialised as the response "id"
	 * field on the wire so opaque (non-GUID) client ids round-trip exactly. Empty falls back to
	 * RequestId.ToString(EGuidFormats::DigitsWithHyphens).
	 */
	FString OriginalIdString;

	/** True iff this is a structured error rather than a successful result. */
	bool bIsError = false;

	/** Set when bIsError=true. Zero on success. */
	int32 ErrorCode = 0;

	/** Human-readable diagnostic; empty on success. */
	FString ErrorMessage;

	/** Tool-defined result payload (any JSON value). Null on error. */
	TSharedPtr<FJsonValue> Result;
};
