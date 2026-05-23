// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Wave E Surface 6 — Generic UE subsystem reflection surface. 3 user-visible tools, all Lane A.
 *
 * Tool roster:
 *   subsystem.list           → enumerate instantiated subsystems across all 5 collections
 *                               (Engine, Editor, World, GameInstance, LocalPlayer). Reports
 *                               { class_path, kind, owner_context } per entry. Optional
 *                               ``kind`` arg filters to one collection.
 *   subsystem.get_property   → resolve a subsystem instance by class path, read a top-level
 *                               UPROPERTY via FProperty reflection. Returns
 *                               { class_path, property_name, type, value }.
 *   subsystem.call_function  → resolve a subsystem instance, find a UFUNCTION by name, marshal
 *                               args, invoke. Returns { class_path, function_name, return_value,
 *                               out_params, is_state_changing, function_signature }.
 *
 * **All 3 tools are Lane A** (``bThreadSafe=false``). GEngine / GEditor / UWorld /
 * UGameInstance / ULocalPlayer subsystem-collection access requires the game thread. FProperty
 * reflection (FMCPReflection helpers) is GT-only.
 *
 * **PIE guard policy (per surface brief).**
 *   - ``subsystem.list``         — no guard; pure read.
 *   - ``subsystem.get_property`` — no guard; pure read.
 *   - ``subsystem.call_function``— no guard; the caller is responsible. We surface an
 *                                   ``is_state_changing`` boolean in the response (derived from
 *                                   FUNC_Const) so the caller can post-hoc inspect whether the
 *                                   invocation mutated state, but we don't block the call.
 *
 * **Function safety gate.** ``subsystem.call_function`` reuses the same BlueprintCallable /
 * BlueprintPure / Exec gate as bp.call_function — caller passes ``args.allow_any=true`` to
 * bypass for editor-only or private UFUNCTIONs.
 *
 * **Subsystem kind enum** (``args.kind``):
 *   - ``"engine"``        → ``GEngine->GetEngineSubsystemArrayCopy<UEngineSubsystem>()``
 *   - ``"editor"``        → ``GEditor->GetEditorSubsystemArrayCopy<UEditorSubsystem>()``
 *   - ``"world"``         → editor world's ``GetSubsystemArrayCopy<UWorldSubsystem>()``
 *                            (PIE world preferred when active, mirrors PhysicsTools convention)
 *   - ``"game_instance"`` → world's UGameInstance ``GetSubsystemArrayCopy<UGameInstanceSubsystem>()``
 *   - ``"local_player"``  → first PC's ULocalPlayer ``GetSubsystemArrayCopy<ULocalPlayerSubsystem>()``
 *   - ``"all"`` (default) → union of all 5
 *
 * **Error codes (reuses existing — no new codes introduced):**
 *   -32004 ObjectNotFound        subsystem not instantiated for the requested class
 *   -32005 PropertyNotFound      UFUNCTION / UPROPERTY not found on the subsystem class
 *   -32006 PropertyTypeMismatch  arg type couldn't be marshalled to UFUNCTION param type
 *   -32007 PropertyAccessDenied  function isn't BlueprintCallable/Pure/Exec and allow_any not set
 *   -32011 WrongClass            class_path didn't resolve to a USubsystem subclass
 *   -32020 ClassNotFound         class_path didn't resolve as a UClass at all
 *   -32602 InvalidParams         malformed args (bad ``kind`` enum, missing required field)
 */
namespace FSubsystemTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	// ─── Wave E Surface 6: Generic subsystem reflection ────────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_List(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_GetProperty(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_CallFunction(const FMCPRequest& Request);
}
