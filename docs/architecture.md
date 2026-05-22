---
title: Architecture
layout: default
nav_order: 3
---

# Architecture
{: .no_toc }

1. TOC
{:toc}

## High-level flow

```
External client
      ↓
TCP socket :30020 (loopback)
      ↓ newline-delimited JSON
FMCPServer (FRunnable, accept on background thread)
      ↓
FMCPConnection (one per client; line-buffered receive)
      ↓
FMCPDispatchQueue (MPSC queue)
      ↓
   ┌──┴───────────────┐
Lane A              Lane B
(OnEndFrame         (worker pool,
 drain, game        bThreadSafe=true)
 thread)
   │                  │
   └──┬───────────────┘
      ↓
FMCPResponse → connection->Send → newline JSON
```

## Lane A vs Lane B

Every tool handler is registered with a `bThreadSafe` boolean:

```cpp
Queue.RegisterHandler(TEXT("debug.draw_line"), &DebugTools::DrawLine, /*bThreadSafe*/ false);
Queue.RegisterHandler(TEXT("marshall.list_properties"), &Marshalling::List, /*bThreadSafe*/ true);
```

### Lane A — game thread

`bThreadSafe = false`. Handler runs inside `FCoreDelegates::OnEndFrame` drain on the game thread. Use when:

- **Any UObject access** — UClass introspection, FProperty read/write, GC-tracked memory
- **Editor state** — GEditor, GEngine, LevelEditor subsystems, viewport, Slate
- **Asset registry** — IAssetRegistry::GetAssets asserts off-GT in UE 5.7
- **Anything calling LoadObject** — package loader + GC visited set is GT-only

Most tools are Lane A.

### Lane B — worker pool

`bThreadSafe = true`. Handler dispatched to `FMCPJobRegistry`'s worker pool. Use when:

- Pure compute (path parsing, string ops, base64 encode/decode)
- No UObject access at all
- Result construction from pre-captured data

Lane B handlers are rare but important for performance — they keep the game thread free for editor UI when the bridge is hammered.

## OnEndFrame drain

The bridge hooks `FCoreDelegates::OnEndFrame` in `FUnrealMCPBridgeModule::StartupModule`:

```cpp
OnEndFrameHandle = FCoreDelegates::OnEndFrame.AddRaw(
    this, &FUnrealMCPBridgeModule::OnEndFrame);
```

`OnEndFrame()` then:
1. `FMCPDispatchQueue::Get().Drain()` — invokes pending Lane A handlers
2. `FMCPServer::Get().GarbageCollectClosedConnections()` — reaps closed sockets

This means **Lane A handlers run at most once per editor tick** — typically 60-144 Hz. Throughput on Lane A is bounded by editor frame rate.

## Protocol: newline-delimited JSON

Every line is one full JSON object. The protocol is **fully synchronous** per-connection:

1. Client sends request line.
2. Server parses, dispatches.
3. When ready (after OnEndFrame drain for Lane A; immediately for Lane B), server sends response line.
4. Client reads until `\n`.

A single connection processes requests sequentially. **Multiple connections** can interleave but the dispatch queue is FIFO per Lane.

Max line size: `kMCPFrameMaxBytes = 64 MiB` (raised from 8 MiB to accommodate screenshot/thumbnail payloads). Any single line larger than this aborts the connection (DoS guard).

## Async jobs (`job.*`)

For long-running operations (cb.bulk_import, asset.batch_metadata_async, etc.), the bridge offers a job-system pattern:

```
client → job.submit { method, args }
       → returns { job_id }
client → job.poll { job_id }
       → returns { status: "running"|"complete"|"failed", progress?, result?, error? }
client → job.cancel { job_id }
       → cooperative cancel; bodies that don't honour cancel still complete
```

Jobs run on `FMCPJobRegistry`'s worker pool (Lane B equivalent). The handler bodies cooperate with `Job->IsCancelled()` checks at safe points.

## Python eval bridge (`kind=ExecPython`)

In addition to `kind=call_function`, the bridge accepts `kind=ExecPython` requests that run arbitrary Python expressions in the editor's bundled interpreter:

```json
{ "id":"1", "kind":"ExecPython", "args":{"expression":"unreal.SystemLibrary.get_engine_version()"} }
```

This is the **fallback path** — used by `MCPTools.registry`-defined tools that have no direct C++ handler. C++ handlers always take precedence; unknown methods route through Python.

## Dispatch precedence

```text
1. kind=ExecPython?              → FMCPPythonEval::EvalExpression
2. Method in C++ handler map?    → C++ handler  (marshall.*, job.*, log.*, tools.list, all bp.*, etc.)
3. UnknownMethodFallback set?    → Python registry lookup
4. Otherwise                     → -32601 method not found
```

## Editor + PIE world resolution

Several tools (debug.*, niagara.* runtime, sequencer.*, transform.*) work in both editor and PIE worlds. The canonical pattern:

```cpp
UWorld* World = (GEditor && GEditor->PlayWorld)
    ? GEditor->PlayWorld
    : GEditor->GetEditorWorldContext().World();
```

Response shape includes `"world": "editor"|"pie"` so the caller knows which world the tool acted on.

**PIE-guarded mutators**: tools that modify the underlying ASSET (not just a per-world instance) refuse during PIE with `-32027 PIEActive`. Examples: `bp.add_node`, `mesh.set_material_slot`, `audio.set_attenuation`. Otherwise PIE-cloned worlds + the shared asset pointer would corrupt the source asset.

## FScopedTransaction (Ctrl-Z support)

Mutator tools wrap their changes in `FScopedTransaction`:

```cpp
FScopedTransaction Transaction(LOCTEXT("MCP_AddNode", "Add Blueprint Node"));
Blueprint->Modify();
Graph->Modify();
// ... mutations ...
```

This makes every external MCP edit undoable via the editor's `Ctrl-Z` exactly like a manual edit. The user can see and undo any change.

## MarkPackageDirty + AssetRegistry::AssetCreated

Asset writes mark the package dirty so the user's "Save Dirty" workflow picks them up:

```cpp
NewAsset->GetOutermost()->MarkPackageDirty();
FAssetRegistryModule::AssetCreated(NewAsset);
```

For external-package actors under WorldPartition's one-file-per-actor mode, mutators check both:

```cpp
if (UPackage* ExternalPkg = Actor->GetExternalPackage()) {
    ExternalPkg->MarkPackageDirty();
} else if (UPackage* OuterPkg = Actor->GetOutermost()) {
    OuterPkg->MarkPackageDirty();
}
```

## Manual asset creation (no editor pop-up)

A classic gotcha: `IAssetTools::CreateAsset(name, path, class, Factory)` opens the matching asset editor (Sequencer for LevelSequence, Persona for AnimMontage, etc.). The Slate state then yields the game thread and starves the bridge OnEndFrame drain.

**Solution**: every creator-tool replicates the factory's body inline:

```cpp
UPackage* Pkg = CreatePackage(*PackageName);
Pkg->FullyLoad();
UAsset* NewAsset = NewObject<UAsset>(Pkg, *AssetName,
    RF_Public | RF_Standalone | RF_Transactional);
NewAsset->Initialize();   // type-specific init
FAssetRegistryModule::AssetCreated(NewAsset);
Pkg->MarkPackageDirty();
```

Skipping IAssetTools avoids the editor pop-up. All Wave C / Wave D creator-tools (sequencer/anim/audio/texture) use this pattern.

## PathInUse check — disk + in-memory

For "create asset" tools, naive `FPackageName::DoesPackageExist` only catches assets persisted to disk. For unsaved transient assets, also probe in-memory:

```cpp
if (FPackageName::DoesPackageExist(DestPath) ||
    FindObject<UObject>(nullptr, *(DestPath + TEXT(".") + AssetName)) != nullptr) {
    return -32014 PathInUse;
}
```

Without the `FindObject` probe, a double-create silently overwrites the in-memory asset.

## Pagination — opaque keyset cursor

List tools (asset.search, mesh.list, etc.) use `FMCPPageCursor`:

```json
{ "h": <u64 filter_hash>, "p": "<last_sort_key>", "t": <total_known> }
```

Base64-encoded JSON. Caller treats it as opaque; only the server inspects.

Keyset pagination semantics: server re-queries with the same filter, sorts by stable key (usually ObjectPath), skips entries with `key <= last_sort_key`, returns next `page_size`. Survives concurrent inserts/deletes between pages.

Filter mutation detection: `h` field carries the filter hash; mismatch → `-32015 StaleCursor`.

## Error codes — see [Error codes](error-codes.html)

## Threading model summary

| Lane | Thread | When | Examples |
|---|---|---|---|
| Lane A | Game thread (OnEndFrame) | Default | All UObject mutators, asset writes, viewport ops |
| Lane B | Worker pool | `bThreadSafe=true` | marshall.*, path utilities |
| Python | Game thread (Python GIL) | `kind=ExecPython` | exec_python, MCPTools.registry fallbacks |
| Async jobs | Job worker pool | `job.submit` | cb.bulk_import, asset.batch_metadata_async |

## Module layout

**As of Phase 5 module split (2026-05-22)**, the plugin contains TWO modules under `Plugins/UnrealMCPBridge/Source/`:

### UnrealMCPBridgeCore — infrastructure (~10 kLOC)

Owns server, dispatch, helpers, utils, marshalling, Python eval. NO tool surfaces. Tools module's PublicDep on Core means surfaces transparently see Core's Public/ headers.

```
UnrealMCPBridgeCore/
├── UnrealMCPBridgeCore.Build.cs
├── Public/
│   ├── MCPTypes.h            # Error codes (-32004..-32058), FMCPRequest, FMCPResponse, LogMCP
│   ├── MCPSurfaceRegistry.h  # MCP_REGISTER_SURFACE() macro + Meyers singleton
│   ├── MCPToolHelpers.h      # MakeError / MakeSuccessObj / StampIds / RequireXxxField
│   ├── MCPAssetLoader.h      # Templated path → LoadObject<T> → cast
│   ├── MCPMutatorScope.h     # RAII: PIE-guard + FScopedTransaction + MarkPackageDirty queue
│   ├── MCPJsonBuilder.h      # Fluent DSL for JSON response building
│   ├── FMCPDispatchQueue.h   # Lane A queue + Lane B registration
│   ├── FMCPServer.h          # TCP accept loop
│   ├── FMCPJobRegistry.h     # Async job pool
│   ├── FMCPLogStream.h       # log.* ring buffer
│   ├── FMCPDay7Handlers.h    # job.* / log.* / tools.list (Core surfaces)
│   ├── FMCPMarshalling.h     # marshall.* — generic FProperty IO
│   ├── FMCPPythonEval.h      # ExecPython + Python tool registry fallback
│   ├── FMCPPythonBootstrap.h # Python script plugin bootstrapping
│   └── Utils/                # ALL utility headers (promoted to Public/ for Tools surfaces)
│       ├── MCPActorPathUtils.h    # Actor path → AActor* resolution
│       ├── MCPAssetPathUtils.h    # Asset path normalization + validation
│       ├── MCPBlueprintUtils.h    # UBlueprint load helpers
│       ├── MCPPageCursor.h        # Opaque keyset pagination cursor
│       ├── MCPPinTypeUtils.h      # FEdGraphPinType ↔ JSON
│       ├── MCPPropertyPathParser.h # Dotted property paths
│       ├── MCPReflection.h        # Generic FProperty read/write
│       ├── MCPWorldContext.h      # IsPIEActive, GetEditorWorld
│       └── ...
└── Private/
    ├── UnrealMCPBridgeCoreModule.cpp # IMPLEMENT_MODULE(FDefaultModuleImpl,...) + DEFINE_LOG_CATEGORY(LogMCP)
    ├── FMCPServer.cpp / FMCPConnection.{h,cpp} / FMCPDispatchQueue.cpp
    ├── FMCPJobRegistry.cpp / FMCPLogStream.cpp
    ├── FMCPMarshalling.cpp / FMCPDay7Handlers.cpp
    ├── FMCPPythonEval.cpp / FMCPPythonBootstrap.cpp / MCPConsoleCommands.cpp
    ├── MCPSurfaceRegistry.cpp / MCPToolHelpers.cpp / MCPMutatorScope.cpp
    └── Utils/                # .cpp implementations of Public/Utils/ headers
```

### UnrealMCPBridge — 63 tool surfaces + module class (~53 kLOC)

```
UnrealMCPBridge/
├── UnrealMCPBridge.Build.cs  # PublicDependencyModuleNames.Add("UnrealMCPBridgeCore")
├── Public/
│   └── UnrealMCPBridge.h     # Module class header (IUnrealMCPBridgeModule)
└── Private/
    ├── UnrealMCPBridge.cpp   # FUnrealMCPBridgeModule lifecycle (273 LOC, down from 865
    │                         # — delegates surface registration to FMCPSurfaceRegistry::RegisterAll)
    └── Tools/                # 63 tool surface files (~53 kLOC total)
        ├── ActorTools.cpp / AIBehaviorTreeTools.cpp / AIBlackboardTools.cpp /
        │   AIControllerTools.cpp / AICrowdTools.cpp / AIEQSTools.cpp /
        │   AIPerceptionTools.cpp / AnimBlueprintTools.cpp / AnimTools.cpp /
        │   AssetCompositeTools.cpp / AssetRegistryTools.cpp / AudioTools.cpp /
        │   BlueprintTools.cpp / BlueprintComponentTools.cpp (BPSCS_) /
        │   BlueprintCompositeTools.cpp / BlueprintGraphTools.cpp / CollisionTools.cpp /
        │   ComponentTools.cpp / ConfigTools.cpp / ContentBrowserTools.cpp /
        │   CookTools.cpp / CurveTools.cpp / DataTableTools.cpp /
        │   DataValidationTools.cpp / DebugTools.cpp / EditorTools.cpp /
        │   EngineTools.cpp / FolderTools.cpp / GameplayTagTools.cpp /
        │   HierarchyTools.cpp / InputTools.cpp / LandscapeTools.cpp /
        │   LevelTools.cpp / LevelCompositeTools.cpp / LevelStreamingTools.cpp /
        │   LiveCodingTools.cpp / LogTools.cpp / MaterialInstanceTools.cpp /
        │   MaterialTools.cpp / MeshTools.cpp / NavMeshTools.cpp / NiagaraTools.cpp /
        │   PackageTools.cpp / PhysicsTools.cpp / PIETools.cpp / RenderTools.cpp /
        │   ScreenshotTools.cpp / SequencerTools.cpp / SequencerExtTools.cpp /
        │   SoftRefTools.cpp / SourceControlTools.cpp / SourceControlCompositeTools.cpp /
        │   StatsTools.cpp / SubsystemTools.cpp / TestTools.cpp / TestCompositeTools.cpp /
        │   TextureTools.cpp / ThumbnailTools.cpp / TransformTools.cpp /
        │   UFunctionTools.cpp / UMGTools.cpp / ViewportTools.cpp /
        │   WorldPartitionTools.cpp
        └── (each surface auto-registers via MCP_REGISTER_SURFACE at file scope)
```

### Module load order

`.uplugin` lists Core first, Tools second. Both LoadingPhase=Default. Lifecycle:

1. Core DLL loads — `FDefaultModuleImpl::StartupModule` is a no-op
2. Tools DLL loads — every surface's `MCP_REGISTER_SURFACE(SurfaceName, &FXxxTools::Register)` static initializer fires, populating `FMCPSurfaceRegistry::Get()`
3. Tools' `FUnrealMCPBridgeModule::StartupModule` runs:
   - Registers Python eval + unknown-method-fallback handlers
   - Registers C++ handlers for `marshall.*` / `job.*` / `log.*` / `tools.list`
   - Calls `FMCPSurfaceRegistry::Get().RegisterAll(...)` — all 63 surfaces wired in one call
   - Starts `FMCPServer`

## Auto-register pattern (MCP_REGISTER_SURFACE)

Every surface .cpp has ONE line at TU scope:

```cpp
MCP_REGISTER_SURFACE(BlueprintTools, &FBlueprintTools::Register)
```

This expands to an anonymous-namespace `struct` with a static-init body that pushes `{Name, RegisterFn}` to `FMCPSurfaceRegistry::Get()`. When `RegisterAll(...)` iterates, every surface's `Register(FMCPDispatchQueue&, TArray<FString>&)` runs.

**Benefit**: parallel-agent waves can add N surfaces concurrently without conflict on `UnrealMCPBridge.cpp` — each agent only touches its own surface .cpp.

## Helper infrastructure

Public headers in `UnrealMCPBridgeCore/Public/` that every surface uses:

### FMCPToolHelpers

```cpp
namespace FMCPToolHelpers {
    void StampIds(const FMCPRequest&, FMCPResponse&);
    FMCPResponse MakeError(const FMCPRequest&, int32 Code, const FString& Message);
    FMCPResponse MakeSuccessObj(const FMCPRequest&, TSharedRef<FJsonObject>);
    FMCPResponse MakeSuccessObj(const FMCPRequest&, TSharedPtr<FJsonObject>); // asserts non-null
    FMCPResponse MakeSuccessValue(const FMCPRequest&, TSharedRef<FJsonValue>);
    bool RequireStringField(const FMCPRequest&, const TCHAR* FieldName, FString&, FMCPResponse& OutErr);
    bool RequireNumberField / RequireIntField / RequireBoolField / RequireArrayField / RequireObjectField (...);
}
```

**Always qualify as `FMCPToolHelpers::MakeError(...)` explicitly** — UE's `Templates/ValueOrError.h` has a free `MakeError<>` template that collides via ADL on `FMCPRequest&` arg.

### FMCPAssetLoader

Templated path → LoadObject → cast helper:

```cpp
int32 ErrCode = 0; FString ErrMsg;
UDataTable* Table = FMCPAssetLoader::Load<UDataTable>(InPath, ErrCode, ErrMsg);
if (!Table) return FMCPToolHelpers::MakeError(Request, ErrCode, ErrMsg);
```

Sets `-32010 InvalidPath`, `-32004 ObjectNotFound`, or `-32011 WrongClass`. `LoadRaw()` variant returns `UObject*` without type check.

### FMCPMutatorScope

RAII guard for editor-world mutators:

```cpp
FMCPMutatorScope Scope(Request, LOCTEXT("MCP_DT_SetRow", "DataTable: Set Row"));
if (Scope.PIEBlocked()) return Scope.Error();  // -32027 PIEActive
Table->Modify();
// ... mutate ...
Scope.DirtyPackage(Table->GetPackage());
// Destructor commits FScopedTransaction + flushes MarkPackageDirty queue.
```

**DO NOT** use in:
- Lane B sync wrappers (would crash listener thread)
- PIE-friendly runtime mutators (physics.apply_impulse, ai.bb.set_value, etc.)
- Tools that already own a `FMCPWritePropertyScope` (nested-transaction concern)

### FMCPJsonBuilder

Fluent DSL for JSON response construction:

```cpp
return FMCPJsonBuilder()
    .Str(TEXT("path"), AssetPath)
    .Num(TEXT("count"), Count)
    .Bool(TEXT("loaded"), bLoaded)
    .Arr(TEXT("rows"), MoveTemp(RowsArray))     // cache .Num() BEFORE chain if needed
    .OptStr(TEXT("name"), MaybeEmptyName)        // skip if empty
    .Object(TEXT("transform"), [&](FMCPJsonBuilder& Sub) { Sub.Num(TEXT("x"), L.X); })
    .ObjectShared(TEXT("preBuilt"), ExistingSharedRef)
    .Field(TEXT("v"), TSharedRef<FJsonValue>(...))
    .Null(TEXT("nullable"))
    .If(bCondition, [&](FMCPJsonBuilder& B) { B.Str(...); })
    .BuildSuccess(Request);

// FMCPJsonArrayBuilder for array-of-objects payloads
```

62 of 63 surfaces use this builder for success-response construction.
