// Copyright FatumGame. All Rights Reserved.

#include "PIETools.h"

#include "FMCPDispatchQueue.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPActorPathUtils.h"
#include "Utils/MCPWorldContext.h"

#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Logging/LogVerbosity.h"
#include "Misc/Optional.h"
#include "Misc/OutputDevice.h"
#include "Misc/StringOutputDevice.h"
#include "PlayInEditorDataTypes.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "UObject/Class.h"
#include "UObject/Package.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// PIE_ prefix per the unity-build symbol-collision pattern (MakeError/MakeSuccess clash with
	// UE's global ValueOrError templates).
	constexpr int32 kPIEErrorInvalidParams = -32602;
	constexpr int32 kPIEErrorInternal      = -32603;

	void PIE_StampIds(const FMCPRequest& Request, FMCPResponse& Response)
	{
		Response.RequestId = Request.RequestId;
		Response.OriginalIdString = Request.OriginalIdString;
	}

	FMCPResponse PIE_MakeError(const FMCPRequest& Request, int32 Code, const FString& Message)
	{
		FMCPResponse R;
		PIE_StampIds(Request, R);
		R.bIsError = true;
		R.ErrorCode = Code;
		R.ErrorMessage = Message;
		return R;
	}

	FMCPResponse PIE_MakeSuccessObj(const FMCPRequest& Request, TSharedPtr<FJsonObject> Result)
	{
		FMCPResponse R;
		PIE_StampIds(Request, R);
		R.bIsError = false;
		R.Result = MakeShared<FJsonValueObject>(MoveTemp(Result));
		return R;
	}

	/** Frozen PIE-required refusal (-32038). Smoke asserts "PIE is not running" + "pie.start" + "editor.*". */
	FMCPResponse PIE_MakeNotActiveError(const FMCPRequest& Request)
	{
		return PIE_MakeError(Request, kMCPErrorPIENotActive, kMCPMessagePIENotActive);
	}

	// ─── PIE world helpers ──────────────────────────────────────────────────────────────────────

	/**
	 * Find the first PIE world that is NOT a dedicated server. Used by pie.console_exec when
	 * caller specifies world='pie' (the default — they want the client PIE world).
	 *
	 * Search order:
	 *   1. ``GEditor->PlayWorld`` (the canonical "active" PIE world the editor displays).
	 *   2. Fallback: walk ``GEngine->GetWorldContexts()`` for ``WorldType == PIE && !RunAsDedicated``.
	 *
	 * Returns null when no non-server PIE world exists.
	 */
	UWorld* PIE_FindFirstClientWorld()
	{
		check(IsInGameThread());
		if (!GEditor)
		{
			return nullptr;
		}
		// PlayWorld may be the server world in NM_ListenServer; we want the first non-server world.
		if (GEditor->PlayWorld
			&& GEditor->PlayWorld->WorldType == EWorldType::PIE)
		{
			// PlayWorld is OK if it isn't the dedicated-server world. Look up its context to check.
			for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
			{
				if (Ctx.World() == GEditor->PlayWorld && !Ctx.RunAsDedicated)
				{
					return GEditor->PlayWorld;
				}
			}
		}
		// Walk every PIE world; first non-server wins.
		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			if (Ctx.WorldType == EWorldType::PIE && !Ctx.RunAsDedicated)
			{
				if (UWorld* W = Ctx.World())
				{
					return W;
				}
			}
		}
		return nullptr;
	}

	/**
	 * Find the dedicated-server PIE world, if any. Returns null when PIE was started without a
	 * server (PIE_Standalone or single-client PIE_ListenServer).
	 */
	UWorld* PIE_FindServerWorld()
	{
		check(IsInGameThread());
		if (!GEngine)
		{
			return nullptr;
		}
		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			if (Ctx.WorldType == EWorldType::PIE && Ctx.RunAsDedicated)
			{
				if (UWorld* W = Ctx.World())
				{
					return W;
				}
			}
		}
		return nullptr;
	}

	/** Walk every PIE world (client + server). */
	void PIE_GatherAllWorlds(TArray<UWorld*>& OutWorlds)
	{
		check(IsInGameThread());
		OutWorlds.Reset();
		if (!GEngine)
		{
			return;
		}
		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			if (Ctx.WorldType == EWorldType::PIE)
			{
				if (UWorld* W = Ctx.World())
				{
					OutWorlds.Add(W);
				}
			}
		}
	}

	/** Count loaded PIE worlds for pie.is_running. */
	int32 PIE_CountWorlds()
	{
		check(IsInGameThread());
		int32 Count = 0;
		if (!GEngine)
		{
			return 0;
		}
		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			if (Ctx.WorldType == EWorldType::PIE && Ctx.World())
			{
				++Count;
			}
		}
		return Count;
	}

	/**
	 * Detect "currently paused" across all loaded PIE worlds. We treat the session as paused iff
	 * EVERY non-server PIE world reports ``IsPaused()`` true. Mixed states return false (the user
	 * should call pie.resume which broadcasts to all).
	 */
	bool PIE_IsAnyWorldPaused()
	{
		check(IsInGameThread());
		if (!GEngine)
		{
			return false;
		}
		bool bAnyNonServer = false;
		bool bAllPaused = true;
		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			if (Ctx.WorldType != EWorldType::PIE || Ctx.RunAsDedicated)
			{
				continue;
			}
			if (UWorld* W = Ctx.World())
			{
				bAnyNonServer = true;
				if (!W->IsPaused())
				{
					bAllPaused = false;
				}
			}
		}
		return bAnyNonServer && bAllPaused;
	}

	/**
	 * Build the canonical (pc_actor_guid, pc_path / pawn_path) pair. ``actor_guid`` may be empty
	 * for PIE-spawned actors whose ActorGuid never got set; callers should fall back to the path.
	 */
	void PIE_AddActorIdentityFields(
		const AActor* Actor,
		const TCHAR* GuidFieldName,
		const TCHAR* PathFieldName,
		TSharedPtr<FJsonObject>& Out)
	{
		check(Actor != nullptr);
		const FGuid ActorGuid = Actor->GetActorGuid();
		Out->SetStringField(GuidFieldName, ActorGuid.IsValid()
			? ActorGuid.ToString(EGuidFormats::DigitsWithHyphens)
			: FString());
		Out->SetStringField(PathFieldName, FMCPActorPathUtils::BuildActorPath(Actor));
	}

	// ─── pie.start mode parser ──────────────────────────────────────────────────────────────────

	enum class EPIE_StartMode : uint8
	{
		SelectedViewport,  // default — PlayMode_InViewPort + InProcess
		NewWindow,         // PlayMode_InEditorFloating + InProcess (separate window)
		Standalone         // PlayMode_InNewProcess + NewProcess (separate process)
	};

	bool PIE_ParseStartMode(const FString& Raw, EPIE_StartMode& OutMode, FString& OutError)
	{
		// Accept the three plan-mandated string forms case-insensitively.
		if (Raw.IsEmpty() || Raw.Equals(TEXT("selected_viewport"), ESearchCase::IgnoreCase))
		{
			OutMode = EPIE_StartMode::SelectedViewport;
			return true;
		}
		if (Raw.Equals(TEXT("new_window"), ESearchCase::IgnoreCase))
		{
			OutMode = EPIE_StartMode::NewWindow;
			return true;
		}
		if (Raw.Equals(TEXT("standalone"), ESearchCase::IgnoreCase))
		{
			OutMode = EPIE_StartMode::Standalone;
			return true;
		}
		OutError = FString::Printf(
			TEXT("mode '%s' invalid — accepted: selected_viewport | new_window | standalone"), *Raw);
		return false;
	}

	/**
	 * Read optional viewport_size from JSON. Accepts ``[width, height]`` JSON array form (plan
	 * default ``[1280, 720]``). Returns true on present + valid; false when missing (caller falls
	 * back to default). Sets ``OutError`` on present-but-malformed (caller surfaces invalid params).
	 */
	bool PIE_ReadViewportSize(
		const TSharedPtr<FJsonObject>& Args,
		FIntPoint& OutSize,
		bool& bOutPresent,
		FString& OutError)
	{
		bOutPresent = false;
		if (!Args.IsValid())
		{
			return true;
		}
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Args->TryGetArrayField(TEXT("viewport_size"), Arr) || !Arr)
		{
			return true; // not present — fall back to default
		}
		if (Arr->Num() != 2)
		{
			OutError = FString::Printf(
				TEXT("viewport_size must be [width,height] (2 ints); got %d entries"), Arr->Num());
			return false;
		}
		int32 W = 0, H = 0;
		if (!(*Arr)[0]->TryGetNumber(W) || !(*Arr)[1]->TryGetNumber(H))
		{
			OutError = TEXT("viewport_size entries must be integers");
			return false;
		}
		if (W < 32 || W > 8192 || H < 32 || H > 8192)
		{
			OutError = FString::Printf(
				TEXT("viewport_size out of range — each dim must be in [32, 8192], got (%d, %d)"), W, H);
			return false;
		}
		OutSize = FIntPoint(W, H);
		bOutPresent = true;
		return true;
	}
} // namespace

namespace FPIETools
{

// ─── pie.start ─────────────────────────────────────────────────────────────────────────────────
//
// Args:
//   mode           : "selected_viewport" (default) | "new_window" | "standalone"
//   viewport_size  : [W, H] (default [1280, 720])
//   player_count   : int [1, 4] (default 1)
//
// Response on success: { started: bool, pie_world_path: "..." | null }
//   - ``pie_world_path`` is null when start was requested but PlayWorld is not yet up (PIE init
//     spans multiple ticks; caller polls ``pie.is_running`` to wait).
//
// Errors:
//   -32602 InvalidParams        mode unrecognised / viewport_size malformed / player_count OOB
//   -32603 Internal             GEditor missing OR ULevelEditorPlaySettings unavailable OR
//                               StartQueuedPlaySessionRequest didn't fire the request
//   -ALREADY_RUNNING-           IsPlaySessionInProgress already true → surface as Internal +
//                               diagnostic; caller polls pie.is_running
FMCPResponse Tool_Start(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!GEditor)
	{
		return PIE_MakeError(Request, kPIEErrorInternal, TEXT("GEditor unavailable (commandlet?)"));
	}

	// Already-running guard — refuse rather than silently restarting.
	if (GEditor->IsPlaySessionInProgress() || FMCPWorldContext::IsPIEActive())
	{
		return PIE_MakeError(Request, kPIEErrorInternal,
			TEXT("PIE session already in progress; call pie.stop first or pie.is_running to check"));
	}

	// Parse mode.
	FString ModeRaw;
	if (Request.Args.IsValid())
	{
		Request.Args->TryGetStringField(TEXT("mode"), ModeRaw);
	}
	EPIE_StartMode Mode = EPIE_StartMode::SelectedViewport;
	FString ParseErr;
	if (!PIE_ParseStartMode(ModeRaw, Mode, ParseErr))
	{
		return PIE_MakeError(Request, kPIEErrorInvalidParams, ParseErr);
	}

	// Parse viewport_size (optional).
	FIntPoint Size(1280, 720);
	bool bSizePresent = false;
	FString SizeErr;
	if (!PIE_ReadViewportSize(Request.Args, Size, bSizePresent, SizeErr))
	{
		return PIE_MakeError(Request, kPIEErrorInvalidParams, SizeErr);
	}

	// Parse player_count (optional).
	int32 PlayerCount = 1;
	if (Request.Args.IsValid())
	{
		Request.Args->TryGetNumberField(TEXT("player_count"), PlayerCount);
	}
	if (PlayerCount < 1 || PlayerCount > 4)
	{
		return PIE_MakeError(Request, kPIEErrorInvalidParams,
			FString::Printf(TEXT("player_count must be in [1, 4]; got %d"), PlayerCount));
	}

	// Locate (and mutate) the editor's ULevelEditorPlaySettings — RequestPlaySession sources its
	// "how should we start" configuration from this CDO (or any caller-supplied override). We mutate
	// the CDO transiently: PIE will read these fields when StartQueuedPlaySessionRequest fires next.
	// Restoring the original values after PIE starts is a future polish; current behavior matches
	// the user clicking the matching options in the editor's Play menu (which also mutates the CDO).
	ULevelEditorPlaySettings* PlaySettings = GetMutableDefault<ULevelEditorPlaySettings>();
	if (!PlaySettings)
	{
		return PIE_MakeError(Request, kPIEErrorInternal,
			TEXT("ULevelEditorPlaySettings CDO unavailable"));
	}

	// Apply size if caller provided one. Otherwise leave whatever the user last set.
	if (bSizePresent)
	{
		PlaySettings->NewWindowWidth = Size.X;
		PlaySettings->NewWindowHeight = Size.Y;
	}
	// PlayNumberOfClients is a private UPROPERTY — use the public setter rather than direct access.
	PlaySettings->SetPlayNumberOfClients(PlayerCount);
	// Stamp LastExecutedPlayModeType so the editor's "Play" button mirrors the mode we picked.
	switch (Mode)
	{
		case EPIE_StartMode::SelectedViewport:
			PlaySettings->LastExecutedPlayModeType = PlayMode_InViewPort;
			break;
		case EPIE_StartMode::NewWindow:
			PlaySettings->LastExecutedPlayModeType = PlayMode_InEditorFloating;
			break;
		case EPIE_StartMode::Standalone:
			PlaySettings->LastExecutedPlayModeType = PlayMode_InNewProcess;
			break;
	}

	// Build the request params. SessionDestination matches the mode; WorldType is PlayInEditor
	// always (Simulate-in-Editor is not exposed by this MVP — Phase 5 plan defers it).
	FRequestPlaySessionParams Params;
	Params.WorldType = EPlaySessionWorldType::PlayInEditor;
	Params.EditorPlaySettings = PlaySettings;
	Params.bAllowOnlineSubsystem = false; // headless safety — never block on online subsystem
	switch (Mode)
	{
		case EPIE_StartMode::SelectedViewport:
		case EPIE_StartMode::NewWindow:
			Params.SessionDestination = EPlaySessionDestinationType::InProcess;
			break;
		case EPIE_StartMode::Standalone:
			Params.SessionDestination = EPlaySessionDestinationType::NewProcess;
			break;
	}

	GEditor->RequestPlaySession(Params);
	// Force the queued request to take effect this tick rather than next-frame. This puts PlayWorld
	// into a settable state within ~one engine tick; caller still needs to poll pie.is_running to
	// wait for the world to fully spin up.
	GEditor->StartQueuedPlaySessionRequest();

	// PlayWorld may or may not be ready right now (depends on whether PIE startup completed within
	// the StartQueuedPlaySessionRequest call). Report what we can; caller polls is_running.
	const FString PlayWorldPath = (GEditor->PlayWorld && GEditor->PlayWorld->GetOutermost())
		? GEditor->PlayWorld->GetOutermost()->GetName()
		: FString();

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("started"), true);
	if (PlayWorldPath.IsEmpty())
	{
		Out->SetField(TEXT("pie_world_path"), MakeShared<FJsonValueNull>());
	}
	else
	{
		Out->SetStringField(TEXT("pie_world_path"), PlayWorldPath);
	}
	return PIE_MakeSuccessObj(Request, Out);
}

// ─── pie.stop ──────────────────────────────────────────────────────────────────────────────────
//
// Args: {}
// Response: { stopped: bool }
//
// Errors:
//   -32038 PIENotActive  No PIE session running
//   -32603 Internal      GEditor missing
//
// Implementation: uses the async ``RequestEndPlayMap`` variant — safer than direct ``EndPlayMap``
// because it queues the teardown to the engine's next tick, avoiding "EndPlayMap called from
// within EndPlayMap" reentrancy that plain EndPlayMap can hit.
FMCPResponse Tool_Stop(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!GEditor)
	{
		return PIE_MakeError(Request, kPIEErrorInternal, TEXT("GEditor unavailable (commandlet?)"));
	}
	if (!FMCPWorldContext::IsPIEActive())
	{
		return PIE_MakeNotActiveError(Request);
	}

	GEditor->RequestEndPlayMap();

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("stopped"), true);
	return PIE_MakeSuccessObj(Request, Out);
}

// ─── pie.pause ─────────────────────────────────────────────────────────────────────────────────
//
// Args: {}
// Response: { paused: bool }
//
// Errors:
//   -32038 PIENotActive  No PIE session running
//   -32603 Internal      GEditor->SetPIEWorldsPaused returned false (already-paused signals success
//                        path with paused=true)
FMCPResponse Tool_Pause(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!GEditor)
	{
		return PIE_MakeError(Request, kPIEErrorInternal, TEXT("GEditor unavailable (commandlet?)"));
	}
	if (!FMCPWorldContext::IsPIEActive())
	{
		return PIE_MakeNotActiveError(Request);
	}

	// SetPIEWorldsPaused returns true on success, false on "no worlds to pause". The latter is a
	// transient race (PIE start hasn't propagated worlds yet) — surface as paused=false so caller
	// can retry. We do NOT separately surface "already paused" as an error; the call is idempotent
	// at the wire surface.
	const bool bOk = GEditor->SetPIEWorldsPaused(true);

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("paused"), bOk);
	return PIE_MakeSuccessObj(Request, Out);
}

// ─── pie.resume ────────────────────────────────────────────────────────────────────────────────
//
// Args: {}
// Response: { resumed: bool }
//
// Errors:
//   -32038 PIENotActive  No PIE session running
FMCPResponse Tool_Resume(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!GEditor)
	{
		return PIE_MakeError(Request, kPIEErrorInternal, TEXT("GEditor unavailable (commandlet?)"));
	}
	if (!FMCPWorldContext::IsPIEActive())
	{
		return PIE_MakeNotActiveError(Request);
	}

	const bool bOk = GEditor->SetPIEWorldsPaused(false);

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("resumed"), bOk);
	return PIE_MakeSuccessObj(Request, Out);
}

// ─── pie.step_frame ────────────────────────────────────────────────────────────────────────────
//
// Args: {}
// Response: { advanced: bool, current_frame: int }
//
// Errors:
//   -32038 PIENotActive  No PIE session running
//   -32603 Internal      PIE is running but NOT paused (must call pie.pause first to be in a
//                        steppable state) OR GEditor missing
//
// Uses GEditor->PlaySessionSingleStepped which is the same path the editor's Frame Step button
// invokes — it advances every PIE world by exactly one tick while keeping the session paused.
FMCPResponse Tool_StepFrame(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!GEditor)
	{
		return PIE_MakeError(Request, kPIEErrorInternal, TEXT("GEditor unavailable (commandlet?)"));
	}
	if (!FMCPWorldContext::IsPIEActive())
	{
		return PIE_MakeNotActiveError(Request);
	}
	// Step requires pause — refuse otherwise so caller doesn't silently miss the requirement.
	if (!PIE_IsAnyWorldPaused())
	{
		return PIE_MakeError(Request, kPIEErrorInternal,
			TEXT("pie.step_frame requires PIE to be paused first; call pie.pause"));
	}

	GEditor->PlaySessionSingleStepped();

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("advanced"), true);
	// GFrameCounter is the global engine frame index — a monotonic uint64. Cast to double for JSON.
	Out->SetNumberField(TEXT("current_frame"), static_cast<double>(GFrameCounter));
	return PIE_MakeSuccessObj(Request, Out);
}

// ─── pie.console_exec ──────────────────────────────────────────────────────────────────────────
//
// Args:
//   command : required string, min length 1
//   world   : "pie" (default) | "editor" | "server" | "all_pie"
//
// Response: { executed: bool, output: string }
//
// Errors:
//   -32602 InvalidParams     missing command / empty command
//   -32038 PIENotActive       world=pie/server/all_pie but no PIE running
//   -32603 Internal           world=server but no dedicated-server PIE world (NO_SERVER_WORLD per plan)
//                             OR world=editor but no editor world (rare)
//
// Note on output capture: ``World->Exec`` writes to the supplied FOutputDevice ONLY for commands
// that explicitly route to it. Many cvar/stat commands write to GLog or to the viewport HUD; their
// output won't appear here. Caller can fall back to log.tail after invoking for those cases.
FMCPResponse Tool_ConsoleExec(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!Request.Args.IsValid())
	{
		return PIE_MakeError(Request, kPIEErrorInvalidParams, TEXT("missing args object"));
	}
	FString Command;
	if (!Request.Args->TryGetStringField(TEXT("command"), Command) || Command.IsEmpty())
	{
		return PIE_MakeError(Request, kPIEErrorInvalidParams,
			TEXT("missing required string field 'command'"));
	}
	FString WorldArg = TEXT("pie");
	Request.Args->TryGetStringField(TEXT("world"), WorldArg);

	// Resolve target world list — multi-target only for world='all_pie'.
	TArray<UWorld*, TInlineAllocator<4>> Targets;
	if (WorldArg.Equals(TEXT("editor"), ESearchCase::IgnoreCase))
	{
		UWorld* Editor = FMCPWorldContext::GetEditorWorld();
		if (!Editor)
		{
			return PIE_MakeError(Request, kPIEErrorInternal,
				TEXT("no editor world available (GEditor missing?)"));
		}
		Targets.Add(Editor);
	}
	else if (WorldArg.Equals(TEXT("server"), ESearchCase::IgnoreCase))
	{
		if (!FMCPWorldContext::IsPIEActive())
		{
			return PIE_MakeNotActiveError(Request);
		}
		UWorld* Server = PIE_FindServerWorld();
		if (!Server)
		{
			return PIE_MakeError(Request, kPIEErrorInternal,
				TEXT("no dedicated-server PIE world (NO_SERVER_WORLD) — start PIE with multiplayer + dedicated server"));
		}
		Targets.Add(Server);
	}
	else if (WorldArg.Equals(TEXT("all_pie"), ESearchCase::IgnoreCase))
	{
		if (!FMCPWorldContext::IsPIEActive())
		{
			return PIE_MakeNotActiveError(Request);
		}
		TArray<UWorld*> All;
		PIE_GatherAllWorlds(All);
		for (UWorld* W : All)
		{
			Targets.Add(W);
		}
		if (Targets.Num() == 0)
		{
			return PIE_MakeNotActiveError(Request);
		}
	}
	else
	{
		// Default 'pie' — first non-server PIE world.
		if (!FMCPWorldContext::IsPIEActive())
		{
			return PIE_MakeNotActiveError(Request);
		}
		UWorld* Client = PIE_FindFirstClientWorld();
		if (!Client)
		{
			return PIE_MakeNotActiveError(Request);
		}
		Targets.Add(Client);
	}

	// Capture output across all targeted worlds. The device's Serialize concatenates all writes,
	// so multi-world (all_pie) outputs are merged into a single string with a separator header
	// per world so callers can tell them apart.
	FStringOutputDevice OutputDevice;
	OutputDevice.SetAutoEmitLineTerminator(true);

	bool bAnyOk = false;
	for (UWorld* W : Targets)
	{
		if (Targets.Num() > 1 && W && W->GetOutermost())
		{
			// Multi-world prefix so caller can disambiguate which world emitted what.
			OutputDevice.Logf(TEXT("[world=%s]"), *W->GetOutermost()->GetName());
		}
		const bool bOk = W && W->Exec(W, *Command, OutputDevice);
		bAnyOk = bAnyOk || bOk;
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("executed"), bAnyOk);
	Out->SetStringField(TEXT("output"), static_cast<const FString&>(OutputDevice));
	return PIE_MakeSuccessObj(Request, Out);
}

// ─── pie.is_running ────────────────────────────────────────────────────────────────────────────
//
// Args: {}
// Response: { running: bool, paused: bool, world_count: int }
//
// Always callable — no PIE-required guard. Reports current PIE state regardless of whether a
// session is up.
FMCPResponse Tool_IsRunning(const FMCPRequest& Request)
{
	check(IsInGameThread());

	const bool bRunning = FMCPWorldContext::IsPIEActive();
	const bool bPaused = bRunning && PIE_IsAnyWorldPaused();
	const int32 WorldCount = bRunning ? PIE_CountWorlds() : 0;

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("running"), bRunning);
	Out->SetBoolField(TEXT("paused"), bPaused);
	Out->SetNumberField(TEXT("world_count"), static_cast<double>(WorldCount));
	return PIE_MakeSuccessObj(Request, Out);
}

// ─── pie.get_player_controller ─────────────────────────────────────────────────────────────────
//
// Args:
//   player_index : int [0, ∞) (default 0)
//
// Response: { pc_actor_guid: string, pc_path: string }
//
// Errors:
//   -32038 PIENotActive       No PIE session
//   -32603 Internal           No PIE client world (PIE up but no playable world yet — rare race)
//   -32602 InvalidParams      player_index < 0
//   -32004 ObjectNotFound     index out of range (no PC at that index — INVALID_PLAYER_INDEX per plan)
FMCPResponse Tool_GetPlayerController(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!FMCPWorldContext::IsPIEActive())
	{
		return PIE_MakeNotActiveError(Request);
	}
	int32 Index = 0;
	if (Request.Args.IsValid())
	{
		Request.Args->TryGetNumberField(TEXT("player_index"), Index);
	}
	if (Index < 0)
	{
		return PIE_MakeError(Request, kPIEErrorInvalidParams,
			FString::Printf(TEXT("player_index must be >= 0; got %d"), Index));
	}

	UWorld* World = PIE_FindFirstClientWorld();
	if (!World)
	{
		return PIE_MakeError(Request, kPIEErrorInternal,
			TEXT("no PIE client world available (PIE may still be initialising)"));
	}

	APlayerController* PC = UGameplayStatics::GetPlayerController(World, Index);
	if (!PC)
	{
		return PIE_MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(
				TEXT("no PlayerController at player_index=%d (INVALID_PLAYER_INDEX); use pie.is_running to inspect world_count"),
				Index));
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> OutPtr = Out;
	PIE_AddActorIdentityFields(PC, TEXT("pc_actor_guid"), TEXT("pc_path"), OutPtr);
	return PIE_MakeSuccessObj(Request, Out);
}

// ─── pie.get_pawn ──────────────────────────────────────────────────────────────────────────────
//
// Args:
//   player_index : int [0, ∞) (default 0)
//
// Response: { pawn_actor_guid: string, pawn_path: string, class: string }
//
// Errors:
//   -32038 PIENotActive       No PIE
//   -32603 Internal           No PIE client world
//   -32602 InvalidParams      player_index < 0
//   -32004 ObjectNotFound     PC at index doesn't exist OR PC has no possessed pawn (NO_PAWN per plan)
FMCPResponse Tool_GetPawn(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!FMCPWorldContext::IsPIEActive())
	{
		return PIE_MakeNotActiveError(Request);
	}
	int32 Index = 0;
	if (Request.Args.IsValid())
	{
		Request.Args->TryGetNumberField(TEXT("player_index"), Index);
	}
	if (Index < 0)
	{
		return PIE_MakeError(Request, kPIEErrorInvalidParams,
			FString::Printf(TEXT("player_index must be >= 0; got %d"), Index));
	}

	UWorld* World = PIE_FindFirstClientWorld();
	if (!World)
	{
		return PIE_MakeError(Request, kPIEErrorInternal,
			TEXT("no PIE client world available (PIE may still be initialising)"));
	}

	APlayerController* PC = UGameplayStatics::GetPlayerController(World, Index);
	if (!PC)
	{
		return PIE_MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(
				TEXT("no PlayerController at player_index=%d (INVALID_PLAYER_INDEX); use pie.is_running"),
				Index));
	}
	APawn* Pawn = PC->GetPawn();
	if (!Pawn)
	{
		return PIE_MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(
				TEXT("PlayerController at player_index=%d has no possessed Pawn (NO_PAWN); possess one or wait for spawn"),
				Index));
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> OutPtr = Out;
	PIE_AddActorIdentityFields(Pawn, TEXT("pawn_actor_guid"), TEXT("pawn_path"), OutPtr);
	Out->SetStringField(TEXT("class"), Pawn->GetClass()->GetPathName());
	return PIE_MakeSuccessObj(Request, Out);
}

// ─── pie.focus_actor ───────────────────────────────────────────────────────────────────────────
//
// Args:
//   actor_id        : required string (Phase 3 actor path — any of the 3 forms)
//   camera_distance : optional float [1.0, ∞) (default 500.0); currently advisory — the underlying
//                     MoveViewportCamerasToActor computes a fitting distance from actor bounds.
//                     We surface the param so a future enhancement can override via MoveViewportCamerasToBox.
//
// Response: { focused: bool }
//
// Errors:
//   -32038 PIENotActive       No PIE
//   -32602 InvalidParams      missing actor_id
//   -32004 ObjectNotFound     actor_id doesn't resolve (ACTOR_NOT_FOUND per plan)
//
// Implementation: passes ``bActiveViewportOnly=true`` so only the focused (or PIE-host) viewport
// jumps — avoids disturbing other editor viewports. Resolution uses ``ResolveActor(bRejectPIE=false)``
// so the caller can pass actor ids from either editor OR PIE world; this is the intended Phase 5
// behaviour (focus on PIE-spawned actors is a common use case).
FMCPResponse Tool_FocusActor(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!GEditor)
	{
		return PIE_MakeError(Request, kPIEErrorInternal, TEXT("GEditor unavailable (commandlet?)"));
	}
	if (!FMCPWorldContext::IsPIEActive())
	{
		return PIE_MakeNotActiveError(Request);
	}
	if (!Request.Args.IsValid())
	{
		return PIE_MakeError(Request, kPIEErrorInvalidParams, TEXT("missing args object"));
	}
	FString ActorId;
	if (!Request.Args->TryGetStringField(TEXT("actor_id"), ActorId) || ActorId.IsEmpty())
	{
		return PIE_MakeError(Request, kPIEErrorInvalidParams,
			TEXT("missing required string field 'actor_id'"));
	}

	// camera_distance is currently advisory but we validate range so future plumbing has a clean
	// input contract. Default 500.0 per plan.
	double CameraDistance = 500.0;
	Request.Args->TryGetNumberField(TEXT("camera_distance"), CameraDistance);
	if (CameraDistance < 1.0)
	{
		return PIE_MakeError(Request, kPIEErrorInvalidParams,
			FString::Printf(TEXT("camera_distance must be >= 1.0; got %g"), CameraDistance));
	}

	bool bAmbig = false;
	FString AmbigHint;
	FString ResolveErr;
	AActor* Target = FMCPActorPathUtils::ResolveActor(ActorId,
		/*bRejectPIE*/ false, bAmbig, AmbigHint, ResolveErr);
	if (!Target)
	{
		return PIE_MakeError(Request, kMCPErrorObjectNotFound, ResolveErr);
	}

	// bActiveViewportOnly=true — restrict the jump to the focused viewport so we don't disturb
	// other editor viewports the user may be using to monitor PIE from a different angle.
	GEditor->MoveViewportCamerasToActor(*Target, /*bActiveViewportOnly*/ true);

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("focused"), true);
	return PIE_MakeSuccessObj(Request, Out);
}

// ─── Registration ──────────────────────────────────────────────────────────────────────────────
void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	// PIE lifecycle.
	RegisterTool(TEXT("pie.start"),      &Tool_Start,      /*Lane A*/ false);
	RegisterTool(TEXT("pie.stop"),       &Tool_Stop,       /*Lane A*/ false);
	RegisterTool(TEXT("pie.pause"),      &Tool_Pause,      /*Lane A*/ false);
	RegisterTool(TEXT("pie.resume"),     &Tool_Resume,     /*Lane A*/ false);
	RegisterTool(TEXT("pie.step_frame"), &Tool_StepFrame,  /*Lane A*/ false);

	// PIE introspection + console.
	RegisterTool(TEXT("pie.console_exec"), &Tool_ConsoleExec, /*Lane A*/ false);
	RegisterTool(TEXT("pie.is_running"),   &Tool_IsRunning,   /*Lane A*/ false);

	// PIE actor identity + viewport focus.
	RegisterTool(TEXT("pie.get_player_controller"), &Tool_GetPlayerController, /*Lane A*/ false);
	RegisterTool(TEXT("pie.get_pawn"),              &Tool_GetPawn,             /*Lane A*/ false);
	RegisterTool(TEXT("pie.focus_actor"),           &Tool_FocusActor,          /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("Phase 5 Chunk A: registered 10 pie.* handlers (lifecycle + introspection + actor identity, all Lane A)"));
}

} // namespace FPIETools

#undef LOCTEXT_NAMESPACE
