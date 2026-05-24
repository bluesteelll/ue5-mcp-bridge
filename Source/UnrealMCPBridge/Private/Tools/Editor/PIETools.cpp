// Copyright FatumGame. All Rights Reserved.

#include "PIETools.h"

#include "MCPSurfaceRegistry.h"

#include "FMCPDispatchQueue.h"
#include "MCPJsonBuilder.h"
#include "MCPToolHelpers.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPActorPathUtils.h"
#include "Utils/MCPWorldContext.h"

#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "HAL/PlatformTime.h"
#include <atomic>
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
#include "UObject/UObjectIterator.h"

// Wave A: input simulation + stats.
#include "Camera/PlayerCameraManager.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerInput.h"
#include "HAL/PlatformMemory.h"
#include "InputCoreTypes.h"

// Engine globals for stats — extern forward decl matches UnrealEngine.cpp's definitions.
extern ENGINE_API float GAverageFPS;
extern ENGINE_API float GAverageMS;

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// PIE_ prefix retained for any helper unique to this surface.
	constexpr int32 kPIEErrorInvalidParams = kMCPErrorInvalidParams;
	constexpr int32 kPIEErrorInternal      = kMCPErrorInternal;

	/** Frozen PIE-required refusal (-32038). Smoke asserts "PIE is not running" + "pie.start" + "editor.*". */
	FMCPResponse PIE_MakeNotActiveError(const FMCPRequest& Request)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPIENotActive, kMCPMessagePIENotActive);
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
// Wave S+9 (2026-05-24): UE PIE start/stop has async cleanup phases (BeginTearingDown +
// Audio teardown + World cleanup) that overlap if start fires too soon after stop. Tested via
// WS3 Phase Q with 0.3s gap → crash in audio mixer during teardown of previous PIE while
// new one starts. Track last-stop-time and enforce minimum gap before next start.
static std::atomic<double> gLastPIEStopTimeSeconds{0.0};
static constexpr double kMinPIEStartGapSeconds = 1.5;

FMCPResponse Tool_Start(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (!GEditor)
	{
		return FMCPToolHelpers::MakeError(Request, kPIEErrorInternal, TEXT("GEditor unavailable (commandlet?)"));
	}

	// Already-running guard — refuse rather than silently restarting. Also check PlayWorld!=null
	// which covers the Playing AND Ending phases (IsPlaySessionInProgress goes false during Ending
	// but PlayWorld is still being torn down).
	if (GEditor->IsPlaySessionInProgress() || FMCPWorldContext::IsPIEActive() || GEditor->PlayWorld != nullptr)
	{
		return FMCPToolHelpers::MakeError(Request, kPIEErrorInternal,
			TEXT("PIE session already in progress; call pie.stop first or pie.is_running to check"));
	}

	// Wave S+9 cooldown guard: pie.stop is async — UE finishes teardown (audio, world cleanup,
	// online subsystems) over ~500ms-1s AFTER the function returns. Starting again before that
	// completes crashes the editor in the audio mixer or other cleanup paths. Verified via WS3
	// Phase Q stress (30 rapid cycles with 0.3s gap → editor died on iteration 2).
	const double Now = FPlatformTime::Seconds();
	const double LastStop = gLastPIEStopTimeSeconds.load(std::memory_order_acquire);
	if (LastStop > 0.0 && (Now - LastStop) < kMinPIEStartGapSeconds)
	{
		const double Remaining = kMinPIEStartGapSeconds - (Now - LastStop);
		return FMCPToolHelpers::MakeError(Request, kPIEErrorInternal,
			FString::Printf(
				TEXT("pie.start too soon after last pie.stop; UE PIE teardown is async — wait %.2fs more "
					 "(min gap=%.1fs). Use pie.is_running to poll for teardown completion."),
				Remaining, kMinPIEStartGapSeconds));
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
		return FMCPToolHelpers::MakeError(Request, kPIEErrorInvalidParams, ParseErr);
	}

	// Parse viewport_size (optional).
	FIntPoint Size(1280, 720);
	bool bSizePresent = false;
	FString SizeErr;
	if (!PIE_ReadViewportSize(Request.Args, Size, bSizePresent, SizeErr))
	{
		return FMCPToolHelpers::MakeError(Request, kPIEErrorInvalidParams, SizeErr);
	}

	// Parse player_count (optional).
	int32 PlayerCount = 1;
	if (Request.Args.IsValid())
	{
		Request.Args->TryGetNumberField(TEXT("player_count"), PlayerCount);
	}
	if (PlayerCount < 1 || PlayerCount > 4)
	{
		return FMCPToolHelpers::MakeError(Request, kPIEErrorInvalidParams,
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
		return FMCPToolHelpers::MakeError(Request, kPIEErrorInternal,
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

	return FMCPJsonBuilder()
		.Bool(TEXT("started"), true)
		.If(PlayWorldPath.IsEmpty(),  [&](FMCPJsonBuilder& B) { B.Null(TEXT("pie_world_path")); })
		.If(!PlayWorldPath.IsEmpty(), [&](FMCPJsonBuilder& B) { B.Str(TEXT("pie_world_path"), PlayWorldPath); })
		.BuildSuccess(Request);
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
		return FMCPToolHelpers::MakeError(Request, kPIEErrorInternal, TEXT("GEditor unavailable (commandlet?)"));
	}
	if (!FMCPWorldContext::IsPIEActive())
	{
		return PIE_MakeNotActiveError(Request);
	}

	GEditor->RequestEndPlayMap();

	// Wave S+9: record stop time so the next pie.start can enforce a cooldown gap.
	// UE PIE teardown is async (audio mixer, world cleanup, online subsystems all unwind
	// over ~1s after RequestEndPlayMap returns); restarting too soon crashes the editor.
	gLastPIEStopTimeSeconds.store(FPlatformTime::Seconds(), std::memory_order_release);

	return FMCPJsonBuilder()
		.Bool(TEXT("stopped"), true)
		.BuildSuccess(Request);
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
		return FMCPToolHelpers::MakeError(Request, kPIEErrorInternal, TEXT("GEditor unavailable (commandlet?)"));
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

	return FMCPJsonBuilder()
		.Bool(TEXT("paused"), bOk)
		.BuildSuccess(Request);
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
		return FMCPToolHelpers::MakeError(Request, kPIEErrorInternal, TEXT("GEditor unavailable (commandlet?)"));
	}
	if (!FMCPWorldContext::IsPIEActive())
	{
		return PIE_MakeNotActiveError(Request);
	}

	const bool bOk = GEditor->SetPIEWorldsPaused(false);

	return FMCPJsonBuilder()
		.Bool(TEXT("resumed"), bOk)
		.BuildSuccess(Request);
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
		return FMCPToolHelpers::MakeError(Request, kPIEErrorInternal, TEXT("GEditor unavailable (commandlet?)"));
	}
	if (!FMCPWorldContext::IsPIEActive())
	{
		return PIE_MakeNotActiveError(Request);
	}
	// Step requires pause — refuse otherwise so caller doesn't silently miss the requirement.
	if (!PIE_IsAnyWorldPaused())
	{
		return FMCPToolHelpers::MakeError(Request, kPIEErrorInternal,
			TEXT("pie.step_frame requires PIE to be paused first; call pie.pause"));
	}

	GEditor->PlaySessionSingleStepped();

	// GFrameCounter is the global engine frame index — a monotonic uint64. Cast to double for JSON.
	return FMCPJsonBuilder()
		.Bool(TEXT("advanced"), true)
		.Num(TEXT("current_frame"), static_cast<double>(GFrameCounter))
		.BuildSuccess(Request);
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
		return FMCPToolHelpers::MakeError(Request, kPIEErrorInvalidParams, TEXT("missing args object"));
	}
	FString Command;
	if (!Request.Args->TryGetStringField(TEXT("command"), Command) || Command.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kPIEErrorInvalidParams,
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
			return FMCPToolHelpers::MakeError(Request, kPIEErrorInternal,
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
			return FMCPToolHelpers::MakeError(Request, kPIEErrorInternal,
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

	return FMCPJsonBuilder()
		.Bool(TEXT("executed"), bAnyOk)
		.Str(TEXT("output"), static_cast<const FString&>(OutputDevice))
		.BuildSuccess(Request);
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

	return FMCPJsonBuilder()
		.Bool(TEXT("running"), bRunning)
		.Bool(TEXT("paused"), bPaused)
		.Num(TEXT("world_count"), static_cast<double>(WorldCount))
		.BuildSuccess(Request);
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
		return FMCPToolHelpers::MakeError(Request, kPIEErrorInvalidParams,
			FString::Printf(TEXT("player_index must be >= 0; got %d"), Index));
	}

	UWorld* World = PIE_FindFirstClientWorld();
	if (!World)
	{
		return FMCPToolHelpers::MakeError(Request, kPIEErrorInternal,
			TEXT("no PIE client world available (PIE may still be initialising)"));
	}

	APlayerController* PC = UGameplayStatics::GetPlayerController(World, Index);
	if (!PC)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(
				TEXT("no PlayerController at player_index=%d (INVALID_PLAYER_INDEX); use pie.is_running to inspect world_count"),
				Index));
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> OutPtr = Out;
	PIE_AddActorIdentityFields(PC, TEXT("pc_actor_guid"), TEXT("pc_path"), OutPtr);
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
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
		return FMCPToolHelpers::MakeError(Request, kPIEErrorInvalidParams,
			FString::Printf(TEXT("player_index must be >= 0; got %d"), Index));
	}

	UWorld* World = PIE_FindFirstClientWorld();
	if (!World)
	{
		return FMCPToolHelpers::MakeError(Request, kPIEErrorInternal,
			TEXT("no PIE client world available (PIE may still be initialising)"));
	}

	APlayerController* PC = UGameplayStatics::GetPlayerController(World, Index);
	if (!PC)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(
				TEXT("no PlayerController at player_index=%d (INVALID_PLAYER_INDEX); use pie.is_running"),
				Index));
	}
	APawn* Pawn = PC->GetPawn();
	if (!Pawn)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(
				TEXT("PlayerController at player_index=%d has no possessed Pawn (NO_PAWN); possess one or wait for spawn"),
				Index));
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> OutPtr = Out;
	PIE_AddActorIdentityFields(Pawn, TEXT("pawn_actor_guid"), TEXT("pawn_path"), OutPtr);
	Out->SetStringField(TEXT("class"), Pawn->GetClass()->GetPathName());
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
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
		return FMCPToolHelpers::MakeError(Request, kPIEErrorInternal, TEXT("GEditor unavailable (commandlet?)"));
	}
	if (!FMCPWorldContext::IsPIEActive())
	{
		return PIE_MakeNotActiveError(Request);
	}
	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kPIEErrorInvalidParams, TEXT("missing args object"));
	}
	FString ActorId;
	if (!Request.Args->TryGetStringField(TEXT("actor_id"), ActorId) || ActorId.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kPIEErrorInvalidParams,
			TEXT("missing required string field 'actor_id'"));
	}

	// camera_distance is currently advisory but we validate range so future plumbing has a clean
	// input contract. Default 500.0 per plan.
	double CameraDistance = 500.0;
	Request.Args->TryGetNumberField(TEXT("camera_distance"), CameraDistance);
	if (CameraDistance < 1.0)
	{
		return FMCPToolHelpers::MakeError(Request, kPIEErrorInvalidParams,
			FString::Printf(TEXT("camera_distance must be >= 1.0; got %g"), CameraDistance));
	}

	bool bAmbig = false;
	FString AmbigHint;
	FString ResolveErr;
	AActor* Target = FMCPActorPathUtils::ResolveActor(ActorId,
		/*bRejectPIE*/ false, bAmbig, AmbigHint, ResolveErr);
	if (!Target)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound, ResolveErr);
	}

	// bActiveViewportOnly=true — restrict the jump to the focused viewport so we don't disturb
	// other editor viewports the user may be using to monitor PIE from a different angle.
	GEditor->MoveViewportCamerasToActor(*Target, /*bActiveViewportOnly*/ true);

	return FMCPJsonBuilder()
		.Bool(TEXT("focused"), true)
		.BuildSuccess(Request);
}

// ═══════════════════════════════════════════════════════════════════════════════════════════════
// Wave A 2026-05 — PIE functional testing surface.
// ═══════════════════════════════════════════════════════════════════════════════════════════════

// ─── pie.simulate_key — keyboard/mouse-button input ─────────────────────────────────────────
//
// Args:
//   - key:        string (required)  FKey name (e.g. "W", "Space", "F", "LeftMouseButton")
//   - action:     string (optional)  "press" | "release" | "tap" (down+up). Default "tap".
//   - player_index: int  (optional)  0
//
// Routes through PlayerController::InputKey rather than Slate so game-side input handlers
// fire correctly. For pure UI focus events use pie.click_screen.
FMCPResponse Tool_SimulateKey(const FMCPRequest& Request)
{
	check(IsInGameThread());
	if (!GEditor || !GEditor->PlayWorld)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPIENotActive, kMCPMessagePIENotActive);
	}
	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kPIEErrorInvalidParams, TEXT("pie.simulate_key requires args.key"));
	}
	FString KeyName, Action(TEXT("tap"));
	int32 PlayerIdx = 0;
	if (!Request.Args->TryGetStringField(TEXT("key"), KeyName) || KeyName.IsEmpty())
	{
		return FMCPToolHelpers::MakeError(Request, kPIEErrorInvalidParams, TEXT("missing required string 'key'"));
	}
	Request.Args->TryGetStringField(TEXT("action"), Action);
	Request.Args->TryGetNumberField(TEXT("player_index"), PlayerIdx);

	const FKey TheKey(*KeyName);
	if (!TheKey.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kPIEErrorInvalidParams,
			FString::Printf(TEXT("unknown FKey name '%s' (see FKey reference, e.g. 'W', 'Space', 'LeftMouseButton')"), *KeyName));
	}

	APlayerController* PC = UGameplayStatics::GetPlayerController(GEditor->PlayWorld, PlayerIdx);
	if (!PC)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("no PlayerController at player_index=%d"), PlayerIdx));
	}

	const FString ActionLower = Action.ToLower();
	if (ActionLower == TEXT("press") || ActionLower == TEXT("tap"))
	{
		PC->InputKey(FInputKeyParams(TheKey, IE_Pressed, 1.0, false));
	}
	if (ActionLower == TEXT("release") || ActionLower == TEXT("tap"))
	{
		PC->InputKey(FInputKeyParams(TheKey, IE_Released, 0.0, false));
	}
	if (ActionLower != TEXT("press") && ActionLower != TEXT("release") && ActionLower != TEXT("tap"))
	{
		return FMCPToolHelpers::MakeError(Request, kPIEErrorInvalidParams,
			FString::Printf(TEXT("unknown action '%s'; use 'press', 'release', or 'tap'"), *Action));
	}

	return FMCPJsonBuilder()
		.Bool(TEXT("simulated"), true)
		.Str(TEXT("key"), KeyName)
		.Str(TEXT("action"), ActionLower)
		.BuildSuccess(Request);
}

// ─── pie.click_screen — synthesize a click at screen coordinates ────────────────────────────
//
// Args:
//   - x, y:         number (required)
//   - button:       string (optional)  "left" | "right" | "middle". Default "left".
//   - player_index: int    (optional)  0
//
// Sends MouseButton DOWN + UP at the given screen coord through PC->InputKey AND moves cursor
// position. Works for UI buttons (UMG OnClicked fires) AND world clicks (PC->ProjectMouseClick).
FMCPResponse Tool_ClickScreen(const FMCPRequest& Request)
{
	check(IsInGameThread());
	if (!GEditor || !GEditor->PlayWorld)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPIENotActive, kMCPMessagePIENotActive);
	}
	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kPIEErrorInvalidParams,
			TEXT("pie.click_screen requires args.x + args.y"));
	}

	double X = -1, Y = -1;
	if (!Request.Args->TryGetNumberField(TEXT("x"), X) || !Request.Args->TryGetNumberField(TEXT("y"), Y))
	{
		return FMCPToolHelpers::MakeError(Request, kPIEErrorInvalidParams, TEXT("missing args.x or args.y"));
	}
	FString Button(TEXT("left"));
	Request.Args->TryGetStringField(TEXT("button"), Button);
	int32 PlayerIdx = 0;
	Request.Args->TryGetNumberField(TEXT("player_index"), PlayerIdx);

	APlayerController* PC = UGameplayStatics::GetPlayerController(GEditor->PlayWorld, PlayerIdx);
	if (!PC)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("no PlayerController at player_index=%d"), PlayerIdx));
	}

	FKey MouseKey = EKeys::LeftMouseButton;
	const FString BLow = Button.ToLower();
	if      (BLow == TEXT("left"))   { MouseKey = EKeys::LeftMouseButton; }
	else if (BLow == TEXT("right"))  { MouseKey = EKeys::RightMouseButton; }
	else if (BLow == TEXT("middle")) { MouseKey = EKeys::MiddleMouseButton; }
	else
	{
		return FMCPToolHelpers::MakeError(Request, kPIEErrorInvalidParams,
			FString::Printf(TEXT("unknown button '%s'; use left/right/middle"), *Button));
	}

	// Move cursor to (x,y) so UI hit-test routes correctly. UPlayerController has SetMouseLocation.
	PC->SetMouseLocation(static_cast<int32>(X), static_cast<int32>(Y));

	// Slate-level button event so UMG button OnClicked fires properly.
	if (FSlateApplication::IsInitialized())
	{
		FSlateApplication& Slate = FSlateApplication::Get();
		FPointerEvent PointerDown(0, FVector2D(X, Y), FVector2D(X, Y),
			TSet<FKey>{MouseKey}, MouseKey,
			0.f, FModifierKeysState());
		Slate.ProcessMouseButtonDownEvent(nullptr, PointerDown);
		FPointerEvent PointerUp(0, FVector2D(X, Y), FVector2D(X, Y),
			TSet<FKey>(), MouseKey,
			0.f, FModifierKeysState());
		Slate.ProcessMouseButtonUpEvent(PointerUp);
	}

	// Game-side: InputKey press+release for in-world click handlers.
	PC->InputKey(FInputKeyParams(MouseKey, IE_Pressed, 1.0, false));
	PC->InputKey(FInputKeyParams(MouseKey, IE_Released, 0.0, false));

	return FMCPJsonBuilder()
		.Bool(TEXT("clicked"), true)
		.Num(TEXT("x"), X)
		.Num(TEXT("y"), Y)
		.Str(TEXT("button"), BLow)
		.BuildSuccess(Request);
}

// ─── pie.click_actor — project actor world pos to screen, then click ────────────────────────
FMCPResponse Tool_ClickActor(const FMCPRequest& Request)
{
	check(IsInGameThread());
	if (!GEditor || !GEditor->PlayWorld)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPIENotActive, kMCPMessagePIENotActive);
	}
	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kPIEErrorInvalidParams,
			TEXT("pie.click_actor requires args.actor_path"));
	}
	FString ActorPath, Button(TEXT("left"));
	if (!Request.Args->TryGetStringField(TEXT("actor_path"), ActorPath))
	{
		return FMCPToolHelpers::MakeError(Request, kPIEErrorInvalidParams, TEXT("missing args.actor_path"));
	}
	Request.Args->TryGetStringField(TEXT("button"), Button);
	int32 PlayerIdx = 0;
	Request.Args->TryGetNumberField(TEXT("player_index"), PlayerIdx);

	bool bAmbig = false;
	FString AmbigHint, ResolveErr;
	AActor* Target = FMCPActorPathUtils::ResolveActor(ActorPath, /*bRejectPIE*/ false,
		bAmbig, AmbigHint, ResolveErr);
	if (!Target)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound, ResolveErr);
	}

	APlayerController* PC = UGameplayStatics::GetPlayerController(GEditor->PlayWorld, PlayerIdx);
	if (!PC)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound, TEXT("no PlayerController for projection"));
	}

	FVector2D ScreenPos(0, 0);
	const bool bProj = PC->ProjectWorldLocationToScreen(Target->GetActorLocation(), ScreenPos);
	if (!bProj)
	{
		return FMCPToolHelpers::MakeError(Request, kPIEErrorInternal,
			TEXT("could not project actor world location to screen (behind camera?)"));
	}

	// Reuse Tool_ClickScreen by hand-constructing a click here (cheaper than re-dispatch).
	const FString BLow = Button.ToLower();
	FKey MouseKey = EKeys::LeftMouseButton;
	if (BLow == TEXT("right"))  { MouseKey = EKeys::RightMouseButton; }
	if (BLow == TEXT("middle")) { MouseKey = EKeys::MiddleMouseButton; }

	PC->SetMouseLocation(static_cast<int32>(ScreenPos.X), static_cast<int32>(ScreenPos.Y));
	if (FSlateApplication::IsInitialized())
	{
		FSlateApplication& Slate = FSlateApplication::Get();
		FPointerEvent Down(0, ScreenPos, ScreenPos, TSet<FKey>{MouseKey}, MouseKey, 0.f, FModifierKeysState());
		FPointerEvent Up  (0, ScreenPos, ScreenPos, TSet<FKey>(),         MouseKey, 0.f, FModifierKeysState());
		Slate.ProcessMouseButtonDownEvent(nullptr, Down);
		Slate.ProcessMouseButtonUpEvent(Up);
	}
	PC->InputKey(FInputKeyParams(MouseKey, IE_Pressed,  1.0, false));
	PC->InputKey(FInputKeyParams(MouseKey, IE_Released, 0.0, false));

	return FMCPJsonBuilder()
		.Bool(TEXT("clicked"), true)
		.Str(TEXT("actor_path"), Target->GetPathName())
		.Num(TEXT("screen_x"), ScreenPos.X)
		.Num(TEXT("screen_y"), ScreenPos.Y)
		.Str(TEXT("button"), BLow)
		.BuildSuccess(Request);
}

// ─── pie.set_time_dilation — global time scale via UGameplayStatics::SetGlobalTimeDilation ──
FMCPResponse Tool_SetTimeDilation(const FMCPRequest& Request)
{
	check(IsInGameThread());
	if (!GEditor || !GEditor->PlayWorld)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPIENotActive, kMCPMessagePIENotActive);
	}
	if (!Request.Args.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kPIEErrorInvalidParams,
			TEXT("pie.set_time_dilation requires args.scale"));
	}
	double Scale = 1.0;
	if (!Request.Args->TryGetNumberField(TEXT("scale"), Scale))
	{
		return FMCPToolHelpers::MakeError(Request, kPIEErrorInvalidParams, TEXT("missing args.scale"));
	}
	if (Scale < 0.0001 || Scale > 100.0)
	{
		return FMCPToolHelpers::MakeError(Request, kPIEErrorInvalidParams,
			FString::Printf(TEXT("scale=%g outside [0.0001, 100.0]"), Scale));
	}
	const float Prior = UGameplayStatics::GetGlobalTimeDilation(GEditor->PlayWorld);
	UGameplayStatics::SetGlobalTimeDilation(GEditor->PlayWorld, static_cast<float>(Scale));

	return FMCPJsonBuilder()
		.Bool(TEXT("applied"), true)
		.Num(TEXT("prior_scale"), static_cast<double>(Prior))
		.Num(TEXT("new_scale"), Scale)
		.BuildSuccess(Request);
}

// ─── pie.get_stats — collect runtime stats snapshot ────────────────────────────────────────
FMCPResponse Tool_GetStats(const FMCPRequest& Request)
{
	check(IsInGameThread());
	if (!GEditor || !GEditor->PlayWorld)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPIENotActive, kMCPMessagePIENotActive);
	}
	UWorld* W = GEditor->PlayWorld;

	const float DeltaTime = W->DeltaTimeSeconds;
	const double InstantFPS = (DeltaTime > 0.f) ? (1.0 / DeltaTime) : 0.0;

	// Average FPS via engine smoothed stats.
	const double AvgFPS = static_cast<double>(GAverageFPS);
	const double AvgMS  = static_cast<double>(GAverageMS);

	const FPlatformMemoryStats MemStats = FPlatformMemory::GetStats();

	int32 ActorCount = 0;
	for (TActorIterator<AActor> It(W); It; ++It) { ++ActorCount; }

	return FMCPJsonBuilder()
		.Num(TEXT("delta_time_ms"), static_cast<double>(DeltaTime) * 1000.0)
		.Num(TEXT("instant_fps"),   InstantFPS)
		.Num(TEXT("avg_fps"),       AvgFPS)
		.Num(TEXT("avg_ms"),        AvgMS)
		.Object(TEXT("memory"), [&](FMCPJsonBuilder& M)
		{
			M.Num(TEXT("used_physical_mb"),       static_cast<double>(MemStats.UsedPhysical)      / (1024.0 * 1024.0))
			 .Num(TEXT("used_virtual_mb"),        static_cast<double>(MemStats.UsedVirtual)       / (1024.0 * 1024.0))
			 .Num(TEXT("available_physical_mb"),  static_cast<double>(MemStats.AvailablePhysical) / (1024.0 * 1024.0))
			 .Num(TEXT("peak_used_physical_mb"),  static_cast<double>(MemStats.PeakUsedPhysical)  / (1024.0 * 1024.0));
		})
		.Num(TEXT("actor_count"),   static_cast<double>(ActorCount))
		.Num(TEXT("time_seconds"),  W->GetTimeSeconds())
		.Num(TEXT("time_dilation"), static_cast<double>(UGameplayStatics::GetGlobalTimeDilation(W)))
		.Str(TEXT("world_path"),    W->GetPathName())
		.BuildSuccess(Request);
}

// ─── pie.dump_world_state — JSON snapshot of all PIE actors (compact form) ─────────────────
FMCPResponse Tool_DumpWorldState(const FMCPRequest& Request)
{
	check(IsInGameThread());
	if (!GEditor || !GEditor->PlayWorld)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorPIENotActive, kMCPMessagePIENotActive);
	}
	UWorld* W = GEditor->PlayWorld;

	FString ClassFilter;
	if (Request.Args.IsValid()) { Request.Args->TryGetStringField(TEXT("class_filter"), ClassFilter); }

	UClass* FilterClass = nullptr;
	if (!ClassFilter.IsEmpty())
	{
		FilterClass = LoadClass<UObject>(nullptr, *ClassFilter);
		if (!FilterClass)
		{
			const FString WithC = ClassFilter.EndsWith(TEXT("_C")) ? ClassFilter : (ClassFilter + TEXT("_C"));
			FilterClass = LoadClass<UObject>(nullptr, *WithC);
		}
		if (!FilterClass)
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorClassNotFound,
				FString::Printf(TEXT("could not resolve class_filter '%s'"), *ClassFilter));
		}
	}

	TArray<TSharedPtr<FJsonValue>> Actors;
	int32 Total = 0;
	for (TActorIterator<AActor> It(W); It; ++It)
	{
		AActor* A = *It;
		if (!A) { continue; }
		if (FilterClass && !A->GetClass()->IsChildOf(FilterClass)) { continue; }
		++Total;

		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("actor_path"), A->GetPathName());
		Obj->SetStringField(TEXT("class"),      A->GetClass()->GetPathName());
		Obj->SetStringField(TEXT("label"),      A->GetActorLabel());

		const FVector L = A->GetActorLocation();
		const FRotator R = A->GetActorRotation();
		const FVector S = A->GetActorScale3D();
		TSharedRef<FJsonObject> TfObj = MakeShared<FJsonObject>();
		TfObj->SetNumberField(TEXT("loc_x"), L.X); TfObj->SetNumberField(TEXT("loc_y"), L.Y); TfObj->SetNumberField(TEXT("loc_z"), L.Z);
		TfObj->SetNumberField(TEXT("rot_p"), R.Pitch); TfObj->SetNumberField(TEXT("rot_y"), R.Yaw); TfObj->SetNumberField(TEXT("rot_r"), R.Roll);
		TfObj->SetNumberField(TEXT("scl_x"), S.X); TfObj->SetNumberField(TEXT("scl_y"), S.Y); TfObj->SetNumberField(TEXT("scl_z"), S.Z);
		Obj->SetObjectField(TEXT("transform"), TfObj);

		Obj->SetBoolField(TEXT("hidden"),     A->IsHidden());
		Obj->SetBoolField(TEXT("pending_kill"), !IsValid(A));
		Actors.Add(MakeShared<FJsonValueObject>(Obj));
	}

	return FMCPJsonBuilder()
		.Str(TEXT("world_path"), W->GetPathName())
		.Num(TEXT("total"),      static_cast<double>(Total))
		.Arr(TEXT("actors"),     MoveTemp(Actors))
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

	// Wave A 2026-05: PIE functional testing surface.
	RegisterTool(TEXT("pie.simulate_key"),     &Tool_SimulateKey,     /*Lane A*/ false);
	RegisterTool(TEXT("pie.click_screen"),     &Tool_ClickScreen,     /*Lane A*/ false);
	RegisterTool(TEXT("pie.click_actor"),      &Tool_ClickActor,      /*Lane A*/ false);
	RegisterTool(TEXT("pie.set_time_dilation"),&Tool_SetTimeDilation, /*Lane A*/ false);
	RegisterTool(TEXT("pie.get_stats"),        &Tool_GetStats,        /*Lane A*/ false);
	RegisterTool(TEXT("pie.dump_world_state"), &Tool_DumpWorldState,  /*Lane A*/ false);

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
		TEXT("Phase 5 Chunk A + Wave A: registered 16 pie.* handlers (lifecycle + introspection + actor identity + input/stats/testing, all Lane A)"));
}

} // namespace FPIETools

#undef LOCTEXT_NAMESPACE

MCP_REGISTER_SURFACE(PIETools, &FPIETools::Register)
