// Copyright FatumGame. All Rights Reserved.

#include "AIBehaviorTreeTools.h"

#include "FMCPDispatchQueue.h"
#include "MCPAssetLoader.h"
#include "MCPJsonBuilder.h"
#include "MCPMutatorScope.h"
#include "MCPToolHelpers.h"
#include "MCPClassResolver.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPActorPathUtils.h"
#include "Utils/MCPAssetPathUtils.h"
#include "Utils/MCPPageCursor.h"
#include "Utils/MCPReflection.h"

#include "AIController.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTNode.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/BTTaskNode.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// Hard cap on recursion depth in NodeToJson — guards against pathological / cyclic trees that
	// somehow slipped past UBehaviorTree::PreSave validation. Real BTs never approach this.
	constexpr int32 kAIBTMaxRecursionDepth = 64;

	// ─── Actor → AAIController resolution ────────────────────────────────────────────────────────

	/**
	 * Resolve an actor_path to an AAIController*. Accepts two cases:
	 *   - The actor itself IS an AAIController (rare but valid — Cast directly).
	 *   - The actor is an APawn whose Controller is an AAIController (the common path).
	 *
	 * Returns null + populates OutErrorCode + OutErrorMsg on failure. Pass-through for the
	 * underlying ResolveActor errors (-32010 invalid path, -32004 not found, etc.) plus the
	 * surface-specific -32011 for "actor exists but has no AAIController".
	 *
	 * bRejectPIE=false so this works on PIE pawns (start/stop are runtime-testing tools).
	 */
	AAIController* AIBT_ResolveAIController(const FString& ActorPath, int32& OutErrorCode, FString& OutErrorMsg)
	{
		bool bAmbiguous = false;
		FString AmbigHint;
		FString ResolveErr;
		AActor* Actor = FMCPActorPathUtils::ResolveActor(
			ActorPath, /*bRejectPIE*/ false, bAmbiguous, AmbigHint, ResolveErr);
		if (!Actor)
		{
			OutErrorCode = kMCPErrorObjectNotFound;
			OutErrorMsg = bAmbiguous
				? FString::Printf(TEXT("actor_path '%s' is ambiguous; candidates: %s"), *ActorPath, *AmbigHint)
				: FString::Printf(TEXT("actor_path '%s' did not resolve: %s"), *ActorPath, *ResolveErr);
			return nullptr;
		}

		// Case 1: actor is itself an AAIController.
		if (AAIController* DirectAIC = Cast<AAIController>(Actor))
		{
			return DirectAIC;
		}

		// Case 2: actor is an APawn whose Controller is an AAIController.
		if (APawn* Pawn = Cast<APawn>(Actor))
		{
			if (AAIController* AIC = Cast<AAIController>(Pawn->GetController()))
			{
				return AIC;
			}
			OutErrorCode = kMCPErrorWrongClass;
			OutErrorMsg = FString::Printf(
				TEXT("pawn '%s' has no AAIController (Controller=%s)"),
				*ActorPath,
				Pawn->GetController() ? *Pawn->GetController()->GetClass()->GetName() : TEXT("nullptr"));
			return nullptr;
		}

		// Case 3: actor is neither an AAIController nor an APawn.
		OutErrorCode = kMCPErrorWrongClass;
		OutErrorMsg = FString::Printf(
			TEXT("actor '%s' is of class '%s'; expected AAIController or APawn-with-AAIController"),
			*ActorPath, *Actor->GetClass()->GetName());
		return nullptr;
	}

	// ─── BT node → JSON (recursive walk) ─────────────────────────────────────────────────────────

	// Forward decl — composite branch recurses via NodeToJson.
	TSharedRef<FJsonObject> AIBT_NodeToJson(const UBTNode* Node, int32 Depth);

	/** Serialize a UBTDecorator/UBTService aux node (no children — just class + name + index). */
	TSharedRef<FJsonObject> AIBT_AuxNodeToJson(const UBTNode* Aux)
	{
		check(Aux);
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("node_class"), Aux->GetClass()->GetName());
		Obj->SetStringField(TEXT("node_name"), Aux->GetNodeName());
		Obj->SetNumberField(TEXT("execution_index"), static_cast<double>(Aux->GetExecutionIndex()));
		return Obj;
	}

	TSharedRef<FJsonObject> AIBT_NodeToJson(const UBTNode* Node, int32 Depth)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (!Node)
		{
			Obj->SetStringField(TEXT("node_class"), TEXT("<null>"));
			Obj->SetStringField(TEXT("node_name"), TEXT("<null>"));
			return Obj;
		}

		Obj->SetStringField(TEXT("node_class"), Node->GetClass()->GetName());
		Obj->SetStringField(TEXT("node_name"), Node->GetNodeName());
		Obj->SetNumberField(TEXT("execution_index"), static_cast<double>(Node->GetExecutionIndex()));

		// Recursion guard. Real BTs never approach this; the cap exists purely to prevent runaway
		// stack growth on a corrupted tree (cycle / self-parent).
		if (Depth >= kAIBTMaxRecursionDepth)
		{
			Obj->SetBoolField(TEXT("truncated_at_max_depth"), true);
			return Obj;
		}

		// Composite nodes carry children + decorators + services. Tasks are leaves; decorators /
		// services are aux nodes serialized inline by their parent composite (via Children[].Decorators
		// and Services[]).
		if (const UBTCompositeNode* Composite = Cast<const UBTCompositeNode>(Node))
		{
			TArray<TSharedPtr<FJsonValue>> ChildArr;
			ChildArr.Reserve(Composite->Children.Num());
			for (const FBTCompositeChild& Child : Composite->Children)
			{
				// Per-child sub-object so callers see the decorator stack that gates THIS child slot
				// (BT decorators attach to the parent's child-edge, not the child node itself).
				TSharedRef<FJsonObject> ChildObj = MakeShared<FJsonObject>();

				// The child node itself — exactly one of ChildComposite / ChildTask is non-null per
				// FBTCompositeChild contract. The other branch covers the legal-empty case (e.g.
				// freshly-constructed composite with a placeholder slot — rare but UE allows it).
				const UBTNode* ChildNode = nullptr;
				if (Child.ChildComposite) { ChildNode = Child.ChildComposite; }
				else if (Child.ChildTask) { ChildNode = Child.ChildTask; }
				ChildObj->SetObjectField(TEXT("node"), AIBT_NodeToJson(ChildNode, Depth + 1));

				// Per-child decorator stack (UBTDecorator instances gating this branch).
				TArray<TSharedPtr<FJsonValue>> ChildDecArr;
				ChildDecArr.Reserve(Child.Decorators.Num());
				for (const UBTDecorator* Dec : Child.Decorators)
				{
					if (Dec) { ChildDecArr.Add(MakeShared<FJsonValueObject>(AIBT_AuxNodeToJson(Dec))); }
				}
				ChildObj->SetArrayField(TEXT("decorators"), ChildDecArr);

				ChildArr.Add(MakeShared<FJsonValueObject>(ChildObj));
			}
			Obj->SetArrayField(TEXT("children"), ChildArr);

			// Composite-level services (UBTService instances ticking while this composite is active).
			TArray<TSharedPtr<FJsonValue>> ServicesArr;
			ServicesArr.Reserve(Composite->Services.Num());
			for (const UBTService* Service : Composite->Services)
			{
				if (Service) { ServicesArr.Add(MakeShared<FJsonValueObject>(AIBT_AuxNodeToJson(Service))); }
			}
			Obj->SetArrayField(TEXT("services"), ServicesArr);
		}
		else if (Cast<const UBTTaskNode>(Node))
		{
			// Task = leaf. Empty children array is intentional (the absence is informative for
			// downstream consumers — they can tell at a glance which nodes are leaves).
			Obj->SetArrayField(TEXT("children"), TArray<TSharedPtr<FJsonValue>>());
		}
		// Other UBTNode subclasses (UBTAuxiliaryNode etc.) fall through with no children / services
		// fields — they shouldn't appear as standalone tree nodes (only as decorators/services), but
		// we serialize their class/name anyway so the caller can debug an unexpected shape.

		return Obj;
	}

	// ─── Wave P path-resolver + class-resolver helpers ────────────────────────────────────────────

	/**
	 * Classification of what a BT path refers to. ``Root`` is the special composite at BT.RootNode
	 * (parent is null in that case — the FBTNodeRef.Parent stays null and IndexInParent=INDEX_NONE).
	 * Child / Decorator / Service refer to entries inside a parent composite.
	 */
	enum class EAIBTRefKind : uint8
	{
		Root,         // BT->RootNode itself
		Child,        // Parent->Children[Index].ChildComposite or ChildTask
		Decorator,    // Parent->Children[Index].Decorators[SubIndex]   OR
		              // BT->RootDecorators[Index] when ParentChildIndex == INDEX_NONE
		Service,      // Parent->Services[Index] (composite only)
	};

	struct FAIBTNodeRef
	{
		UBTNode*          Node             = nullptr;  // Resolved leaf node (or nullptr for empty Root)
		UBTCompositeNode* Parent           = nullptr;  // Composite owning the child entry (null for Root)
		int32             ParentChildIndex = INDEX_NONE; // Index into Parent->Children[] for Decorator
		                                                  // attached to a child edge. INDEX_NONE means
		                                                  // the decorator/service is on Root level.
		int32             SubIndex         = INDEX_NONE; // Decorator/Service slot index (or child index
		                                                  // for Kind=Child).
		EAIBTRefKind      Kind             = EAIBTRefKind::Root;
	};

	/**
	 * Walk a dotted/bracket BT path.
	 *
	 * Path grammar:
	 *   "Root"                          → BT.RootNode (composite)
	 *   "Root/Children[N]"              → BT.RootNode.Children[N] (composite or task node)
	 *   "Root/Children[N]/Children[M]"  → nested composite child (composite must exist at [N])
	 *   "Root/Decorators[N]"            → BT.RootDecorators[N]   (root-level decorator)
	 *   "Root/Children[N]/Decorators[M]"→ Children[N].Decorators[M] (child-edge decorator)
	 *   "Root/Services[N]"              → BT.RootNode.Services[N] (root composite service)
	 *   "Root/Children[N]/Services[M]"  → BT.RootNode.Children[N].ChildComposite.Services[M]
	 *
	 * Returns false + populates OutError on any malformed segment / index OOB / type mismatch.
	 * Caller picks the appropriate -32xxx error code based on context (most callers map
	 * "missing root" / "index OOB" / "not a composite" to InvalidParams since the path itself
	 * is caller-supplied).
	 *
	 * Implementation note: this is intentionally string-driven parsing. We accept the path-by-
	 * convention overhead in exchange for stable, human-readable diagnostics (the alternative —
	 * structured args { kind: "child", parent: [...indices], index: N } — is much harder to
	 * write by AI agents and harder to read in test logs).
	 */
	bool AIBT_ResolvePath(UBehaviorTree* BT, const FString& Path, FAIBTNodeRef& OutRef, FString& OutError)
	{
		check(BT);

		OutRef = FAIBTNodeRef{};
		if (Path.IsEmpty())
		{
			OutError = TEXT("path is empty");
			return false;
		}

		// Tokenize by '/'.
		TArray<FString> Tokens;
		Path.ParseIntoArray(Tokens, TEXT("/"), /*InCullEmpty*/ true);
		if (Tokens.Num() == 0 || !Tokens[0].Equals(TEXT("Root"), ESearchCase::IgnoreCase))
		{
			OutError = FString::Printf(TEXT("path '%s' must start with 'Root'"), *Path);
			return false;
		}

		// Lambda to parse a token like "Children[3]" into ("Children", 3). Returns false on malformed.
		auto ParseBracketToken = [&OutError](const FString& Token, FString& OutName, int32& OutIndex) -> bool
		{
			int32 LBracket = INDEX_NONE;
			int32 RBracket = INDEX_NONE;
			if (!Token.FindChar(TEXT('['), LBracket) || !Token.FindLastChar(TEXT(']'), RBracket)
				|| RBracket <= LBracket + 1 || RBracket != Token.Len() - 1)
			{
				OutError = FString::Printf(TEXT("token '%s' must be 'Name[Index]'"), *Token);
				return false;
			}
			OutName = Token.Left(LBracket);
			const FString IndexStr = Token.Mid(LBracket + 1, RBracket - LBracket - 1);
			if (IndexStr.IsEmpty() || !IndexStr.IsNumeric())
			{
				OutError = FString::Printf(TEXT("token '%s' has malformed index"), *Token);
				return false;
			}
			OutIndex = FCString::Atoi(*IndexStr);
			if (OutIndex < 0)
			{
				OutError = FString::Printf(TEXT("token '%s' has negative index"), *Token);
				return false;
			}
			return true;
		};

		// Bare "Root" — return reference to the root composite (or empty if RootNode is null).
		if (Tokens.Num() == 1)
		{
			OutRef.Kind   = EAIBTRefKind::Root;
			OutRef.Node   = BT->RootNode;
			OutRef.Parent = nullptr;
			return true;
		}

		// Multi-segment path — walk from Root.
		UBTCompositeNode* CurrentParent = BT->RootNode;
		int32 CurrentChildIdx = INDEX_NONE;  // INDEX_NONE → "at root level" (use RootDecorators for Decorators)
		UBTNode* CurrentNode = BT->RootNode;

		for (int32 i = 1; i < Tokens.Num(); ++i)
		{
			const FString& Tok = Tokens[i];
			FString Name;
			int32   Idx = INDEX_NONE;
			if (!ParseBracketToken(Tok, Name, Idx)) { return false; }

			const bool bIsLastToken = (i == Tokens.Num() - 1);

			if (Name.Equals(TEXT("Children"), ESearchCase::IgnoreCase))
			{
				if (!CurrentParent)
				{
					OutError = FString::Printf(TEXT("path '%s': 'Children[%d]' at segment %d — parent "
						"is not a composite (path goes through a leaf task or null root)"), *Path, Idx, i);
					return false;
				}
				if (!CurrentParent->Children.IsValidIndex(Idx))
				{
					OutError = FString::Printf(TEXT("path '%s': Children[%d] out of range [0, %d) at segment %d"),
						*Path, Idx, CurrentParent->Children.Num(), i);
					return false;
				}

				const FBTCompositeChild& ChildSlot = CurrentParent->Children[Idx];
				UBTNode* ChildNode = ChildSlot.ChildComposite
					? static_cast<UBTNode*>(ChildSlot.ChildComposite)
					: static_cast<UBTNode*>(ChildSlot.ChildTask);
				if (!ChildNode)
				{
					OutError = FString::Printf(TEXT("path '%s': Children[%d] slot has no ChildComposite or ChildTask "
						"at segment %d (corrupt asset?)"), *Path, Idx, i);
					return false;
				}

				if (bIsLastToken)
				{
					OutRef.Kind             = EAIBTRefKind::Child;
					OutRef.Node             = ChildNode;
					OutRef.Parent           = CurrentParent;
					OutRef.ParentChildIndex = Idx;
					OutRef.SubIndex         = Idx;
					return true;
				}

				// Descend — next iteration walks INTO the child slot. The child must be a composite
				// for further "Children[]" descent to work; tasks are leaves.
				CurrentParent      = ChildSlot.ChildComposite;  // May be null if slot is a Task
				CurrentChildIdx    = Idx;
				CurrentNode        = ChildNode;
				continue;
			}

			if (Name.Equals(TEXT("Decorators"), ESearchCase::IgnoreCase))
			{
				if (!bIsLastToken)
				{
					OutError = FString::Printf(TEXT("path '%s': 'Decorators[N]' must be the last segment"), *Path);
					return false;
				}

				// Two cases:
				//   (a) "Root/Decorators[N]" — CurrentChildIdx==INDEX_NONE, CurrentParent==BT->RootNode.
				//       Indexes into BT->RootDecorators[N].
				//   (b) "Root/.../Children[M]/Decorators[N]" — CurrentChildIdx==M, walked one level up
				//       (the loop body for Children already left CurrentParent pointing at the COMPOSITE
				//       that owns the child slot, NOT at the child itself). We index into
				//       CurrentParent->Children[CurrentChildIdx].Decorators[N].
				if (CurrentChildIdx == INDEX_NONE)
				{
					// Root-level: index into BT->RootDecorators[]
					if (!BT->RootDecorators.IsValidIndex(Idx))
					{
						OutError = FString::Printf(
							TEXT("path '%s': RootDecorators[%d] out of range [0, %d)"),
							*Path, Idx, BT->RootDecorators.Num());
						return false;
					}
					OutRef.Kind             = EAIBTRefKind::Decorator;
					OutRef.Node             = BT->RootDecorators[Idx];
					OutRef.Parent           = nullptr;  // Root-level: no parent composite
					OutRef.ParentChildIndex = INDEX_NONE;
					OutRef.SubIndex         = Idx;
					return true;
				}

				// Child-edge: index into Parent->Children[CurrentChildIdx].Decorators[Idx]
				check(CurrentParent);  // Loop invariant
				if (!CurrentParent->Children.IsValidIndex(CurrentChildIdx))
				{
					OutError = FString::Printf(
						TEXT("path '%s': internal — child index %d invalidated mid-walk"),
						*Path, CurrentChildIdx);
					return false;
				}
				const FBTCompositeChild& ChildSlot = CurrentParent->Children[CurrentChildIdx];
				if (!ChildSlot.Decorators.IsValidIndex(Idx))
				{
					OutError = FString::Printf(
						TEXT("path '%s': Children[%d].Decorators[%d] out of range [0, %d)"),
						*Path, CurrentChildIdx, Idx, ChildSlot.Decorators.Num());
					return false;
				}
				OutRef.Kind             = EAIBTRefKind::Decorator;
				OutRef.Node             = ChildSlot.Decorators[Idx];
				OutRef.Parent           = CurrentParent;
				OutRef.ParentChildIndex = CurrentChildIdx;
				OutRef.SubIndex         = Idx;
				return true;
			}

			if (Name.Equals(TEXT("Services"), ESearchCase::IgnoreCase))
			{
				if (!bIsLastToken)
				{
					OutError = FString::Printf(TEXT("path '%s': 'Services[N]' must be the last segment"), *Path);
					return false;
				}

				// Services attach to composites. Two cases:
				//   (a) "Root/Services[N]" → CurrentChildIdx==INDEX_NONE, services on BT->RootNode (composite).
				//   (b) "Root/.../Children[M]/Services[N]" → service on the COMPOSITE at the
				//       Children[M] slot. We need to walk INTO the child composite for this — the
				//       Children path already did that as part of the descent loop iteration.
				//
				// In case (b) the loop iteration for Children[M] set CurrentParent = ChildSlot.ChildComposite
				// when NOT last token, and reset CurrentChildIdx to INDEX_NONE for the next descent
				// — but Services[N] is the LAST token, so we need a different state.
				//
				// Implementation: when bIsLastToken && Name=="Services", the previous loop iteration
				// (Children[M]) would have early-returned bIsLastToken — so we wouldn't BE here.
				// To reach Services[N] we must have walked past Children[M] WITH continuation, meaning
				// CurrentParent already points at the descended composite OR equals BT->RootNode for
				// the "Root/Services" case.
				if (!CurrentParent)
				{
					OutError = FString::Printf(
						TEXT("path '%s': Services[%d] — current node is not a composite (services attach to composites only)"),
						*Path, Idx);
					return false;
				}
				if (!CurrentParent->Services.IsValidIndex(Idx))
				{
					OutError = FString::Printf(
						TEXT("path '%s': Services[%d] out of range [0, %d)"),
						*Path, Idx, CurrentParent->Services.Num());
					return false;
				}
				OutRef.Kind             = EAIBTRefKind::Service;
				OutRef.Node             = CurrentParent->Services[Idx];
				// For removal use case: the service lives on CurrentParent; "child index" is
				// CurrentChildIdx pointing back at the parent's owning composite if any.
				OutRef.Parent           = CurrentParent;
				OutRef.ParentChildIndex = CurrentChildIdx;
				OutRef.SubIndex         = Idx;
				return true;
			}

			OutError = FString::Printf(TEXT("path '%s': unknown segment '%s' (expected Children/Decorators/Services)"),
				*Path, *Tok);
			return false;
		}

		// Unreachable — the loop always returns or errors.
		OutError = FString::Printf(TEXT("path '%s': internal parse failure"), *Path);
		return false;
	}

	// AIBT_ResolveSubclassOf removed; replaced by FMCPClassResolver::Resolve (Wave Q2).
	// Default options match the prior behaviour: bRequirePathPrefix=true, bTryClassSuffix=true,
	// bRejectAbstract=true, bRejectDeprecated=true. Caller passes BaseClass via Options.BaseClass.

	// AIBT_ApplyProperties removed; replaced by FMCPToolHelpers::ApplyJsonProperties (Wave Q1).

	/** Convert ApplyJsonProperties output arrays into the standard FMCPJson shape. */
	void AIBT_AppendPropertyResults(FMCPJsonBuilder& Builder,
		const TArray<FString>& Applied, const TArray<FString>& Skipped)
	{
		FMCPJsonArrayBuilder AppliedArr;
		for (const FString& S : Applied) { AppliedArr.AddString(S); }
		FMCPJsonArrayBuilder SkippedArr;
		for (const FString& S : Skipped) { SkippedArr.AddString(S); }
		Builder.Arr(TEXT("properties_applied"), AppliedArr.ToValueArray())
		       .Arr(TEXT("properties_skipped"), SkippedArr.ToValueArray());
	}
} // namespace

namespace FAIBehaviorTreeTools
{

// ─── ai.bt.list_assets ─────────────────────────────────────────────────────────────────────────
//
// Args:
//   path_prefix?  : string   (default empty = all of /Game)
//   page_size?    : int      (default 100, clamp [1, 1000])
//   page_token?   : string   (opaque cursor returned by prior call)
//
// Result:
//   {
//     behavior_trees: [
//       {
//         asset_path             : string,    full /Game/AI/BT_X.BT_X soft-object-path form
//         blackboard_asset_path? : string,    omitted when BT has no BlackboardAsset
//         root_node_class?       : string,    omitted when BT has no RootNode (newly-created /
//                                             not-yet-edited tree)
//       }, ...
//     ],
//     total_known      : int,
//     next_page_token? : string
//   }
//
// Errors:
//   -32015 StaleCursor      filter mutated between pages (path_prefix changed; cursor's filter_hash
//                           no longer matches the current call's hash)
//   -32602 InvalidParams    malformed page_token (base64 decode / JSON parse failure)
//
// Notes on resilience:
//   - path_prefix is treated as an opaque string handed to FARFilter::PackagePaths (no -32010
//     up-front validation). Invalid prefixes silently return empty results rather than erroring —
//     matches the convention in data_table.list / asset.list.
//   - LoadModuleChecked("AssetRegistry") asserts on failure but the module is always loaded in
//     editor builds (the plugin is editor-only). No -32603 path here in practice.
//
// Notes:
//   - LoadObject for each entry on the page (NOT for total_known sweep) so blackboard_asset_path /
//     root_node_class are accurate without an extra round-trip. Loading is idempotent so callers
//     paginating pay the load cost once per BT.
//   - Pagination mirrors data_table.list (keyset-sorted by ObjectPath, base64 cursor with
//     filter_hash for staleness).
//   - Empty result is a legitimate state (project has no UBehaviorTree assets yet) — surfaced as
//     behavior_trees=[] with total_known=0, NOT as -32603.
FMCPResponse Tool_ListAssets(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString PathPrefix;
	if (Request.Args.IsValid()) { Request.Args->TryGetStringField(TEXT("path_prefix"), PathPrefix); }

	int32 PageSize = 100;
	if (Request.Args.IsValid()) { Request.Args->TryGetNumberField(TEXT("page_size"), PageSize); }
	PageSize = FMath::Clamp(PageSize, 1, 1000);

	FString PageToken;
	if (Request.Args.IsValid()) { Request.Args->TryGetStringField(TEXT("page_token"), PageToken); }

	// Hash the filter shape so the cursor can detect mid-pagination filter mutations.
	const uint32 FilterHash = GetTypeHash(PathPrefix);

	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FARFilter Filter;
	Filter.ClassPaths.Add(UBehaviorTree::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = false;
	Filter.bRecursivePaths   = true;
	if (!PathPrefix.IsEmpty())
	{
		Filter.PackagePaths.Add(*PathPrefix);
	}
	TArray<FAssetData> Assets;
	AR.GetAssets(Filter, Assets);

	// Stable sort by ObjectPath (the keyset-pagination sort key — same convention as data_table.list).
	Assets.Sort([](const FAssetData& A, const FAssetData& B)
	{
		return A.GetSoftObjectPath().ToString() < B.GetSoftObjectPath().ToString();
	});

	// Decode + validate cursor.
	int32 StartIdx = 0;
	FMCPPageCursor InCursor;
	if (!PageToken.IsEmpty())
	{
		FString DecodeErr;
		if (!FMCPPageCursorUtils::Decode(PageToken, InCursor, DecodeErr))
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
				FString::Printf(TEXT("invalid page_token: %s"), *DecodeErr));
		}
		if (!FMCPPageCursorUtils::ValidateAgainstFilter(InCursor, FilterHash))
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorStaleCursor,
				TEXT("filter mutated between pages (path_prefix changed); restart pagination"));
		}
		while (StartIdx < Assets.Num() &&
			   Assets[StartIdx].GetSoftObjectPath().ToString() <= InCursor.LastAssetPath)
		{
			++StartIdx;
		}
	}

	TArray<TSharedPtr<FJsonValue>> BTArr;
	const int32 EndIdx = FMath::Min(StartIdx + PageSize, Assets.Num());
	BTArr.Reserve(EndIdx - StartIdx);
	for (int32 i = StartIdx; i < EndIdx; ++i)
	{
		const FAssetData& A = Assets[i];
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("asset_path"), A.GetSoftObjectPath().ToString());

		// Load the BT to read blackboard + root-class fields. AssetRegistry tags do NOT expose
		// these on UBehaviorTree (no equivalent of UDataTable's "RowStructure" AR tag). Load cost
		// is amortised across calls — UE caches the loaded UObject.
		if (UBehaviorTree* BT = Cast<UBehaviorTree>(A.GetAsset()))
		{
			if (BT->BlackboardAsset)
			{
				Obj->SetStringField(TEXT("blackboard_asset_path"),
					BT->BlackboardAsset->GetPathName());
			}
			if (BT->RootNode)
			{
				Obj->SetStringField(TEXT("root_node_class"),
					BT->RootNode->GetClass()->GetName());
			}
		}
		// If GetAsset() returned null (rare — could happen for an in-progress redirector), fall
		// through with just asset_path. Caller branches on the absence of optional fields.

		BTArr.Add(MakeShared<FJsonValueObject>(Obj));
	}

	const int32 TotalKnown = Assets.Num();
	const bool bHasNextPage = (EndIdx < TotalKnown && EndIdx > 0);
	return FMCPJsonBuilder()
		.Arr(TEXT("behavior_trees"), MoveTemp(BTArr))
		.Num(TEXT("total_known"), TotalKnown)
		.If(bHasNextPage, [&](FMCPJsonBuilder& B)
		{
			FMCPPageCursor OutCursor;
			OutCursor.FilterHash = FilterHash;
			OutCursor.LastAssetPath = Assets[EndIdx - 1].GetSoftObjectPath().ToString();
			OutCursor.TotalKnownSnapshot = TotalKnown;
			B.Str(TEXT("next_page_token"), FMCPPageCursorUtils::Encode(OutCursor));
		})
		.BuildSuccess(Request);
}

// ─── ai.bt.get_nodes ───────────────────────────────────────────────────────────────────────────
//
// Args:
//   bt_path: string   required — UBehaviorTree asset path
//
// Result:
//   {
//     bt_path : string,                full asset path of the resolved tree
//     blackboard_asset_path? : string, when BlackboardAsset is non-null
//     root: {
//       node_class      : string,
//       node_name       : string,
//       execution_index : int,
//       children: [
//         {
//           node: { node_class, node_name, execution_index, children?, services? },
//           decorators: [{ node_class, node_name, execution_index }, ...]
//         }, ...
//       ],
//       services: [{ node_class, node_name, execution_index }, ...]
//     }
//   }
//
// Errors:
//   -32004 ObjectNotFound      bt_path not loadable
//   -32010 InvalidPath         malformed bt_path
//   -32011 WrongClass          path resolved but is not a UBehaviorTree
//   -32602 InvalidParams       missing bt_path
//
// Notes on null root:
//   - BT loaded with RootNode == null (newly-created / unedited tree) is NOT an error. We surface
//     a success response with root=null rather than -32603; the caller branches on the null.
//
// Notes:
//   - Recursive walk via AIBT_NodeToJson, depth-capped at kAIBTMaxRecursionDepth (64). Real BTs
//     are flat (rarely deeper than 6-8 levels) so this is purely a corruption guard.
//   - UBTDecorator instances are NOT serialized as children of the gated node — they live ON the
//     PARENT composite's Children[].Decorators array (BT decorators attach to child edges, not
//     child nodes). The JSON shape reflects that: each child entry under "children" carries its
//     own "decorators" sub-array.
//   - UBTService instances are composite-level: serialized under "services" on the composite
//     node, NOT per-child.
//   - Task nodes (UBTTaskNode) are leaves; their "children" array is always empty (preserved in
//     output so callers can do uniform shape-matching).
FMCPResponse Tool_GetNodes(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString BTPath;
	FMCPResponse ArgErr;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("bt_path"), BTPath, ArgErr))
	{
		return ArgErr;
	}

	int32 ErrCode = 0;
	FString ErrMsg;
	UBehaviorTree* BT = FMCPAssetLoader::Load<UBehaviorTree>(BTPath, ErrCode, ErrMsg);
	if (!BT)
	{
		return FMCPToolHelpers::MakeError(Request, ErrCode, ErrMsg);
	}

	return FMCPJsonBuilder()
		.Str(TEXT("bt_path"), BT->GetPathName())
		.If(BT->BlackboardAsset != nullptr, [&](FMCPJsonBuilder& B)
		{
			B.Str(TEXT("blackboard_asset_path"), BT->BlackboardAsset->GetPathName());
		})
		.If(BT->RootNode != nullptr,
			[&](FMCPJsonBuilder& B) { B.ObjectShared(TEXT("root"), AIBT_NodeToJson(BT->RootNode, /*Depth*/ 0)); })
		.If(BT->RootNode == nullptr,
			// Newly-created BT with no root composite — legitimate state, surface as root=null.
			[](FMCPJsonBuilder& B) { B.Null(TEXT("root")); })
		.BuildSuccess(Request);
}

// ─── ai.bt.start_on_actor ──────────────────────────────────────────────────────────────────────
//
// Args:
//   actor_path: string   required — actor path (any FMCPActorPathUtils form)
//   bt_path   : string   required — UBehaviorTree asset path
//
// Result:
//   {
//     started         : bool,    true if RunBehaviorTree returned true (false typically means the
//                                AAIController already had an incompatible BT running OR the BT's
//                                blackboard couldn't bind)
//     controller_path : string,  full path of the resolved AAIController
//     bt_path         : string   echoed for round-trip confirmation
//   }
//
// Errors:
//   -32004 ObjectNotFound  actor_path / bt_path not resolvable
//   -32010 InvalidPath     malformed bt_path
//   -32011 WrongClass      bt_path is not UBehaviorTree; OR actor has no AAIController
//   -32602 InvalidParams   missing args
//
// Notes:
//   - NOT PIE-guarded by design — runtime BT iteration testing IS the use case. Editor-world
//     AAIController instances are atypical (pawn controllers spawn at PIE start), so on the
//     editor world this typically surfaces -32011 (actor has no controller) which is the right
//     diagnosis.
//   - AAIController::RunBehaviorTree returns false on several legitimate failure modes (no
//     blackboard binding, etc.). We surface that as started=false (success response with bool
//     field), not as an error code — the caller can distinguish "BT failed to start" from "tool
//     failed".
FMCPResponse Tool_StartOnActor(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString ActorPath;
	FMCPResponse ArgErr;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("actor_path"), ActorPath, ArgErr))
	{
		return ArgErr;
	}
	FString BTPath;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("bt_path"), BTPath, ArgErr))
	{
		return ArgErr;
	}

	// Resolve BT first so the user gets the asset-side diagnosis before the actor-side one. Order
	// is cheap to swap; chose this purely for caller UX (an invalid BT path is more likely than an
	// invalid actor path in editor scripting).
	int32 BTErrCode = 0;
	FString BTErrMsg;
	UBehaviorTree* BT = FMCPAssetLoader::Load<UBehaviorTree>(BTPath, BTErrCode, BTErrMsg);
	if (!BT)
	{
		return FMCPToolHelpers::MakeError(Request, BTErrCode, BTErrMsg);
	}

	int32 AICErrCode = 0;
	FString AICErrMsg;
	AAIController* AIC = AIBT_ResolveAIController(ActorPath, AICErrCode, AICErrMsg);
	if (!AIC)
	{
		return FMCPToolHelpers::MakeError(Request, AICErrCode, AICErrMsg);
	}

	const bool bStarted = AIC->RunBehaviorTree(BT);

	return FMCPJsonBuilder()
		.Bool(TEXT("started"), bStarted)
		.Str(TEXT("controller_path"), AIC->GetPathName())
		.Str(TEXT("bt_path"), BT->GetPathName())
		.BuildSuccess(Request);
}

// ─── ai.bt.stop_on_actor ───────────────────────────────────────────────────────────────────────
//
// Args:
//   actor_path: string   required — actor path (any FMCPActorPathUtils form)
//
// Result:
//   {
//     stopped         : bool,    true when we successfully called StopTree (always true if the
//                                BTComponent was non-null; reports false only when there was no
//                                BehaviorTreeComponent on the controller, which means nothing was
//                                running to stop)
//     controller_path : string,  full path of the resolved AAIController
//     prior_active_bt?: string   asset path of the BT that was running BEFORE StopTree (omitted
//                                when stopped=false OR when the component had no current tree)
//   }
//
// Errors:
//   -32004 ObjectNotFound  actor_path not resolvable
//   -32011 WrongClass      actor has no AAIController
//   -32602 InvalidParams   missing actor_path
//
// Notes:
//   - NOT PIE-guarded (same rationale as start_on_actor — runtime introspection is the use case).
//   - We snapshot GetCurrentTree() BEFORE calling StopTree so the response can report what was
//     stopped. Mode defaults to EBTStopMode::Safe (the BT clean-shutdown path) — matches the
//     engine's default. Forced stop would require a separate arg if the caller has a specific
//     use case for it; deferred to a future revision.
FMCPResponse Tool_StopOnActor(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString ActorPath;
	FMCPResponse ArgErr;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("actor_path"), ActorPath, ArgErr))
	{
		return ArgErr;
	}

	int32 AICErrCode = 0;
	FString AICErrMsg;
	AAIController* AIC = AIBT_ResolveAIController(ActorPath, AICErrCode, AICErrMsg);
	if (!AIC)
	{
		return FMCPToolHelpers::MakeError(Request, AICErrCode, AICErrMsg);
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("controller_path"), AIC->GetPathName());

	// Cast BrainComponent to UBehaviorTreeComponent — the only BT-aware brain subclass shipped by
	// the engine. Custom brain components (rare) won't have GetCurrentTree / StopTree and we
	// correctly report stopped=false.
	UBehaviorTreeComponent* BTC = Cast<UBehaviorTreeComponent>(AIC->BrainComponent);
	if (!BTC)
	{
		Out->SetBoolField(TEXT("stopped"), false);
		return FMCPToolHelpers::MakeSuccessObj(Request, Out);
	}

	UBehaviorTree* PriorTree = BTC->GetCurrentTree();
	BTC->StopTree(EBTStopMode::Safe);

	Out->SetBoolField(TEXT("stopped"), true);
	if (PriorTree)
	{
		Out->SetStringField(TEXT("prior_active_bt"), PriorTree->GetPathName());
	}
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── ai.bt.create_asset (Wave P) ───────────────────────────────────────────────────────────────
//
// Args:    { path: string, root_composite_class?: string,
//            blackboard_asset_path?: string }
// Result:  { asset_path, has_root, root_class? }
//
// Creates an empty UBehaviorTree at the supplied path. If ``root_composite_class`` is supplied
// (defaults to "/Script/AIModule.BTComposite_Selector"), a root composite is constructed and
// assigned to RootNode. Optional ``blackboard_asset_path`` links a UBlackboardData asset.
//
// Set ``root_composite_class`` to the literal empty string ``""`` to skip root creation entirely
// (use case: caller plans to add the root via ai.bt.add_node "Root" path immediately after).
//
// Errors:
//   -32010 InvalidPath          path malformed
//   -32014 PathInUse            path already exists
//   -32011 WrongClass           root_composite_class not a UBTCompositeNode subclass / abstract
//   -32020 ClassNotFound        root_composite_class did not resolve
//   -32023 InvalidClassPath     root_composite_class malformed
//   -32004 ObjectNotFound       blackboard_asset_path not loadable / wrong class
//   -32027 PIEActive
FMCPResponse Tool_CreateAsset(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_BTCreateAsset", "Create Behavior Tree"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString DestPathRaw;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("path"), DestPathRaw, Err)) { return Err; }

	const FString DestPathNorm = FMCPAssetPathUtils::Normalize(DestPathRaw);
	if (DestPathNorm.IsEmpty() || !FMCPAssetPathUtils::IsValidGameOrPlugin(DestPathNorm))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidPath,
			FString::Printf(TEXT("path '%s' malformed or unknown mount"), *DestPathRaw));
	}

	const FString PackagePath = FPaths::GetPath(DestPathNorm);
	const FString AssetName   = FPaths::GetBaseFilename(DestPathNorm);

	if (FPackageName::DoesPackageExist(DestPathNorm) ||
		FindObject<UObject>(nullptr, *(DestPathNorm + TEXT(".") + AssetName)) != nullptr)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPathInUse,
			FString::Printf(TEXT("path '%s' already exists"), *DestPathNorm));
	}

	// Determine root composite class: default Selector when key absent; skip root when ""
	// (empty string) supplied; resolve when non-empty.
	FString RootClassPath = TEXT("/Script/AIModule.BTComposite_Selector");
	bool bRootRequested = true;
	if (Request.Args.IsValid())
	{
		FString Provided;
		if (Request.Args->TryGetStringField(TEXT("root_composite_class"), Provided))
		{
			RootClassPath = Provided;
			bRootRequested = !Provided.IsEmpty();
		}
	}

	UClass* RootClass = nullptr;
	if (bRootRequested)
	{
		FString ResolveErr;
		RootClass = FMCPClassResolver::ResolveStrict(RootClassPath, UBTCompositeNode::StaticClass(), ResolveErr);
		if (!RootClass)
		{
			if (ResolveErr.Contains(TEXT("is not a subclass")))
			{
				return FMCPToolHelpers::MakeError(Request, kMCPErrorWrongClass, ResolveErr);
			}
			if (ResolveErr.Contains(TEXT("must start with '/'")))
			{
				return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidClassPath, ResolveErr);
			}
			return FMCPToolHelpers::MakeError(Request, kMCPErrorClassNotFound, ResolveErr);
		}
	}

	// Optional blackboard pre-resolve before mutating.
	UBlackboardData* BBAsset = nullptr;
	FString BBPath;
	if (Request.Args.IsValid() && Request.Args->TryGetStringField(TEXT("blackboard_asset_path"), BBPath)
		&& !BBPath.IsEmpty())
	{
		int32 LoadErr = 0;
		FString LoadMsg;
		BBAsset = FMCPAssetLoader::Load<UBlackboardData>(BBPath, LoadErr, LoadMsg);
		if (!BBAsset) { return FMCPToolHelpers::MakeError(Request, LoadErr, LoadMsg); }
	}

	UPackage* BTPkg = CreatePackage(*DestPathNorm);
	if (!BTPkg)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInternal,
			FString::Printf(TEXT("CreatePackage returned null for '%s'"), *DestPathNorm));
	}
	BTPkg->FullyLoad();

	UBehaviorTree* BT = NewObject<UBehaviorTree>(
		BTPkg, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
	if (!BT)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInternal,
			FString::Printf(TEXT("NewObject<UBehaviorTree> returned null for '%s'"), *DestPathNorm));
	}

	if (RootClass)
	{
		UBTCompositeNode* RootComp = NewObject<UBTCompositeNode>(
			BT, RootClass, NAME_None, RF_Public | RF_Transactional);
		check(RootComp);
		BT->RootNode = RootComp;
	}

	if (BBAsset) { BT->BlackboardAsset = BBAsset; }

	FAssetRegistryModule::AssetCreated(BT);
	Scope.DirtyPackage(BTPkg);

	return FMCPJsonBuilder()
		.Str (TEXT("asset_path"), BT->GetPathName())
		.Bool(TEXT("has_root"),   BT->RootNode != nullptr)
		.OptStr(TEXT("root_class"), BT->RootNode ? BT->RootNode->GetClass()->GetPathName() : FString())
		.OptStr(TEXT("blackboard_asset_path"), BBAsset ? BBAsset->GetPathName() : FString())
		.BuildSuccess(Request);
}

// ─── ai.bt.add_node (Wave P) ───────────────────────────────────────────────────────────────────
//
// Args:    { bt_path: string, parent_path: string, node_class: string, properties?: {...} }
// Result:  { added, node_path, node_class, parent_path, node_index, properties_applied,
//            properties_skipped }
//
// Two distinct paths:
//   (1) parent_path == "Root" AND BT.RootNode is null → ``node_class`` MUST be a UBTCompositeNode
//       subclass; the new composite BECOMES BT.RootNode (no FBTCompositeChild wrapper).
//   (2) parent_path resolves to an existing UBTCompositeNode → new node (composite OR task) is
//       appended to Parent->Children as an FBTCompositeChild slot. Composite goes in
//       slot.ChildComposite; Task goes in slot.ChildTask.
//
// ``node_class`` MUST be a concrete UBTNode subclass (UBTCompositeNode OR UBTTaskNode). Decorators
// and services are added via ai.bt.add_decorator / ai.bt.add_service — NOT via add_node.
//
// Errors:
//   -32011 WrongClass           node_class is not a UBTNode subclass / parent is not a composite
//   -32020 ClassNotFound        node_class did not resolve
//   -32023 InvalidClassPath     malformed node_class path
//   -32602 InvalidParams        parent_path Root with non-composite class when RootNode is null
//   -32027 PIEActive
FMCPResponse Tool_AddNode(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_BTAddNode", "Add BT Node"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString BTPath, ParentPath, NodeClassPath;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("bt_path"),     BTPath,         Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("parent_path"), ParentPath,     Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("node_class"),  NodeClassPath,  Err)) { return Err; }

	int32 LoadErr = 0;
	FString LoadMsg;
	UBehaviorTree* BT = FMCPAssetLoader::Load<UBehaviorTree>(BTPath, LoadErr, LoadMsg);
	if (!BT) { return FMCPToolHelpers::MakeError(Request, LoadErr, LoadMsg); }

	// Resolve node class. Must be UBTNode (will be further restricted to Composite/Task below).
	FString ResolveErr;
	UClass* NodeClass = FMCPClassResolver::ResolveStrict(NodeClassPath, UBTNode::StaticClass(), ResolveErr);
	if (!NodeClass)
	{
		if (ResolveErr.Contains(TEXT("is not a subclass")))
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorWrongClass, ResolveErr);
		}
		if (ResolveErr.Contains(TEXT("must start with '/'")))
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidClassPath, ResolveErr);
		}
		return FMCPToolHelpers::MakeError(Request, kMCPErrorClassNotFound, ResolveErr);
	}

	const bool bIsComposite = NodeClass->IsChildOf(UBTCompositeNode::StaticClass());
	const bool bIsTask      = NodeClass->IsChildOf(UBTTaskNode::StaticClass());
	if (!bIsComposite && !bIsTask)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorWrongClass,
			FString::Printf(TEXT("node_class '%s' must be UBTCompositeNode or UBTTaskNode "
				"(decorators/services use ai.bt.add_decorator / ai.bt.add_service)"),
				*NodeClass->GetPathName()));
	}

	BT->Modify();

	// Mutually exclusive cases: "Root" with empty RootNode, OR resolve to a composite parent.
	UBTNode* NewNode = nullptr;
	FString  NodePath;
	int32    NodeIdx = INDEX_NONE;

	const bool bParentIsRoot = ParentPath.Equals(TEXT("Root"), ESearchCase::IgnoreCase);

	if (bParentIsRoot && BT->RootNode == nullptr)
	{
		// Case 1: install the root composite.
		if (!bIsComposite)
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
				FString::Printf(TEXT("parent_path='Root' with empty tree requires node_class to be "
					"a UBTCompositeNode subclass; got '%s' which is a task"), *NodeClass->GetPathName()));
		}
		UBTCompositeNode* NewRoot = NewObject<UBTCompositeNode>(
			BT, NodeClass, NAME_None, RF_Public | RF_Transactional);
		check(NewRoot);
		BT->RootNode = NewRoot;
		NewNode  = NewRoot;
		NodePath = TEXT("Root");
		NodeIdx  = INDEX_NONE;  // Root has no parent index
	}
	else
	{
		// Case 2: resolve parent_path to a composite; append child.
		FAIBTNodeRef ParentRef;
		FString PathErr;
		if (!AIBT_ResolvePath(BT, ParentPath, ParentRef, PathErr))
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
				FString::Printf(TEXT("parent_path '%s' did not resolve: %s"), *ParentPath, *PathErr));
			}

		UBTCompositeNode* ParentComp = Cast<UBTCompositeNode>(ParentRef.Node);
		if (!ParentComp)
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorWrongClass,
				FString::Printf(TEXT("parent_path '%s' resolves to '%s', not a UBTCompositeNode (tasks/decorators/services cannot have children)"),
					*ParentPath, ParentRef.Node ? *ParentRef.Node->GetClass()->GetName() : TEXT("<null>")));
		}

		ParentComp->Modify();

		// Construct the new node. Both composites and tasks are owned by the BT asset (matches the
		// engine's serialization model — Children slot owns the pointer but the BT is the outer).
		UBTNode* Created = NewObject<UBTNode>(BT, NodeClass, NAME_None, RF_Public | RF_Transactional);
		check(Created);

		FBTCompositeChild NewSlot;
		if (bIsComposite)
		{
			NewSlot.ChildComposite = Cast<UBTCompositeNode>(Created);
			NewSlot.ChildTask = nullptr;
		}
		else
		{
			NewSlot.ChildComposite = nullptr;
			NewSlot.ChildTask = Cast<UBTTaskNode>(Created);
		}

		NodeIdx = ParentComp->Children.Add(NewSlot);
		NewNode = Created;
		NodePath = FString::Printf(TEXT("%s/Children[%d]"), *ParentPath, NodeIdx);
	}

	check(NewNode);

	// Apply optional properties to the new node.
	TArray<FString> PropsApplied, PropsSkipped;
	const TSharedPtr<FJsonObject>* PropsObj = nullptr;
	if (Request.Args.IsValid() && Request.Args->TryGetObjectField(TEXT("properties"), PropsObj)
		&& PropsObj && (*PropsObj).IsValid())
	{
		FMCPToolHelpers::ApplyJsonProperties(NewNode, *PropsObj, PropsApplied, PropsSkipped);
	}

	Scope.DirtyPackage(BT->GetPackage());

	FMCPJsonBuilder Builder;
	Builder
		.Bool(TEXT("added"),       true)
		.Str (TEXT("bt_path"),     BT->GetPathName())
		.Str (TEXT("node_path"),   NodePath)
		.Str (TEXT("node_class"),  NewNode->GetClass()->GetPathName())
		.Str (TEXT("parent_path"), ParentPath)
		.Int (TEXT("node_index"),  NodeIdx)
		.Bool(TEXT("is_composite"), bIsComposite);
	AIBT_AppendPropertyResults(Builder, PropsApplied, PropsSkipped);
	return Builder.BuildSuccess(Request);
}

// ─── ai.bt.add_decorator (Wave P) ──────────────────────────────────────────────────────────────
//
// Args:    { bt_path: string, node_path: string, decorator_class: string, properties?: {...} }
// Result:  { added, decorator_path, decorator_class, decorator_index, properties_applied,
//            properties_skipped }
//
// Appends a UBTDecorator to either:
//   - BT.RootDecorators[] when ``node_path`` == "Root"
//   - Parent->Children[X].Decorators[] when ``node_path`` == "Root/.../Children[X]"
//
// Decorators on tasks ARE valid in UE (a task is a Children[] entry; the decorators array hangs
// off the slot, not the task itself). Decorators on intermediate Decorator/Service paths are NOT
// valid — the decorator/service slot doesn't carry a Decorators array.
//
// Errors:
//   -32011 WrongClass            decorator_class not a UBTDecorator subclass; node_path resolves to
//                                a path that doesn't support decorators (decorator/service slot)
//   -32020 ClassNotFound         decorator_class did not resolve
//   -32023 InvalidClassPath      malformed decorator_class path
//   -32602 InvalidParams         node_path malformed
//   -32027 PIEActive
FMCPResponse Tool_AddDecorator(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_BTAddDecorator", "Add BT Decorator"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString BTPath, NodePath, DecClassPath;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("bt_path"),         BTPath,        Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("node_path"),       NodePath,      Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("decorator_class"), DecClassPath,  Err)) { return Err; }

	int32 LoadErr = 0;
	FString LoadMsg;
	UBehaviorTree* BT = FMCPAssetLoader::Load<UBehaviorTree>(BTPath, LoadErr, LoadMsg);
	if (!BT) { return FMCPToolHelpers::MakeError(Request, LoadErr, LoadMsg); }

	FString ResolveErr;
	UClass* DecClass = FMCPClassResolver::ResolveStrict(DecClassPath, UBTDecorator::StaticClass(), ResolveErr);
	if (!DecClass)
	{
		if (ResolveErr.Contains(TEXT("is not a subclass")))
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorWrongClass, ResolveErr);
		}
		if (ResolveErr.Contains(TEXT("must start with '/'")))
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidClassPath, ResolveErr);
		}
		return FMCPToolHelpers::MakeError(Request, kMCPErrorClassNotFound, ResolveErr);
	}

	// Resolve target path. Decorators attach to "Root" (RootDecorators) OR to a Children[] slot.
	FAIBTNodeRef TargetRef;
	FString PathErr;
	if (!AIBT_ResolvePath(BT, NodePath, TargetRef, PathErr))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			FString::Printf(TEXT("node_path '%s' did not resolve: %s"), *NodePath, *PathErr));
	}

	// Reject paths to existing decorators / services — decorators do not carry sub-decorators.
	if (TargetRef.Kind == EAIBTRefKind::Decorator || TargetRef.Kind == EAIBTRefKind::Service)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorWrongClass,
			FString::Printf(TEXT("node_path '%s' resolves to a decorator/service slot — decorators attach to Root or Children[] slots only"),
				*NodePath));
	}

	BT->Modify();

	UBTDecorator* NewDec = NewObject<UBTDecorator>(BT, DecClass, NAME_None, RF_Public | RF_Transactional);
	check(NewDec);

	// Apply properties BEFORE inserting so consumers can rely on the array having a fully-
	// initialized entry on iteration.
	TArray<FString> PropsApplied, PropsSkipped;
	const TSharedPtr<FJsonObject>* PropsObj = nullptr;
	if (Request.Args.IsValid() && Request.Args->TryGetObjectField(TEXT("properties"), PropsObj)
		&& PropsObj && (*PropsObj).IsValid())
	{
		FMCPToolHelpers::ApplyJsonProperties(NewDec, *PropsObj, PropsApplied, PropsSkipped);
	}

	int32 DecIdx = INDEX_NONE;
	FString DecPath;
	if (TargetRef.Kind == EAIBTRefKind::Root)
	{
		// "Root" → BT->RootDecorators
		DecIdx = BT->RootDecorators.Add(NewDec);
		DecPath = FString::Printf(TEXT("Root/Decorators[%d]"), DecIdx);
	}
	else
	{
		// Child slot → Parent->Children[ParentChildIndex].Decorators
		check(TargetRef.Kind == EAIBTRefKind::Child);
		check(TargetRef.Parent);
		check(TargetRef.Parent->Children.IsValidIndex(TargetRef.ParentChildIndex));

		TargetRef.Parent->Modify();
		FBTCompositeChild& ChildSlot = TargetRef.Parent->Children[TargetRef.ParentChildIndex];
		DecIdx = ChildSlot.Decorators.Add(NewDec);
		DecPath = FString::Printf(TEXT("%s/Decorators[%d]"), *NodePath, DecIdx);
	}

	Scope.DirtyPackage(BT->GetPackage());

	FMCPJsonBuilder Builder;
	Builder
		.Bool(TEXT("added"),           true)
		.Str (TEXT("bt_path"),         BT->GetPathName())
		.Str (TEXT("decorator_path"),  DecPath)
		.Str (TEXT("decorator_class"), DecClass->GetPathName())
		.Int (TEXT("decorator_index"), DecIdx)
		.Str (TEXT("node_path"),       NodePath);
	AIBT_AppendPropertyResults(Builder, PropsApplied, PropsSkipped);
	return Builder.BuildSuccess(Request);
}

// ─── ai.bt.add_service (Wave P) ────────────────────────────────────────────────────────────────
//
// Args:    { bt_path: string, node_path: string, service_class: string, properties?: {...} }
// Result:  { added, service_path, service_class, service_index, properties_applied,
//            properties_skipped }
//
// Appends a UBTService to Composite->Services[]. node_path MUST resolve to a composite (Root or
// nested). Tasks do NOT have a Services array — adding a service to a task path is rejected.
//
// Errors:
//   -32011 WrongClass            service_class not a UBTService subclass; node_path is a task/
//                                decorator/service (services attach to composites only)
//   -32020 ClassNotFound         service_class did not resolve
//   -32023 InvalidClassPath      malformed service_class path
//   -32602 InvalidParams         node_path malformed / unresolvable
//   -32027 PIEActive
FMCPResponse Tool_AddService(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_BTAddService", "Add BT Service"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString BTPath, NodePath, SvcClassPath;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("bt_path"),       BTPath,        Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("node_path"),     NodePath,      Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("service_class"), SvcClassPath,  Err)) { return Err; }

	int32 LoadErr = 0;
	FString LoadMsg;
	UBehaviorTree* BT = FMCPAssetLoader::Load<UBehaviorTree>(BTPath, LoadErr, LoadMsg);
	if (!BT) { return FMCPToolHelpers::MakeError(Request, LoadErr, LoadMsg); }

	FString ResolveErr;
	UClass* SvcClass = FMCPClassResolver::ResolveStrict(SvcClassPath, UBTService::StaticClass(), ResolveErr);
	if (!SvcClass)
	{
		if (ResolveErr.Contains(TEXT("is not a subclass")))
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorWrongClass, ResolveErr);
		}
		if (ResolveErr.Contains(TEXT("must start with '/'")))
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidClassPath, ResolveErr);
		}
		return FMCPToolHelpers::MakeError(Request, kMCPErrorClassNotFound, ResolveErr);
	}

	FAIBTNodeRef TargetRef;
	FString PathErr;
	if (!AIBT_ResolvePath(BT, NodePath, TargetRef, PathErr))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			FString::Printf(TEXT("node_path '%s' did not resolve: %s"), *NodePath, *PathErr));
	}

	// Service target MUST be a composite (Root composite OR a Child composite).
	UBTCompositeNode* TargetComp = Cast<UBTCompositeNode>(TargetRef.Node);
	if (!TargetComp)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorWrongClass,
			FString::Printf(TEXT("node_path '%s' resolves to '%s' — services attach to UBTCompositeNode only"),
				*NodePath, TargetRef.Node ? *TargetRef.Node->GetClass()->GetName() : TEXT("<null>")));
	}

	BT->Modify();
	TargetComp->Modify();

	UBTService* NewSvc = NewObject<UBTService>(BT, SvcClass, NAME_None, RF_Public | RF_Transactional);
	check(NewSvc);

	TArray<FString> PropsApplied, PropsSkipped;
	const TSharedPtr<FJsonObject>* PropsObj = nullptr;
	if (Request.Args.IsValid() && Request.Args->TryGetObjectField(TEXT("properties"), PropsObj)
		&& PropsObj && (*PropsObj).IsValid())
	{
		FMCPToolHelpers::ApplyJsonProperties(NewSvc, *PropsObj, PropsApplied, PropsSkipped);
	}

	const int32 SvcIdx = TargetComp->Services.Add(NewSvc);
	const FString SvcPath = FString::Printf(TEXT("%s/Services[%d]"), *NodePath, SvcIdx);

	Scope.DirtyPackage(BT->GetPackage());

	FMCPJsonBuilder Builder;
	Builder
		.Bool(TEXT("added"),         true)
		.Str (TEXT("bt_path"),       BT->GetPathName())
		.Str (TEXT("service_path"),  SvcPath)
		.Str (TEXT("service_class"), SvcClass->GetPathName())
		.Int (TEXT("service_index"), SvcIdx)
		.Str (TEXT("node_path"),     NodePath);
	AIBT_AppendPropertyResults(Builder, PropsApplied, PropsSkipped);
	return Builder.BuildSuccess(Request);
}

// ─── ai.bt.remove_node (Wave P) ────────────────────────────────────────────────────────────────
//
// Args:    { bt_path: string, node_path: string }
// Result:  { removed, removed_kind, node_class }
//
// Removes the node at the resolved path:
//   - "Root"           → clears BT.RootNode (sets to null; subtree GC-collected)
//   - Children[N]      → Parent->Children.RemoveAt(N) (subtree GC-collected)
//   - Decorators[N]    → BT.RootDecorators.RemoveAt or Children[X].Decorators.RemoveAt
//   - Services[N]      → Composite->Services.RemoveAt
//
// NOTE: removing Children[N] shifts subsequent siblings — paths held by the caller pointing at
// later siblings become invalid. Caller's responsibility to re-fetch via ai.bt.get_nodes if
// further mutations are planned.
//
// Errors:
//   -32602 InvalidParams        node_path malformed / unresolvable / Root cleared when already null
//   -32027 PIEActive
FMCPResponse Tool_RemoveNode(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_BTRemoveNode", "Remove BT Node"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString BTPath, NodePath;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("bt_path"),   BTPath,   Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("node_path"), NodePath, Err)) { return Err; }

	int32 LoadErr = 0;
	FString LoadMsg;
	UBehaviorTree* BT = FMCPAssetLoader::Load<UBehaviorTree>(BTPath, LoadErr, LoadMsg);
	if (!BT) { return FMCPToolHelpers::MakeError(Request, LoadErr, LoadMsg); }

	FAIBTNodeRef Ref;
	FString PathErr;
	if (!AIBT_ResolvePath(BT, NodePath, Ref, PathErr))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			FString::Printf(TEXT("node_path '%s' did not resolve: %s"), *NodePath, *PathErr));
	}

	// "Root" with null RootNode is a no-op (idempotent — caller can call remove("Root") twice safely).
	if (Ref.Kind == EAIBTRefKind::Root && !Ref.Node)
	{
		return FMCPJsonBuilder()
			.Bool(TEXT("removed"),      false)
			.Str (TEXT("bt_path"),      BT->GetPathName())
			.Str (TEXT("node_path"),    NodePath)
			.Str (TEXT("removed_kind"), TEXT("Root"))
			.BuildSuccess(Request);
	}

	const FString PriorClassName = Ref.Node ? Ref.Node->GetClass()->GetPathName() : FString();
	const TCHAR*  KindStr        = TEXT("Unknown");

	BT->Modify();

	switch (Ref.Kind)
	{
		case EAIBTRefKind::Root:
			// Drop the root composite — subtree (Children/Decorators/Services owned by composites)
			// becomes unreachable from BT, GC collects on next sweep.
			BT->RootNode = nullptr;
			KindStr = TEXT("Root");
			break;

		case EAIBTRefKind::Child:
		{
			check(Ref.Parent);
			check(Ref.Parent->Children.IsValidIndex(Ref.SubIndex));
			Ref.Parent->Modify();
			Ref.Parent->Children.RemoveAt(Ref.SubIndex);
			KindStr = TEXT("Child");
			break;
		}

		case EAIBTRefKind::Decorator:
		{
			if (Ref.ParentChildIndex == INDEX_NONE)
			{
				// Root-level decorator
				check(BT->RootDecorators.IsValidIndex(Ref.SubIndex));
				BT->RootDecorators.RemoveAt(Ref.SubIndex);
			}
			else
			{
				check(Ref.Parent);
				check(Ref.Parent->Children.IsValidIndex(Ref.ParentChildIndex));
				Ref.Parent->Modify();
				FBTCompositeChild& ChildSlot = Ref.Parent->Children[Ref.ParentChildIndex];
				check(ChildSlot.Decorators.IsValidIndex(Ref.SubIndex));
				ChildSlot.Decorators.RemoveAt(Ref.SubIndex);
			}
			KindStr = TEXT("Decorator");
			break;
		}

		case EAIBTRefKind::Service:
		{
			check(Ref.Parent);
			check(Ref.Parent->Services.IsValidIndex(Ref.SubIndex));
			Ref.Parent->Modify();
			Ref.Parent->Services.RemoveAt(Ref.SubIndex);
			KindStr = TEXT("Service");
			break;
		}
	}

	Scope.DirtyPackage(BT->GetPackage());

	return FMCPJsonBuilder()
		.Bool(TEXT("removed"),      true)
		.Str (TEXT("bt_path"),      BT->GetPathName())
		.Str (TEXT("node_path"),    NodePath)
		.Str (TEXT("removed_kind"), KindStr)
		.OptStr(TEXT("node_class"), PriorClassName)
		.BuildSuccess(Request);
}

// ─── ai.bt.set_node_property (Wave P) ──────────────────────────────────────────────────────────
//
// Args:    { bt_path: string, node_path: string, property_path: string, value: <any> }
// Result:  { set, node_path, property_path }
//
// Generic property setter on any BT node (composite, task, decorator, service). Delegates to
// FMCPReflection::ResolvePropertyPath → WritePropertyValueAt. Property path is dotted (e.g.
// "BlackboardKey.SelectedKeyName" for the Blackboard decorator's selector).
//
// Errors:
//   -32004 ObjectNotFound       resolved root container is null
//   -32005 PropertyNotFound     property path segment doesn't exist
//   -32006 PropertyTypeMismatch  value cannot be written to the property
//   -32007 PropertyAccessDenied  property is editor-locked
//   -32602 InvalidParams        node_path malformed
//   -32027 PIEActive
FMCPResponse Tool_SetNodeProperty(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_BTSetNodeProp", "Set BT Node Property"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString BTPath, NodePath, PropertyPath;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("bt_path"),       BTPath,       Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("node_path"),     NodePath,     Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("property_path"), PropertyPath, Err)) { return Err; }

	// "value" must be present (callers can pass null explicitly to clear an object/class field).
	if (!Request.Args.IsValid() || !Request.Args->HasField(TEXT("value")))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			TEXT("missing required field 'value' (use JSON null to clear Object/Class properties)"));
	}
	const TSharedPtr<FJsonValue> ValueField = Request.Args->TryGetField(TEXT("value"));

	int32 LoadErr = 0;
	FString LoadMsg;
	UBehaviorTree* BT = FMCPAssetLoader::Load<UBehaviorTree>(BTPath, LoadErr, LoadMsg);
	if (!BT) { return FMCPToolHelpers::MakeError(Request, LoadErr, LoadMsg); }

	FAIBTNodeRef Ref;
	FString PathErr;
	if (!AIBT_ResolvePath(BT, NodePath, Ref, PathErr))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			FString::Printf(TEXT("node_path '%s' did not resolve: %s"), *NodePath, *PathErr));
	}
	if (!Ref.Node)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("node_path '%s' resolved to a null slot"), *NodePath));
	}

	// Walk the property path on the resolved node.
	UObject* Container = nullptr;
	void* ValuePtr = nullptr;
	FProperty* LeafProp = nullptr;
	int32 ResolveErrCode = 0;
	FString ResolveErrMsg;
	if (!FMCPReflection::ResolvePropertyPath(Ref.Node, PropertyPath,
		Container, ValuePtr, LeafProp, ResolveErrCode, ResolveErrMsg))
	{
		return FMCPToolHelpers::MakeError(Request, ResolveErrCode, ResolveErrMsg);
	}

	// Edit-const gate (matches the FMCPWritePropertyScope contract).
	if (LeafProp->HasAnyPropertyFlags(CPF_EditConst | CPF_BlueprintReadOnly | CPF_DisableEditOnInstance))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPropertyAccessDenied,
			FString::Printf(TEXT("property '%s' on '%s' is editor-locked (EditConst / BlueprintReadOnly)"),
				*PropertyPath, *Ref.Node->GetClass()->GetName()));
	}

	BT->Modify();
	Ref.Node->Modify();

	FString WriteErr;
	if (!FMCPReflection::WritePropertyValueAt(LeafProp, ValuePtr, ValueField, Ref.Node, WriteErr))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPropertyTypeMismatch,
			FString::Printf(TEXT("write rejected for '%s': %s"), *PropertyPath, *WriteErr));
	}

	Scope.DirtyPackage(BT->GetPackage());

	return FMCPJsonBuilder()
		.Bool(TEXT("set"),           true)
		.Str (TEXT("bt_path"),       BT->GetPathName())
		.Str (TEXT("node_path"),     NodePath)
		.Str (TEXT("property_path"), PropertyPath)
		.BuildSuccess(Request);
}

// ─── Registration ──────────────────────────────────────────────────────────────────────────────
void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	RegisterTool(TEXT("ai.bt.list_assets"),       &Tool_ListAssets,       /*Lane A*/ false);
	RegisterTool(TEXT("ai.bt.get_nodes"),         &Tool_GetNodes,         /*Lane A*/ false);
	RegisterTool(TEXT("ai.bt.start_on_actor"),    &Tool_StartOnActor,     /*Lane A*/ false);
	RegisterTool(TEXT("ai.bt.stop_on_actor"),     &Tool_StopOnActor,      /*Lane A*/ false);
	RegisterTool(TEXT("ai.bt.create_asset"),      &Tool_CreateAsset,      /*Lane A*/ false);
	RegisterTool(TEXT("ai.bt.add_node"),          &Tool_AddNode,          /*Lane A*/ false);
	RegisterTool(TEXT("ai.bt.add_decorator"),     &Tool_AddDecorator,     /*Lane A*/ false);
	RegisterTool(TEXT("ai.bt.add_service"),       &Tool_AddService,       /*Lane A*/ false);
	RegisterTool(TEXT("ai.bt.remove_node"),       &Tool_RemoveNode,       /*Lane A*/ false);
	RegisterTool(TEXT("ai.bt.set_node_property"), &Tool_SetNodeProperty,  /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("AIBehaviorTree surface registered: 10 ai.bt.* handlers "
			 "(list_assets / get_nodes / start_on_actor / stop_on_actor + Wave P: create_asset / "
			 "add_node / add_decorator / add_service / remove_node / set_node_property), all Lane A"));
}

} // namespace FAIBehaviorTreeTools

// Wave I refactor 2026-05: auto-registration via FMCPSurfaceRegistry replaces the
// manual include + Register call in UnrealMCPBridge.cpp.
#include "MCPSurfaceRegistry.h"
MCP_REGISTER_SURFACE(AIBehaviorTreeTools, &FAIBehaviorTreeTools::Register)

#undef LOCTEXT_NAMESPACE
