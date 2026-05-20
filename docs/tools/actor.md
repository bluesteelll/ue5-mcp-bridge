---
title: actor.* (Actor lifecycle)
layout: default
parent: Tools reference
nav_order: 3
---

# actor.* — Actor lifecycle + property access (24 tools)

Spawn, destroy, modify properties, components, find actors by class/label/tag, attach. Operates on actors in the editor world (or PIE world via `pie.*` siblings).

## Lifecycle

| Tool | Description |
|---|---|
| `actor.spawn` | `class_path` + optional `location`/`rotation`/`scale`/`label`. Returns `actor_path`. -32027 PIE / -32021 abstract / -32022 wrong family / -32023 invalid path |
| `actor.destroy` | Destroy a single actor |
| `actor.destroy_batch` | Destroy multiple actors atomically |
| `actor.duplicate` | Clone an actor + optionally adjust transform |

## Properties

| Tool | Description |
|---|---|
| `actor.get_property` | Generic FProperty read via dotted path (`Actor.Component.Field`) |
| `actor.set_property` | Generic FProperty write |
| `actor.list_properties` | Enumerate UPROPERTIES on the actor |

## Components

| Tool | Description |
|---|---|
| `actor.list_components` | List components on a spawned actor instance |
| `actor.add_component` | Add a runtime component instance (not SCS — use bp.add_component for that) |
| `actor.remove_component` | Remove component instance |
| `actor.find_component` | Locate by class or name; -32024 if ambiguous |

## Find by

| Tool | Description |
|---|---|
| `actor.find_by_class` | Linear scan filtered by UClass |
| `actor.find_by_label` | Linear scan filtered by display label |
| `actor.find_by_tag` | Linear scan filtered by AActor::Tags |

## Path resolution (`MCPActorPathUtils`)

All actor-taking tools accept multiple path forms:

- **Full canonical**: `/Temp/Untitled_0.Untitled:PersistentLevel.Brush_0`
- **Map::name pair**: `/Game/Maps/MainMap::PlayerStart_1`
- **Bare name** (when unique in current world): `PlayerStart_1` or `Brush_0`

Response shape always echoes the resolved canonical path.

## Example

```python
# Spawn at origin
r = call("actor.spawn", {
    "class_path": "/Script/Engine.StaticMeshActor",
    "location": [0, 0, 100],
    "label": "MyCube"
})
ap = r["result"]["actor_path"]

# Set the static mesh via property write
call("marshall.write_property", {
    "target": ap,
    "property_path": "StaticMeshComponent.StaticMesh",
    "value": "/Engine/BasicShapes/Cube"
})

# Move it
call("transform.batch_set", {
    "actor_paths": [ap],
    "location": [500, 200, 100]
})

# Read back its location to verify
r = call("actor.get_property", {
    "actor_path": ap,
    "property_path": "RootComponent.RelativeLocation"
})
print(r["result"]["value"])  # → {"x": 500, "y": 200, "z": 100}

# Destroy
call("actor.destroy", {"actor_path": ap})
```

For per-tool argument details consult the [plugin README](https://github.com/bluesteelll/ue5-mcp-bridge/blob/main/README.md).
