// Copyright FatumGame. All Rights Reserved.

#include "UnrealMCPBridge.h"

#include "FMCPDay7Handlers.h"
#include "FMCPDispatchQueue.h"
#include "FMCPJobRegistry.h"
#include "FMCPLogStream.h"
#include "FMCPMarshalling.h"
#include "FMCPPythonBootstrap.h"
#include "FMCPPythonEval.h"
#include "FMCPServer.h"
#include "MCPTypes.h"
#include "Tools/ActorTools.h"
#include "Tools/AnimTools.h"
#include "Tools/AssetCompositeTools.h"
#include "Tools/AssetRegistryTools.h"
#include "Tools/AudioTools.h"
#include "Tools/BlueprintCompositeTools.h"
#include "Tools/BlueprintGraphTools.h"
#include "Tools/BlueprintTools.h"
#include "Tools/ComponentTools.h"
#include "Tools/ConfigTools.h"
#include "Tools/ContentBrowserTools.h"
#include "Tools/DebugTools.h"
#include "Tools/EditorTools.h"
#include "Tools/FolderTools.h"
#include "Tools/GameplayTagTools.h"
#include "Tools/LevelCompositeTools.h"
#include "Tools/LevelTools.h"
#include "Tools/LiveCodingTools.h"
#include "Tools/LogTools.h"
#include "Tools/MaterialTools.h"
#include "Tools/MeshTools.h"
#include "Tools/NiagaraTools.h"
#include "Tools/PhysicsTools.h"
#include "Tools/PIETools.h"
#include "Tools/SequencerTools.h"
#include "Tools/SourceControlCompositeTools.h"
#include "Tools/SourceControlTools.h"
#include "Tools/StatsTools.h"
#include "Tools/TestCompositeTools.h"
#include "Tools/TestTools.h"
#include "Tools/UFunctionTools.h"
#include "Tools/UMGTools.h"
#include "Tools/WorldPartitionTools.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/CoreDelegates.h"

DEFINE_LOG_CATEGORY(LogMCP);

void FUnrealMCPBridgeModule::StartupModule()
{
	UE_LOG(LogMCP, Log, TEXT("MCP bridge module starting (Phase 1 Day 7 — async jobs + log streaming + tools.list)"));

	// 1. Open the TCP listener. Failure here does not abort module load; we log + continue so the
	//    user can MCP.RestartListener after fixing the port conflict.
	FString StartErr;
	if (!FMCPServer::Get().Start(kMCPDefaultPort, StartErr))
	{
		UE_LOG(LogMCP, Warning,
			TEXT("MCP bridge listener failed to start: %s. Use 'MCP.RestartListener' after resolving the conflict."),
			*StartErr);
	}

	// 2. Attach the GLog ring-buffer capture BEFORE we log anything else from this point on, so
	//    the buffer reflects the full module-startup history. AddOutputDevice is idempotent
	//    (see FMCPLogStream::Attach).
	FMCPLogStream::Get().Attach();

	// 3. Register Python eval bridge handlers. These self-check IsPythonInitialized() and return
	//    structured -32603 errors if Python isn't up, so installing eagerly (before Python init)
	//    is safe — early requests just get a deterministic "python not initialised" surface.
	//
	//    Dispatch precedence is:
	//        (kind=ExecPython?) → ExecPythonHandler
	//        else (Method ∈ C++ handler map?) → C++ handler  (marshall.*, job.*, log.*, tools.list)
	//        else (UnknownMethodFallback installed?) → Python registry lookup
	//        else → -32601 method not found
	RegisterDefaultDispatchHandlers();

	// 4. Hook the OnEndFrame drain.
	OnEndFrameHandle = FCoreDelegates::OnEndFrame.AddRaw(this, &FUnrealMCPBridgeModule::OnEndFrame);

	// 5. Python sys.path bootstrap — fires immediately if Python is already up, else queues.
	FMCPPythonBootstrap::RegisterPythonInitCallback();

	bStarted = true;
	UE_LOG(LogMCP, Log, TEXT("MCP bridge module ready (listener=%s port=%d, log_stream=%s)"),
		FMCPServer::Get().IsListening() ? TEXT("RUNNING") : TEXT("STOPPED"),
		FMCPServer::Get().GetListenPort(),
		FMCPLogStream::Get().GetEntryCount() > 0 ? TEXT("ATTACHED") : TEXT("ATTACHED_EMPTY"));
}

void FUnrealMCPBridgeModule::ShutdownModule()
{
	UE_LOG(LogMCP, Log, TEXT("MCP bridge module shutting down"));

	if (OnEndFrameHandle.IsValid())
	{
		FCoreDelegates::OnEndFrame.Remove(OnEndFrameHandle);
		OnEndFrameHandle.Reset();
	}

	UnregisterDefaultDispatchHandlers();

	// Stop accepting new socket traffic FIRST so no fresh job.submit calls land while we're tearing
	// the registry down.
	FMCPServer::Get().Stop();

	// Cancel any in-flight async jobs + destroy the worker pool. Bodies that don't honour cancel
	// will still run to completion — pool->Destroy() blocks. That's intentional (forced abort of
	// in-progress Python could corrupt the interpreter).
	FMCPJobRegistry::Get().Shutdown();

	// Detach log capture LAST so any teardown logs from Job registry / server get into the ring
	// for the final flush (cosmetic — by now nobody can query log.tail anyway).
	FMCPLogStream::Get().Detach();

	bStarted = false;
}

bool FUnrealMCPBridgeModule::IsListening() const
{
	return FMCPServer::Get().IsListening();
}

int32 FUnrealMCPBridgeModule::GetListenPort() const
{
	return FMCPServer::Get().GetListenPort();
}

int64 FUnrealMCPBridgeModule::GetDispatchedRequestCount() const
{
	return FMCPDispatchQueue::Get().GetDispatchedCount();
}

void FUnrealMCPBridgeModule::OnEndFrame()
{
	// Pumps both: drains the dispatch queue (handlers run + responses sent), then GCs closed sockets.
	FMCPDispatchQueue::Get().Drain();
	FMCPServer::Get().GarbageCollectClosedConnections();
}

void FUnrealMCPBridgeModule::RegisterDefaultDispatchHandlers()
{
	// kind=ExecPython → arbitrary Python expression evaluator.
	// The wrapper reads Request.Args->"expression"; missing/empty produces -32602.
	FMCPDispatchQueue::Get().SetExecPythonHandler(
		[](const FMCPRequest& Req) -> FMCPResponse
		{
			FString Expression;
			if (Req.Args.IsValid())
			{
				Req.Args->TryGetStringField(TEXT("expression"), Expression);
			}
			if (Expression.IsEmpty())
			{
				FMCPResponse Err;
				Err.RequestId = Req.RequestId;
				Err.OriginalIdString = Req.OriginalIdString;
				Err.bIsError = true;
				Err.ErrorCode = -32602;
				Err.ErrorMessage = TEXT("exec_python requires args.expression (string)");
				return Err;
			}
			return FMCPPythonEval::EvalExpression(Req, Expression);
		});

	// Unknown-method fallback → Python registry. Once installed, ANY unmatched CallFunction
	// request gets routed to MCPTools.registry.get_tool(method). The handler itself emits a
	// JSON-RPC -32601 if the tool isn't registered Python-side either.
	FMCPDispatchQueue::Get().SetUnknownMethodFallback(
		[](const FMCPRequest& Req) -> FMCPResponse
		{
			return FMCPPythonEval::CallPythonTool(Req);
		});

	// Day 4-5: Tier 2 marshalling handlers. Registered as C++ handlers (not Python tools) for
	// two reasons: (a) FProperty reflection isn't exposed to Python, and (b) C++ dispatch is
	// ~2-3 ms faster per call (no wrapper script + base64 round-trip). These names take
	// precedence over the Python fallback for ``marshall.*`` — see dispatch order in
	// FMCPDispatchQueue::Drain. FHandler is a TFunction so we can pass the static method pointer
	// directly — TFunction's converting ctor wraps it.
	auto RegisterHandler = [this](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe = false)
	{
		FMCPDispatchQueue::Get().RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		RegisteredMethodNames.Add(MethodName);
	};
	RegisterHandler(TEXT("marshall.list_properties"), &FMCPMarshalling::ListProperties);
	RegisterHandler(TEXT("marshall.read_property"),   &FMCPMarshalling::ReadProperty);
	RegisterHandler(TEXT("marshall.write_property"),  &FMCPMarshalling::WriteProperty);
	RegisterHandler(TEXT("marshall.describe_struct"), &FMCPMarshalling::DescribeStruct);

	// Day 7: async job system (5 handlers). Bodies dispatch to a separate worker pool — the
	// handlers themselves are synchronous and bounded.
	// HOTFIX 2 (2026-05): job.status + job.result registered Lane B so Python composites that
	// submit jobs from inside the game thread (via dispatch_internal TCP loopback) can poll
	// without deadlocking the game thread on socket.recv. Both handlers only read FMCPJobRegistry
	// state under its internal lock — see the Lane B promotion notes in FMCPDay7Handlers.cpp.
	// job.submit / job.cancel / job.list_active remain Lane A: submit creates a job (writes the
	// jobs map under the same lock — safe in isolation, but conservatively Lane A because the
	// SubmitJob path also calls EnsureInitialized which spins up the worker pool the first time
	// and that pool init should run on the game thread for predictability). list_active is rarely
	// hot-pathed by composites.
	RegisterHandler(TEXT("job.submit"),      &FMCPDay7Handlers::JobSubmit);
	RegisterHandler(TEXT("job.status"),      &FMCPDay7Handlers::JobStatus,      /*bThreadSafe*/ true);
	RegisterHandler(TEXT("job.result"),      &FMCPDay7Handlers::JobResult,      /*bThreadSafe*/ true);
	RegisterHandler(TEXT("job.cancel"),      &FMCPDay7Handlers::JobCancel);
	RegisterHandler(TEXT("job.list_active"), &FMCPDay7Handlers::JobListActive);

	// Day 7: log streaming (3 handlers). FMCPLogStream is attached to GLog at module startup so
	// every log line since then is queryable via these handlers.
	RegisterHandler(TEXT("log.tail"),      &FMCPDay7Handlers::LogTail);
	RegisterHandler(TEXT("log.subscribe"), &FMCPDay7Handlers::LogSubscribe);
	RegisterHandler(TEXT("log.search"),    &FMCPDay7Handlers::LogSearch);

	// Day 7: first-class tools.list verb. Replaces mcp_server's exec_python ferry — returns
	// {python_tools: {meta...}, cpp_handlers: [names]} in a single round-trip.
	RegisterHandler(TEXT("tools.list"), &FMCPDay7Handlers::ToolsList);

	// Phase 2: Assets + Content Browser surface. Each Register function appends to
	// RegisteredMethodNames so ShutdownModule can mirror-cleanup.
	FAssetRegistryTools::Register(FMCPDispatchQueue::Get(), RegisteredMethodNames);
	FContentBrowserTools::Register(FMCPDispatchQueue::Get(), RegisteredMethodNames);
	FAssetCompositeTools::Register(FMCPDispatchQueue::Get(), RegisteredMethodNames);

	// Phase 3 Days 1-3: Level operations surface (12 user-visible tools + 1 Lane B sanity probe).
	// MUST be after Phase 2 registers so the registration log line below counts everything that
	// landed in the dispatch table this StartupModule pass.
	FLevelTools::Register(FMCPDispatchQueue::Get(), RegisteredMethodNames);

	// Phase 3 Days 4-8: Actor operations surface (20 user-visible tools, all Lane A).
	FActorTools::Register(FMCPDispatchQueue::Get(), RegisteredMethodNames);

	// Phase 3 Days 9-10: Component operations surface (8 user-visible tools, all Lane A).
	FComponentTools::Register(FMCPDispatchQueue::Get(), RegisteredMethodNames);

	// Phase 3 Days 11-14: 5 internal Lane-B composite handlers backing the 5 Python composites in
	// MCPTools/tools/level_composites.py (level.full_actor_dump / find_actors_with_class +
	// actor.batch_spawn / batch_destroy / batch_set_property). Same async-only pattern as the
	// Phase 2 asset.* composites — sync handler is Lane B, body runs via FMCPJobRegistry, AI
	// client polls job.status / job.result externally.
	FLevelCompositeTools::Register(FMCPDispatchQueue::Get(), RegisteredMethodNames);

	// Phase 4 Days 1-10: Blueprint reads (6 tools) + writes (6 tools) + bp.compile sync. All
	// 13 user-visible tools are Lane A. Reads bypass PIE guard (assets are PIE-safe); writes +
	// compile refuse PIE with -32027 + frozen kMCPMessagePIEActive text. bp.reparent (write 6)
	// is experimental + requires args.confirm_dangerous=true. Days 11-15 add material.* surface.
	FBlueprintTools::Register(FMCPDispatchQueue::Get(), RegisteredMethodNames);

	// Phase 4 Day 10: 1 internal Lane-B composite handler backing the single Python composite
	// bp.compile_all_dirty in blueprint_composites.py. Walks AR by scope_paths, loads each BP,
	// compiles each, aggregates {compiled, succeeded, failed[{path, errors[]}], duration_ms}.
	// Cooperative cancel every 16 BPs (heavier than Phase 3 default 256 because compile is heavy).
	FBlueprintCompositeTools::Register(FMCPDispatchQueue::Get(), RegisteredMethodNames);

	// Phase 4 Days 11-15: Material surface (9 tools, all Lane A). 2 reads + 4 MIC writes +
	// 1 create + 2 diagnostic. Reads PIE-safe; writes refuse PIE with -32027. MIC-only writes
	// enforced — base UMaterial mutations would need graph edits (out of Phase 4 scope).
	FMaterialTools::Register(FMCPDispatchQueue::Get(), RegisteredMethodNames);

	// UFunction reflection surface (2 tools, Lane A): bp.call_function + bp.list_class_functions.
	// Generic invoker for any BlueprintCallable/BlueprintPure/Exec UFUNCTION (static or instance),
	// plus discovery tool. Covers UFlecsContainerLibrary + any future crafting/inventory
	// BP function libraries without per-system bespoke tools.
	FUFunctionTools::Register(FMCPDispatchQueue::Get(), RegisteredMethodNames);

	// Wave A 2026-05: stats surface (engine + memory snapshots, Lane A, editor-context safe).
	FStatsTools::Register(FMCPDispatchQueue::Get(), RegisteredMethodNames);

	// Wave B Tier 4 2026-05: Blueprint graph-node construction surface (2 tools, Lane A).
	//   bp.add_node      — instantiate K2Node subclass on a UEdGraph, configure variable/function
	//                      reference, AddNode + AllocateDefaultPins + ReconstructNode.
	//   bp.connect_pins  — UEdGraphSchema_K2::CanCreateConnection (pre-check) + TryCreateConnection,
	//                      reports broke_existing_count.
	// Both PIE-guarded. New error codes -32050 GraphNotFound, -32051 NodeNotFound, -32052
	// PinNotFound, -32053 PinConnectionRefused (see MCPTypes.h).
	FBlueprintGraphTools::Register(FMCPDispatchQueue::Get(), RegisteredMethodNames);

	// Wave C Tier 5b 2026-05: Animation surface (5 tools, all Lane A).
	//   anim.list_sequences   — paginated UAnimSequence enumeration
	//   anim.create_montage   — UAnimMontage from a source sequence (no editor pop-up — replicates
	//                            UAnimMontageFactory::FactoryCreateNew inline)
	//   anim.add_section      — append FCompositeSection to a montage
	//   anim.add_notify       — append FAnimNotifyEvent on a montage notify track
	//   anim.set_blend_mode   — set BlendIn / BlendOut alpha-blend times
	// New error codes -32054 SkeletonMismatch, -32055 NotifyTrackNotFound (see MCPTypes.h).
	FAnimTools::Register(FMCPDispatchQueue::Get(), RegisteredMethodNames);

	// Wave C Tier 5c 2026-05: Audio surface (3 tools, all Lane A).
	//   audio.create_sound_cue  — USoundCue with optional initial USoundWave (no editor pop-up)
	//   audio.set_attenuation   — set/clear AttenuationSettings on any USoundBase
	//   audio.list_mix_classes  — enumerate USoundClass + USoundMix assets
	FAudioTools::Register(FMCPDispatchQueue::Get(), RegisteredMethodNames);

	// Wave C Tier 5c 2026-05: WorldPartition surface (3 tools, all Lane A).
	//   wp.is_partitioned          — UWorld -> GetWorldPartition()
	//   wp.get_actor_runtime_grid  — AActor::GetRuntimeGrid
	//   wp.set_actor_runtime_grid  — AActor::SetRuntimeGrid (PIE-guarded)
	// Scope-reduced from original spec (wp.list_cells / wp.load_cell / wp.set_runtime_grid) —
	// per-actor runtime-grid edits are the most-needed write op and don't need streaming runtime.
	FWorldPartitionTools::Register(FMCPDispatchQueue::Get(), RegisteredMethodNames);

	// Wave D Surface 1 2026-05: GameplayTag surface (4 tools, all Lane A).
	//   gameplaytag.list                  — UGameplayTagsManager registered tag enumeration
	//   gameplaytag.query_actor           — IGameplayTagAssetInterface OR property-reflection
	//   gameplaytag.add_to_container      — append tag to named FGameplayTagContainer UPROPERTY
	//   gameplaytag.remove_from_container — remove tag from same
	FGameplayTagTools::Register(FMCPDispatchQueue::Get(), RegisteredMethodNames);

	// Wave D Surface 2 2026-05: Debug visual overlay surface (6 tools, all Lane A).
	//   debug.draw_line / draw_sphere / draw_box / draw_arrow / draw_text  — DrawDebugHelpers wrappers
	//   debug.clear                                                         — FlushPersistentDebugLines + FlushDebugStrings
	// NOT PIE-guarded: drawing works in both editor world and PIE world; tool picks the
	// appropriate world via PIE-first / editor-fallback resolution (matches PhysicsTools'
	// trace-world convention). No transactions, no package dirty marking — overlays are
	// transient line-batcher state, not asset data.
	FDebugTools::Register(FMCPDispatchQueue::Get(), RegisteredMethodNames);

	// Wave D Surface 3 2026-05: UStaticMesh inspection + edit surface (5 tools, all Lane A).
	//   mesh.list               — paginated UStaticMesh enumeration (FARFilter + FMCPPageCursor)
	//   mesh.get_info           — bounds + LOD count + material slots + LOD0 vertex/triangle counts
	//   mesh.list_lods          — per-LOD vertex/triangle counts + screen-size thresholds
	//   mesh.set_material_slot  — overwrite material at a slot (PIE-guarded, transacted)
	//   mesh.duplicate          — manual DuplicateObject (no editor pop-up; PIE-guarded)
	// Reads bypass PIE guard; mutators refuse PIE with -32027. No new error codes — reuses
	// -32004 / -32010 / -32011 / -32014 / -32026 / -32027 / -32602 / -32603.
	FMeshTools::Register(FMCPDispatchQueue::Get(), RegisteredMethodNames);

	// Wave D Surface 4 2026-05: Actor outliner folder surface (4 tools, all Lane A).
	//   folder.list      - enumerate folder paths (FActorFolders::ForEachFolder) + per-folder
	//                       child_count (subfolders that begin with "<this>/")
	//   folder.create    - FActorFolders::Get().CreateFolder (PIE-guarded, transacted)
	//   folder.delete    - FActorFolders::Get().DeleteFolder + optional re-parent of children
	//                       (walks FActorIterator, rewrites Actor->SetFolderPath). PIE-guarded.
	//   folder.set_actor - AActor::SetFolderPath (PIE-guarded, transacted; empty = root)
	// Reads bypass PIE guard; mutators refuse PIE with -32027. New error code -32056
	// FolderNotFound for folder.delete on a non-existent path.
	FFolderTools::Register(FMCPDispatchQueue::Get(), RegisteredMethodNames);

	// Phase 5 Chunk A: PIE surface (10 tools, all Lane A). Inverse PIE-guard: every pie.* tool
	// except pie.start and pie.is_running requires PIE to BE running; refuses with -32038
	// PIENotActive + frozen kMCPMessagePIENotActive text otherwise. Lifecycle (start/stop/pause/
	// resume/step_frame) + introspection (is_running/console_exec) + actor identity
	// (get_player_controller/get_pawn/focus_actor).
	FPIETools::Register(FMCPDispatchQueue::Get(), RegisteredMethodNames);

	// Phase 5 Chunk B: Editor utilities surface (10 tools, all Lane A). Viewport screenshots
	// (in-memory + disk for editor + PIE) + camera get/set + world-outliner selection get/set +
	// toast notification + current-world identity + force-one-tick. The one PIE-required tool
	// (pie.screenshot_to_disk) lives in the pie.* namespace for surface coherence even though
	// it's registered alongside the editor.* tools in this chunk. Path-based outputs go through
	// FMCPPathSandbox::Resolve → -32013 PathEscape on whitelist miss; set_selection caps at 200
	// (-32017 InputTooLarge) mirroring Phase 2 batch_metadata.
	FEditorTools::Register(FMCPDispatchQueue::Get(), RegisteredMethodNames);

	// Phase 5 Chunk C: UMG + Niagara + Physics traces (5 tools, all Lane A, all read-only).
	//   - UMG (2)     : umg.list_widgets + umg.get_widget_property (WidgetTree walk + Tier-1 property read)
	//   - Niagara (1) : niagara.list_parameters (user / system / per-emitter parameter enumeration)
	//   - Physics (2) : physics.line_trace + physics.sweep_capsule (UE Chaos system; NOT Jolt/Barrage)
	// No PIE guard — all 5 are reads. Physics traces operate on the PIE world when PIE is running,
	// editor world otherwise (selectable transparently via world resolution helpers). New error codes
	// landed in MCPTypes.h: -32039 WidgetNotFound, -32041 InvalidCollisionChannel; -32040 reserved for
	// future niagara.set_user_param.
	FUMGTools::Register(FMCPDispatchQueue::Get(), RegisteredMethodNames);
	FNiagaraTools::Register(FMCPDispatchQueue::Get(), RegisteredMethodNames);
	FPhysicsTools::Register(FMCPDispatchQueue::Get(), RegisteredMethodNames);

	// Phase 5 Chunk D: Sequencer read-only surface (5 tools, all Lane A). list_cinematics enumerates
	// ULevelSequence assets via FARFilter; get_tracks/get_camera_cuts walk the MovieScene's master
	// tracks + per-binding tracks; get_keyframes resolves dotted track_path to a UMovieSceneSection
	// and decodes float/double/bool/integer channel values (other types emit sentinel value=null);
	// get_current_time reads the active LevelSequence editor playhead via
	// ULevelSequenceEditorBlueprintLibrary::GetGlobalPosition. New error codes landed in MCPTypes.h:
	// -32042 NoActiveSequencer, -32043 TrackNotFound, -32044 SectionIndexOOB.
	FSequencerTools::Register(FMCPDispatchQueue::Get(), RegisteredMethodNames);

	// Phase 6 Chunk A: Source Control surface (6 tools = 5 sync sc.* + 1 async composite
	// sc.submit). All sync tools are Lane A (sc.status/checkout/diff/diff_binary/revert);
	// the composite's internal submitter sc._submit_internal is Lane B (queues a job
	// with bGameThreadRequired=true; the Python wrapper sc.submit returns {job_id} and AI
	// polls externally). sc.revert requires confirm_destructive=true → -32033 ReparentUnsafe
	// (reused destructive-confirm gate) when missing. New error code: -32045
	// SourceControlProviderUnavailable, raised when ISourceControlModule::Get().IsEnabled()
	// returns false or GetProvider().IsAvailable() returns false. Provider is resolved
	// per-call (Git LFS, Perforce, Subversion — anything UE Editor's Source Control panel
	// has configured). Binary diff payloads capped at 32 MiB/side → -32017 InputTooLarge.
	FSourceControlTools::Register(FMCPDispatchQueue::Get(), RegisteredMethodNames);
	FSourceControlCompositeTools::Register(FMCPDispatchQueue::Get(), RegisteredMethodNames);

	// Phase 6 Chunk B: Automation Test surface (8 tools = 7 sync test.* + 1 async composite
	// test.run_automation). All sync tools are Lane A (list_automation_specs / run_single_test /
	// get_last_results / cancel_current / list_categories / get_test_info / set_filter_flags);
	// the composite's internal submitter test._run_automation_internal is Lane B (queues a job
	// with bGameThreadRequired=true; Python wrapper test.run_automation returns {job_id} and AI
	// polls externally). New error code: -32046 TestNotFound, raised when caller's test_name
	// (FullTestPath) doesn't exist in the FAutomationTestFramework registry. test.run_single_test
	// has a sync wall-clock cap (kTSTSingleTestMaxSeconds = 30s) — longer-running tests must go
	// through the async test.run_automation path. test.list_automation_specs paginates via
	// FMCPPageCursor keyed on FullTestPath + filter hash. No additional Build.cs dep needed —
	// FAutomationTestFramework lives in Core which the bridge already depends on transitively.
	FTestTools::Register(FMCPDispatchQueue::Get(), RegisteredMethodNames);
	FTestCompositeTools::Register(FMCPDispatchQueue::Get(), RegisteredMethodNames);

	// Phase 6 Chunk C: Config / CVars surface (6 sync cfg.* tools, all Lane A).
	//   - cfg.get_cvar / cfg.set_cvar / cfg.list_cvars : IConsoleManager wrappers with typed
	//     marshalling (D4 — accepts JSON bool/int/float/string; coerces per cvar's reported type),
	//     command-vs-variable gate (D5 → -32011 WrongClass for IConsoleCommand entries), read-only
	//     gate (D9 → -32047 CVarReadOnly when ECVF_ReadOnly is set). Writes use ECVF_SetByCode so
	//     subsequent Console-priority writes from the operator's editor console still win.
	//   - cfg.read / cfg.write / cfg.list_sections : GConfig wrappers sandboxed (D8) to four ini
	//     BASE names {DefaultEngine, DefaultGame, DefaultInput, DefaultEditor}. Anything else →
	//     -32013 PathEscape. cfg.write flushes immediately to <ProjectDir>/Config/Default*.ini and
	//     verifies via post-write GetString that the cache holds the new value.
	//   - Pagination via FMCPPageCursor on cfg.list_cvars + cfg.list_sections; default page_size
	//     100, clamped [1, 1000]. Filter mutation across pages → -32015 StaleCursor.
	// New error code: -32047 CVarReadOnly. No new Build.cs deps (IConsoleManager + GConfig + FPaths
	// all in Core, already a transitive dep).
	FConfigTools::Register(FMCPDispatchQueue::Get(), RegisteredMethodNames);

	// Phase 6 Chunk D: Log surface additions (3 sync log.* tools, all Lane A).
	//   - log.set_category_verbosity : routes through FSelfRegisteringExec::StaticExec with the
	//     canonical ``Log <Category> <Verbosity>`` command. Forward-references accepted (UE's own
	//     Log Exec stores the value in BootAssociations for unknown categories). Process-lifetime
	//     scope only (D6) — to persist verbosity changes across editor restarts use cfg.write
	//     against DefaultEngine.ini [Core.Log].
	//   - log.list_categories : paginated enumeration of categories observed in FMCPLogStream
	//     since attach. Observed-set caveat — quiet categories (never logged anything) are
	//     invisible (the public FLogSuppressionInterface doesn't expose name→FLogCategoryBase*
	//     lookup). FMCPPageCursor pagination, default 100, clamped [1, 1000].
	//   - log.clear : empties the in-memory ring buffer that backs log.tail / log.search. Does
	//     NOT clear on-disk file logs.
	// Combined with the 3 Phase 1 log.* tools (log.tail / log.search / log.subscribe) this brings
	// the log.* surface to 6 user-visible tools total. New error code: -32049 LogCategoryUnknown.
	FLogTools::Register(FMCPDispatchQueue::Get(), RegisteredMethodNames);

	// Phase 6 Chunk E: Live Coding surface (1 async composite — livecoding.recompile, Python
	// wrapper in phase6_composites.py). Backing internal handler livecoding._recompile_internal
	// is Lane B (queues a GT-required job; AI polls externally via job.status / job.result).
	// PIE-guarded (D11 — LC requires PIE off; -32027 PIEActive on submit). Platform-gated to
	// Windows desktop editor builds via PLATFORM_WINDOWS macros; non-Windows → -32048
	// LiveCodingDisabled. Module availability gated via ILiveCodingModule::HasStarted() +
	// CanEnableForSession(). Compile result enum values exposed verbatim in the response. Best-
	// effort patched_modules / failed_modules extraction from LogLiveCoding entries. New error
	// code: -32048 LiveCodingDisabled. Build.cs adds LiveCoding private dep (Win64-only).
	FLiveCodingTools::Register(FMCPDispatchQueue::Get(), RegisteredMethodNames);

	UE_LOG(LogMCP, Log,
		TEXT("Registered dispatch handlers: kind=ExecPython → FMCPPythonEval::EvalExpression, ")
		TEXT("unknown-method-fallback → FMCPPythonEval::CallPythonTool, ")
		TEXT("C++ handlers → marshall.* (4) + job.* (5) + log.* (3) + tools.list + asset.* (13) + cb.* (12) + asset._internal (5) + level.* (12) + actor.* (20) + component.* (8) + level._internal/actor._internal (5) + bp.* (13) + bp._internal (1) + material.* (9) + pie.* (10) + editor.* (9) + pie.screenshot_to_disk + umg.* (2) + niagara.* (1) + physics.* (2) + sequencer.* (5) + sc.* (5) + sc._internal (1) + test.* (7) + test._internal (1) + cfg.* (6) + log.* (3 NEW Phase 6) + livecoding._internal (1) + _phase3_lane_b_sanity (1) — ALL 6 PHASES COMPLETE: 169 user-visible tools cumulative"));
}

void FUnrealMCPBridgeModule::UnregisterDefaultDispatchHandlers()
{
	// Drop the Python eval handlers + any explicit C++ method handlers we accumulated.
	FMCPDispatchQueue::Get().SetExecPythonHandler({});
	FMCPDispatchQueue::Get().SetUnknownMethodFallback({});

	for (const FString& Method : RegisteredMethodNames)
	{
		FMCPDispatchQueue::Get().UnregisterHandler(Method);
	}
	RegisteredMethodNames.Reset();
}

IMPLEMENT_MODULE(FUnrealMCPBridgeModule, UnrealMCPBridge)
