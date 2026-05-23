// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Wave H Surface 2 — UCurveBase + UCurveTable inspection / mutation surface. 4 user-visible tools,
 * all Lane A.
 *
 * Tool roster:
 *   curve.list      — paginated UCurveBase + UCurveTable enumeration via IAssetRegistry::GetAssets.
 *                     Optional ``types`` filter narrows to a subset of
 *                     {UCurveFloat, UCurveLinearColor, UCurveVector, UCurveTable}; default = all.
 *                     Per-entry { asset_path, curve_class }. Standard FMCPPageCursor over ObjectPath.
 *                     Read-only, no PIE guard.
 *   curve.get_data  — read keyframe data from a single curve.
 *                       UCurveFloat              → { keys: [...] } from FloatCurve
 *                       UCurveLinearColor        → { keys: [...] } over 4 channels (R, G, B, A)
 *                       UCurveVector             → { keys: [...] } over 3 channels (X, Y, Z)
 *                       UCurveTable + row name   → { keys: [...] } for the named row's float curve
 *                     Each key: { channel?: string (multi-channel curves only), time, value,
 *                                 interp_mode: "Linear"|"Constant"|"Cubic"|"None",
 *                                 tangent_in, tangent_out }.
 *                     Read-only, no PIE guard.
 *   curve.set_data  — replace ENTIRE key set on a curve (clear-then-replace semantics). Takes
 *                     ``keys: [{ time, value, interp_mode? }]`` plus optional ``key`` (row name for
 *                     UCurveTable). For multi-channel curves (UCurveLinearColor / UCurveVector)
 *                     each input key MUST include the ``channel`` field (R/G/B/A or X/Y/Z) — keys
 *                     without channel are silently distributed to channel 0 with a single-channel
 *                     fallback note. PIE-guarded mutator with FScopedTransaction + MarkPackageDirty.
 *                     Returns { prior_key_count, new_key_count }.
 *   curve.add_key   — append (or update-in-place when time matches an existing key) a single key
 *                     on the curve. Defaults interp_mode to "Cubic". For multi-channel curves the
 *                     ``channel`` field is required; for UCurveTable the ``key`` field selects the
 *                     row. PIE-guarded mutator, transacted, dirty. Returns { added: bool,
 *                     was_replaced: bool, new_key_count }.
 *
 * **Read tools (list/get_data) bypass PIE guard.** Mutators (set_data/add_key) refuse during PIE
 * with -32027 + frozen kMCPMessagePIEActive text.
 *
 * **Curve-class shape contract.**
 *   - UCurveFloat has one FRichCurve member ``FloatCurve``.
 *   - UCurveLinearColor has FRichCurve FloatCurves[4] (R, G, B, A).
 *   - UCurveVector has FRichCurve FloatCurves[3] (X, Y, Z).
 *   - UCurveTable's RowMap stores FRealCurve* (abstract). All FatumGame curve tables ship in
 *     ECurveTableMode::RichCurves so downcasting to FRichCurve is safe; SimpleCurve mode is
 *     supported via fallback (FSimpleCurve only has float interp_mode and single tangent).
 *
 * **Interp mode mapping** (JSON ↔ enum):
 *   "Linear"   ↔ RCIM_Linear
 *   "Constant" ↔ RCIM_Constant
 *   "Cubic"    ↔ RCIM_Cubic   (default for add_key when interp_mode omitted)
 *   "None"     ↔ RCIM_None
 *
 * **Error codes (all reused from existing range — no new codes):**
 *   -32004 ObjectNotFound        Curve / row name not loadable / missing
 *   -32010 InvalidPath           malformed curve_path
 *   -32011 WrongClass            asset isn't a curve (UCurveFloat/UCurveLinearColor/UCurveVector
 *                                 /UCurveTable)
 *   -32027 PIEActive             editor-world mutator during PIE
 *   -32602 InvalidParams         missing args / malformed keys array / missing channel for
 *                                 multi-channel curve / missing key for UCurveTable / unknown
 *                                 interp_mode string
 *   -32603 InternalError         curve table has no valid row map / unhandled curve subclass
 */
namespace FCurveTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	// ─── Wave H Surface 2: Curve tools ──────────────────────────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_List(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_GetData(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_SetData(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_AddKey(const FMCPRequest& Request);
}
