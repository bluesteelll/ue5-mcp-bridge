// Copyright FatumGame. All Rights Reserved.

#include "MCPPropertyPathParser.h"

namespace FMCPPropertyPathParser
{

namespace
{
	bool IsIdentStartChar(TCHAR C)
	{
		return (C >= TEXT('A') && C <= TEXT('Z'))
			|| (C >= TEXT('a') && C <= TEXT('z'))
			|| C == TEXT('_');
	}
	bool IsIdentRestChar(TCHAR C)
	{
		return IsIdentStartChar(C)
			|| (C >= TEXT('0') && C <= TEXT('9'));
	}
}

bool Parse(const FString& Path, TArray<FPropertyPathStep>& OutSteps, FString& OutError)
{
	OutSteps.Reset();
	OutError.Reset();

	FString Trimmed = Path;
	Trimmed.TrimStartAndEndInline();
	if (Trimmed.IsEmpty())
	{
		return true;
	}

	const TCHAR* Buf = *Trimmed;
	const int32 Len = Trimmed.Len();
	int32 i = 0;

	while (i < Len)
	{
		// Identifier
		const int32 IdentStart = i;
		if (!IsIdentStartChar(Buf[i]))
		{
			OutError = FString::Printf(
				TEXT("expected identifier at offset %d in '%s' (got '%c')"),
				i, *Path, Buf[i]);
			return false;
		}
		++i;
		while (i < Len && IsIdentRestChar(Buf[i]))
		{
			++i;
		}
		const int32 IdentLen = i - IdentStart;

		FPropertyPathStep Step;
		Step.PropName = FString::ConstructFromPtrSize(Buf + IdentStart, IdentLen);

		// Optional array index
		if (i < Len && Buf[i] == TEXT('['))
		{
			++i; // consume '['
			const int32 DigitStart = i;
			while (i < Len && Buf[i] >= TEXT('0') && Buf[i] <= TEXT('9'))
			{
				++i;
			}
			const int32 DigitLen = i - DigitStart;
			if (DigitLen == 0)
			{
				OutError = FString::Printf(
					TEXT("expected digits at offset %d in '%s' inside array index"),
					DigitStart, *Path);
				return false;
			}
			if (i >= Len || Buf[i] != TEXT(']'))
			{
				OutError = FString::Printf(
					TEXT("expected ']' at offset %d in '%s' to close array index"),
					i, *Path);
				return false;
			}
			++i; // consume ']'

			// Parse digits → int32. Guard overflow (>9 digits is suspicious; explicit check below).
			const FString DigitStr = FString::ConstructFromPtrSize(Buf + DigitStart, DigitLen);
			const int64 Parsed = FCString::Atoi64(*DigitStr);
			if (Parsed < 0 || Parsed > static_cast<int64>(MAX_int32))
			{
				OutError = FString::Printf(
					TEXT("array index '%s' out of int32 range at offset %d in '%s'"),
					*DigitStr, DigitStart, *Path);
				return false;
			}
			Step.ArrayIndex = static_cast<int32>(Parsed);
		}

		OutSteps.Add(MoveTemp(Step));

		// Separator '.' or end
		if (i >= Len)
		{
			break;
		}
		if (Buf[i] != TEXT('.'))
		{
			OutError = FString::Printf(
				TEXT("expected '.' or end at offset %d in '%s' (got '%c')"),
				i, *Path, Buf[i]);
			return false;
		}
		++i; // consume '.'

		// Trailing '.' is a syntax error.
		if (i >= Len)
		{
			OutError = FString::Printf(
				TEXT("trailing '.' at offset %d in '%s'"), i - 1, *Path);
			return false;
		}
	}

	return true;
}

FString FormatPath(const TArray<FPropertyPathStep>& Steps)
{
	FString Out;
	Out.Reserve(Steps.Num() * 16);
	for (int32 i = 0; i < Steps.Num(); ++i)
	{
		if (i > 0)
		{
			Out.AppendChar(TEXT('.'));
		}
		Out.Append(Steps[i].PropName);
		if (Steps[i].ArrayIndex != INDEX_NONE)
		{
			Out.Appendf(TEXT("[%d]"), Steps[i].ArrayIndex);
		}
	}
	return Out;
}

} // namespace FMCPPropertyPathParser
