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
#include "Tools/AnimBlueprintTools.h"
#include "Tools/AnimTools.h"
#include "Tools/AssetCompositeTools.h"
#include "Tools/AssetRegistryTools.h"
#include "Tools/AudioTools.h"
#include "Tools/BlueprintComponentTools.h"
#include "Tools/BlueprintCompositeTools.h"
#include "Tools/BlueprintGraphTools.h"
#include "Tools/BlueprintTools.h"
#include "Tools/ComponentTools.h"
#include "Tools/ConfigTools.h"
#include "Tools/ContentBrowserTools.h"
#include "Tools/CurveTools.h"
#include "Tools/DataTableTools.h"
#include "Tools/DebugTools.h"
#include "Tools/EditorTools.h"
#include "Tools/EngineTools.h"
#include "Tools/FolderTools.h"
#include "Tools/GameplayTagTools.h"
#include "Tools/HierarchyTools.h"
#include "Tools/InputTools.h"
#include "Tools/LevelCompositeTools.h"
#include "Tools/LevelStreamingTools.h"
#include "Tools/LevelTools.h"
#include "Tools/LiveCodingTools.h"
#include "Tools/LogTools.h"
#include "Tools/MaterialTools.h"
#include "Tools/MeshTools.h"
#include "Tools/NavMeshTools.h"
#include "Tools/NiagaraTools.h"
#include "Tools/PhysicsTools.h"
#include "Tools/PIETools.h"
#include "Tools/RenderTools.h"
#include "Tools/SequencerTools.h"
#include "Tools/SourceControlCompositeTools.h"
#include "Tools/SourceControlTools.h"
#include "Tools/StatsTools.h"
#include "Tools/SubsystemTools.h"
#include "Tools/TestCompositeTools.h"
#include "Tools/TestTools.h"
#include "Tools/TextureTools.h"
#include "Tools/TransformTools.h"
#include "Tools/UFunctionTools.h"
#include "Tools/UMGTools.h"
#include "Tools/ViewportTools.h"
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

	// Wave F Surface 3: Blueprint SCS component CRUD (4 tools, all Lane A). add/remove/
	// set_component_default are PIE-guarded mutators with FScopedTransaction; list is read-only and
	// PIE-safe. Kept in a separate file from BlueprintTools.cpp because the latter is already large
	// (2.8k lines) and the SCS surface is logically distinct (operates on SCS_Node templates
	// instead of UEdGraph / variable / function tables).
	FBlueprintComponentTools::Register(FMCPDispatchQueue::Get(), RegisteredMethodNames);

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

	// Wave D Surface 5 2026-05: Level streaming sub-level management surface (4 tools, all
	// Lane A).
	//   level_streaming.list         - enumerate ULevelStreaming entries of a persistent world
	//                                   (defaults to current editor world when no path supplied);
	//                                   per-entry: sublevel_path, level_class, package_name,
	//                                   is_loaded, is_visible, should_be_loaded, should_be_visible.
	//   level_streaming.add          - UEditorLevelUtils::AddLevelToWorld(World, PackageName,
	//                                   ULevelStreamingDynamic, FTransform). PIE-guarded.
	//                                   Refuses with -32014 PathInUse when the sublevel is already
	//                                   in the streaming list (deterministic no-op detection).
	//   level_streaming.remove       - UEditorLevelUtils::RemoveLevelFromWorld (or
	//                                   RemoveInvalidLevelFromWorld when ULevel isn't loaded).
	//                                   PIE-guarded.
	//   level_streaming.set_loaded   - mutate bShouldBeLoaded + bShouldBeVisible on a
	//                                   ULevelStreaming instance. PIE-safe (no guard); PIE-first/
	//                                   editor-fallback world resolution. FlushLevelStreaming
	//                                   only when NOT in PIE (runtime tick drains during PIE).
	// Reuses existing error codes — no new codes introduced. Distinct from Phase 3's
	// level.set_streaming_state (which is editor-world only + PIE-guarded) — both call the
	// same SetShouldBe* under the hood but differ in gate semantics.
	FLevelStreamingTools::Register(FMCPDispatchQueue::Get(), RegisteredMethodNames);

	// Wave D Surface 6 2026-05: Transform batch surface (3 tools, all Lane A, all PIE-guarded).
	//   transform.batch_set      - apply location/rotation/scale to N actors atomically;
	//                               relative=true adds to current, otherwise absolute set. At least
	//                               one of the 3 fields required.
	//   transform.snap_to_floor  - line-trace downward from each actor (configurable channel +
	//                               max distance); on hit → SetActorLocation(Hit.Location). Misses
	//                               leave actor untouched + report null hit_actor.
	//   transform.align          - align actors along an axis. mode='set' writes supplied value;
	//                               mode='min'/'max'/'average' compute from current values then
	//                               apply. Other 2 axes preserved per actor.
	// Each call wraps the WHOLE batch in ONE FScopedTransaction → Ctrl-Z reverts atomically.
	// Per-actor failures (unresolved path / no root component) go into failures[]; 0 resolved →
	// top-level -32004 (or -32602 for align min/max/average — can't aggregate empty set).
	// External-package actors (WorldPartition one-file-per-actor) dirtied via GetExternalPackage()
	// with GetOutermost() fallback. Reuses -32041 InvalidCollisionChannel from Phase 5 Chunk C.
	FTransformTools::Register(FMCPDispatchQueue::Get(), RegisteredMethodNames);

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
	//
	// Wave G Surface 1: physics.* runtime writes (4 tools, all Lane A, registered alongside the Phase
	// 5 traces inside ``FPhysicsTools::Register``). apply_impulse / set_simulation / set_velocity /
	// overlap_test wrap UPrimitiveComponent + UWorld::OverlapMultiByChannel. NO PIE guard, NO
	// FScopedTransaction, NO MarkPackageDirty — physics state is runtime (Chaos solver), not an
	// undoable asset edit. Reuses -32004 ObjectNotFound, -32011 WrongClass, -32602 InvalidParams,
	// -32603 Internal, and -32041 InvalidCollisionChannel — no new codes added in MCPTypes.h.
	FUMGTools::Register(FMCPDispatchQueue::Get(), RegisteredMethodNames);
	FNiagaraTools::Register(FMCPDispatchQueue::Get(), RegisteredMethodNames);
	FPhysicsTools::Register(FMCPDispatchQueue::Get(), RegisteredMethodNames);

	// Wave G Surface 4 2026-05: Anim Blueprint state machine surface (4 tools, all Lane A).
	//   animbp.list_state_machines  - walk UAnimBlueprint anim graphs for UAnimGraphNode_StateMachine
	//                                  instances; reports per-SM name + state_count + sm_node_guid.
	//                                  Read-only, no PIE guard.
	//   animbp.get_states           - enumerate UAnimStateNode inside a named state machine; reports
	//                                  per-state name, position, GUID, is_entry flag, and (when a
	//                                  SequencePlayer is present in the state's BoundGraph) the
	//                                  resolved anim_sequence_path. Read-only, no PIE guard.
	//   animbp.add_state            - NewObject<UAnimStateNode> + PostPlacedNewNode (which auto-
	//                                  creates BoundGraph + StateResult); rename BoundGraph to the
	//                                  user-supplied state_name. Optional anim_sequence_path path
	//                                  spawns a UAnimGraphNode_SequencePlayer inside the state's
	//                                  anim graph and wires its pose output to the StateResult.
	//                                  PIE-guarded, FScopedTransaction, MarkBlueprintAsStructurallyModified.
	//                                  Refuses duplicate state names with -32014 PathInUse.
	//   animbp.add_transition       - NewObject<UAnimStateTransitionNode> + PostPlacedNewNode (which
	//                                  auto-creates the boolean-rule UAnimationTransitionGraph) +
	//                                  CreateConnections to wire pins[0] to from_state output and
	//                                  pins[1] to to_state input. PIE-guarded, transacted, structural
	//                                  modify. Refuses from==to with -32602 (self-transitions need
	//                                  the engine's CreateSelfTransition path).
	// Reuses existing error codes - no new codes introduced: -32004 / -32010 / -32011 / -32014 /
	// -32027 / -32031 / -32602 / -32603. Build.cs adds ``AnimGraph`` private dep (editor module —
	// fine, the plugin is editor-only too).
	FAnimBlueprintTools::Register(FMCPDispatchQueue::Get(), RegisteredMethodNames);

	// Wave G Surface 5 2026-05: Render / show-flag / post-process surface (4 tools, all Lane A).
	//   render.list_show_flags                 - enumerate FEngineShowFlags entries on a viewport
	//                                             (name + enabled bool). No PIE guard. Iterates via
	//                                             FEngineShowFlags::IterateAllFlags with a sink that
	//                                             pre-biases custom flags by SF_FirstCustom.
	//   render.set_show_flag                   - SetSingleFlag on a viewport's EngineShowFlags +
	//                                             Invalidate; returns prior+new diff. No PIE guard.
	//   render.set_engine_stat                 - GEngine->SetEngineStat wrapper (stat fps / stat unit
	//                                             / etc.). PIE-first/editor-fallback world; no PIE
	//                                             guard.
	//   render.set_post_process_volume_property - mutate a single FPostProcessSettings field on a
	//                                             named APostProcessVolume actor + force the companion
	//                                             bOverride_<X>=true so the override actually applies.
	//                                             PIE-guarded, FScopedTransaction, MarkPackageDirty
	//                                             (external package preferred for WorldPartition OFPA).
	// Reuses existing error codes - no new codes introduced: -32004 / -32005 / -32011 / -32026 /
	// -32027 / -32602 / -32603. No new Build.cs deps (FEngineShowFlags + UPostProcessVolume in
	// Engine, FLevelEditorViewportClient in LevelEditor — both already linked).
	FRenderTools::Register(FMCPDispatchQueue::Get(), RegisteredMethodNames);

	// Wave G Surface 6 2026-05: Engine introspection + GC control surface (3 tools, all Lane A,
	// no PIE guard).
	//   engine.get_info             - FEngineVersion::Current() + FApp::Get{BuildConfiguration,
	//                                  BuildTargetType,ProjectName} + FPaths::{ProjectDir,EngineDir}
	//                                  + FPlatformProperties::PlatformName + GIsEditor +
	//                                  FApp::IsUnattended + IsRunningCommandlet + current world
	//                                  summary (name/type/is_pie, PIE-preferred). Read-only.
	//   engine.gc_collect           - CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, bPurge) when
	//                                  args.force=true (synchronous); GEngine->ForceGarbageCollection
	//                                  when false (deferred). Reports allocator delta + wall-clock
	//                                  duration via FPlatformMemory::GetStats + FPlatformTime.
	//   engine.get_memory_snapshot  - FPlatformMemory::GetStats + GMalloc->GetDescriptiveName +
	//                                  optional args.include_breakdown=true → platform-specific
	//                                  allocator stats from FPlatformMemoryStats::GetPlatformSpecificStats.
	// Reuses existing error codes - no new codes introduced: -32603 Internal (GEngine null).
	// No new Build.cs deps - all APIs live in Core/CoreUObject/Engine/UnrealEd already linked.
	FEngineTools::Register(FMCPDispatchQueue::Get(), RegisteredMethodNames);

	// Wave G Surface 3 2026-05: NavMesh query surface (4 tools, all Lane A).
	//   navmesh.list                 - enumerate ARecastNavMesh actors + key config fields
	//                                   (agent radius/height, cell size, tile size, init flag).
	//                                   Read-only, no PIE guard.
	//   navmesh.rebuild              - UNavigationSystemV1::Build() (system-wide) OR per-actor
	//                                   ARecastNavMesh::RebuildAll() when navmesh_actor_path is
	//                                   supplied. PIE-guarded (build is editor-time only).
	//                                   Returns wall-clock duration of build KICK-OFF (build is
	//                                   asynchronous; caller polls navmesh.list is_initialized).
	//   navmesh.find_path            - FindPathSync between two world points; returns waypoint
	//                                   list + cumulative path_length. Read-only, no PIE guard.
	//                                   When no navmesh built -> found=false with empty waypoints
	//                                   (NOT -32603; legitimate planner branch).
	//   navmesh.project_to_navmesh   - ProjectPointToNavigation within a box of +/-search_extent
	//                                   around location. Read-only, no PIE guard. When no
	//                                   navmesh -> projected=false with original location echoed.
	// Reuses existing error codes - no new codes introduced: -32004 / -32027 / -32602 / -32603.
	// Build.cs adds NavigationSystem private dep (runtime module).
	FNavMeshTools::Register(FMCPDispatchQueue::Get(), RegisteredMethodNames);

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

	// Wave E Surface 1 2026-05: Viewport surface (4 tools, all Lane A).
	//   viewport.list             - enumerate FLevelEditorViewportClient entries with index +
	//                                type + active-flag + per-viewport camera state.
	//   viewport.get_camera       - read one viewport's camera by index (default 0).
	//   viewport.set_camera       - teleport one viewport's camera; at least one of
	//                                location/rotation/fov required; returns prior+new diff.
	//   viewport.focus_on_actor   - FLevelEditorViewportClient::FocusViewportOnBox(Bounds,
	//                                bInstant=true) framing by actor_path; padding expands box.
	// Reuses existing error codes: -32004 (no viewports / actor missing for focus),
	// -32026 (viewport_index out of range), -32602 (no fields supplied to set_camera /
	// malformed vector|rotator). No PIE guard (editor viewport remains accessible during PIE);
	// no FScopedTransaction (viewport state isn't on undo stack); no MarkPackageDirty (no
	// asset state touched).
	FViewportTools::Register(FMCPDispatchQueue::Get(), RegisteredMethodNames);

	// Wave E Surface 3 2026-05: Actor hierarchy / attachment surface (3 tools, all Lane A).
	//   hierarchy.attach        - AActor::AttachToActor with optional socket + per-axis
	//                              FAttachmentTransformRules (keep_relative|keep_world|snap) +
	//                              weld_simulated_bodies. PIE-guarded, transacted, marks child
	//                              external package dirty.
	//   hierarchy.detach        - AActor::DetachFromActor with FDetachmentTransformRules
	//                              (keep_relative|keep_world; snap is rejected — no detach
	//                              analogue). PIE-guarded, transacted, marks child dirty.
	//   hierarchy.list_children - AActor::GetAttachedActors (recursive=true walks the whole
	//                              subtree). Read-only, no PIE guard. Per-child: path, label,
	//                              attach_socket (omitted when NAME_None).
	// Reuses existing error codes: -32004 (child or parent missing), -32027 (PIE active for
	// attach/detach), -32602 (bad rule string, child==parent, snap-as-detach-rule, root component
	// missing causing AttachToActor refusal).
	FHierarchyTools::Register(FMCPDispatchQueue::Get(), RegisteredMethodNames);

	// Wave E Surface 4 2026-05: UTexture2D inspection + edit surface (4 tools, all Lane A).
	//   texture.list                  - paginated UTexture2D enumeration (FARFilter + FMCPPageCursor,
	//                                    mirror of mesh.list).
	//   texture.get_info              - size [w,h] + pixel_format + compression + mip_count + srgb +
	//                                    lod_group + lod_bias + never_stream + address_x/y.
	//   texture.set_compression       - mutate UTexture2D::CompressionSettings + optional
	//                                    UpdateResource() flush. PIE-guarded, transacted.
	//   texture.generate_solid_color  - procedural solid-color texture via NewObject<UTexture2D> +
	//                                    Source.Init(W,H,1,1,TSF_BGRA8). PIE-guarded. PathInUse
	//                                    covers on-disk + in-memory.
	// Reads bypass PIE guard; mutators refuse PIE with -32027. No new error codes — reuses
	// -32004 / -32010 / -32011 / -32014 / -32027 / -32602 / -32603.
	FTextureTools::Register(FMCPDispatchQueue::Get(), RegisteredMethodNames);

	// Wave E Surface 5 2026-05: Enhanced Input introspection surface (4 tools, all Lane A,
	// all read-only - no PIE guard, no transactions, no MarkPackageDirty).
	//   input.list_mapping_contexts  - paginated UInputMappingContext asset enumeration
	//                                   (FARFilter + FMCPPageCursor; mirror of mesh.list shape).
	//   input.list_input_actions     - paginated UInputAction asset enumeration.
	//   input.get_context_bindings   - walks UIMC->GetMappings() and reports per-mapping
	//                                   action_path + key + modifier-class list + trigger-class
	//                                   list. Default-key-mappings only (profile overrides not
	//                                   enumerated - callers use marshall.read_property for those).
	//   input.list_player_contexts   - enumerates active mapping contexts on a player
	//                                   controller's UEnhancedInputLocalPlayerSubsystem. In UE 5.7
	//                                   UEnhancedPlayerInput::AppliedInputContextData is
	//                                   protected, so this probes HasMappingContext against every
	//                                   loaded UInputMappingContext asset. Returns ``hint`` field
	//                                   to disambiguate states. Skips unloaded IMC assets (the
	//                                   subsystem only holds strong refs to loaded IMCs).
	// Reuses existing error codes - no new codes introduced: -32004 / -32010 / -32011 / -32015 /
	// -32602. Reads PIE-safe (no guard). Build.cs adds ``EnhancedInput`` private dep (runtime
	// module from the EnhancedInput plugin, default-enabled in UE 5.7).
	FInputTools::Register(FMCPDispatchQueue::Get(), RegisteredMethodNames);

	// Wave E Surface 6 2026-05: Generic UE subsystem reflection surface (3 tools, all Lane A).
	//   subsystem.list           - enumerate instantiated subsystems across all 5 collections
	//                               (Engine/Editor/World/GameInstance/LocalPlayer). Optional
	//                               args.kind filters to one collection; "all" (default) returns
	//                               the union. Each entry: { class_path, kind, owner_context }.
	//   subsystem.get_property   - resolve subsystem instance by class path, read top-level
	//                               UPROPERTY via FMCPReflection. PIE-safe (no guard).
	//   subsystem.call_function  - resolve subsystem instance, invoke UFUNCTION by name with
	//                               marshalled args. NO PIE guard — caller responsibility.
	//                               Surfaces is_state_changing heuristic (!FUNC_Const &&
	//                               !FUNC_BlueprintPure) so caller can post-hoc detect mutation.
	//                               Reuses bp.call_function's allow_any safety gate.
	// Reuses existing error codes - no new codes introduced: -32004 / -32005 / -32006 / -32007 /
	// -32011 / -32020 / -32602. Reads PIE-safe (no guard).
	FSubsystemTools::Register(FMCPDispatchQueue::Get(), RegisteredMethodNames);

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

	// Wave H Surface 1 2026-05: UDataTable tabular game data surface (4 tools, all Lane A).
	//   data_table.list        - paginated UDataTable enumeration via IAssetRegistry::GetAssets,
	//                             path_prefix-filtered. Per-entry { asset_path, name,
	//                             row_struct_path, row_count }. Read-only, no PIE guard.
	//   data_table.get_rows    - enumerate rows with FProperty field marshalling via FMCPReflection.
	//                             Optional row_name_filter (substring); simple string-keyed
	//                             pagination on row name. Read-only, no PIE guard.
	//   data_table.set_row     - update existing row OR create when create_if_missing=true. Uses
	//                             UDataTable::AddRow(FName, uint8*, UScriptStruct*) for creation
	//                             (works with any UScriptStruct — does NOT require FTableRowBase
	//                             derivation). PIE-guarded, FScopedTransaction + HandleDataTableChanged
	//                             + MarkPackageDirty. Unknown field names silently skipped (counts
	//                             into fields_skipped for caller diagnostic).
	//   data_table.delete_row  - UDataTable::RemoveRow. PIE-guarded, transacted, marks dirty.
	//                             No-op response (deleted=false) when row already absent.
	// Reuses existing error codes - no new codes introduced: -32004 / -32010 / -32011 / -32015 /
	// -32027 / -32602 / -32603. No new Build.cs deps - UDataTable is in Engine (already linked).
	FDataTableTools::Register(FMCPDispatchQueue::Get(), RegisteredMethodNames);

	// Wave H Surface 2 2026-05: Curve inspection / mutation surface (4 tools, all Lane A).
	//   curve.list      - paginated UCurveBase + UCurveTable enumeration via IAssetRegistry::GetAssets.
	//                      Optional ``types`` filter narrows to {UCurveFloat, UCurveLinearColor,
	//                      UCurveVector, UCurveTable}. Per-entry { asset_path, curve_class }.
	//                      Standard FMCPPageCursor over ObjectPath, filter hash includes types[].
	//                      Read-only, no PIE guard.
	//   curve.get_data  - read keyframe data. UCurveFloat → single-channel keys[]; LinearColor →
	//                      4 channels (R/G/B/A) with channel field per key; Vector → 3 channels
	//                      (X/Y/Z); CurveTable + ``key`` (row name) → row's float curve keys.
	//                      Supports both RichCurves and SimpleCurves table modes. Read-only.
	//   curve.set_data  - replace entire key set (Reset + AddKey loop). Multi-channel curves
	//                      bucket keys by ``channel`` field before mutation; missing channel for
	//                      multi-channel curve raises -32602. UCurveTable requires ``key``
	//                      (row name); SimpleCurves mode rejected with -32602 for mutators.
	//                      PIE-guarded, FScopedTransaction, MarkPackageDirty.
	//   curve.add_key   - append or update-in-place single key (FRichCurve::UpdateOrAddKey,
	//                      UE_KINDA_SMALL_NUMBER time tolerance). Default interp_mode = "Cubic".
	//                      PIE-guarded, transacted, dirty. Returns { added, was_replaced,
	//                      new_key_count }.
	// Reuses existing error codes - no new codes introduced: -32004 / -32010 / -32011 / -32015 /
	// -32027 / -32602 / -32603. No new Build.cs deps - all curve headers in Engine (already linked).
	FCurveTools::Register(FMCPDispatchQueue::Get(), RegisteredMethodNames);

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
