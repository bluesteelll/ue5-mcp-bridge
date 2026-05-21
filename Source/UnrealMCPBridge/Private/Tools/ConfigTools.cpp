// Copyright FatumGame. All Rights Reserved.

#include "ConfigTools.h"

#include "MCPSurfaceRegistry.h"

#include "FMCPDispatchQueue.h"
#include "MCPJsonBuilder.h"
#include "MCPToolHelpers.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPPageCursor.h"

#include "HAL/IConsoleManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// CFG_ prefix per the unity-build symbol-collision pattern. StampIds / MakeError / MakeSuccessObj
	// migrated to FMCPToolHelpers in Phase 3 (Group G3); only the surface-local error-code aliases
	// and pagination/truncation caps live here. cvar/ini mutations are engine-wide and intentionally
	// NOT FMCPMutatorScope-guarded — cfg.set_cvar mirrors the console UX (works in PIE), cfg.write
	// targets DefaultEngine.ini etc. which are session-spanning files orthogonal to PIE state.
	constexpr int32 kCFGErrorInvalidParams = kMCPErrorInvalidParams; // -32602
	constexpr int32 kCFGErrorInternal      = kMCPErrorInternal;      // -32603

	// Default page size for ``cfg.list_cvars`` + ``cfg.list_sections``. Mirrors Phase 4/5/6
	// pagination defaults.
	constexpr int32 kCFGDefaultPageSize = 100;

	// Hard-cap for the first-line of help text returned by ``cfg.list_cvars`` to keep the
	// per-entry summary small. Help text is full-fidelity in ``cfg.get_cvar``.
	constexpr int32 kCFGHelpFirstLineMaxChars = 240;

	// Hard-cap for ``value_summary`` in ``cfg.list_cvars`` per-entry — string-typed cvars may
	// hold paths or comma-separated lists that grow unboundedly; truncate with " ..." suffix.
	constexpr int32 kCFGValueSummaryMaxChars = 240;

	int32 CFG_ClampPageSize(const TSharedPtr<FJsonObject>& Args, const TCHAR* FieldName, int32 Default)
	{
		int32 Out = Default;
		if (Args.IsValid())
		{
			Args->TryGetNumberField(FieldName, Out);
		}
		return FMath::Clamp(Out, 1, 1000);
	}

	bool CFG_DecodeCursor(
		const FMCPRequest& Request,
		const FString& TokenWire,
		uint64 ExpectedFilterHash,
		FMCPPageCursor& OutCursor,
		FMCPResponse& OutError)
	{
		FString DecodeErr;
		if (!FMCPPageCursorUtils::Decode(TokenWire, OutCursor, DecodeErr))
		{
			OutError = FMCPToolHelpers::MakeError(Request, kMCPErrorStaleCursor,
				FString::Printf(TEXT("page_token decode failed: %s"), *DecodeErr));
			return false;
		}
		if (!FMCPPageCursorUtils::ValidateAgainstFilter(OutCursor, ExpectedFilterHash))
		{
			OutError = FMCPToolHelpers::MakeError(Request, kMCPErrorStaleCursor,
				TEXT("page_token filter_hash mismatch — caller mutated 'prefix_filter' or 'ini_file' "
					 "between pages; restart pagination with page_token=null"));
			return false;
		}
		return true;
	}

	uint64 CFG_HashFilter(const FString& Filter)
	{
		const uint32 H1 = GetTypeHash(Filter);
		return static_cast<uint64>(H1);
	}

	uint64 CFG_HashIniFile(const FString& IniBaseName)
	{
		const uint32 H1 = GetTypeHash(IniBaseName);
		return static_cast<uint64>(H1);
	}

	/**
	 * Type-string used in tool results to describe a CVar's value family. We expose 5 categories
	 * mirroring ``IsVariableBool/Int/Float/String`` + a fallback ``"unknown"`` for cvars that
	 * implement IConsoleVariable but report none of the typed Is*() probes (rare — typically
	 * means a custom subclass).
	 */
	const TCHAR* CFG_GetCVarTypeString(const IConsoleVariable& Var)
	{
		if (Var.IsVariableBool())   return TEXT("bool");
		if (Var.IsVariableInt())    return TEXT("int");
		if (Var.IsVariableFloat())  return TEXT("float");
		if (Var.IsVariableString()) return TEXT("string");
		return TEXT("unknown");
	}

	/**
	 * Read the cvar's current value as a JSON value of its native type. For ``"unknown"`` typed
	 * cvars we fall through to GetString to avoid losing data.
	 */
	TSharedRef<FJsonValue> CFG_GetCVarValueJson(const IConsoleVariable& Var)
	{
		if (Var.IsVariableBool())
		{
			return MakeShared<FJsonValueBoolean>(Var.GetBool());
		}
		if (Var.IsVariableInt())
		{
			return MakeShared<FJsonValueNumber>(static_cast<double>(Var.GetInt()));
		}
		if (Var.IsVariableFloat())
		{
			return MakeShared<FJsonValueNumber>(static_cast<double>(Var.GetFloat()));
		}
		// String + unknown fall through to GetString.
		return MakeShared<FJsonValueString>(Var.GetString());
	}

	/**
	 * Truncate a string to ``MaxChars`` with a " ..." suffix on overflow. Used for value_summary
	 * and help_first_line in cfg.list_cvars to keep per-entry payload bounded.
	 */
	FString CFG_TruncateForListSummary(const FString& In, int32 MaxChars)
	{
		if (In.Len() <= MaxChars)
		{
			return In;
		}
		// MaxChars includes the suffix so the wire payload stays under MaxChars.
		const FString Suffix = TEXT(" ...");
		const int32 KeepChars = FMath::Max(0, MaxChars - Suffix.Len());
		return In.Left(KeepChars) + Suffix;
	}

	/**
	 * Extract the first line of the cvar's help string (split on '\n'), trim trailing
	 * whitespace, and truncate to ``kCFGHelpFirstLineMaxChars``.
	 */
	FString CFG_GetHelpFirstLine(const IConsoleObject& Obj)
	{
		FString Help = Obj.GetHelp();
		int32 NewlineIdx = INDEX_NONE;
		if (Help.FindChar(TEXT('\n'), NewlineIdx))
		{
			Help = Help.Left(NewlineIdx);
		}
		Help.TrimStartAndEndInline();
		return CFG_TruncateForListSummary(Help, kCFGHelpFirstLineMaxChars);
	}

	/**
	 * Whitelisted ini base names (D8 sandbox). These are the only ini files cfg.read/write/list_sections
	 * accept. ``UnknownIniHint`` is returned in error messages so callers can correct typos without
	 * a follow-up call.
	 */
	const TCHAR* kCFGWhitelistedIniBases[] = {
		TEXT("DefaultEngine"),
		TEXT("DefaultGame"),
		TEXT("DefaultInput"),
		TEXT("DefaultEditor")
	};

	/**
	 * Resolve an ini base name (e.g. ``"DefaultEngine"``) to an absolute disk path
	 * (``<ProjectDir>/Config/DefaultEngine.ini``). Returns true on whitelist hit + populates
	 * OutAbsPath. False on miss + populates OutError.
	 *
	 * Caller passes the BASE NAME (no extension, no path) — anything containing '.', '/', '\',
	 * etc. is rejected as out-of-sandbox to prevent ``"../DefaultEngine"`` style escapes.
	 */
	bool CFG_ResolveIniPath(const FString& IniBaseName, FString& OutAbsPath, FString& OutError)
	{
		OutAbsPath.Reset();
		OutError.Reset();

		if (IniBaseName.IsEmpty())
		{
			OutError = TEXT("ini_file is empty");
			return false;
		}

		// Reject any disk-path or extension token outright. The whitelist is base-name-only by
		// design (D8) — caller must supply ``"DefaultEngine"``, NOT ``"DefaultEngine.ini"`` or
		// ``"Config/DefaultEngine"``.
		if (IniBaseName.Contains(TEXT("/"))  || IniBaseName.Contains(TEXT("\\")) ||
			IniBaseName.Contains(TEXT(".."))|| IniBaseName.Contains(TEXT(".")))
		{
			OutError = FString::Printf(TEXT("ini_file '%s' must be a BASE NAME with no path or extension "
				"(accepted: DefaultEngine / DefaultGame / DefaultInput / DefaultEditor)"), *IniBaseName);
			return false;
		}

		bool bWhitelisted = false;
		for (const TCHAR* Allowed : kCFGWhitelistedIniBases)
		{
			if (IniBaseName.Equals(Allowed, ESearchCase::CaseSensitive))
			{
				bWhitelisted = true;
				break;
			}
		}
		if (!bWhitelisted)
		{
			OutError = FString::Printf(
				TEXT("ini_file '%s' is not in the whitelist {DefaultEngine, DefaultGame, "
				     "DefaultInput, DefaultEditor} — cfg.* tools restrict mutations to project-"
				     "level Default*.ini files for safety (D8)"), *IniBaseName);
			return false;
		}

		// Mirror Epic's own ConfigContext.cpp path construction (line 344 in UE 5.7):
		//   DestIniFilename = "<ProjectConfigDir>/<BaseIniName>.ini"
		// where ProjectConfigDir == FPaths::SourceConfigDir() (relative form ending in "Config/").
		// Using the exact same canonical form ensures GConfig's filename-keyed cache lookup hits
		// the entry the engine populated at startup. Forcing absolute (ConvertRelativePathToFull)
		// would create a separate cache entry → GetSectionNames returns false (file "not known"),
		// reads of engine-defined keys silently fail.
		//
		// Run NormalizeConfigIniPath to collapse Epic's printf-induced double-slash (their format
		// string is "%s/%s.ini" against a "Config/"-ending ProjectConfigDir → "Config//"). Without
		// normalisation here, GConfig's Find() retries via NormalizeConfigIniPath internally but
		// emits a warning log per access. Pre-normalising avoids the spam.
		FString RawPath = FPaths::SourceConfigDir() / IniBaseName + TEXT(".ini");
		OutAbsPath = FConfigCacheIni::NormalizeConfigIniPath(RawPath);
		return true;
	}

	/**
	 * Heuristic type hint for raw ini values returned by ``cfg.read``. Pure ascii integers / floats
	 * get marked accordingly; boolean keywords (True/False/Yes/No, case-insensitive) → "bool";
	 * everything else falls back to "string". This is advisory only — caller can always re-parse
	 * the raw_string.
	 */
	const TCHAR* CFG_GuessIniTypeHint(const FString& RawValue)
	{
		const FString Trimmed = RawValue.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{
			return TEXT("string");
		}
		if (Trimmed.Equals(TEXT("True"), ESearchCase::IgnoreCase)  ||
			Trimmed.Equals(TEXT("False"), ESearchCase::IgnoreCase) ||
			Trimmed.Equals(TEXT("Yes"), ESearchCase::IgnoreCase)   ||
			Trimmed.Equals(TEXT("No"), ESearchCase::IgnoreCase))
		{
			return TEXT("bool");
		}
		// Integer probe: all digits + optional leading minus.
		bool bIsAllDigits = true;
		bool bHasDot = false;
		for (int32 i = 0; i < Trimmed.Len(); ++i)
		{
			const TCHAR c = Trimmed[i];
			if (i == 0 && c == TEXT('-'))
			{
				continue;
			}
			if (c == TEXT('.'))
			{
				if (bHasDot)
				{
					bIsAllDigits = false;
					break;
				}
				bHasDot = true;
				continue;
			}
			if (c < TEXT('0') || c > TEXT('9'))
			{
				bIsAllDigits = false;
				break;
			}
		}
		if (bIsAllDigits)
		{
			return bHasDot ? TEXT("float") : TEXT("int");
		}
		return TEXT("string");
	}
} // namespace

namespace FConfigTools
{

// ─── cfg.get_cvar ─────────────────────────────────────────────────────────────────────────────
//
// Args:    { name: string }    (required; case-insensitive lookup per IConsoleManager)
// Result:  { name, type ("bool"/"int"/"float"/"string"/"unknown"), value, value_string, help,
//            default_value, set_by (Constructor/Scalability/.../Console), flags_raw (int),
//            is_read_only (bool), is_cheat (bool), is_unregistered (bool) }
//
// Errors:
//   -32602 InvalidParams    missing/empty 'name'
//   -32004 ObjectNotFound   no cvar/console object with this name (case-insensitive)
//   -32011 WrongClass       resolved to an IConsoleCommand instead of an IConsoleVariable (D5)
//
// **D5 — commands ≠ variables.** ``FindConsoleObject`` matches BOTH IConsoleVariable and
// IConsoleCommand. ``AsVariable`` returns nullptr on the command path, which we surface as
// -32011 with a recovery hint pointing at ``pie.console_exec`` for execution semantics.
//
// **set_by tier.** Each cvar tracks the highest SetBy priority at which a value was applied (per
// the ``ECVF_SetByMask`` history mechanism). ``GetConsoleVariableSetByName`` returns one of
// {Constructor, Scalability, GameSetting, ProjectSetting, SystemSettingsIni, ...} — see
// ``ENUMERATE_SET_BY`` in IConsoleManager.h for the full enumeration.
FMCPResponse Tool_GetCVar(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kCFGErrorInvalidParams, TEXT("missing args object"));
	}
	FString Name;
	if (!Request.Args->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kCFGErrorInvalidParams,
			TEXT("missing required string field 'name'"));
	}

	IConsoleManager& Mgr = IConsoleManager::Get();
	IConsoleObject* Obj = Mgr.FindConsoleObject(*Name);
	if (!Obj)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("console object '%s' not found (case-insensitive lookup; "
				"see cfg.list_cvars for available names)"), *Name));
	}

	IConsoleVariable* Var = Obj->AsVariable();
	if (!Var)
	{
		// IConsoleCommand or some other non-variable IConsoleObject.
		return FMCPToolHelpers::MakeError(Request, kMCPErrorWrongClass,
			FString::Printf(TEXT("console object '%s' is a COMMAND, not a variable — use "
				"pie.console_exec to execute commands; cfg.get_cvar/set_cvar are variable-only "
				"per Phase 6 D5"), *Name));
	}

	const EConsoleVariableFlags Flags = Var->GetFlags();
	return FMCPJsonBuilder()
		.Str(TEXT("name"),           Name)
		.Str(TEXT("type"),           CFG_GetCVarTypeString(*Var))
		.Field(TEXT("value"),        CFG_GetCVarValueJson(*Var))
		.Str(TEXT("value_string"),   Var->GetString())
		.Str(TEXT("help"),           Obj->GetHelp())
		.Str(TEXT("default_value"),  Var->GetDefaultValue())
		.Str(TEXT("set_by"),         GetConsoleVariableSetByName(Flags))
		.Num(TEXT("flags_raw"),      static_cast<double>(static_cast<uint32>(Flags)))
		.Bool(TEXT("is_read_only"),  Var->TestFlags(ECVF_ReadOnly))
		.Bool(TEXT("is_cheat"),      Var->TestFlags(ECVF_Cheat))
		.Bool(TEXT("is_unregistered"), Var->TestFlags(ECVF_Unregistered))
		.BuildSuccess(Request);
}

// ─── cfg.set_cvar ─────────────────────────────────────────────────────────────────────────────
//
// Args:    { name: string, value: bool|number|string }    (required)
// Result:  { set: bool, name, type, prior_value, prior_value_string, new_value, new_value_string,
//            set_by (post-write tier) }
//
// Errors:
//   -32602 InvalidParams           missing 'name' or 'value' field
//   -32004 ObjectNotFound          no cvar with this name
//   -32011 WrongClass              resolved to IConsoleCommand instead of IConsoleVariable (D5)
//   -32047 CVarReadOnly            cvar has ECVF_ReadOnly flag — runtime mutation disallowed (D9)
//   -32006 PropertyTypeMismatch    JSON value type can't coerce to the cvar's native type
//
// **D4 — JSON-typed marshalling.** The wire ``value`` MUST be a primitive JSON type. We coerce by
// the cvar's reported type:
//
//   String cvars   ← any JSON primitive stringified (bool → "1"/"0", number → SanitizeFloat)
//   Bool cvars     ← JSON bool / int / "0"|"1" / "true"|"false" / "yes"|"no"  (case-insensitive)
//   Int cvars      ← JSON int / float (truncated to int) / numeric string
//   Float cvars    ← JSON int (cast to float) / float / numeric string
//   Unknown cvars  ← JSON string only (defensive — typed coercion not safe without Is* probe)
//
// **Internal mechanism.** ``IConsoleVariable::Set(const TCHAR*, ECVF_SetByCode)`` is the canonical
// write path. The string overload internally calls TTypeFromString<T>::FromString which handles
// "0"/"1" → bool, "1.5" → float, etc. We always go via the string overload (uniform error path)
// rather than the typed bool/int/float overloads to keep type marshalling decisions in ONE place.
//
// **set_by tier.** We write with ECVF_SetByCode (priority 0x0E000000) — the second-highest
// priority. This means subsequent SetByConsole writes (operator typing in the editor console) can
// still override our value, but earlier-priority writes (ini, scalability) cannot. ECVF_SetByCode
// matches Epic's own pattern for programmatic CVar mutations.
FMCPResponse Tool_SetCVar(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kCFGErrorInvalidParams, TEXT("missing args object"));
	}
	FString Name;
	if (!Request.Args->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kCFGErrorInvalidParams,
			TEXT("missing required string field 'name'"));
	}
	TSharedPtr<FJsonValue> InValue = Request.Args->TryGetField(TEXT("value"));
	if (!InValue.IsValid() || InValue->IsNull())
	{
		return FMCPToolHelpers::MakeError(Request, kCFGErrorInvalidParams,
			TEXT("missing required field 'value' (must be bool, number, or string)"));
	}

	IConsoleManager& Mgr = IConsoleManager::Get();
	IConsoleObject* Obj = Mgr.FindConsoleObject(*Name);
	if (!Obj)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("console object '%s' not found (see cfg.list_cvars)"), *Name));
	}

	IConsoleVariable* Var = Obj->AsVariable();
	if (!Var)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorWrongClass,
			FString::Printf(TEXT("console object '%s' is a COMMAND, not a variable — use "
				"pie.console_exec to execute commands (D5)"), *Name));
	}

	if (Var->TestFlags(ECVF_ReadOnly))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorCVarReadOnly,
			FString::Printf(TEXT("cvar '%s' is read-only (ECVF_ReadOnly) — write the value into "
				"<ProjectDir>/Config/DefaultEngine.ini's [ConsoleVariables] section via cfg.write "
				"and restart the editor"), *Name));
	}

	const FString TypeStr = CFG_GetCVarTypeString(*Var);

	// Capture the prior value BEFORE the write so we can report it back atomically.
	const FString PriorValueString = Var->GetString();
	TSharedRef<FJsonValue> PriorValueJson = CFG_GetCVarValueJson(*Var);

	// Coerce the incoming JSON value into a string acceptable to IConsoleVariable::Set.
	FString WriteValue;
	const EJson InType = InValue->Type;

	auto CoerceFailureMismatch = [&](const FString& Detail)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPropertyTypeMismatch,
			FString::Printf(TEXT("cvar '%s' is type '%s'; %s"), *Name, *TypeStr, *Detail));
	};

	if (Var->IsVariableString() || TypeStr == TEXT("unknown"))
	{
		// Strings + unknown: accept any JSON primitive, stringify it. For unknowns we go through
		// the string path so the cvar's own parser handles its custom format.
		switch (InType)
		{
			case EJson::Boolean:
				WriteValue = InValue->AsBool() ? TEXT("1") : TEXT("0");
				break;
			case EJson::Number:
				// Use %g for compact floats and integral preservation.
				WriteValue = FString::SanitizeFloat(InValue->AsNumber());
				break;
			case EJson::String:
				WriteValue = InValue->AsString();
				break;
			default:
				return CoerceFailureMismatch(TEXT("expected JSON bool/number/string"));
		}
	}
	else if (Var->IsVariableBool())
	{
		switch (InType)
		{
			case EJson::Boolean:
				WriteValue = InValue->AsBool() ? TEXT("1") : TEXT("0");
				break;
			case EJson::Number:
				// Non-zero → true; zero → false. Mirrors C++ bool coercion.
				WriteValue = (InValue->AsNumber() != 0.0) ? TEXT("1") : TEXT("0");
				break;
			case EJson::String:
			{
				FString S = InValue->AsString();
				S.TrimStartAndEndInline();
				if (S.Equals(TEXT("1"))      ||
					S.Equals(TEXT("true"),  ESearchCase::IgnoreCase) ||
					S.Equals(TEXT("yes"),   ESearchCase::IgnoreCase) ||
					S.Equals(TEXT("on"),    ESearchCase::IgnoreCase))
				{
					WriteValue = TEXT("1");
				}
				else if (S.Equals(TEXT("0"))      ||
					S.Equals(TEXT("false"), ESearchCase::IgnoreCase) ||
					S.Equals(TEXT("no"),    ESearchCase::IgnoreCase) ||
					S.Equals(TEXT("off"),   ESearchCase::IgnoreCase))
				{
					WriteValue = TEXT("0");
				}
				else
				{
					return CoerceFailureMismatch(FString::Printf(
						TEXT("string value '%s' is not a recognised bool keyword "
						     "(use 0/1, true/false, yes/no, on/off)"), *S));
				}
				break;
			}
			default:
				return CoerceFailureMismatch(TEXT("expected JSON bool/number/string"));
		}
	}
	else if (Var->IsVariableInt())
	{
		switch (InType)
		{
			case EJson::Boolean:
				WriteValue = InValue->AsBool() ? TEXT("1") : TEXT("0");
				break;
			case EJson::Number:
				// Truncate to int (mirrors implicit int casts).
				WriteValue = FString::FromInt(static_cast<int32>(InValue->AsNumber()));
				break;
			case EJson::String:
			{
				const FString S = InValue->AsString().TrimStartAndEnd();
				if (S.IsEmpty() || !S.IsNumeric())
				{
					return CoerceFailureMismatch(FString::Printf(
						TEXT("string value '%s' is not numeric"), *S));
				}
				WriteValue = FString::FromInt(FCString::Atoi(*S));
				break;
			}
			default:
				return CoerceFailureMismatch(TEXT("expected JSON bool/number/string"));
		}
	}
	else if (Var->IsVariableFloat())
	{
		switch (InType)
		{
			case EJson::Boolean:
				WriteValue = InValue->AsBool() ? TEXT("1.0") : TEXT("0.0");
				break;
			case EJson::Number:
				WriteValue = FString::SanitizeFloat(InValue->AsNumber());
				break;
			case EJson::String:
			{
				const FString S = InValue->AsString().TrimStartAndEnd();
				if (S.IsEmpty() || !S.IsNumeric())
				{
					return CoerceFailureMismatch(FString::Printf(
						TEXT("string value '%s' is not numeric"), *S));
				}
				WriteValue = FString::SanitizeFloat(FCString::Atof(*S));
				break;
			}
			default:
				return CoerceFailureMismatch(TEXT("expected JSON bool/number/string"));
		}
	}
	else
	{
		// Shouldn't reach here — exhausted via IsVariable*() above. Defensive fallback.
		return FMCPToolHelpers::MakeError(Request, kCFGErrorInternal,
			FString::Printf(TEXT("cvar '%s' has unrecognised type — neither bool/int/float/string"),
				*Name));
	}

	// Apply the write. ECVF_SetByCode is the second-highest priority — caller's intent overrides
	// ini-loaded and scalability values but a subsequent Console-priority write (operator) wins.
	Var->Set(*WriteValue, ECVF_SetByCode);

	return FMCPJsonBuilder()
		.Bool(TEXT("set"),                  true)
		.Str(TEXT("name"),                  Name)
		.Str(TEXT("type"),                  TypeStr)
		.Field(TEXT("prior_value"),         PriorValueJson)
		.Str(TEXT("prior_value_string"),    PriorValueString)
		.Field(TEXT("new_value"),           CFG_GetCVarValueJson(*Var))
		.Str(TEXT("new_value_string"),      Var->GetString())
		.Str(TEXT("set_by"),                GetConsoleVariableSetByName(Var->GetFlags()))
		.BuildSuccess(Request);
}

// ─── cfg.list_cvars ───────────────────────────────────────────────────────────────────────────
//
// Args:    { prefix_filter?: string, page_token?: string, page_size?: int (default 100, max 1000) }
// Result:  { cvars: [{name, type, value_summary, help_first_line, set_by, is_read_only, is_cheat,
//            is_command}], next_page_token?: string | null, total_known: int, prefix_filter_echo }
//
// Errors:
//   -32015 StaleCursor    page_token's filter_hash doesn't match the current prefix_filter
//
// **Includes commands.** ``cfg.list_cvars`` is named for the dominant use case but actually
// enumerates ALL IConsoleObject entries — variables AND commands. The ``is_command`` boolean
// disambiguates per-entry. Commands have no value/type beyond their help text, so we emit
// ``value_summary=""`` and ``type="command"`` for them.
//
// **Sort key.** CVar name, case-insensitive. ForEachConsoleObjectThatStartsWith does NOT guarantee
// sorted iteration — we collect then sort to match the cursor's invariant.
//
// **Filter.** ``prefix_filter`` is matched against the cvar name as a CASE-INSENSITIVE prefix.
// Empty/omitted → enumerate everything. The framework's built-in prefix arg is honoured at the
// ForEach level for efficiency, then we re-validate the prefix client-side for casefolding.
FMCPResponse Tool_ListCVars(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString Prefix;
	if (Request.Args.IsValid())
	{
		Request.Args->TryGetStringField(TEXT("prefix_filter"), Prefix);
	}
	const uint64 FilterHash = CFG_HashFilter(Prefix);

	FString TokenWire;
	if (Request.Args.IsValid())
	{
		Request.Args->TryGetStringField(TEXT("page_token"), TokenWire);
	}
	FMCPPageCursor Cursor;
	FMCPResponse CursorErr;
	if (!CFG_DecodeCursor(Request, TokenWire, FilterHash, Cursor, CursorErr))
	{
		return CursorErr;
	}

	const int32 PageSize = CFG_ClampPageSize(Request.Args, TEXT("page_size"), kCFGDefaultPageSize);

	// Collect all matches in (name, IConsoleObject*) tuples. Visitor must not retain Obj* past the
	// call (per Epic's pattern) — we materialise required fields immediately during iteration to
	// keep this loop self-contained.
	struct FEntry
	{
		FString Name;
		FString Type;
		FString ValueSummary;
		FString HelpFirstLine;
		FString SetByName;
		bool    bIsReadOnly = false;
		bool    bIsCheat = false;
		bool    bIsCommand = false;
		EConsoleVariableFlags FlagsRaw = ECVF_Default;
	};
	TArray<FEntry> Entries;

	IConsoleManager& Mgr = IConsoleManager::Get();
	const FConsoleObjectVisitor Visitor = FConsoleObjectVisitor::CreateLambda(
		[&Entries, &Prefix](const TCHAR* Name, IConsoleObject* Obj)
	{
		if (!Obj || !Name)
		{
			return;
		}
		// ForEachConsoleObjectThatStartsWith honours its prefix arg case-INSENSITIVELY at the
		// framework level (per FConsoleManager::ForEachConsoleObjectThatStartsWith impl which
		// uses FNameStringMatch). The explicit re-check below is defensive — guards against
		// future changes to Epic's filter semantics and ensures our public contract holds even
		// if the framework path becomes case-sensitive.
		if (!Prefix.IsEmpty())
		{
			const FString NameS(Name);
			if (NameS.Len() < Prefix.Len() ||
				!NameS.Left(Prefix.Len()).Equals(Prefix, ESearchCase::IgnoreCase))
			{
				return;
			}
		}

		FEntry E;
		E.Name = Name;
		E.HelpFirstLine = CFG_GetHelpFirstLine(*Obj);
		E.FlagsRaw = Obj->GetFlags();
		E.bIsCheat = Obj->TestFlags(ECVF_Cheat);
		// SetByName lives in the SetByMask bits.
		E.SetByName = GetConsoleVariableSetByName(E.FlagsRaw);

		if (IConsoleVariable* Var = Obj->AsVariable())
		{
			E.Type = CFG_GetCVarTypeString(*Var);
			E.ValueSummary = CFG_TruncateForListSummary(Var->GetString(), kCFGValueSummaryMaxChars);
			E.bIsReadOnly = Var->TestFlags(ECVF_ReadOnly);
			E.bIsCommand = false;
		}
		else
		{
			E.Type = TEXT("command");
			E.ValueSummary = TEXT("");
			E.bIsReadOnly = false;
			E.bIsCommand = true;
		}
		Entries.Add(MoveTemp(E));
	});

	// Pass the raw prefix to the framework for early-rejection; the case-insensitive widening is
	// applied in-visitor above.
	Mgr.ForEachConsoleObjectThatStartsWith(Visitor, *Prefix);

	// Stable sort by case-insensitive name to match cursor invariant.
	Entries.StableSort([](const FEntry& A, const FEntry& B)
	{
		return A.Name.Compare(B.Name, ESearchCase::IgnoreCase) < 0;
	});

	// Cursor sentinel: skip past LastAssetPath (re-used field for "last cvar name").
	int32 StartIdx = 0;
	if (!Cursor.LastAssetPath.IsEmpty())
	{
		while (StartIdx < Entries.Num())
		{
			if (Entries[StartIdx].Name.Compare(Cursor.LastAssetPath, ESearchCase::IgnoreCase) > 0)
			{
				break;
			}
			++StartIdx;
		}
	}
	const int32 EndIdxExcl = FMath::Min(Entries.Num(), StartIdx + PageSize);

	TArray<TSharedPtr<FJsonValue>> ItemsArr;
	ItemsArr.Reserve(EndIdxExcl - StartIdx);
	FString LastEmittedName;
	for (int32 i = StartIdx; i < EndIdxExcl; ++i)
	{
		const FEntry& E = Entries[i];
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"),              E.Name);
		Obj->SetStringField(TEXT("type"),              E.Type);
		Obj->SetStringField(TEXT("value_summary"),     E.ValueSummary);
		Obj->SetStringField(TEXT("help_first_line"),   E.HelpFirstLine);
		Obj->SetStringField(TEXT("set_by"),            E.SetByName);
		Obj->SetBoolField(TEXT("is_read_only"),        E.bIsReadOnly);
		Obj->SetBoolField(TEXT("is_cheat"),            E.bIsCheat);
		Obj->SetBoolField(TEXT("is_command"),          E.bIsCommand);
		Obj->SetNumberField(TEXT("flags_raw"),         static_cast<double>(static_cast<uint32>(E.FlagsRaw)));
		ItemsArr.Add(MakeShared<FJsonValueObject>(Obj));
		LastEmittedName = E.Name;
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetArrayField(TEXT("cvars"),              ItemsArr);
	Out->SetNumberField(TEXT("total_known"),       static_cast<double>(Entries.Num()));
	Out->SetStringField(TEXT("prefix_filter_echo"), Prefix);

	if (EndIdxExcl < Entries.Num() && !LastEmittedName.IsEmpty())
	{
		FMCPPageCursor NextCursor;
		NextCursor.FilterHash = FilterHash;
		NextCursor.LastAssetPath = LastEmittedName;
		NextCursor.TotalKnownSnapshot = Entries.Num();
		Out->SetStringField(TEXT("next_page_token"), FMCPPageCursorUtils::Encode(NextCursor));
	}
	else
	{
		Out->SetField(TEXT("next_page_token"), MakeShared<FJsonValueNull>());
	}
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── cfg.read ─────────────────────────────────────────────────────────────────────────────────
//
// Args:    { ini_file: string, section: string, key: string }    (all required)
//          ini_file ∈ {"DefaultEngine", "DefaultGame", "DefaultInput", "DefaultEditor"} (D8)
//          section is the literal section header WITHOUT brackets (e.g. "/Script/Engine.Engine")
// Result:  { value: string, raw_string: string, type_hint: string, ini_path: string,
//            ini_file_echo: string, section_echo: string, key_echo: string, found: bool }
//
// Errors:
//   -32602 InvalidParams    missing/empty ini_file or section or key
//   -32013 PathEscape       ini_file not in whitelist (D8) or contains path/extension separators
//   -32004 ObjectNotFound   key not found in the resolved section (or section absent)
//
// **value vs raw_string.** Both are the literal cached string — duplicated for forwards
// compatibility (future versions may unquote / unescape into value while leaving raw_string
// verbatim). For Phase 6 they're identical.
//
// **type_hint.** Best-effort guess from the raw string: "bool" / "int" / "float" / "string".
// Caller can ignore and re-parse client-side.
FMCPResponse Tool_Read(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kCFGErrorInvalidParams, TEXT("missing args object"));
	}
	FString IniBaseName, Section, Key;
	if (!Request.Args->TryGetStringField(TEXT("ini_file"), IniBaseName) || IniBaseName.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kCFGErrorInvalidParams,
			TEXT("missing required string field 'ini_file'"));
	}
	if (!Request.Args->TryGetStringField(TEXT("section"), Section) || Section.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kCFGErrorInvalidParams,
			TEXT("missing required string field 'section' (literal header without brackets, "
				 "e.g. '/Script/Engine.Engine')"));
	}
	if (!Request.Args->TryGetStringField(TEXT("key"), Key) || Key.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kCFGErrorInvalidParams,
			TEXT("missing required string field 'key'"));
	}

	FString IniPath, ResolveErr;
	if (!CFG_ResolveIniPath(IniBaseName, IniPath, ResolveErr))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPathEscape, ResolveErr);
	}

	check(GConfig);
	FString RawValue;
	const bool bFound = GConfig->GetString(*Section, *Key, RawValue, IniPath);

	if (!bFound)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("key '%s' not found in section [%s] of '%s.ini' (or section absent)"),
				*Key, *Section, *IniBaseName));
	}

	return FMCPJsonBuilder()
		.Bool(TEXT("found"),         true)
		.Str(TEXT("value"),          RawValue)
		.Str(TEXT("raw_string"),     RawValue)
		.Str(TEXT("type_hint"),      CFG_GuessIniTypeHint(RawValue))
		.Str(TEXT("ini_path"),       IniPath)
		.Str(TEXT("ini_file_echo"),  IniBaseName)
		.Str(TEXT("section_echo"),   Section)
		.Str(TEXT("key_echo"),       Key)
		.BuildSuccess(Request);
}

// ─── cfg.write ────────────────────────────────────────────────────────────────────────────────
//
// Args:    { ini_file: string, section: string, key: string, value: bool|number|string,
//            is_string?: bool (default true) }
//          ini_file ∈ {"DefaultEngine", "DefaultGame", "DefaultInput", "DefaultEditor"} (D8)
// Result:  { written: bool, prior_value: string ("" if absent), prior_existed: bool,
//            new_value_string: string, ini_path: string, flushed: bool }
//
// Errors:
//   -32602 InvalidParams    missing required field
//   -32013 PathEscape       ini_file not in whitelist (D8)
//   -32603 Internal         GConfig flush failed (file unwritable etc.)
//
// **All writes are stringified.** ``GConfig->SetString`` is the canonical primitive — the cache
// stores values as FString and downstream readers re-parse via GetBool/GetInt/etc. The ``is_string``
// flag is reserved for future quoting/escaping behaviour (e.g. wrapping in quotes for values with
// spaces) — currently both true and false paths go through SetString with the same coercion. Plan
// keeps the field for forward-compat per the plan §Category-C description.
//
// **Flush behaviour.** ``GConfig->Flush(false, IniPath)`` writes the in-memory cache to disk
// WITHOUT removing the entry from the in-process cache. Subsequent reads (this run) hit the
// cache; next-launch reads load the new value from disk. We re-poll GetString after flush so
// ``new_value_string`` reflects the actual cached state (in case of normalisation by SetString).
FMCPResponse Tool_Write(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kCFGErrorInvalidParams, TEXT("missing args object"));
	}
	FString IniBaseName, Section, Key;
	if (!Request.Args->TryGetStringField(TEXT("ini_file"), IniBaseName) || IniBaseName.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kCFGErrorInvalidParams,
			TEXT("missing required string field 'ini_file'"));
	}
	if (!Request.Args->TryGetStringField(TEXT("section"), Section) || Section.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kCFGErrorInvalidParams,
			TEXT("missing required string field 'section'"));
	}
	if (!Request.Args->TryGetStringField(TEXT("key"), Key) || Key.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kCFGErrorInvalidParams,
			TEXT("missing required string field 'key'"));
	}
	TSharedPtr<FJsonValue> InValue = Request.Args->TryGetField(TEXT("value"));
	if (!InValue.IsValid() || InValue->IsNull())
	{
		return FMCPToolHelpers::MakeError(Request, kCFGErrorInvalidParams,
			TEXT("missing required field 'value' (must be bool, number, or string)"));
	}

	FString IniPath, ResolveErr;
	if (!CFG_ResolveIniPath(IniBaseName, IniPath, ResolveErr))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPathEscape, ResolveErr);
	}

	// Stringify the value. Same coercion table as cfg.set_cvar's string-cvar path (mirrors).
	FString WriteValue;
	switch (InValue->Type)
	{
		case EJson::Boolean:
			WriteValue = InValue->AsBool() ? TEXT("True") : TEXT("False");
			break;
		case EJson::Number:
			WriteValue = FString::SanitizeFloat(InValue->AsNumber());
			break;
		case EJson::String:
			WriteValue = InValue->AsString();
			break;
		default:
			return FMCPToolHelpers::MakeError(Request, kMCPErrorPropertyTypeMismatch,
				TEXT("'value' must be JSON bool / number / string (got object/array/null)"));
	}

	check(GConfig);

	// Capture prior value (best-effort; key may not exist).
	FString PriorValue;
	const bool bPriorExisted = GConfig->GetString(*Section, *Key, PriorValue, IniPath);

	// Apply + flush. SetString mutates the in-memory cache; Flush(false, IniPath) writes the
	// cache to disk WITHOUT removing the entry. bRemoveFromCache=false keeps the in-process
	// cache hot for subsequent reads this session.
	GConfig->SetString(*Section, *Key, *WriteValue, IniPath);
	GConfig->Flush(false, IniPath);

	// Verify the post-write read sees the new value (sanity check; SetString may normalise).
	FString PostValue;
	const bool bPostFound = GConfig->GetString(*Section, *Key, PostValue, IniPath);
	if (!bPostFound)
	{
		return FMCPToolHelpers::MakeError(Request, kCFGErrorInternal,
			FString::Printf(TEXT("post-write read failed: SetString succeeded but GetString could "
				"not retrieve '%s' from [%s] of '%s.ini' — cache corruption?"),
				*Key, *Section, *IniBaseName));
	}

	// Flush sanity: confirm the file exists on disk after Flush. GConfig may suppress writes if
	// the file is read-only on disk; we report the result so caller can detect this case.
	const bool bFileOnDisk = IFileManager::Get().FileExists(*IniPath);

	return FMCPJsonBuilder()
		.Bool(TEXT("written"),            true)
		.Bool(TEXT("prior_existed"),      bPriorExisted)
		.Str(TEXT("prior_value"),         bPriorExisted ? PriorValue : FString())
		.Str(TEXT("new_value_string"),    PostValue)
		.Str(TEXT("ini_path"),            IniPath)
		.Str(TEXT("ini_file_echo"),       IniBaseName)
		.Str(TEXT("section_echo"),        Section)
		.Str(TEXT("key_echo"),            Key)
		.Bool(TEXT("flushed"),            bFileOnDisk)
		.BuildSuccess(Request);
}

// ─── cfg.list_sections ────────────────────────────────────────────────────────────────────────
//
// Args:    { ini_file: string, page_token?: string, page_size?: int (default 100, max 1000) }
// Result:  { sections: [string], next_page_token?: string | null, total_known: int,
//            ini_path: string, ini_file_echo: string }
//
// Errors:
//   -32602 InvalidParams    missing/empty ini_file
//   -32013 PathEscape       ini_file not in whitelist (D8)
//   -32004 ObjectNotFound   GConfig has no record of this file (returns false from GetSectionNames)
//
// Sections are returned as bare names (no brackets), sorted case-insensitive ascending. Cursor
// keyed on the section name + filter hash of the ini_file (changing ini_file mid-pagination →
// -32015 StaleCursor).
FMCPResponse Tool_ListSections(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kCFGErrorInvalidParams, TEXT("missing args object"));
	}
	FString IniBaseName;
	if (!Request.Args->TryGetStringField(TEXT("ini_file"), IniBaseName) || IniBaseName.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kCFGErrorInvalidParams,
			TEXT("missing required string field 'ini_file'"));
	}

	FString IniPath, ResolveErr;
	if (!CFG_ResolveIniPath(IniBaseName, IniPath, ResolveErr))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPathEscape, ResolveErr);
	}

	const uint64 FilterHash = CFG_HashIniFile(IniBaseName);

	FString TokenWire;
	Request.Args->TryGetStringField(TEXT("page_token"), TokenWire);

	FMCPPageCursor Cursor;
	FMCPResponse CursorErr;
	if (!CFG_DecodeCursor(Request, TokenWire, FilterHash, Cursor, CursorErr))
	{
		return CursorErr;
	}

	const int32 PageSize = CFG_ClampPageSize(Request.Args, TEXT("page_size"), kCFGDefaultPageSize);

	check(GConfig);
	TArray<FString> SectionNames;
	if (!GConfig->GetSectionNames(IniPath, SectionNames))
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("GConfig has no record of '%s.ini' (path: %s) — file may not "
				"exist on disk or was never loaded into the config cache"),
				*IniBaseName, *IniPath));
	}

	SectionNames.StableSort([](const FString& A, const FString& B)
	{
		return A.Compare(B, ESearchCase::IgnoreCase) < 0;
	});

	int32 StartIdx = 0;
	if (!Cursor.LastAssetPath.IsEmpty())
	{
		while (StartIdx < SectionNames.Num())
		{
			if (SectionNames[StartIdx].Compare(Cursor.LastAssetPath, ESearchCase::IgnoreCase) > 0)
			{
				break;
			}
			++StartIdx;
		}
	}
	const int32 EndIdxExcl = FMath::Min(SectionNames.Num(), StartIdx + PageSize);

	TArray<TSharedPtr<FJsonValue>> ItemsArr;
	ItemsArr.Reserve(EndIdxExcl - StartIdx);
	FString LastEmitted;
	for (int32 i = StartIdx; i < EndIdxExcl; ++i)
	{
		ItemsArr.Add(MakeShared<FJsonValueString>(SectionNames[i]));
		LastEmitted = SectionNames[i];
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetArrayField(TEXT("sections"),       ItemsArr);
	Out->SetNumberField(TEXT("total_known"),   static_cast<double>(SectionNames.Num()));
	Out->SetStringField(TEXT("ini_path"),      IniPath);
	Out->SetStringField(TEXT("ini_file_echo"), IniBaseName);

	if (EndIdxExcl < SectionNames.Num() && !LastEmitted.IsEmpty())
	{
		FMCPPageCursor NextCursor;
		NextCursor.FilterHash = FilterHash;
		NextCursor.LastAssetPath = LastEmitted;
		NextCursor.TotalKnownSnapshot = SectionNames.Num();
		Out->SetStringField(TEXT("next_page_token"), FMCPPageCursorUtils::Encode(NextCursor));
	}
	else
	{
		Out->SetField(TEXT("next_page_token"), MakeShared<FJsonValueNull>());
	}
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── Registration ──────────────────────────────────────────────────────────────────────────────
void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	RegisterTool(TEXT("cfg.get_cvar"),      &Tool_GetCVar,       /*Lane A*/ false);
	RegisterTool(TEXT("cfg.set_cvar"),      &Tool_SetCVar,       /*Lane A*/ false);
	RegisterTool(TEXT("cfg.list_cvars"),    &Tool_ListCVars,     /*Lane A*/ false);
	RegisterTool(TEXT("cfg.read"),          &Tool_Read,          /*Lane A*/ false);
	RegisterTool(TEXT("cfg.write"),         &Tool_Write,         /*Lane A*/ false);
	RegisterTool(TEXT("cfg.list_sections"), &Tool_ListSections,  /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("Phase 6 Chunk C (Config/CVars): registered 6 cfg.* sync handlers ")
		TEXT("(get_cvar/set_cvar/list_cvars/read/write/list_sections, all Lane A); ")
		TEXT("ini sandbox = {DefaultEngine, DefaultGame, DefaultInput, DefaultEditor}"));
}

} // namespace FConfigTools

#undef LOCTEXT_NAMESPACE

MCP_REGISTER_SURFACE(ConfigTools, &FConfigTools::Register)
