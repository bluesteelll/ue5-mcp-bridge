"""Tool registry for the Unreal MCP Bridge.

A tool is a Python callable decorated with ``@tool(name, schema_in, ...)``. The
registry is process-global: re-importing the same tool module simply replaces
the entry (last-writer-wins). Re-imports are common during interactive editor
sessions (Live Reload, hot-import), so a *warning* is logged on overwrite to
surface accidental name collisions while keeping the workflow ergonomic.

Phase 1 Day 1 scope: decorator + accessor. The bridge dispatcher that consumes
``get_all_tools()`` arrives in Phase 1 Day 3+ work.
"""

from __future__ import annotations

from typing import Any, Callable, Dict, List, Optional


# Module-private registry. Tools are keyed by their wire name.
_TOOLS: Dict[str, Dict[str, Any]] = {}


def tool(
    name: str,
    schema_in: Dict[str, Any],
    schema_out: Optional[Dict[str, Any]] = None,
    thread_safe: bool = False,
    failure_modes: Optional[List[Dict[str, Any]]] = None,
    _internal: bool = False,
) -> Callable[[Callable[..., Any]], Callable[..., Any]]:
    """Register a callable as an MCP tool.

    Args:
        name: Wire-protocol tool name, e.g. ``"editor.ping"``. Must be unique across
            all loaded tool modules; collisions log a warning and overwrite.
        schema_in: JSON Schema for the input payload (passed as a Python dict to the
            tool body at dispatch time).
        schema_out: JSON Schema for the return value. Optional; defaults to empty
            permissive schema for prototype tools.
        thread_safe: If True the tool MAY be invoked on the listener thread (Lane B)
            without going through the game-thread queue. The tool body MUST NOT
            touch UObject / GWorld / AssetRegistry mutable state in that case.
            See blueprint v2 §C3 for the dispatch-lane contract.
        failure_modes: Optional list of ``{"code", "when", "recovery"}`` dicts
            documenting expected error surfaces, used for tool-listing UX.
        _internal: If True the tool is registered + dispatchable but FILTERED OUT
            of ``tools.list`` enumeration. Used for Python-side helper composites
            that need a private name (e.g. ``asset._find_unused_internal_py``).
            Note: C++ internal handlers (``asset._batch_metadata_internal`` etc.)
            are registered directly via ``FMCPDispatchQueue::RegisterHandler`` and
            are never seen by this Python decorator — they appear in the
            ``cpp_handlers`` list of ``tools.list`` regardless of this flag.
    """

    def deco(fn: Callable[..., Any]) -> Callable[..., Any]:
        prev = _TOOLS.get(name)
        if prev is not None:
            # Last-writer-wins per critic M1 fix — log overwrite so collisions don't go silent.
            try:
                import unreal  # noqa: WPS433 — runtime UE binding
                unreal.log_warning(
                    f"MCPTools.registry: overwriting tool '{name}' "
                    f"(was from {prev['module']}, now from {fn.__module__})"
                )
            except ImportError:
                # Running outside the editor (unit-test harness) — degrade silently.
                pass

        _TOOLS[name] = {
            "fn": fn,
            "schema_in": schema_in,
            "schema_out": schema_out or {},
            "thread_safe": thread_safe,
            "failure_modes": failure_modes or [],
            "module": fn.__module__,
            "_internal": bool(_internal),
        }
        return fn

    return deco


def get_all_tools() -> Dict[str, Dict[str, Any]]:
    """Return a shallow copy of the registry, EXCLUDING tools registered with ``_internal=True``.

    Internal helpers stay dispatchable (a caller who knows the exact name can still invoke
    them via the unknown-method fallback) but are hidden from the public ``tools.list``
    enumeration consumed by AI agents and discovery clients.
    """

    return {k: v for k, v in _TOOLS.items() if not v.get("_internal", False)}


def get_all_tools_including_internal() -> Dict[str, Dict[str, Any]]:
    """Return a shallow copy of the FULL registry, including internal helpers.

    Used by smoke tests that need to assert presence of internal Python wrappers.
    """

    return dict(_TOOLS)


def get_tool(name: str) -> Optional[Dict[str, Any]]:
    """Look up a single tool entry by wire name, or ``None`` if not registered."""

    return _TOOLS.get(name)


def clear() -> None:
    """Test-only: wipe the registry. Never called by production bridge code."""

    _TOOLS.clear()
