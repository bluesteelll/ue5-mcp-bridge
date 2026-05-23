// Copyright FatumGame. All Rights Reserved.

#include "TestTools.h"

#include "MCPSurfaceRegistry.h"

#include "FMCPDispatchQueue.h"
#include "MCPJsonBuilder.h"
#include "MCPToolHelpers.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPPageCursor.h"

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/AutomationEvent.h"
#include "Misc/DateTime.h"
#include "Misc/EnumClassFlags.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// TST_ prefix per the unity-build symbol-collision pattern. Per-surface error constants kept;
	// XX_StampIds/MakeError/MakeSuccessObj removed in Phase 3 — use FMCPToolHelpers::Xxx
	// from MCPToolHelpers.h.
	constexpr int32 kTSTErrorInvalidParams = -32602;

	// Phase 4.2-final (2026-05-22): module-scoped lock serializing Lane B readers vs Lane A
	// writers on FAutomationTestFramework state. The framework's TestInfo map + RequestedTestFilter
	// + CurrentTest pointer are mutated by run_single_test / cancel_current / set_filter_flags
	// (Lane A) and read by list_automation_specs / list_categories / get_test_info / get_last_results
	// (now Lane B). Without this lock, Lane B readers would torn-read RequestedTestFilter or race
	// the CurrentTest pointer transitions.
	FCriticalSection gAutomationFrameworkLock;

	// Wall-clock cap for the sync ``test.run_single_test`` path. Smoke tests routinely complete
	// well under 1s; anything over this should be invoked via ``test.run_automation`` (async,
	// pollable, no wall-clock cap). 30s is generous enough for "small product tests" while still
	// fast-failing pathological cases.
	constexpr double kTSTSingleTestMaxSeconds = 30.0;

	// Per-tick sleep between latent-command drains in the sync runner — keeps GT freed up between
	// drains so other systems (rendering, slate) aren't starved. 5ms is a reasonable cadence —
	// at 200Hz drain rate we'd consume <1% of the GT for a 30s ceiling.
	constexpr float kTSTSingleTestDrainSleepSeconds = 0.005f;

	// Default page size for ``test.list_automation_specs``. Mirrors Phase 4 bp.list_* defaults.
	constexpr int32 kTSTDefaultPageSize = 100;

	int32 TST_ClampPageSize(const TSharedPtr<FJsonObject>& Args, const TCHAR* FieldName, int32 Default)
	{
		int32 Out = Default;
		if (Args.IsValid())
		{
			Args->TryGetNumberField(FieldName, Out);
		}
		return FMath::Clamp(Out, 1, 1000);
	}

	bool TST_DecodeCursor(
		const FMCPRequest& Request,
		const FString& TokenWire,
		uint64 ExpectedFilterHash,
		FMCPPageCursor& OutCursor,
		FMCPResponse& OutError)
	{
		FString DecodeErr;
		if (!FMCPPageCursorUtils::Decode(TokenWire, OutCursor, DecodeErr))
		{
			OutError = FMCPToolHelpers::MakeError(Request, kMCPErrorStaleCursor,
				FString::Printf(TEXT("page_token decode failed: %s"), *DecodeErr));
			return false;
		}
		if (!FMCPPageCursorUtils::ValidateAgainstFilter(OutCursor, ExpectedFilterHash))
		{
			OutError = FMCPToolHelpers::MakeError(Request, kMCPErrorStaleCursor,
				TEXT("page_token filter_hash mismatch — caller mutated 'filter' between pages; "
					 "restart pagination with page_token=null"));
			return false;
		}
		return true;
	}

	/**
	 * Stable filter hash for paginated ``test.list_automation_specs``. The optional ``filter``
	 * substring narrows the snapshot returned by GetValidTestNames; mutating the substring between
	 * pages would skew which tests are visible per page → -32015 StaleCursor.
	 */
	uint64 TST_HashFilter(const FString& FilterSubstring)
	{
		const uint32 H1 = GetTypeHash(FilterSubstring);
		return static_cast<uint64>(H1);
	}

	/**
	 * Map ``EAutomationTestFlags`` mask → JSON array of human-readable flag names. Walks the
	 * Engine-provided flag-name table; entries are added in stable map-iteration order, which the
	 * caller can sort if desired. Single-bit masks come through verbatim; combined masks (e.g.
	 * ``PriorityMask``) are NOT emitted to avoid double-coverage with their components.
	 */
	TArray<TSharedPtr<FJsonValue>> TST_FlagsToJsonArray(EAutomationTestFlags Flags)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		const TMap<FString, EAutomationTestFlags>& Map = EAutomationTestFlags_GetTestFlagsMap();
		for (const TPair<FString, EAutomationTestFlags>& Pair : Map)
		{
			// Skip composite "mask" entries — they have multiple bits set; the per-bit entries
			// cover them already. We detect composites by checking for >1 bit in the mask.
			const uint64 Raw = static_cast<uint64>(static_cast<uint32>(Pair.Value));
			if (FMath::CountBits(Raw) != 1)
			{
				continue;
			}
			if (EnumHasAnyFlags(Flags, Pair.Value))
			{
				Out.Add(MakeShared<FJsonValueString>(Pair.Key));
			}
		}
		return Out;
	}

	/**
	 * Lookup a flag name in the Engine's flag-name table. Case-sensitive (matches the table's
	 * native case). Returns true + populates OutFlag on success.
	 */
	bool TST_LookupFlag(const FString& Name, EAutomationTestFlags& OutFlag)
	{
		const TMap<FString, EAutomationTestFlags>& Map = EAutomationTestFlags_GetTestFlagsMap();
		if (const EAutomationTestFlags* Found = Map.Find(Name))
		{
			OutFlag = *Found;
			return true;
		}
		return false;
	}

	/**
	 * Marshall ``FAutomationTestInfo`` into the spec JSON shape used by
	 * ``test.list_automation_specs`` and ``test.get_test_info``.
	 *
	 * Shape:
	 *   {
	 *     "name":             "DisplayName",
	 *     "full_path":        "FullTestPath" (dot-separated; canonical identifier),
	 *     "command_line":     "TestName"     (the string accepted by StartTestByName),
	 *     "category":         "<first segment of FullTestPath>" or "" if FullTestPath empty,
	 *     "test_tags":        "TestTags" (concatenated string from RegisterAutomationTestTags),
	 *     "parameter":        "TestParameter" (asset name / map name for parameterised tests),
	 *     "source_file":      "SourceFile",
	 *     "source_line":      int,
	 *     "asset_path":       "AssetPath",
	 *     "open_command":     "OpenCommand",
	 *     "num_participants": int,
	 *     "flags":            ["EditorContext", "SmokeFilter", ...],
	 *     "flags_raw":        int  (raw bitmask cast — for callers that want the verbatim bits)
	 *   }
	 *
	 * ``category`` is derived locally (Epic doesn't expose it) — first dot-separated segment of
	 * FullTestPath. Empty if FullTestPath itself is empty.
	 */
	TSharedRef<FJsonObject> TST_BuildSpecJson(const FAutomationTestInfo& Info)
	{
		// Derive category = first dot-separated segment.
		FString Category;
		const FString& FullPath = Info.GetFullTestPath();
		if (!FullPath.IsEmpty())
		{
			int32 DotIdx = INDEX_NONE;
			if (FullPath.FindChar(TEXT('.'), DotIdx))
			{
				Category = FullPath.Left(DotIdx);
			}
			else
			{
				Category = FullPath;
			}
		}

		return FMCPJsonBuilder()
			.Str(TEXT("name"),             Info.GetDisplayName())
			.Str(TEXT("full_path"),        Info.GetFullTestPath())
			.Str(TEXT("command_line"),     Info.GetTestName())
			.Str(TEXT("category"),         Category)
			.Str(TEXT("test_tags"),        Info.GetTestTags())
			.Str(TEXT("parameter"),        Info.GetTestParameter())
			.Str(TEXT("source_file"),      Info.GetSourceFile())
			.Num(TEXT("source_line"),      static_cast<double>(Info.GetSourceFileLine()))
			.Str(TEXT("asset_path"),       Info.GetAssetPath())
			.Str(TEXT("open_command"),     Info.GetOpenCommand())
			.Num(TEXT("num_participants"), static_cast<double>(Info.GetNumParticipantsRequired()))
			.Arr(TEXT("flags"),            TST_FlagsToJsonArray(Info.GetTestFlags()))
			.Num(TEXT("flags_raw"),        static_cast<double>(static_cast<uint32>(Info.GetTestFlags())))
			.ToShared();
	}

	/**
	 * Stable comparator for FAutomationTestInfo sort by FullTestPath (case-insensitive). Same
	 * sort key used by the FMCPPageCursor for paginated enumeration.
	 */
	bool TST_SpecLess(const FAutomationTestInfo& A, const FAutomationTestInfo& B)
	{
		return A.GetFullTestPath().Compare(B.GetFullTestPath(), ESearchCase::IgnoreCase) < 0;
	}

	/**
	 * Look up a single test by exact-match against FullTestPath in the current framework's
	 * registered tests. Returns true + populates OutInfo on success. Lookup is exact-match,
	 * case-sensitive (framework's native comparison).
	 *
	 * Cost: O(N) — walks ``GetValidTestNames``. Acceptable for one-off lookups (single test);
	 * for bulk lookups callers should use ``test.list_automation_specs`` once and filter
	 * client-side.
	 */
	bool TST_FindTestByFullPath(const FString& FullTestPath, FAutomationTestInfo& OutInfo)
	{
		TArray<FAutomationTestInfo> AllTests;
		FAutomationTestFramework::Get().GetValidTestNames(AllTests);
		for (const FAutomationTestInfo& Info : AllTests)
		{
			if (Info.GetFullTestPath().Equals(FullTestPath, ESearchCase::CaseSensitive))
			{
				OutInfo = Info;
				return true;
			}
		}
		return false;
	}

	/**
	 * Marshall ``EAutomationEventType`` enum → wire string.
	 */
	const TCHAR* TST_EventTypeToString(EAutomationEventType Type)
	{
		switch (Type)
		{
			case EAutomationEventType::Info:    return TEXT("info");
			case EAutomationEventType::Warning: return TEXT("warning");
			case EAutomationEventType::Error:   return TEXT("error");
		}
		return TEXT("unknown");
	}

	/**
	 * Build the per-entry JSON for one ``FAutomationExecutionEntry`` (used by
	 * ``test.run_single_test`` and ``test.get_last_results``).
	 */
	TSharedRef<FJsonObject> TST_BuildEntryJson(const FAutomationExecutionEntry& Entry)
	{
		return FMCPJsonBuilder()
			.Str(TEXT("type"),      TST_EventTypeToString(Entry.Event.Type))
			.Str(TEXT("message"),   Entry.Event.Message)
			.Str(TEXT("context"),   Entry.Event.Context)
			.Str(TEXT("filename"),  Entry.Filename)
			.Num(TEXT("line"),      static_cast<double>(Entry.LineNumber))
			.Str(TEXT("timestamp"), Entry.Timestamp.ToIso8601())
			.ToShared();
	}

	/**
	 * Walk an FAutomationTestExecutionInfo and split entries into errors / warnings / info arrays
	 * for the canonical result shape used by sync run + last-results readers.
	 */
	void TST_PartitionEntries(const FAutomationTestExecutionInfo& Info,
		TArray<TSharedPtr<FJsonValue>>& OutErrors,
		TArray<TSharedPtr<FJsonValue>>& OutWarnings,
		TArray<TSharedPtr<FJsonValue>>& OutInfo)
	{
		OutErrors.Reset();
		OutWarnings.Reset();
		OutInfo.Reset();
		for (const FAutomationExecutionEntry& Entry : Info.GetEntries())
		{
			TSharedRef<FJsonObject> EntryJson = TST_BuildEntryJson(Entry);
			switch (Entry.Event.Type)
			{
				case EAutomationEventType::Error:
					OutErrors.Add(MakeShared<FJsonValueObject>(EntryJson));
					break;
				case EAutomationEventType::Warning:
					OutWarnings.Add(MakeShared<FJsonValueObject>(EntryJson));
					break;
				case EAutomationEventType::Info:
				default:
					OutInfo.Add(MakeShared<FJsonValueObject>(EntryJson));
					break;
			}
		}
	}
} // namespace

namespace FTestTools
{

// ─── test.list_automation_specs ───────────────────────────────────────────────────────────────
//
// Args:    { filter?: string, page_token?: string, page_size?: int (default 100, max 1000) }
//          ``filter`` substring matched against FullTestPath (case-insensitive). Omit/empty = all.
// Result:  { specs: [<spec json>, ...], next_page_token?: string | null, total_known: int,
//            filter_echo: string }
//
// Errors:
//   -32015 StaleCursor    page_token's embedded filter_hash doesn't match the current filter
//
// Sort key: FullTestPath (case-insensitive). FilterHash includes the ``filter`` substring so
// changing the filter between pages → -32015. Caller restarts pagination with page_token=null.
//
// ``total_known`` reports the count AFTER filter application (= filtered universe size, not the
// full registered count). The first page's total_known is captured in the cursor for callers who
// want a stable snapshot; later pages echo the live count, which may drift if tests register/
// unregister mid-pagination (rare; would also affect the page_token validity in practice).
FMCPResponse Tool_ListAutomationSpecs(const FMCPRequest& Request)
{
	// Phase 4.2-final: Lane B with gAutomationFrameworkLock serializing reader vs Lane A writers.
	FScopeLock Lock(&gAutomationFrameworkLock);

	FString Filter;
	if (Request.Args.IsValid())
	{
		Request.Args->TryGetStringField(TEXT("filter"), Filter);
	}

	const uint64 FilterHash = TST_HashFilter(Filter);

	FString TokenWire;
	if (Request.Args.IsValid())
	{
		Request.Args->TryGetStringField(TEXT("page_token"), TokenWire);
	}
	FMCPPageCursor Cursor;
	FMCPResponse CursorErr;
	if (!TST_DecodeCursor(Request, TokenWire, FilterHash, Cursor, CursorErr))
	{
		return CursorErr;
	}

	const int32 PageSize = TST_ClampPageSize(Request.Args, TEXT("page_size"), kTSTDefaultPageSize);

	// Enumerate. We must call GetValidTestNames every request — there's no public stable cursor
	// into the framework's internal map, and tests can register/unregister between pages. The
	// FilterHash + page_token sentinel makes this consistent.
	TArray<FAutomationTestInfo> AllTests;
	FAutomationTestFramework::Get().GetValidTestNames(AllTests);

	// Apply optional substring filter (case-insensitive against FullTestPath).
	TArray<FAutomationTestInfo> Filtered;
	if (Filter.IsEmpty())
	{
		Filtered = MoveTemp(AllTests);
	}
	else
	{
		Filtered.Reserve(AllTests.Num());
		for (FAutomationTestInfo& Info : AllTests)
		{
			if (Info.GetFullTestPath().Contains(Filter, ESearchCase::IgnoreCase))
			{
				Filtered.Add(MoveTemp(Info));
			}
		}
	}

	// Sort by FullTestPath (case-insensitive) — stable cursor sort key.
	Filtered.StableSort(&TST_SpecLess);

	// Skip past sentinel: LastAssetPath holds the previous page's last FullTestPath (case-insensitive).
	int32 StartIdx = 0;
	if (!Cursor.LastAssetPath.IsEmpty())
	{
		while (StartIdx < Filtered.Num())
		{
			if (Filtered[StartIdx].GetFullTestPath().Compare(Cursor.LastAssetPath, ESearchCase::IgnoreCase) > 0)
			{
				break;
			}
			++StartIdx;
		}
	}
	const int32 EndIdxExcl = FMath::Min(Filtered.Num(), StartIdx + PageSize);

	TArray<TSharedPtr<FJsonValue>> Items;
	Items.Reserve(EndIdxExcl - StartIdx);
	FString LastEmittedKey;
	for (int32 i = StartIdx; i < EndIdxExcl; ++i)
	{
		Items.Add(MakeShared<FJsonValueObject>(TST_BuildSpecJson(Filtered[i])));
		LastEmittedKey = Filtered[i].GetFullTestPath();
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetArrayField(TEXT("specs"), Items);
	Out->SetNumberField(TEXT("total_known"), static_cast<double>(Filtered.Num()));
	Out->SetStringField(TEXT("filter_echo"), Filter);

	if (EndIdxExcl < Filtered.Num() && !LastEmittedKey.IsEmpty())
	{
		FMCPPageCursor NextCursor;
		NextCursor.FilterHash = FilterHash;
		NextCursor.LastAssetPath = LastEmittedKey;
		NextCursor.TotalKnownSnapshot = Filtered.Num();
		Out->SetStringField(TEXT("next_page_token"), FMCPPageCursorUtils::Encode(NextCursor));
	}
	else
	{
		Out->SetField(TEXT("next_page_token"), MakeShared<FJsonValueNull>());
	}
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── test.run_single_test ─────────────────────────────────────────────────────────────────────
//
// Args:    { test_name: string }    (required; exact match against FullTestPath)
// Result:  { name, succeeded: bool, completed: bool, cap_exceeded: bool, duration_secs: float,
//            cap_secs: float, errors[], warnings[], info[], error_count: int, warning_count: int }
//
// Errors:
//   -32602 InvalidParams    missing/empty test_name
//   -32046 TestNotFound     framework's GetValidTestNames doesn't include this FullTestPath
//
// **Sync-but-bounded.** We pump latent commands in-process via ``ExecuteLatentCommands`` (returns
// true when the queue is empty AND the test is complete). Wall-clock cap (``cap_secs``) kicks in
// if a test inadvertently runs longer than expected — the response carries ``cap_exceeded=true``,
// ``succeeded=false``, ``completed=false`` and the partial execution info captured at the moment
// of timeout. Wire envelope is ALWAYS ok=true on cap-exceeded (FMCPResponse can't carry a Result
// alongside an error per the serializer) so the caller distinguishes via the boolean flags.
// Callers expecting long runs MUST use ``test.run_automation`` (async, no cap).
//
// We also drive ``ExecuteNetworkCommands`` per loop iteration in case a registered test enqueued
// any — most local smoke tests don't, but this keeps single-machine network-tagged tests viable
// (they no-op when there's no peer).
FMCPResponse Tool_RunSingleTest(const FMCPRequest& Request)
{
	check(IsInGameThread());
	// Phase 4.2-final: writer holds gAutomationFrameworkLock for the FULL StartTestByName +
	// ExecuteLatentCommands + StopTest block (up to kMaxRunTimeSeconds = 30s). Lane B readers
	// poll this lock and queue up while a test is running — acceptable since the alternative is
	// torn reads of the framework's CurrentTest pointer / RequestedTestFilter field.
	FScopeLock Lock(&gAutomationFrameworkLock);

	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kTSTErrorInvalidParams, TEXT("missing args object"));
	}
	FString TestName;
	if (!Request.Args->TryGetStringField(TEXT("test_name"), TestName) || TestName.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kTSTErrorInvalidParams,
			TEXT("missing required string field 'test_name'"));
	}

	// Validate the test exists in the framework's registered set. ContainsTest matches by
	// TestName (the command-line form), but FullTestPath is the canonical identifier we accept
	// from the caller. Walk via FindTestByFullPath for exact match.
	FAutomationTestInfo Info;
	if (!TST_FindTestByFullPath(TestName, Info))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorTestNotFound,
			FString::Printf(TEXT("automation test '%s' not found; see test.list_automation_specs"),
				*TestName));
	}

	FAutomationTestFramework& Framework = FAutomationTestFramework::Get();

	// Start the test. StartTestByName uses TestName (command-line form, NOT FullTestPath). The
	// FullTestPath optional arg is forwarded for completeness so the framework records the
	// canonical path.
	const double StartSeconds = FPlatformTime::Seconds();
	Framework.StartTestByName(Info.GetTestName(), /*InRoleIndex*/ 0, Info.GetFullTestPath());

	// Pump latent + network commands until the test reports complete OR we exceed the wall-clock
	// cap. ExecuteLatentCommands returns true when "the latent command queue is now empty AND
	// the test is complete" — per Epic's doc-comment.
	bool bCompleted = false;
	bool bCapHit = false;
	while (true)
	{
		// Network commands typically no-op in single-process; still drive them in case a test
		// enqueued any from inside its lambda body.
		Framework.ExecuteNetworkCommands();
		if (Framework.ExecuteLatentCommands())
		{
			bCompleted = true;
			break;
		}
		const double Now = FPlatformTime::Seconds();
		if (Now - StartSeconds > kTSTSingleTestMaxSeconds)
		{
			bCapHit = true;
			// Best-effort abort: clear remaining latents so StopTest sees a clean queue.
			Framework.DequeueAllCommands();
			break;
		}
		FPlatformProcess::Sleep(kTSTSingleTestDrainSleepSeconds);
	}

	// Collect results — StopTest both terminates the run AND populates the OutExecutionInfo.
	FAutomationTestExecutionInfo ExecInfo;
	const bool bSuccessful = Framework.StopTest(ExecInfo);
	const double DurationSeconds = FPlatformTime::Seconds() - StartSeconds;

	TArray<TSharedPtr<FJsonValue>> ErrorsArr;
	TArray<TSharedPtr<FJsonValue>> WarningsArr;
	TArray<TSharedPtr<FJsonValue>> InfoArr;
	TST_PartitionEntries(ExecInfo, ErrorsArr, WarningsArr, InfoArr);

	// Always return ok=true with the result payload. ``cap_exceeded=true`` + ``succeeded=false``
	// + ``completed=false`` is the cap-hit signal — caller branches on those flags rather than
	// the wire envelope. (FMCPResponse's serializer can't carry a Result block alongside an
	// error block, so we'd lose the partial results if we returned an error envelope.)
	return FMCPJsonBuilder()
		.Str (TEXT("name"),          Info.GetFullTestPath())
		.Bool(TEXT("succeeded"),     bSuccessful && !bCapHit)
		.Bool(TEXT("completed"),     bCompleted)
		.Bool(TEXT("cap_exceeded"),  bCapHit)
		.Num (TEXT("duration_secs"), DurationSeconds)
		.Num (TEXT("cap_secs"),      kTSTSingleTestMaxSeconds)
		.Arr (TEXT("errors"),        MoveTemp(ErrorsArr))
		.Arr (TEXT("warnings"),      MoveTemp(WarningsArr))
		.Arr (TEXT("info"),          MoveTemp(InfoArr))
		.Num (TEXT("error_count"),   static_cast<double>(ExecInfo.GetErrorTotal()))
		.Num (TEXT("warning_count"), static_cast<double>(ExecInfo.GetWarningTotal()))
		.BuildSuccess(Request);
}

// ─── test.get_last_results ────────────────────────────────────────────────────────────────────
//
// Args:    {}    (no args)
// Result:  { has_results: bool, current_test_full_path: string ("" if no test active),
//            is_running: bool, error_count: int, warning_count: int, duration_secs: float,
//            entries: [{type, message, context, filename, line, timestamp}] }
//
// **No -32xxx errors.** Always returns 200 with the snapshot; ``has_results=false`` indicates the
// framework has no current/last-run test info.
//
// **What this returns.** ``GetExecutionInfo`` is exposed on individual FAutomationTestBase
// instances (the test object's private execution info). The framework also tracks a current-test
// pointer via ``GetCurrentTest()``. We compose: if a test is currently running, return its live
// execution info; otherwise return an empty snapshot with has_results=false. (Past completed
// tests' info goes through ``StopTest`` immediately on completion — there's no historical
// rolling buffer the framework keeps for us.)
FMCPResponse Tool_GetLastResults(const FMCPRequest& Request)
{
	// Phase 4.2-final: Lane B with gAutomationFrameworkLock.
	FScopeLock Lock(&gAutomationFrameworkLock);

	FAutomationTestFramework& Framework = FAutomationTestFramework::Get();
	FAutomationTestBase* Current = Framework.GetCurrentTest();

	if (!Current)
	{
		return FMCPJsonBuilder()
			.Bool(TEXT("has_results"),            false)
			.Bool(TEXT("is_running"),             false)
			.Str (TEXT("current_test_full_path"), TEXT(""))
			.Num (TEXT("error_count"),            0.0)
			.Num (TEXT("warning_count"),          0.0)
			.Num (TEXT("duration_secs"),          0.0)
			.Arr (TEXT("entries"),                TArray<TSharedPtr<FJsonValue>>{})
			.BuildSuccess(Request);
	}

	FAutomationTestExecutionInfo ExecInfo;
	Current->GetExecutionInfo(ExecInfo);

	TArray<TSharedPtr<FJsonValue>> EntriesArr;
	EntriesArr.Reserve(ExecInfo.GetEntries().Num());
	for (const FAutomationExecutionEntry& Entry : ExecInfo.GetEntries())
	{
		EntriesArr.Add(MakeShared<FJsonValueObject>(TST_BuildEntryJson(Entry)));
	}

	return FMCPJsonBuilder()
		.Bool(TEXT("has_results"),            true)
		.Bool(TEXT("is_running"),             true)
		.Str (TEXT("current_test_full_path"), Framework.GetCurrentTestFullPath())
		.Num (TEXT("error_count"),            static_cast<double>(ExecInfo.GetErrorTotal()))
		.Num (TEXT("warning_count"),          static_cast<double>(ExecInfo.GetWarningTotal()))
		.Num (TEXT("duration_secs"),          ExecInfo.Duration)
		.Arr (TEXT("entries"),                MoveTemp(EntriesArr))
		.BuildSuccess(Request);
}

// ─── test.cancel_current ──────────────────────────────────────────────────────────────────────
//
// Args:    {}    (no args)
// Result:  { cancelled: bool, was_running: bool, current_test_full_path: string }
//
// **Best-effort.** UE 5.7's FAutomationTestFramework has no first-class "cancel" API; the
// closest primitive is ``StopTest`` (which terminates the currently-running test AND clears the
// current-test pointer) + ``DequeueAllCommands`` (which clears the latent + network queues so
// subsequent latent steps no-op). We do both. Some tests with long-running blocking work
// (timers in their cleanup) may continue past the cancel point — Epic's doc-comments don't
// document forced abort semantics. We report ``was_running`` from the GetCurrentTest probe so
// the caller can tell whether the cancel was a no-op.
//
// Note: ``StopTest`` returns its result via FAutomationTestExecutionInfo (passed by-ref) AND
// signals success via the bool return. We don't echo the execution info here — callers should
// invoke ``test.get_last_results`` BEFORE cancelling if they want the partial run results.
FMCPResponse Tool_CancelCurrent(const FMCPRequest& Request)
{
	check(IsInGameThread());
	// Phase 4.2-final: writer locks framework state — must wait for any in-flight run_single_test
	// to release. Acceptable: cancel is a rare operator action.
	FScopeLock Lock(&gAutomationFrameworkLock);

	FAutomationTestFramework& Framework = FAutomationTestFramework::Get();
	FAutomationTestBase* Current = Framework.GetCurrentTest();
	const bool bWasRunning = (Current != nullptr);
	const FString PriorFullPath = bWasRunning ? Framework.GetCurrentTestFullPath() : FString();

	// Clear latent + network queues first so no further steps fire after StopTest returns.
	Framework.DequeueAllCommands();

	// Issue StopTest. If no test is running this is a no-op but safe to call.
	bool bSuccessful = false;
	if (bWasRunning)
	{
		FAutomationTestExecutionInfo Discard;
		bSuccessful = Framework.StopTest(Discard);
	}

	return FMCPJsonBuilder()
		.Bool(TEXT("cancelled"),              bWasRunning)
		.Bool(TEXT("was_running"),            bWasRunning)
		.Bool(TEXT("stop_succeeded"),         bSuccessful)
		.Str (TEXT("current_test_full_path"), PriorFullPath)
		.BuildSuccess(Request);
}

// ─── test.list_categories ─────────────────────────────────────────────────────────────────────
//
// Args:    {}    (no args)
// Result:  { categories: [string], total: int }
//
// **No -32xxx errors.** Walks all registered tests once, extracts the first dot-separated
// segment of each FullTestPath, dedupes into a TSet, sorts alphabetically (case-insensitive).
// This mirrors the Session Frontend's top-level grouping (e.g. ``System``, ``Project``,
// ``Editor``, ``LogTests`` etc.).
//
// Pagination intentionally omitted — categories are O(10) in practice (not O(10k) like
// individual tests).
FMCPResponse Tool_ListCategories(const FMCPRequest& Request)
{
	// Phase 4.2-final: Lane B with gAutomationFrameworkLock.
	FScopeLock Lock(&gAutomationFrameworkLock);

	TArray<FAutomationTestInfo> AllTests;
	FAutomationTestFramework::Get().GetValidTestNames(AllTests);

	TSet<FString> CategorySet;
	for (const FAutomationTestInfo& Info : AllTests)
	{
		const FString& FullPath = Info.GetFullTestPath();
		if (FullPath.IsEmpty())
		{
			continue;
		}
		int32 DotIdx = INDEX_NONE;
		if (FullPath.FindChar(TEXT('.'), DotIdx))
		{
			CategorySet.Add(FullPath.Left(DotIdx));
		}
		else
		{
			CategorySet.Add(FullPath);
		}
	}

	TArray<FString> Sorted = CategorySet.Array();
	Sorted.Sort([](const FString& A, const FString& B)
	{
		return A.Compare(B, ESearchCase::IgnoreCase) < 0;
	});

	TArray<TSharedPtr<FJsonValue>> CatArr;
	CatArr.Reserve(Sorted.Num());
	for (const FString& Cat : Sorted)
	{
		CatArr.Add(MakeShared<FJsonValueString>(Cat));
	}

	return FMCPJsonBuilder()
		.Arr(TEXT("categories"), MoveTemp(CatArr))
		.Num(TEXT("total"),      static_cast<double>(Sorted.Num()))
		.BuildSuccess(Request);
}

// ─── test.get_test_info ───────────────────────────────────────────────────────────────────────
//
// Args:    { test_name: string }    (required; exact match against FullTestPath, case-sensitive)
// Result:  <spec json>  (same shape as one entry from test.list_automation_specs)
//
// Errors:
//   -32602 InvalidParams    missing/empty test_name
//   -32046 TestNotFound     no test with this FullTestPath in the registered set
FMCPResponse Tool_GetTestInfo(const FMCPRequest& Request)
{
	// Phase 4.2-final: Lane B with gAutomationFrameworkLock.
	FScopeLock Lock(&gAutomationFrameworkLock);

	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kTSTErrorInvalidParams, TEXT("missing args object"));
	}
	FString TestName;
	if (!Request.Args->TryGetStringField(TEXT("test_name"), TestName) || TestName.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kTSTErrorInvalidParams,
			TEXT("missing required string field 'test_name'"));
	}

	FAutomationTestInfo Info;
	if (!TST_FindTestByFullPath(TestName, Info))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorTestNotFound,
			FString::Printf(TEXT("automation test '%s' not found; see test.list_automation_specs"),
				*TestName));
	}

	return FMCPToolHelpers::MakeSuccessObj(Request, TST_BuildSpecJson(Info));
}

// ─── test.set_filter_flags ────────────────────────────────────────────────────────────────────
//
// Args:    { flags: [string] }    (required array; case-sensitive flag names per
//                                  EAutomationTestFlags_GetTestFlagsMap)
// Result:  { applied: [string], rejected: [{flag, reason}], applied_mask: int }
//
// Errors:
//   -32602 InvalidParams    missing flags array
//
// **No silent unknown handling.** Unknown flag names go into ``rejected[]`` with the per-name
// reason; known flags are OR-ed into the bitmask and passed to ``SetRequestedTestFilter``. Empty
// known-set after filtering → still calls SetRequestedTestFilter(EAutomationTestFlags::None) so
// the operator's intent ("no filter") is honoured.
//
// **Effect persists until reset.** SetRequestedTestFilter mutates a process-lifetime field on
// the framework singleton. Caller should re-issue with their desired final state at the end of
// a batch if they don't want subsequent ``test.list_automation_specs`` calls to be silently
// filtered. (We do NOT auto-reset on next call — that would surprise tooling.)
FMCPResponse Tool_SetFilterFlags(const FMCPRequest& Request)
{
	// Phase 4.2-final: writer holds gAutomationFrameworkLock — serializes against Lane B readers
	// of RequestedTestFilter (which is a plain enum field with no internal atomic protection).
	FScopeLock Lock(&gAutomationFrameworkLock);

	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kTSTErrorInvalidParams, TEXT("missing args object"));
	}
	const TArray<TSharedPtr<FJsonValue>>* FlagsArr = nullptr;
	if (!Request.Args->TryGetArrayField(TEXT("flags"), FlagsArr) || !FlagsArr)
	{
		return FMCPToolHelpers::MakeError(Request, kTSTErrorInvalidParams,
			TEXT("missing required array field 'flags' (e.g. [\"SmokeFilter\",\"EngineFilter\"])"));
	}

	EAutomationTestFlags AppliedMask = EAutomationTestFlags::None;
	TArray<TSharedPtr<FJsonValue>> AppliedArr;
	TArray<TSharedPtr<FJsonValue>> RejectedArr;

	for (int32 i = 0; i < FlagsArr->Num(); ++i)
	{
		FString Name;
		if (!(*FlagsArr)[i].IsValid() || !(*FlagsArr)[i]->TryGetString(Name) || Name.IsEmpty())
		{
			TSharedRef<FJsonObject> RejObj = MakeShared<FJsonObject>();
			RejObj->SetStringField(TEXT("flag"), TEXT(""));
			RejObj->SetStringField(TEXT("reason"),
				FString::Printf(TEXT("flags[%d] is not a non-empty string"), i));
			RejectedArr.Add(MakeShared<FJsonValueObject>(RejObj));
			continue;
		}

		EAutomationTestFlags Flag = EAutomationTestFlags::None;
		if (TST_LookupFlag(Name, Flag))
		{
			AppliedMask |= Flag;
			AppliedArr.Add(MakeShared<FJsonValueString>(Name));
		}
		else
		{
			TSharedRef<FJsonObject> RejObj = MakeShared<FJsonObject>();
			RejObj->SetStringField(TEXT("flag"), Name);
			RejObj->SetStringField(TEXT("reason"),
				TEXT("unknown flag name (see EAutomationTestFlags_GetTestFlagsMap for canonical names)"));
			RejectedArr.Add(MakeShared<FJsonValueObject>(RejObj));
		}
	}

	FAutomationTestFramework::Get().SetRequestedTestFilter(AppliedMask);

	return FMCPJsonBuilder()
		.Arr(TEXT("applied"),      MoveTemp(AppliedArr))
		.Arr(TEXT("rejected"),     MoveTemp(RejectedArr))
		.Num(TEXT("applied_mask"), static_cast<double>(static_cast<uint32>(AppliedMask)))
		.BuildSuccess(Request);
}

// ─── Registration ──────────────────────────────────────────────────────────────────────────────
void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	// Phase 4.2-final (2026-05-22): 4 readers promoted to Lane B with gAutomationFrameworkLock
	// serializing them vs Lane A writers (run_single_test / cancel_current / set_filter_flags).
	// run_single_test / cancel_current stay Lane A — they call StartTestByName /
	// ExecuteLatentCommands / StopTest which invoke ProcessEvent on UObject test bodies (GT-only).
	// set_filter_flags stays Lane A — conservative; writes a framework field affecting test
	// invocation behavior; cleanest to keep on GT alongside the invocation paths.
	RegisterTool(TEXT("test.list_automation_specs"), &Tool_ListAutomationSpecs, /*Lane B*/ true);
	RegisterTool(TEXT("test.run_single_test"),       &Tool_RunSingleTest,       /*Lane A*/ false);
	RegisterTool(TEXT("test.get_last_results"),      &Tool_GetLastResults,      /*Lane B*/ true);
	RegisterTool(TEXT("test.cancel_current"),        &Tool_CancelCurrent,       /*Lane A*/ false);
	RegisterTool(TEXT("test.list_categories"),       &Tool_ListCategories,      /*Lane B*/ true);
	RegisterTool(TEXT("test.get_test_info"),         &Tool_GetTestInfo,         /*Lane B*/ true);
	RegisterTool(TEXT("test.set_filter_flags"),      &Tool_SetFilterFlags,      /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("Phase 6 Chunk B (Automation Test): registered 7 test.* sync handlers ")
		TEXT("(list_automation_specs/run_single_test/get_last_results/cancel_current/list_categories/")
		TEXT("get_test_info/set_filter_flags, all Lane A)"));
}

} // namespace FTestTools

#undef LOCTEXT_NAMESPACE

MCP_REGISTER_SURFACE(TestTools, &FTestTools::Register)
