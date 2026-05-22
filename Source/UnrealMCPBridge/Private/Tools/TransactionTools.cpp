// Copyright FatumGame. All Rights Reserved.

#include "TransactionTools.h"

#include "MCPSurfaceRegistry.h"

#include "FMCPDispatchQueue.h"
#include "MCPJsonBuilder.h"
#include "MCPToolHelpers.h"

#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "Editor/Transactor.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// TXN_ prefix per the unity-build symbol-collision convention.
	constexpr int32 kTXNErrorInternal = -32603;

	// Resolve the editor's transaction buffer. Returns nullptr if GEditor is null or has no
	// transactor (unusual — should always be set in editor builds; just a safety check for
	// commandlet / cooker contexts that load the plugin DLL).
	const UTransactor* TXN_GetTransactor()
	{
		if (!GEditor)
		{
			return nullptr;
		}
		return GEditor->Trans;
	}

	// Build a per-transaction JSON object. Returns null TSharedPtr if Transaction is null.
	TSharedRef<FJsonObject> TXN_BuildTransactionJson(const FTransaction* Transaction, int32 QueueIndex)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("queue_index"), QueueIndex);
		if (!Transaction)
		{
			Obj->SetStringField(TEXT("description"), TEXT(""));
			Obj->SetStringField(TEXT("type"), TEXT(""));
			Obj->SetBoolField(TEXT("contains_pie_objects"), false);
			Obj->SetBoolField(TEXT("is_transient"), false);
			return Obj;
		}

		// Description is a per-locale FText (Cancel / Move / Edit / etc.) — emit the source string
		// since AI consumers want the canonical English token, not the localized variant.
		const FText DescText = Transaction->GetDescription();
		Obj->SetStringField(TEXT("description"), DescText.ToString());

		// Type tag — distinguishes "Normal" from "TransObjectAnnotation" etc. when the transaction
		// holds an FTransactionObjectAnnotation rather than a standard FTransaction.
		const TCHAR* TypeTag = Transaction->GetTransactionType();
		Obj->SetStringField(TEXT("type"), TypeTag ? FString(TypeTag) : FString());

		Obj->SetBoolField(TEXT("contains_pie_objects"), Transaction->ContainsPieObjects());
		Obj->SetBoolField(TEXT("is_transient"), Transaction->IsTransient());
		return Obj;
	}
} // namespace

namespace FTransactionTools
{

// --- transaction.list -------------------------------------------------------------------------
//
// Args:    { limit?: int (default 100, clamp [1, 1000]) }
// Result:  { transactions: [{ queue_index, description, type, contains_pie_objects, is_transient }],
//            queue_length: int }
//
// Read-only — no PIE guard. Reads the UTransactor queue from newest (queue_length-1) to oldest,
// up to `limit` entries. The newest entry is the most-recently-completed transaction (e.g. the
// last thing the user/AI just did).
//
// transactions[0] is the NEWEST; entries are ordered most-recent-first for AI consumption.
//
// NOTE: this is the EDITOR transaction queue. PIE has its own independent UTransactor instance
// at GEditor->Trans during PIE — both editor + PIE transactions appear in the same queue when
// PIE is active. The `contains_pie_objects` flag lets the caller filter PIE-only entries out.
FMCPResponse Tool_List(const FMCPRequest& Request)
{
	check(IsInGameThread());

	const UTransactor* Trans = TXN_GetTransactor();
	if (!Trans)
	{
		return FMCPToolHelpers::MakeError(Request, kTXNErrorInternal,
			TEXT("GEditor->Trans is null (editor transaction buffer unavailable)"));
	}

	int32 Limit = 100;
	if (Request.Args.IsValid())
	{
		Request.Args->TryGetNumberField(TEXT("limit"), Limit);
	}
	Limit = FMath::Clamp(Limit, 1, 1000);

	const int32 QueueLength = Trans->GetQueueLength();
	const int32 ToEmit = FMath::Min(Limit, QueueLength);

	FMCPJsonArrayBuilder Items;
	for (int32 Offset = 0; Offset < ToEmit; ++Offset)
	{
		const int32 QueueIndex = QueueLength - 1 - Offset; // newest first
		const FTransaction* Tx = Trans->GetTransaction(QueueIndex);
		Items.AddValue(MakeShared<FJsonValueObject>(TXN_BuildTransactionJson(Tx, QueueIndex)));
	}

	return FMCPJsonBuilder()
		.Arr(TEXT("transactions"), Items.ToValueArray())
		.Int(TEXT("queue_length"), QueueLength)
		.BuildSuccess(Request);
}

// --- transaction.get_state --------------------------------------------------------------------
//
// Args:    {}
// Result:  { queue_length: int, undo_count: int, redo_count: int,
//            can_undo: bool, can_redo: bool,
//            undo_title?: string, redo_title?: string }
//
// Read-only — no PIE guard. Returns the editor's Undo/Redo position.
//
// `undo_count` is how many transactions can be undone (== current position from queue start);
// `redo_count` is how many can be redone (transactions that were undone but not yet pruned).
// `queue_length` == undo_count + redo_count.
//
// `undo_title` / `redo_title` are the FText labels Unreal would show in the Edit menu (e.g.
// "Undo Move Actor", "Redo Edit Property"). Omitted when can_undo / can_redo are false.
FMCPResponse Tool_GetState(const FMCPRequest& Request)
{
	check(IsInGameThread());

	UTransactor* Trans = GEditor ? GEditor->Trans : nullptr;
	if (!Trans)
	{
		return FMCPToolHelpers::MakeError(Request, kTXNErrorInternal,
			TEXT("GEditor->Trans is null (editor transaction buffer unavailable)"));
	}

	const int32 QueueLength = Trans->GetQueueLength();
	const int32 UndoCount   = Trans->GetUndoCount();
	const int32 RedoCount   = QueueLength - UndoCount;

	FText UndoText, RedoText;
	const bool bCanUndo = Trans->CanUndo(&UndoText);
	const bool bCanRedo = Trans->CanRedo(&RedoText);

	return FMCPJsonBuilder()
		.Int(TEXT("queue_length"), QueueLength)
		.Int(TEXT("undo_count"),   UndoCount)
		.Int(TEXT("redo_count"),   RedoCount)
		.Bool(TEXT("can_undo"),    bCanUndo)
		.Bool(TEXT("can_redo"),    bCanRedo)
		.If(bCanUndo, [&](FMCPJsonBuilder& B) { B.Str(TEXT("undo_title"), UndoText.ToString()); })
		.If(bCanRedo, [&](FMCPJsonBuilder& B) { B.Str(TEXT("redo_title"), RedoText.ToString()); })
		.BuildSuccess(Request);
}

void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	// Both Lane A — UTransactor::CanUndo/CanRedo + GetTransaction touch UObject globals
	// (transaction records contain UObject pointers). FText::ToString is allocator-thread-safe
	// but FTransaction internal state is not.
	RegisterTool(TEXT("transaction.list"),       &Tool_List,     /*Lane A*/ false);
	RegisterTool(TEXT("transaction.get_state"),  &Tool_GetState, /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("Transaction surface registered: transaction.list + transaction.get_state (Lane A)"));
}

} // namespace FTransactionTools

MCP_REGISTER_SURFACE(TransactionTools, &FTransactionTools::Register)

#undef LOCTEXT_NAMESPACE
