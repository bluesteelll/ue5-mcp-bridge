// Copyright FatumGame. All Rights Reserved.

#include "MCPMutatorScope.h"

#include "Utils/MCPWorldContext.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

FMCPMutatorScope::FMCPMutatorScope(const FMCPRequest& InRequest, FText InTransactionLabel)
	: RequestRef(InRequest)
	, TransactionLabel(MoveTemp(InTransactionLabel))
{
	if (FMCPWorldContext::IsPIEActive())
	{
		bPIEBlocked = true;
		return;
	}
	// Heap-alloc so we don't need TOptional + non-default-constructible payload juggling.
	Transaction = new FScopedTransaction(TransactionLabel);
}

FMCPMutatorScope::~FMCPMutatorScope()
{
	if (Transaction)
	{
		if (bAborted)
		{
			Transaction->Cancel();
		}
		delete Transaction;
		Transaction = nullptr;
	}
	if (!bAborted)
	{
		for (UPackage* Pkg : PendingDirty)
		{
			if (Pkg)
			{
				Pkg->MarkPackageDirty();
			}
		}
	}
	PendingDirty.Reset();
}

FMCPResponse FMCPMutatorScope::Error() const
{
	return FMCPToolHelpers::MakeError(RequestRef, kMCPErrorPIEActive, kMCPMessagePIEActive);
}

void FMCPMutatorScope::DirtyPackage(UPackage* Pkg)
{
	if (!Pkg)
	{
		return;
	}
	PendingDirty.AddUnique(Pkg);
}

void FMCPMutatorScope::Abort()
{
	bAborted = true;
}

#undef LOCTEXT_NAMESPACE
