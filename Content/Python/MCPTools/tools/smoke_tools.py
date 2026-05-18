"""Smoke tools — minimal demonstrations that exercise the registry decorator.

These exist so the Phase 1 Day 1 scaffold has at least one ``@tool`` invocation
in the import graph. They will be reachable end-to-end once the TCP listener
(Day 2) and dispatcher (Day 3) land. For now importing this module is enough
to verify the decorator and registry mechanics work without errors.
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
