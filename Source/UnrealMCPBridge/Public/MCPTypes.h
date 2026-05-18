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
 * Phase 3 — Level + Actor + Component surface. 10 of 11 codes wired in Days 1-8 (-32024 lands
 * in Days 9-10 with the Component tools that surface the ambiguity case).
 *
 *   -32019 LevelNotFound                Map asset path resolves to no UWorld (level.load /
 *                                       level.save / level.unload / level.set_streaming_state).
 *                                       Also used for "actor's owning sublevel not visible/loaded"
 *                                       by every actor mutator (D18 sublevel-visibility guard).
 *   -32020 ClassNotFound                Phase 3 Day 4+ actor.spawn — class path resolved but failed
 *                                       autoload OR the class is abstract / wrong family. Subcodes
 *                                       -32021/-32022/-32023 narrow the diagnosis.
 *   -32021 ClassAbstract                actor.spawn target UClass has CLASS_Abstract — cannot be
 *                                       instantiated. Caller picks a concrete subclass.
 *   -32022 WrongClassFamily             actor.spawn class_path resolves to a UClass that is NOT a
 *                                       subclass of AActor (e.g. UActorComponent path passed).
 *   -32023 InvalidClassPath             actor.spawn class_path is syntactically malformed — bare
 *                                       name without /Script/... or /Game/... mount, missing
 *                                       leading slash, contains backslash, etc.
 *   -32025 PropertyPathTooDeep          Property path nesting exceeds the hard cap (16 segments).
 *                                       Prevents pathological recursion / OOM on user-supplied
 *                                       paths. Mirrors the FMCPPropertyPathParser depth check.
 *   -32026 PropertyIndexOOB             Property path used [N] indexing past the array's bounds.
 *                                       Surfaced by actor.get_property / actor.set_property when
 *                                       the indexed segment is out of range.
 *   -32027 PIEActive                    Editor-world mutator refused because GEditor->PlayWorld is
 *                                       non-null. Message is frozen verbatim (see D10 in the plan):
 *                                       "editor-world mutators are unavailable during PIE; Phase 5
 *                                       will add pie.* tools for PIE world; otherwise stop PIE first
 *                                       if you meant the editor world."
 *   -32028 LevelNotStreamingEntry       level.set_streaming_state target is loaded but is NOT in
 *                                       World->GetStreamingLevels() (e.g. the persistent level
 *                                       itself, or a level loaded by some other code path).
 *                                       Also used by actor.attach when child + parent live in
 *                                       different sublevels (cross-level attach hazard).
 *   -32029 WorldPartitionNotSupported   Phase 3 hard-rejects World Partition maps — they have a
 *                                       fundamentally different streaming model (cells, not
 *                                       ULevelStreamingDynamic) and Phase 5 will ship a dedicated
 *                                       wp.* surface for them. Detected via UWorld::IsPartitionedWorld().
 */
inline constexpr int32 kMCPErrorLevelNotFound               = -32019;
inline constexpr int32 kMCPErrorClassNotFound               = -32020;
inline constexpr int32 kMCPErrorClassAbstract               = -32021;
inline constexpr int32 kMCPErrorWrongClassFamily            = -32022;
inline constexpr int32 kMCPErrorInvalidClassPath            = -32023;
inline constexpr int32 kMCPErrorPropertyPathTooDeep         = -32025;
inline constexpr int32 kMCPErrorPropertyIndexOOB            = -32026;
inline constexpr int32 kMCPErrorPIEActive                   = -32027;
inline constexpr int32 kMCPErrorLevelNotStreamingEntry      = -32028;
inline constexpr int32 kMCPErrorWorldPartitionNotSupported  = -32029;

/**
 * Frozen wire message returned by every Phase 3+ editor-world mutator when PIE is active.
 * **Do NOT edit this string** — smoke tests assert both substrings ``"Phase 5"`` AND ``"pie."``.
 */
inline const TCHAR* const kMCPMessagePIEActive = TEXT(
	"editor-world mutators are unavailable during PIE; Phase 5 will add pie.* tools for PIE world; "
	"otherwise stop PIE first if you meant the editor world.");

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
