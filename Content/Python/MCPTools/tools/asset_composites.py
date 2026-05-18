"""Phase 2 Days 11-12 — Category D Python composite tools (6 tools).

**HOTFIX 3 (2026-05): ALL composites are async-only — they return ``{job_id}`` and the AI
client polls externally via ``job.status`` / ``job.result``.**

These are THIN wrappers around the C++ Asset Registry internal handlers registered in
``AssetCompositeTools.cpp``. Each composite is one line: validate args (when necessary) +
``dispatch_internal('asset._..._internal', args)`` + return the ``{job_id}`` envelope.

Why all-async?
==============

The composite Python function runs inside ``FMCPPythonEval::CallPythonTool`` which executes on
the **game thread** (Python's GIL is pinned to the GT). Three things conspire to require the
all-async pattern:

  1. Game thread ownership — once the composite enters, the GT is blocked until the composite
     returns.
  2. UE 5.7 AR off-GT assert — ``IAR.GetAssets`` requires the GT (Hotfix 1 finding).
  3. Job-body GT dispatch — a job submitted with ``bGameThreadRequired=true`` cannot run while
     the GT is still owned by the composite.

If a composite tries to block on its OWN job's result (the pre-Hotfix-3 ``wait_for_job_and_
return_result`` pattern), the GT can never drain the job body → 60s timeout deadlock. Promoting
``job.result`` to Lane B (Hotfix 2) didn't help because the composite STILL owns the GT while
sleeping between polls.

Resolution: composites NEVER poll. They submit + return ``{job_id}``. The AI client polls
``job.status`` / ``job.result`` from outside the GT (external TCP socket on its own thread).
This is the pattern ``asset.batch_metadata_async`` has used since Day 12 — it always worked.

Output schemas
==============

Every composite returns the SAME shape:

  ``{"job_id": "<uuid>"}``

The AI client then calls ``job.result {"job_id": ..., "wait_timeout_s": <N>}`` (Lane B, returns
when the job reaches terminal state) and reads ``.result`` from the response. Inner result
payloads match the schema documented on each composite below — that's what the AI client gets
back from ``job.result``, not from this composite call directly.
"""

from __future__ import annotations

from typing import Any, Dict

from MCPTools.registry import tool
from MCPTools.tools.asset_tools import dispatch_internal


# ─── asset.find_unused (async-only — submits job, returns {job_id}) ─────────────────────────
@tool(
    name="asset.find_unused",
    schema_in={
        "type": "object",
        "properties": {
            "package_paths": {"type": "array", "items": {"type": "string"}, "minItems": 1},
            "exclude_class_paths": {
                "type": "array",
                "items": {"type": "string"},
                "default": [
                    "/Script/Engine.World",
                    "/Script/Engine.MapBuildDataRegistry",
                    "/Script/Engine.GameModeBase",
                    "/Script/Engine.GameMode",
                    "/Script/Engine.PlayerController",
                    "/Script/Engine.GameStateBase",
                    "/Script/Engine.PlayerState",
                    "/Script/Engine.HUD",
                    "/Script/Engine.GameInstance",
                    "/Script/Engine.GameUserSettings",
                    "/Script/Engine.SaveGame",
                ],
            },
        },
        "required": ["package_paths"],
    },
    schema_out={
        "type": "object",
        "properties": {"job_id": {"type": "string"}},
        "required": ["job_id"],
    },
    thread_safe=False,
    failure_modes=[
        {"code": "INVALID_PATH", "when": "any package_paths malformed", "recovery": "/Game/..."},
        {"code": "JOB_SUBMIT_FAILED", "when": "Job registry not initialized",
         "recovery": "Retry after 1s"},
    ],
)
def find_unused(args: Dict[str, Any]) -> Dict[str, Any]:
    """Async-only: submits job, returns {job_id}. Poll via job.status / job.result.

    Composite cannot run synchronously because:

      1. Composite runs on the game thread (Python GIL is pinned to GT).
      2. AR enumeration in UE 5.7 requires the game thread (Hotfix 1 finding).
      3. If composite blocks on poll, game thread can't drain job body → deadlock.

    Always-async resolves all three: composite returns immediately, AI client polls
    via external TCP socket (off GT), job body runs when GT is free.

    Inner result (returned by ``job.result`` once Succeeded):
      ``{"unused": [{"asset_path", "class"}, ...], "scanned_count": int}``

    STATIC-analysis only — runtime references (LoadClass, GameMode default-pawn refs,
    data-table cells, savegame string refs) are invisible to the Asset Registry. Always
    confirm via in-editor Reference Viewer before deleting.
    """
    package_paths = args.get("package_paths")
    if not isinstance(package_paths, list) or not package_paths:
        raise ValueError("package_paths: required non-empty array")

    # Pass through both args; the C++ handler handles defaults for exclude_class_paths.
    fwd: Dict[str, Any] = {"package_paths": package_paths}
    if "exclude_class_paths" in args:
        fwd["exclude_class_paths"] = args["exclude_class_paths"]
    else:
        # Forward the default set so the C++ handler exclusion behavior matches the public schema.
        fwd["exclude_class_paths"] = [
            "/Script/Engine.World",
            "/Script/Engine.MapBuildDataRegistry",
            "/Script/Engine.GameModeBase",
            "/Script/Engine.GameMode",
            "/Script/Engine.PlayerController",
            "/Script/Engine.GameStateBase",
            "/Script/Engine.PlayerState",
            "/Script/Engine.HUD",
            "/Script/Engine.GameInstance",
            "/Script/Engine.GameUserSettings",
            "/Script/Engine.SaveGame",
        ]
    return dispatch_internal("asset._find_unused_internal", fwd)


# ─── asset.size_report (async-only — submits job, returns {job_id}) ─────────────────────────
@tool(
    name="asset.size_report",
    schema_in={
        "type": "object",
        "properties": {
            "package_paths": {"type": "array", "items": {"type": "string"}, "minItems": 1},
            "top_n": {"type": "integer", "minimum": 1, "maximum": 1000, "default": 50},
        },
        "required": ["package_paths"],
    },
    schema_out={
        "type": "object",
        "properties": {"job_id": {"type": "string"}},
        "required": ["job_id"],
    },
    thread_safe=False,
    failure_modes=[
        {"code": "INVALID_PATH", "when": "malformed", "recovery": "/Game/..."},
        {"code": "JOB_SUBMIT_FAILED", "when": "Job registry not initialized",
         "recovery": "Retry after 1s"},
    ],
)
def size_report(args: Dict[str, Any]) -> Dict[str, Any]:
    """Async-only: submits job, returns {job_id}. Poll via job.status / job.result.

    Same deadlock-avoidance rationale as ``find_unused`` — see that docstring.

    Inner result (returned by ``job.result`` once Succeeded):
      ``{"top": [{"asset_path", "class", "bytes"}, ...], "total_bytes": int}``
    """
    package_paths = args.get("package_paths")
    if not isinstance(package_paths, list) or not package_paths:
        raise ValueError("package_paths: required non-empty array")

    fwd: Dict[str, Any] = {"package_paths": package_paths}
    if "top_n" in args:
        fwd["top_n"] = args["top_n"]
    return dispatch_internal("asset._size_report_internal", fwd)


# ─── asset.batch_metadata (Option A: async-only — submits job, returns {job_id}) ────────────
@tool(
    name="asset.batch_metadata",
    schema_in={
        "type": "object",
        "properties": {
            "paths": {"type": "array", "items": {"type": "string"}, "minItems": 1, "maxItems": 5000},
        },
        "required": ["paths"],
    },
    schema_out={
        "type": "object",
        "properties": {"job_id": {"type": "string"}},
        "required": ["job_id"],
    },
    thread_safe=False,
    failure_modes=[
        {"code": "INVALID_PATH", "when": "any path malformed", "recovery": "/Game/..."},
        {"code": "INPUT_TOO_LARGE", "when": "paths.length > 5000",
         "recovery": "Split into multiple smaller batches and merge results"},
        {"code": "JOB_SUBMIT_FAILED", "when": "Job registry not initialized",
         "recovery": "Retry after 1s"},
    ],
)
def batch_metadata(args: Dict[str, Any]) -> Dict[str, Any]:
    """Async-only: submits worker-pool job, returns {job_id}.

    HOTFIX 3 (2026-05): merged the previous sync/async split into a single async tool.
    The previous sync ``asset.batch_metadata`` looped ``asset.metadata`` per path via TCP
    loopback (Lane A) which deadlocked the composite for the same reason ``find_unused`` did
    pre-Hotfix-2. The async-job pattern resolves it: validate paths upfront in C++, push the
    worker-pool job that does the AR-only lookups (no GT requirement, thread-safe), return
    ``{job_id}`` immediately.

    ``asset.batch_metadata_async`` is kept as an alias for backward compatibility; both go
    through the same ``asset._batch_metadata_internal`` handler.

    Inner result (returned by ``job.result`` once Succeeded):
      ``{"assets": [{"asset_path", "package_path", "class", "tags"}, ...],
         "failed": [{"path", "error"}, ...], "duration_ms": float}``
    """
    paths = args.get("paths")
    if not isinstance(paths, list) or not paths:
        raise ValueError("paths: required non-empty array")
    # Runtime enforcer for the schema's maxItems=5000 (schema is documentation, not enforced
    # by the dispatcher). Surfaced as INPUT_TOO_LARGE per the failure_modes contract.
    if len(paths) > 5000:
        raise ValueError(
            f"INPUT_TOO_LARGE: paths.length={len(paths)} > 5000 — split into smaller batches"
        )

    return dispatch_internal("asset._batch_metadata_internal", {"paths": paths})


# ─── asset.batch_metadata_async (backward-compat alias for asset.batch_metadata) ────────────
@tool(
    name="asset.batch_metadata_async",
    schema_in={
        "type": "object",
        "properties": {
            "paths": {"type": "array", "items": {"type": "string"}, "minItems": 1, "maxItems": 5000},
        },
        "required": ["paths"],
    },
    schema_out={
        "type": "object",
        "properties": {"job_id": {"type": "string"}},
        "required": ["job_id"],
    },
    thread_safe=False,
    failure_modes=[
        {"code": "INVALID_PATH", "when": "any path malformed", "recovery": "/Game/..."},
        {"code": "JOB_SUBMIT_FAILED", "when": "Job registry not initialized",
         "recovery": "Retry after 1s"},
    ],
)
def batch_metadata_async(args: Dict[str, Any]) -> Dict[str, Any]:
    """Backward-compat alias: identical to ``asset.batch_metadata`` post-Hotfix-3.

    Both tools route to ``asset._batch_metadata_internal`` and return ``{job_id}``. Kept as a
    distinct tool entry so existing call sites continue to work.
    """
    paths = args.get("paths")
    if not isinstance(paths, list) or not paths:
        raise ValueError("paths: required non-empty array")
    if len(paths) > 5000:
        raise ValueError(
            f"INPUT_TOO_LARGE: paths.length={len(paths)} > 5000 — split into smaller batches"
        )

    return dispatch_internal("asset._batch_metadata_internal", {"paths": paths})


# ─── asset.find_broken_references (async-only — submits job, returns {job_id}) ──────────────
@tool(
    name="asset.find_broken_references",
    schema_in={
        "type": "object",
        "properties": {
            "package_paths": {"type": "array", "items": {"type": "string"}, "minItems": 1},
        },
        "required": ["package_paths"],
    },
    schema_out={
        "type": "object",
        "properties": {"job_id": {"type": "string"}},
        "required": ["job_id"],
    },
    thread_safe=False,
    failure_modes=[
        {"code": "INVALID_PATH", "when": "malformed paths", "recovery": "/Game/..."},
        {"code": "JOB_SUBMIT_FAILED", "when": "Job registry not initialized",
         "recovery": "Retry after 1s"},
    ],
)
def find_broken_references(args: Dict[str, Any]) -> Dict[str, Any]:
    """Async-only: submits job, returns {job_id}. Poll via job.status / job.result.

    HOTFIX 3 (2026-05): converted from per-asset Python loop (``asset.list`` → per-asset
    ``asset.find_dependents`` → per-dep ``asset.exists``) to a single C++ internal handler.
    The previous loop deadlocked the composite for the same reason ``find_unused`` did
    pre-Hotfix-2 — every TCP loopback call queued back to the GT that was blocked on the
    composite's outer socket.

    For each asset in scope: walks HARD dependencies (soft deps may legitimately be unloaded),
    checks each dep package exists in the AR. Native ``/Script/`` deps are skipped (always
    present).

    Inner result (returned by ``job.result`` once Succeeded):
      ``{"broken": [{"asset_path", "missing_paths": [...]}, ...], "scanned_count": int}``
    """
    package_paths = args.get("package_paths")
    if not isinstance(package_paths, list) or not package_paths:
        raise ValueError("package_paths: required non-empty array")

    return dispatch_internal("asset._find_broken_references_internal",
                             {"package_paths": package_paths})


# ─── asset.find_duplicates_by_name (async-only — submits job, returns {job_id}) ─────────────
@tool(
    name="asset.find_duplicates_by_name",
    schema_in={
        "type": "object",
        "properties": {
            "package_paths": {"type": "array", "items": {"type": "string"}, "minItems": 1},
            "ignore_class": {"type": "boolean", "default": True},
        },
        "required": ["package_paths"],
    },
    schema_out={
        "type": "object",
        "properties": {"job_id": {"type": "string"}},
        "required": ["job_id"],
    },
    thread_safe=False,
    failure_modes=[
        {"code": "INVALID_PATH", "when": "malformed paths", "recovery": "/Game/..."},
        {"code": "JOB_SUBMIT_FAILED", "when": "Job registry not initialized",
         "recovery": "Retry after 1s"},
    ],
)
def find_duplicates_by_name(args: Dict[str, Any]) -> Dict[str, Any]:
    """Async-only: submits job, returns {job_id}. Poll via job.status / job.result.

    HOTFIX 3 (2026-05): converted from a per-scope Python loop (``asset.list`` paginated
    per scope) to a single C++ internal handler. Same deadlock-avoidance rationale as
    ``find_broken_references``.

    Groups assets by short basename across all scopes. With ``ignore_class=true`` (default),
    two assets with the same short name but different classes count as duplicates. Emits only
    groups with count > 1.

    Inner result (returned by ``job.result`` once Succeeded):
      ``{"duplicates": [{"name", "paths": [{"asset_path", "class"}, ...]}, ...],
         "scanned_count": int}``
    """
    package_paths = args.get("package_paths")
    if not isinstance(package_paths, list) or not package_paths:
        raise ValueError("package_paths: required non-empty array")

    fwd: Dict[str, Any] = {"package_paths": package_paths}
    if "ignore_class" in args:
        fwd["ignore_class"] = bool(args["ignore_class"])
    return dispatch_internal("asset._find_duplicates_by_name_internal", fwd)
