---
title: Examples
layout: default
nav_order: 6
---

# Examples
{: .no_toc }

End-to-end usage patterns. All examples assume the editor is open and the bridge is responsive on `127.0.0.1:30020`.

1. TOC
{:toc}

## Probe + tools.list

```python
import socket, json, time

def call(method, args=None, timeout=15):
    s = socket.create_connection(("127.0.0.1", 30020), timeout=timeout)
    req = {"id": "x", "kind": "call_function", "method": method, "args": args or {}}
    s.sendall((json.dumps(req) + "\n").encode())
    buf = b""
    deadline = time.time() + timeout
    while time.time() < deadline:
        c = s.recv(256*1024)
        if not c: break
        buf += c
        if b"\n" in buf:
            return json.loads(buf[:buf.index(b"\n")].decode())
    return None

r = call("tools.list")
print(f"python_ready: {r['result']['python_ready']}")
print(f"cpp_handlers: {len(r['result']['cpp_handlers'])} tools")
```

## Build a Blueprint from scratch

```python
# 1. Create a new Actor BP
BP_PATH = "/Game/Generated/BP_GeneratedActor"
call("bp.create_blueprint", {
    "dest_path": BP_PATH,
    "parent_class_path": "/Script/Engine.Actor",
    "save": False
})

# 2. Add a Health variable
call("bp.add_variable", {
    "blueprint_path": BP_PATH,
    "variable_name": "Health",
    "pin_type": {"category": "Real", "subcategory": "double"},
    "default_value": "100.0"
})

# 3. Set its metadata
call("bp.set_variable_metadata", {
    "blueprint_path": BP_PATH,
    "variable_name": "Health",
    "metadata": {
        "category": "Stats",
        "tooltip": "Current health points",
        "edit_anywhere": True,
        "expose_on_spawn": True,
        "replicate": "rep_notify",
        "rep_notify_function": "OnRep_Health"
    }
})

# 4. Add a function with parameters
call("bp.add_function", {
    "blueprint_path": BP_PATH,
    "function_name": "ApplyDamage"
})
call("bp.add_function_parameter", {
    "blueprint_path": BP_PATH,
    "function_name": "ApplyDamage",
    "param_name": "Amount",
    "direction": "input",
    "pin_type": {"category": "Real", "subcategory": "double"},
    "default_value": "0.0"
})

# 5. Add a component
call("bp.add_component", {
    "blueprint_path": BP_PATH,
    "component_class_path": "/Script/Engine.StaticMeshComponent",
    "variable_name": "MeshComponent",
    "parent_component": "DefaultSceneRoot"
})

# 6. Implement an interface
call("bp.add_interface", {
    "blueprint_path": BP_PATH,
    "interface_class_path": "/Script/Engine.ActorSoundParameterInterface"
})

# 7. Compile
r = call("bp.compile", {"blueprint_path": BP_PATH})
print(f"Compile: {r['result']['status']} ({len(r['result'].get('errors', []))} errors)")

# 8. Inspect what we built
r = call("bp.get_summary", {"blueprint_path": BP_PATH})
print(r["result"])
```

## Spawn + manipulate an actor in editor

```python
# Spawn at origin
r = call("actor.spawn", {
    "class_path": "/Script/Engine.StaticMeshActor",
    "location": [0, 0, 100],
    "label": "MyCube"
})
actor_path = r["result"]["actor_path"]
print(f"Spawned: {actor_path}")

# Set its mesh via property write
call("marshall.write_property", {
    "target": actor_path,
    "property_path": "StaticMeshComponent.StaticMesh",
    "value": "/Engine/BasicShapes/Cube"
})

# Move it
call("transform.batch_set", {
    "actor_paths": [actor_path],
    "location": [500, 200, 100],
    "rotation": [0, 45, 0]
})

# Focus the editor viewport on it
call("viewport.focus_on_actor", {"actor_path": actor_path})

# Take a screenshot
call("editor.viewport_screenshot", {"output_path": "/tmp/snap.png"})
```

## Visual debug overlay during PIE

```python
# Start PIE
call("pie.start")

# Draw a sphere around player spawn (will show in PIE viewport)
call("debug.draw_sphere", {
    "center": [0, 0, 100],
    "radius": 200,
    "color": [1, 1, 0, 1],
    "lifetime": 30.0
})

# Draw a path from origin
for i in range(10):
    call("debug.draw_line", {
        "start": [i * 100, 0, 50],
        "end": [(i+1) * 100, 0, 50],
        "color": [0, 1, 0, 1],
        "thickness": 5.0,
        "lifetime": 30.0
    })

# Stop PIE (overlays will clear)
call("pie.stop")
```

## Sequencer authoring

```python
SEQ_PATH = "/Game/Cinematics/MyShot"

# 1. Create the sequence
call("sequencer.create_sequence", {"dest_path": SEQ_PATH, "save": False})

# 2. Add camera-cut master track
call("sequencer.add_master_track", {
    "sequence_path": SEQ_PATH,
    "track_class": "/Script/MovieSceneTracks.MovieSceneCameraCutTrack"
})

# 3. Add a 2-second camera cut starting at tick 0 (24000 ticks/sec default)
call("sequencer.add_camera_cut", {
    "sequence_path": SEQ_PATH,
    "start_frame": 0,
    "end_frame": 48000
})

# 4. Inspect
r = call("sequencer.get_tracks", {"sequence_path": SEQ_PATH})
print(r["result"])
```

## Material instance parameter tweak

```python
MAT_INST = "/Game/Materials/MI_PlayerSkin"

# Read current scalar param
r = call("material.list_scalar_parameters", {"material_path": MAT_INST})
print(r["result"]["parameters"])

# Bump emissive intensity
call("material.set_scalar_parameter", {
    "material_path": MAT_INST,
    "parameter_name": "EmissiveStrength",
    "value": 5.0
})
```

## Pagination — search across all of /Game

```python
all_meshes = []
page_token = None
while True:
    r = call("mesh.list", {
        "path_prefix": "/Game",
        "page_size": 100,
        "page_token": page_token
    })
    page = r["result"]["sequences" if False else "meshes"]
    all_meshes.extend(page)
    page_token = r["result"].get("next_page_token")
    if not page_token: break

print(f"Total meshes in /Game: {len(all_meshes)}")
```

## Niagara — spawn FX from script

```python
# One-shot effect at world location
r = call("niagara.spawn_at_location", {
    "system_path": "/Game/FlecsDA/Niagara/NS_RopeSwing",
    "location": [0, 0, 500],
    "rotation": [0, 0, 0],
    "auto_destroy": True
})
comp_path = r["result"]["component_path"]

# Check it's active
r = call("niagara.list_active")
print(f"Active components: {len(r['result']['components'])}")

# Wait, then stop all
time.sleep(5)
call("niagara.stop_all")
```

## Read all gameplay tags in a project

```python
all_tags = []
page_token = None
while True:
    r = call("gameplaytag.list", {
        "page_size": 500,
        "page_token": page_token,
        "only_dictionary": False
    })
    all_tags.extend(t["tag"] for t in r["result"]["tags"])
    page_token = r["result"].get("next_page_token")
    if not page_token: break

print(f"All tags: {len(all_tags)}")
# Filter to a parent prefix:
damage_tags = [t for t in all_tags if t.startswith("Damage.")]
print(f"Damage.* tags: {damage_tags}")
```

## Subsystem reflection — call an editor-side UFUNCTION

```python
# Check if an asset exists via UEditorAssetSubsystem
r = call("subsystem.call_function", {
    "class_path": "/Script/EditorScriptingUtilities.EditorAssetSubsystem",
    "function_name": "DoesAssetExist",
    "args": {"AssetPath": "/Game/Maps/MainMenu"}
})
print(r["result"]["return_value"])
```

## Source Control commit

```python
# Mark dirty assets for checkout
r = call("sc.checkout_files", {
    "file_paths": ["/Game/MyAsset", "/Game/MyOtherAsset"]
})
print(r["result"])

# Commit with message
call("sc.commit", {
    "file_paths": ["/Game/MyAsset", "/Game/MyOtherAsset"],
    "commit_message": "auto: nightly content sync"
})
```
