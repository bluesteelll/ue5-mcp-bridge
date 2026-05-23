// Copyright FatumGame. All Rights Reserved.

#include "ComponentTools.h"

#include "MCPSurfaceRegistry.h"

#include "FMCPDispatchQueue.h"
#include "MCPJsonBuilder.h"
#include "MCPMutatorScope.h"
#include "MCPToolHelpers.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPActorPathUtils.h"
#include "Utils/MCPComponentPathUtils.h"
#include "Utils/MCPReflection.h"
#include "Utils/MCPWorldContext.h"

#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/Level.h"
#include "GameFramework/Actor.h"
#include "Math/Rotator.h"
#include "Math/Vector.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// CMP_ prefix per the unity-build symbol-collision pattern. StampIds / MakeError / MakeSuccessObj
	// migrated to FMCPToolHelpers in Phase 3 (Group G3); only the surface-local error-code aliases
	// and the CMP_MakePIEError shim (still used by Tool_SetProperty which owns its own transaction
	// via FMCPWritePropertyScope and can't share FMCPMutatorScope's RAII) live here.
	constexpr int32 kCMPErrorInvalidParams = kMCPErrorInvalidParams; // -32602
	constexpr int32 kCMPErrorInternal      = kMCPErrorInternal;      // -32603

	/** Frozen PIE-mutator refusal (per D10 — smoke asserts both "Phase 5" and "pie." substrings). */
	FMCPResponse CMP_MakePIEError(const FMCPRequest& Request)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPIEActive, kMCPMessagePIEActive);
	}

	// ─── arg parsing helpers ─────────────────────────────────────────────────────────────────────

	/** Vector reader — accepts {x,y,z} shorthand OR {_kind:"Vector",x,y,z}. */
	bool CMP_ReadJsonVector(const TSharedPtr<FJsonObject>& Obj, FVector& OutV)
	{
		if (!Obj.IsValid()) { return false; }
		double X = 0.0, Y = 0.0, Z = 0.0;
		const bool bHasX = Obj->TryGetNumberField(TEXT("x"), X);
		const bool bHasY = Obj->TryGetNumberField(TEXT("y"), Y);
		const bool bHasZ = Obj->TryGetNumberField(TEXT("z"), Z);
		if (!bHasX && !bHasY && !bHasZ) { return false; }
		OutV = FVector(X, Y, Z);
		return true;
	}

	/** Rotator reader — accepts {pitch,yaw,roll} OR {_kind:"Rotator",pitch,yaw,roll}. */
	bool CMP_ReadJsonRotator(const TSharedPtr<FJsonObject>& Obj, FRotator& OutR)
	{
		if (!Obj.IsValid()) { return false; }
		double Pitch = 0.0, Yaw = 0.0, Roll = 0.0;
		const bool bHasP = Obj->TryGetNumberField(TEXT("pitch"), Pitch);
		const bool bHasY = Obj->TryGetNumberField(TEXT("yaw"), Yaw);
		const bool bHasR = Obj->TryGetNumberField(TEXT("roll"), Roll);
		if (!bHasP && !bHasY && !bHasR) { return false; }
		OutR = FRotator(Pitch, Yaw, Roll);
		return true;
	}

	/** Read ``args.component_path`` field; emit kCMPErrorInvalidParams on missing/empty. */
	bool CMP_RequireComponentPath(const FMCPRequest& Request, FString& OutPath, FMCPResponse& OutError,
		const TCHAR* FieldName = TEXT("component_path"))
	{
		if (!Request.Args.IsValid())
		{
			OutError = FMCPToolHelpers::MakeError(Request, kCMPErrorInvalidParams, TEXT("missing args object"));
			return false;
		}
		if (!Request.Args->TryGetStringField(FieldName, OutPath) || OutPath.IsEmpty())
		{
			OutError = FMCPToolHelpers::MakeError(Request, kCMPErrorInvalidParams,
				FString::Printf(TEXT("missing required string field '%s'"), FieldName));
			return false;
		}
		return true;
	}

	/** Read ``args.actor_path`` field; emit kCMPErrorInvalidParams on missing/empty. */
	bool CMP_RequireActorPath(const FMCPRequest& Request, FString& OutPath, FMCPResponse& OutError,
		const TCHAR* FieldName = TEXT("actor_path"))
	{
		if (!Request.Args.IsValid())
		{
			OutError = FMCPToolHelpers::MakeError(Request, kCMPErrorInvalidParams, TEXT("missing args object"));
			return false;
		}
		if (!Request.Args->TryGetStringField(FieldName, OutPath) || OutPath.IsEmpty())
		{
			OutError = FMCPToolHelpers::MakeError(Request, kCMPErrorInvalidParams,
				FString::Printf(TEXT("missing required string field '%s'"), FieldName));
			return false;
		}
		return true;
	}

	/**
	 * Resolve a component path + emit appropriate error. ``bRejectPIE`` is the caller's choice —
	 * mutators pass true, read-only callers pass false. Additionally checks D18 sublevel-visibility
	 * for the owning actor when ``bCheckSublevelVisible=true``.
	 *
	 * On null return populates ``OutError`` with the right kMCPError* code:
	 *   - kMCPErrorAmbiguousComponent (-32024) when ResolveComponent set bAmbiguous=true
	 *   - kMCPErrorObjectNotFound      (-32004) otherwise (parse / actor / no-such-component)
	 * Sublevel-visibility violation → kMCPErrorLevelNotFound (-32019).
	 */
	UActorComponent* CMP_ResolveComponentOrError(
		const FMCPRequest& Request,
		const FString& Path,
		bool bRejectPIE,
		bool bCheckSublevelVisible,
		FMCPResponse& OutError)
	{
		bool bAmbig = false;
		FString AmbigHint;
		FString ResolveErr;
		UActorComponent* Comp = FMCPComponentPathUtils::ResolveComponent(
			Path, bRejectPIE, bAmbig, AmbigHint, ResolveErr);
		if (!Comp)
		{
			if (bAmbig)
			{
				OutError = FMCPToolHelpers::MakeError(Request, kMCPErrorAmbiguousComponent, ResolveErr);
			}
			else
			{
				OutError = FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound, ResolveErr);
			}
			return nullptr;
		}
		if (bCheckSublevelVisible)
		{
			const AActor* Owner = Comp->GetOwner();
			const ULevel* Level = Owner ? Owner->GetLevel() : nullptr;
			if (Level && !Level->bIsVisible)
			{
				OutError = FMCPToolHelpers::MakeError(Request, kMCPErrorLevelNotFound,
					FString::Printf(
						TEXT("component '%s' owning sublevel '%s' is loaded but not visible/loaded; cannot mutate"),
						*Comp->GetName(),
						Level->GetOutermost() ? *Level->GetOutermost()->GetName() : TEXT("?")));
				return nullptr;
			}
		}
		return Comp;
	}

	// ─── JSON serialisers ────────────────────────────────────────────────────────────────────────

	TSharedRef<FJsonObject> CMP_VectorToJson(const FVector& V)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("_kind"), TEXT("Vector"));
		Obj->SetNumberField(TEXT("x"), V.X);
		Obj->SetNumberField(TEXT("y"), V.Y);
		Obj->SetNumberField(TEXT("z"), V.Z);
		return Obj;
	}

	TSharedRef<FJsonObject> CMP_RotatorToJson(const FRotator& R)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("_kind"), TEXT("Rotator"));
		Obj->SetNumberField(TEXT("pitch"), R.Pitch);
		Obj->SetNumberField(TEXT("yaw"),   R.Yaw);
		Obj->SetNumberField(TEXT("roll"),  R.Roll);
		return Obj;
	}

	/**
	 * Build the full component snapshot returned by component.get.
	 *
	 * Shape:
	 *   {
	 *     "path":                    "<actor_path>/<component_fname>",
	 *     "name":                    "FName string",
	 *     "class":                   "/Script/Engine.StaticMeshComponent",
	 *     "is_scene":                bool,
	 *     "parent_component_name":   "FName string" | null,
	 *     "is_root_component":       bool,
	 *     "owner_actor_path":        "<actor_path>",
	 *     "relative_location":       Vector  | null (scene-only),
	 *     "relative_rotation":       Rotator | null (scene-only),
	 *     "relative_scale":          Vector  | null (scene-only),
	 *     "properties_summary": [
	 *       { name, type, flags, offset }, ...
	 *     ]
	 *   }
	 */
	TSharedRef<FJsonObject> CMP_BuildComponentSnapshot(const UActorComponent* Comp)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (!Comp)
		{
			return Obj;
		}
		Obj->SetStringField(TEXT("path"),  FMCPComponentPathUtils::BuildComponentPath(Comp));
		Obj->SetStringField(TEXT("name"),  Comp->GetFName().ToString());
		Obj->SetStringField(TEXT("class"), Comp->GetClass()->GetPathName());

		const USceneComponent* Scene = Cast<const USceneComponent>(Comp);
		const bool bIsScene = (Scene != nullptr);
		Obj->SetBoolField(TEXT("is_scene"), bIsScene);

		const AActor* Owner = Comp->GetOwner();
		if (Owner)
		{
			Obj->SetStringField(TEXT("owner_actor_path"), FMCPActorPathUtils::BuildActorPath(Owner));
		}
		else
		{
			Obj->SetField(TEXT("owner_actor_path"), MakeShared<FJsonValueNull>());
		}

		bool bIsRoot = false;
		if (bIsScene && Owner && Owner->GetRootComponent() == Scene)
		{
			bIsRoot = true;
		}
		Obj->SetBoolField(TEXT("is_root_component"), bIsRoot);

		if (bIsScene)
		{
			if (const USceneComponent* Parent = Scene->GetAttachParent())
			{
				Obj->SetStringField(TEXT("parent_component_name"), Parent->GetFName().ToString());
			}
			else
			{
				Obj->SetField(TEXT("parent_component_name"), MakeShared<FJsonValueNull>());
			}
			Obj->SetObjectField(TEXT("relative_location"), CMP_VectorToJson(Scene->GetRelativeLocation()));
			Obj->SetObjectField(TEXT("relative_rotation"), CMP_RotatorToJson(Scene->GetRelativeRotation()));
			Obj->SetObjectField(TEXT("relative_scale"),    CMP_VectorToJson(Scene->GetRelativeScale3D()));
		}
		else
		{
			Obj->SetField(TEXT("parent_component_name"), MakeShared<FJsonValueNull>());
			Obj->SetField(TEXT("relative_location"),     MakeShared<FJsonValueNull>());
			Obj->SetField(TEXT("relative_rotation"),     MakeShared<FJsonValueNull>());
			Obj->SetField(TEXT("relative_scale"),        MakeShared<FJsonValueNull>());
		}

		// properties_summary: walk the UClass for FProperty entries (incl. inherited) via
		// MakePropertySummary — matches the FMCPMarshalling.cpp::ListProperties traversal pattern.
		TArray<TSharedPtr<FJsonValue>> Summary;
		const UClass* CompClass = Comp->GetClass();
		for (TFieldIterator<FProperty> It(CompClass, EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			Summary.Add(MakeShared<FJsonValueObject>(FMCPReflection::MakePropertySummary(*It)));
		}
		Obj->SetArrayField(TEXT("properties_summary"), Summary);

		return Obj;
	}

	// ─── class resolution (component.add) ────────────────────────────────────────────────────────

	/**
	 * Resolve ``component_class`` to a concrete UClass that is a UActorComponent subclass.
	 *
	 * Surfaces the same 4-code family as actor.spawn::ACT_ResolveActorClass (D9):
	 *   -32023 InvalidClassPath  — syntactically malformed
	 *   -32020 ClassNotFound     — well-formed path, LoadObject returned null after best-effort autoload
	 *   -32021 ClassAbstract     — class has CLASS_Abstract
	 *   -32022 WrongClassFamily  — not a UActorComponent subclass (e.g. AActor path passed)
	 */
	UClass* CMP_ResolveComponentClass(const FMCPRequest& Request, const FString& ClassPath,
		FMCPResponse& OutError)
	{
		if (ClassPath.IsEmpty() || ClassPath[0] != TEXT('/'))
		{
			OutError = FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidClassPath,
				FString::Printf(
					TEXT("component_class '%s' invalid — must start with '/' (e.g. '/Script/Engine.PointLightComponent')"),
					*ClassPath));
			return nullptr;
		}
		if (ClassPath.Contains(TEXT("\\")))
		{
			OutError = FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidClassPath,
				FString::Printf(TEXT("component_class '%s' contains backslash"), *ClassPath));
			return nullptr;
		}

		UClass* Class = LoadObject<UClass>(nullptr, *ClassPath);
		if (!Class && !ClassPath.EndsWith(TEXT("_C")))
		{
			// BP component class probe — try the auto-generated _C suffix.
			const FString Retry = ClassPath + TEXT("_C");
			Class = LoadObject<UClass>(nullptr, *Retry);
		}
		if (!Class)
		{
			OutError = FMCPToolHelpers::MakeError(Request, kMCPErrorClassNotFound,
				FString::Printf(
					TEXT("component_class '%s' could not be resolved to a UClass (LoadObject returned null); ")
					TEXT("for Blueprint component classes try with trailing '_C'"),
					*ClassPath));
			return nullptr;
		}
		if (Class->HasAnyClassFlags(CLASS_Abstract))
		{
			OutError = FMCPToolHelpers::MakeError(Request, kMCPErrorClassAbstract,
				FString::Printf(
					TEXT("component_class '%s' is abstract — pick a concrete subclass"),
					*Class->GetPathName()));
			return nullptr;
		}
		if (!Class->IsChildOf(UActorComponent::StaticClass()))
		{
			OutError = FMCPToolHelpers::MakeError(Request, kMCPErrorWrongClassFamily,
				FString::Printf(
					TEXT("component_class '%s' is not a UActorComponent subclass (got base '%s')"),
					*Class->GetPathName(),
					Class->GetSuperClass() ? *Class->GetSuperClass()->GetPathName() : TEXT("?")));
			return nullptr;
		}
		return Class;
	}

	// ─── cycle detection (component.move_in_hierarchy) ───────────────────────────────────────────

	/**
	 * True iff making ``NewParent`` the parent of ``Child`` would create a cycle in the scene-graph.
	 * Walks the proposed parent chain looking for ``Child``. Bounded to 256 hops as a safety net —
	 * real-world scene-graphs are at most a few dozen deep.
	 */
	bool CMP_ParentWouldCycle(const USceneComponent* NewParent, const USceneComponent* Child)
	{
		if (!NewParent || !Child) { return false; }
		if (NewParent == Child) { return true; }
		const USceneComponent* Cur = NewParent;
		for (int32 i = 0; i < 256 && Cur; ++i)
		{
			if (Cur == Child) { return true; }
			Cur = Cur->GetAttachParent();
		}
		return false;
	}
} // namespace

namespace FComponentTools
{

// ─── component.add (mutator — PIE-guarded) ───────────────────────────────────────────────────
//
// Creates a new UActorComponent of ``component_class`` on the actor at ``actor_path``. For
// USceneComponent subclasses, the optional ``attach_to`` argument names an existing scene component
// (by FName) on the same actor to attach under; null means attach under RootComponent. When the
// actor has no RootComponent and a USceneComponent is being added, the new component BECOMES the
// root.
//
// Args:
//   actor_path        (required) — full actor path (forms 1/2/3 per FMCPActorPathUtils)
//   component_class   (required) — /Script/<Module>.<ClassName> or /Game/.../BP_X.BP_X_C
//   component_name    (optional FName string) — desired internal name; engine auto-suffixes on collision
//   attach_to         (optional FName string) — existing USceneComponent on the same actor; scene-only
//
// Response: { ok: true, component_path, name, class, is_scene, parent_component_name, is_root_component }
FMCPResponse Tool_Add(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("ComponentAdd", "MCP: add component"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kCMPErrorInvalidParams, TEXT("missing args object"));
	}

	FString ActorPath;
	FMCPResponse PathErr;
	if (!CMP_RequireActorPath(Request, ActorPath, PathErr)) { return PathErr; }

	FString ClassPath;
	if (!Request.Args->TryGetStringField(TEXT("component_class"), ClassPath) || ClassPath.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kCMPErrorInvalidParams,
			TEXT("missing required string field 'component_class'"));
	}

	// Resolve actor (mutator, sublevel-visibility checked).
	bool bActorAmbig = false;
	FString ActorAmbigHint;
	FString ActorErr;
	AActor* Actor = FMCPActorPathUtils::ResolveActor(
		ActorPath, /*bRejectPIE*/ true, bActorAmbig, ActorAmbigHint, ActorErr);
	if (!Actor)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound, ActorErr);
	}
	if (const ULevel* L = Actor->GetLevel())
	{
		if (!L->bIsVisible)
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorLevelNotFound,
				FString::Printf(
					TEXT("actor '%s' owning sublevel '%s' is loaded but not visible/loaded; cannot mutate"),
					*Actor->GetName(),
					L->GetOutermost() ? *L->GetOutermost()->GetName() : TEXT("?")));
		}
	}

	// Resolve class (UActorComponent subclass).
	FMCPResponse ClassErr;
	UClass* CompClass = CMP_ResolveComponentClass(Request, ClassPath, ClassErr);
	if (!CompClass)
	{
		return ClassErr;
	}

	// Desired name (optional).
	FName DesiredName(NAME_None);
	FString DesiredNameStr;
	if (Request.Args->TryGetStringField(TEXT("component_name"), DesiredNameStr) && !DesiredNameStr.IsEmpty())
	{
		DesiredName = FName(*DesiredNameStr);
	}

	// attach_to (optional) — only meaningful for USceneComponent additions. Resolved against the
	// actor's existing components AFTER class-family check below.
	FString AttachToStr;
	const bool bAttachToProvided =
		Request.Args->TryGetStringField(TEXT("attach_to"), AttachToStr) && !AttachToStr.IsEmpty();

	const bool bIsScene = CompClass->IsChildOf(USceneComponent::StaticClass());

	// Validate attach_to early — non-scene component cannot have an attach parent.
	if (bAttachToProvided && !bIsScene)
	{
		return FMCPToolHelpers::MakeError(Request, kCMPErrorInvalidParams,
			FString::Printf(
				TEXT("attach_to provided ('%s') but component_class '%s' is not a USceneComponent subclass"),
				*AttachToStr, *CompClass->GetPathName()));
	}

	// Resolve attach_to component (if provided).
	USceneComponent* AttachParent = nullptr;
	if (bAttachToProvided)
	{
		TArray<USceneComponent*> SceneComps;
		Actor->GetComponents(SceneComps);
		const FName AttachToFName(*AttachToStr);
		for (USceneComponent* SC : SceneComps)
		{
			if (SC && SC->GetFName() == AttachToFName)
			{
				AttachParent = SC;
				break;
			}
		}
		if (!AttachParent)
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
				FString::Printf(
					TEXT("attach_to component '%s' not found on actor '%s'"),
					*AttachToStr, *Actor->GetName()));
		}
	}

	Actor->Modify();

	// Create the component. Owner is ``Actor`` so it lives in the actor's outer chain.
	UActorComponent* NewComp = NewObject<UActorComponent>(Actor, CompClass, DesiredName, RF_Transactional);
	if (!NewComp)
	{
		return FMCPToolHelpers::MakeError(Request, kCMPErrorInternal,
			FString::Printf(TEXT("NewObject<%s> returned null"), *CompClass->GetName()));
	}

	// Register lifecycle — must add to the actor's owned set so destruction unwires it cleanly.
	Actor->AddInstanceComponent(NewComp);
	NewComp->OnComponentCreated();

	// Scene-component attachment.
	if (bIsScene)
	{
		USceneComponent* NewScene = Cast<USceneComponent>(NewComp);
		check(NewScene);

		USceneComponent* ExistingRoot = Actor->GetRootComponent();
		if (AttachParent)
		{
			// Attach under specified parent.
			AttachParent->Modify();
			NewScene->AttachToComponent(AttachParent, FAttachmentTransformRules::KeepRelativeTransform);
		}
		else if (ExistingRoot)
		{
			// Default: attach under root.
			ExistingRoot->Modify();
			NewScene->AttachToComponent(ExistingRoot, FAttachmentTransformRules::KeepRelativeTransform);
		}
		else
		{
			// No root → this scene component becomes the root.
			Actor->SetRootComponent(NewScene);
		}
	}

	NewComp->RegisterComponent();

	// Refresh editor outliner / actor browser.
#if WITH_EDITOR
	Actor->RerunConstructionScripts();
#endif

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("ok"), true);
	Out->SetStringField(TEXT("component_path"), FMCPComponentPathUtils::BuildComponentPath(NewComp));
	Out->SetStringField(TEXT("name"),           NewComp->GetFName().ToString());
	Out->SetStringField(TEXT("class"),          NewComp->GetClass()->GetPathName());
	Out->SetBoolField  (TEXT("is_scene"),       bIsScene);
	if (bIsScene)
	{
		const USceneComponent* NewScene = Cast<const USceneComponent>(NewComp);
		check(NewScene);
		if (const USceneComponent* Parent = NewScene->GetAttachParent())
		{
			Out->SetStringField(TEXT("parent_component_name"), Parent->GetFName().ToString());
		}
		else
		{
			Out->SetField(TEXT("parent_component_name"), MakeShared<FJsonValueNull>());
		}
		Out->SetBoolField(TEXT("is_root_component"),
			Actor->GetRootComponent() == NewScene);
	}
	else
	{
		Out->SetField(TEXT("parent_component_name"), MakeShared<FJsonValueNull>());
		Out->SetBoolField(TEXT("is_root_component"), false);
	}
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── component.remove (mutator — PIE-guarded) ────────────────────────────────────────────────
//
// Removes a component from its owning actor. The component is unregistered and destroyed under
// transaction so undo restores it. Removing the actor's root component is REFUSED — the root
// anchors all attached children; users must re-parent first then remove.
//
// Args: { component_path }
// Response: { ok: true, component_path, removed_name, owner_actor_path }
FMCPResponse Tool_Remove(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("ComponentRemove", "MCP: remove component"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString Path;
	FMCPResponse PathErr;
	if (!CMP_RequireComponentPath(Request, Path, PathErr)) { return PathErr; }

	FMCPResponse ResolveErr;
	UActorComponent* Comp = CMP_ResolveComponentOrError(
		Request, Path, /*bRejectPIE*/ true, /*bCheckSublevelVisible*/ true, ResolveErr);
	if (!Comp) { return ResolveErr; }

	AActor* Owner = Comp->GetOwner();
	if (!Owner)
	{
		return FMCPToolHelpers::MakeError(Request, kCMPErrorInternal,
			FString::Printf(TEXT("component '%s' has no owning actor"), *Comp->GetName()));
	}

	// Root component refusal — would orphan all children + leave actor without spatial transform.
	if (const USceneComponent* Scene = Cast<const USceneComponent>(Comp))
	{
		if (Owner->GetRootComponent() == Scene)
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidPath,
				FString::Printf(
					TEXT("refusing to remove root component '%s' of actor '%s' — re-parent children first or destroy the actor"),
					*Comp->GetName(), *Owner->GetName()));
		}
	}

	const FString CanonicalPath = FMCPComponentPathUtils::BuildComponentPath(Comp);
	const FString OwnerPath     = FMCPActorPathUtils::BuildActorPath(Owner);
	const FString RemovedName   = Comp->GetFName().ToString();

	Owner->Modify();
	Comp->Modify();

	Comp->UnregisterComponent();
	Owner->RemoveInstanceComponent(Comp);
	Comp->DestroyComponent(/*bPromoteChildren*/ false);

#if WITH_EDITOR
	Owner->RerunConstructionScripts();
#endif

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("ok"), true);
	Out->SetStringField(TEXT("component_path"),   CanonicalPath);
	Out->SetStringField(TEXT("removed_name"),     RemovedName);
	Out->SetStringField(TEXT("owner_actor_path"), OwnerPath);
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── component.get (read-only — works in PIE) ────────────────────────────────────────────────
//
// Returns the full component snapshot.
//
// Args: { component_path }
// Response: see CMP_BuildComponentSnapshot.
FMCPResponse Tool_Get(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString Path;
	FMCPResponse PathErr;
	if (!CMP_RequireComponentPath(Request, Path, PathErr)) { return PathErr; }

	FMCPResponse ResolveErr;
	UActorComponent* Comp = CMP_ResolveComponentOrError(
		Request, Path, /*bRejectPIE*/ false, /*bCheckSublevelVisible*/ false, ResolveErr);
	if (!Comp) { return ResolveErr; }

	return FMCPToolHelpers::MakeSuccessObj(Request, CMP_BuildComponentSnapshot(Comp));
}

// ─── component.get_property (read-only — works in PIE) ───────────────────────────────────────
//
// Reads a single FProperty value off the component by dotted path. Delegates to
// FMCPReflection::ResolvePropertyPath + ReadPropertyValueAt.
//
// Args: { component_path, property_path: dotted path }
// Response: { component_path, property_path, type, value }
FMCPResponse Tool_GetProperty(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString Path;
	FMCPResponse PathErr;
	if (!CMP_RequireComponentPath(Request, Path, PathErr)) { return PathErr; }

	FString PropertyPath;
	if (!Request.Args->TryGetStringField(TEXT("property_path"), PropertyPath) || PropertyPath.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kCMPErrorInvalidParams,
			TEXT("missing required string field 'property_path'"));
	}

	FMCPResponse ResolveErr;
	UActorComponent* Comp = CMP_ResolveComponentOrError(
		Request, Path, /*bRejectPIE*/ false, /*bCheckSublevelVisible*/ false, ResolveErr);
	if (!Comp) { return ResolveErr; }

	UObject* Container = nullptr;
	void* LeafValuePtr = nullptr;
	FProperty* LeafProp = nullptr;
	int32 ErrCode = 0;
	FString ErrMsg;
	if (!FMCPReflection::ResolvePropertyPath(Comp, PropertyPath, Container, LeafValuePtr, LeafProp,
		ErrCode, ErrMsg))
	{
		return FMCPToolHelpers::MakeError(Request, ErrCode, ErrMsg);
	}

	TSharedPtr<FJsonValue> Value = FMCPReflection::ReadPropertyValueAt(LeafProp, LeafValuePtr);

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("component_path"), FMCPComponentPathUtils::BuildComponentPath(Comp));
	Out->SetStringField(TEXT("property_path"),  PropertyPath);
	Out->SetStringField(TEXT("type"),           FMCPReflection::DescribePropertyType(LeafProp));
	Out->SetField(TEXT("value"), Value);
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── component.set_property (mutator — PIE-guarded) ──────────────────────────────────────────
//
// Writes a single FProperty by dotted path. Implements the full D7 contract — edit-const gate
// FIRST (3-flag set per Days 4-8 hotfix `210c050`), then FMCPWritePropertyScope owns the 4-step
// Pre/Modify/Transaction/Post contract.
//
// Args: { component_path, property_path: dotted path, value: <jsonified> }
// Response: { ok, component_path, property_path, value }
FMCPResponse Tool_SetProperty(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (FMCPWorldContext::IsPIEActive())
	{
		return CMP_MakePIEError(Request);
	}

	FString Path;
	FMCPResponse PathErr;
	if (!CMP_RequireComponentPath(Request, Path, PathErr)) { return PathErr; }

	FString PropertyPath;
	if (!Request.Args->TryGetStringField(TEXT("property_path"), PropertyPath) || PropertyPath.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kCMPErrorInvalidParams,
			TEXT("missing required string field 'property_path'"));
	}
	const TSharedPtr<FJsonValue> ValueField = Request.Args->TryGetField(TEXT("value"));
	if (!ValueField.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kCMPErrorInvalidParams,
			TEXT("missing required field 'value'"));
	}

	FMCPResponse ResolveErr;
	UActorComponent* Comp = CMP_ResolveComponentOrError(
		Request, Path, /*bRejectPIE*/ true, /*bCheckSublevelVisible*/ true, ResolveErr);
	if (!Comp) { return ResolveErr; }

	UObject* Container = nullptr;
	void* LeafValuePtr = nullptr;
	FProperty* LeafProp = nullptr;
	int32 ErrCode = 0;
	FString ErrMsg;
	if (!FMCPReflection::ResolvePropertyPath(Comp, PropertyPath, Container, LeafValuePtr, LeafProp,
		ErrCode, ErrMsg))
	{
		return FMCPToolHelpers::MakeError(Request, ErrCode, ErrMsg);
	}

	// Step 1: edit-const gate FIRST (early return, no transaction). 2-flag set (CPF_EditConst |
	// CPF_DisableEditOnInstance) — CPF_BlueprintReadOnly dropped (2026-05) since it's a runtime
	// BP restriction, not an editor-write restriction; the editor's Details panel happily writes
	// to BlueprintReadOnly UPROPERTIES at design time and the MCP bridge is acting as editor
	// surrogate. Caller can force-bypass via args.bypass_readonly=true.
	// TryGetBoolField returns false on missing OR wrong type, leaving OutValue default (false).
	// Prior pattern (HasField + GetBoolField) crashes if field exists but wrong JSON type
	// (e.g. caller passes `bypass_readonly: "yes"` as a string instead of a boolean).
	bool bBypassReadOnly = false;
	if (Request.Args.IsValid())
	{
		Request.Args->TryGetBoolField(TEXT("bypass_readonly"), bBypassReadOnly);
	}
	const uint64 Flags = LeafProp->PropertyFlags;
	if (!bBypassReadOnly && (Flags & (CPF_EditConst | CPF_DisableEditOnInstance)))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPropertyAccessDenied,
			FString::Printf(
				TEXT("property '%s' is read-only (CPF flags=%llu); pass args.bypass_readonly=true to override"),
				*LeafProp->GetName(), static_cast<unsigned long long>(Flags)));
	}

	// Step 2: RAII scope owns Pre/Modify/Transaction; dtor fires PostEditChangeProperty.
	FString WriteErr;
	bool bWriteOk = false;
	{
		FMCPWritePropertyScope Scope(Container, LeafProp,
			LOCTEXT("ComponentSetProperty", "MCP: set component property"));
		bWriteOk = FMCPReflection::WritePropertyValueAt(LeafProp, LeafValuePtr, ValueField, Container, WriteErr);
	}
	if (!bWriteOk)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPropertyTypeMismatch,
			FString::Printf(TEXT("write rejected: %s"), *WriteErr));
	}

	// Re-read post-write for round-trip echo.
	TSharedPtr<FJsonValue> EchoValue = FMCPReflection::ReadPropertyValueAt(LeafProp, LeafValuePtr);

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("ok"), true);
	Out->SetStringField(TEXT("component_path"), FMCPComponentPathUtils::BuildComponentPath(Comp));
	Out->SetStringField(TEXT("property_path"),  PropertyPath);
	Out->SetField(TEXT("value"), EchoValue);
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── component.set_transform (mutator — PIE-guarded, USceneComponent only) ───────────────────
//
// Sets relative location / rotation / scale on a USceneComponent. Each field is optional —
// missing fields preserve the current value. ``space`` is reserved for future "World" / "Relative"
// addressing; for now it only accepts "Relative" (or omitted) and rejects unknowns.
//
// Args: { component_path, location?, rotation?, scale?, space? }
// Response: { ok, component_path, relative_location, relative_rotation, relative_scale }
FMCPResponse Tool_SetTransform(const FMCPRequest& Request)
{
	check(IsInGameThread());

	// Per-field transaction: each call wraps its own scope so undo is per-field (matches
	// actor.set_location / set_rotation / set_scale grouping convention from Days 5-8). We do
	// this in ONE Modify pass with one transaction to keep the outliner history clean.
	FMCPMutatorScope Scope(Request, LOCTEXT("ComponentSetTransform", "MCP: set component transform"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString Path;
	FMCPResponse PathErr;
	if (!CMP_RequireComponentPath(Request, Path, PathErr)) { return PathErr; }

	FMCPResponse ResolveErr;
	UActorComponent* Comp = CMP_ResolveComponentOrError(
		Request, Path, /*bRejectPIE*/ true, /*bCheckSublevelVisible*/ true, ResolveErr);
	if (!Comp) { return ResolveErr; }

	USceneComponent* Scene = Cast<USceneComponent>(Comp);
	if (!Scene)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorWrongClassFamily,
			FString::Printf(
				TEXT("component '%s' is not a USceneComponent — set_transform requires scene component (class '%s')"),
				*Comp->GetName(), *Comp->GetClass()->GetPathName()));
	}

	// Optional ``space`` arg (currently only Relative supported).
	FString SpaceStr;
	if (Request.Args->TryGetStringField(TEXT("space"), SpaceStr) && !SpaceStr.IsEmpty())
	{
		if (!SpaceStr.Equals(TEXT("Relative"), ESearchCase::IgnoreCase))
		{
			return FMCPToolHelpers::MakeError(Request, kCMPErrorInvalidParams,
				FString::Printf(TEXT("space '%s' unsupported — only 'Relative' is accepted in Phase 3"),
					*SpaceStr));
		}
	}

	bool bAnyField = false;

	Scene->Modify();

	const TSharedPtr<FJsonObject>* LocObj = nullptr;
	if (Request.Args->TryGetObjectField(TEXT("location"), LocObj) && LocObj && LocObj->IsValid())
	{
		FVector V;
		if (CMP_ReadJsonVector(*LocObj, V))
		{
			Scene->SetRelativeLocation(V);
			bAnyField = true;
		}
	}
	const TSharedPtr<FJsonObject>* RotObj = nullptr;
	if (Request.Args->TryGetObjectField(TEXT("rotation"), RotObj) && RotObj && RotObj->IsValid())
	{
		FRotator R;
		if (CMP_ReadJsonRotator(*RotObj, R))
		{
			Scene->SetRelativeRotation(R);
			bAnyField = true;
		}
	}
	const TSharedPtr<FJsonObject>* ScaleObj = nullptr;
	if (Request.Args->TryGetObjectField(TEXT("scale"), ScaleObj) && ScaleObj && ScaleObj->IsValid())
	{
		FVector V;
		if (CMP_ReadJsonVector(*ScaleObj, V))
		{
			Scene->SetRelativeScale3D(V);
			bAnyField = true;
		}
	}

	if (!bAnyField)
	{
		return FMCPToolHelpers::MakeError(Request, kCMPErrorInvalidParams,
			TEXT("component.set_transform: at least one of location / rotation / scale must be provided"));
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("ok"), true);
	Out->SetStringField(TEXT("component_path"),    FMCPComponentPathUtils::BuildComponentPath(Scene));
	Out->SetObjectField(TEXT("relative_location"), CMP_VectorToJson(Scene->GetRelativeLocation()));
	Out->SetObjectField(TEXT("relative_rotation"), CMP_RotatorToJson(Scene->GetRelativeRotation()));
	Out->SetObjectField(TEXT("relative_scale"),    CMP_VectorToJson(Scene->GetRelativeScale3D()));
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── component.move_in_hierarchy (mutator — PIE-guarded, USceneComponent only) ───────────────
//
// Re-parents a USceneComponent within its owning actor's scene-graph. Per D17:
//   - Both source and new parent MUST be USceneComponent
//   - Both MUST live on the same actor (no cross-actor moves)
//   - Cannot re-parent the root component
//   - Cycle detection via parent-chain walk (per D14)
// Uses ``AttachToComponent(NewParent, KeepRelativeTransform)``.
//
// Args: { component_path, new_parent_component_path }
// Response: { ok, component_path, previous_parent_name, new_parent_name }
FMCPResponse Tool_MoveInHierarchy(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("ComponentMoveInHierarchy",
		"MCP: move component in hierarchy"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString ChildPath;
	FMCPResponse PathErr;
	if (!CMP_RequireComponentPath(Request, ChildPath, PathErr)) { return PathErr; }

	FString NewParentPath;
	if (!Request.Args->TryGetStringField(TEXT("new_parent_component_path"), NewParentPath)
		|| NewParentPath.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kCMPErrorInvalidParams,
			TEXT("missing required string field 'new_parent_component_path'"));
	}

	// Resolve child (mutator).
	FMCPResponse ChildErr;
	UActorComponent* ChildComp = CMP_ResolveComponentOrError(
		Request, ChildPath, /*bRejectPIE*/ true, /*bCheckSublevelVisible*/ true, ChildErr);
	if (!ChildComp) { return ChildErr; }

	// Resolve new parent (mutator).
	FMCPResponse ParentErr;
	UActorComponent* ParentComp = CMP_ResolveComponentOrError(
		Request, NewParentPath, /*bRejectPIE*/ true, /*bCheckSublevelVisible*/ true, ParentErr);
	if (!ParentComp) { return ParentErr; }

	// Both must be USceneComponent.
	USceneComponent* ChildScene = Cast<USceneComponent>(ChildComp);
	if (!ChildScene)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorWrongClassFamily,
			FString::Printf(
				TEXT("child component '%s' is not a USceneComponent (class '%s')"),
				*ChildComp->GetName(), *ChildComp->GetClass()->GetPathName()));
	}
	USceneComponent* ParentScene = Cast<USceneComponent>(ParentComp);
	if (!ParentScene)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorWrongClassFamily,
			FString::Printf(
				TEXT("new parent component '%s' is not a USceneComponent (class '%s')"),
				*ParentComp->GetName(), *ParentComp->GetClass()->GetPathName()));
	}

	if (ChildScene == ParentScene)
	{
		return FMCPToolHelpers::MakeError(Request, kCMPErrorInvalidParams,
			TEXT("child and new_parent are the same component"));
	}

	// Same actor only.
	AActor* ChildOwner = ChildScene->GetOwner();
	AActor* ParentOwner = ParentScene->GetOwner();
	if (!ChildOwner || ChildOwner != ParentOwner)
	{
		return FMCPToolHelpers::MakeError(Request, kCMPErrorInvalidParams,
			FString::Printf(
				TEXT("child owner '%s' differs from parent owner '%s' — move_in_hierarchy is intra-actor only"),
				ChildOwner ? *ChildOwner->GetName() : TEXT("(null)"),
				ParentOwner ? *ParentOwner->GetName() : TEXT("(null)")));
	}

	// Cannot re-parent the root component.
	if (ChildOwner->GetRootComponent() == ChildScene)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidPath,
			FString::Printf(
				TEXT("refusing to re-parent root component '%s' of actor '%s' — would orphan the actor's transform anchor"),
				*ChildScene->GetName(), *ChildOwner->GetName()));
	}

	// Cycle detection (per D14): walk proposed parent chain looking for child. Surfaces under
	// kCMPErrorInvalidParams to match actor.attach's cycle-rejection convention (per the equivalent
	// ACT_AttachWouldCycle path in ActorTools.cpp — both surfaces share the JSON-RPC error family).
	if (CMP_ParentWouldCycle(ParentScene, ChildScene))
	{
		return FMCPToolHelpers::MakeError(Request, kCMPErrorInvalidParams,
			FString::Printf(
				TEXT("move_in_hierarchy would create a cycle — '%s' (or its ancestor) is already a descendant of '%s'"),
				*ParentScene->GetName(), *ChildScene->GetName()));
	}

	const USceneComponent* PrevParent = ChildScene->GetAttachParent();
	const FString PrevParentName = PrevParent ? PrevParent->GetFName().ToString() : FString();

	ChildScene->Modify();
	ParentScene->Modify();
	ChildOwner->Modify();

	const bool bOk = ChildScene->AttachToComponent(ParentScene, FAttachmentTransformRules::KeepRelativeTransform);

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("ok"), bOk);
	Out->SetStringField(TEXT("component_path"),     FMCPComponentPathUtils::BuildComponentPath(ChildScene));
	Out->SetStringField(TEXT("new_parent_name"),    ParentScene->GetFName().ToString());
	if (!PrevParentName.IsEmpty())
	{
		Out->SetStringField(TEXT("previous_parent_name"), PrevParentName);
	}
	else
	{
		Out->SetField(TEXT("previous_parent_name"), MakeShared<FJsonValueNull>());
	}
	if (!bOk)
	{
		return FMCPToolHelpers::MakeError(Request, kCMPErrorInternal,
			TEXT("AttachToComponent returned false (component lifecycle issue?)"));
	}
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── component.list_class_default_subcomponents (read-only — no actor needed) ────────────────
//
// Inspects the CDO of ``class_path`` and returns its default subcomponent list. Useful for the AI
// agent to discover "what components does an AStaticMeshActor have out of the box?" before calling
// actor.spawn. Operates on the UClass + its CDO; needs no live actor.
//
// Args: { class_path }
// Response: { class_path, subobjects: [ { name, class, is_scene, parent_component_name, is_root_component }, ... ] }
FMCPResponse Tool_ListClassDefaultSubcomponents(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kCMPErrorInvalidParams, TEXT("missing args object"));
	}

	FString ClassPath;
	if (!Request.Args->TryGetStringField(TEXT("class_path"), ClassPath) || ClassPath.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kCMPErrorInvalidParams,
			TEXT("missing required string field 'class_path'"));
	}

	// Resolve class — accept any AActor subclass (concrete OR abstract — we only need the CDO).
	if (ClassPath[0] != TEXT('/'))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidClassPath,
			FString::Printf(TEXT("class_path '%s' invalid — must start with '/'"), *ClassPath));
	}
	UClass* Class = LoadObject<UClass>(nullptr, *ClassPath);
	if (!Class && !ClassPath.EndsWith(TEXT("_C")))
	{
		const FString Retry = ClassPath + TEXT("_C");
		Class = LoadObject<UClass>(nullptr, *Retry);
	}
	if (!Class)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorClassNotFound,
			FString::Printf(TEXT("class_path '%s' could not be resolved (LoadObject returned null)"),
				*ClassPath));
	}
	if (!Class->IsChildOf(AActor::StaticClass()))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorWrongClassFamily,
			FString::Printf(
				TEXT("class '%s' is not an AActor subclass — CDO subcomponent inspection only applies to actor classes"),
				*Class->GetPathName()));
	}

	UObject* CDO = Class->GetDefaultObject(/*bCreateIfNeeded*/ true);
	AActor* CDOActor = Cast<AActor>(CDO);
	if (!CDOActor)
	{
		return FMCPToolHelpers::MakeError(Request, kCMPErrorInternal,
			FString::Printf(
				TEXT("class '%s' GetDefaultObject returned non-AActor (got '%s')"),
				*Class->GetPathName(), CDO ? *CDO->GetClass()->GetName() : TEXT("(null)")));
	}

	TArray<UActorComponent*> Comps;
	CDOActor->GetComponents(Comps);

	const USceneComponent* CDORoot = CDOActor->GetRootComponent();

	TArray<TSharedPtr<FJsonValue>> Subobjects;
	Subobjects.Reserve(Comps.Num());
	for (const UActorComponent* C : Comps)
	{
		if (!C) { continue; }
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"),  C->GetFName().ToString());
		Entry->SetStringField(TEXT("class"), C->GetClass()->GetPathName());
		const USceneComponent* Scene = Cast<const USceneComponent>(C);
		const bool bIsScene = (Scene != nullptr);
		Entry->SetBoolField(TEXT("is_scene"), bIsScene);
		if (bIsScene)
		{
			if (const USceneComponent* Parent = Scene->GetAttachParent())
			{
				Entry->SetStringField(TEXT("parent_component_name"), Parent->GetFName().ToString());
			}
			else
			{
				Entry->SetField(TEXT("parent_component_name"), MakeShared<FJsonValueNull>());
			}
			Entry->SetBoolField(TEXT("is_root_component"), Scene == CDORoot);
		}
		else
		{
			Entry->SetField(TEXT("parent_component_name"), MakeShared<FJsonValueNull>());
			Entry->SetBoolField(TEXT("is_root_component"), false);
		}
		Subobjects.Add(MakeShared<FJsonValueObject>(Entry));
	}

	return FMCPJsonBuilder()
		.Str(TEXT("class_path"), Class->GetPathName())
		.Arr(TEXT("subobjects"), MoveTemp(Subobjects))
		.Num(TEXT("total"), static_cast<double>(Subobjects.Num()))
		.BuildSuccess(Request);
}

// ─── Registration ────────────────────────────────────────────────────────────────────────────
void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	// Day 9: add / remove / get / get_property
	RegisterTool(TEXT("component.add"),          &Tool_Add,          /*Lane A*/ false);
	RegisterTool(TEXT("component.remove"),       &Tool_Remove,       /*Lane A*/ false);
	RegisterTool(TEXT("component.get"),          &Tool_Get,          /*Lane A*/ false);
	RegisterTool(TEXT("component.get_property"), &Tool_GetProperty,  /*Lane A*/ false);

	// Day 10: set_property / set_transform / move_in_hierarchy / list_class_defaults
	RegisterTool(TEXT("component.set_property"),                       &Tool_SetProperty,                       /*Lane A*/ false);
	RegisterTool(TEXT("component.set_transform"),                      &Tool_SetTransform,                      /*Lane A*/ false);
	RegisterTool(TEXT("component.move_in_hierarchy"),                  &Tool_MoveInHierarchy,                   /*Lane A*/ false);
	RegisterTool(TEXT("component.list_class_default_subcomponents"),   &Tool_ListClassDefaultSubcomponents,     /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("Phase 3 Days 9-10: registered 8 component.* handlers (all Lane A)"));
}

} // namespace FComponentTools

#undef LOCTEXT_NAMESPACE

MCP_REGISTER_SURFACE(ComponentTools, &FComponentTools::Register)
