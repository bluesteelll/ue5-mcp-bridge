// Copyright FatumGame. All Rights Reserved.

#include "BPCallSiteTools.h"

#include "MCPSurfaceRegistry.h"

#include "FMCPDispatchQueue.h"
#include "MCPJsonBuilder.h"
#include "MCPToolHelpers.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "K2Node_CallFunction.h"
#include "UObject/Class.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectGlobals.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	// BPCS_ prefix per the unity-build symbol-collision pattern.
	constexpr int32 kBPCSDefaultMaxResults = 200;
	constexpr int32 kBPCSMaxResultsCap     = 1000;
	constexpr int32 kBPCSScanCap           = 5000;  // soft cap on scanned BPs per call

	/**
	 * Cursor format: ``BP_index:<N>|node_index:<M>``. Where N = index into the
	 * sorted ``AssetDataList`` array; M = index into the current BP's flattened (Graph, Node) walk
	 * order (we restart node iteration from 0 when crossing a BP boundary, so cursor only needs to
	 * carry the within-BP node index for the BP we paused on).
	 *
	 * Opaque from caller's perspective; per blueprint N1 we document the format so reverse parsing
	 * is robust.
	 */
	struct FBPCSCursor
	{
		int32 BPIndex   = 0;
		int32 NodeIndex = 0;
	};

	bool BPCS_ParseCursor(const FString& Wire, FBPCSCursor& OutCursor, FString& OutErr)
	{
		OutCursor = FBPCSCursor();
		if (Wire.IsEmpty()) { return true; }   // empty cursor → start from beginning, OK

		FString Left, Right;
		if (!Wire.Split(TEXT("|"), &Left, &Right))
		{
			OutErr = FString::Printf(TEXT("cursor missing '|' delimiter: '%s'"), *Wire);
			return false;
		}
		auto ParsePart = [](const FString& Part, const TCHAR* ExpectedKey, int32& OutVal) -> bool
		{
			FString Key, Val;
			if (!Part.Split(TEXT(":"), &Key, &Val)) { return false; }
			if (!Key.Equals(ExpectedKey, ESearchCase::IgnoreCase)) { return false; }
			OutVal = FCString::Atoi(*Val);
			return OutVal >= 0;
		};
		if (!ParsePart(Left, TEXT("BP_index"), OutCursor.BPIndex))
		{
			OutErr = FString::Printf(TEXT("cursor left part not 'BP_index:N': '%s'"), *Left);
			return false;
		}
		if (!ParsePart(Right, TEXT("node_index"), OutCursor.NodeIndex))
		{
			OutErr = FString::Printf(TEXT("cursor right part not 'node_index:N': '%s'"), *Right);
			return false;
		}
		return true;
	}

	FString BPCS_EncodeCursor(int32 BPIndex, int32 NodeIndex)
	{
		return FString::Printf(TEXT("BP_index:%d|node_index:%d"), BPIndex, NodeIndex);
	}

	/**
	 * Resolve a ``target_class`` string. Per critique Q2 decision: support both native ``/Script/``
	 * paths (via ``FindFirstObjectSafe<UClass>``, since native classes are always loaded) and
	 * ``/Game/...`` UPackage paths (via ``LoadObject<UBlueprint>`` → ``BP->GeneratedClass``).
	 *
	 * Returns the resolved UClass on success. On failure returns null and populates ``OutErr`` for
	 * the caller to surface as -32004 ObjectNotFound. Empty input → returns null + leaves OutErr
	 * empty (caller treats null as "no class filter").
	 */
	UClass* BPCS_ResolveTargetClass(const FString& ClassPath, FString& OutErr)
	{
		if (ClassPath.IsEmpty()) { return nullptr; }

		if (ClassPath.StartsWith(TEXT("/Script/")))
		{
			// Native class path — try direct find. FindFirstObjectSafe accepts the full path or
			// the short name; we pass the full path verbatim.
			UClass* Native = FindFirstObjectSafe<UClass>(*ClassPath);
			if (!Native)
			{
				// FindFirstObjectSafe with a path may fail when the path contains the module
				// prefix; try also as a short class name fallback (e.g. "Actor" from
				// "/Script/Engine.Actor").
				FString ShortName = ClassPath;
				int32 DotPos = INDEX_NONE;
				if (ShortName.FindLastChar(TEXT('.'), DotPos))
				{
					ShortName = ShortName.RightChop(DotPos + 1);
				}
				Native = FindFirstObjectSafe<UClass>(*ShortName);
			}
			if (!Native)
			{
				OutErr = FString::Printf(TEXT("target_class '%s' (treated as native /Script/ path) not found"), *ClassPath);
				return nullptr;
			}
			return Native;
		}

		// Blueprint class path (/Game/...). Load the BP and pull GeneratedClass.
		UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *ClassPath);
		if (!BP)
		{
			// Retry with .LeafName suffix (FMCPBlueprintUtils mirrors this pattern).
			FString Retry = ClassPath;
			int32 LastSlash = INDEX_NONE;
			if (Retry.FindLastChar(TEXT('/'), LastSlash))
			{
				const FString Leaf = Retry.RightChop(LastSlash + 1);
				Retry = Retry + TEXT(".") + Leaf;
				BP = LoadObject<UBlueprint>(nullptr, *Retry);
			}
		}
		if (!BP)
		{
			OutErr = FString::Printf(TEXT("target_class '%s' could not be loaded as a UBlueprint"), *ClassPath);
			return nullptr;
		}
		if (!BP->GeneratedClass)
		{
			OutErr = FString::Printf(TEXT("target_class '%s' loaded but has no GeneratedClass (uncompiled?)"), *ClassPath);
			return nullptr;
		}
		return BP->GeneratedClass;
	}
} // namespace

namespace FBPCallSiteTools
{

// ─── bp.find_function_callsites ───────────────────────────────────────────────────────────────
FMCPResponse Tool_FindFunctionCallsites(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPResponse Err;
	FString TargetFunctionStr;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("target_function"), TargetFunctionStr, Err)) { return Err; }
	const FName TargetFunctionFName(*TargetFunctionStr);

	// Optional target_class (per critique Q2 — supports both /Script/ native and /Game/ BP).
	FString TargetClassStr;
	if (Request.Args.IsValid())
	{
		Request.Args->TryGetStringField(TEXT("target_class"), TargetClassStr);
	}
	UClass* TargetClass = nullptr;
	if (!TargetClassStr.IsEmpty())
	{
		FString ResolveErr;
		TargetClass = BPCS_ResolveTargetClass(TargetClassStr, ResolveErr);
		if (!TargetClass)
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound, ResolveErr);
		}
	}

	// max_results clamp [1, 1000].
	int32 MaxResults = kBPCSDefaultMaxResults;
	if (Request.Args.IsValid()) { Request.Args->TryGetNumberField(TEXT("max_results"), MaxResults); }
	MaxResults = FMath::Clamp(MaxResults, 1, kBPCSMaxResultsCap);

	// Cursor.
	FString CursorWire;
	if (Request.Args.IsValid()) { Request.Args->TryGetStringField(TEXT("cursor"), CursorWire); }
	FBPCSCursor StartCursor;
	{
		FString ParseErr;
		if (!BPCS_ParseCursor(CursorWire, StartCursor, ParseErr))
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams, ParseErr);
		}
	}

	// ── Asset registry preflight (per critique C3 — return -32058 with retry hint if loading). ──
	IAssetRegistry& IAR = FAssetRegistryModule::GetRegistry();
	if (IAR.IsLoadingAssets())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorOperationFailed,
			TEXT("asset registry still scanning project; retry in 1-5 seconds"));
	}

	// Enumerate all blueprints (non-subclass-walk: only direct UBlueprint instances).
	TArray<FAssetData> AssetDataList;
	{
		FARFilter Filter;
		Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
		Filter.bRecursiveClasses = false;
		IAR.GetAssets(Filter, AssetDataList);
	}
	// Stable sort by ObjectPath so cursor BPIndex is reproducible across calls (matches AR_SortByObjectPath).
	AssetDataList.StableSort([](const FAssetData& A, const FAssetData& B)
	{
		return A.GetObjectPathString().Compare(B.GetObjectPathString(), ESearchCase::IgnoreCase) < 0;
	});

	const int32 TotalBPs = AssetDataList.Num();
	if (StartCursor.BPIndex < 0) { StartCursor.BPIndex = 0; }

	// ── Walk. ──────────────────────────────────────────────────────────────────────────────────
	FMCPJsonArrayBuilder Callsites;
	int32 ScannedBPs = 0;
	int32 NextBPIndex = StartCursor.BPIndex;
	int32 NextNodeIndex = StartCursor.NodeIndex;
	bool bHasMore = false;

	auto MakeCallsiteEntry = [&](const FString& BPPath, const FString& GraphName, const UEdGraphNode* Node)
	{
		Callsites.AddObject([&](FMCPJsonBuilder& B)
		{
			B.Str(TEXT("caller_blueprint"), BPPath);
			B.Str(TEXT("caller_function"),  GraphName);
			B.Str(TEXT("node_guid"),        Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
			B.Str(TEXT("node_title"),       Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
			B.Str(TEXT("comment"),          Node->NodeComment);
		});
	};

	for (int32 BPIdx = StartCursor.BPIndex; BPIdx < TotalBPs; ++BPIdx)
	{
		// Soft scan cap — return partial result + cursor for safety on enormous projects.
		if (ScannedBPs >= kBPCSScanCap)
		{
			NextBPIndex = BPIdx;
			NextNodeIndex = 0;
			bHasMore = true;
			break;
		}

		const FAssetData& AD = AssetDataList[BPIdx];
		const FString BPPath = AD.GetSoftObjectPath().ToString();

		++ScannedBPs;

		UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *BPPath);
		if (!BP)
		{
			UE_LOG(LogMCP, Warning, TEXT("bp.find_function_callsites: LoadObject failed for '%s'; skipping"), *BPPath);
			continue;
		}

		// Flatten graphs in stable order: UbergraphPages then FunctionGraphs then MacroGraphs.
		TArray<UEdGraph*> AllGraphs;
		AllGraphs.Reserve(BP->UbergraphPages.Num() + BP->FunctionGraphs.Num() + BP->MacroGraphs.Num());
		for (UEdGraph* G : BP->UbergraphPages) { if (G) AllGraphs.Add(G); }
		for (UEdGraph* G : BP->FunctionGraphs) { if (G) AllGraphs.Add(G); }
		for (UEdGraph* G : BP->MacroGraphs)    { if (G) AllGraphs.Add(G); }

		// Walk all nodes across all graphs in this BP. The cursor's NodeIndex is interpreted as
		// a flat index across this BP's (graph, node) tuples — we restart from 0 when crossing a
		// BP boundary, so NodeIndex only matters for the BP we paused on (BPIdx == StartCursor.BPIndex).
		int32 FlatNodeIdx = 0;
		const int32 SkipUntil = (BPIdx == StartCursor.BPIndex) ? StartCursor.NodeIndex : 0;

		bool bBPFullScanComplete = true;
		for (UEdGraph* Graph : AllGraphs)
		{
			if (!Graph) { continue; }
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node) { ++FlatNodeIdx; continue; }
				// Resume-skip on first iteration of resumed BP.
				if (FlatNodeIdx < SkipUntil) { ++FlatNodeIdx; continue; }

				UK2Node_CallFunction* CallFn = Cast<UK2Node_CallFunction>(Node);
				if (CallFn)
				{
					// Match on FName equality (case-sensitive; matches engine convention for
					// FunctionReference.GetMemberName()).
					if (CallFn->FunctionReference.GetMemberName() == TargetFunctionFName)
					{
						if (!TargetClass || CallFn->FunctionReference.GetMemberParentClass() == TargetClass)
						{
							MakeCallsiteEntry(BPPath, Graph->GetName(), Node);
							if (Callsites.Num() >= MaxResults)
							{
								// Stop here; cursor points to the NEXT node in this graph.
								NextBPIndex = BPIdx;
								NextNodeIndex = FlatNodeIdx + 1;
								bHasMore = true;
								bBPFullScanComplete = false;
								goto DoneScanning;
							}
						}
					}
				}
				++FlatNodeIdx;
			}
		}
		// Finished this BP — move on (or wrap up).
		if (bBPFullScanComplete && BPIdx == TotalBPs - 1)
		{
			NextBPIndex = TotalBPs;   // sentinel "done"
			NextNodeIndex = 0;
		}
	}

DoneScanning:

	FMCPJsonBuilder Out;
	Out.Arr(TEXT("callsites"), Callsites.ToValueArray())
		.Int(TEXT("total_scanned_blueprints"), ScannedBPs)
		.Int(TEXT("total_callsites_in_page"), Callsites.Num());
	if (bHasMore && NextBPIndex < TotalBPs)
	{
		Out.Str(TEXT("next_cursor"), BPCS_EncodeCursor(NextBPIndex, NextNodeIndex));
	}
	else
	{
		Out.Null(TEXT("next_cursor"));
	}
	return Out.BuildSuccess(Request);
}

// ─── Registration ─────────────────────────────────────────────────────────────────────────────
void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	// Lane A — LoadObject<UBlueprint> + IAR.GetAssets() are both GT-only post-2026-05 hotfix.
	RegisterTool(TEXT("bp.find_function_callsites"), &Tool_FindFunctionCallsites, /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("BPCallSite surface registered: bp.find_function_callsites (Lane A)"));
}

} // namespace FBPCallSiteTools

MCP_REGISTER_SURFACE(BPCallSiteTools, &FBPCallSiteTools::Register)
