# UnrealMCPBridge

Editor-only plugin that exposes Unreal Editor tools to an external MCP
(Model Context Protocol) server over TCP. Together with `mcp_server/` in the
project root it lets an AI client (Claude Desktop, Claude Code, etc.) drive
the editor for asset/level/blueprint authoring tasks.

## Status

**Phase 1 Day 1 — scaffold only.** Compiles cleanly. The TCP listener,
game-thread dispatch queue, Python tool framework runtime, and Tier 2 C++
property-marshalling helper are scheduled for Day 2..Day 5.

## Reference

Full design blueprint (v2 with critic fixes applied):
`D:/tmp/mcp_unreal_blueprint_v2_patch.md`

Phase 1 marshalling spike findings (verified UE 5.7 Python bindings):
`D:/tmp/mcp_unreal_spike_results.md`

## Install (later, once functional)

1. Build the project (`UnrealMCPBridge` is `EnabledByDefault: true`).
2. Install the companion server: `pip install -e ../../mcp_server`.
3. Launch the editor; the bridge listener will start on a configurable port
   once Day 2 work lands.

## Layout

```
UnrealMCPBridge/
  UnrealMCPBridge.uplugin
  Source/UnrealMCPBridge/
    UnrealMCPBridge.Build.cs
    Public/  UnrealMCPBridge.h, MCPTypes.h
    Private/ UnrealMCPBridge.cpp
  Content/Python/MCPTools/
    registry.py     # @tool decorator
    marshall.py     # Tier 1 type marshalling (Day 4)
    tools/
      smoke_tools.py  # editor.ping demo
```
