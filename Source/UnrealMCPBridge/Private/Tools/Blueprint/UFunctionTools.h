// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Generic UFUNCTION reflection surface — call ANY BlueprintCallable / BlueprintPure / Exec
 * UFUNCTION on a UClass (static call) or UObject (instance call) through MCP.
 *
 * Primary use cases:
 *   - Trigger BP function libraries (UFlecsContainerLibrary::AddItemToContainer etc.).
 *   - Drive crafting / inventory / ability systems that expose BlueprintCallable entry points.
 *   - Call Exec / debug functions on actors during PIE.
 *
 * Both tools are Lane A (game thread only — UFunction invocation requires GT and the property
 * marshalling layer (FMCPReflection) is GT-only). NO PIE guard — callers may legitimately need
 * to drive functions during PIE (e.g. to add items to a player's inventory mid-playtest);
 * caller is responsible for picking PIE-safe vs editor-safe targets.
 *
 * Function safety gate: by default only FUNC_BlueprintCallable | FUNC_BlueprintPure | FUNC_Exec
 * functions are callable. Caller can pass ``args.allow_any=true`` to bypass — useful for
 * private editor-only helpers or AutomationTest UFUNCTIONs.
 */
namespace FUFunctionTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	UNREALMCPBRIDGE_API FMCPResponse Tool_CallFunction(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_ListClassFunctions(const FMCPRequest& Request);
}
