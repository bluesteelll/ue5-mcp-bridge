// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Internationalization/Text.h"
#include "MCPTypes.h"
#include "MCPToolHelpers.h"

class UPackage;
class FScopedTransaction;

/**
 * RAII guard for the 134 editor-world mutator sites across 50+ tool surfaces. Bundles three
 * boilerplate steps:
 *
 *   1. **PIE-guard**: rejects the call with -32027 PIEActive + frozen wire message if
 *      `GEditor->PlayWorld` is non-null. Caller checks `PIEBlocked()` once and returns `Error()`.
 *   2. **FScopedTransaction**: opens an undoable transaction with the supplied label; closes
 *      on destruction (commits) or on `Abort()` (cancels).
 *   3. **MarkPackageDirty queue**: `DirtyPackage(Pkg)` queues a package; destructor flushes
 *      all queued packages to MarkPackageDirty in one pass (avoids double-dirty within a single tool).
 *
 * Usage:
 *     FMCPMutatorScope Scope(Request, LOCTEXT("MCP_DT_SetRow", "DataTable: Set Row"));
 *     if (Scope.PIEBlocked()) return Scope.Error();
 *     // ... do work ...
 *     Table->Modify();
 *     Scope.DirtyPackage(Table->GetPackage());
 *     // Destructor: commits transaction, marks Table->GetPackage() dirty.
 *
 * On error path you can `Scope.Abort()` to cancel the transaction (won't mark dirty).
 *
 * **Thread-safety:** NOT Lane B safe — touches GEditor + FScopedTransaction. Same constraint
 * as the existing inline PIE-check / transaction pattern.
 */
class UNREALMCPBRIDGE_API FMCPMutatorScope
{
public:
	/**
	 * Construct the scope. Captures `Request` by reference (must outlive the scope). Performs the
	 * PIE-active check immediately; subsequent `PIEBlocked()` calls return the cached result.
	 * Opens an FScopedTransaction with `TransactionLabel` ONLY if not PIE-blocked.
	 */
	FMCPMutatorScope(const FMCPRequest& InRequest, FText InTransactionLabel);

	/** Destructor: commits transaction (unless Abort'd) and flushes pending dirty packages. */
	~FMCPMutatorScope();

	/** Non-copyable / non-movable — RAII semantics. */
	FMCPMutatorScope(const FMCPMutatorScope&) = delete;
	FMCPMutatorScope& operator=(const FMCPMutatorScope&) = delete;
	FMCPMutatorScope(FMCPMutatorScope&&) = delete;
	FMCPMutatorScope& operator=(FMCPMutatorScope&&) = delete;

	/** True iff PIE was active at construction time (caller must early-return with `Error()`). */
	bool PIEBlocked() const { return bPIEBlocked; }

	/** Pre-built -32027 error response. Only meaningful when `PIEBlocked()` is true. */
	FMCPResponse Error() const;

	/** Queue a package to be marked dirty on destructor. Skips null and duplicate pointers. */
	void DirtyPackage(UPackage* Pkg);

	/** Cancel the pending transaction — destructor won't commit (and won't dirty queued pkgs). */
	void Abort();

private:
	const FMCPRequest& RequestRef;
	FText TransactionLabel;
	bool bPIEBlocked = false;
	bool bAborted = false;
	TArray<UPackage*> PendingDirty;

	/**
	 * Heap-allocated to allow conditional construction (skip transaction if PIE-blocked) without
	 * the optional-value-init dance. Cleaned up in destructor. We could use TUniquePtr but
	 * raw-with-delete keeps the include surface smaller in the public header.
	 */
	FScopedTransaction* Transaction = nullptr;
};
