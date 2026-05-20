// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Wave D Surface 2 — debug.* visual overlay surface. 6 user-visible tools, all Lane A.
 *
 * Tool roster:
 *   debug.draw_line     → DrawDebugLine wrapper
 *   debug.draw_sphere   → DrawDebugSphere wrapper
 *   debug.draw_box      → DrawDebugBox wrapper (with optional rotation)
 *   debug.draw_arrow    → DrawDebugDirectionalArrow wrapper
 *   debug.draw_text     → DrawDebugString wrapper (world-space text)
 *   debug.clear         → FlushPersistentDebugLines + FlushDebugStrings (everything)
 *
 * **World selection.** Picks PIE world if PIE active, otherwise editor world — overlays
 * naturally appear in the window the caller is watching. NOT PIE-guarded (drawing works
 * in both worlds; debug overlays are not asset state).
 *
 * **No transactions / no package dirty marking.** Debug overlays live in the persistent
 * line batcher (per-world transient state), not on any UObject — there is nothing to undo
 * and nothing to save.
 *
 * **All Lane A** — DrawDebug* + line batcher access run on the game thread.
 *
 * Errors: -32602 InvalidParams (missing/malformed args), -32603 Internal (no world).
 */
namespace FDebugTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	UNREALMCPBRIDGE_API FMCPResponse Tool_DrawLine(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_DrawSphere(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_DrawBox(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_DrawArrow(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_DrawText(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_Clear(const FMCPRequest& Request);
}
