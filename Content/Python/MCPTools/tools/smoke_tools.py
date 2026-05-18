"""Smoke tools — minimal Python-served demonstrations of the MCP bridge dispatch path.

These tools exist for two reasons:
  1. End-to-end smoke testing (see Plugins/UnrealMCPBridge/Tests/smoke_ping.py).
  2. Worked examples for hand-written tools — copy/paste a function, decorate with @tool,
     drop it in this package, restart the editor or hot-reload Python and it's reachable
     via the MCP bridge as ``{"kind":"call_function","method":"<name>", "args":{...}}``.

All three tools below carry ``thread_safe=True`` because they're pure reads from
``unreal.SystemLibrary`` static methods — safe to invoke on the listener thread once
Lane B (off-game-thread fast path) lands in a later phase. Day 3 still routes everything
through the game-thread dispatch queue regardless of this flag — the flag is metadata for
the eventual Lane-B router.
"""

from __future__ import annotations

import unreal

from MCPTools.registry import tool


@tool(
    name="editor.ping",
    schema_in={"type": "object", "properties": {}, "additionalProperties": False},
    schema_out={
        "type": "object",
        "properties": {
            "pong": {"type": "boolean"},
            "editor_version": {"type": "string"},
        },
        "required": ["pong", "editor_version"],
    },
    thread_safe=True,
    failure_modes=[
        {
            "code": -32000,
            "when": "editor shutting down",
            "recovery": "retry after reconnect",
        }
    ],
)
def ping(_args):
    """Liveness check. Returns the engine version string and a constant true flag."""

    return {
        "pong": True,
        "editor_version": unreal.SystemLibrary.get_engine_version(),
    }


@tool(
    name="editor.engine_version",
    schema_in={"type": "object", "properties": {}, "additionalProperties": False},
    schema_out={
        "type": "object",
        "properties": {"version": {"type": "string"}},
        "required": ["version"],
    },
    thread_safe=True,
)
def engine_version(_args):
    """Return the editor's engine version string (e.g. "5.7.0-...+..."). """

    return {"version": unreal.SystemLibrary.get_engine_version()}


@tool(
    name="editor.project_name",
    schema_in={"type": "object", "properties": {}, "additionalProperties": False},
    schema_out={
        "type": "object",
        "properties": {"name": {"type": "string"}},
        "required": ["name"],
    },
    thread_safe=True,
)
def project_name(_args):
    """Return the loaded project's name (matches the .uproject filename without extension)."""

    return {"name": unreal.SystemLibrary.get_project_name()}
