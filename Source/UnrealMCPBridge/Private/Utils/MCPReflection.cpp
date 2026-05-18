// Copyright FatumGame. All Rights Reserved.

#include "MCPReflection.h"

#include "MCPTypes.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/OutputDeviceNull.h"
#include "UObject/Class.h"
#include "UObject/EnumProperty.h"
#include "UObject/Field.h"
#include "UObject/Object.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/TextProperty.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	// ---------------------------------------------------------------------------------------------
	// Forward declarations for the mutually recursive walkers (read + write).
	// ---------------------------------------------------------------------------------------------
	TSharedPtr<FJsonValue> PropertyValueToJson(const FProperty* Prop, const void* ValuePtr);
	bool JsonToPropertyValue(const FProperty* Prop, void* ValuePtr, const TSharedPtr<FJsonValue>& Value,
		UObject* OwnerObject, FString& OutError);

	/**
	 * Emit one of the symmetry structs (Vector / Rotator / Transform / LinearColor / Quat) by name,
	 * reading concrete fields out of the struct memory. Returns nullptr if Struct is not recognised
	 * — caller falls back to the generic recursive walk.
	 *
	 * IMPORTANT: All vector/rotator math types in UE 5+ are stored as DOUBLE (FVector3d). We cast
	 * through doubles to JSON numbers — JSON numbers are encoded as double anyway so no precision
	 * loss. FLinearColor remains float-based.
	 */
	TSharedPtr<FJsonValue> TryEmitKnownStructJson(const UScriptStruct* Struct, const void* StructPtr)
	{
		if (!Struct || !StructPtr)
		{
			return nullptr;
		}

		const FName StructName = Struct->GetFName();

		if (StructName == NAME_Vector)
		{
			const FVector& V = *static_cast<const FVector*>(StructPtr);
			TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("_kind"), TEXT("Vector"));
			Obj->SetNumberField(TEXT("x"), V.X);
			Obj->SetNumberField(TEXT("y"), V.Y);
			Obj->SetNumberField(TEXT("z"), V.Z);
			return MakeShared<FJsonValueObject>(Obj);
		}
		if (StructName == NAME_Rotator)
		{
			const FRotator& R = *static_cast<const FRotator*>(StructPtr);
			TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("_kind"), TEXT("Rotator"));
			Obj->SetNumberField(TEXT("pitch"), R.Pitch);
			Obj->SetNumberField(TEXT("yaw"), R.Yaw);
			Obj->SetNumberField(TEXT("roll"), R.Roll);
			return MakeShared<FJsonValueObject>(Obj);
		}
		if (StructName == NAME_Quat)
		{
			const FQuat& Q = *static_cast<const FQuat*>(StructPtr);
			TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("_kind"), TEXT("Quat"));
			Obj->SetNumberField(TEXT("x"), Q.X);
			Obj->SetNumberField(TEXT("y"), Q.Y);
			Obj->SetNumberField(TEXT("z"), Q.Z);
			Obj->SetNumberField(TEXT("w"), Q.W);
			return MakeShared<FJsonValueObject>(Obj);
		}
		if (StructName == NAME_Transform)
		{
			const FTransform& T = *static_cast<const FTransform*>(StructPtr);
			const FVector Loc = T.GetLocation();
			const FQuat Rot = T.GetRotation();
			const FVector Scale = T.GetScale3D();

			TSharedRef<FJsonObject> TransObj = MakeShared<FJsonObject>();
			TransObj->SetStringField(TEXT("_kind"), TEXT("Vector"));
			TransObj->SetNumberField(TEXT("x"), Loc.X);
			TransObj->SetNumberField(TEXT("y"), Loc.Y);
			TransObj->SetNumberField(TEXT("z"), Loc.Z);

			TSharedRef<FJsonObject> QuatObj = MakeShared<FJsonObject>();
			QuatObj->SetStringField(TEXT("_kind"), TEXT("Quat"));
			QuatObj->SetNumberField(TEXT("x"), Rot.X);
			QuatObj->SetNumberField(TEXT("y"), Rot.Y);
			QuatObj->SetNumberField(TEXT("z"), Rot.Z);
			QuatObj->SetNumberField(TEXT("w"), Rot.W);

			TSharedRef<FJsonObject> ScaleObj = MakeShared<FJsonObject>();
			ScaleObj->SetStringField(TEXT("_kind"), TEXT("Vector"));
			ScaleObj->SetNumberField(TEXT("x"), Scale.X);
			ScaleObj->SetNumberField(TEXT("y"), Scale.Y);
			ScaleObj->SetNumberField(TEXT("z"), Scale.Z);

			TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("_kind"), TEXT("Transform"));
			Obj->SetObjectField(TEXT("translation"), TransObj);
			Obj->SetObjectField(TEXT("rotation"), QuatObj);
			Obj->SetObjectField(TEXT("scale"), ScaleObj);
			return MakeShared<FJsonValueObject>(Obj);
		}
		// FLinearColor / FColor / FBox / etc. — recognised by FName string compare.
		if (StructName == TEXT("LinearColor"))
		{
			const FLinearColor& C = *static_cast<const FLinearColor*>(StructPtr);
			TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("_kind"), TEXT("LinearColor"));
			Obj->SetNumberField(TEXT("r"), C.R);
			Obj->SetNumberField(TEXT("g"), C.G);
			Obj->SetNumberField(TEXT("b"), C.B);
			Obj->SetNumberField(TEXT("a"), C.A);
			return MakeShared<FJsonValueObject>(Obj);
		}

		return nullptr;
	}

	/**
	 * Generic struct walker — fallback for any UScriptStruct not in the known-types table.
	 * Walks every FProperty and emits a JSON object keyed by FProperty::GetName().
	 *
	 * Stamps ``_kind=Struct`` + ``_type=<struct path name>`` so the round-trip side can route to
	 * the correct destination type when applicable.
	 */
	TSharedPtr<FJsonValue> StructValueToJsonGeneric(const UScriptStruct* Struct, const void* StructPtr)
	{
		check(Struct);
		check(StructPtr);

		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("_kind"), TEXT("Struct"));
		Obj->SetStringField(TEXT("_type"), Struct->GetStructPathName().ToString());

		// IncludeSuper so inherited fields aren't dropped (USTRUCT inheritance is rare but legal).
		for (TFieldIterator<FProperty> It(Struct, EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			const FProperty* InnerProp = *It;
			const void* InnerValuePtr = InnerProp->ContainerPtrToValuePtr<void>(StructPtr);
			TSharedPtr<FJsonValue> Child = PropertyValueToJson(InnerProp, InnerValuePtr);
			Obj->SetField(InnerProp->GetName(), Child);
		}
		return MakeShared<FJsonValueObject>(Obj);
	}

	/**
	 * Top-level conversion: FProperty value → FJsonValue.
	 *
	 * `ValuePtr` MUST already point at the value memory (i.e. caller did ContainerPtrToValuePtr).
	 * NEVER returns null — on truly unrecognised properties emits ``{"_kind":"Unsupported", ...}``.
	 */
	TSharedPtr<FJsonValue> PropertyValueToJson(const FProperty* Prop, const void* ValuePtr)
	{
		if (!Prop || !ValuePtr)
		{
			return MakeShared<FJsonValueNull>();
		}

		// Bool — separate from numeric because of the bitfield/byte/native-bool indirection.
		if (const FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
		{
			return MakeShared<FJsonValueBoolean>(BoolProp->GetPropertyValue(ValuePtr));
		}

		// Enum — emit as string name so it round-trips through JSON without losing meaning.
		if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
		{
			const FNumericProperty* Underlying = EnumProp->GetUnderlyingProperty();
			const int64 Raw = Underlying->GetSignedIntPropertyValue(ValuePtr);
			const UEnum* Enum = EnumProp->GetEnum();
			TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("_kind"), TEXT("Enum"));
			Obj->SetStringField(TEXT("type"), Enum ? Enum->GetName() : TEXT(""));
			Obj->SetNumberField(TEXT("value"), static_cast<double>(Raw));
			Obj->SetStringField(TEXT("name"), Enum ? Enum->GetNameStringByValue(Raw) : TEXT(""));
			return MakeShared<FJsonValueObject>(Obj);
		}

		// ByteProperty with enum (legacy `TEnumAsByte<>`) — same surface as FEnumProperty.
		if (const FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
		{
			if (ByteProp->Enum)
			{
				const int64 Raw = ByteProp->GetSignedIntPropertyValue(ValuePtr);
				TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
				Obj->SetStringField(TEXT("_kind"), TEXT("Enum"));
				Obj->SetStringField(TEXT("type"), ByteProp->Enum->GetName());
				Obj->SetNumberField(TEXT("value"), static_cast<double>(Raw));
				Obj->SetStringField(TEXT("name"), ByteProp->Enum->GetNameStringByValue(Raw));
				return MakeShared<FJsonValueObject>(Obj);
			}
			return MakeShared<FJsonValueNumber>(static_cast<double>(ByteProp->GetUnsignedIntPropertyValue(ValuePtr)));
		}

		// Numeric (covers Int8/16/32/64, UInt8/16/32/64, Float, Double).
		if (const FNumericProperty* NumProp = CastField<FNumericProperty>(Prop))
		{
			if (NumProp->IsFloatingPoint())
			{
				return MakeShared<FJsonValueNumber>(NumProp->GetFloatingPointPropertyValue(ValuePtr));
			}
			// Cast through double — JSON numbers are doubles. Integers up to 2^53 round-trip exactly.
			return MakeShared<FJsonValueNumber>(static_cast<double>(NumProp->GetSignedIntPropertyValue(ValuePtr)));
		}

		if (const FStrProperty* StrProp = CastField<FStrProperty>(Prop))
		{
			return MakeShared<FJsonValueString>(StrProp->GetPropertyValue(ValuePtr));
		}

		if (const FNameProperty* NameProp = CastField<FNameProperty>(Prop))
		{
			TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("_kind"), TEXT("Name"));
			Obj->SetStringField(TEXT("value"), NameProp->GetPropertyValue(ValuePtr).ToString());
			return MakeShared<FJsonValueObject>(Obj);
		}

		if (const FTextProperty* TextProp = CastField<FTextProperty>(Prop))
		{
			const FText& Value = TextProp->GetPropertyValue(ValuePtr);
			TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("_kind"), TEXT("Text"));
			Obj->SetStringField(TEXT("value"), Value.ToString());
			return MakeShared<FJsonValueObject>(Obj);
		}

		// Soft object reference — emit path string so the client can rehydrate via TryLoad().
		if (const FSoftObjectProperty* SoftProp = CastField<FSoftObjectProperty>(Prop))
		{
			const FSoftObjectPtr& Soft = SoftProp->GetPropertyValue(ValuePtr);
			TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("_kind"), TEXT("SoftObjectPath"));
			Obj->SetStringField(TEXT("value"), Soft.ToString());
			return MakeShared<FJsonValueObject>(Obj);
		}

		// Hard UObject* reference — emit asset path + class so caller can inspect or rebind.
		if (const FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop))
		{
			UObject* Inner = ObjProp->GetObjectPropertyValue(ValuePtr);
			if (!Inner)
			{
				return MakeShared<FJsonValueNull>();
			}
			TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("_kind"), TEXT("ObjectRef"));
			Obj->SetStringField(TEXT("path"), Inner->GetPathName());
			Obj->SetStringField(TEXT("class"), Inner->GetClass()->GetName());
			return MakeShared<FJsonValueObject>(Obj);
		}

		if (const FStructProperty* StructProp = CastField<FStructProperty>(Prop))
		{
			if (TSharedPtr<FJsonValue> Known = TryEmitKnownStructJson(StructProp->Struct, ValuePtr))
			{
				return Known;
			}
			return StructValueToJsonGeneric(StructProp->Struct, ValuePtr);
		}

		if (const FArrayProperty* ArrProp = CastField<FArrayProperty>(Prop))
		{
			TArray<TSharedPtr<FJsonValue>> Items;
			FScriptArrayHelper Helper(ArrProp, ValuePtr);
			const int32 Count = Helper.Num();
			Items.Reserve(Count);
			for (int32 i = 0; i < Count; ++i)
			{
				const void* ElemPtr = Helper.GetRawPtr(i);
				Items.Add(PropertyValueToJson(ArrProp->Inner, ElemPtr));
			}
			return MakeShared<FJsonValueArray>(Items);
		}

		if (const FSetProperty* SetProp = CastField<FSetProperty>(Prop))
		{
			TArray<TSharedPtr<FJsonValue>> Items;
			FScriptSetHelper Helper(SetProp, ValuePtr);
			// FScriptSetHelper uses sparse storage — must iterate by index AND check IsValidIndex.
			const int32 MaxIndex = Helper.GetMaxIndex();
			Items.Reserve(Helper.Num());
			for (int32 i = 0; i < MaxIndex; ++i)
			{
				if (!Helper.IsValidIndex(i))
				{
					continue;
				}
				const void* ElemPtr = Helper.GetElementPtr(i);
				Items.Add(PropertyValueToJson(SetProp->ElementProp, ElemPtr));
			}
			return MakeShared<FJsonValueArray>(Items);
		}

		if (const FMapProperty* MapProp = CastField<FMapProperty>(Prop))
		{
			TArray<TSharedPtr<FJsonValue>> Pairs;
			FScriptMapHelper Helper(MapProp, ValuePtr);
			const int32 MaxIndex = Helper.GetMaxIndex();
			Pairs.Reserve(Helper.Num());
			for (int32 i = 0; i < MaxIndex; ++i)
			{
				if (!Helper.IsValidIndex(i))
				{
					continue;
				}
				const uint8* KeyPtr = Helper.GetKeyPtr(i);
				const uint8* ValuePtrInner = Helper.GetValuePtr(i);
				TSharedRef<FJsonObject> PairObj = MakeShared<FJsonObject>();
				PairObj->SetField(TEXT("key"), PropertyValueToJson(MapProp->KeyProp, KeyPtr));
				PairObj->SetField(TEXT("value"), PropertyValueToJson(MapProp->ValueProp, ValuePtrInner));
				Pairs.Add(MakeShared<FJsonValueObject>(PairObj));
			}
			TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("_kind"), TEXT("Map"));
			Obj->SetArrayField(TEXT("pairs"), Pairs);
			return MakeShared<FJsonValueObject>(Obj);
		}

		// Final fallback — text-export for anything we don't have an explicit branch for.
		// Covers delegate/interface/etc. on a best-effort basis; client sees the raw stringification.
		FString TextValue;
		Prop->ExportTextItem_Direct(TextValue, ValuePtr, nullptr, nullptr, PPF_None);
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("_kind"), TEXT("Unsupported"));
		Obj->SetStringField(TEXT("type"), FMCPReflection::DescribePropertyType(Prop));
		Obj->SetStringField(TEXT("text"), TextValue);
		return MakeShared<FJsonValueObject>(Obj);
	}

	/**
	 * JSON → FProperty value writer. Delegates known-kind dicts to typed branches, and falls back
	 * to text-import (`ImportText_Direct`) when the supplied JSON is a raw string.
	 *
	 * Returns false on type mismatch / parse failure; OutError gets a human-readable diagnostic.
	 * Caller surfaces that as the message of an FMCPResponse error (-32006).
	 */
	bool JsonToPropertyValue(const FProperty* Prop, void* ValuePtr, const TSharedPtr<FJsonValue>& Value,
		UObject* OwnerObject, FString& OutError)
	{
		if (!Prop || !ValuePtr)
		{
			OutError = TEXT("null property or destination");
			return false;
		}
		if (!Value.IsValid())
		{
			OutError = TEXT("null json value");
			return false;
		}

		// Bool — accept JSON bool only (numeric coercion is too footgun-prone).
		if (const FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
		{
			bool b = false;
			if (!Value->TryGetBool(b))
			{
				OutError = TEXT("expected json bool");
				return false;
			}
			BoolProp->SetPropertyValue(ValuePtr, b);
			return true;
		}

		// Enum — accept either {"_kind":"Enum","name":"..."} (preferred) or {"value":N} or raw int.
		auto SetEnumByPayload = [&OutError](const FEnumProperty* EnumProp, void* ValuePtrInner,
			const TSharedPtr<FJsonValue>& InValue) -> bool
		{
			const FNumericProperty* Underlying = EnumProp->GetUnderlyingProperty();
			const UEnum* Enum = EnumProp->GetEnum();

			// Raw number form.
			double NumVal = 0.0;
			if (InValue->TryGetNumber(NumVal))
			{
				Underlying->SetIntPropertyValue(ValuePtrInner, static_cast<int64>(NumVal));
				return true;
			}
			// Object form with "name" or "value".
			const TSharedPtr<FJsonObject>* ObjPtr = nullptr;
			if (InValue->TryGetObject(ObjPtr) && ObjPtr && (*ObjPtr).IsValid())
			{
				const TSharedPtr<FJsonObject>& Obj = *ObjPtr;
				FString NameStr;
				if (Obj->TryGetStringField(TEXT("name"), NameStr) && Enum && !NameStr.IsEmpty())
				{
					const int64 ByName = Enum->GetValueByNameString(NameStr);
					if (ByName == INDEX_NONE)
					{
						OutError = FString::Printf(TEXT("enum '%s' has no entry '%s'"),
							*Enum->GetName(), *NameStr);
						return false;
					}
					Underlying->SetIntPropertyValue(ValuePtrInner, ByName);
					return true;
				}
				double Inner = 0.0;
				if (Obj->TryGetNumberField(TEXT("value"), Inner))
				{
					Underlying->SetIntPropertyValue(ValuePtrInner, static_cast<int64>(Inner));
					return true;
				}
			}
			OutError = TEXT("expected enum json (number, or {name:...}, or {value:N})");
			return false;
		};
		if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
		{
			return SetEnumByPayload(EnumProp, ValuePtr, Value);
		}

		// ByteProperty — both raw byte AND enum-as-byte modes.
		if (const FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
		{
			if (ByteProp->Enum)
			{
				// Reuse the enum lambda by faking an FEnumProperty path — but Underlying differs.
				// Inline the equivalent logic for clarity:
				double NumVal = 0.0;
				if (Value->TryGetNumber(NumVal))
				{
					ByteProp->SetIntPropertyValue(ValuePtr, static_cast<uint64>(NumVal));
					return true;
				}
				const TSharedPtr<FJsonObject>* ObjPtr = nullptr;
				if (Value->TryGetObject(ObjPtr) && ObjPtr && (*ObjPtr).IsValid())
				{
					FString NameStr;
					if ((*ObjPtr)->TryGetStringField(TEXT("name"), NameStr) && !NameStr.IsEmpty())
					{
						const int64 ByName = ByteProp->Enum->GetValueByNameString(NameStr);
						if (ByName == INDEX_NONE)
						{
							OutError = FString::Printf(TEXT("byte-enum '%s' has no entry '%s'"),
								*ByteProp->Enum->GetName(), *NameStr);
							return false;
						}
						ByteProp->SetIntPropertyValue(ValuePtr, static_cast<uint64>(ByName));
						return true;
					}
					double Inner = 0.0;
					if ((*ObjPtr)->TryGetNumberField(TEXT("value"), Inner))
					{
						ByteProp->SetIntPropertyValue(ValuePtr, static_cast<uint64>(Inner));
						return true;
					}
				}
				OutError = TEXT("expected byte-enum json (number or {name:...} or {value:N})");
				return false;
			}
			// Raw byte path.
			double NumVal = 0.0;
			if (!Value->TryGetNumber(NumVal))
			{
				OutError = TEXT("expected number for FByteProperty");
				return false;
			}
			ByteProp->SetIntPropertyValue(ValuePtr, static_cast<uint64>(NumVal));
			return true;
		}

		// Numeric.
		if (const FNumericProperty* NumProp = CastField<FNumericProperty>(Prop))
		{
			double NumVal = 0.0;
			if (!Value->TryGetNumber(NumVal))
			{
				OutError = TEXT("expected number");
				return false;
			}
			if (NumProp->IsFloatingPoint())
			{
				NumProp->SetFloatingPointPropertyValue(ValuePtr, NumVal);
			}
			else
			{
				NumProp->SetIntPropertyValue(ValuePtr, static_cast<int64>(NumVal));
			}
			return true;
		}

		// String / Name / Text.
		if (const FStrProperty* StrProp = CastField<FStrProperty>(Prop))
		{
			FString S;
			if (!Value->TryGetString(S))
			{
				OutError = TEXT("expected string");
				return false;
			}
			StrProp->SetPropertyValue(ValuePtr, S);
			return true;
		}
		if (const FNameProperty* NameProp = CastField<FNameProperty>(Prop))
		{
			FString S;
			if (Value->TryGetString(S))
			{
				NameProp->SetPropertyValue(ValuePtr, FName(*S));
				return true;
			}
			const TSharedPtr<FJsonObject>* ObjPtr = nullptr;
			if (Value->TryGetObject(ObjPtr) && ObjPtr && (*ObjPtr).IsValid())
			{
				FString Inner;
				if ((*ObjPtr)->TryGetStringField(TEXT("value"), Inner))
				{
					NameProp->SetPropertyValue(ValuePtr, FName(*Inner));
					return true;
				}
			}
			OutError = TEXT("expected string or {_kind:Name,value:...}");
			return false;
		}
		if (const FTextProperty* TextProp = CastField<FTextProperty>(Prop))
		{
			FString S;
			if (Value->TryGetString(S))
			{
				TextProp->SetPropertyValue(ValuePtr, FText::FromString(S));
				return true;
			}
			const TSharedPtr<FJsonObject>* ObjPtr = nullptr;
			if (Value->TryGetObject(ObjPtr) && ObjPtr && (*ObjPtr).IsValid())
			{
				FString Inner;
				if ((*ObjPtr)->TryGetStringField(TEXT("value"), Inner))
				{
					TextProp->SetPropertyValue(ValuePtr, FText::FromString(Inner));
					return true;
				}
			}
			OutError = TEXT("expected string or {_kind:Text,value:...}");
			return false;
		}

		// Soft object path.
		if (const FSoftObjectProperty* SoftProp = CastField<FSoftObjectProperty>(Prop))
		{
			FString PathStr;
			if (!Value->TryGetString(PathStr))
			{
				const TSharedPtr<FJsonObject>* ObjPtr = nullptr;
				if (Value->TryGetObject(ObjPtr) && ObjPtr && (*ObjPtr).IsValid())
				{
					(*ObjPtr)->TryGetStringField(TEXT("value"), PathStr);
				}
			}
			if (PathStr.IsEmpty())
			{
				OutError = TEXT("expected string or {_kind:SoftObjectPath,value:...}");
				return false;
			}
			// FSoftObjectPtr has no operator=(FSoftObjectPath), so construct from FSoftObjectPath
			// then pass by const-ref. Brace-init to avoid the most-vexing-parse interpreting this
			// as a function declaration.
			const FSoftObjectPath SoftPath(PathStr);
			const FSoftObjectPtr NewVal{SoftPath};
			SoftProp->SetPropertyValue(ValuePtr, NewVal);
			return true;
		}

		// Hard object reference — resolve path string then set pointer.
		if (const FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop))
		{
			FString PathStr;
			if (!Value->TryGetString(PathStr))
			{
				const TSharedPtr<FJsonObject>* ObjPtr = nullptr;
				if (Value->TryGetObject(ObjPtr) && ObjPtr && (*ObjPtr).IsValid())
				{
					(*ObjPtr)->TryGetStringField(TEXT("path"), PathStr);
				}
			}
			if (PathStr.IsEmpty())
			{
				// Null assignment is legitimate.
				ObjProp->SetObjectPropertyValue(ValuePtr, nullptr);
				return true;
			}
			UObject* Resolved = FMCPReflection::ResolveObjectPath(PathStr);
			if (!Resolved)
			{
				OutError = FString::Printf(TEXT("object path could not be resolved: %s"), *PathStr);
				return false;
			}
			if (ObjProp->PropertyClass && !Resolved->IsA(ObjProp->PropertyClass))
			{
				OutError = FString::Printf(TEXT("resolved object class %s incompatible with expected %s"),
					*Resolved->GetClass()->GetName(), *ObjProp->PropertyClass->GetName());
				return false;
			}
			ObjProp->SetObjectPropertyValue(ValuePtr, Resolved);
			return true;
		}

		// Struct — accept either known _kind discriminator dict OR raw field dict.
		if (const FStructProperty* StructProp = CastField<FStructProperty>(Prop))
		{
			const TSharedPtr<FJsonObject>* ObjPtr = nullptr;
			if (!Value->TryGetObject(ObjPtr) || !ObjPtr || !(*ObjPtr).IsValid())
			{
				// Fallback: text-import (e.g. caller sent "(X=1,Y=2,Z=3)").
				FString S;
				if (Value->TryGetString(S))
				{
					FOutputDeviceNull DevNull;
					const TCHAR* Result = Prop->ImportText_Direct(*S, ValuePtr, OwnerObject, PPF_None, &DevNull);
					if (!Result)
					{
						OutError = FString::Printf(TEXT("ImportText_Direct rejected struct literal: %s"), *S);
						return false;
					}
					return true;
				}
				OutError = TEXT("expected json object for struct");
				return false;
			}

			const FName StructName = StructProp->Struct->GetFName();
			const TSharedPtr<FJsonObject>& Obj = *ObjPtr;

			if (StructName == NAME_Vector)
			{
				FVector* V = static_cast<FVector*>(ValuePtr);
				double X = V->X, Y = V->Y, Z = V->Z;
				Obj->TryGetNumberField(TEXT("x"), X);
				Obj->TryGetNumberField(TEXT("y"), Y);
				Obj->TryGetNumberField(TEXT("z"), Z);
				V->X = X; V->Y = Y; V->Z = Z;
				return true;
			}
			if (StructName == NAME_Rotator)
			{
				FRotator* R = static_cast<FRotator*>(ValuePtr);
				double P = R->Pitch, Yw = R->Yaw, Rl = R->Roll;
				Obj->TryGetNumberField(TEXT("pitch"), P);
				Obj->TryGetNumberField(TEXT("yaw"), Yw);
				Obj->TryGetNumberField(TEXT("roll"), Rl);
				R->Pitch = P; R->Yaw = Yw; R->Roll = Rl;
				return true;
			}
			if (StructName == NAME_Quat)
			{
				FQuat* Q = static_cast<FQuat*>(ValuePtr);
				double X = Q->X, Y = Q->Y, Z = Q->Z, W = Q->W;
				Obj->TryGetNumberField(TEXT("x"), X);
				Obj->TryGetNumberField(TEXT("y"), Y);
				Obj->TryGetNumberField(TEXT("z"), Z);
				Obj->TryGetNumberField(TEXT("w"), W);
				Q->X = X; Q->Y = Y; Q->Z = Z; Q->W = W;
				return true;
			}
			if (StructName == TEXT("LinearColor"))
			{
				FLinearColor* C = static_cast<FLinearColor*>(ValuePtr);
				double R = C->R, G = C->G, B = C->B, A = C->A;
				Obj->TryGetNumberField(TEXT("r"), R);
				Obj->TryGetNumberField(TEXT("g"), G);
				Obj->TryGetNumberField(TEXT("b"), B);
				Obj->TryGetNumberField(TEXT("a"), A);
				C->R = R; C->G = G; C->B = B; C->A = A;
				return true;
			}

			// Generic struct walker. Match by FProperty::GetName() (PascalCase / project-style).
			for (TFieldIterator<FProperty> It(StructProp->Struct, EFieldIteratorFlags::IncludeSuper); It; ++It)
			{
				const FProperty* InnerProp = *It;
				const TSharedPtr<FJsonValue> ChildVal = Obj->TryGetField(InnerProp->GetName());
				if (!ChildVal.IsValid())
				{
					continue; // Leave field at existing value if not provided.
				}
				void* InnerValuePtr = InnerProp->ContainerPtrToValuePtr<void>(ValuePtr);
				if (!JsonToPropertyValue(InnerProp, InnerValuePtr, ChildVal, OwnerObject, OutError))
				{
					OutError = FString::Printf(TEXT("field '%s': %s"), *InnerProp->GetName(), *OutError);
					return false;
				}
			}
			return true;
		}

		// Arrays / Sets / Maps — round-trip via JSON arrays / object-of-pairs. Truncate-and-fill
		// semantics: caller-supplied length wins.
		if (const FArrayProperty* ArrProp = CastField<FArrayProperty>(Prop))
		{
			const TArray<TSharedPtr<FJsonValue>>* InnerArrPtr = nullptr;
			if (!Value->TryGetArray(InnerArrPtr) || !InnerArrPtr)
			{
				OutError = TEXT("expected json array");
				return false;
			}
			const TArray<TSharedPtr<FJsonValue>>& InnerArr = *InnerArrPtr;
			FScriptArrayHelper Helper(ArrProp, ValuePtr);
			Helper.EmptyAndAddValues(InnerArr.Num());
			for (int32 i = 0; i < InnerArr.Num(); ++i)
			{
				void* ElemPtr = Helper.GetRawPtr(i);
				if (!JsonToPropertyValue(ArrProp->Inner, ElemPtr, InnerArr[i], OwnerObject, OutError))
				{
					OutError = FString::Printf(TEXT("array[%d]: %s"), i, *OutError);
					return false;
				}
			}
			return true;
		}

		// Final fallback — text-import. ImportText_Direct accepts UE's text-property serialisation
		// (e.g. "(X=1,Y=2,Z=3)" for vectors, "5.0" for floats, etc.). Best-effort.
		FString S;
		if (Value->TryGetString(S))
		{
			FOutputDeviceNull DevNull;
			const TCHAR* Result = Prop->ImportText_Direct(*S, ValuePtr, OwnerObject, PPF_None, &DevNull);
			if (!Result)
			{
				OutError = FString::Printf(TEXT("ImportText_Direct failed for type %s with value '%s'"),
					*FMCPReflection::DescribePropertyType(Prop), *S);
				return false;
			}
			return true;
		}

		OutError = FString::Printf(TEXT("no JSON→property converter for type %s"),
			*FMCPReflection::DescribePropertyType(Prop));
		return false;
	}
} // namespace

// =====================================================================================================
// Public API
// =====================================================================================================

namespace FMCPReflection
{
	UObject* ResolveObjectPath(const FString& Path)
	{
		if (Path.IsEmpty())
		{
			return nullptr;
		}

		// Fast path: object is already in memory. Covers transient objects + already-loaded assets.
		if (UObject* Existing = FindObject<UObject>(nullptr, *Path))
		{
			return Existing;
		}

		// Slow path: trigger a load via soft path. Handles uncooked assets sitting on disk that
		// haven't been pulled into memory yet. Editor-only context — synchronous load is fine.
		const FSoftObjectPath Soft(Path);
		if (UObject* Loaded = Soft.TryLoad())
		{
			return Loaded;
		}

		// Last resort: LoadObject<>() in case the path lacks the redundant `.Name` suffix.
		return LoadObject<UObject>(nullptr, *Path);
	}

	const UScriptStruct* ResolveScriptStructPath(const FString& Path)
	{
		if (Path.IsEmpty())
		{
			return nullptr;
		}

		if (const UScriptStruct* Found = FindObject<UScriptStruct>(nullptr, *Path, EFindObjectFlags::ExactClass))
		{
			return Found;
		}
		// Some struct paths only resolve after the package is loaded — fall back to LoadObject.
		return LoadObject<UScriptStruct>(nullptr, *Path);
	}

	FString DescribePropertyType(const FProperty* Prop)
	{
		if (!Prop)
		{
			return TEXT("Unknown");
		}
		FString Extended;
		return Prop->GetCPPType(&Extended, 0) + Extended;
	}

	TSharedRef<FJsonObject> MakePropertySummary(const FProperty* Prop)
	{
		check(Prop);
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Prop->GetName());
		Obj->SetStringField(TEXT("type"), DescribePropertyType(Prop));
		// PropertyFlags is uint64 internally; JSON numbers max at 2^53 — fits comfortably for the
		// bits actually populated by UE. Encode as string fallback would lose readability.
		Obj->SetNumberField(TEXT("flags"), static_cast<double>(Prop->PropertyFlags));
		Obj->SetNumberField(TEXT("offset"), static_cast<double>(Prop->GetOffset_ForInternal()));
		return Obj;
	}

	bool ResolvePropertyPath(
		UObject* RootTarget,
		const FString& DottedPath,
		UObject*& OutContainer,
		void*& OutContainerPtr,
		FProperty*& OutLeafProp,
		int32& OutErrorCode,
		FString& OutError)
	{
		OutContainer = nullptr;
		OutContainerPtr = nullptr;
		OutLeafProp = nullptr;
		OutErrorCode = 0;
		OutError.Reset();

		if (!RootTarget)
		{
			OutErrorCode = kMCPErrorObjectNotFound;
			OutError = TEXT("null root object");
			return false;
		}
		if (DottedPath.IsEmpty())
		{
			OutErrorCode = kMCPErrorPropertyNotFound;
			OutError = TEXT("empty property path");
			return false;
		}

		TArray<FString> Segments;
		DottedPath.ParseIntoArray(Segments, TEXT("."), true);
		if (Segments.Num() == 0)
		{
			OutErrorCode = kMCPErrorPropertyNotFound;
			OutError = TEXT("could not split property path");
			return false;
		}

		// Container state across the walk. Either:
		//   (CurContainer = a UObject*, CurStruct = UObject's class)
		//   (CurContainer = a struct void*, CurStruct = the UScriptStruct*)
		void* CurContainer = static_cast<void*>(RootTarget);
		const UStruct* CurStruct = RootTarget->GetClass();

		FProperty* LeafProp = nullptr;
		void* LeafValuePtr = nullptr;

		for (int32 i = 0; i < Segments.Num(); ++i)
		{
			const FString& Seg = Segments[i];
			if (Seg.IsEmpty())
			{
				OutErrorCode = kMCPErrorPropertyNotFound;
				OutError = FString::Printf(TEXT("empty segment at index %d in '%s'"), i, *DottedPath);
				return false;
			}
			FProperty* Prop = CurStruct->FindPropertyByName(FName(*Seg));
			if (!Prop)
			{
				OutErrorCode = kMCPErrorPropertyNotFound;
				OutError = FString::Printf(TEXT("no property '%s' on %s (path '%s')"),
					*Seg, *CurStruct->GetName(), *DottedPath);
				return false;
			}

			void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(CurContainer);

			const bool bIsLast = (i == Segments.Num() - 1);
			if (bIsLast)
			{
				LeafProp = Prop;
				LeafValuePtr = ValuePtr;
				break;
			}

			// Not last → descend. Must be either object ref or struct ref.
			if (const FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop))
			{
				UObject* Inner = ObjProp->GetObjectPropertyValue(ValuePtr);
				if (!Inner)
				{
					OutErrorCode = kMCPErrorPropertyNotFound;
					OutError = FString::Printf(TEXT("segment '%s' is null UObject — cannot descend"),
						*Seg);
					return false;
				}
				CurContainer = static_cast<void*>(Inner);
				CurStruct = Inner->GetClass();
				continue;
			}
			if (const FStructProperty* StructProp = CastField<FStructProperty>(Prop))
			{
				CurContainer = ValuePtr;
				CurStruct = StructProp->Struct;
				continue;
			}

			OutErrorCode = kMCPErrorPropertyTypeMismatch;
			OutError = FString::Printf(
				TEXT("segment '%s' is %s — only object/struct refs can be intermediate path nodes"),
				*Seg, *DescribePropertyType(Prop));
			return false;
		}

		check(LeafProp);
		check(LeafValuePtr);
		OutContainer = RootTarget;
		OutContainerPtr = LeafValuePtr;
		OutLeafProp = LeafProp;
		return true;
	}

	TSharedPtr<FJsonValue> ReadPropertyValueAt(const FProperty* Prop, const void* ValuePtr)
	{
		return PropertyValueToJson(Prop, ValuePtr);
	}

	bool WritePropertyValueAt(
		FProperty* Prop,
		void* ValuePtr,
		const TSharedPtr<FJsonValue>& Value,
		UObject* OwnerObject,
		FString& OutError)
	{
		return JsonToPropertyValue(Prop, ValuePtr, Value, OwnerObject, OutError);
	}

	TSharedPtr<FJsonValue> ReadPropertyValue(const UObject* Target, const FProperty* Prop)
	{
		if (!Target || !Prop)
		{
			return MakeShared<FJsonValueNull>();
		}
		const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Target);
		return PropertyValueToJson(Prop, ValuePtr);
	}

	bool WritePropertyValue(
		UObject* Target,
		FProperty* Prop,
		const TSharedPtr<FJsonValue>& Value,
		FString& OutError)
	{
		if (!Target)
		{
			OutError = TEXT("null target object");
			return false;
		}
		if (!Prop)
		{
			OutError = TEXT("null property");
			return false;
		}
		void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Target);
		return JsonToPropertyValue(Prop, ValuePtr, Value, Target, OutError);
	}
} // namespace FMCPReflection

// =====================================================================================================
// FMCPWritePropertyScope
// =====================================================================================================

FMCPWritePropertyScope::FMCPWritePropertyScope(UObject* InTarget, FProperty* InProp, const FText& TransactionLabel)
	: Target(InTarget)
	, Prop(InProp)
	, Transaction(TransactionLabel)
{
	check(Target);
	check(Prop);
	Target->PreEditChange(Prop);
	Target->Modify();
}

FMCPWritePropertyScope::~FMCPWritePropertyScope()
{
	// Fire PostEditChangeProperty unconditionally — RAII guarantees this even on early return /
	// exception unwind. Pair with Pre fired in the ctor. The FPropertyChangedEvent uses
	// EPropertyChangeType::Unspecified by default which matches what FScopedTransaction users do.
	FPropertyChangedEvent Evt(Prop);
	Target->PostEditChangeProperty(Evt);
}
