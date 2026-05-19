// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "Logging/LogVerbosity.h"
#include "Misc/DateTime.h"
#include "Misc/OutputDevice.h"
#include "UObject/NameTypes.h"

/**
 * One captured log entry. Stored in the ring buffer; serialised on demand by log.tail / log.search.
 *
 * Field sizes are deliberately compact: FString for Message (variable) + Category (FName-backed),
 * uint8 for Verbosity (matches ELogVerbosity::Type's underlying), FDateTime for timestamp.
 *
 * The struct does NOT inherit from FOutputDevice — it's purely data.
 */
struct FMCPLogEntry
{
	FDateTime Timestamp;
	FName Category;
	ELogVerbosity::Type Verbosity = ELogVerbosity::Log;
	FString Message;
};

/**
 * GLog-attached output device that captures the last N log lines in a ring buffer for AI inspection.
 *
 * **Lifecycle:** module StartupModule constructs the device, calls
 * `FOutputDeviceRedirector::Get()->AddOutputDevice(this)`. Module ShutdownModule reverses (Remove
 * before destruction is critical — GLog dereferences output devices unconditionally).
 *
 * **Thread safety:** `Serialize` can be invoked from any thread that calls UE_LOG. Ring-buffer
 * append + read methods are serialised by EntriesLock. We override `CanBeUsedOnAnyThread() = true`
 * so the redirector skips its internal thread-marshalling and calls us directly.
 *
 * **Ring buffer:** Fixed at 5000 entries (per blueprint v2 §3.2). At 100 logs/sec ≈ 50 seconds
 * of history — usually enough for an AI to inspect the immediate aftermath of a tool call. Larger
 * windows + cvar control are deferred to Phase 2.
 *
 * **Self-recursion note:** Serialize never calls UE_LOG itself — only TArray assign + atomic
 * increment under the lock. If a future maintainer adds logging here they MUST add a TLS-based
 * recursion guard or the FCriticalSection becomes a deadlock against itself.
 *
 * **No deduplication / coalescing.** Repeated identical entries each consume a slot. Higher-level
 * filtering happens at log.tail / log.search call time.
 */
class UNREALMCPBRIDGE_API FMCPLogStream : public FOutputDevice
{
public:
	/** Singleton accessor. The instance is owned by the module and lives for module lifetime. */
	static FMCPLogStream& Get();

	/** Attach to GLog. Idempotent — second call is a logged no-op. Must be called from game thread. */
	void Attach();

	/** Detach from GLog. Idempotent. Safe to call from game thread during shutdown. */
	void Detach();

	// FOutputDevice — invoked from any thread by the GLog redirector.
	virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override;
	virtual bool CanBeUsedOnAnyThread() const override { return true; }
	virtual bool CanBeUsedOnMultipleThreads() const override { return true; }
	virtual bool IsMemoryOnly() const override { return true; }

	/**
	 * Return the last N entries in chronological order (oldest first).
	 *
	 * If CategoryFilter is non-null, only entries whose Category matches are returned.
	 * N is clamped to [1, kMaxEntries]; 0 or negative returns an empty array.
	 *
	 * Allocates — copies the matching FMCPLogEntry instances out of the ring under EntriesLock.
	 */
	TArray<FMCPLogEntry> GetLastN(int32 N, const FName* CategoryFilter) const;

	/**
	 * Regex search across the ring (Message field only — Category/Timestamp not regex'd here, use
	 * GetLastN for category filtering).
	 *
	 * Returns up to MaxResults matches in chronological order. Matches deeper than the ring depth
	 * are simply unavailable — older entries have already rolled out.
	 *
	 * The regex is compiled fresh per call (FRegexPattern). For frequent searches caller should
	 * snapshot via GetLastN(kMaxEntries) and do its own matching.
	 */
	TArray<FMCPLogEntry> Search(const FString& Pattern, int32 MaxResults) const;

	/** Current entry count [0, kMaxEntries]. */
	int32 GetEntryCount() const;

	/** Total log entries observed since attach (includes those that have rolled out of the ring). */
	int64 GetTotalObserved() const { return TotalObserved.load(std::memory_order_relaxed); }

	/**
	 * Phase 6 Chunk D — clear the ring buffer of all entries.
	 *
	 * Returns the number of entries dropped (the prior Count). Does NOT clear file logs
	 * (UE writes those append-only to Saved/Logs/<Project>.log via FOutputDeviceFile). Does NOT
	 * reset TotalObserved — that counter is cumulative-since-attach and useful for observing
	 * total log volume independent of the bounded ring.
	 *
	 * Thread safety: acquires EntriesLock, walks no further than kMaxEntries assignments.
	 * Game-thread-only by convention (log.clear is a Lane A tool). Allowed any-thread because
	 * the only mutation is under the lock — but cleared content can race against the next
	 * Serialize() call from any thread.
	 */
	int32 Clear();

	/**
	 * Phase 6 Chunk D — snapshot of all categories that have produced at least one log entry
	 * since attach. Returns (Category, MostRecentVerbosity) tuples sorted by name.
	 *
	 * The MostRecentVerbosity is the LAST observed verbosity for that category (NOT the current
	 * FLogCategoryBase::GetVerbosity() — UE's suppression registry doesn't expose name→category
	 * lookup publicly, so we report what we've seen instead). For filtered logging the public
	 * surface (``log.list_categories``) makes this distinction explicit in its docstring.
	 *
	 * Acquires EntriesLock briefly (TMap copy). Caller owns the returned array.
	 */
	struct FObservedCategoryInfo
	{
		FName Category;
		ELogVerbosity::Type LastObservedVerbosity = ELogVerbosity::Log;
		int64 ObservationCount = 0;
	};
	TArray<FObservedCategoryInfo> GetObservedCategories() const;

	/** Ring buffer capacity. Public so log.tail / log.search can validate N parameters. */
	static constexpr int32 kMaxEntries = 5000;

private:
	FMCPLogStream();
	virtual ~FMCPLogStream() override;
	FMCPLogStream(const FMCPLogStream&) = delete;
	FMCPLogStream& operator=(const FMCPLogStream&) = delete;

	/**
	 * Fixed-size ring buffer of entries. Index `Head` is the next write slot; `Count` is the number
	 * of valid entries (<= kMaxEntries). Once Count == kMaxEntries the buffer wraps and Head
	 * overwrites the oldest entry on each Serialize.
	 *
	 * Read methods walk in chronological order: oldest entry is at
	 *   (Count == kMaxEntries) ? Head : 0
	 */
	TArray<FMCPLogEntry> Entries;
	int32 Head = 0;       // next write index
	int32 Count = 0;      // current valid count

	mutable FCriticalSection EntriesLock;

	std::atomic<int64> TotalObserved{0};
	std::atomic<bool> bAttached{false};

	/**
	 * Phase 6 Chunk D — observed-categories registry. Populated lazily inside Serialize() so
	 * log.list_categories can enumerate everything that's actually emitted log entries since
	 * attach. UE's FLogSuppressionImplementation owns the canonical FName→FLogCategoryBase* map
	 * but doesn't expose it through the public FLogSuppressionInterface (only AssociateSuppress /
	 * DisassociateSuppress / ProcessConfigAndCommandLine are public). Our observed-set is a
	 * pragmatic subset: every category that has logged at least once since this stream attached.
	 *
	 * Value is the LAST verbosity seen for that category (used as "current verbosity" hint in
	 * log.list_categories) + a cumulative count of observations. Updated under EntriesLock.
	 */
	struct FCategoryStats
	{
		ELogVerbosity::Type LastVerbosity = ELogVerbosity::Log;
		int64 ObservationCount = 0;
	};
	TMap<FName, FCategoryStats> ObservedCategories;
};
