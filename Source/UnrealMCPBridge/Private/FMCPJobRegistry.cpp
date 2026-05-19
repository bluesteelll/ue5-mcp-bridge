// Copyright FatumGame. All Rights Reserved.

#include "FMCPJobRegistry.h"

#include "UnrealMCPBridge.h"

#include "Async/Async.h"
#include "Async/Future.h"
#include "Containers/Ticker.h"
#include "HAL/PlatformTime.h"
#include "HAL/RunnableThread.h"
#include "Misc/IQueuedWork.h"
#include "Misc/QueuedThreadPool.h"
#include "Misc/ScopeLock.h"

const TCHAR* LexJobState(EMCPJobState State)
{
	switch (State)
	{
		case EMCPJobState::Pending:   return TEXT("Pending");
		case EMCPJobState::Running:   return TEXT("Running");
		case EMCPJobState::Succeeded: return TEXT("Succeeded");
		case EMCPJobState::Failed:    return TEXT("Failed");
		case EMCPJobState::Cancelled: return TEXT("Cancelled");
		default:                      return TEXT("Unknown");
	}
}

/**
 * IQueuedWork wrapper around a single (FMCPJob, FBody) pair. Self-deletes after DoThreadedWork
 * returns so the registry doesn't need to track work items separately from jobs.
 *
 * Holds a TSharedRef<FMCPJob> so the job survives even if the registry GCs its entry mid-execution
 * (which the TTL pass cannot do — terminal-only — but defensive against future refactors).
 */
class FMCPJobWork final : public IQueuedWork
{
public:
	FMCPJobWork(TSharedRef<FMCPJob> InJob, FMCPJobRegistry::FBody InBody)
		: Job(MoveTemp(InJob))
		, Body(MoveTemp(InBody))
	{
	}

	virtual void DoThreadedWork() override
	{
		// Delegate the heavy lifting to the registry so the worker just orchestrates.
		FMCPJobRegistry::Get().ExecuteJobOnWorker(Job, MoveTemp(Body));
		delete this; // self-cleanup — IQueuedWork has no completion callback
	}

	virtual void Abandon() override
	{
		// Pool is being destroyed before we started. Mark Cancelled so any waiters unblock and
		// match the documented terminal-state semantics.
		Job->State.store(EMCPJobState::Cancelled, std::memory_order_release);
		Job->FinishedAt = FPlatformTime::Seconds();
		Job->ErrorMessage = TEXT("worker pool abandoned (likely shutdown)");
		delete this;
	}

	virtual const TCHAR* GetDebugName() const override
	{
		return TEXT("MCPJobWork");
	}

private:
	TSharedRef<FMCPJob> Job;
	FMCPJobRegistry::FBody Body;
};

FMCPJobRegistry& FMCPJobRegistry::Get()
{
	static FMCPJobRegistry Instance;
	return Instance;
}

FMCPJobRegistry::~FMCPJobRegistry()
{
	Shutdown();
}

void FMCPJobRegistry::EnsureInitialized()
{
	// Double-checked init under the same lock the rest of the registry uses. Pool creation is
	// idempotent — but we don't want two threads racing to create two pools.
	if (bInitialised.load(std::memory_order_acquire))
	{
		return;
	}

	FScopeLock Lock(&JobsLock);
	if (bInitialised.load(std::memory_order_relaxed))
	{
		return;
	}

	if (bShutdown.load(std::memory_order_acquire))
	{
		UE_LOG(LogMCP, Warning, TEXT("FMCPJobRegistry::EnsureInitialized called after Shutdown — no-op"));
		return;
	}

	const int32 NumCores = FPlatformMisc::NumberOfCores();
	const uint32 NumWorkers = static_cast<uint32>(FMath::Max(1, NumCores - 2));

	Pool = FQueuedThreadPool::Allocate();
	const bool bPoolOk = Pool->Create(
		NumWorkers,
		256 * 1024,           // 256 KiB stack — generous, matches our typical Python embed needs
		TPri_Normal,
		TEXT("MCPJobPool"));

	if (!bPoolOk)
	{
		UE_LOG(LogMCP, Error, TEXT("FMCPJobRegistry: FQueuedThreadPool::Create failed (NumWorkers=%u). Jobs will queue but never execute."),
			NumWorkers);
		// Don't delete Pool — leaving it valid keeps AddQueuedWork from crashing; the queued work
		// will simply never be picked up. We surface this loudly above.
	}

	// TTL ticker — coreTicker fires on game thread, callback is bound to a registry method.
	TtlTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		TEXT("MCPJobRegistry.TtlSweep"),
		kTtlSweepIntervalSeconds,
		[this](float DeltaTime) -> bool { return TickTtlCleanup(DeltaTime); });

	bInitialised.store(true, std::memory_order_release);
	UE_LOG(LogMCP, Log,
		TEXT("FMCPJobRegistry initialised — pool=%s workers=%u stack=256KiB ttl=%.0fs sweep=%.0fs"),
		bPoolOk ? TEXT("OK") : TEXT("FAILED"),
		NumWorkers, kTtlSeconds, kTtlSweepIntervalSeconds);
}

void FMCPJobRegistry::Shutdown()
{
	bool bExpected = false;
	if (!bShutdown.compare_exchange_strong(bExpected, true, std::memory_order_acq_rel))
	{
		return; // already shut down
	}

	// Remove the ticker first so it doesn't fire during teardown.
	if (TtlTickerHandle.IsValid())
	{
		FTSTicker::RemoveTicker(TtlTickerHandle);
		TtlTickerHandle.Reset();
	}

	// Signal cancel to every active job + drop the map. Bodies that ignore the flag will still run
	// to completion on the worker — pool->Destroy() blocks on running tasks. This is intentional;
	// hard-killing a job mid-Python could leave the interpreter in a half-state.
	TArray<TSharedRef<FMCPJob>> Snapshot;
	{
		FScopeLock Lock(&JobsLock);
		Snapshot.Reserve(Jobs.Num());
		for (TPair<FGuid, TSharedRef<FMCPJob>>& Pair : Jobs)
		{
			Pair.Value->bCancelRequested.store(true, std::memory_order_release);
			Snapshot.Add(Pair.Value);
		}
		Jobs.Reset();
	}

	if (Pool)
	{
		Pool->Destroy();
		delete Pool;
		Pool = nullptr;
	}

	bInitialised.store(false, std::memory_order_release);
	UE_LOG(LogMCP, Log, TEXT("FMCPJobRegistry shutdown — cancelled %d in-flight jobs"), Snapshot.Num());
}

FGuid FMCPJobRegistry::SubmitJob(const FString& Description, FBody Body, bool bGameThreadRequired)
{
	check(Body); // empty TFunction is a programmer error — fail fast, don't queue a zombie
	EnsureInitialized();

	if (bShutdown.load(std::memory_order_acquire))
	{
		UE_LOG(LogMCP, Warning, TEXT("FMCPJobRegistry::SubmitJob during shutdown — refusing (desc='%s')"), *Description);
		return FGuid();
	}

	TSharedRef<FMCPJob> Job = MakeShared<FMCPJob>();
	Job->Id = FGuid::NewGuid();
	Job->Description = Description;
	Job->SubmittedAt = FPlatformTime::Seconds();
	Job->bGameThreadRequired = bGameThreadRequired;
	// State defaults to Pending in the struct ctor.

	{
		FScopeLock Lock(&JobsLock);
		Jobs.Add(Job->Id, Job);
	}

	// IQueuedWork is heap-allocated — wrapper self-deletes after DoThreadedWork.
	FMCPJobWork* Work = new FMCPJobWork(Job, MoveTemp(Body));
	if (Pool)
	{
		Pool->AddQueuedWork(Work);
	}
	else
	{
		// Pool creation failed — short-circuit to Failed terminal so the client gets a deterministic
		// error from job.status. Have to still call Abandon to avoid leaking the IQueuedWork.
		Work->Abandon();
		FScopeLock Lock(&JobsLock);
		Job->State.store(EMCPJobState::Failed, std::memory_order_release);
		Job->ErrorMessage = TEXT("worker pool unavailable");
		Job->FinishedAt = FPlatformTime::Seconds();
	}

	UE_LOG(LogMCP, Verbose, TEXT("FMCPJobRegistry::SubmitJob id=%s desc='%s' gt_required=%d"),
		*Job->Id.ToString(EGuidFormats::DigitsWithHyphens), *Description, bGameThreadRequired ? 1 : 0);
	return Job->Id;
}

void FMCPJobRegistry::ExecuteJobOnWorker(TSharedRef<FMCPJob> Job, FBody Body)
{
	// Honour cancel-before-start.
	if (Job->bCancelRequested.load(std::memory_order_acquire))
	{
		Job->State.store(EMCPJobState::Cancelled, std::memory_order_release);
		Job->FinishedAt = FPlatformTime::Seconds();
		UE_LOG(LogMCP, Verbose, TEXT("FMCPJobRegistry: job %s cancelled before start"), *Job->Id.ToString(EGuidFormats::DigitsWithHyphens));
		return;
	}

	Job->State.store(EMCPJobState::Running, std::memory_order_release);
	Job->StartedAt = FPlatformTime::Seconds();

	TSharedPtr<FJsonValue> Result;

	if (Job->bGameThreadRequired)
	{
		// Game-thread coordinator: dispatch the body to GT via FTSTicker and block this worker on
		// the future. Worker slot is monopolised for the body's runtime.
		//
		// **2026-05 fix:** switched from AsyncTask(ENamedThreads::GameThread, ...) to FTSTicker.
		// AsyncTask runs the body INSIDE a TaskGraph task processing context — any nested
		// TaskGraph dispatch from the body (e.g. AssetTools.ImportAssetTasks → Interchange spawns
		// internal TaskGraph work) tripped TaskGraph's RecursionGuard at Async/TaskGraph.cpp:689
		// and crashed the editor. FTSTicker callbacks fire from FEngineLoop::Tick OUTSIDE the
		// TaskGraph task processing loop, so nested TaskGraph dispatches are fresh entries.
		//
		// TPromise/TFuture state is shared via internal TSharedRef so moving the promise into the
		// lambda doesn't invalidate the future we already retrieved.
		TPromise<TSharedPtr<FJsonValue>> Promise;
		TFuture<TSharedPtr<FJsonValue>> Future = Promise.GetFuture();

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[PromiseShared = MakeShared<TPromise<TSharedPtr<FJsonValue>>>(MoveTemp(Promise)),
			 JobCap = Job, BodyCap = MoveTemp(Body)]
			(float /*DeltaTime*/) mutable -> bool
			{
				TSharedPtr<FJsonValue> GTResult = BodyCap(JobCap.Get());
				PromiseShared->SetValue(MoveTemp(GTResult));
				return false;  // one-shot
			}),
			/*InDelay*/ 0.0f);

		// Blocks the worker. Body runs on the game thread via next-tick FTSTicker fire.
		Result = Future.Get();
	}
	else
	{
		Result = Body(Job.Get());
	}

	Job->FinishedAt = FPlatformTime::Seconds();

	// Decide terminal state from (Result, ErrorMessage, bCancelRequested) per the documented XOR
	// contract in FMCPJob.
	if (Result.IsValid())
	{
		Job->Result = MoveTemp(Result);
		Job->State.store(EMCPJobState::Succeeded, std::memory_order_release);
	}
	else if (Job->bCancelRequested.load(std::memory_order_acquire) && Job->ErrorMessage.IsEmpty())
	{
		Job->State.store(EMCPJobState::Cancelled, std::memory_order_release);
	}
	else
	{
		if (Job->ErrorMessage.IsEmpty())
		{
			Job->ErrorMessage = TEXT("job body returned null without setting ErrorMessage");
		}
		Job->State.store(EMCPJobState::Failed, std::memory_order_release);
	}

	UE_LOG(LogMCP, Verbose, TEXT("FMCPJobRegistry: job %s -> %s (%.3fs)"),
		*Job->Id.ToString(EGuidFormats::DigitsWithHyphens),
		LexJobState(Job->State.load(std::memory_order_acquire)),
		Job->FinishedAt - Job->StartedAt);
}

EMCPJobState FMCPJobRegistry::GetState(const FGuid& Id) const
{
	FScopeLock Lock(&JobsLock);
	if (const TSharedRef<FMCPJob>* Found = Jobs.Find(Id))
	{
		return (*Found)->State.load(std::memory_order_acquire);
	}
	return EMCPJobState::Pending; // "unknown" is indistinguishable from "not started yet" by design
}

TSharedPtr<FJsonValue> FMCPJobRegistry::GetResult(const FGuid& Id) const
{
	FScopeLock Lock(&JobsLock);
	if (const TSharedRef<FMCPJob>* Found = Jobs.Find(Id))
	{
		const TSharedRef<FMCPJob>& Job = *Found;
		if (Job->State.load(std::memory_order_acquire) == EMCPJobState::Succeeded)
		{
			return Job->Result; // shared ptr copy — safe to hand out beyond lock scope
		}
	}
	return nullptr;
}

bool FMCPJobRegistry::GetStatus(const FGuid& Id, FStatusSnapshot& Out) const
{
	FScopeLock Lock(&JobsLock);
	const TSharedRef<FMCPJob>* Found = Jobs.Find(Id);
	if (!Found)
	{
		return false;
	}
	const TSharedRef<FMCPJob>& Job = *Found;
	Out.Id = Job->Id;
	Out.Description = Job->Description;
	Out.State = Job->State.load(std::memory_order_acquire);
	Out.Progress = Job->Progress.load(std::memory_order_acquire);
	Out.bCancelRequested = Job->bCancelRequested.load(std::memory_order_acquire);
	Out.bGameThreadRequired = Job->bGameThreadRequired;
	Out.ErrorMessage = Job->ErrorMessage;
	Out.SubmittedAt = Job->SubmittedAt;
	Out.StartedAt = Job->StartedAt;
	Out.FinishedAt = Job->FinishedAt;
	return true;
}

bool FMCPJobRegistry::RequestCancel(const FGuid& Id)
{
	FScopeLock Lock(&JobsLock);
	const TSharedRef<FMCPJob>* Found = Jobs.Find(Id);
	if (!Found)
	{
		return false;
	}
	const TSharedRef<FMCPJob>& Job = *Found;
	const EMCPJobState Cur = Job->State.load(std::memory_order_acquire);
	if (Cur == EMCPJobState::Succeeded || Cur == EMCPJobState::Failed || Cur == EMCPJobState::Cancelled)
	{
		return false; // already terminal — cancel is a no-op
	}
	Job->bCancelRequested.store(true, std::memory_order_release);
	return true;
}

TArray<FMCPJobRegistry::FStatusSnapshot> FMCPJobRegistry::GetActive() const
{
	TArray<FStatusSnapshot> Out;
	FScopeLock Lock(&JobsLock);
	Out.Reserve(Jobs.Num());
	for (const TPair<FGuid, TSharedRef<FMCPJob>>& Pair : Jobs)
	{
		const TSharedRef<FMCPJob>& Job = Pair.Value;
		const EMCPJobState S = Job->State.load(std::memory_order_acquire);
		if (S == EMCPJobState::Succeeded || S == EMCPJobState::Failed || S == EMCPJobState::Cancelled)
		{
			continue; // active = pending or running
		}
		FStatusSnapshot Snap;
		Snap.Id = Job->Id;
		Snap.Description = Job->Description;
		Snap.State = S;
		Snap.Progress = Job->Progress.load(std::memory_order_acquire);
		Snap.bCancelRequested = Job->bCancelRequested.load(std::memory_order_acquire);
		Snap.bGameThreadRequired = Job->bGameThreadRequired;
		Snap.ErrorMessage = Job->ErrorMessage;
		Snap.SubmittedAt = Job->SubmittedAt;
		Snap.StartedAt = Job->StartedAt;
		Snap.FinishedAt = Job->FinishedAt;
		Out.Add(MoveTemp(Snap));
	}
	return Out;
}

int32 FMCPJobRegistry::GetTrackedJobCount() const
{
	FScopeLock Lock(&JobsLock);
	return Jobs.Num();
}

bool FMCPJobRegistry::TickTtlCleanup(float DeltaTime)
{
	const double Now = FPlatformTime::Seconds();
	int32 Reaped = 0;

	{
		FScopeLock Lock(&JobsLock);
		for (auto It = Jobs.CreateIterator(); It; ++It)
		{
			const TSharedRef<FMCPJob>& Job = It.Value();
			const EMCPJobState S = Job->State.load(std::memory_order_acquire);
			const bool bTerminal =
				S == EMCPJobState::Succeeded || S == EMCPJobState::Failed || S == EMCPJobState::Cancelled;
			if (!bTerminal)
			{
				continue;
			}
			if (Job->FinishedAt > 0.0 && (Now - Job->FinishedAt) > kTtlSeconds)
			{
				It.RemoveCurrent();
				++Reaped;
			}
		}
	}

	if (Reaped > 0)
	{
		UE_LOG(LogMCP, Log, TEXT("FMCPJobRegistry TTL sweep: reaped %d terminal jobs (>%.0fs old)"), Reaped, kTtlSeconds);
	}
	return true; // reschedule
}
