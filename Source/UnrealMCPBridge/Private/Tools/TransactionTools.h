// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FMCPDispatchQueue;

/**
 * Transaction (Undo/Redo) introspection surface — gives AI agents awareness of editor mutation
 * state since the last MCP call. Read-only; mutators (transaction.undo / transaction.redo) are
 * deliberately NOT exposed in this initial drop — they cascade across actors/assets in ways that
 * confuse AI agents about what they "see".
 *
 * Tools (2):
 *   transaction.list(limit?)      Recent transactions with descriptions + size + flags
 *   transaction.get_state()        undo_count / redo_count / queue_length / can_undo / can_redo
 *
 * Error codes:
 *   -32602 InvalidParams          Bad limit value (must be 1..1000, default 100)
 *   -32603 InternalError          GEditor->Trans null (no transaction buffer available — unusual)
 */
namespace FTransactionTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);
}
