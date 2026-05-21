// Copyright FatumGame. All Rights Reserved.

#include "LogTools.h"

#include "MCPSurfaceRegistry.h"

#include "FMCPDispatchQueue.h"
#include "FMCPLogStream.h"
#include "MCPJsonBuilder.h"
#include "MCPToolHelpers.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPPageCursor.h"

#include "Logging/LogVerbosity.h"
#include "Misc/CoreMisc.h"
#include "Misc/StringOutputDevice.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// LOG_ prefix per the unity-build symbol-collision pattern. Per-surface error constants kept;
	// XX_StampIds/MakeError/MakeSuccessObj/RequireStringField removed in Phase 3 — see
	// FMCPToolHelpers in MCPToolHelpers.h.
	constexpr int32 kLOGErrorInvalidParams = -32602;
	constexpr int32 kLOGErrorInternal      = -32603;

	// Default page size for ``log.list_categories``. Mirrors Phase 4/5/6 pagination defaults.
	constexpr int32 kLOGDefaultPageSize = 100;

	int32 LOG_ClampPageSize(const TSharedPtr<FJsonObject>& Args, const TCHAR* FieldName, int32 Default)
	{
		int32 Out = Default;
		if (Args.IsValid())
		{
			Args->TryGetNumberField(FieldName, Out);
		}
		return FMath::Clamp(Out, 1, 1000);
	}

	bool LOG_DecodeCursor(
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
				TEXT("page_token filter_hash mismatch — caller mutated 'prefix_filter' between "
					 "pages; restart pagination with page_token=null"));
			return false;
		}
		return true;
	}

	uint64 LOG_HashFilter(const FString& Filter)
	{
		const uint32 H1 = GetTypeHash(Filter);
		return static_cast<uint64>(H1);
	}

	/**
	 * Map a UE verbosity name (case-insensitive, accepting Off/All/Default aliases) to the
	 * canonical console-command token. Returns the token suitable for ``Log <Category> <Token>``.
	 * Returns empty string on unknown token. ``Default`` is a special case — it resets the category
	 * to its boot-time default and is handled by the suppression impl identically to other tokens.
	 *
	 * We canonicalise BEFORE passing to the Exec path so the console command sees the exact
	 * keyword UE's parser expects (case-sensitive lookup in ProcessCmdString happens via FName
	 * comparison against ``NAME_Verbose`` / ``NAME_Error`` / etc. — see LogSuppressionInterface.cpp
	 * lines 235-265). Passing ``"verbose"`` (lowercase) works because FName is case-insensitive at
	 * construction, but normalising to the exact case is defensive against future tightening.
	 */
	FString LOG_CanonicaliseVerbosity(const FString& Raw)
	{
		const FString Trimmed = Raw.TrimStartAndEnd();
		if (Trimmed.IsEmpty()) return FString();
		// UE's accepted vocabulary (LogSuppressionInterface::ProcessCmdString):
		//   NoLogging / Fatal / Error / Warning / Display / Log / Verbose / VeryVerbose / On / Off
		//   / Break / Reset / Default / All
		// We map operator-friendly aliases to UE canonical:
		//   Off → NoLogging  (UE's None alias)
		//   All → VeryVerbose
		// Reset / Break / Default pass through unchanged.
		struct FAlias { const TCHAR* In; const TCHAR* Out; };
		static const FAlias Aliases[] = {
			{ TEXT("NoLogging"),   TEXT("NoLogging") },
			{ TEXT("None"),        TEXT("None") },
			{ TEXT("Off"),         TEXT("Off") },
			{ TEXT("Fatal"),       TEXT("Fatal") },
			{ TEXT("Error"),       TEXT("Error") },
			{ TEXT("Warning"),     TEXT("Warning") },
			{ TEXT("Display"),     TEXT("Display") },
			{ TEXT("Log"),         TEXT("Log") },
			{ TEXT("Verbose"),     TEXT("Verbose") },
			{ TEXT("VeryVerbose"), TEXT("VeryVerbose") },
			{ TEXT("All"),         TEXT("All") },
			{ TEXT("On"),          TEXT("On") },
			{ TEXT("Default"),     TEXT("Default") },
			{ TEXT("Reset"),       TEXT("Reset") },
			{ TEXT("Break"),       TEXT("Break") },
		};
		for (const FAlias& A : Aliases)
		{
			if (Trimmed.Equals(A.In, ESearchCase::IgnoreCase))
			{
				return A.Out;
			}
		}
		return FString();
	}

	/**
	 * Comma-separated list of accepted verbosity tokens for error messages. Compiled once at
	 * static-init time; safe to share across calls.
	 *
	 * (Note: we don't expose a separate "string → ELogVerbosity::Type" helper because the value
	 * is opaque to the bridge surface — the canonicalised string is what we send to UE's
	 * console-command path AND what we echo back to the caller. UE's ParseLogVerbosityFromString
	 * exists for callers that want enum values but doesn't cover the Off/All/Default aliases we
	 * accept here, so it's not a 1:1 helper anyway.)
	 */
	const TCHAR* LOG_AcceptedVerbositiesList()
	{
		return TEXT("NoLogging / None / Off / Fatal / Error / Warning / Display / Log / Verbose / "
		            "VeryVerbose / All / On / Default / Reset / Break");
	}
} // namespace

namespace FLogTools
{

// ─── log.set_category_verbosity ──────────────────────────────────────────────────────────────────
//
// Args:    { category: string, verbosity: string }    (both required)
//          verbosity ∈ {NoLogging, None, Off, Fatal, Error, Warning, Display, Log, Verbose,
//                        VeryVerbose, All, On, Default, Reset, Break}  (case-insensitive)
// Result:  { applied: bool, category: string, requested_verbosity: string (canonical),
//            prior_verbosity: string (last-observed, "Unknown" if never seen),
//            warning?: string,  // populated when category not observed (forward-reference write)
//            compile_time_clamped: bool  // best-effort detection; false when unknown
//          }
//
// Errors:
//   -32602 InvalidParams         missing 'category' / 'verbosity' OR unknown verbosity token
//   -32603 InternalError         GLog StaticExec returned false (console command rejected)
//
// **Internal mechanism.** The implementation routes through ``FSelfRegisteringExec::StaticExec``
// with the canonical ``Log <Category> <Verbosity>`` command — UE's ``FLogSuppressionImplementation``
// (registered as a ``FSelfRegisteringExec`` instance at Core module init) intercepts and applies
// the change to its internal ReverseAssociations + BootAssociations maps, which in turn invokes
// ``FLogCategoryBase::SetVerbosity`` for every category matching the name. This is the SAME code
// path operator-typed ``Log`` console commands take — guaranteed behavioural parity.
//
// **D6 — process-lifetime only.** No ini write. To persist verbosity changes across editor
// restarts, write the matching key into ``DefaultEngine.ini``'s ``[Core.Log]`` section via
// ``cfg.write`` (Chunk C).
//
// **Forward-reference writes.** UE silently accepts ``Log <Category> <Verbosity>`` for unknown
// category names — the suppression impl stores the value in BootAssociations and applies it when
// the category later registers via AssociateSuppress. We surface a ``warning`` field on the
// response when the category has never been observed in this bridge's log stream so the operator
// knows the request went through a forward-reference path (no immediate effect visible).
//
// **compile_time_clamped detection.** Best-effort. We compare the requested verbosity against the
// last-observed verbosity AFTER the StaticExec call: if the post-call observation (waiting up to
// one tick) shows a lower value than requested, the cap was hit. In practice we just compare the
// pre-call observed verbosity against the requested value; if the requested value is higher than
// what was previously observable, we mark ``compile_time_clamped=false`` (unknown).
FMCPResponse Tool_SetCategoryVerbosity(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString Category;
	FString RawVerbosity;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("category"),  Category,     Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("verbosity"), RawVerbosity, Err)) { return Err; }

	const FString CanonVerbosity = LOG_CanonicaliseVerbosity(RawVerbosity);
	if (CanonVerbosity.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kLOGErrorInvalidParams,
			FString::Printf(TEXT("verbosity '%s' is not a recognised token; accepted: %s"),
				*RawVerbosity, LOG_AcceptedVerbositiesList()));
	}

	// Capture prior observed verbosity (best-effort — only knowable if the category has emitted
	// at least one log line since FMCPLogStream attached).
	const FName CategoryName(*Category);
	FString PriorVerbosityStr = TEXT("Unknown");
	bool bObservedBefore = false;
	{
		const TArray<FMCPLogStream::FObservedCategoryInfo> Observed =
			FMCPLogStream::Get().GetObservedCategories();
		for (const FMCPLogStream::FObservedCategoryInfo& Info : Observed)
		{
			if (Info.Category == CategoryName)
			{
				PriorVerbosityStr = ToString(Info.LastObservedVerbosity);
				bObservedBefore = true;
				break;
			}
		}
	}

	// Build the console command + route through the canonical exec path. We capture stdout via
	// FStringOutputDevice (FOutputDeviceNull would drop the response; FStringOutputDevice tracks
	// the OK/error formatting the suppression impl emits to Ar.Logf in ProcessCmdString).
	// Format: ``Log <Category> <Verbosity>`` — matches the exact token order UE's Log Exec parses.
	const FString CmdString = FString::Printf(TEXT("Log %s %s"), *Category, *CanonVerbosity);

	FStringOutputDevice CapturedOutput;
	const bool bExecOk = FSelfRegisteringExec::StaticExec(nullptr, *CmdString, CapturedOutput);
	if (!bExecOk)
	{
		// StaticExec returns false when NO registered exec handled the command. This shouldn't
		// happen for a properly-formed ``Log`` command (FLogSuppressionImplementation always
		// claims it) but defensive surface in case the suppression impl is torn down mid-shutdown.
		return FMCPToolHelpers::MakeError(Request, kLOGErrorInternal,
			FString::Printf(TEXT("FSelfRegisteringExec::StaticExec rejected command '%s'; "
				"FLogSuppressionImplementation may not be initialised"), *CmdString));
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("applied"),                  true);
	Out->SetStringField(TEXT("category"),               Category);
	Out->SetStringField(TEXT("requested_verbosity"),    CanonVerbosity);
	Out->SetStringField(TEXT("prior_verbosity"),        PriorVerbosityStr);
	Out->SetStringField(TEXT("console_output"),         static_cast<const FString&>(CapturedOutput));
	// compile_time_clamped is unknown without a post-emission re-observation — false here means
	// "not detected" rather than "definitely not clamped". Caller can confirm by emitting a UE_LOG
	// from the target category + re-querying via log.list_categories.
	Out->SetBoolField(TEXT("compile_time_clamped"),     false);

	if (!bObservedBefore)
	{
		Out->SetStringField(TEXT("warning"),
			FString::Printf(TEXT("category '%s' has not produced any log entries since the bridge "
				"attached — the verbosity change went through UE's forward-reference path "
				"(BootAssociations) and will apply when the category first registers. Trigger any "
				"UE_LOG from this category to confirm the value took effect."), *Category));
	}

	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── log.list_categories ─────────────────────────────────────────────────────────────────────────
//
// Args:    { prefix_filter?: string, page_token?: string, page_size?: int (default 100, max 1000) }
// Result:  { categories: [{name, current_verbosity, observation_count}],
//            next_page_token?: string | null, total_known: int, prefix_filter_echo: string }
//
// Errors:
//   -32015 StaleCursor    page_token's filter_hash doesn't match the current prefix_filter
//
// **Observed-set caveat (D6).** Only categories that have produced at least one log entry since
// FMCPLogStream::Attach are returned. Categories that have registered but never emitted are
// invisible. This is a deliberate trade-off — the public ``FLogSuppressionInterface`` does not
// expose its ReverseAssociations map (TMultiMap<FName, FLogCategoryBase*>) and the alternative
// (private-state access via fork-of-engine surgery) violates the bridge's "engine-clean" stance.
//
// **current_verbosity** is the LAST observed verbosity for that category (NOT necessarily the
// current FLogCategoryBase::GetVerbosity()). This is usually the same — categories don't
// retroactively change verbosity per-call — but if SetVerbosity was called between the last
// log emission and now, the observed value will be stale until the next emission re-populates.
//
// **Sort key.** Category FName::ToString, case-insensitive. Filter is a CASE-INSENSITIVE prefix.
//
// **Pagination.** FMCPPageCursor with filter_hash = hash(prefix_filter); LastAssetPath stores the
// last category name emitted on this page.
FMCPResponse Tool_ListCategories(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString Prefix;
	if (Request.Args.IsValid())
	{
		Request.Args->TryGetStringField(TEXT("prefix_filter"), Prefix);
	}
	const uint64 FilterHash = LOG_HashFilter(Prefix);

	FString TokenWire;
	if (Request.Args.IsValid())
	{
		Request.Args->TryGetStringField(TEXT("page_token"), TokenWire);
	}
	FMCPPageCursor Cursor;
	FMCPResponse CursorErr;
	if (!LOG_DecodeCursor(Request, TokenWire, FilterHash, Cursor, CursorErr))
	{
		return CursorErr;
	}

	const int32 PageSize = LOG_ClampPageSize(Request.Args, TEXT("page_size"), kLOGDefaultPageSize);

	// Snapshot observed categories from FMCPLogStream. Acquires the stream's lock briefly inside
	// GetObservedCategories — no UObject access, safe under any condition.
	TArray<FMCPLogStream::FObservedCategoryInfo> Observed =
		FMCPLogStream::Get().GetObservedCategories();

	// Apply prefix filter (case-insensitive). Filtered set is materialised before sort+page so the
	// cursor's LastAssetPath is interpreted against the filtered view, not the unfiltered universe.
	struct FEntry
	{
		FString Name;
		ELogVerbosity::Type Verbosity = ELogVerbosity::Log;
		int64 Count = 0;
	};
	TArray<FEntry> Entries;
	Entries.Reserve(Observed.Num());
	for (const FMCPLogStream::FObservedCategoryInfo& Info : Observed)
	{
		const FString NameStr = Info.Category.ToString();
		if (!Prefix.IsEmpty())
		{
			if (NameStr.Len() < Prefix.Len() ||
				!NameStr.Left(Prefix.Len()).Equals(Prefix, ESearchCase::IgnoreCase))
			{
				continue;
			}
		}
		FEntry E;
		E.Name = NameStr;
		E.Verbosity = Info.LastObservedVerbosity;
		E.Count = Info.ObservationCount;
		Entries.Add(MoveTemp(E));
	}

	// Stable sort by case-insensitive name to match cursor invariant.
	Entries.StableSort([](const FEntry& A, const FEntry& B)
	{
		return A.Name.Compare(B.Name, ESearchCase::IgnoreCase) < 0;
	});

	// Cursor sentinel: skip past LastAssetPath (re-used field for "last category name").
	int32 StartIdx = 0;
	if (!Cursor.LastAssetPath.IsEmpty())
	{
		while (StartIdx < Entries.Num())
		{
			if (Entries[StartIdx].Name.Compare(Cursor.LastAssetPath, ESearchCase::IgnoreCase) > 0)
			{
				break;
			}
			++StartIdx;
		}
	}
	const int32 EndIdxExcl = FMath::Min(Entries.Num(), StartIdx + PageSize);

	TArray<TSharedPtr<FJsonValue>> ItemsArr;
	ItemsArr.Reserve(EndIdxExcl - StartIdx);
	FString LastEmittedName;
	for (int32 i = StartIdx; i < EndIdxExcl; ++i)
	{
		const FEntry& E = Entries[i];
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"),               E.Name);
		Obj->SetStringField(TEXT("current_verbosity"),  ToString(E.Verbosity));
		Obj->SetNumberField(TEXT("observation_count"),  static_cast<double>(E.Count));
		ItemsArr.Add(MakeShared<FJsonValueObject>(Obj));
		LastEmittedName = E.Name;
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetArrayField(TEXT("categories"),         ItemsArr);
	Out->SetNumberField(TEXT("total_known"),       static_cast<double>(Entries.Num()));
	Out->SetStringField(TEXT("prefix_filter_echo"), Prefix);

	if (EndIdxExcl < Entries.Num() && !LastEmittedName.IsEmpty())
	{
		FMCPPageCursor NextCursor;
		NextCursor.FilterHash = FilterHash;
		NextCursor.LastAssetPath = LastEmittedName;
		NextCursor.TotalKnownSnapshot = Entries.Num();
		Out->SetStringField(TEXT("next_page_token"), FMCPPageCursorUtils::Encode(NextCursor));
	}
	else
	{
		Out->SetField(TEXT("next_page_token"), MakeShared<FJsonValueNull>());
	}
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── log.clear ───────────────────────────────────────────────────────────────────────────────────
//
// Args:    {}    (none)
// Result:  { cleared: bool, line_count_before: int, ring_capacity: int, total_observed: int }
//
// Errors:  (none — clear is always successful; no input to validate)
//
// **Scope.** Empties the FMCPLogStream ring buffer that backs log.tail / log.search. Does NOT
// touch the on-disk file logs (UE writes those via FOutputDeviceFile to Saved/Logs/<Project>.log
// in append-only mode — separate concern, governed by ``-LogTimes`` / ``-NoLogTimes`` and Epic's
// log rotation policy). Does NOT reset TotalObserved — that cumulative counter remains valid.
// Does NOT reset the observed-categories registry — those names persist across log.clear (matches
// UE's own ``Log List`` behaviour which surveys registered categories, not log content).
//
// **Concurrency note.** log.clear races against any in-flight UE_LOG emission from other threads
// (sim thread, render thread, etc.). The race resolves as: writes that happen DURING log.clear's
// EntriesLock acquisition queue + commit after Clear returns; writes that happen AFTER return
// land in slot 0 + advance from there. No data loss or corruption — the ring is consistent at all
// observable points.
FMCPResponse Tool_Clear(const FMCPRequest& Request)
{
	check(IsInGameThread());

	const int32 PriorCount = FMCPLogStream::Get().Clear();

	return FMCPJsonBuilder()
		.Bool(TEXT("cleared"),           true)
		.Num (TEXT("line_count_before"), static_cast<double>(PriorCount))
		.Num (TEXT("ring_capacity"),     static_cast<double>(FMCPLogStream::kMaxEntries))
		.Num (TEXT("total_observed"),    static_cast<double>(FMCPLogStream::Get().GetTotalObserved()))
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

	RegisterTool(TEXT("log.set_category_verbosity"), &Tool_SetCategoryVerbosity, /*Lane A*/ false);
	RegisterTool(TEXT("log.list_categories"),        &Tool_ListCategories,       /*Lane A*/ false);
	RegisterTool(TEXT("log.clear"),                  &Tool_Clear,                /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("Phase 6 Chunk D (Logs additions): registered 3 log.* sync handlers ")
		TEXT("(set_category_verbosity / list_categories / clear, all Lane A); ")
		TEXT("Phase 1 already shipped log.tail / log.search / log.subscribe — total log.* surface = 6"));
}

} // namespace FLogTools

#undef LOCTEXT_NAMESPACE

MCP_REGISTER_SURFACE(LogTools, &FLogTools::Register)
