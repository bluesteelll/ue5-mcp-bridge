---
title: Tools reference
layout: default
nav_order: 4
has_children: true
---

# Tools reference

274 tools across 25+ namespaces. Each namespace has its own page with per-tool args + behaviour.

## Quick index

### Asset & content browser
- [`asset.*`](asset.html) (23) — search, list, references, dependents, create_data_asset, paginated
- [`cb.*`](cb.html) (17) — ContentBrowser — duplicate, delete, rename, move, save, reimport
- [`mesh.*`](mesh.html) (5) — static mesh list/info/LOD/material/duplicate
- [`texture.*`](texture.html) (4) — list/info/set_compression/generate_solid_color

### Level / Actor / Component
- [`level.*`](level.html) (14) — maps, current level, persistent actors, world settings, streaming state
- [`level_streaming.*`](level_streaming.html) (4) — streaming sub-level list/add/remove/set_loaded
- [`actor.*`](actor.html) (24) — spawn, destroy, properties, components, find by class/label/tag
- [`hierarchy.*`](hierarchy.html) (3) — actor attach/detach/list_children
- [`folder.*`](folder.html) (4) — actor outliner folder management
- [`transform.*`](transform.html) (3) — batch_set / snap_to_floor / align
- [`wp.*`](wp.html) (3) — WorldPartition partitioned-check, per-actor RuntimeGrid

### Blueprint (full CRUD after Wave F)
- [`bp.*`](bp.html) (49) — create/compile, variables, functions, graph nodes, components/SCS, interfaces, metadata, comments

### Materials & visual
- [`material.*`](material.html) (14) — material instance params, scalar/vector/texture, compile errors
- [`debug.*`](debug.html) (6) — DrawDebugLine/Sphere/Box/Arrow/Text + clear
- [`viewport.*`](viewport.html) (4) — list/get/set camera + focus_on_actor

### Cinematic & animation
- [`sequencer.*`](sequencer.html) (10) — LevelSequence tracks/cuts/keyframes + write tools
- [`anim.*`](anim.html) (5) — UAnimSequence/UAnimMontage list/create/sections/notifies/blend
- [`niagara.*`](niagara.html) (7) — FX systems — params, create, runtime spawn/list/stop
- [`audio.*`](audio.html) (3) — USoundCue, attenuation, sound class enumeration

### UI & input
- [`ui.*` / `umg.*`](umg.html) (10) — Widget Blueprints — read+write tree
- [`input.*`](input.html) (4) — Enhanced Input — IMC/IA enumeration, context bindings

### Runtime & PIE
- [`pie.*`](pie.html) (9) — PIE start/stop, input dispatch, screenshots
- [`editor.*`](editor.html) (6) — editor state, notifications, viewport screenshots
- [`physics.*`](physics.html) (2) — line trace, sweep capsule
- [`gameplaytag.*`](gameplaytag.html) (4) — list + query/mutate FGameplayTagContainer

### Reflection & subsystems
- [`marshall.*`](marshall.html) (4) — generic UProperty read/write/list
- [`subsystem.*`](subsystem.html) (3) — generic UE subsystem reflection

### Build & ops
- [`sc.*`](sc.html) (11) — Source Control checkout/revert/commit
- [`test.*`](test.html) (7) — Automation test runner
- [`config.*`](config.html) (6) — CVar / .ini editing
- [`logs.*`](logs.html) (5) — log subscription + stream
- [`livecoding.*`](livecoding.html) (3) — hot-reload trigger
- [`stats.*`](stats.html) (4) — engine + memory snapshots

### Meta
- [`tools.list`](meta.html#toolslist) — enumerate all registered handlers
- [`job.*`](meta.html#jobs) — async job system
- [`exec_python`](meta.html#exec-python) — arbitrary Python expression eval
