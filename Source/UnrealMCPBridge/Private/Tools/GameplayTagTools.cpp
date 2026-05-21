// Copyright FatumGame. All Rights Reserved.

#include "GameplayTagTools.h"

#include "MCPSurfaceRegistry.h"

#include "FMCPDispatchQueue.h"
#include "MCPToolHelpers.h"
#include "MCPMutatorScope.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPActorPathUtils.h"
#include "Utils/MCPWorldContext.h"

#include "Components/ActorComponent.h"
#include "GameFramework/Actor.h"
#include "GameplayTagContainer.h"
#include "GameplayTagAssetInterface.h"
#include "GameplayTagsManager.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	/**
	 * Find an FGameplayTagContainer-typed UPROPERTY by name on an UObject (actor or component).
	 * Returns the StructProperty + valuePtr pair on success; null on failure.
	 */
	FStructProperty* GT_FindTagContainerProperty(UObject* Object, const FString& PropertyName, void*& OutValuePtr)
	{
		OutValuePtr = nullptr;
		if (!Object) { return nullptr; }
		UClass* Cls = Object->GetClass();
		FProperty* Prop = Cls->FindPropertyByName(FName(*PropertyName));
		if (!Prop) { return nullptr; }
		FStructProperty* StructProp = CastField<FStructProperty>(Prop);
		if (!StructProp) { return nullptr; }
		if (StructProp->Struct != TBaseStructure<FGameplayTagContainer>::Get()) { return nullptr; }
		OutValuePtr = StructProp->ContainerPtrToValuePtr<void>(Object);
		return StructProp;
	}

	/**
	 * Try to find a FGameplayTagContainer property on the actor itself, then on each component.
	 * Returns the UObject that holds the property + valuePtr + StructProperty.
	 */
	FStructProperty* GT_LocateTagContainer(AActor* Actor, const FString& PropertyName,
		UObject*& OutHolder, void*& OutValuePtr)
	{
		OutHolder = nullptr;
		OutValuePtr = nullptr;
		if (!Actor) { return nullptr; }

		// Try the actor itself first.
		if (FStructProperty* P = GT_FindTagContainerProperty(Actor, PropertyName, OutValuePtr))
		{
			OutHolder = Actor;
			return P;
		}
		// Walk components.
		for (UActorComponent* Comp : Actor->GetComponents())
		{
			if (!Comp) { continue; }
			if (FStructProperty* P = GT_FindTagContainerProperty(Comp, PropertyName, OutValuePtr))
			{
				OutHolder = Comp;
				return P;
			}
		}
		return nullptr;
	}

	TSharedRef<FJsonValueArray> GT_ContainerToJson(const FGameplayTagContainer& Container)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Reserve(Container.Num());
		for (const FGameplayTag& Tag : Container)
		{
			Arr.Add(MakeShared<FJsonValueString>(Tag.ToString()));
		}
		return MakeShared<FJsonValueArray>(Arr);
	}
} // namespace

namespace FGameplayTagTools
{

// ─── gameplaytag.list ─────────────────────────────────────────────────────────────────────────
//
// Args:    { parent_filter?: string, only_dictionary?: bool, page_size?: int, page_token?: string }
// Result:  { tags: [{ tag, parent_count, children_count }], total_known, next_page_token? }
FMCPResponse Tool_List(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString ParentFilter;
	bool bOnlyDictionary = true;
	int32 PageSize = 200;
	FString PageToken;
	if (Request.Args.IsValid())
	{
		Request.Args->TryGetStringField(TEXT("parent_filter"), ParentFilter);
		Request.Args->TryGetBoolField(TEXT("only_dictionary"), bOnlyDictionary);
		Request.Args->TryGetNumberField(TEXT("page_size"), PageSize);
		Request.Args->TryGetStringField(TEXT("page_token"), PageToken);
	}
	PageSize = FMath::Clamp(PageSize, 1, 2000);

	UGameplayTagsManager& Mgr = UGameplayTagsManager::Get();
	FGameplayTagContainer AllTags;
	Mgr.RequestAllGameplayTags(AllTags, bOnlyDictionary);

	// Convert to array, filter by parent prefix, sort alphabetically.
	TArray<FGameplayTag> Filtered;
	Filtered.Reserve(AllTags.Num());
	for (const FGameplayTag& Tag : AllTags)
	{
		if (!ParentFilter.IsEmpty())
		{
			const FString TagStr = Tag.ToString();
			if (!TagStr.StartsWith(ParentFilter)) { continue; }
		}
		Filtered.Add(Tag);
	}
	Filtered.Sort([](const FGameplayTag& A, const FGameplayTag& B)
	{
		return A.ToString() < B.ToString();
	});

	// Pagination — simple offset-from-string. Stale cursor: same-filter mutation between pages
	// is unlikely for tag dictionary (essentially read-only at runtime), so we use raw string key.
	int32 StartIdx = 0;
	if (!PageToken.IsEmpty())
	{
		while (StartIdx < Filtered.Num() && Filtered[StartIdx].ToString() <= PageToken)
		{
			++StartIdx;
		}
	}

	TArray<TSharedPtr<FJsonValue>> Out;
	const int32 EndIdx = FMath::Min(StartIdx + PageSize, Filtered.Num());
	for (int32 i = StartIdx; i < EndIdx; ++i)
	{
		const FGameplayTag& Tag = Filtered[i];
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("tag"), Tag.ToString());
		const FGameplayTag Parent = Mgr.RequestGameplayTagDirectParent(Tag);
		if (Parent.IsValid())
		{
			Obj->SetStringField(TEXT("direct_parent"), Parent.ToString());
		}
		const FGameplayTagContainer Children = Mgr.RequestGameplayTagChildren(Tag);
		Obj->SetNumberField(TEXT("direct_children_count"), Children.Num());
		Out.Add(MakeShared<FJsonValueObject>(Obj));
	}

	TSharedRef<FJsonObject> Resp = MakeShared<FJsonObject>();
	Resp->SetArrayField(TEXT("tags"), Out);
	Resp->SetNumberField(TEXT("total_known"), Filtered.Num());
	if (EndIdx < Filtered.Num() && EndIdx > 0)
	{
		Resp->SetStringField(TEXT("next_page_token"), Filtered[EndIdx - 1].ToString());
	}
	return FMCPToolHelpers::MakeSuccessObj(Request, Resp);
}

// ─── gameplaytag.query_actor ──────────────────────────────────────────────────────────────────
//
// Args:    { actor_path: string, property_name?: string }
// Result:  { source: "interface"|"property"|"none", tags: [string], property_holder?: string }
//
// Strategy: try IGameplayTagAssetInterface first; if absent or property_name explicitly given,
// fall back to property reflection (locates FGameplayTagContainer-typed UPROPERTY by name).
FMCPResponse Tool_QueryActor(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString ActorPath;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("actor_path"), ActorPath, Err)) { return Err; }

	bool bAmbiguous = false;
	FString AmbiguityHint, ResolveErr;
	AActor* Actor = FMCPActorPathUtils::ResolveActor(ActorPath, /*bRejectPIE*/ false,
		bAmbiguous, AmbiguityHint, ResolveErr);
	if (!Actor)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("actor '%s' not found: %s"), *ActorPath, *ResolveErr));
	}

	FString PropertyName;
	const bool bHasPropertyName = Request.Args->TryGetStringField(TEXT("property_name"), PropertyName) && !PropertyName.IsEmpty();

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("actor_path"), Actor->GetPathName());

	// Strategy 1: interface implementation (preferred when caller didn't specify a property).
	if (!bHasPropertyName)
	{
		if (IGameplayTagAssetInterface* IFace = Cast<IGameplayTagAssetInterface>(Actor))
		{
			FGameplayTagContainer Tags;
			IFace->GetOwnedGameplayTags(Tags);
			Out->SetStringField(TEXT("source"), TEXT("interface"));
			Out->SetField(TEXT("tags"), GT_ContainerToJson(Tags));
			return FMCPToolHelpers::MakeSuccessObj(Request, Out);
		}
		// Also scan components for the interface.
		for (UActorComponent* Comp : Actor->GetComponents())
		{
			if (IGameplayTagAssetInterface* CompIFace = Cast<IGameplayTagAssetInterface>(Comp))
			{
				FGameplayTagContainer Tags;
				CompIFace->GetOwnedGameplayTags(Tags);
				Out->SetStringField(TEXT("source"), TEXT("interface"));
				Out->SetStringField(TEXT("interface_holder"), Comp->GetPathName());
				Out->SetField(TEXT("tags"), GT_ContainerToJson(Tags));
				return FMCPToolHelpers::MakeSuccessObj(Request, Out);
			}
		}
	}

	// Strategy 2: property reflection.
	if (bHasPropertyName)
	{
		UObject* Holder = nullptr;
		void* ValuePtr = nullptr;
		FStructProperty* StructProp = GT_LocateTagContainer(Actor, PropertyName, Holder, ValuePtr);
		if (!StructProp || !ValuePtr)
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
				FString::Printf(
					TEXT("no FGameplayTagContainer property named '%s' on actor '%s' or any component"),
					*PropertyName, *ActorPath));
		}
		const FGameplayTagContainer& Container = *static_cast<const FGameplayTagContainer*>(ValuePtr);
		Out->SetStringField(TEXT("source"), TEXT("property"));
		Out->SetStringField(TEXT("property_holder"), Holder->GetPathName());
		Out->SetStringField(TEXT("property_name"), PropertyName);
		Out->SetField(TEXT("tags"), GT_ContainerToJson(Container));
		return FMCPToolHelpers::MakeSuccessObj(Request, Out);
	}

	// No source available.
	Out->SetStringField(TEXT("source"), TEXT("none"));
	TArray<TSharedPtr<FJsonValue>> EmptyTags;
	Out->SetArrayField(TEXT("tags"), EmptyTags);
	Out->SetStringField(TEXT("hint"), TEXT("Actor doesn't implement IGameplayTagAssetInterface. "
		"Pass property_name to read a specific FGameplayTagContainer UPROPERTY."));
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── gameplaytag.add_to_container ─────────────────────────────────────────────────────────────
//
// Args:    { actor_path: string, property_name: string, tag: string }
// Result:  { added: bool, was_already_present: bool, new_count, holder }
FMCPResponse Tool_AddToContainer(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_GameplayTag_Add", "Add Gameplay Tag"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString ActorPath, PropertyName, TagStr;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("actor_path"),    ActorPath,    Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("property_name"), PropertyName, Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("tag"),           TagStr,       Err)) { return Err; }

	bool bAmbiguous = false;
	FString AmbiguityHint, ResolveErr;
	AActor* Actor = FMCPActorPathUtils::ResolveActor(ActorPath, /*bRejectPIE*/ true,
		bAmbiguous, AmbiguityHint, ResolveErr);
	if (!Actor)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("actor '%s' not found: %s"), *ActorPath, *ResolveErr));
	}

	UObject* Holder = nullptr;
	void* ValuePtr = nullptr;
	FStructProperty* StructProp = GT_LocateTagContainer(Actor, PropertyName, Holder, ValuePtr);
	if (!StructProp || !ValuePtr)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("no FGameplayTagContainer property '%s' on actor '%s'"),
				*PropertyName, *ActorPath));
	}

	const FGameplayTag Tag = UGameplayTagsManager::Get().RequestGameplayTag(FName(*TagStr), /*ErrorIfNotFound*/ false);
	if (!Tag.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("gameplay tag '%s' is not registered (check Project Settings -> GameplayTags)"), *TagStr));
	}

	FGameplayTagContainer* Container = static_cast<FGameplayTagContainer*>(ValuePtr);

	Holder->Modify();

	const bool bWasPresent = Container->HasTagExact(Tag);
	if (!bWasPresent)
	{
		Container->AddTag(Tag);
	}

	Scope.DirtyPackage(Holder->GetOutermost());

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("added"), !bWasPresent);
	Out->SetBoolField(TEXT("was_already_present"), bWasPresent);
	Out->SetNumberField(TEXT("new_count"), Container->Num());
	Out->SetStringField(TEXT("holder"), Holder->GetPathName());
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── gameplaytag.remove_from_container ────────────────────────────────────────────────────────
//
// Args:    { actor_path: string, property_name: string, tag: string }
// Result:  { removed: bool, was_present: bool, new_count, holder }
FMCPResponse Tool_RemoveFromContainer(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_GameplayTag_Remove", "Remove Gameplay Tag"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString ActorPath, PropertyName, TagStr;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("actor_path"),    ActorPath,    Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("property_name"), PropertyName, Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("tag"),           TagStr,       Err)) { return Err; }

	bool bAmbiguous = false;
	FString AmbiguityHint, ResolveErr;
	AActor* Actor = FMCPActorPathUtils::ResolveActor(ActorPath, /*bRejectPIE*/ true,
		bAmbiguous, AmbiguityHint, ResolveErr);
	if (!Actor)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("actor '%s' not found: %s"), *ActorPath, *ResolveErr));
	}

	UObject* Holder = nullptr;
	void* ValuePtr = nullptr;
	FStructProperty* StructProp = GT_LocateTagContainer(Actor, PropertyName, Holder, ValuePtr);
	if (!StructProp || !ValuePtr)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("no FGameplayTagContainer property '%s' on actor '%s'"),
				*PropertyName, *ActorPath));
	}

	const FGameplayTag Tag = UGameplayTagsManager::Get().RequestGameplayTag(FName(*TagStr), /*ErrorIfNotFound*/ false);
	if (!Tag.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("gameplay tag '%s' is not registered"), *TagStr));
	}

	FGameplayTagContainer* Container = static_cast<FGameplayTagContainer*>(ValuePtr);

	Holder->Modify();

	const bool bWasPresent = Container->HasTagExact(Tag);
	if (bWasPresent)
	{
		Container->RemoveTag(Tag);
	}

	Scope.DirtyPackage(Holder->GetOutermost());

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("removed"), bWasPresent);
	Out->SetBoolField(TEXT("was_present"), bWasPresent);
	Out->SetNumberField(TEXT("new_count"), Container->Num());
	Out->SetStringField(TEXT("holder"), Holder->GetPathName());
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	RegisterTool(TEXT("gameplaytag.list"),                  &Tool_List,                /*Lane A*/ false);
	RegisterTool(TEXT("gameplaytag.query_actor"),           &Tool_QueryActor,          /*Lane A*/ false);
	RegisterTool(TEXT("gameplaytag.add_to_container"),      &Tool_AddToContainer,      /*Lane A*/ false);
	RegisterTool(TEXT("gameplaytag.remove_from_container"), &Tool_RemoveFromContainer, /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("GameplayTag surface registered: 4 tools "
			 "(list + query_actor + add_to_container + remove_from_container), all Lane A"));
}

} // namespace FGameplayTagTools

#undef LOCTEXT_NAMESPACE

MCP_REGISTER_SURFACE(GameplayTagTools, &FGameplayTagTools::Register)
