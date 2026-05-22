---
title: Roadmap
layout: default
nav_order: 7
---

# Roadmap

## Completed waves

| Wave | Date | Tools added | Focus |
|---|---|---|---|
| Phases 1-6 | 2026-04 | 169 baseline | Core infra (TCP, dispatch, marshalling, log streaming) + Asset/CB/Level/Actor/Component/BP/Material/UMG/PIE/Editor/Niagara-read/Physics/Sequencer-read/SC/Test/Config/Logs/LiveCoding |
| Wave A | 2026-05 | +8 → 177 | PIE functional testing + Stats |
| Asset import surface | 2026-05 | +3 → 180 | cb.reimport + asset.create + cb.list_supported_formats |
| UMG surface | 2026-05 | +10 → 190 | Full Widget Blueprint authoring |
| Wave B Tier 3 | 2026-05 | +3 → 193 | Niagara writes (set_user_param, create_emitter, set_emitter_enabled) |
| Wave B Tier 4 | 2026-05 | +2 → 195 | BP graph nodes (add_node, connect_pins) |
| Data Asset creation | 2026-05 | +2 → 197 | asset.create_data_asset, asset.list_data_asset_classes |
| UFunction reflection | 2026-05 | +2 → 199 | bp.call_function, bp.list_class_functions |
| Phase 6 polish | 2026-05 | rebal. → 207 | Final tally after consolidations |
| **Wave C** | 2026-05 | +16 → 223 | Sequencer writes (5), Animation (5), Audio (3), WorldPartition (3) |
| **Wave D** | 2026-05 | +26 → 233 | gameplaytag (4), debug (6), mesh (5), folder (4), level_streaming (4), transform (3) |
| **Wave E** | 2026-05 | +21 → 254 | viewport (4), niagara runtime (3), hierarchy (3), texture (4), input (4), subsystem (3) |
| **Wave F** | 2026-05 | +20 → **274** | Full BP CRUD — graph editing (5), function signatures (4), components/SCS (4), interfaces (3), metadata+comments (4) |
| **Wave G** | 2026-05 | +23 → 297 | physics writes (4), mat graph nodes (4), navmesh (4), animbp (4), render (4), engine (3) |
| **Wave H** | 2026-05 | +20 → 317 | data_table (4), curve (4), data_validation (3), screenshot ext (3), thumbnail (3), cook (3) |
| **Wave I** | 2026-05 | +25 → 342 | Asset hygiene + pipeline polish — package (5), soft_ref (4), collision (4), mat_inst (5), sequencer_ext (3), landscape (4). **First parallel-agent wave; MCPSurfaceRegistry auto-register infra introduced.** |
| **Wave J** | 2026-05 | +19 → **361** | AI runtime + authoring — ai.bt (4), ai.bb (3), ai.controller (3), ai.eqs (3), ai.perception (3), ai.crowd (3). **First wave fully on auto-register; agents touched ONLY their own files.** |
| **Refactor** | 2026-05-22 | 0 → **361** (no new tools) | 10-stream autonomous refactor — see "Refactor 2026-05" below |

## Refactor 2026-05 (autonomous, 10-stream)

| Phase | Commit | Upstream | Win |
|---|---|---|---|
| 1: helper infra + tests-in-repo | `b2fd19d` | `5c274ba` | 4 helper headers (688 LOC) + 47 tests moved into Plugins/UnrealMCPBridge/Tests/python/ |
| 2: Wave I+J migration (12 surfaces) | `8e5384c` | `785a0b1` | -754 raw LOC across 12 surfaces |
| 3: 51 surfaces migration | `89ff08c` | `a50f289` | -2 423 raw LOC across 51 surfaces |
| 4: -32015 disambiguation | `21e6403` | `d24cb5c` | New `-32058 OperationFailed` code |
| 5-lite: 51 surfaces auto-register | `f286cc4` | `c62fa3b` | `UnrealMCPBridge.cpp` 865 → 273 LOC (-592). All 63 surfaces self-register via `MCP_REGISTER_SURFACE` |
| 5: module split | `9f2786c` | `a70b1d6` | UnrealMCPBridgeCore (~10kLOC) + UnrealMCPBridge (~53kLOC) |
| 4.2-a: Lane B promote (4 tools) | `7e306c0` | `11f4ce1` | stats + engine.* readers to worker pool |
| 4.2-b: Lane B promote (7 tools) | `24ad2e8` | `1a606ca` | log + cfg readers to worker pool |
| 4.2-c: Lane B with FCriticalSection (7 tools) | `ed49fe1` | `45e3220` | collision + test readers with module-scoped locks |
| Stream 4: JSON DSL migration | `fff147d` | `30bbcc9` | ~231 tools across 62 surfaces use `FMCPJsonBuilder` fluent DSL |

**Cumulative metrics**: Plugin LOC 66 065 → ~60 800 (−5 265). Lane B 17 → 35 (5.0% → **10.5%**). Tool count unchanged (361). Build GREEN at every phase. All 63/63 surfaces on shared helpers + auto-register + JSON DSL. Error catalog -32004..-32058.

## Planned — Wave G (was next; now COMPLETE — see above)

**Focus: most actual tools for live game dev workflow.** ~23 tools, 274 → 297.

| # | Surface | Tools | Why |
|---|---|---|---|
| G1 | `physics.*` writes | 4 | Runtime testing — apply_impulse / set_simulation / set_velocity / overlap_test |
| G2 | `mat.*` graph nodes | 4 | Material authoring (analog of bp.add_node) — add_expression / connect / set_parameter / delete |
| G3 | `navmesh.*` | 4 | AI navigation — list / rebuild / find_path / project_to_navmesh |
| G4 | `animbp.*` | 4 | Anim BP state machines — list / get_states / add_state / add_transition |
| G5 | `render.*` | 4 | Engine show flags, post-process, stat toggle |
| G6 | `engine.*` | 3 | Project info / GC / memory snapshot |

### G1 — physics.* writes

| Tool | Args | API |
|---|---|---|
| `physics.apply_impulse` | actor_path, impulse, world_or_local? | `UPrimitiveComponent::AddImpulse` |
| `physics.set_simulation` | actor_path, simulate, recurse? | `SetSimulatePhysics` |
| `physics.set_velocity` | actor_path, linear?, angular? | `SetPhysicsLinearVelocity` + `SetPhysicsAngularVelocity` |
| `physics.overlap_test` | location, radius, channel? | `UWorld::OverlapMultiByChannel` |

### G2 — mat.* graph nodes

| Tool | Args | API |
|---|---|---|
| `mat.add_expression` | material_path, expression_class, position?, parameter_name? | `NewObject<UMaterialExpression*>` + add to expressions |
| `mat.connect_expressions` | material_path, from_guid, from_output_idx, to_guid, to_input_name | `FExpressionInput.Connect` |
| `mat.set_expression_parameter` | material_path, expression_guid, property_name, value | `ImportText_Direct` on expression UPROPERTY |
| `mat.delete_expression` | material_path, expression_guid | Remove from `Material->Expressions` + cleanup connections |

### G3 — navmesh.*

| Tool | Args | API |
|---|---|---|
| `navmesh.list` | (no args) | `UNavigationSystemV1::GetMainNavData` + enumerate `ARecastNavMesh` |
| `navmesh.rebuild` | (no args) | `UNavigationSystemV1::Build()` |
| `navmesh.find_path` | start, end, agent_class? | `UNavigationSystemV1::FindPathSync` |
| `navmesh.project_to_navmesh` | location, search_extent? | `UNavigationSystemV1::ProjectPointToNavigation` |

### G4 — animbp.*

| Tool | Args | API |
|---|---|---|
| `animbp.list_state_machines` | anim_blueprint_path | Walk AnimGraph → find `UAnimGraphNode_StateMachine` |
| `animbp.get_states` | anim_bp_path, state_machine_name | Read `UAnimStateMachineGraph->Nodes` filtered to `UAnimStateNode` |
| `animbp.add_state` | anim_bp_path, sm_name, state_name, position?, anim_sequence_path? | `NewObject<UAnimStateNode>` + add to graph |
| `animbp.add_transition` | anim_bp_path, sm_name, from_state, to_state | `UAnimStateMachineGraph::CreateTransitionNode` |

### G5 — render.*

| Tool | Args | API |
|---|---|---|
| `render.list_show_flags` | (no args) | Enumerate `FEngineShowFlags` fields |
| `render.set_show_flag` | flag_name, enabled, viewport_index? | `FLevelEditorViewportClient::EngineShowFlags.SetSingleFlag` |
| `render.set_engine_stat` | stat_name, enabled | Console: `stat <name>` or `GEngine->SetEngineStat` |
| `render.set_post_process_volume_property` | volume_actor_path, property_path, value | `ImportText` on `UPostProcessVolume::Settings` |

### G6 — engine.*

| Tool | Args | API |
|---|---|---|
| `engine.get_info` | (no args) | `FApp::*`, `FEngineVersion`, `FPlatformProperties` |
| `engine.gc_collect` | force? | `CollectGarbage` |
| `engine.get_memory_snapshot` | include_breakdown? | `FPlatformMemory::GetStats` + `GMalloc->DumpAllocatorStats` |

## Future — Wave H+ candidates

Lower priority but in the backlog:

- `landscape.*` — heightmap painting, layer weight, foliage spawn
- `replication.*` — RPC config, replication graph
- `localization.*` — string tables, FText namespaces
- `pak.*` / `package.*` — cooking, packaging automation
- `niagara.* graph` — emitter module graph editing
- `pcg.*` — Procedural Content Generation (UE 5.3+)
- `controlrig.*` — IK posing
- `metasound.*` — DSP graph manipulation
- `data_validation.*` — run editor data validators
- `data_table.*` — DataTable read/write with row JSON
- `behavior_tree.*` — BT introspection + editing
