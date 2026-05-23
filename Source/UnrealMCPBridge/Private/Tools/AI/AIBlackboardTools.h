// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Wave J Surface 2 — Blackboard runtime read/write surface (ai.bb.*). 3 user-visible tools, all
 * Lane A.
 *
 * Tool roster:
 *   ai.bb.list_keys   Enumerate every key on the actor's AIController-attached
 *                     UBlackboardComponent. Returns { keys: [{ name, type, value_repr }] }.
 *                     ``type`` is the friendly name with the ``BlackboardKeyType_`` prefix
 *                     stripped (Bool / Int / Float / String / Vector / Object / Class / Enum /
 *                     Name / Rotator). ``value_repr`` is the engine's own DescribeValue() text
 *                     (best-effort one-line summary), useful for telemetry/dump. Read-only,
 *                     no PIE guard. Pawn or AIController paths are both accepted.
 *   ai.bb.get_value   Typed read of a single key. Returns { type, value } where ``value`` is
 *                     a JSON value matching the engine type as closely as possible:
 *                       Bool->bool, Int->int, Float->number, String->string, Name->string,
 *                       Vector->[x,y,z], Rotator->[pitch,yaw,roll], Object->object_path,
 *                       Class->class_path, Enum->int. Unknown key types degrade to the
 *                       DescribeValue() string. Read-only, no PIE guard.
 *   ai.bb.set_value   Typed write of a single key. PIE-SAFE — this IS a runtime tool and
 *                     blackboard state is exactly what callers want to mutate at runtime.
 *                     Value is validated against the key type before being applied; mismatch
 *                     surfaces -32602 with a descriptive message. Returns
 *                     { set, prior_value, type }. The ``prior_value`` is captured by routing
 *                     through the same typed-getter path as ai.bb.get_value before the write
 *                     happens.
 *
 * **No PIE guard on any tool.** Blackboards are runtime state — there is no editor-time
 * "blackboard data" being modified. The whole point of the set_value tool is to script AI
 * state during PIE/runtime for testing. Reads work in editor too (e.g. a default
 * BehaviorTreeComponent on a placed pawn before play).
 *
 * **No FScopedTransaction.** Blackboard mutations are not asset edits — they live on a
 * runtime component, not on a UObjectAsset. Wrapping in a transaction would do nothing useful
 * (the change cannot be undone via the editor's undo stack regardless).
 *
 * **AIController resolution.** Accepts EITHER an APawn path (auto-resolves via
 * Pawn->GetController()) OR a direct AAIController path. This matches the natural caller
 * mental model: "I have my BP_Enemy actor, get its blackboard" should not require knowing
 * whether the controller is a separate actor instance in the world.
 *
 * **Type mapping (UBlackboardKeyType subclass -> wire type string).** Recognized subclasses
 * report friendly names with the ``BlackboardKeyType_`` prefix stripped — e.g.
 * ``BlackboardKeyType_Bool`` -> ``"Bool"``. Subclasses outside the recognized set
 * (BlackboardKeyType_Struct / SOClaimHandle / NativeEnum / future entries) fall through to
 * a stringified class-name with the prefix retained ("BlackboardKeyType_Foo"), exposing the
 * raw type so callers can branch on it. get/set on unknown types degrades gracefully to the
 * descriptive string path (set is rejected with -32602 since we can't parse the value).
 *
 * **Error codes (all reused — no new codes):**
 *   -32004 ObjectNotFound  actor / AIController / BlackboardComponent missing
 *   -32005 PropertyNotFound  key name not present on the blackboard asset
 *   -32010 InvalidPath  malformed actor_path (delegates to FMCPActorPathUtils::ParseActorPath)
 *   -32602 InvalidParams  missing required field; or set_value value type mismatched with key
 *   -32603 InternalError  AIController has no BlackboardAsset; unknown subclass on set_value
 */
namespace FAIBlackboardTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	// Wave J Surface 2: ai.bb.* tools (runtime read/write).
	UNREALMCPBRIDGE_API FMCPResponse Tool_ListKeys(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_GetValue(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_SetValue(const FMCPRequest& Request);

	// Wave P: ai.bb.* authoring (UBlackboardData asset + Keys[] CRUD).
	// create_asset    NewObject<UBlackboardData> + optional Parent BB link
	// add_key         FBlackboardEntry + NewObject<UBlackboardKeyType_X>; short-name resolution
	//                 Bool/Int/Float/String/Name/Vector/Rotator/Class/Object/Enum.
	//                 Class/Object use key_options.base_class; Enum uses key_options.enum_path.
	// remove_key      Match by EntryName; idempotent (returns removed=false if absent).
	// (set_key_default DROPPED for v1 — BB defaults use per-type binary serialization that doesn't
	// round-trip cleanly; callers use runtime ai.bb.set_value instead.)
	UNREALMCPBRIDGE_API FMCPResponse Tool_CreateAsset(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_AddKey(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_RemoveKey(const FMCPRequest& Request);
}
