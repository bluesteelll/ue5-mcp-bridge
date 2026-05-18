// Copyright FatumGame. All Rights Reserved.

#include "MCPPinTypeUtils.h"

#include "MCPTypes.h"

#include "EdGraphSchema_K2.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	/**
	 * Single source of truth for PC_* ↔ wire-string mapping. The list is identity-mapped from
	 * ``UEdGraphSchema_K2::PC_*`` constants (UE 5.7) — extending coverage to a new pin type
	 * requires ONLY adding a row here.
	 */
	struct FPinCategoryRow
	{
		const FName* Category;   // pointer because PC_* are inline static FNames; can't constexpr
		const TCHAR* Wire;
	};

	const FPinCategoryRow& GetCategoryTable(int32 Index);
	int32 GetCategoryTableSize();

	// Defined out-of-line so the FName* dereference resolves at first call (after K2 schema init).
	const FPinCategoryRow& GetCategoryTable(int32 Index)
	{
		static const FPinCategoryRow Table[] = {
			{ &UEdGraphSchema_K2::PC_Boolean,    TEXT("Boolean")    },
			{ &UEdGraphSchema_K2::PC_Byte,       TEXT("Byte")       },
			{ &UEdGraphSchema_K2::PC_Int,        TEXT("Int")        },
			{ &UEdGraphSchema_K2::PC_Int64,      TEXT("Int64")      },
			{ &UEdGraphSchema_K2::PC_Real,       TEXT("Real")       },
			{ &UEdGraphSchema_K2::PC_String,     TEXT("String")     },
			{ &UEdGraphSchema_K2::PC_Name,       TEXT("Name")       },
			{ &UEdGraphSchema_K2::PC_Text,       TEXT("Text")       },
			{ &UEdGraphSchema_K2::PC_Object,     TEXT("Object")     },
			{ &UEdGraphSchema_K2::PC_Class,      TEXT("Class")      },
			{ &UEdGraphSchema_K2::PC_SoftObject, TEXT("SoftObject") },
			{ &UEdGraphSchema_K2::PC_SoftClass,  TEXT("SoftClass")  },
			{ &UEdGraphSchema_K2::PC_Interface,  TEXT("Interface")  },
			{ &UEdGraphSchema_K2::PC_Struct,     TEXT("Struct")     },
			{ &UEdGraphSchema_K2::PC_Enum,       TEXT("Enum")       },
			{ &UEdGraphSchema_K2::PC_Wildcard,   TEXT("Wildcard")   },
			{ &UEdGraphSchema_K2::PC_Delegate,   TEXT("Delegate")   },
			{ &UEdGraphSchema_K2::PC_MCDelegate, TEXT("MCDelegate") },
			{ &UEdGraphSchema_K2::PC_FieldPath,  TEXT("FieldPath")  },
			{ &UEdGraphSchema_K2::PC_Exec,       TEXT("Exec")       },
		};
		return Table[Index];
	}

	int32 GetCategoryTableSize()
	{
		return 20;  // hand-maintained to match the table above
	}

	/**
	 * Categories whose pin requires a subcategory object path (Class/Struct/Enum/etc.). The presence
	 * of the path is enforced on ``FromJson`` — emitting null on ``ToJson`` is allowed only when
	 * the original pin truly has no subcategory object (e.g. a wildcard Object pin).
	 */
	bool CategoryRequiresSubcategoryObject(const FName& Category)
	{
		return Category == UEdGraphSchema_K2::PC_Object
			|| Category == UEdGraphSchema_K2::PC_Class
			|| Category == UEdGraphSchema_K2::PC_SoftObject
			|| Category == UEdGraphSchema_K2::PC_SoftClass
			|| Category == UEdGraphSchema_K2::PC_Interface
			|| Category == UEdGraphSchema_K2::PC_Struct
			|| Category == UEdGraphSchema_K2::PC_Enum
			|| Category == UEdGraphSchema_K2::PC_Delegate
			|| Category == UEdGraphSchema_K2::PC_MCDelegate
			|| Category == UEdGraphSchema_K2::PC_FieldPath;
	}

	/**
	 * Best-effort UObject resolve for a subcategory_object_path. Mirrors the Phase 3 ``actor.spawn``
	 * retry pattern — if the path doesn't already end in ``_C`` and the first load fails, retry with
	 * the suffix to handle "/Game/.../BP_X.BP_X" → "/Game/.../BP_X.BP_X_C".
	 */
	UObject* TryLoadSubcategoryObject(const FString& Path)
	{
		if (Path.IsEmpty())
		{
			return nullptr;
		}
		UObject* Obj = LoadObject<UObject>(nullptr, *Path);
		if (!Obj && !Path.EndsWith(TEXT("_C")))
		{
			const FString Retry = Path + TEXT("_C");
			Obj = LoadObject<UObject>(nullptr, *Retry);
		}
		return Obj;
	}

	/**
	 * Build the wire object for FEdGraphTerminalType (map value-type slot). Container flags are
	 * intentionally omitted — UE 5.7 disallows containers-of-containers in K2 pin types, so a Map's
	 * value field is always scalar/struct/object.
	 *
	 * Returns nullptr on unsupported terminal category; caller propagates the error.
	 */
	TSharedPtr<FJsonObject> TerminalTypeToJson(
		const FEdGraphTerminalType& Terminal,
		int32& OutErrorCode,
		FString& OutError)
	{
		const FString CatWire = FMCPPinTypeUtils::CategoryToWire(Terminal.TerminalCategory);
		if (CatWire.IsEmpty())
		{
			OutErrorCode = kMCPErrorPinTypeUnsupported;
			OutError = FString::Printf(
				TEXT("pin map-value type '%s' not supported by MCPPinTypeUtils; see failure_modes"),
				*Terminal.TerminalCategory.ToString());
			return nullptr;
		}

		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("category"), CatWire);
		Obj->SetStringField(TEXT("subcategory"), Terminal.TerminalSubCategory.ToString());

		if (Terminal.TerminalSubCategoryObject.IsValid())
		{
			Obj->SetStringField(TEXT("subcategory_object_path"), Terminal.TerminalSubCategoryObject->GetPathName());
		}
		else
		{
			Obj->SetField(TEXT("subcategory_object_path"), MakeShared<FJsonValueNull>());
		}
		Obj->SetBoolField(TEXT("is_weak_pointer"), Terminal.bTerminalIsWeakPointer);
		Obj->SetBoolField(TEXT("is_const"),        Terminal.bTerminalIsConst);
		return Obj;
	}

	/**
	 * Parse a JSON terminal-type object (the inner ``value_type`` for Map pins). Required when the
	 * outer pin reports ``is_map=true``.
	 */
	bool TerminalTypeFromJson(
		const TSharedPtr<FJsonObject>& Obj,
		FEdGraphTerminalType& OutTerminal,
		int32& OutErrorCode,
		FString& OutError)
	{
		if (!Obj.IsValid())
		{
			OutErrorCode = kMCPErrorPinTypeUnsupported;
			OutError = TEXT("map pin type missing 'value_type' object");
			return false;
		}

		FString CatWire;
		if (!Obj->TryGetStringField(TEXT("category"), CatWire) || CatWire.IsEmpty())
		{
			OutErrorCode = kMCPErrorPinTypeUnsupported;
			OutError = TEXT("map 'value_type' missing required string field 'category'");
			return false;
		}
		const FName Category = FMCPPinTypeUtils::WireToCategory(CatWire);
		if (Category == NAME_None)
		{
			OutErrorCode = kMCPErrorPinTypeUnsupported;
			OutError = FString::Printf(
				TEXT("map 'value_type.category' '%s' not supported by MCPPinTypeUtils; see failure_modes"),
				*CatWire);
			return false;
		}
		OutTerminal.TerminalCategory = Category;

		FString SubCat;
		if (Obj->TryGetStringField(TEXT("subcategory"), SubCat) && !SubCat.IsEmpty())
		{
			OutTerminal.TerminalSubCategory = FName(*SubCat);
		}

		FString SubObjPath;
		if (Obj->TryGetStringField(TEXT("subcategory_object_path"), SubObjPath) && !SubObjPath.IsEmpty())
		{
			UObject* Resolved = TryLoadSubcategoryObject(SubObjPath);
			if (!Resolved)
			{
				OutErrorCode = kMCPErrorObjectNotFound;
				OutError = FString::Printf(
					TEXT("map 'value_type.subcategory_object_path' '%s' could not be resolved"),
					*SubObjPath);
				return false;
			}
			OutTerminal.TerminalSubCategoryObject = Resolved;
		}
		else if (CategoryRequiresSubcategoryObject(Category))
		{
			OutErrorCode = kMCPErrorPinTypeUnsupported;
			OutError = FString::Printf(
				TEXT("map 'value_type.category' '%s' requires non-empty subcategory_object_path"),
				*CatWire);
			return false;
		}

		bool bWeak = false;
		Obj->TryGetBoolField(TEXT("is_weak_pointer"), bWeak);
		OutTerminal.bTerminalIsWeakPointer = bWeak;

		bool bConst = false;
		Obj->TryGetBoolField(TEXT("is_const"), bConst);
		OutTerminal.bTerminalIsConst = bConst;
		return true;
	}
} // namespace

namespace FMCPPinTypeUtils
{

FString CategoryToWire(const FName& PinCategory)
{
	for (int32 i = 0; i < GetCategoryTableSize(); ++i)
	{
		const FPinCategoryRow& Row = GetCategoryTable(i);
		if (Row.Category && *Row.Category == PinCategory)
		{
			return FString(Row.Wire);
		}
	}
	return FString();
}

FName WireToCategory(const FString& Wire)
{
	for (int32 i = 0; i < GetCategoryTableSize(); ++i)
	{
		const FPinCategoryRow& Row = GetCategoryTable(i);
		if (Row.Wire && Wire.Equals(Row.Wire, ESearchCase::CaseSensitive))
		{
			return Row.Category ? *Row.Category : NAME_None;
		}
	}
	return NAME_None;
}

TSharedPtr<FJsonObject> ToJson(
	const FEdGraphPinType& PinType,
	int32& OutErrorCode,
	FString& OutError)
{
	const FString CatWire = CategoryToWire(PinType.PinCategory);
	if (CatWire.IsEmpty())
	{
		OutErrorCode = kMCPErrorPinTypeUnsupported;
		OutError = FString::Printf(
			TEXT("pin type category '%s' not supported by MCPPinTypeUtils; see failure_modes"),
			*PinType.PinCategory.ToString());
		return nullptr;
	}

	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("category"), CatWire);
	Obj->SetStringField(TEXT("subcategory"), PinType.PinSubCategory.ToString());

	if (PinType.PinSubCategoryObject.IsValid())
	{
		Obj->SetStringField(TEXT("subcategory_object_path"), PinType.PinSubCategoryObject->GetPathName());
	}
	else
	{
		Obj->SetField(TEXT("subcategory_object_path"), MakeShared<FJsonValueNull>());
	}

	const bool bIsArray = (PinType.ContainerType == EPinContainerType::Array);
	const bool bIsSet   = (PinType.ContainerType == EPinContainerType::Set);
	const bool bIsMap   = (PinType.ContainerType == EPinContainerType::Map);
	Obj->SetBoolField(TEXT("is_array"), bIsArray);
	Obj->SetBoolField(TEXT("is_set"),   bIsSet);
	Obj->SetBoolField(TEXT("is_map"),   bIsMap);

	Obj->SetBoolField(TEXT("is_reference"),    PinType.bIsReference);
	Obj->SetBoolField(TEXT("is_const"),        PinType.bIsConst);
	Obj->SetBoolField(TEXT("is_weak_pointer"), PinType.bIsWeakPointer);

	if (bIsMap)
	{
		int32 ValueErr = 0;
		FString ValueErrMsg;
		TSharedPtr<FJsonObject> ValueObj = TerminalTypeToJson(PinType.PinValueType, ValueErr, ValueErrMsg);
		if (!ValueObj.IsValid())
		{
			OutErrorCode = ValueErr;
			OutError = ValueErrMsg;
			return nullptr;
		}
		Obj->SetObjectField(TEXT("value_type"), ValueObj);
	}
	else
	{
		Obj->SetField(TEXT("value_type"), MakeShared<FJsonValueNull>());
	}

	return Obj;
}

bool FromJson(
	const TSharedPtr<FJsonObject>& Obj,
	FEdGraphPinType& OutPinType,
	int32& OutErrorCode,
	FString& OutError)
{
	if (!Obj.IsValid())
	{
		OutErrorCode = kMCPErrorPinTypeUnsupported;
		OutError = TEXT("pin_type JSON object is missing");
		return false;
	}

	FString CatWire;
	if (!Obj->TryGetStringField(TEXT("category"), CatWire) || CatWire.IsEmpty())
	{
		OutErrorCode = kMCPErrorPinTypeUnsupported;
		OutError = TEXT("pin_type missing required string field 'category'");
		return false;
	}
	const FName Category = WireToCategory(CatWire);
	if (Category == NAME_None)
	{
		OutErrorCode = kMCPErrorPinTypeUnsupported;
		OutError = FString::Printf(
			TEXT("pin_type category '%s' not supported by MCPPinTypeUtils; see failure_modes"),
			*CatWire);
		return false;
	}

	OutPinType.ResetToDefaults();
	OutPinType.PinCategory = Category;

	FString SubCat;
	if (Obj->TryGetStringField(TEXT("subcategory"), SubCat) && !SubCat.IsEmpty())
	{
		OutPinType.PinSubCategory = FName(*SubCat);
	}

	FString SubObjPath;
	if (Obj->TryGetStringField(TEXT("subcategory_object_path"), SubObjPath) && !SubObjPath.IsEmpty())
	{
		UObject* Resolved = TryLoadSubcategoryObject(SubObjPath);
		if (!Resolved)
		{
			OutErrorCode = kMCPErrorObjectNotFound;
			OutError = FString::Printf(
				TEXT("pin_type subcategory_object_path '%s' could not be resolved"),
				*SubObjPath);
			return false;
		}
		OutPinType.PinSubCategoryObject = Resolved;
	}
	else if (CategoryRequiresSubcategoryObject(Category))
	{
		OutErrorCode = kMCPErrorPinTypeUnsupported;
		OutError = FString::Printf(
			TEXT("pin_type category '%s' requires non-empty subcategory_object_path"),
			*CatWire);
		return false;
	}

	bool bIsArray = false;
	bool bIsSet   = false;
	bool bIsMap   = false;
	Obj->TryGetBoolField(TEXT("is_array"), bIsArray);
	Obj->TryGetBoolField(TEXT("is_set"),   bIsSet);
	Obj->TryGetBoolField(TEXT("is_map"),   bIsMap);

	const int32 ContainerCount = (bIsArray ? 1 : 0) + (bIsSet ? 1 : 0) + (bIsMap ? 1 : 0);
	if (ContainerCount > 1)
	{
		OutErrorCode = kMCPErrorPinTypeUnsupported;
		OutError = TEXT("pin_type can have at most one of {is_array, is_set, is_map} = true");
		return false;
	}

	if (bIsArray)      { OutPinType.ContainerType = EPinContainerType::Array; }
	else if (bIsSet)   { OutPinType.ContainerType = EPinContainerType::Set; }
	else if (bIsMap)   { OutPinType.ContainerType = EPinContainerType::Map; }
	else               { OutPinType.ContainerType = EPinContainerType::None; }

	bool bIsRef  = false;
	bool bIsCst  = false;
	bool bIsWeak = false;
	Obj->TryGetBoolField(TEXT("is_reference"),    bIsRef);
	Obj->TryGetBoolField(TEXT("is_const"),        bIsCst);
	Obj->TryGetBoolField(TEXT("is_weak_pointer"), bIsWeak);
	OutPinType.bIsReference   = bIsRef;
	OutPinType.bIsConst       = bIsCst;
	OutPinType.bIsWeakPointer = bIsWeak;

	if (bIsMap)
	{
		const TSharedPtr<FJsonObject>* ValueObj = nullptr;
		if (!Obj->TryGetObjectField(TEXT("value_type"), ValueObj) || !ValueObj || !ValueObj->IsValid())
		{
			OutErrorCode = kMCPErrorPinTypeUnsupported;
			OutError = TEXT("pin_type with is_map=true requires a non-null 'value_type' object");
			return false;
		}
		FEdGraphTerminalType ValueTerminal;
		if (!TerminalTypeFromJson(*ValueObj, ValueTerminal, OutErrorCode, OutError))
		{
			return false;
		}
		OutPinType.PinValueType = ValueTerminal;
	}

	return true;
}

} // namespace FMCPPinTypeUtils
