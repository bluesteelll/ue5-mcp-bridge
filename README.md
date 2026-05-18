# UnrealMCPBridge

Editor-only plugin that exposes Unreal Editor tools to an external MCP
(Model Context Protocol) server over TCP. Together with `mcp_server/` in the
project root it lets an AI client (Claude Desktop, Claude Code, etc.) drive
the editor for asset/level/blueprint authoring tasks.

## Status

**Phase 2 complete — 31 user-visible tools + 3 internal C++ Lane B handlers.**

| Phase | Tools | Surface |
|---|---|---|
| Phase 1 | 16 | marshall.* (4), job.* (5), log.* (3), tools.list, editor.* (3) |
| Phase 2 | 31 | asset.* (13 C++ + 6 Python composites = 19), cb.* (12) — plus 3 internal hidden handlers |
| **Total** | **47** | (user-visible; 34 total handler registrations counting hidden internals) |

11 of the 31 user-visible Phase 2 tools run on **Lane B** — the TCP listener thread
bypasses the game-thread queue entirely for ~16× latency improvement on AR queries
(see "Lane B contract" below). All 3 internal `asset._*` handlers are Lane B too.

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
        AssetRegistryTools.h/.cpp   # Category A — 12 Lane B reads + thumbnails
        ContentBrowserTools.h/.cpp  # Category B — 12 Lane A writes (1 Lane B list_folders)
        AssetCompositeTools.h/.cpp  # Category D — 3 internal C++ helpers
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
tools, plus **3 internal hidden** asset._*internal handlers (used by Python composites).

### Category A — Asset Registry queries (13 C++ tools, mostly Lane B)

```
asset.exists                  → {exists, asset_path_canonical}                    Lane B
asset.metadata                → {class, package, tags, size_disk, ...}            Lane B
asset.list                    → {assets[], next_page_token, total_known, ...}     Lane B
asset.find_references         → {referencers[], next_page_token, total_known}     Lane B
asset.find_dependents         → {dependents[], next_page_token, total_known}      Lane B
asset.search_by_class         → {matches[], next_page_token, total_known}         Lane B
asset.search_by_tag           → {matches[], next_page_token, total_known}         Lane B
asset.search_by_name          → {matches[], next_page_token, total_known}         Lane B
asset.get_class_hierarchy     → {chain[]}                                          Lane B
asset.get_outermost_package   → {package_path, on_disk}                            Lane B
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

### Category B — Content Browser write operations (12 tools, Lane A)

```
cb.create_folder      → {created, normalized_path}      idempotent
cb.rename             → {success, canonical_new_path}    FScopedTransaction
cb.save               → {saved}                          no transaction (saves not undoable)
cb.move               → {moved[], failed[]}              per-asset transaction (D4)
cb.duplicate          → {new_path}                       FScopedTransaction
cb.delete             → {deleted, redirector_left}       force=true Display-logged (Warning on depth-2)
cb.fix_redirectors    → {fixed_count, removed_count}    500-redirector hard cap
cb.list_folders       → {folders[]}                       LANE B (only Cat B Lane B tool)
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

### Category D — Python composites (6 user-visible + 3 internal C++)

```
asset.find_unused              → {unused[], scanned_count}      → asset._find_unused_internal (1 RT)
asset.size_report              → {top[], total_bytes}           → asset._size_report_internal (1 RT)
asset.batch_metadata           → {assets[], failed[]}            sync ≤200, loops asset.metadata
asset.batch_metadata_async     → {job_id}                        → asset._batch_metadata_internal (async)
asset.find_broken_references   → {broken[], scanned_count}      Python: list → find_dependents → exists
asset.find_duplicates_by_name  → {duplicates[]}                 Python: list per scope + groupby
```

Example:
```jsonc
{"id":"u1","kind":"call_function","method":"asset.find_unused",
 "args":{"package_paths":["/Game/Untracked"]}}
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
