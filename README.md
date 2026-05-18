# UnrealMCPBridge

Editor-only plugin that exposes Unreal Editor tools to an external MCP
(Model Context Protocol) server over TCP. Together with `mcp_server/` in the
project root it lets an AI client (Claude Desktop, Claude Code, etc.) drive
the editor for asset/level/blueprint authoring tasks.

## Status

**Phase 2 complete — 31 user-visible tools + 5 internal C++ handlers (post-Hotfix-3).**

| Phase | Tools | Surface |
|---|---|---|
| Phase 1 | 16 | marshall.* (4), job.* (5), log.* (3), tools.list, editor.* (3) |
| Phase 2 | 31 | asset.* (13 C++ + 6 Python composites = 19), cb.* (12) — plus 5 internal hidden handlers |
| **Total** | **47** | (user-visible; 36 total handler registrations counting hidden internals) |

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
        AssetRegistryTools.h/.cpp   # Category A — 13 AR reads (all Lane A post-hotfix; was 10 Lane B)
        ContentBrowserTools.h/.cpp  # Category B — 12 CB writes (all Lane A; list_folders was Lane B)
        AssetCompositeTools.h/.cpp  # Category D — 5 internal C++ helpers (all Lane B, all async-job)
      Utils/
        MCPAssetPathUtils.h/.cpp    # Path canonicalisation
        MCPARFilterParser.h/.cpp    # FARFilter JSON ↔ struct + hash
        MCPPageCursor.h/.cpp        # Opaque cursor encode/decode
        MCPPathSandbox.h/.cpp       # Disk path whitelist guard
  Content/Python/MCPTools/
    registry.py     # @tool decorator (with _internal=True filter)
    marshall.py     # Tier 1 type marshalling
    tools/
      smoke_tools.py       # editor.ping demo
      asset_tools.py       # Shared helpers for composites
      asset_composites.py  # 6 Category D Python composites
  Tests/
    smoke_ping.py        # Phase 1 14-subtest smoke
    smoke_phase2.py      # Phase 2 34-subtest smoke
    lane_b_spike.py      # Day 0 audit harness
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

Two end-to-end harnesses live in `Tests/`:

- `smoke_ping.py` — 14 sub-tests covering Phase 1 (`editor.*`, `marshall.*`,
  `job.*`, `log.*`, `tools.list`).
- `smoke_phase2.py` — 34 sub-tests covering Phase 2 (every Category A/B/D tool
  exercised with a positive + negative case).

Both require the editor running with the bridge listener up. Run as:
```
python Tests/smoke_phase2.py [--host 127.0.0.1] [--port 30020]
```

Pre-test data prep (one-time, optional — sub-tests with missing assets log
SKIP rather than fail):
1. `Content/MCPTest/PhaseTwo/DA_PhaseTwoTest.uasset` — duplicate of any
   `UFlecsEntityDefinition`. Required for sub-tests 1, 3, 7-11, 13, 22, etc.
2. `Plugins/UnrealMCPBridge/Tests/test_assets/test_texture.png` — 32x32
   magenta PNG for sub-test 23 (`cb.import`).
3. `Plugins/UnrealMCPBridge/Tests/test_assets/test_mesh.fbx` — minimal cube
   FBX for sub-test 27 (`cb.bulk_import`). MAY BE SKIPPED.
