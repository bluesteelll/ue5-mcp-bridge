# UnrealMCPBridge

Editor-only plugin that exposes Unreal Editor tools to an external MCP
(Model Context Protocol) server over TCP. Together with `mcp_server/` in the
project root it lets an AI client (Claude Desktop, Claude Code, etc.) drive
the editor for asset/level/blueprint authoring tasks.

## Status

**Phase 5 complete — 145 user-visible tools + 11 internal hidden handlers.**

| Phase | Tools | Surface |
|---|---|---|
| Phase 1 | 16 | marshall.* (4), job.* (5), log.* (3), tools.list, editor.* (3) |
| Phase 2 | 31 | asset.* (13 C++ + 6 Python composites = 19), cb.* (12) — plus 5 internal hidden handlers |
| Phase 3 | 45 | level.* (12), actor.* (20), component.* (8), composites (5 user-visible Python) — plus 5 internal hidden C++ handlers |
| Phase 4 | 23 | bp.* (13 C++ + 1 Python composite = 14), material.* (9) — plus 1 internal hidden C++ handler |
| Phase 5 | 30 | pie.* (10), editor.* (9 + pie.screenshot_to_disk), umg.* (2), niagara.* (1), physics.* (2), sequencer.* (5) |
| **Total** | **145** | (user-visible; 156 total handler registrations counting hidden internals) |

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
        AssetRegistryTools.h/.cpp     # Phase 2 Category A — 13 AR reads (all Lane A post-hotfix; was 10 Lane B)
        ContentBrowserTools.h/.cpp    # Phase 2 Category B — 12 CB writes (all Lane A; list_folders was Lane B)
        AssetCompositeTools.h/.cpp    # Phase 2 Category D — 5 internal C++ helpers (all Lane B, all async-job)
        LevelTools.h/.cpp             # Phase 3 Category A — 12 level.* tools + 1 hidden Lane B probe
        ActorTools.h/.cpp             # Phase 3 Category B — 20 actor.* tools (all Lane A)
        ComponentTools.h/.cpp         # Phase 3 Category C — 8 component.* tools (all Lane A)
        LevelCompositeTools.h/.cpp    # Phase 3 Category D — 5 internal C++ submitters (Lane B, async-job)
        BlueprintTools.h/.cpp         # Phase 4 Days 1-10 — 13 bp.* tools (6 reads + 6 writes + bp.compile)
        BlueprintCompositeTools.h/.cpp # Phase 4 Day 10 — 1 internal Lane B submitter (bp.compile_all_dirty)
        MaterialTools.h/.cpp          # Phase 4 Days 11-15 — 9 material.* tools (reads + MIC writes + create + diagnostic)
        PIETools.h/.cpp               # Phase 5 Chunk A — 10 pie.* tools (lifecycle + introspection + actor identity)
        EditorTools.h/.cpp            # Phase 5 Chunk B — 9 editor.* + pie.screenshot_to_disk (viewports + selection + toast)
        UMGTools.h/.cpp               # Phase 5 Chunk C — 2 umg.* tools (WidgetTree walk + CDO property read)
        NiagaraTools.h/.cpp           # Phase 5 Chunk C — 1 niagara.list_parameters (user/system/emitter enum)
        PhysicsTools.h/.cpp           # Phase 5 Chunk C — 2 physics.* tools (Chaos line trace + capsule sweep)
        SequencerTools.h/.cpp         # Phase 5 Chunk D — 5 sequencer.* tools (list_cinematics + tracks + cuts + keyframes + current_time)
      Utils/                                # Unified utility namespace post-polish #11
        MCPAssetPathUtils.h/.cpp     # Phase 2 — asset path canonicalisation
        MCPARFilterParser.h/.cpp     # Phase 2 — FARFilter JSON ↔ struct + hash
        MCPPageCursor.h/.cpp         # Phase 2 — opaque sentinel cursor (also used by Phase 3 + 4)
        MCPPathSandbox.h/.cpp        # Phase 2 — disk-path whitelist guard
        MCPReflection.h/.cpp         # Phase 3 Day 0 — FProperty read/write helpers + FMCPWritePropertyScope RAII
        MCPWorldContext.h/.cpp       # Phase 3 — GetEditorWorld / IsPIEActive / ResolveLevelByMapPath
        MCPActorPathUtils.h/.cpp     # Phase 3 — ParseActorPath / BuildActorPath / ResolveActor
        MCPComponentPathUtils.h/.cpp # Phase 3 — ResolveComponent (with ambiguity detection)
        MCPPropertyPathParser.h/.cpp # Phase 3 — dotted path + array-index parser
        MCPBlueprintUtils.h/.cpp     # Phase 4 — LoadBlueprintByPath / FindVariableIndex / GetGeneratedClass
        MCPPinTypeUtils.h/.cpp       # Phase 4 — FEdGraphPinType ↔ JSON (D3/D4 — fail-fast on unsupported)
        MCPMaterialUtils.h/.cpp      # Phase 4 — LoadMaterialInterfaceByPath / LoadMICByPath / WalkToBaseMaterial
        MCPScreenshotUtils.h/.cpp    # Phase 5 — viewport/PIE screenshot encode (PNG/JPG via FImageUtils)
  Content/Python/MCPTools/
    registry.py     # @tool decorator (with _internal=True filter); Phase 1 polish #12 wraps
                    # tool body with try/except → translates Python exceptions to JSON-RPC errors
    marshall.py     # Tier 1 type marshalling
    tools/
      smoke_tools.py         # editor.ping demo
      asset_tools.py         # Shared helpers for Phase 2 composites
      asset_composites.py    # Phase 2 — 6 Category D Python composites
      level_composites.py    # Phase 3 — 5 Category D Python composites (level/actor batch ops)
      blueprint_composites.py # Phase 4 — 1 Category C Python composite (bp.compile_all_dirty)
  Tests/
    smoke_ping.py                # Phase 1 14-subtest smoke
    smoke_phase2.py              # Phase 2 34-subtest smoke
    smoke_phase3.py              # Phase 3 wrapper — runs all 4 Days sub-suites and aggregates
    smoke_phase3_days_1_3.py     # Phase 3 — 12 level.* tools
    smoke_phase3_days_4_8.py     # Phase 3 — 20 actor.* tools (22 sub-tests)
    smoke_phase3_days_9_10.py    # Phase 3 — 8 component.* tools
    smoke_phase3_days_11_14.py   # Phase 3 — 5 composite tools (9 sub-tests)
    smoke_phase4.py              # Phase 4 wrapper — runs all 3 Days sub-suites and aggregates
    smoke_phase4_days_1_5.py     # Phase 4 — 6 bp.* read tools
    smoke_phase4_days_6_10.py    # Phase 4 — 6 bp.* writes + bp.compile + bp.compile_all_dirty
    smoke_phase4_days_11_15.py   # Phase 4 — 9 material.* tools (reads + MIC writes + create + diagnostic)
    smoke_phase5.py              # Phase 5 wrapper — runs all 4 chunk sub-suites and aggregates
    smoke_phase5_pie.py          # Phase 5 Chunk A — 10 pie.* tools
    smoke_phase5_editor.py       # Phase 5 Chunk B — 10 editor.* + pie.screenshot_to_disk
    smoke_phase5_chunk_c.py      # Phase 5 Chunk C — 5 tools (umg + niagara + physics)
    smoke_phase5_sequencer.py    # Phase 5 Chunk D — 5 sequencer.* tools
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

## Phase 4 tool catalogue (23 user-visible tools + 1 hidden internal)

Breakdown: **13 C++ bp.*** + **1 Python composite bp.*** + **9 C++ material.*** = 23 user-visible
tools, plus **1 internal hidden** `bp._compile_all_dirty_internal` Lane B submitter used by the
Python composite.

All 23 tools are **Lane A** (game thread). Writes refuse PIE with `-32027`; reads are PIE-safe
(assets are shared between editor and PIE worlds). No World Partition check (asset-namespace
tools don't traverse map data).

### Category A — Blueprint reads (6 tools, all Lane A, no PIE guard)

```
bp.exists                 → {exists, generated_class_path, parent_class_path, is_data_only}
bp.list_variables         → {variables[{name, pin_type, default_value, category_group, ...}], next_page_token, total_known}
bp.get_variable           → {variable: { name, pin_type, default_value, replicated, ... }}
bp.list_functions         → {functions[{name, category, access_specifier, signature{inputs, outputs}, ...}], next_page_token, total_known}
bp.get_function           → {function: {..., local_variables[], execution_path_node_count}}
bp.list_nodes_in_function → {nodes[{node_guid, class, title, pins[{name, direction, pin_type, connected_to[]}]}], next_page_token, total_known}
```

Pagination is sentinel-cursor (FMCPPageCursor) over a stable sort by variable name / function name
/ node GUID string. Mid-pagination blueprint swap → `-32015 StaleCursor`. Unsupported pin types
(e.g. PC_Verse, future Epic-added categories) fail fast with `-32032 PinTypeUnsupported` rather
than silently coercing to a lossy PC_String fallback (D4).

### Category B — Blueprint writes (6 tools, all Lane A, PIE-guarded)

```
bp.add_variable          → {added, variable_name}                        edit-const-gate carve-out
bp.remove_variable       → {removed, was_present}                        idempotent
bp.change_variable_type  → {changed, prior_pin_type, warning}            warning text describes invalidation risk
bp.add_function          → {added, function_name}                        UserDefinedPins for inputs/outputs
bp.remove_function       → {removed, was_present}
bp.reparent              → {reparented, prior_parent, lost_variables[], lost_functions[]}
                                                                          experimental + confirm_dangerous=true gate
```

`bp.add_variable` deliberately SKIPS the standard edit-const 3-flag gate (CLAUDE D7 carve-out) —
new BP variables ship with `CPF_DisableEditOnInstance | CPF_BlueprintVisible | CPF_Edit` by
default, so applying the gate to the freshly-created variable would false-positive every add.
The gate is reserved for the future `bp.set_variable_default` tool.

`bp.reparent` requires `args.confirm_dangerous=true` (literal bool); omission returns
`-32033 ReparentUnsafe`. The frozen advisory text mentions "confirm_dangerous" + "may invalidate
variables/functions inherited from prior parent class". Every successful reparent emits a Display
log line with old/new parent paths + lost-member counts.

### Category C — Blueprint build (2 tools)

```
bp.compile          → {compiled, errors[], warnings[], duration_ms, status}      Lane A sync, PIE-guarded
bp.compile_all_dirty → {job_id}  → bp._compile_all_dirty_internal               Lane B submitter + GT body
                                                                                  ↓
                                                                                 (poll job.result)
                                                                                  ↓
                                                                                 {compiled, succeeded, failed[{path, errors[]}], duration_ms}
```

`bp.compile`'s `fail_on_error=true` (default false) returns `-32030 KismetCompilationError`
embedding the same `{errors, warnings, duration_ms, status}` payload, so AI strict-mode callers
can short-circuit without re-running.

`bp.compile_all_dirty` is the lone Phase 4 async composite. Python wrapper validates
`scope_paths` (non-empty `/Game/...` list — empty raises `ValueError` → `-32602`). Cooperative
cancel cadence is 16 BPs (lower than Phase 3's 256 because compile is heavier). Failure
aggregation policy (D1): continue + aggregate — does NOT abort on first failure. AI workflow
sees the full failure surface in one round-trip.

### Category D — Material reads (2 tools, all Lane A, no PIE guard)

```
material.list_parameters → {parameters: {scalar[{name, default, value, group}], vector[...], texture[{name, default_path, value_path, group}], static_switch[{name, default, value}]},
                            source_class, next_page_token?, total_known}
material.get_parameter   → {found, type, value, default, group}
```

`material.list_parameters` paginates over the FLATTENED list (scalar → vector → texture →
static_switch, sorted lex within each category). `material.get_parameter` auto-detects type via
4-way search (scalar → vector → texture → static_switch — first match wins); `parameter_type`
arg short-circuits to a single category. Missing parameter → `-32036 ParameterNotFound`.

`group` field is empty string in Phase 4 — `FMaterialParameterInfo` does not surface a
per-parameter group through the public UE 5.7 API. Marked optional in the wire schema; a future
material.* extension may populate via UMaterialEditorInstanceConstant walk.

### Category E — Material writes (4 tools, all Lane A, PIE-guarded, MIC-only)

```
material.set_scalar_param   → {applied, prior_value}
material.set_vector_param   → {applied, prior_value: {r,g,b,a}}
material.set_texture_param  → {applied, prior_value: <path|"">}
material.set_static_switch  → {applied, prior_value, recompile_triggered: true, recompile_already_pending: bool}
```

All 4 enforce MIC-only writes via `FMCPMaterialUtils::LoadMICByPath`. Base UMaterial paths
return `-32034 MaterialClassMismatch` with the standard advisory: "expected
UMaterialInstanceConstant ... mutating base UMaterial requires graph edits (out of Phase 4 scope;
future Phase 7 may add material.edit_node)".

Every write site applies the canonical 3-flag edit-const gate
(`CPF_EditConst | CPF_BlueprintReadOnly | CPF_DisableEditOnInstance`) on the override-array
FProperty; gate bypass via `args.bypass_readonly=true`. Writes are wrapped in
`FMCPWritePropertyScope` (RAII Pre/Modify/Transaction/Post).

`material.set_static_switch` additionally checks
`GShaderCompilingManager->GetNumRemainingJobs() < 1000` (CVar `mcp.material.shader_queue_soft_limit`
reserved for future tunability); queue saturation → `-32035 ShaderRecompilePending`. The result
carries `recompile_triggered=true` AND `recompile_already_pending: bool` so callers can decide
whether the recompile is a fresh trigger or appending to existing work.

### Category F — Material create + diagnostic (3 tools, all Lane A)

```
material.create_instance      → {created, mic_path}                      PIE-guarded
material.is_shader_compiling  → {compiling: bool, remaining_jobs: int}   no PIE guard
material.get_compile_errors   → {has_errors, errors[], warnings[]}       no PIE guard
```

`material.create_instance` uses `UMaterialInstanceConstantFactoryNew` + `IAssetTools::CreateAsset`.
Conflict check via `FPackageName::DoesPackageExist(dest_path)` → `-32014 PathInUse` (D10 — reuses
Phase 2 error code). Caller can `cb.delete` then retry or pick new path. Post-create the asset
gets `SetParentEditorOnly(ParentMaterial)` + `PostEditChangeProperty` so it's immediately usable
for parameter overrides on the same tick.

`material.is_shader_compiling` is a trivial pass-through of `GShaderCompilingManager` state — no
mutation, safe under any editor condition. `material.get_compile_errors` walks the MIC parent
chain to the base UMaterial then reads `FMaterialResource::GetCompileErrors()` from
`Material->GetMaterialResource(GMaxRHIShaderPlatform)`. Warnings array is reserved (UE 5.7's
FMaterialResource does not surface compile-time warnings separately).

### Phase 4 error codes (-32030..-32037)

8 new error codes were added in Phase 4:

```
-32030 KismetCompilationError    bp.compile fail_on_error=true — compile produced errors (same payload embedded)
-32031 BlueprintTypeMismatch     bp.* — path resolved to non-UBlueprint asset
-32032 PinTypeUnsupported        bp.* variable/function pin IO — PC_* category not handled (fail-fast per D4)
-32033 ReparentUnsafe            bp.reparent — caller omitted confirm_dangerous=true
-32034 MaterialClassMismatch     material.* — path resolved to wrong material-class family (e.g. UMaterial for a write)
-32035 ShaderRecompilePending    material.set_static_switch — queue at or above soft cap (default 1000 jobs)
-32036 ParameterNotFound         material.{get,set}_*_param — parameter name not on the resolved material
-32037 VariableNotFound          bp.{get,remove,change}_variable / bp.{get,remove}_function — name missing
```

### Phase 4 example: MIC scalar override + shader recompile poll

```jsonc
// 1. Read current value
{"id":"r1","kind":"call_function","method":"material.get_parameter",
 "args":{"material_path":"/Game/MyMaterials/MI_HeroLighting", "parameter_name":"Brightness"}}
// → {"ok":true, "result":{"found":true, "type":"scalar", "value":1.0, "default":1.0, "group":""}}

// 2. Write a new value
{"id":"w1","kind":"call_function","method":"material.set_scalar_param",
 "args":{"material_path":"/Game/MyMaterials/MI_HeroLighting", "parameter_name":"Brightness", "value":2.5}}
// → {"ok":true, "result":{"applied":true, "prior_value":1.0}}

// 3. Flip a static switch — triggers async shader recompile
{"id":"s1","kind":"call_function","method":"material.set_static_switch",
 "args":{"material_path":"/Game/MyMaterials/MI_HeroLighting", "parameter_name":"UseAlbedoMask", "value":true}}
// → {"ok":true, "result":{"applied":true, "prior_value":false,
//                          "recompile_triggered":true, "recompile_already_pending":false}}

// 4. Poll until shaders done
{"id":"c1","kind":"call_function","method":"material.is_shader_compiling", "args":{}}
// → {"ok":true, "result":{"compiling":true, "remaining_jobs":47}}
// (poll again until compiling=false)
```

### Phase 4 example: async batch-compile

```jsonc
// 1. Submit (Python wrapper handles ValueError → -32602 for empty scope_paths)
{"id":"bc1","kind":"call_function","method":"bp.compile_all_dirty",
 "args":{"scope_paths":["/Game/Characters"], "fail_fast":false}}
// → {"ok":true, "result":{"job_id":"abc-def-..."}}

// 2. Poll
{"id":"bc2","kind":"call_function","method":"job.result",
 "args":{"job_id":"abc-def-...", "wait_timeout_s":30}}
// → {"ok":true, "result":{"state":"Succeeded",
//      "result":{"compiled":42, "succeeded":40, "failed":[{"path":"/Game/.../BP_X", "errors":["..."]}],
//                "duration_ms":12345.6}}}
```

## Phase 5 tool catalogue (30 user-visible tools)

Breakdown: **10 pie.*** + **9 editor.*** + **1 pie.screenshot_to_disk** (lives in EditorTools but
exposed under pie.* for surface coherence) + **2 umg.*** + **1 niagara.*** + **2 physics.*** +
**5 sequencer.*** = 30 user-visible tools. No new internal hidden handlers — Chunk D's
``sequencer.list_cinematics`` does the AR enumeration inline via C++ rather than spawning a Python
composite (asset count is bounded; the composite-async pattern doesn't apply).

All 30 tools are **Lane A** (game thread). Chunks A/B/D are exclusively reads or simple
mutations that finish in one tick. Phase 5 introduces a NEW PIE-guard polarity (the inverse of
Phase 3's PIEActive guard).

### Inverse PIE-guard pattern (NEW with Phase 5)

Phase 3 mutators ban PIE: editor-world writes refuse with ``-32027 PIEActive`` when
``GEditor->PlayWorld != nullptr``. Phase 5's ``pie.*`` family is the SYMMETRIC OPPOSITE: every
``pie.*`` tool (except ``pie.start`` itself and the always-callable ``pie.is_running``) refuses
with ``-32038 PIENotActive`` when no PIE is running. The frozen message text steers callers to
``pie.start`` or the appropriate ``editor.*`` tool:

```
"PIE is not running; pie.start first (or use editor.* tools for editor-world ops)"
```

Smoke tests assert the substrings ``"PIE is not running"`` AND ``"pie.start"`` AND ``"editor.*"``.

### Chunk A — PIE lifecycle + introspection (10 tools, all Lane A)

```
pie.start                  → {started, already_running}        always callable
pie.stop                   → {stopped}                          PIE-required
pie.pause                  → {paused, was_already_paused}       PIE-required
pie.resume                 → {resumed}                          PIE-required
pie.step_frame             → {stepped}                          PIE-required (paused only)
pie.console_exec           → {executed, output: <string>}       PIE-required
pie.is_running             → {running, paused, world_kind}      always callable (probe)
pie.get_player_controller  → {pc_actor_guid, pc_path, ...}      PIE-required
pie.get_pawn               → {pawn_actor_guid, pawn_path}       PIE-required
pie.focus_actor            → {focused, actor_path}              PIE-required
```

``pie.console_exec`` uses ``FStringOutputDevice`` to capture ``World->Exec`` output. Many commands
(``stat fps``, ``r.SetRes 1920x1080``) write to the HUD/viewport rather than the output device, so
``output`` may be empty even on ``executed=true``. Surface as best-effort.

``pie.focus_actor`` accepts both editor + PIE actor IDs; if the actor is in the PIE world and PIE
is active it uses ``PC->SetViewTargetWithBlend``, else falls through to
``GEditor->MoveViewportCamerasToActor``.

### Chunk B — Editor utilities (9 editor.* + 1 pie.screenshot, all Lane A)

```
editor.viewport_screenshot          → {base64, width, height, format}     in-memory base64 PNG/JPG
editor.viewport_screenshot_to_disk  → {path, bytes, width, height}        path-sandboxed disk write
pie.screenshot_to_disk              → {path, bytes, width, height}        PIE-required (lives here for cohesion)
editor.get_camera                   → {location, rotation, fov, ortho_width}
editor.set_camera                   → {set: true}
editor.get_selection                → {actors[], components[]}
editor.set_selection                → {selected_count}                    capped at 200 (-32017)
editor.show_message                 → {shown: true}                        FSlateNotificationManager toast
editor.current_world                → {world_path, world_name, pie_active}
editor.tick_once                    → {ticked: true}                       advanced — re-entrancy hazard
```

Screenshot family supports ``format`` ∈ {png, jpg} + ``quality`` ∈ [1, 100] (JPG only).
Width/height clamped to [32, 2048]. Path-based outputs go through ``FMCPPathSandbox::Resolve`` →
``-32013 PathEscape`` on whitelist miss. Default output path is
``Saved/Screenshots/MCP/{timestamp}.{ext}``.

``editor.set_selection`` enforces a hard cap of 200 actor IDs per call (matches the Phase 2
``asset.batch_metadata`` precedent). Larger sets land as ``-32017 InputTooLarge``.

``editor.tick_once`` is marked advanced — calling ``GEditor->Tick(DT, false)`` mid-dispatch can
re-entrantly drain the Lane-A queue. Recommended for headless / scripted-test usage only.

### Chunk C — UMG + Niagara + Physics (5 tools, all Lane A, all reads)

```
umg.list_widgets         → {widgets[{name, class, parent, is_variable}]}     editor-only WBP walk
umg.get_widget_property  → {value, type}                                       CDO-scoped (design-time)
niagara.list_parameters  → {user_params[], system_params[], emitter_params[]}  decoded user defaults
physics.line_trace       → {world, world_kind, ignored_count, hit, hits[]}    Chaos line trace
physics.sweep_capsule    → {world, world_kind, ignored_count, hit, hits[]}    Chaos capsule sweep
```

``umg.list_widgets`` walks the editor-only ``UWidgetBlueprint::WidgetTree`` via ``ForEachWidget``
(stops at foreign widget-tree boundaries — matches the editor's Widget Hierarchy panel). Each
entry: ``name`` (UWidget::GetName), ``class`` (UClass path), ``parent`` (parent's name or null
for root), ``is_variable`` (UWidget::bIsVariable). Root widget has ``parent=null``.

``umg.get_widget_property`` is **CDO-scoped** — returns design-time defaults, NOT runtime instance
state. After resolving the widget by name it delegates to
``FMCPReflection::ResolvePropertyPath`` + ``ReadPropertyValueAt`` (same pipeline as
``marshall.read_property`` / ``actor.get_property``). Widget-not-found → ``-32039 WidgetNotFound``
(message embeds up to 16 top-level widget names for typo recovery).

``niagara.list_parameters`` decodes **user** parameter binary values (float/int/bool/Vec2/Vec3/
Vec4/Quat/LinearColor/Position); ``system_params`` and ``emitter_params`` emit name+type only.
Unsupported value types emit a typed sentinel string ``"<unsupported: <typename>>"`` (never null).

``physics.line_trace`` and ``physics.sweep_capsule`` operate on the UE **Chaos** system (NOT Jolt/
Barrage). Flecs/Barrage entities (FatumGame projectiles, ISM-rendered items) are INVISIBLE to
these traces. A future ``flecs.*`` trace family is reserved for Jolt queries (e.g. wrapping
``UFlecsArtillerySubsystem::CastRayAllHits`` from the Barrage plugin). PIE world takes precedence
over the editor world when both are available — response ``world`` field surfaces which.

Collision channels accepted: Visibility (default), Camera, WorldStatic, WorldDynamic, Pawn,
PhysicsBody, Vehicle, Destructible. Unknown channel → ``-32041 InvalidCollisionChannel`` with the
accepted-list hint in the message body. ``ignore_actors`` array is best-effort — unresolved
entries are SKIPPED with a Verbose log (NOT a hard error).

### Chunk D — Sequencer (5 tools, all Lane A, all reads)

```
sequencer.list_cinematics   → {sequences[{path, name, duration_secs, frame_rate}]}     /Game default scope
sequencer.get_tracks        → {master_tracks[], possessables[], spawnables[]}          MovieScene walk
sequencer.get_camera_cuts   → {cuts[{start_frame, end_frame, camera_binding}], frame_rate}
sequencer.get_keyframes     → {keys[{channel, time, value, interp}], section_range, frame_rate, supported_types}
sequencer.get_current_time  → {time_seconds, frame, tick, frame_rate, tick_rate, sequence_path, world}
```

``sequencer.list_cinematics`` iterates ``ULevelSequence`` assets via FARFilter restricted to the
``LevelSequence.LevelSequence`` class path. Each entry loads the asset to read
``MovieScene->GetPlaybackRange()`` + ``GetDisplayRate()`` for duration/rate. Cold-loaded sequences
are loaded on first query (intentional — cinematic counts are bounded by content scope, O(10-100)
typically). The plan-mandated default scope is ``["/Game"]``.

``sequencer.get_tracks`` splits the MovieScene's tracks across three arrays:
  - ``master_tracks`` — top-level tracks NOT bound to an object (camera cuts, audio, fade, sub-
    sequences). The ``CameraCutTrack`` is folded in here even though UE stores it in its own slot
    (``GetCameraCutTrack()``) — surface coherence wins over the underlying storage layout.
  - ``possessables`` — name+object_class+binding_guid plus a nested ``tracks`` array per binding.
  - ``spawnables`` — same shape as possessables but for spawnable bindings (objects the sequence
    creates rather than possesses).

``sequencer.get_keyframes`` uses a dotted ``track_path``: ``"MasterName.SectionIdx"`` or
``"BindingName.TrackName.SectionIdx"``. Channel value decoding covers **float, double, bool,
integer** (Phase 5 D1 scope; matches plan §F-Sequencer L1287). Other channel types (event, byte,
object, string, sub-section, blendable transform) emit a sentinel ``{channel, time:0, value:null,
interp:"auto"}`` entry per channel so the wire schema stays uniform — the ``supported_types`` array
in the response declares which value-types ARE decoded. Per-key ``interp`` is one of
``linear|constant|cubic|auto``.

``sequencer.get_current_time`` reads the active editor Sequencer playhead via
``ULevelSequenceEditorBlueprintLibrary::GetCurrentLevelSequence`` + ``GetGlobalPosition``. Returns
``time_seconds`` (continuous), ``frame`` (display rate), AND ``tick`` (tick resolution) so callers
don't need to round-trip the conversion. ``world`` is ``"pie"`` or ``"editor"`` reflecting
``IsPIEActive()`` at call time. When no Sequencer tab is open OR the active sequence is not a
``ULevelSequence`` → ``-32042 NoActiveSequencer``.

All ``time`` / ``frame`` fields in Sequencer results are emitted in TICKS at the MovieScene's
``GetTickResolution()`` — the embedded ``frame_rate`` / ``tick_rate`` objects carry rational
``{numerator, denominator, decimal}`` so callers can convert exactly to seconds OR to display-rate
frames.

### Phase 5 error codes (-32038..-32044)

7 new error codes were added in Phase 5:

```
-32038 PIENotActive             pie.* tool called outside PIE; frozen message points at pie.start / editor.*
-32039 WidgetNotFound            umg.get_widget_property — widget_name not found in WidgetTree; hint embeds names
-32040 NiagaraParameterNotFound  RESERVED — reserved for future niagara.set_user_param (Phase 6+)
-32041 InvalidCollisionChannel   physics.* — channel string didn't map to ECollisionChannel; accepted-list hint
-32042 NoActiveSequencer         sequencer.get_current_time — no Sequencer tab open OR non-LevelSequence active
-32043 TrackNotFound             sequencer.get_keyframes — track_path didn't resolve to a track
-32044 SectionIndexOOB           sequencer.get_keyframes — section index segment exceeds GetAllSections().Num()
```

The `-32038 PIENotActive` message is **frozen** verbatim (kMCPMessagePIENotActive) — smoke tests
assert substrings ``"PIE is not running"``, ``"pie.start"``, ``"editor.*"``. Mirror of Phase 3's
frozen ``kMCPMessagePIEActive`` text.

### Phase 5 example: PIE start + step + screenshot + stop

```jsonc
// 1. Lifecycle
{"id":"p1","kind":"call_function","method":"pie.start", "args":{"mode":"selected_viewport"}}
// → {"ok":true, "result":{"started":true, "already_running":false}}

{"id":"p2","kind":"call_function","method":"pie.pause", "args":{}}
// → {"ok":true, "result":{"paused":true, "was_already_paused":false}}

{"id":"p3","kind":"call_function","method":"pie.step_frame", "args":{}}
// → {"ok":true, "result":{"stepped":true}}

// 2. Screenshot the paused frame
{"id":"p4","kind":"call_function","method":"pie.screenshot_to_disk", "args":{"width":640, "height":360}}
// → {"ok":true, "result":{"path":".../Saved/Screenshots/MCP/...png", "bytes":12345, "width":640, "height":360}}

// 3. Console command (output may be empty for HUD-only commands)
{"id":"p5","kind":"call_function","method":"pie.console_exec", "args":{"command":"stat fps"}}
// → {"ok":true, "result":{"executed":true, "output":""}}

{"id":"p6","kind":"call_function","method":"pie.stop", "args":{}}
// → {"ok":true, "result":{"stopped":true}}
```

### Phase 5 example: Sequencer keyframe inspection

```jsonc
// 1. Find cinematics
{"id":"s1","kind":"call_function","method":"sequencer.list_cinematics", "args":{"scope_paths":["/Game/Cinematics"]}}
// → {"ok":true, "result":{"sequences":[{"path":"/Game/Cinematics/LS_Intro", "name":"LS_Intro",
//                                        "duration_secs":12.5,
//                                        "frame_rate":{"numerator":30, "denominator":1, "decimal":30.0}}]}}

// 2. Enumerate tracks
{"id":"s2","kind":"call_function","method":"sequencer.get_tracks", "args":{"sequence_path":"/Game/Cinematics/LS_Intro"}}
// → {"ok":true, "result":{"master_tracks":[{"name":"CameraCutTrack", "class":"/Script/...", "section_count":2}],
//                          "possessables":[{"name":"HeroActor", "binding_guid":"...",
//                                            "tracks":[{"name":"Transform", "class":"/Script/MovieSceneTracks.MovieScene3DTransformTrack",
//                                                       "section_count":1}]}],
//                          "spawnables":[]}}

// 3. Read keyframes from the transform track
{"id":"s3","kind":"call_function","method":"sequencer.get_keyframes",
 "args":{"sequence_path":"/Game/Cinematics/LS_Intro", "track_path":"HeroActor.Transform.0"}}
// → {"ok":true, "result":{"keys":[{"channel":"Location.X", "time":0, "value":0.0, "interp":"cubic"},
//                                   {"channel":"Location.X", "time":24000, "value":100.0, "interp":"cubic"}, ...],
//                          "section_range":{"has_lower":true, "has_upper":true, "start":0, "end":300000},
//                          "frame_rate":{"numerator":24000, "denominator":1, "decimal":24000.0},
//                          "supported_types":["float", "double", "bool", "integer"]}}
```

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
| `smoke_phase4.py` | **wrapper** | Runs all 3 Phase 4 sub-suites below and aggregates pass/fail |
| `smoke_phase4_days_1_5.py` | 14 | Phase 4 Days 1-5 — 6 `bp.*` read tools (exists/list/get) + pagination |
| `smoke_phase4_days_6_10.py` | 19 | Phase 4 Days 6-10 — 6 `bp.*` writes + `bp.compile` + `bp.compile_all_dirty` |
| `smoke_phase4_days_11_15.py` | 17 | Phase 4 Days 11-15 — 9 `material.*` tools (reads + MIC writes + create + diagnostic + boundary) |
| `smoke_phase5.py` | **wrapper** | Runs all 4 Phase 5 chunk sub-suites below and aggregates pass/fail |
| `smoke_phase5_pie.py` | 16+ | Phase 5 Chunk A — 10 `pie.*` tools (lifecycle + introspection + actor identity + PIE-required guard) |
| `smoke_phase5_editor.py` | 13 | Phase 5 Chunk B — 10 `editor.*` + `pie.screenshot_to_disk` |
| `smoke_phase5_chunk_c.py` | 11 | Phase 5 Chunk C — 5 tools (`umg.*` + `niagara.list_parameters` + `physics.*`) |
| `smoke_phase5_sequencer.py` | 10 | Phase 5 Chunk D — 5 `sequencer.*` tools (list + tracks + cuts + keyframes + current_time) |

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
