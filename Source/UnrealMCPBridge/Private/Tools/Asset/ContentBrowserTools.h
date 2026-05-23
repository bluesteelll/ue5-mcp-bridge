// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Phase 2 — Category B (Content Browser write operations). 12 tools.
 *
 * All Lane A (game-thread) — every call mutates Editor state (FScopedTransaction,
 * EditorAssetSubsystem, ObjectTools, IAssetTools). Per-asset transactional wrap (D4) so
 * Ctrl+Z undoes one item at a time on bulk ops.
 *
 * Async tools (``cb.save_all_dirty``, ``cb.bulk_import``) return ``{job_id}`` immediately;
 * caller polls ``job.status`` / ``job.result``.
 */
namespace FContentBrowserTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	// Day 6: creation / metadata
	UNREALMCPBRIDGE_API FMCPResponse Tool_CreateFolder(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_Rename(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_Save(const FMCPRequest& Request);

	// Day 7: bulk mutations
	UNREALMCPBRIDGE_API FMCPResponse Tool_Move(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_Duplicate(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_Delete(const FMCPRequest& Request);

	// Day 8: redirector + folder enumeration
	UNREALMCPBRIDGE_API FMCPResponse Tool_FixRedirectors(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_ListFolders(const FMCPRequest& Request);

	// Day 9: import / export
	UNREALMCPBRIDGE_API FMCPResponse Tool_Import(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_Export(const FMCPRequest& Request);

	// Day 10: async jobs
	UNREALMCPBRIDGE_API FMCPResponse Tool_SaveAllDirty(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_BulkImport(const FMCPRequest& Request);
}
