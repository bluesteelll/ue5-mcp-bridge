// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Wave D Surface 4 — actor outliner folder surface. 4 user-visible tools, all Lane A.
 *
 * Tool roster:
 *   folder.list      → enumerate every folder path in the editor world (FActorFolders::ForEachFolder).
 *   folder.create    → create a folder via FActorFolders::Get().CreateFolder. PIE-guarded, transacted.
 *   folder.delete    → delete a folder; optionally re-parent children to the deleted folder's
 *                      parent (walk all actors whose folder path lives under the deleted prefix).
 *                      PIE-guarded, transacted.
 *   folder.set_actor → AActor::SetFolderPath. Empty folder_path = world root. PIE-guarded, transacted.
 *
 * **All Lane A** — game-thread only (FActorFolders + UWorld traversal + AActor mutation).
 * Mutators refuse PIE with -32027 + frozen kMCPMessagePIEActive. Reads bypass.
 *
 * Errors: standard kMCPError* + 1 new code -32056 FolderNotFound for folder.delete /
 *         folder.set_actor when an explicit folder_path doesn't exist.
 */
namespace FFolderTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	UNREALMCPBRIDGE_API FMCPResponse Tool_List(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_Create(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_Delete(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_SetActor(const FMCPRequest& Request);
}
