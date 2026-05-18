# UnrealMCPBridge

Editor-only plugin that exposes Unreal Editor tools to an external MCP
(Model Context Protocol) server over TCP. Together with `mcp_server/` in the
project root it lets an AI client (Claude Desktop, Claude Code, etc.) drive
the editor for asset/level/blueprint authoring tasks.

## Status

**Phase 3 complete — 92 user-visible tools + 10 internal hidden handlers.**

| Phase | Tools | Surface |
|---|---|---|
| Phase 1 | 16 | marshall.* (4), job.* (5), log.* (3), tools.list, editor.* (3) |
| Phase 2 | 31 | asset.* (13 C++ + 6 Python composites = 19), cb.* (12) — plus 5 internal hidden handlers |
| Phase 3 | 45 | level.* (12), actor.* (20), component.* (8), composites (5 user-visible Python) — plus 5 internal hidden C++ handlers |
| **Total** | **92** | (user-visible; 102 total handler registrations counting hidden internals) |

### Phase 3 polish round (2026-05) — 5 known issues addressed

After Phase 3 Days 11-14 landed, a polish pass cleared 5 remaining nits:

- **#9** `level.get_persistent_level_actors` migrated from integer-offset pagination to
  `FMCPPageCursor` sentinel cursor (filter_hash + last_path + total_known snapshot). Caller
  changing filter mid-pagination now returns `-32015 StaleCursor` instead of silently skipping
  items.
- **#10** ActorTools.cpp: page_size default 200 → 100 (256 KB JSON cap convention),
  `ACT_ReadJsonVector` made `[[nodiscard]]`, `ACT_HashFilter` discriminator promoted to
  `enum class EACTHashFilter : uint8`, `Tool_Attach` cycle-detection walk gained a 256-depth
  bound returning `-32603` on overflow.
- **#11** Folder unification — `Source/UnrealMCPBridge/Private/Utility/` (Phase 3 utilities)
  merged into `Utils/` (Phase 1/2 utilities). Single utility namespace per project convention.
- **#12** Phase 1 Python dispatcher: tool-body Python exceptions now translate to JSON-RPC
  errors with type-tagged messages (`ValueError`/`KeyError`/`TypeError` → `-32602 Invalid Params`,
  other `Exception` → `-32603 Internal Error`). Prior to this fix, a composite raising
  `ValueError` on empty input would block the game thread and the client would get a `-32002 Timeout`
  after ~5 s instead of a structured error.
- **#13** README updated with Phase 3 inventory + consolidated `smoke_phase3.py` wrapper script
  (calls all 4 Days sub-suites and aggregates pass/fail).

### Known limitation — Phase 2 ships all tools Lane A (Hotfix 1, 2026-05)

The original Phase 2 design registered 11 user-visible AR-query tools (plus 2 internal
helpers) on **Lane B** (TCP listener thread, bypassing the game-thread `Drain` queue) for
~16× latency improvement. The AR read API is documented thread-safe since UE 5.0, so this
seemed safe.

Autonomous testing in UE 5.7 surfaced an assertion crash:

```
Assertion failed: IsInGameThread() || IsInAsyncLoadingThread() || IsInParallelLoadingThread()
[AssetRegistry.cpp:2906]
Enumerating in-memory assets can only be done on the game thread or in the loader,
there are too many GetAssetRegistryTags() still not thread-safe.
```

Epic themselves note "too many `GetAssetRegistryTags()` still not thread-safe." Per the
phase-2 plan's R11 contingency ("> 5 Lane B demotions → Lane B becomes a no-op for Phase 2,
infrastructure remains for Phase 3+"), every AR query tool was demoted to **Lane A** (game
thread, processed in the `OnEndFrame` drain). The Lane B router infrastructure
(`FMCPDispatchQueue::IsThreadSafe` + `DispatchInline`, `FMCPConnection` listener-thread
short-circuit) stays in place — only the per-tool registration flag changes.

Phase 3+ may revisit Lane B promotion by passing
`FARFilter::bIncludeOnlyOnDiskAssets=true` per call, which should skip the in-memory
enumeration path that hits the assert. The handler bodies are already authored to the Lane B
contract (no UObject access, no GWorld), so flipping the flag is a one-line revival per tool
after the filter change is verified stand-alone.

### Hotfix 2 → Hotfix 3 evolution (2026-05) — Python composites became fully async-only

**Hotfix 2** promoted the 3 internal composite handlers (``_find_unused_internal``,
``_size_report_internal``, ``_batch_metadata_internal``) plus ``job.status`` /
``job.result`` to Lane B, on the assumption that the composite could safely block on its
own job's poll loop if the polling endpoint was off-game-thread. This did NOT fix the
deadlock because the composite STILL owns the GT while sleeping between polls — a
GT-required job body can never run while the GT is sleeping in the composite.

**Hotfix 3** converts every composite to async-only — they return ``{job_id}`` and the AI
client polls externally. The C++ internal handler set grew from 3 to 5 (added
``_find_broken_references_internal`` and ``_find_duplicates_by_name_internal``); all 5 use
the same async-job pattern. ``asset.batch_metadata`` collapsed its sync/async split into a
single async tool (the 200-cap was removed; the 5000-cap matches the previous async variant).
See "Why composites are async" under Category D for the full deadlock pattern explanation.

## Reference

Full design blueprint (v2 with critic fixes applied):
`D:/tmp/mcp_unreal_blueprint_v2_patch.md`

Phase 2 implementation plan:
`D:/tmp/mcp_phase2_plan.md`

## Install

1. Build the project (`UnrealMCPBridge` is `EnabledByDefault: true`).
2. Install the companion server: `pip install -e ../../mcp_server`.
3. Launch the editor; the bridge listener starts on port 30020 (loopback only)
   and logs `LogMCP: MCP bridge listening on 127.0.0.1:30020`.

## Layout

```
UnrealMCPBridge/
  UnrealMCPBridge.uplugin
  Source/UnrealMCPBridge/
    UnrealMCPBridge.Build.cs
    Public/  UnrealMCPBridge.h, MCPTypes.h, FMCPDispatchQueue.h, FMCPJobRegistry.h
    Private/
      UnrealMCPBridge.cpp, FMCPConnection.cpp, FMCPServer.cpp, ...
      Tools/
        AssetRegistryTools.h/.cpp   # Phase 2 Category A — 13 AR reads (all Lane A post-hotfix; was 10 Lane B)
        ContentBrowserTools.h/.cpp  # Phase 2 Category B — 12 CB writes (all Lane A; list_folders was Lane B)
        AssetCompositeTools.h/.cpp  # Phase 2 Category D — 5 internal C++ helpers (all Lane B, all async-job)
        LevelTools.h/.cpp           # Phase 3 Category A — 12 level.* tools + 1 hidden Lane B probe
        ActorTools.h/.cpp           # Phase 3 Category B — 20 actor.* tools (all Lane A)
        ComponentTools.h/.cpp       # Phase 3 Category C — 8 component.* tools (all Lane A)
        LevelCompositeTools.h/.cpp  # Phase 3 Category D — 5 internal C++ submitters (Lane B, async-job)
      Utils/                                # Unified utility namespace post-polish #11
        MCPAssetPathUtils.h/.cpp    # Phase 2 — asset path canonicalisation
        MCPARFilterParser.h/.cpp    # Phase 2 — FARFilter JSON ↔ struct + hash
        MCPPageCursor.h/.cpp        # Phase 2 — opaque sentinel cursor (also used by Phase 3)
        MCPPathSandbox.h/.cpp       # Phase 2 — disk-path whitelist guard
        MCPReflection.h/.cpp        # Phase 3 Day 0 — FProperty read/write helpers + FMCPWritePropertyScope RAII
        MCPWorldContext.h/.cpp      # Phase 3 — GetEditorWorld / IsPIEActive / ResolveLevelByMapPath
        MCPActorPathUtils.h/.cpp    # Phase 3 — ParseActorPath / BuildActorPath / ResolveActor
        MCPComponentPathUtils.h/.cpp # Phase 3 — ResolveComponent (with ambiguity detection)
        MCPPropertyPathParser.h/.cpp # Phase 3 — dotted path + array-index parser
  Content/Python/MCPTools/
    registry.py     # @tool decorator (with _internal=True filter); Phase 1 polish #12 wraps
                    # tool body with try/except → translates Python exceptions to JSON-RPC errors
    marshall.py     # Tier 1 type marshalling
    tools/
      smoke_tools.py       # editor.ping demo
      asset_tools.py       # Shared helpers for Phase 2 composites
      asset_composites.py  # Phase 2 — 6 Category D Python composites
      level_composites.py  # Phase 3 — 5 Category D Python composites (level/actor batch ops)
  Tests/
    smoke_ping.py                # Phase 1 14-subtest smoke
    smoke_phase2.py              # Phase 2 34-subtest smoke
    smoke_phase3.py              # Phase 3 wrapper — runs all 4 Days sub-suites and aggregates
    smoke_phase3_days_1_3.py     # Phase 3 — 12 level.* tools
    smoke_phase3_days_4_8.py     # Phase 3 — 20 actor.* tools (22 sub-tests)
    smoke_phase3_days_9_10.py    # Phase 3 — 8 component.* tools
    smoke_phase3_days_11_14.py   # Phase 3 — 5 composite tools (9 sub-tests)
    lane_b_spike.py              # Phase 2 Day 0 Lane B audit harness
```

## Phase 2 tool catalogue (31 user-visible tools)

Breakdown: **13 C++ asset.*** + **6 Python composite asset.*** + **12 cb.*** = 31 user-visible
tools, plus **5 internal hidden** asset._*internal handlers (used by Python composites — all
async-job pattern post-Hotfix-3).

### Category A — Asset Registry queries (13 C++ tools, all Lane A post-hotfix)

```
asset.exists                  → {exists, asset_path_canonical}                    Lane A (was B)
asset.metadata                → {class, package, tags, size_disk, ...}            Lane A (was B)
asset.list                    → {assets[], next_page_token, total_known, ...}     Lane A (was B)
asset.find_references         → {referencers[], next_page_token, total_known}     Lane A (was B)
asset.find_dependents         → {dependents[], next_page_token, total_known}      Lane A (was B)
asset.search_by_class         → {matches[], next_page_token, total_known}         Lane A (was B)
asset.search_by_tag           → {matches[], next_page_token, total_known}         Lane A (was B)
asset.search_by_name          → {matches[], next_page_token, total_known}         Lane A (was B)
asset.get_class_hierarchy     → {chain[]}                                          Lane A (was B)
asset.get_outermost_package   → {package_path, on_disk}                            Lane A (was B)
asset.get_thumbnail           → {base64, mime, width, height, is_class_generic}   Lane A (RT enqueue)
asset.get_thumbnail_to_disk   → {path, bytes, width, height}                       Lane A (RT enqueue)
asset.is_dirty                → {dirty, in_memory}                                  Lane A (loaded-pkg map)
```

Example:
```jsonc
{"id":"q1","kind":"call_function","method":"asset.list",
 "args":{"filter":{"package_paths":["/Game/Characters"],"recursive_paths":true,
                   "class_paths":["/Script/Engine.SkeletalMesh"]},
         "page_size":50}}
```

### Category B — Content Browser write operations (12 tools, all Lane A)

```
cb.create_folder      → {created, normalized_path}      idempotent
cb.rename             → {success, canonical_new_path}    FScopedTransaction
cb.save               → {saved}                          no transaction (saves not undoable)
cb.move               → {moved[], failed[]}              per-asset transaction (D4)
cb.duplicate          → {new_path}                       FScopedTransaction
cb.delete             → {deleted, redirector_left}       force=true Display-logged (Warning on depth-2)
cb.fix_redirectors    → {fixed_count, removed_count}    500-redirector hard cap
cb.list_folders       → {folders[]}                       Lane A (was B; hotfix 2026-05)
cb.import             → {asset_path}                     UAssetImportTask, sandboxed source
cb.export             → {exported, bytes}                temp-dir trampoline + sandboxed dest
cb.save_all_dirty     → {job_id}                          ASYNC (job)
cb.bulk_import        → {job_id}                          ASYNC (job)
```

Example:
```jsonc
{"id":"m1","kind":"call_function","method":"cb.move",
 "args":{"source_paths":["/Game/Old/Foo","/Game/Old/Bar"],
         "dest_folder":"/Game/New"}}
```

### Category D — Python composites (6 user-visible + 5 internal C++) — ALL ASYNC

**HOTFIX 3 (2026-05): every composite is async-only — they return ``{job_id}`` and the AI
client polls externally via ``job.status`` / ``job.result``.** See "Why composites are async"
below for the deadlock pattern this resolves.

```
asset.find_unused              → {job_id}  → asset._find_unused_internal             (Lane B, async, GT)
asset.size_report              → {job_id}  → asset._size_report_internal             (Lane B, async, GT)
asset.batch_metadata           → {job_id}  → asset._batch_metadata_internal          (Lane B, async, worker pool)
asset.batch_metadata_async     → {job_id}  → asset._batch_metadata_internal          (Lane B, async — alias)
asset.find_broken_references   → {job_id}  → asset._find_broken_references_internal  (Lane B, async, GT)
asset.find_duplicates_by_name  → {job_id}  → asset._find_duplicates_by_name_internal (Lane B, async, GT)
```

Inner result schemas (returned by ``job.result`` once Succeeded):

```
asset.find_unused              → {unused[{asset_path, class}], scanned_count}
asset.size_report              → {top[{asset_path, class, bytes}], total_bytes}
asset.batch_metadata           → {assets[{asset_path, package_path, class, tags}], failed[{path, error}], duration_ms}
asset.batch_metadata_async     → (same as asset.batch_metadata)
asset.find_broken_references   → {broken[{asset_path, missing_paths[]}], scanned_count}
asset.find_duplicates_by_name  → {duplicates[{name, paths[{asset_path, class}]}], scanned_count}
```

Example (submit + poll):
```jsonc
// 1. submit
{"id":"u1","kind":"call_function","method":"asset.find_unused",
 "args":{"package_paths":["/Game/Untracked"]}}
// → response: {"ok":true, "result":{"job_id":"abc-123-..."}}

// 2. poll until terminal (off-game-thread)
{"id":"u2","kind":"call_function","method":"job.result",
 "args":{"job_id":"abc-123-...","wait_timeout_s":30}}
// → response on Succeeded: {"ok":true, "result":{"state":"Succeeded", "result":{"unused":[...], "scanned_count":...}}}
```

### Why composites are async (Hotfix 3 deadlock pattern)

The pre-Hotfix-3 composites tried to be synchronous: submit a job + poll ``job.result`` from
inside the composite body, return the inner result to the caller. This deadlocked under three
constraints that conspired against the design:

  1. **Composite owns the game thread.** Python composites run inside
     ``FMCPPythonEval::CallPythonTool`` which executes on the GT (Python GIL is pinned to GT).
     Once the composite enters, the GT is blocked until it returns.
  2. **AR enumeration requires GT (UE 5.7).** ``IAR.GetAssets`` asserts off-GT —
     ``AssetRegistry.cpp:2906`` "Enumerating in-memory assets can only be done on the game
     thread or in the loader, there are too many GetAssetRegistryTags() still not thread-safe."
     This was the Hotfix 1 finding.
  3. **Game-thread-required jobs need GT to run.** A job submitted with
     ``bGameThreadRequired=true`` is dispatched via ``AsyncTask(ENamedThreads::GameThread, ...)``
     — it cannot execute while the GT is owned by the composite.

Combine all three: composite owns GT → polls job.result → job body waits for GT → never gets
it → 60s deadlock until TCP timeout. Promoting ``job.result`` to Lane B (Hotfix 2 attempt)
removed the *outer* loopback queue, but the inner GT-required job body still couldn't drain
because the composite kept sleeping in ``time.sleep`` between polls while holding GT.

Resolution: composites NEVER poll. They return ``{job_id}`` immediately. The AI client polls
``job.status`` / ``job.result`` from outside the GT (its own thread on the external TCP socket).
The composite call exits in <1ms, GT becomes free, GT-required job body drains in the
next tick. This is the pattern ``asset.batch_metadata_async`` has used since Day 12 and which
always worked correctly.

The bridge-level helper ``wait_for_job_and_return_result`` (in ``asset_tools.py``) remains
available for future tooling that runs off-GT, but NO production composite uses it.

## Phase 3 tool catalogue (45 user-visible tools)

Breakdown: **12 level.*** + **20 actor.*** + **8 component.*** + **5 user-visible Python composites**
= 45 user-visible tools, plus **5 internal hidden** level._*/actor._*_internal C++ submitters
used by the Python composites (all async-job pattern).

All Phase 3 mutators are **PIE-guarded** — refused with `-32027 PIEActive` when
`GEditor->PlayWorld != nullptr`, returning the frozen message that points callers at the
future `pie.*` surface (Phase 5). World Partition maps are hard-rejected by every level/actor
mutator with `-32029 WorldPartitionNotSupported`.

### Category A — Level operations (12 tools, all Lane A)

```
level.list_loaded                  → {levels[{map_path, kind, loaded, visible, ...}]}
level.current_map                  → {map_path, world_kind}
level.load                         → {loaded, was_already_loaded}             editor-world only
level.save                         → {saved, dirty_before, package_size}      transactional
level.create                       → {created, map_path}                       PIE-guarded
level.unload                       → {unloaded, was_loaded}                    PIE-guarded
level.set_streaming_state          → {state_changed, prior_state, new_state}  PIE-guarded
level.get_world_settings           → {properties:{gravity, time_dilation, ...7 fields}}
level.set_world_settings           → {applied_count, rejected[{field, reason}]}
level.get_persistent_level_actors  → {actors[{actor_path, class, label}], next_page_token, total_known}
level.save_all_dirty               → {job_id}                                  ASYNC (Lane A submitter)
level.duplicate                    → {duplicated, source_map, dest_map}        PIE-guarded
```

### Category B — Actor operations (20 tools, all Lane A)

```
actor.spawn                  → {actor_path, label}                       PIE-guarded
actor.destroy                → {destroyed, was_already_gone}             PIE-guarded
actor.duplicate              → {actor_path}                              PIE-guarded
actor.get                    → {actor_path, class, label, folder, transform, components_count}
actor.set_transform          → {applied, prior}                          PIE-guarded
actor.set_location           → {applied, prior_location}                 PIE-guarded
actor.set_rotation           → {applied, prior_rotation}                 PIE-guarded
actor.set_scale              → {applied, prior_scale}                    PIE-guarded
actor.set_label              → {applied, prior_label}                    PIE-guarded
actor.set_folder             → {applied, prior_folder}                   PIE-guarded
actor.attach                 → {attached, prior_parent}                  PIE-guarded; 256-depth cycle bound
actor.detach                 → {detached, was_attached_to}               PIE-guarded
actor.get_property           → {value, type, property_path}              uses FMCPReflection
actor.set_property           → {applied, prior_value}                    edit-const 3-flag gate + RAII scope
actor.exists                 → {exists}
actor.select_in_editor       → {selected}                                editor-only
actor.find_by_class          → {matches[{actor_path, label}], next_page_token, total_known}
actor.find_by_label          → {matches[...], next_page_token, total_known}
actor.find_by_tag            → {matches[...], next_page_token, total_known}
actor.list_components        → {components[{component_path, class, attach_parent}]}
```

### Category C — Component operations (8 tools, all Lane A)

```
component.add                                → {component_path, class}              PIE-guarded; full lifecycle
component.remove                             → {removed}                            PIE-guarded
component.get                                → {component_path, class, attach_parent, transform, properties}
component.get_property                       → {value, type}
component.set_property                       → {applied, prior_value}              edit-const gate
component.set_transform                      → {applied, prior}                    PIE-guarded
component.move_in_hierarchy                  → {moved, prior_parent}               PIE-guarded
component.list_class_default_subcomponents   → {subcomponents[{name, class}]}
```

`component.add` walks the full UE registration lifecycle:
`NewObject` → `AddInstanceComponent` → `OnComponentCreated` → `RegisterComponent` →
`RerunConstructionScripts`. Returns the resolved component path including ambiguity disambiguator
suffix if needed (`-32024 AmbiguousComponent` if multiple components share an FName).

### Category D — Python composites (5 user-visible + 5 internal C++) — ALL ASYNC

Same pattern as Phase 2 composites — composites return `{job_id}` immediately; AI client polls
`job.status` / `job.result` from outside the game thread. See "Why composites are async" under
Phase 2's Category D for the deadlock pattern this resolves.

```
level.full_actor_dump          → {job_id}  → level._full_actor_dump_internal         (Lane B, async, GT body)
level.find_actors_with_class   → {job_id}  → level._find_actors_with_class_internal  (Lane B, async, GT body)
actor.batch_spawn              → {job_id}  → actor._batch_spawn_internal             (Lane B, async, GT body)
actor.batch_destroy            → {job_id}  → actor._batch_destroy_internal           (Lane B, async, GT body)
actor.batch_set_property       → {job_id}  → actor._batch_set_property_internal      (Lane B, async, GT body)
```

Inner result schemas (returned by `job.result` once `Succeeded`):

```
level.full_actor_dump          → {actors[{actor_path, class, label, transform, ...}], total_count, scanned_count}
level.find_actors_with_class   → {matches[{actor_path, label}], scanned_count}
actor.batch_spawn              → {succeeded[{actor_path, label, index}], failed[{index, reason}]}
actor.batch_destroy            → {succeeded[{actor_path, was_already_gone, index}], failed[...]}
actor.batch_set_property       → {succeeded[{actor_path, property_path, prior_value, index}], failed[...]}
```

Batch caps: `MAX_BATCH_ITEMS=1000` per request, `MAX_ACTORS_PER_DUMP=5000` for the dump. Empty
input arrays return `-32602 InvalidParams` synchronously (NOT as a failing job — caught at the
Python wrapper layer, which is why polish #12 mattered: pre-fix this synchronous rejection would
silently time out instead of returning the proper error).

### Phase 3 error codes (-32019..-32029)

11 new error codes were added in Phase 3:

```
-32019 LevelNotFound                  map_path resolves to no UWorld, OR actor's owning sublevel not loaded
-32020 ClassNotFound                  actor.spawn class_path autoload failed
-32021 ClassAbstract                  actor.spawn target UClass has CLASS_Abstract
-32022 WrongClassFamily               actor.spawn class_path is not an AActor subclass
-32023 InvalidClassPath               actor.spawn class_path syntactically malformed
-32024 AmbiguousComponent             component.* tools: actor has multiple components with same FName
-32025 PropertyPathTooDeep            Property path nesting exceeds 16-segment hard cap
-32026 PropertyIndexOOB               Property path used [N] indexing past array bounds
-32027 PIEActive                      Editor-world mutator refused — PIE running (Phase 5 will ship pie.*)
-32028 LevelNotStreamingEntry         level.set_streaming_state target not in GetStreamingLevels()
-32029 WorldPartitionNotSupported     Map is World Partition — Phase 5 will ship dedicated wp.* surface
```

The `-32027 PIEActive` message is **frozen** — smoke tests assert both substrings `"Phase 5"`
and `"pie."` so any client UI can rely on stable wording.

## Common patterns

### `cb.move_with_redirector_cleanup` (2-line recipe — no separate tool)

```python
bridge.call("cb.move", {"source_paths": ["/Game/Old/Foo"], "dest_folder": "/Game/New"})
bridge.call("cb.fix_redirectors", {"path": "/Game/New", "recursive": True})
```

The first call leaves a `UObjectRedirector` at each source path so existing
references stay valid; the second consolidates and deletes the redirectors.

### Atomic bulk rename inside a single subfolder

```python
# Build a {old: new} map, walk with cb.rename. Per-item transactions so user can
# Ctrl+Z one mistake at a time.
for old, new in plan.items():
    bridge.call("cb.rename", {"old_path": old, "new_path": new})
```

### Polling an async job

```python
job = bridge.call("cb.save_all_dirty", {})
while True:
    status = bridge.call("job.status", {"job_id": job["job_id"]})
    if status["state"] in ("Succeeded", "Failed", "Cancelled"):
        break
    time.sleep(0.5)
print(status)
```

## Lane B contract

Handlers registered with `bThreadSafe=true` (Lane B) run on the **TCP listener
thread**, NOT on the game thread. This bypasses the OnEndFrame Drain queue and
removes the per-call ~16ms tick-quantization latency.

**Lane B handlers MUST NOT:**
- Touch UObjects (no `LoadObject`, no `FindObject`, no `GetClass()` walks)
- Touch `GWorld`, `GEngine`, or any UEngineSubsystem / UEditorSubsystem
- Modify any state (no `Set*`, no AR mutations, no editor commands, no
  `UPackage::SetDirty`)
- Allocate persistent UObjects or call `NewObject`
- Hold the GC lock or interact with `FUObjectGlobals`

**Lane B handlers MAY:**
- Call `IAssetRegistry::Get()->GetAssets`, `GetReferencers`, `GetDependencies`,
  `GetSubPaths`, `IsLoadingAssets`, `GetAssetByObjectPath`, `GetAssetsByTags`,
  `GetAssetsByTagValues`, `GetAssetsByPath` (thread-safe since UE 5.0)
- Call `FPackageName::DoesPackageExist` (filesystem-only)
- Call `IFileManager::FileSize`
- Perform pure math / string / JSON serialization

If a Lane B tool exhibits sporadic asserts or crashes, demote by passing
`bThreadSafe=false` to `RegisterHandler` — the infrastructure stays in place;
only the per-tool flag changes.

See `Source/UnrealMCPBridge/Public/FMCPDispatchQueue.h` for the full contract
text and the `IsThreadSafe()` / `DispatchInline()` API documentation.

## `cb.delete force=true` logging

The `cb.delete` tool defaults to `force=false` (safe path: refuses if asset has
referencers; MAY autoload the asset's package to perform the reference walk).

`force=true` uses `ObjectTools::ForceDeleteObjects` — **no reference check, no
recycle bin**. Three layers of guard:

1. **Always Display-logged**: every `force=true` call emits a line at Display
   verbosity with the full path:
   ```
   LogMCP: Display: MCP cb.delete force=true: /Game/Player/SomeAsset
   ```
2. **Depth-2 paths additionally Warning-logged**: paths matching
   `/Game/<single-segment>/<leaf>` (e.g. `/Game/Player/Foo`, `/Game/Maps/MainMenu`)
   additionally emit a Warning — these are the most-likely-to-be-mistake deletes:
   ```
   LogMCP: Warning: MCP cb.delete force=true on depth-2 path (likely-mistake guard): /Game/Player/SomeAsset
   ```
3. **Asset-vs-folder guard**: paths resolving to folders return `INVALID_PATH`
   immediately (cb.delete is single-asset only).

Audit force-delete history via `MCP.LogTail force=true` console command or
`log.search` MCP tool with pattern `force=true`.

## `asset.find_unused` static-analysis caveat

**STATIC analysis only — runtime references are INVISIBLE to the AR.**

What `asset.find_unused` cannot see:
- `LoadClass(Class, "/Game/...")` calls at runtime
- Game Mode default-pawn / -controller spawn references
- Blueprint construction-script soft references
- Savegame data referencing asset paths as strings
- Data-table cell values containing asset paths
- Any reflection-based asset spawn (e.g. `UInstancedStaticMeshComponent` with
  per-instance mesh assignment)

**Always confirm via in-editor Reference Viewer (right-click asset → Reference
Viewer) BEFORE deleting any result from `asset.find_unused`.**

The default `exclude_class_paths` covers the canonical runtime-load set
(`World`, `MapBuildDataRegistry`, `GameModeBase`, `GameMode`, `PlayerController`,
`GameStateBase`, `PlayerState`, `HUD`, `GameInstance`, `GameUserSettings`,
`SaveGame`). Extend this list for project-specific classes that runtime-load
their content (custom Game-Mode subclasses, asset-manager driven systems, etc.).

## Smoke tests

End-to-end harnesses live in `Tests/`. All require the editor running with the bridge
listener up (loopback port 30020 by default).

| Suite | Sub-tests | Surface |
|---|---|---|
| `smoke_ping.py` | 14 | Phase 1 — `editor.*`, `marshall.*`, `job.*`, `log.*`, `tools.list` |
| `smoke_phase2.py` | 34 | Phase 2 — every Category A/B/D asset/cb tool, positive + negative |
| `smoke_phase3.py` | **wrapper** | Runs all 4 Phase 3 sub-suites below and aggregates pass/fail |
| `smoke_phase3_days_1_3.py` | 7 | Phase 3 Days 1-3 — 12 `level.*` tools + Lane B sanity |
| `smoke_phase3_days_4_8.py` | 22 | Phase 3 Days 4-8 — 20 `actor.*` tools |
| `smoke_phase3_days_9_10.py` | 9 | Phase 3 Days 9-10 — 8 `component.*` tools |
| `smoke_phase3_days_11_14.py` | 9 | Phase 3 Days 11-14 — 5 composite tools (full_actor_dump, batch_spawn, ...) |

Run a specific phase:
```
python Tests/smoke_phase2.py [--host 127.0.0.1] [--port 30020]
python Tests/smoke_phase3.py                              # runs all 4 sub-suites in sequence
python Tests/smoke_phase3_days_4_8.py                     # just the actor.* surface
```

Pre-test data prep (one-time, optional — sub-tests with missing assets log
SKIP rather than fail):
1. `Content/MCPTest/PhaseTwo/DA_PhaseTwoTest.uasset` — duplicate of any
   `UFlecsEntityDefinition`. Required for sub-tests 1, 3, 7-11, 13, 22, etc.
2. `Plugins/UnrealMCPBridge/Tests/test_assets/test_texture.png` — 32x32
   magenta PNG for sub-test 23 (`cb.import`).
3. `Plugins/UnrealMCPBridge/Tests/test_assets/test_mesh.fbx` — minimal cube
   FBX for sub-test 27 (`cb.bulk_import`). MAY BE SKIPPED.
4. **Phase 3:** ANY non-empty editor map (the default test map at `/Game/Maps/Default`
   suffices). `smoke_phase3_days_1_3.py` sub-test 5 reads `level.get_persistent_level_actors`
   page 1 and asserts `total_known >= 0`. If the persistent level is empty, downstream
   tests (`find_actors_with_class`, `batch_spawn`/`destroy`) still pass — they spawn their
   own actors first.
