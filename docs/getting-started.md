---
title: Getting Started
layout: default
nav_order: 2
---

# Getting Started
{: .no_toc }

1. TOC
{:toc}

## Prerequisites

- **Unreal Engine 5.7** (Windows desktop editor)
- **Visual Studio 2022** (VC++ 14.44, Windows 11 SDK 22621) for C++ compilation
- **Python 3.10+** (UE bundles its own; only needed for external test clients)

The plugin compiles cleanly on UE 5.7 mainline. Other versions may need API adjustments (specifically: `LevelSequenceFactoryNew`, `FActorFolders` header rename, `ImplementNewInterface` signature differ across 5.4 → 5.7).

## Install — vendored copy

```bash
cd <YourGameProject>/Plugins
git clone https://github.com/bluesteelll/ue5-mcp-bridge.git UnrealMCPBridge
```

That gives you `Plugins/UnrealMCPBridge/` with `Source/`, `Content/`, `Tests/`, `UnrealMCPBridge.uplugin`.

## Install — git submodule (recommended for multi-project use)

```bash
cd <YourGameProject>
git submodule add https://github.com/bluesteelll/ue5-mcp-bridge.git Plugins/UnrealMCPBridge
git submodule update --init --recursive
```

Then pull upstream changes via:
```bash
cd Plugins/UnrealMCPBridge
git pull origin main
cd ../..
git add Plugins/UnrealMCPBridge
git commit -m "bump mcp bridge"
```

## Build

Regenerate project files (UE will pick up the new plugin automatically), then build the editor target:

```bash
"D:/Epic Games/UE_5.7/Engine/Binaries/DotNET/UnrealBuildTool/UnrealBuildTool.exe" \
  YourGameEditor Win64 Development \
  -Project=YourGame.uproject \
  -WaitMutex -FromMSBuild
```

Or just open the .uproject — UE will compile the plugin on first launch (slow but works).

The plugin is **editor-only** — it's not compiled into shipped builds. The `Type` field in `.uplugin` is `Editor`.

## Verify

Once the editor is open, watch the output log:

```
LogMCP: MCP bridge module starting (Phase 1 Day 7 — async jobs + log streaming + tools.list)
LogMCP: MCP bridge module ready (listener=RUNNING port=30020, log_stream=ATTACHED)
```

Probe the bridge from any TCP-capable client:

```python
import socket, json
s = socket.create_connection(("127.0.0.1", 30020), timeout=3)
s.sendall(b'{"id":"p","kind":"call_function","method":"tools.list","args":{}}\n')
print(s.recv(64*1024).decode()[:200])
```

Expected: `{"id":"p","ok":true,"result":{"python_ready":true,"cpp_handlers":[...]}}`

## Change the port

Default `127.0.0.1:30020` (loopback only — no external access).

Override via console command in editor:

```
MCP.RestartListener 30021
```

Or via project's `DefaultGame.ini`:

```ini
[/Script/UnrealMCPBridge.MCPSettings]
ListenerPort=30021
```

The plugin **never accepts non-loopback connections** — `MCP.RestartListener` rejects any bind to non-`127.0.0.1` addresses.

## First call — full round-trip

```python
import socket, json, time

def call(method, args=None, timeout=15):
    s = socket.create_connection(("127.0.0.1", 30020), timeout=timeout)
    req = {"id": "1", "kind": "call_function", "method": method, "args": args or {}}
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

# Enumerate all currently-loaded levels:
r = call("level.list_loaded")
print(r["result"])

# Spawn a test cube:
r = call("actor.spawn", {
    "class_path": "/Script/Engine.StaticMeshActor",
    "location": [0, 0, 200],
    "label": "TestCube"
})
print(r["result"]["actor_path"])

# Draw a debug line:
call("debug.draw_line", {
    "start": [0, 0, 0],
    "end": [500, 0, 500],
    "color": [1, 0, 0, 1],
    "thickness": 3.0,
    "lifetime": 10.0
})
```

## Request/response envelope

Every request:

```json
{
  "id": "<caller-defined string>",
  "kind": "call_function",
  "method": "<namespace.tool>",
  "args": { ... }
}
```

Every response (success):

```json
{
  "id": "<echoed>",
  "ok": true,
  "result": { ... }
}
```

Every response (error):

```json
{
  "id": "<echoed>",
  "ok": false,
  "error": {
    "code": -32004,
    "message": "actor 'BadName' not found in current world"
  }
}
```

See [Architecture](architecture.html) for protocol details and [Error codes](error-codes.html) for the full table.

## Next steps

- [Architecture deep dive](architecture.html) — TCP, Lane A/B, dispatch model
- [Tools reference](tools/) — per-namespace docs
- [Error codes](error-codes.html) — full table with recovery hints
- [Examples](examples.html) — end-to-end workflows
