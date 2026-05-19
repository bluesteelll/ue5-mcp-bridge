// Copyright FatumGame. All Rights Reserved.

#include "FMCPLogStream.h"

#include "UnrealMCPBridge.h"

#include "Internationalization/Regex.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/ScopeLock.h"

FMCPLogStream& FMCPLogStream::Get()
{
	static FMCPLogStream Instance;
	return Instance;
}

FMCPLogStream::FMCPLogStream()
{
	// Pre-size the ring so we don't realloc on every Serialize until the buffer fills up.
	Entries.SetNum(kMaxEntries);
}

FMCPLogStream::~FMCPLogStream()
{
	// If the module shut down cleanly Detach() already happened. Defensive call in case the
	// destructor runs first (static destruction order with the static Get() singleton).
	if (bAttached.load(std::memory_order_acquire))
	{
		Detach();
	}
}

void FMCPLogStream::Attach()
{
	check(IsInGameThread());
	bool bExpected = false;
	if (!bAttached.compare_exchange_strong(bExpected, true, std::memory_order_acq_rel))
	{
		UE_LOG(LogMCP, Verbose, TEXT("FMCPLogStream::Attach: already attached, skipping"));
		return;
	}

	FOutputDeviceRedirector* Redirector = FOutputDeviceRedirector::Get();
	if (!Redirector)
	{
		UE_LOG(LogMCP, Warning, TEXT("FMCPLogStream::Attach: GLog redirector unavailable — log streaming disabled"));
		bAttached.store(false, std::memory_order_release);
		return;
	}

	Redirector->AddOutputDevice(this);
	UE_LOG(LogMCP, Log, TEXT("FMCPLogStream attached (ring capacity=%d)"), kMaxEntries);
}

void FMCPLogStream::Detach()
{
	check(IsInGameThread());
	bool bExpected = true;
	if (!bAttached.compare_exchange_strong(bExpected, false, std::memory_order_acq_rel))
	{
		return; // not attached
	}

	if (FOutputDeviceRedirector* Redirector = FOutputDeviceRedirector::Get())
	{
		Redirector->RemoveOutputDevice(this);
	}
	UE_LOG(LogMCP, Log, TEXT("FMCPLogStream detached"));
}

void FMCPLogStream::Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category)
{
	// Filter the verbosity NoLogging / SetColor pseudo-entries — they're not real log lines.
	if (Verbosity == ELogVerbosity::NoLogging || Verbosity == ELogVerbosity::SetColor)
	{
		return;
	}
	if (!V || *V == 0)
	{
		return;
	}

	const ELogVerbosity::Type MaskedVerbosity =
		static_cast<ELogVerbosity::Type>(Verbosity & ELogVerbosity::VerbosityMask);

	TotalObserved.fetch_add(1, std::memory_order_relaxed);

	// Construct the entry outside the lock so we don't hold it during the FString copy.
	FMCPLogEntry NewEntry;
	NewEntry.Timestamp = FDateTime::UtcNow();
	NewEntry.Category = Category;
	NewEntry.Verbosity = MaskedVerbosity;
	NewEntry.Message = V;

	FScopeLock Lock(&EntriesLock);
	Entries[Head] = MoveTemp(NewEntry);
	Head = (Head + 1) % kMaxEntries;
	if (Count < kMaxEntries)
	{
		++Count;
	}

	// Phase 6 Chunk D — update observed-categories registry for log.list_categories. Under the
	// same lock as the ring write so log.list_categories sees a consistent snapshot. TMap::FindOrAdd
	// is O(1) amortised; per-category stats are minimal (12 bytes + FName key).
	FCategoryStats& Stats = ObservedCategories.FindOrAdd(Category);
	Stats.LastVerbosity = MaskedVerbosity;
	++Stats.ObservationCount;
}

int32 FMCPLogStream::GetEntryCount() const
{
	FScopeLock Lock(&EntriesLock);
	return Count;
}

TArray<FMCPLogEntry> FMCPLogStream::GetLastN(int32 N, const FName* CategoryFilter) const
{
	TArray<FMCPLogEntry> Out;
	if (N <= 0)
	{
		return Out;
	}
	N = FMath::Min(N, kMaxEntries);

	FScopeLock Lock(&EntriesLock);

	// Walk in chronological order, oldest first.
	// Oldest index = (Count == kMaxEntries) ? Head : 0
	const int32 OldestIdx = (Count == kMaxEntries) ? Head : 0;

	// Two-phase collect with category filter (cheap to do under the lock — no allocations
	// happen for entries that don't match because we only emplace into Out on match).
	Out.Reserve(FMath::Min(N, Count));

	// We must iterate ALL valid entries because filtered selection can skip many. To return the
	// LAST N matches in chronological order, walk newest-to-oldest, collect, then reverse.
	TArray<FMCPLogEntry> Reverse;
	Reverse.Reserve(N);

	for (int32 Step = 0; Step < Count && Reverse.Num() < N; ++Step)
	{
		// Newest-first: (Head - 1 - Step) mod kMaxEntries — Head points at the NEXT write slot,
		// so Head-1 is the most recently written entry.
		const int32 Idx = (Head - 1 - Step + kMaxEntries * 2) % kMaxEntries;
		const FMCPLogEntry& E = Entries[Idx];
		if (CategoryFilter && E.Category != *CategoryFilter)
		{
			continue;
		}
		Reverse.Add(E);
	}

	// Reverse to chronological order.
	for (int32 i = Reverse.Num() - 1; i >= 0; --i)
	{
		Out.Add(MoveTemp(Reverse[i]));
	}

	// Silence unused-variable warnings if OldestIdx isn't read above (it documents intent).
	(void)OldestIdx;
	return Out;
}

int32 FMCPLogStream::Clear()
{
	FScopeLock Lock(&EntriesLock);
	const int32 PriorCount = Count;
	// Reset the ring metadata. We leave the Entries TArray sized but stale; future Serialize()
	// calls overwrite slots in-order from Head=0. Avoids the realloc cost of SetNum(0).
	Head = 0;
	Count = 0;
	// Leave ObservedCategories intact — it's a cumulative-since-attach catalogue, not a ring
	// snapshot. Operator intuition: log.clear wipes log.tail content; log.list_categories continues
	// reporting every category seen since the editor started (matching UE's own Log List behaviour
	// which surveys the suppression registry, not the in-memory ring).
	return PriorCount;
}

TArray<FMCPLogStream::FObservedCategoryInfo> FMCPLogStream::GetObservedCategories() const
{
	TArray<FObservedCategoryInfo> Out;
	FScopeLock Lock(&EntriesLock);
	Out.Reserve(ObservedCategories.Num());
	for (const TPair<FName, FCategoryStats>& Pair : ObservedCategories)
	{
		FObservedCategoryInfo Info;
		Info.Category = Pair.Key;
		Info.LastObservedVerbosity = Pair.Value.LastVerbosity;
		Info.ObservationCount = Pair.Value.ObservationCount;
		Out.Add(MoveTemp(Info));
	}
	return Out;
}

TArray<FMCPLogEntry> FMCPLogStream::Search(const FString& Pattern, int32 MaxResults) const
{
	TArray<FMCPLogEntry> Out;
	if (Pattern.IsEmpty() || MaxResults <= 0)
	{
		return Out;
	}
	MaxResults = FMath::Min(MaxResults, kMaxEntries);

	// Compile regex once. FRegexPattern has no IsValid()-style check; an invalid pattern produces
	// FRegexMatcher::FindNext() that returns false immediately — caller gets zero results, no crash.
	const FRegexPattern Re(Pattern);

	FScopeLock Lock(&EntriesLock);

	// Walk newest-first, accumulate matches, reverse to chronological at the end.
	TArray<FMCPLogEntry> Reverse;
	Reverse.Reserve(MaxResults);

	for (int32 Step = 0; Step < Count && Reverse.Num() < MaxResults; ++Step)
	{
		const int32 Idx = (Head - 1 - Step + kMaxEntries * 2) % kMaxEntries;
		const FMCPLogEntry& E = Entries[Idx];
		FRegexMatcher Matcher(Re, E.Message);
		if (Matcher.FindNext())
		{
			Reverse.Add(E);
		}
	}

	Out.Reserve(Reverse.Num());
	for (int32 i = Reverse.Num() - 1; i >= 0; --i)
	{
		Out.Add(MoveTemp(Reverse[i]));
	}
	return Out;
}
