// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UBlueprint;
class UClass;
class UEdGraph;
struct FBPVariableDescription;

/**
 * Phase 4 — Blueprint asset access helpers.
 *
 * Centralises the path → ``UBlueprint*`` lookup that every bp.* tool performs first. Each
 * resolver normalises path shape (``/Game/Foo/BP_X`` ↔ ``/Game/Foo/BP_X.BP_X``), distinguishes
 * "not found" from "found but wrong class", and surfaces the appropriate Phase 4 error code:
 *
 *   - ``kMCPErrorObjectNotFound`` (-32004)        — LoadObject returned null after retry
 *   - ``kMCPErrorBlueprintTypeMismatch`` (-32031) — path resolved to a non-UBlueprint asset
 *
 * **Threading.** All helpers MUST run on the game thread — ``LoadObject<UBlueprint>`` triggers
 * Blueprint class deferred-compile + CDO touches under the GC lock. Phase 4 ships every tool
 * Lane A, so this is the natural execution context.
 *
 * **No PIE guard here.** Reads are PIE-safe; the writes that need the guard call
 * ``FMCPWorldContext::IsPIEActive`` themselves before resolving the BP.
 */
namespace FMCPBlueprintUtils
{
	/**
	 * Resolve ``Path`` to a ``UBlueprint*``. Accepts both package-name (``/Game/Foo/BP_X``) and
	 * object-path (``/Game/Foo/BP_X.BP_X``) forms — the loader retries with the ``.LeafName``
	 * suffix appended if the first attempt fails.
	 *
	 * Returns the loaded blueprint on success. On failure populates ``OutErrorCode`` + ``OutError``:
	 *   - Empty/invalid path  → -32010 InvalidPath ("path must start with /Game, /Engine, etc.")
	 *   - LoadObject failure  → -32004 ObjectNotFound
	 *   - Loaded but wrong class → -32031 BlueprintTypeMismatch (with the actual class name)
	 *
	 * Caller maps ``OutErrorCode`` to a response via its tool-private ``BP_MakeError`` helper.
	 */
	UNREALMCPBRIDGE_API UBlueprint* LoadBlueprintByPath(
		const FString& Path,
		int32& OutErrorCode,
		FString& OutError);

	/**
	 * Convenience: return ``Blueprint->GeneratedClass`` as a ``UClass*``. May return null if the
	 * blueprint has never been compiled (rare — Epic regenerates on load). Caller decides whether
	 * a null generated class is a hard error or surfaces as ``generated_class_path: null``.
	 */
	UNREALMCPBRIDGE_API UClass* GetGeneratedClass(const UBlueprint* Blueprint);

	/**
	 * Re-export of ``FBlueprintEditorUtils::IsDataOnlyBlueprint`` as a non-throwing free function
	 * so tool bodies stay free of the heavy Kismet2 include. Returns false when Blueprint is null.
	 */
	UNREALMCPBRIDGE_API bool IsDataOnlyBlueprint(const UBlueprint* Blueprint);

	/**
	 * Look up a variable by name in ``Blueprint->NewVariables``. Returns the array index on success,
	 * INDEX_NONE if not found. Case-sensitive FName comparison.
	 */
	UNREALMCPBRIDGE_API int32 FindVariableIndex(const UBlueprint* Blueprint, const FName VarName);

	/**
	 * Look up a function graph by name in ``Blueprint->FunctionGraphs``. Returns the array index
	 * on success, INDEX_NONE if not found. Case-sensitive comparison on ``UEdGraph::GetFName()``.
	 *
	 * Skips graphs that are constructor-script / ubergraph (those are accessible via the dedicated
	 * Blueprint members, not the user-function list).
	 */
	UNREALMCPBRIDGE_API int32 FindFunctionGraphIndex(const UBlueprint* Blueprint, const FName FunctionName);

	/**
	 * Get the function graph by name, returns nullptr if not found.
	 */
	UNREALMCPBRIDGE_API UEdGraph* FindFunctionGraph(const UBlueprint* Blueprint, const FName FunctionName);
}
