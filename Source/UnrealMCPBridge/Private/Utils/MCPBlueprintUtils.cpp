// Copyright FatumGame. All Rights Reserved.

#include "MCPBlueprintUtils.h"

#include "MCPAssetPathUtils.h"
#include "MCPTypes.h"

#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

namespace FMCPBlueprintUtils
{

UBlueprint* LoadBlueprintByPath(
	const FString& Path,
	int32& OutErrorCode,
	FString& OutError)
{
	if (Path.IsEmpty())
	{
		OutErrorCode = kMCPErrorInvalidPath;
		OutError = TEXT("blueprint_path is empty");
		return nullptr;
	}

	// Normalise + validate mount prefix (rejects backslashes, ``..``, drive letters, unknown mounts).
	const FString Normalised = FMCPAssetPathUtils::Normalize(Path);
	if (Normalised.IsEmpty() || !FMCPAssetPathUtils::IsValidGameOrPlugin(Normalised))
	{
		OutErrorCode = kMCPErrorInvalidPath;
		OutError = FString::Printf(
			TEXT("blueprint_path '%s' is malformed or references an unknown mount point"),
			*Path);
		return nullptr;
	}

	// Try package-name form first (LoadObject handles the leaf-name suffix attachment).
	// Try object-path form (``...:BP_X.BP_X``) as the fallback.
	UObject* Loaded = LoadObject<UObject>(nullptr, *Normalised);
	if (!Loaded)
	{
		const FString ObjectPath = FMCPAssetPathUtils::ToObjectPath(Normalised);
		if (!ObjectPath.IsEmpty() && ObjectPath != Normalised)
		{
			Loaded = LoadObject<UObject>(nullptr, *ObjectPath);
		}
	}
	if (!Loaded)
	{
		OutErrorCode = kMCPErrorObjectNotFound;
		OutError = FString::Printf(
			TEXT("blueprint_path '%s' could not be loaded (no asset found)"),
			*Path);
		return nullptr;
	}

	UBlueprint* Blueprint = Cast<UBlueprint>(Loaded);
	if (!Blueprint)
	{
		OutErrorCode = kMCPErrorBlueprintTypeMismatch;
		OutError = FString::Printf(
			TEXT("asset '%s' is class '%s', not UBlueprint"),
			*Path, *Loaded->GetClass()->GetPathName());
		return nullptr;
	}
	return Blueprint;
}

UClass* GetGeneratedClass(const UBlueprint* Blueprint)
{
	if (!Blueprint)
	{
		return nullptr;
	}
	return Blueprint->GeneratedClass;
}

bool IsDataOnlyBlueprint(const UBlueprint* Blueprint)
{
	if (!Blueprint)
	{
		return false;
	}
	return FBlueprintEditorUtils::IsDataOnlyBlueprint(Blueprint);
}

int32 FindVariableIndex(const UBlueprint* Blueprint, const FName VarName)
{
	if (!Blueprint || VarName.IsNone())
	{
		return INDEX_NONE;
	}
	for (int32 i = 0; i < Blueprint->NewVariables.Num(); ++i)
	{
		if (Blueprint->NewVariables[i].VarName == VarName)
		{
			return i;
		}
	}
	return INDEX_NONE;
}

int32 FindFunctionGraphIndex(const UBlueprint* Blueprint, const FName FunctionName)
{
	if (!Blueprint || FunctionName.IsNone())
	{
		return INDEX_NONE;
	}
	for (int32 i = 0; i < Blueprint->FunctionGraphs.Num(); ++i)
	{
		const UEdGraph* Graph = Blueprint->FunctionGraphs[i];
		if (Graph && Graph->GetFName() == FunctionName)
		{
			return i;
		}
	}
	return INDEX_NONE;
}

UEdGraph* FindFunctionGraph(const UBlueprint* Blueprint, const FName FunctionName)
{
	const int32 Idx = FindFunctionGraphIndex(Blueprint, FunctionName);
	if (Idx == INDEX_NONE)
	{
		return nullptr;
	}
	return Blueprint->FunctionGraphs[Idx];
}

} // namespace FMCPBlueprintUtils
