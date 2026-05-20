---
title: Home
layout: default
nav_order: 1
description: "UE5 MCP Bridge — 274+ tools for scripted Unreal Engine 5.7 workflow over TCP"
permalink: /
---

# UE5 MCP Bridge
{: .fs-9 }

A Model Context Protocol (MCP) bridge plugin for **Unreal Engine 5.7** that exposes **274+ editor tools** over a single TCP connection, enabling scripted UE workflows from LLM agents (Claude, GPT, etc.) or any external automation tool.
{: .fs-6 .fw-300 }

[Get Started](getting-started.html){: .btn .btn-primary .fs-5 .mb-4 .mb-md-0 .mr-2 }
[View on GitHub](https://github.com/bluesteelll/ue5-mcp-bridge){: .btn .fs-5 .mb-4 .mb-md-0 }

---

## What is this?

`ue5-mcp-bridge` is a UE5 editor plugin that opens a TCP listener on `127.0.0.1:30020` and accepts newline-delimited JSON-RPC requests. Each request invokes a tool — a typed C++ handler that introspects/mutates editor state (assets, actors, Blueprints, materials, Niagara, sequencer, animations, audio, world partition, debug overlays, viewport, input mapping, subsystems, ...).

The plugin is the canonical source for **Lane A/B dispatch model**, **bidirectional pagination**, **scoped transactions**, **PIE-guarded mutators** — all the boilerplate to make external automation safe against UE's complex editor state.

## Highlights

- **274 user-visible tools** across 25+ namespaces — assets, BP, materials, ECS, sequencer, anim, audio, world partition, debug, viewport, niagara, input, subsystems, and more
- **Single TCP port** (default `30020`, loopback only) with newline-framed JSON-RPC
- **Lane A (game thread, OnEndFrame drain) + Lane B (worker pool)** — pure compute offloaded so UI tools never block
- **Full Blueprint CRUD** — create/compile, variables, functions, graph nodes, SCS components, interfaces, metadata, comments (Wave F)
- **Editor + PIE world resolution** — debug draws / Niagara spawns / sequencer queries pick the right world automatically
- **Stable error contract** — 60+ documented error codes (-32004 ObjectNotFound, -32027 PIEActive, etc.) for predictable client retries
- **Production-tested** — every surface has live smoke tests; ~600 PASS across all waves

## Architecture overview

```
External client (Claude / Python / etc.)
      ↓
TCP socket :30020 (newline-delimited JSON)
      ↓
FMCPServer (accept + parse)
      ↓
FMCPDispatchQueue
      ├─ Lane A (game thread, OnEndFrame drain) — UObject mutations
      └─ Lane B (worker threads, bThreadSafe=true) — pure compute
      ↓
Tool handler (cpp namespace function)
      ↓
FMCPResponse → newline JSON back to client
```

See [Architecture](architecture.html) for the deep dive.

## Quick example

```python
import socket, json

s = socket.create_connection(("127.0.0.1", 30020), timeout=5)

# Spawn a static mesh actor in editor world
req = {
  "id": "1",
  "kind": "call_function",
  "method": "actor.spawn",
  "args": {
    "class_path": "/Script/Engine.StaticMeshActor",
    "location": [0, 0, 100],
    "label": "MyTestCube"
  }
}
s.sendall((json.dumps(req) + "\n").encode())

# Read response (newline-terminated)
buf = b""
while b"\n" not in buf:
    buf += s.recv(8192)
print(json.loads(buf[:buf.index(b"\n")].decode()))
# → {"id":"1", "ok":true, "result":{"actor_path":"/Temp/Untitled_0.Untitled:PersistentLevel.MyTestCube"}}
```

## What's covered

| Namespace | Tools | Description |
|---|---|---|
| `level.*` | 14 | Maps, current level, persistent actors, world settings, streaming state |
| `actor.*` | 24 | Spawn/destroy, properties, components, find by class/label/tag, attach |
| `bp.*` | 49 | **Full BP CRUD** — create/compile, variables, functions, graph nodes, components, interfaces, metadata, comments |
| `mat.*` / `material.*` | 14 | Material instance params, scalar/vector/texture, compile state |
| `asset.*` | 23 | Search/list/refs/dependents/create_data_asset, paginated |
| `cb.*` | 17 | ContentBrowser — duplicate/delete/rename/move/save/reimport |
| `ui.*` / `umg.*` | 10 | Widget Blueprints — read+write Widget tree |
| `pie.*` | 9 | PIE start/stop, input dispatch, screenshots |
| `editor.*` | 6 | Editor state, notifications, viewport screenshots |
| `niagara.*` | 7 | FX systems — params + writes + runtime spawn/list/stop |
| `physics.*` | 2 | Line trace, sweep capsule |
| `sequencer.*` | 10 | LevelSequence — tracks/cuts/keyframes + create/add/keyframe writes |
| `anim.*` | 5 | UAnimSequence/UAnimMontage — list, create, sections, notifies, blend |
| `audio.*` | 3 | USoundCue, attenuation, sound class enumeration |
| `wp.*` | 3 | WorldPartition partitioned-check, per-actor RuntimeGrid |
| `sc.*` | 11 | Source Control — checkout, revert, commit |
| `test.*` | 7 | Automation tests |
| `config.*` | 6 | CVar / .ini editing |
| `logs.*` | 5 | Log subscription + stream |
| `livecoding.*` | 3 | Hot-reload trigger |
| `stats.*` | 4 | Engine + memory snapshots |
| `marshall.*` | 4 | Generic UProperty read/write/list |
| `gameplaytag.*` | 4 | List + query/mutate FGameplayTagContainer |
| `debug.*` | 6 | DrawDebugLine/Sphere/Box/Arrow/Text + clear |
| `mesh.*` | 5 | Static mesh list/info/LOD/material/duplicate |
| `folder.*` | 4 | Actor outliner folder list/create/delete/set |
| `level_streaming.*` | 4 | Streaming sub-level list/add/remove/set_loaded |
| `transform.*` | 3 | Batch_set / snap_to_floor / align |
| `viewport.*` | 4 | List/get/set camera + focus_on_actor |
| `hierarchy.*` | 3 | Actor attach/detach/list_children |
| `texture.*` | 4 | List/info/set_compression/generate_solid_color |
| `input.*` | 4 | Enhanced Input — IMC/IA enumeration, context bindings |
| `subsystem.*` | 3 | Generic UE subsystem reflection (list/get_property/call_function) |

See the full [Tools reference](tools/) for per-tool documentation.

## License

Source-available; consult repository LICENSE.

## Getting started

→ [Installation & first connection](getting-started.html)

## Roadmap

→ [Wave G plan](roadmap.html) — physics writes, material graph, navmesh, anim BP state machines, render flags, engine info (planned ~23 tools)
