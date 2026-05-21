// Copyright FatumGame. All Rights Reserved.

#include "HierarchyTools.h"

#include "FMCPDispatchQueue.h"
#include "MCPMutatorScope.h"
#include "MCPToolHelpers.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPActorPathUtils.h"
#include "Utils/MCPWorldContext.h"

#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "UObject/Package.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	/**
	 * Parse a wire rule string into EAttachmentRule. SnapToTarget is permitted.
	 * Returns false + populates OutError on unknown string. Empty input → KeepRelative (caller
	 * default).
	 */
	bool HRC_ParseAttachRule(const FString& Raw, EAttachmentRule& OutRule, FString& OutError)
	{
		if (Raw.IsEmpty())      { OutRule = EAttachmentRule::KeepRelative; return true; }
		if (Raw == TEXT("keep_relative")) { OutRule = EAttachmentRule::KeepRelative; return true; }
		if (Raw == TEXT("keep_world"))    { OutRule = EAttachmentRule::KeepWorld;    return true; }
		if (Raw == TEXT("snap"))          { OutRule = EAttachmentRule::SnapToTarget; return true; }
		OutError = FString::Printf(TEXT("unknown attach rule '%s' (expected keep_relative|keep_world|snap)"), *Raw);
		return false;
	}

	/**
	 * Parse a wire rule string into EDetachmentRule. SnapToTarget is REJECTED (no detach analogue
	 * — the engine's FDetachmentTransformRules only models KeepRelative / KeepWorld).
	 */
	bool HRC_ParseDetachRule(const FString& Raw, EDetachmentRule& OutRule, FString& OutError)
	{
		if (Raw.IsEmpty())      { OutRule = EDetachmentRule::KeepWorld; return true; }
		if (Raw == TEXT("keep_relative")) { OutRule = EDetachmentRule::KeepRelative; return true; }
		if (Raw == TEXT("keep_world"))    { OutRule = EDetachmentRule::KeepWorld;    return true; }
		if (Raw == TEXT("snap"))
		{
			OutError = TEXT("detach rule 'snap' is not supported — use 'keep_world' (preserves world transform) or 'keep_relative' (preserves relative-to-prior-parent transform)");
			return false;
		}
		OutError = FString::Printf(TEXT("unknown detach rule '%s' (expected keep_relative|keep_world)"), *Raw);
		return false;
	}

	/** Queue dirty for the actor's package (external-package-aware: WorldPartition one-file-per-actor). */
	void HRC_QueueActorDirty(FMCPMutatorScope& Scope, AActor* Actor)
	{
		check(Actor);
		if (UPackage* ExternalPkg = Actor->GetExternalPackage())
		{
			Scope.DirtyPackage(ExternalPkg);
		}
		else
		{
			Scope.DirtyPackage(Actor->GetOutermost());
		}
	}

	/** Read an optional string field; absent → empty string (NOT an error). */
	FString HRC_GetOptionalString(const FMCPRequest& Request, const TCHAR* FieldName)
	{
		FString Out;
		if (Request.Args.IsValid()) { Request.Args->TryGetStringField(FieldName, Out); }
		return Out;
	}
} // namespace

namespace FHierarchyTools
{

// ─── hierarchy.attach ─────────────────────────────────────────────────────────────────────────
//
// Args:    { child_actor: string,
//            parent_actor: string,
//            socket_name?: string (default NAME_None),
//            location_rule?: "keep_relative"|"keep_world"|"snap" (default keep_relative),
//            rotation_rule?: same enum,
//            scale_rule?:    same enum,
//            weld_simulated_bodies?: bool (default false) }
// Result:  { child: string (pathname), parent: string (pathname), socket: string,
//            prior_parent?: string, world_path: string }
//
// PIE-guarded. FScopedTransaction wraps the AttachToActor call. The CHILD actor's package is
// marked dirty (external-package-aware) so the editor records the modification for saving.
FMCPResponse Tool_Attach(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_HierarchyAttach", "Attach Actor"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString ChildPath, ParentPath;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("child_actor"),  ChildPath,  Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("parent_actor"), ParentPath, Err)) { return Err; }

	// Resolve both actors. Both mutate-side semantically (the attach edge is on the child but the
	// parent's attach-children list also gains an entry) — reject PIE on both to be safe.
	bool bAmbiguous = false;
	FString AmbiguityHint, ResolveErr;
	AActor* Child = FMCPActorPathUtils::ResolveActor(ChildPath, /*bRejectPIE*/ true,
		bAmbiguous, AmbiguityHint, ResolveErr);
	if (!Child)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("child actor '%s' not found: %s"), *ChildPath, *ResolveErr));
	}

	AActor* Parent = FMCPActorPathUtils::ResolveActor(ParentPath, /*bRejectPIE*/ true,
		bAmbiguous, AmbiguityHint, ResolveErr);
	if (!Parent)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("parent actor '%s' not found: %s"), *ParentPath, *ResolveErr));
	}

	if (Child == Parent)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			TEXT("cannot attach an actor to itself"));
	}

	// Parse per-axis rules (default keep_relative when field absent).
	EAttachmentRule LocRule, RotRule, ScaleRule;
	FString ParseErr;
	if (!HRC_ParseAttachRule(HRC_GetOptionalString(Request, TEXT("location_rule")), LocRule, ParseErr))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			FString::Printf(TEXT("location_rule: %s"), *ParseErr));
	}
	if (!HRC_ParseAttachRule(HRC_GetOptionalString(Request, TEXT("rotation_rule")), RotRule, ParseErr))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			FString::Printf(TEXT("rotation_rule: %s"), *ParseErr));
	}
	if (!HRC_ParseAttachRule(HRC_GetOptionalString(Request, TEXT("scale_rule")), ScaleRule, ParseErr))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			FString::Printf(TEXT("scale_rule: %s"), *ParseErr));
	}

	bool bWeldSimulatedBodies = false;
	if (Request.Args.IsValid()) { Request.Args->TryGetBoolField(TEXT("weld_simulated_bodies"), bWeldSimulatedBodies); }

	const FString SocketName = HRC_GetOptionalString(Request, TEXT("socket_name"));
	const FName SocketFName = SocketName.IsEmpty() ? NAME_None : FName(*SocketName);

	// Capture prior parent for the response payload (helpful for diff visualization / undo UX).
	AActor* PriorParent = Child->GetAttachParentActor();

	const FAttachmentTransformRules Rules(LocRule, RotRule, ScaleRule, bWeldSimulatedBodies);

	Child->Modify();
	const bool bAttached = Child->AttachToActor(Parent, Rules, SocketFName);

	if (!bAttached)
	{
		// AttachToActor returns false when the child has no RootComponent OR an internal
		// USceneComponent::AttachToComponent cycle/permission check fails. Abort the transaction
		// so Ctrl-Z doesn't replay a no-op.
		Scope.Abort();
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			FString::Printf(
				TEXT("AttachToActor refused for child '%s' to parent '%s' (root component missing, ")
				TEXT("cycle detected, or unsupported root type)"),
				*Child->GetPathName(), *Parent->GetPathName()));
	}

	HRC_QueueActorDirty(Scope, Child);

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("child"),  Child->GetPathName());
	Out->SetStringField(TEXT("parent"), Parent->GetPathName());
	Out->SetStringField(TEXT("socket"), SocketFName.ToString());
	if (PriorParent) { Out->SetStringField(TEXT("prior_parent"), PriorParent->GetPathName()); }
	Out->SetStringField(TEXT("world_path"),
		Child->GetWorld() ? Child->GetWorld()->GetPathName() : FString());
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── hierarchy.detach ─────────────────────────────────────────────────────────────────────────
//
// Args:    { child_actor: string,
//            location_rule?: "keep_relative"|"keep_world" (default keep_world),
//            rotation_rule?: same,
//            scale_rule?:    same }
// Result:  { child: string, prior_parent?: string, prior_socket?: string, world_path: string }
//
// PIE-guarded. Detaching an actor that has no current parent is a SILENT no-op (no error) —
// matches engine semantics where DetachFromActor on an unparented USceneComponent is harmless.
// prior_parent / prior_socket are omitted in that case.
FMCPResponse Tool_Detach(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_HierarchyDetach", "Detach Actor"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString ChildPath;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("child_actor"), ChildPath, Err)) { return Err; }

	bool bAmbiguous = false;
	FString AmbiguityHint, ResolveErr;
	AActor* Child = FMCPActorPathUtils::ResolveActor(ChildPath, /*bRejectPIE*/ true,
		bAmbiguous, AmbiguityHint, ResolveErr);
	if (!Child)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("child actor '%s' not found: %s"), *ChildPath, *ResolveErr));
	}

	// Parse per-axis rules (default keep_world when field absent).
	EDetachmentRule LocRule, RotRule, ScaleRule;
	FString ParseErr;
	if (!HRC_ParseDetachRule(HRC_GetOptionalString(Request, TEXT("location_rule")), LocRule, ParseErr))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			FString::Printf(TEXT("location_rule: %s"), *ParseErr));
	}
	if (!HRC_ParseDetachRule(HRC_GetOptionalString(Request, TEXT("rotation_rule")), RotRule, ParseErr))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			FString::Printf(TEXT("rotation_rule: %s"), *ParseErr));
	}
	if (!HRC_ParseDetachRule(HRC_GetOptionalString(Request, TEXT("scale_rule")), ScaleRule, ParseErr))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			FString::Printf(TEXT("scale_rule: %s"), *ParseErr));
	}

	AActor* PriorParent = Child->GetAttachParentActor();
	const FName PriorSocket = Child->GetAttachParentSocketName();

	const FDetachmentTransformRules Rules(LocRule, RotRule, ScaleRule, /*bCallModify*/ true);

	Child->Modify();
	Child->DetachFromActor(Rules);

	HRC_QueueActorDirty(Scope, Child);

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("child"), Child->GetPathName());
	if (PriorParent)
	{
		Out->SetStringField(TEXT("prior_parent"), PriorParent->GetPathName());
		Out->SetStringField(TEXT("prior_socket"), PriorSocket.ToString());
	}
	Out->SetStringField(TEXT("world_path"),
		Child->GetWorld() ? Child->GetWorld()->GetPathName() : FString());
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── hierarchy.list_children ──────────────────────────────────────────────────────────────────
//
// Args:    { actor_path: string, recursive?: bool (default false) }
// Result:  { actor: string, recursive: bool, world_path: string,
//            children: [{ actor_path, attach_socket?, label }] }
//
// Read-only — no PIE guard. recursive=true walks the full subtree
// (AActor::GetAttachedActors(_, _, /*bRecursivelyIncludeAttachedActors*/ true)). attach_socket is
// emitted only when the child reports a non-NAME_None socket (mirrors hierarchy.detach output).
FMCPResponse Tool_ListChildren(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString ActorPath;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("actor_path"), ActorPath, Err)) { return Err; }

	bool bRecursive = false;
	if (Request.Args.IsValid()) { Request.Args->TryGetBoolField(TEXT("recursive"), bRecursive); }

	bool bAmbiguous = false;
	FString AmbiguityHint, ResolveErr;
	AActor* Actor = FMCPActorPathUtils::ResolveActor(ActorPath, /*bRejectPIE*/ false,
		bAmbiguous, AmbiguityHint, ResolveErr);
	if (!Actor)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("actor '%s' not found: %s"), *ActorPath, *ResolveErr));
	}

	TArray<AActor*> Children;
	Actor->GetAttachedActors(Children, /*bResetArray*/ true, /*bRecursivelyIncludeAttachedActors*/ bRecursive);

	TArray<TSharedPtr<FJsonValue>> ChildArray;
	ChildArray.Reserve(Children.Num());
	for (AActor* ChildActor : Children)
	{
		if (!ChildActor) { continue; }

		TSharedRef<FJsonObject> ChildObj = MakeShared<FJsonObject>();
		ChildObj->SetStringField(TEXT("actor_path"), ChildActor->GetPathName());
		ChildObj->SetStringField(TEXT("label"),      ChildActor->GetActorLabel());

		const FName Socket = ChildActor->GetAttachParentSocketName();
		if (Socket != NAME_None)
		{
			ChildObj->SetStringField(TEXT("attach_socket"), Socket.ToString());
		}
		ChildArray.Add(MakeShared<FJsonValueObject>(ChildObj));
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("actor"),     Actor->GetPathName());
	Out->SetBoolField  (TEXT("recursive"), bRecursive);
	Out->SetStringField(TEXT("world_path"),
		Actor->GetWorld() ? Actor->GetWorld()->GetPathName() : FString());
	Out->SetArrayField (TEXT("children"),  ChildArray);
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	RegisterTool(TEXT("hierarchy.attach"),        &Tool_Attach,       /*Lane A*/ false);
	RegisterTool(TEXT("hierarchy.detach"),        &Tool_Detach,       /*Lane A*/ false);
	RegisterTool(TEXT("hierarchy.list_children"), &Tool_ListChildren, /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("Hierarchy surface registered: 3 hierarchy.* tools (attach + detach + list_children), all Lane A"));
}

} // namespace FHierarchyTools

#undef LOCTEXT_NAMESPACE
