// Copyright FatumGame. All Rights Reserved.

#include "SourceControlTools.h"

#include "FMCPDispatchQueue.h"
#include "MCPToolHelpers.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPPathSandbox.h"

#include "ISourceControlModule.h"
#include "ISourceControlProvider.h"
#include "ISourceControlRevision.h"
#include "ISourceControlState.h"
#include "SourceControlOperations.h"

#include "Containers/StringConv.h"
#include "HAL/FileManager.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// SCT_ prefix per the unity-build symbol-collision pattern. Per-surface error constants kept;
	// XX_StampIds/MakeError/MakeSuccessObj removed in Phase 3 — use FMCPToolHelpers::Xxx
	// from MCPToolHelpers.h. Source-control ops don't use FScopedTransaction (they go through
	// ISourceControlProvider::Execute), so no FMCPMutatorScope migration here.
	constexpr int32 kSCTErrorInvalidParams = -32602;
	constexpr int32 kSCTErrorInternal      = -32603;

	// Binary diff per-side cap (D9 + risk table #6). 32 MiB raw → ~43 MiB base64 → comfortably
	// fits two sides + JSON envelope inside the 64 MiB wire frame limit.
	constexpr int64 kSCTBinaryDiffMaxBytesPerSide = 32 * 1024 * 1024;

	// Heuristic byte budget used to classify content as "looks textual" vs "binary" — we only
	// scan the first 8 KiB because by the 8 KiB mark a UTF-8 / ASCII text file would not contain
	// any NUL bytes, while a UAsset would have hundreds.
	constexpr int32 kSCTTextDetectScanBytes = 8 * 1024;

	/**
	 * Verify SC is enabled + available; return a populated error response (and set bOk=false) on
	 * failure. Caller passes through the response on bOk=false. Echoes the provider name in the
	 * error message so AI clients can disambiguate "Git plugin loaded but unreachable" from
	 * "no provider configured".
	 */
	bool SCT_RequireProvider(const FMCPRequest& Request, FMCPResponse& OutError)
	{
		ISourceControlModule& Module = ISourceControlModule::Get();
		if (!Module.IsEnabled())
		{
			OutError = FMCPToolHelpers::MakeError(Request, kMCPErrorSourceControlProviderUnavailable,
				TEXT("source control provider not configured (ISourceControlModule::Get().IsEnabled() "
					 "== false) — open Editor → Source Control to set up Git/Perforce/etc."));
			return false;
		}
		ISourceControlProvider& Provider = Module.GetProvider();
		if (!Provider.IsAvailable())
		{
			OutError = FMCPToolHelpers::MakeError(Request, kMCPErrorSourceControlProviderUnavailable,
				FString::Printf(TEXT("source control provider '%s' is enabled but not available "
					"(IsAvailable()=false) — check workspace connectivity (e.g. Perforce server "
					"reachable, Git workspace not locked)."),
					*Provider.GetName().ToString()));
			return false;
		}
		return true;
	}

	/**
	 * Translate one caller-supplied path token to a sandboxed absolute disk path suitable for
	 * ``Provider.Execute``.
	 *
	 * Accepts:
	 *   - Absolute disk path:  ``C:/Project/Content/Foo.uasset``  → sandbox-checked, normalised
	 *   - UE package path:     ``/Game/Foo/Bar``                  → resolved via FPackageName
	 *                                                                (.uasset extension appended
	 *                                                                if file exists on disk)
	 *
	 * Returns true on success — populates OutAbsPath. Returns false on failure — populates
	 * OutError (caller surfaces as -32010 InvalidPath or -32013 PathEscape per error category).
	 */
	bool SCT_ResolveSCPath(const FString& InRawPath, FString& OutAbsPath, FString& OutError,
		int32& OutErrorCode)
	{
		OutAbsPath.Reset();
		OutError.Reset();
		OutErrorCode = 0;

		if (InRawPath.IsEmpty())
		{
			OutError = TEXT("file_path is empty");
			OutErrorCode = kMCPErrorInvalidPath;
			return false;
		}

		// UE package-name form (starts with '/' but isn't a UNIX absolute disk path on Windows).
		// On Windows ``/Game`` and ``/Engine`` start with '/' but aren't disk paths; on POSIX
		// ``/Game`` would collide with an actual disk root. Disambiguate by FPackageName::IsValidLongPackageName.
		if (InRawPath.StartsWith(TEXT("/")) && FPackageName::IsValidLongPackageName(InRawPath))
		{
			FString FilenameOnDisk;
			if (!FPackageName::TryConvertLongPackageNameToFilename(InRawPath, FilenameOnDisk))
			{
				OutError = FString::Printf(
					TEXT("package path '%s' could not be mapped to a disk filename "
						 "(unknown mount point?)"), *InRawPath);
				OutErrorCode = kMCPErrorInvalidPath;
				return false;
			}
			// .uasset is the default; FPackageName doesn't append an extension. We probe both
			// .uasset and .umap to handle level packages, and fall through to .uasset if neither
			// exists (SC may still recognise the path for an add-pending file).
			const FString UAsset = FilenameOnDisk + TEXT(".uasset");
			const FString UMap   = FilenameOnDisk + TEXT(".umap");
			if (IFileManager::Get().FileExists(*UAsset))
			{
				FilenameOnDisk = UAsset;
			}
			else if (IFileManager::Get().FileExists(*UMap))
			{
				FilenameOnDisk = UMap;
			}
			else
			{
				FilenameOnDisk = UAsset; // best guess
			}
			// Verify sandbox containment.
			FString Sandboxed;
			FString SandboxErr;
			if (!FMCPPathSandbox::Resolve(FilenameOnDisk, Sandboxed, SandboxErr))
			{
				OutError = SandboxErr;
				OutErrorCode = kMCPErrorPathEscape;
				return false;
			}
			OutAbsPath = Sandboxed;
			return true;
		}

		// Disk path branch — let FMCPPathSandbox normalise + verify whitelist.
		FString Sandboxed;
		FString SandboxErr;
		if (!FMCPPathSandbox::Resolve(InRawPath, Sandboxed, SandboxErr))
		{
			OutError = SandboxErr;
			OutErrorCode = kMCPErrorPathEscape;
			return false;
		}
		OutAbsPath = Sandboxed;
		return true;
	}

	/**
	 * Read a {file_paths:[string]} array argument into a TArray<FString>, resolving each entry
	 * to a sandboxed disk path. Caller passes bAllowEmpty=true for tools like sc.status whose
	 * file_paths arg is optional (omitted = "all files known to provider"); false for tools
	 * like sc.checkout where empty is an error.
	 *
	 * Returns true on success — populates OutPaths. Returns false on failure — populates
	 * OutError (caller surfaces).
	 */
	bool SCT_ReadFilePathsArray(const FMCPRequest& Request, const TCHAR* FieldName,
		bool bAllowEmpty, TArray<FString>& OutPaths, FMCPResponse& OutError)
	{
		OutPaths.Reset();
		if (!Request.Args.IsValid())
		{
			if (bAllowEmpty)
			{
				return true;
			}
			OutError = FMCPToolHelpers::MakeError(Request, kSCTErrorInvalidParams,
				FString::Printf(TEXT("missing required array field '%s'"), FieldName));
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		const bool bHasField = Request.Args->TryGetArrayField(FieldName, Arr);
		if (!bHasField || !Arr)
		{
			if (bAllowEmpty)
			{
				return true;
			}
			OutError = FMCPToolHelpers::MakeError(Request, kSCTErrorInvalidParams,
				FString::Printf(TEXT("missing required array field '%s'"), FieldName));
			return false;
		}
		if (Arr->Num() == 0 && !bAllowEmpty)
		{
			OutError = FMCPToolHelpers::MakeError(Request, kSCTErrorInvalidParams,
				FString::Printf(TEXT("'%s' must contain at least one non-empty path string"),
					FieldName));
			return false;
		}

		OutPaths.Reserve(Arr->Num());
		for (int32 i = 0; i < Arr->Num(); ++i)
		{
			FString Raw;
			if (!(*Arr)[i].IsValid() || !(*Arr)[i]->TryGetString(Raw) || Raw.IsEmpty())
			{
				OutError = FMCPToolHelpers::MakeError(Request, kSCTErrorInvalidParams,
					FString::Printf(TEXT("'%s'[%d] is not a non-empty string"), FieldName, i));
				return false;
			}

			FString AbsPath;
			FString ResolveErr;
			int32 ResolveCode = 0;
			if (!SCT_ResolveSCPath(Raw, AbsPath, ResolveErr, ResolveCode))
			{
				OutError = FMCPToolHelpers::MakeError(Request, ResolveCode,
					FString::Printf(TEXT("'%s'[%d]='%s': %s"),
						FieldName, i, *Raw, *ResolveErr));
				return false;
			}
			OutPaths.Add(MoveTemp(AbsPath));
		}
		return true;
	}

	/**
	 * Classify SC state into a small string enum exposed on the wire.
	 *
	 * State enum values:
	 *   "unknown"           — provider has no record OR IsUnknown() returns true
	 *   "unchanged"         — file is in workspace, current with head, not checked out
	 *   "checked_out"       — locally checked out (we have the lock on locking providers)
	 *   "checked_out_other" — checked out by another user
	 *   "modified"          — locally modified (Git-style; Perforce uses checked_out)
	 *   "added"             — pending add (new file)
	 *   "deleted"           — pending delete
	 *   "conflicted"        — merge conflict (needs resolve before submit)
	 *   "not_current"       — workspace revision is behind head
	 *   "ignored"           — matches an SC ignore rule
	 *   "not_controlled"    — file is not under SC at all
	 */
	FString SCT_ClassifyState(const ISourceControlState& State)
	{
		if (State.IsConflicted())            { return TEXT("conflicted"); }
		if (State.IsCheckedOut())            { return TEXT("checked_out"); }
		if (State.IsCheckedOutOther())       { return TEXT("checked_out_other"); }
		if (State.IsAdded())                 { return TEXT("added"); }
		if (State.IsDeleted())               { return TEXT("deleted"); }
		if (State.IsModified())              { return TEXT("modified"); }
		if (State.IsIgnored())               { return TEXT("ignored"); }
		if (!State.IsSourceControlled())     { return TEXT("not_controlled"); }
		if (State.IsUnknown())               { return TEXT("unknown"); }
		if (!State.IsCurrent())              { return TEXT("not_current"); }
		return TEXT("unchanged");
	}

	/**
	 * Build the per-file status JSON object emitted by sc.status / sc.checkout / sc.revert
	 * result blocks (shape varies per tool; this is the shared "info" piece).
	 *
	 * Shape:
	 *   {
	 *     "path":             absolute disk path,
	 *     "state":            classified enum string (see SCT_ClassifyState),
	 *     "revision":         int (head revision number; -1 if unknown),
	 *     "action":           last action description from head revision; empty string if none,
	 *     "last_modified_by": username from head revision; empty string if none,
	 *     "is_checked_out":   bool,
	 *     "is_modified":      bool,
	 *     "is_added":         bool,
	 *     "is_deleted":       bool,
	 *     "is_current":       bool
	 *   }
	 */
	TSharedRef<FJsonObject> SCT_BuildStateJson(const FString& AbsPath, const ISourceControlState& State)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("path"), AbsPath);
		Obj->SetStringField(TEXT("state"), SCT_ClassifyState(State));

		int32 RevisionNumber = -1;
		FString Action;
		FString LastModifiedBy;
		if (State.GetHistorySize() > 0)
		{
			TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> Head = State.GetHistoryItem(0);
			if (Head.IsValid())
			{
				RevisionNumber = Head->GetRevisionNumber();
				Action = Head->GetAction();
				LastModifiedBy = Head->GetUserName();
			}
		}
		Obj->SetNumberField(TEXT("revision"), static_cast<double>(RevisionNumber));
		Obj->SetStringField(TEXT("action"), Action);
		Obj->SetStringField(TEXT("last_modified_by"), LastModifiedBy);

		Obj->SetBoolField(TEXT("is_checked_out"), State.IsCheckedOut());
		Obj->SetBoolField(TEXT("is_modified"),    State.IsModified());
		Obj->SetBoolField(TEXT("is_added"),       State.IsAdded());
		Obj->SetBoolField(TEXT("is_deleted"),     State.IsDeleted());
		Obj->SetBoolField(TEXT("is_current"),     State.IsCurrent());
		return Obj;
	}

	// ─── Diff helpers ───────────────────────────────────────────────────────────────────────────

	/**
	 * Decide if a byte buffer is "text-like" by scanning the first kSCTTextDetectScanBytes bytes
	 * for NUL bytes. UTF-8/ASCII text never contains NUL; binary assets contain many.
	 */
	bool SCT_LooksTextual(const TArray<uint8>& Buffer)
	{
		const int32 Limit = FMath::Min(Buffer.Num(), kSCTTextDetectScanBytes);
		for (int32 i = 0; i < Limit; ++i)
		{
			if (Buffer[i] == 0)
			{
				return false;
			}
		}
		return true;
	}

	/**
	 * Load a file's bytes into ``OutBuffer``. Returns true on success. Caller can pre-check
	 * file size to gate the binary-diff cap (kSCTBinaryDiffMaxBytesPerSide).
	 */
	bool SCT_LoadFileBytes(const FString& AbsPath, TArray<uint8>& OutBuffer, FString& OutError)
	{
		OutBuffer.Reset();
		if (!IFileManager::Get().FileExists(*AbsPath))
		{
			OutError = FString::Printf(TEXT("file does not exist on disk: '%s'"), *AbsPath);
			return false;
		}
		if (!FFileHelper::LoadFileToArray(OutBuffer, *AbsPath))
		{
			OutError = FString::Printf(TEXT("FFileHelper::LoadFileToArray failed for '%s'"), *AbsPath);
			return false;
		}
		return true;
	}

	/**
	 * Resolve a revision spec to bytes:
	 *   - Empty/"working"  → read the working-copy file at AbsPath directly
	 *   - "head"/"#head"   → call State.GetHistoryItem(0)->Get() to retrieve head revision to a temp file
	 *   - "<numeric>"      → call State.FindHistoryRevision(N)->Get()
	 *   - "<string>"       → call State.FindHistoryRevision(string)->Get() (e.g. Git SHA, Perforce CL)
	 *
	 * Returns true on success — populates OutBytes. Returns false on failure — populates OutError.
	 *
	 * The temp file lives in ``<Project>/Saved/Temp/MCPBridge/SC/`` and is left behind for diagnosis;
	 * UE's normal temp cleanup eventually reaps it.
	 */
	bool SCT_LoadRevisionBytes(const FString& AbsPath, const FString& RevisionSpec,
		const ISourceControlState& State, TArray<uint8>& OutBytes, FString& OutError)
	{
		OutBytes.Reset();
		OutError.Reset();

		const FString Lower = RevisionSpec.ToLower();
		if (RevisionSpec.IsEmpty() || Lower == TEXT("working"))
		{
			return SCT_LoadFileBytes(AbsPath, OutBytes, OutError);
		}

		TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> Revision;
		if (Lower == TEXT("head") || Lower == TEXT("#head"))
		{
			if (State.GetHistorySize() == 0)
			{
				OutError = FString::Printf(TEXT("file '%s' has no history (not yet committed?)"),
					*AbsPath);
				return false;
			}
			Revision = State.GetHistoryItem(0);
		}
		else if (RevisionSpec.IsNumeric())
		{
			const int32 N = FCString::Atoi(*RevisionSpec);
			Revision = State.FindHistoryRevision(N);
		}
		else
		{
			Revision = State.FindHistoryRevision(RevisionSpec);
		}

		if (!Revision.IsValid())
		{
			OutError = FString::Printf(TEXT("revision '%s' not found in history of '%s'"),
				*RevisionSpec, *AbsPath);
			return false;
		}

		// Provider writes the revision to a temp file (its choice of name when InOutFilename empty).
		FString TempFile;
		if (!Revision->Get(TempFile, EConcurrency::Synchronous))
		{
			OutError = FString::Printf(TEXT("ISourceControlRevision::Get failed for '%s' rev '%s'"),
				*AbsPath, *RevisionSpec);
			return false;
		}
		return SCT_LoadFileBytes(TempFile, OutBytes, OutError);
	}

	/**
	 * Convert a byte buffer to a UTF-8-aware FString. Assumes the buffer is text-like
	 * (caller has run SCT_LooksTextual). For typical source files (text formats, .ini, .cs)
	 * the buffer is UTF-8 in UE projects — we decode it via FUTF8ToTCHAR then construct an
	 * FString from the converted span.
	 */
	FString SCT_BytesToString(const TArray<uint8>& Buffer)
	{
		if (Buffer.Num() == 0)
		{
			return FString();
		}
		const ANSICHAR* Data = reinterpret_cast<const ANSICHAR*>(Buffer.GetData());
		FUTF8ToTCHAR Conv(Data, Buffer.Num());
		return FString::ConstructFromPtrSize(Conv.Get(), Conv.Length());
	}

	/**
	 * Generate a minimal unified diff of two text strings. Self-contained — no external dependencies.
	 * Algorithm: line-by-line comparison via LCS (longest common subsequence) on hashed lines for
	 * O(N*M) worst case; sufficient for typical source files <100k lines.
	 *
	 * Output format mirrors `diff -u` lite (no @@ headers — caller knows the file context):
	 *   - " unchanged_line"
	 *   - "-removed_line"
	 *   - "+added_line"
	 *
	 * For tiny files (<100 lines) we use the naive table; for larger files we cap at 1000 lines
	 * per side (~1 M comparisons) and emit a truncation marker. This is acceptable because diff
	 * tooling is a workflow aid, not a precision instrument.
	 */
	FString SCT_BuildUnifiedDiff(const FString& A, const FString& B)
	{
		TArray<FString> LinesA;
		TArray<FString> LinesB;
		A.ParseIntoArrayLines(LinesA, /*bCullEmpty*/ false);
		B.ParseIntoArrayLines(LinesB, /*bCullEmpty*/ false);

		constexpr int32 kMaxLinesPerSide = 1000;
		const bool bTruncatedA = LinesA.Num() > kMaxLinesPerSide;
		const bool bTruncatedB = LinesB.Num() > kMaxLinesPerSide;
		if (bTruncatedA) { LinesA.SetNum(kMaxLinesPerSide); }
		if (bTruncatedB) { LinesB.SetNum(kMaxLinesPerSide); }

		const int32 M = LinesA.Num();
		const int32 N = LinesB.Num();

		// LCS table — table[i][j] = length of LCS of LinesA[0..i] and LinesB[0..j].
		TArray<TArray<int32>> Table;
		Table.SetNum(M + 1);
		for (int32 i = 0; i <= M; ++i)
		{
			Table[i].SetNumZeroed(N + 1);
		}
		for (int32 i = 1; i <= M; ++i)
		{
			for (int32 j = 1; j <= N; ++j)
			{
				if (LinesA[i - 1] == LinesB[j - 1])
				{
					Table[i][j] = Table[i - 1][j - 1] + 1;
				}
				else
				{
					Table[i][j] = FMath::Max(Table[i - 1][j], Table[i][j - 1]);
				}
			}
		}

		// Backtrace into a reversed op list.
		TArray<FString> ReversedOps;
		int32 i = M;
		int32 j = N;
		while (i > 0 && j > 0)
		{
			if (LinesA[i - 1] == LinesB[j - 1])
			{
				ReversedOps.Add(FString::Printf(TEXT(" %s"), *LinesA[i - 1]));
				--i; --j;
			}
			else if (Table[i - 1][j] >= Table[i][j - 1])
			{
				ReversedOps.Add(FString::Printf(TEXT("-%s"), *LinesA[i - 1]));
				--i;
			}
			else
			{
				ReversedOps.Add(FString::Printf(TEXT("+%s"), *LinesB[j - 1]));
				--j;
			}
		}
		while (i > 0)
		{
			ReversedOps.Add(FString::Printf(TEXT("-%s"), *LinesA[i - 1]));
			--i;
		}
		while (j > 0)
		{
			ReversedOps.Add(FString::Printf(TEXT("+%s"), *LinesB[j - 1]));
			--j;
		}

		FString Out;
		Out.Reserve((M + N) * 64);
		if (bTruncatedA || bTruncatedB)
		{
			Out += FString::Printf(
				TEXT("# diff truncated to first %d lines per side (a=%d truncated=%s, b=%d truncated=%s)\n"),
				kMaxLinesPerSide, M, bTruncatedA ? TEXT("true") : TEXT("false"),
				N, bTruncatedB ? TEXT("true") : TEXT("false"));
		}
		for (int32 k = ReversedOps.Num() - 1; k >= 0; --k)
		{
			Out += ReversedOps[k];
			Out += TEXT("\n");
		}
		return Out;
	}
} // namespace

namespace FSourceControlTools
{

// ─── sc.status ────────────────────────────────────────────────────────────────────────────────
//
// Args:    { file_paths?: [string] }    (omit/empty = all known dirty + checked out files)
// Result:  { files: [<state json>, ...] }
//
// Errors:
//   -32602 InvalidParams                       file_paths element is not a string
//   -32010 InvalidPath                         file_paths entry could not be resolved
//   -32013 PathEscape                          file_paths entry escapes sandbox
//   -32045 SourceControlProviderUnavailable    provider not enabled / not available
//
// When file_paths is omitted, the response is the provider's cached state for every file it
// knows about (typically: only files that have been touched this session — SC is lazy). For
// fresh status of arbitrary paths, pass them explicitly.
FMCPResponse Tool_Status(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPResponse ProviderErr;
	if (!SCT_RequireProvider(Request, ProviderErr)) { return ProviderErr; }

	TArray<FString> Paths;
	FMCPResponse ArgErr;
	if (!SCT_ReadFilePathsArray(Request, TEXT("file_paths"), /*bAllowEmpty*/ true, Paths, ArgErr))
	{
		return ArgErr;
	}

	ISourceControlProvider& Provider = ISourceControlModule::Get().GetProvider();

	TArray<FSourceControlStateRef> States;
	if (Paths.Num() > 0)
	{
		// Force a fresh query so revision numbers + remote state reflect current head.
		const TSharedRef<FUpdateStatus, ESPMode::ThreadSafe> StatusOp =
			ISourceControlOperation::Create<FUpdateStatus>();
		StatusOp->SetUpdateHistory(true);
		const ECommandResult::Type ExecResult = Provider.Execute(
			StatusOp, Paths, EConcurrency::Synchronous);
		if (ExecResult == ECommandResult::Failed)
		{
			return FMCPToolHelpers::MakeError(Request, kSCTErrorInternal,
				FString::Printf(TEXT("ISourceControlProvider::Execute(FUpdateStatus) returned Failed "
					"on provider '%s' for %d file(s)"),
					*Provider.GetName().ToString(), Paths.Num()));
		}
		// Even on Succeeded we re-query the cache to get FSourceControlStateRef pointers.
		Provider.GetState(Paths, States, EStateCacheUsage::Use);
	}
	else
	{
		// Snapshot every cached state — caller wants "everything the provider knows".
		States = Provider.GetCachedStateByPredicate(
			[](const FSourceControlStateRef&) { return true; });
	}

	TArray<TSharedPtr<FJsonValue>> FilesArr;
	FilesArr.Reserve(States.Num());
	for (const FSourceControlStateRef& State : States)
	{
		FilesArr.Add(MakeShared<FJsonValueObject>(
			SCT_BuildStateJson(State->GetFilename(), State.Get())));
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetArrayField(TEXT("files"), FilesArr);
	Out->SetStringField(TEXT("provider"), Provider.GetName().ToString());
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── sc.checkout ──────────────────────────────────────────────────────────────────────────────
//
// Args:    { file_paths: [string] }    (required, non-empty)
// Result:  { checked_out: [{path}, ...], failed: [{path, reason}, ...], provider }
//
// Errors:
//   -32602 InvalidParams                       missing/empty file_paths or non-string element
//   -32010 InvalidPath                         file_paths entry could not be resolved
//   -32013 PathEscape                          file_paths entry escapes sandbox
//   -32045 SourceControlProviderUnavailable    provider not enabled / not available
//
// Per-file failure is NOT a hard error — we still return 200 OK with a populated ``failed[]``
// list so the caller can see which subset succeeded. ``failed[].reason`` describes the per-file
// failure (typically "checked out by other" or "already checked out" or "not under SC").
FMCPResponse Tool_Checkout(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPResponse ProviderErr;
	if (!SCT_RequireProvider(Request, ProviderErr)) { return ProviderErr; }

	TArray<FString> Paths;
	FMCPResponse ArgErr;
	if (!SCT_ReadFilePathsArray(Request, TEXT("file_paths"), /*bAllowEmpty*/ false, Paths, ArgErr))
	{
		return ArgErr;
	}

	ISourceControlProvider& Provider = ISourceControlModule::Get().GetProvider();

	// FCheckOut operates on the whole batch atomically — on failure SOME files may have been
	// checked out before the failure. We re-query state afterward to determine which actually
	// got checked out vs which didn't (the precise breakdown depends on provider implementation).
	const TSharedRef<FCheckOut, ESPMode::ThreadSafe> CheckoutOp =
		ISourceControlOperation::Create<FCheckOut>();
	const ECommandResult::Type ExecResult = Provider.Execute(
		CheckoutOp, Paths, EConcurrency::Synchronous);

	// Re-fetch state for each path so we can report per-file outcome regardless of batch-level
	// success/failure.
	TArray<FSourceControlStateRef> States;
	Provider.GetState(Paths, States, EStateCacheUsage::Use);

	TMap<FString, FSourceControlStateRef> StateByPath;
	StateByPath.Reserve(States.Num());
	for (const FSourceControlStateRef& State : States)
	{
		StateByPath.Add(State->GetFilename(), State);
	}

	TArray<TSharedPtr<FJsonValue>> CheckedOutArr;
	TArray<TSharedPtr<FJsonValue>> FailedArr;
	CheckedOutArr.Reserve(Paths.Num());
	for (const FString& Path : Paths)
	{
		const FSourceControlStateRef* StatePtr = StateByPath.Find(Path);
		if (StatePtr && (*StatePtr)->IsCheckedOut())
		{
			TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("path"), Path);
			CheckedOutArr.Add(MakeShared<FJsonValueObject>(Obj));
		}
		else
		{
			TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("path"), Path);
			FString Reason = TEXT("checkout failed (provider rejected)");
			if (StatePtr)
			{
				const ISourceControlState& State = StatePtr->Get();
				if (State.IsCheckedOutOther())
				{
					FString OtherUser;
					State.IsCheckedOutOther(&OtherUser);
					Reason = FString::Printf(TEXT("checked out by other user: '%s'"), *OtherUser);
				}
				else if (!State.IsSourceControlled())
				{
					Reason = TEXT("file is not under source control");
				}
				else if (State.IsConflicted())
				{
					Reason = TEXT("file is in conflict state (resolve first)");
				}
				else if (!State.CanCheckout())
				{
					Reason = TEXT("provider reports file is not checkout-eligible");
				}
			}
			Obj->SetStringField(TEXT("reason"), Reason);
			FailedArr.Add(MakeShared<FJsonValueObject>(Obj));
		}
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetArrayField(TEXT("checked_out"), CheckedOutArr);
	Out->SetArrayField(TEXT("failed"),      FailedArr);
	Out->SetStringField(TEXT("provider"),   Provider.GetName().ToString());
	Out->SetBoolField(TEXT("batch_ok"),     ExecResult == ECommandResult::Succeeded);
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── sc.diff ──────────────────────────────────────────────────────────────────────────────────
//
// Args:    { file_path: string, revision_a?: string, revision_b?: string }
//          revision defaults: a=head, b=working
//          revision spec: "" / "working" (working copy), "head"/"#head", "<numeric>", "<string>"
// Result:  Text path:   { is_binary: false, diff_text, bytes_a, bytes_b }
//          Binary path: { is_binary: true, base64_a, base64_b, bytes_a, bytes_b }
//
// Errors:
//   -32602 InvalidParams                       missing file_path
//   -32010 InvalidPath                         file_path could not be resolved
//   -32013 PathEscape                          file_path escapes sandbox
//   -32017 InputTooLarge                       either side exceeds kSCTBinaryDiffMaxBytesPerSide
//   -32045 SourceControlProviderUnavailable    provider not enabled / not available
//   -32603 Internal                            revision could not be loaded
FMCPResponse Tool_Diff(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPResponse ProviderErr;
	if (!SCT_RequireProvider(Request, ProviderErr)) { return ProviderErr; }

	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kSCTErrorInvalidParams, TEXT("missing args object"));
	}
	FString RawPath;
	if (!Request.Args->TryGetStringField(TEXT("file_path"), RawPath) || RawPath.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kSCTErrorInvalidParams,
			TEXT("missing required string field 'file_path'"));
	}

	FString AbsPath;
	FString ResolveErr;
	int32 ResolveCode = 0;
	if (!SCT_ResolveSCPath(RawPath, AbsPath, ResolveErr, ResolveCode))
	{
		return FMCPToolHelpers::MakeError(Request, ResolveCode, ResolveErr);
	}

	// revision_a default: head; revision_b default: working
	FString RevA;
	FString RevB;
	Request.Args->TryGetStringField(TEXT("revision_a"), RevA);
	Request.Args->TryGetStringField(TEXT("revision_b"), RevB);
	if (RevA.IsEmpty()) { RevA = TEXT("head"); }
	if (RevB.IsEmpty()) { RevB = TEXT("working"); }

	ISourceControlProvider& Provider = ISourceControlModule::Get().GetProvider();

	// Need history; force a fresh status query.
	const TSharedRef<FUpdateStatus, ESPMode::ThreadSafe> StatusOp =
		ISourceControlOperation::Create<FUpdateStatus>();
	StatusOp->SetUpdateHistory(true);
	TArray<FString> Files;
	Files.Add(AbsPath);
	Provider.Execute(StatusOp, Files, EConcurrency::Synchronous);

	const FSourceControlStatePtr StatePtr = Provider.GetState(AbsPath, EStateCacheUsage::Use);
	if (!StatePtr.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kSCTErrorInternal,
			FString::Printf(TEXT("provider returned no state for '%s'"), *AbsPath));
	}

	TArray<uint8> BytesA;
	TArray<uint8> BytesB;
	FString LoadErr;
	if (!SCT_LoadRevisionBytes(AbsPath, RevA, *StatePtr, BytesA, LoadErr))
	{
		return FMCPToolHelpers::MakeError(Request, kSCTErrorInternal,
			FString::Printf(TEXT("revision_a load failed: %s"), *LoadErr));
	}
	if (!SCT_LoadRevisionBytes(AbsPath, RevB, *StatePtr, BytesB, LoadErr))
	{
		return FMCPToolHelpers::MakeError(Request, kSCTErrorInternal,
			FString::Printf(TEXT("revision_b load failed: %s"), *LoadErr));
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetNumberField(TEXT("bytes_a"), static_cast<double>(BytesA.Num()));
	Out->SetNumberField(TEXT("bytes_b"), static_cast<double>(BytesB.Num()));
	Out->SetStringField(TEXT("path"), AbsPath);
	Out->SetStringField(TEXT("revision_a"), RevA);
	Out->SetStringField(TEXT("revision_b"), RevB);

	const bool bTextual = SCT_LooksTextual(BytesA) && SCT_LooksTextual(BytesB);
	if (bTextual)
	{
		const FString TextA = SCT_BytesToString(BytesA);
		const FString TextB = SCT_BytesToString(BytesB);
		const FString Diff = SCT_BuildUnifiedDiff(TextA, TextB);
		Out->SetBoolField(TEXT("is_binary"), false);
		Out->SetStringField(TEXT("diff_text"), Diff);
	}
	else
	{
		if (BytesA.Num() > kSCTBinaryDiffMaxBytesPerSide ||
			BytesB.Num() > kSCTBinaryDiffMaxBytesPerSide)
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorInputTooLarge,
				FString::Printf(
					TEXT("binary diff exceeds cap (%lld bytes/side): a=%d b=%d. "
						 "Use cb.export for large binaries."),
					kSCTBinaryDiffMaxBytesPerSide, BytesA.Num(), BytesB.Num()));
		}
		Out->SetBoolField(TEXT("is_binary"), true);
		Out->SetStringField(TEXT("base64_a"),
			FBase64::Encode(BytesA.GetData(), BytesA.Num()));
		Out->SetStringField(TEXT("base64_b"),
			FBase64::Encode(BytesB.GetData(), BytesB.Num()));
	}
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── sc.diff_binary ──────────────────────────────────────────────────────────────────────────
//
// Same as sc.diff but skips the text-detection heuristic. Always returns base64 chunks even for
// text files (caller may want raw bytes for line-ending introspection or to encode UTF-16 etc.).
//
// Args + result shape identical to sc.diff in the binary branch — ``is_binary`` is always true.
// Error codes identical to sc.diff.
FMCPResponse Tool_DiffBinary(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPResponse ProviderErr;
	if (!SCT_RequireProvider(Request, ProviderErr)) { return ProviderErr; }

	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kSCTErrorInvalidParams, TEXT("missing args object"));
	}
	FString RawPath;
	if (!Request.Args->TryGetStringField(TEXT("file_path"), RawPath) || RawPath.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kSCTErrorInvalidParams,
			TEXT("missing required string field 'file_path'"));
	}

	FString AbsPath;
	FString ResolveErr;
	int32 ResolveCode = 0;
	if (!SCT_ResolveSCPath(RawPath, AbsPath, ResolveErr, ResolveCode))
	{
		return FMCPToolHelpers::MakeError(Request, ResolveCode, ResolveErr);
	}

	FString RevA;
	FString RevB;
	Request.Args->TryGetStringField(TEXT("revision_a"), RevA);
	Request.Args->TryGetStringField(TEXT("revision_b"), RevB);
	if (RevA.IsEmpty()) { RevA = TEXT("head"); }
	if (RevB.IsEmpty()) { RevB = TEXT("working"); }

	ISourceControlProvider& Provider = ISourceControlModule::Get().GetProvider();

	const TSharedRef<FUpdateStatus, ESPMode::ThreadSafe> StatusOp =
		ISourceControlOperation::Create<FUpdateStatus>();
	StatusOp->SetUpdateHistory(true);
	TArray<FString> Files;
	Files.Add(AbsPath);
	Provider.Execute(StatusOp, Files, EConcurrency::Synchronous);

	const FSourceControlStatePtr StatePtr = Provider.GetState(AbsPath, EStateCacheUsage::Use);
	if (!StatePtr.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kSCTErrorInternal,
			FString::Printf(TEXT("provider returned no state for '%s'"), *AbsPath));
	}

	TArray<uint8> BytesA;
	TArray<uint8> BytesB;
	FString LoadErr;
	if (!SCT_LoadRevisionBytes(AbsPath, RevA, *StatePtr, BytesA, LoadErr))
	{
		return FMCPToolHelpers::MakeError(Request, kSCTErrorInternal,
			FString::Printf(TEXT("revision_a load failed: %s"), *LoadErr));
	}
	if (!SCT_LoadRevisionBytes(AbsPath, RevB, *StatePtr, BytesB, LoadErr))
	{
		return FMCPToolHelpers::MakeError(Request, kSCTErrorInternal,
			FString::Printf(TEXT("revision_b load failed: %s"), *LoadErr));
	}

	if (BytesA.Num() > kSCTBinaryDiffMaxBytesPerSide ||
		BytesB.Num() > kSCTBinaryDiffMaxBytesPerSide)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInputTooLarge,
			FString::Printf(
				TEXT("binary diff exceeds cap (%lld bytes/side): a=%d b=%d. "
					 "Use cb.export for large binaries."),
				kSCTBinaryDiffMaxBytesPerSide, BytesA.Num(), BytesB.Num()));
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("is_binary"), true);
	Out->SetStringField(TEXT("path"), AbsPath);
	Out->SetStringField(TEXT("revision_a"), RevA);
	Out->SetStringField(TEXT("revision_b"), RevB);
	Out->SetNumberField(TEXT("bytes_a"), static_cast<double>(BytesA.Num()));
	Out->SetNumberField(TEXT("bytes_b"), static_cast<double>(BytesB.Num()));
	Out->SetStringField(TEXT("base64_a"),
		FBase64::Encode(BytesA.GetData(), BytesA.Num()));
	Out->SetStringField(TEXT("base64_b"),
		FBase64::Encode(BytesB.GetData(), BytesB.Num()));
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── sc.revert ────────────────────────────────────────────────────────────────────────────────
//
// Args:    { file_paths: [string], confirm_destructive: true }   (both required)
// Result:  { reverted: [{path}, ...], failed: [{path, reason}, ...], provider }
//
// Errors:
//   -32602 InvalidParams                       missing/empty file_paths
//   -32010 InvalidPath / -32013 PathEscape     per-path resolution failures
//   -32033 ReparentUnsafe (reused)             missing confirm_destructive=true (destructive op)
//   -32045 SourceControlProviderUnavailable    provider not enabled / not available
//
// Revert is destructive — local changes are LOST. Caller MUST pass ``confirm_destructive=true``;
// missing or false → -32033 with a recovery hint. Per-file failure surfaced in ``failed[]``
// (same pattern as sc.checkout).
FMCPResponse Tool_Revert(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPResponse ProviderErr;
	if (!SCT_RequireProvider(Request, ProviderErr)) { return ProviderErr; }

	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kSCTErrorInvalidParams, TEXT("missing args object"));
	}

	// Destructive-confirm gate FIRST so we don't even resolve paths if caller is unaware.
	bool bConfirm = false;
	if (!Request.Args->TryGetBoolField(TEXT("confirm_destructive"), bConfirm) || !bConfirm)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorReparentUnsafe,
			TEXT("sc.revert is destructive (local changes LOST); pass confirm_destructive=true to proceed"));
	}

	TArray<FString> Paths;
	FMCPResponse ArgErr;
	if (!SCT_ReadFilePathsArray(Request, TEXT("file_paths"), /*bAllowEmpty*/ false, Paths, ArgErr))
	{
		return ArgErr;
	}

	ISourceControlProvider& Provider = ISourceControlModule::Get().GetProvider();

	const TSharedRef<FRevert, ESPMode::ThreadSafe> RevertOp =
		ISourceControlOperation::Create<FRevert>();
	const ECommandResult::Type ExecResult = Provider.Execute(
		RevertOp, Paths, EConcurrency::Synchronous);

	// Re-query state to determine per-file outcome.
	TArray<FSourceControlStateRef> States;
	Provider.GetState(Paths, States, EStateCacheUsage::Use);

	TMap<FString, FSourceControlStateRef> StateByPath;
	StateByPath.Reserve(States.Num());
	for (const FSourceControlStateRef& State : States)
	{
		StateByPath.Add(State->GetFilename(), State);
	}

	TArray<TSharedPtr<FJsonValue>> RevertedArr;
	TArray<TSharedPtr<FJsonValue>> FailedArr;
	for (const FString& Path : Paths)
	{
		const FSourceControlStateRef* StatePtr = StateByPath.Find(Path);
		// Success criteria: post-revert state has no pending change (not checked out, not modified,
		// not added, not deleted).
		const bool bSuccessLike = StatePtr &&
			!(*StatePtr)->IsCheckedOut() &&
			!(*StatePtr)->IsModified() &&
			!(*StatePtr)->IsAdded() &&
			!(*StatePtr)->IsDeleted();
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("path"), Path);
		if (bSuccessLike)
		{
			RevertedArr.Add(MakeShared<FJsonValueObject>(Obj));
		}
		else
		{
			FString Reason = TEXT("revert failed (post-revert state still shows pending change)");
			if (StatePtr)
			{
				if ((*StatePtr)->IsConflicted())
				{
					Reason = TEXT("file is in conflict (resolve first)");
				}
				else if (!(*StatePtr)->IsSourceControlled())
				{
					Reason = TEXT("file is not under source control (nothing to revert)");
				}
			}
			Obj->SetStringField(TEXT("reason"), Reason);
			FailedArr.Add(MakeShared<FJsonValueObject>(Obj));
		}
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetArrayField(TEXT("reverted"),  RevertedArr);
	Out->SetArrayField(TEXT("failed"),    FailedArr);
	Out->SetStringField(TEXT("provider"), Provider.GetName().ToString());
	Out->SetBoolField(TEXT("batch_ok"),   ExecResult == ECommandResult::Succeeded);
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── Registration ──────────────────────────────────────────────────────────────────────────────
void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	RegisterTool(TEXT("sc.status"),      &Tool_Status,     /*Lane A*/ false);
	RegisterTool(TEXT("sc.checkout"),    &Tool_Checkout,   /*Lane A*/ false);
	RegisterTool(TEXT("sc.diff"),        &Tool_Diff,       /*Lane A*/ false);
	RegisterTool(TEXT("sc.diff_binary"), &Tool_DiffBinary, /*Lane A*/ false);
	RegisterTool(TEXT("sc.revert"),      &Tool_Revert,     /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("Phase 6 Chunk A (Source Control): registered 5 sc.* sync handlers ")
		TEXT("(status/checkout/diff/diff_binary/revert, all Lane A)"));
}

} // namespace FSourceControlTools

#undef LOCTEXT_NAMESPACE
