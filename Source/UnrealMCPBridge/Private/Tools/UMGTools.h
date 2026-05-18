// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Phase 5 — Chunk C, Category C (UMG read-only). 2 user-visible tools, all Lane A.
 *
 * Tool roster (per Phase 5 plan §C-UMG lines 678-758):
 *   umg.list_widgets         → enumerate child widgets in a UserWidget Blueprint's WidgetTree
 *   umg.get_widget_property  → read a property from a named widget in a WBP (CDO-scoped read)
 *
 * **All 2 tools are Lane A** (``bThreadSafe=false``). Reasons:
 *   - ``LoadObject<UWidgetBlueprint>`` touches the package loader + GC visited set; GT-only.
 *   - ``UWidgetTree::ForEachWidgetAndDescendants`` walks UObject* graphs — GT-safe by convention,
 *     not guaranteed thread-safe (named-slot interface dispatch can touch shared state).
 *   - ``FMCPReflection::ReadPropertyValueAt`` performs UStruct introspection and may auto-load
 *     soft references during text-export of unsupported types; GT-only.
 *
 * **Read-only — no PIE guard.** Widget blueprints are shared between editor and PIE worlds; reads
 * are safe in both contexts. Property reads are CDO-scoped (we read the WidgetTree as designed
 * in the editor, NOT runtime instance state) so PIE has no effect on the result.
 *
 * **Parent resolution (umg.list_widgets).** For each widget in the tree we report ``parent_name``
 * via ``UWidgetTree::FindWidgetParent``. The RootWidget reports ``parent_name=null``. The
 * ``is_variable`` flag mirrors ``UWidget::bIsVariable`` (the "Is Variable" editor checkbox).
 *
 * **WidgetTree resolution.** ``UWidgetBlueprint::WidgetTree`` lives on the editor-only
 * ``UBaseWidgetBlueprint`` base (``WITH_EDITORONLY_DATA``). Both tools refuse to run in cooked
 * builds — they're editor-only by design. The Build.cs adds ``UMG`` (runtime widget classes) +
 * ``UMGEditor`` (UWidgetBlueprint asset class).
 *
 * **Property-path delegation (umg.get_widget_property).** After resolving the widget by name in
 * the WidgetTree we delegate to ``FMCPReflection::ResolvePropertyPath`` + ``ReadPropertyValueAt``
 * — the same Tier-1 marshalling pipeline used by ``marshall.read_property`` and
 * ``actor.get_property``. Property-path errors are surfaced verbatim with their canonical Phase 3
 * error codes (-32005 PropertyNotFound, -32006 PropertyTypeMismatch, -32025 PropertyPathTooDeep,
 * -32026 PropertyIndexOOB).
 *
 * **Widget-not-found error code (-32039).** New Chunk C code returned by
 * ``umg.get_widget_property`` when ``widget_name`` doesn't match any widget in the WidgetTree.
 * Message embeds the list of top-level widget names (capped at 16) so the caller can correct a
 * typo without an extra ``umg.list_widgets`` round-trip when the tree is small.
 */
namespace FUMGTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	// ─── Category C: UMG read-only ──────────────────────────────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_ListWidgets(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_GetWidgetProperty(const FMCPRequest& Request);
}
