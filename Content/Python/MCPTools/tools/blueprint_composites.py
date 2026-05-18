"""Phase 4 Day 10 — Category C Blueprint Python composite tool (1 tool).

**Async-only — returns ``{job_id}`` and the AI client polls externally via ``job.status`` /
``job.result``.**

This is a THIN wrapper around the C++ internal handler registered in
``BlueprintCompositeTools.cpp``. The composite is one line of dispatch after argument validation:
validate ``scope_paths`` (non-empty list of ``/Game/...`` paths) → ``dispatch_internal(
'bp._compile_all_dirty_internal', args)`` → return the ``{job_id}`` envelope.

Why async-only?
================

Same rationale as Phase 3 ``level_composites``. The composite Python function runs inside
``FMCPPythonEval::CallPythonTool`` which executes on the game thread (Python's GIL is pinned to
the GT). Per-BP compile inside the job body REQUIRES the game thread (``UBlueprint`` /
``FKismetEditorUtilities`` aren't thread-safe). If a composite tried to block on its own job's
result, the GT would never drain the job body → 60s timeout deadlock.

Resolution: composites NEVER poll. They submit + return ``{job_id}``. The AI client polls
``job.status`` / ``job.result`` from outside the GT (external TCP socket on its own thread).
This is the pattern ``asset.batch_metadata_async`` has used since Phase 2 Day 12 — it always
worked. All Phase 2/3/4 composites now follow it uniformly.

Output schema
=============

The composite returns ``{"job_id": "<uuid>"}``. The AI client then calls
``job.result {"job_id": ..., "wait_timeout_s": <N>}`` (Lane B, returns when the job reaches
terminal state) and reads ``.result`` from the response::

    {
      "compiled":    int,                          # total BPs processed
      "succeeded":   int,                          # count with healthy Status post-compile
      "failed":      [{"path": str, "errors": [str]}],
      "duration_ms": float
    }

Failure aggregation (D1)
========================

The body does NOT abort on first failure. It accumulates per-BP status across the entire scope
so AI workflows get the full failure surface in one round-trip — fix N BPs across multiple files
in one edit. Pass ``fail_fast=true`` to opt into stop-on-first-fail behaviour.

scope_paths (D6)
================

Required. Defaults expected to be ``["/Game"]``. Empty array → ValueError → auto-translates to
JSON-RPC -32602 via the Phase 3 polish #12 dispatcher. Engine BPs (``/Engine/...``), plugin BPs
(``/<PluginName>/...``), and ``/Script/...`` are excluded unless explicitly listed.

PIE behaviour
=============

The composite refuses if PIE is running when the C++ job body executes — surfaced via
``job.result``'s error field with the frozen ``kMCPMessagePIEActive`` text. The PIE guard fires
INSIDE the C++ body (not at submit time) because PIE state can transition between listener-thread
submit and game-thread execution.

Cancellation
============

The job body polls ``Job.bCancelRequested`` every 16 BPs (heavier cadence than Phase 3 default
256 because each compile is ~50ms-2s instead of a trivial mutation). AI client can issue
``job.cancel`` and expect a Cancelled state within ~30 seconds typical.
"""

from __future__ import annotations

from typing import Any, Dict

from MCPTools.registry import tool
from MCPTools.tools.asset_tools import dispatch_internal


# ─── bp.compile_all_dirty (async-only — submits job, returns {job_id}) ──────────────────────
@tool(
    name="bp.compile_all_dirty",
    schema_in={
        "type": "object",
        "properties": {
            "scope_paths": {
                "type": "array",
                "minItems": 1,
                "items": {"type": "string"},
                "description": "AR PackagePaths (recursive) to scan. Required non-empty. "
                               "Engine/plugin/Script paths excluded unless explicitly listed.",
            },
            "fail_fast": {
                "type": "boolean",
                "default": False,
                "description": "Stop on first per-BP failure. Default false (continue + aggregate).",
            },
        },
        "required": ["scope_paths"],
    },
    schema_out={
        "type": "object",
        "properties": {"job_id": {"type": "string"}},
        "required": ["job_id"],
    },
    thread_safe=False,
    failure_modes=[
        {"code": "INVALID_PARAMS",
         "when": "scope_paths missing/empty/not an array of strings",
         "recovery": "Pass scope_paths as a non-empty list of /Game/... paths."},
        {"code": "PIE_ACTIVE",
         "when": "PIE is running when the job body executes",
         "recovery": "Stop PIE first; Phase 5 will ship pie.* tools for PIE-world mutation."},
        {"code": "JOB_SUBMIT_FAILED",
         "when": "Job registry not initialized",
         "recovery": "Retry after 1s"},
    ],
)
def compile_all_dirty(args: Dict[str, Any]) -> Dict[str, Any]:
    """Async-only: submits job, returns {job_id}. Poll via job.status / job.result.

    Walks the AssetRegistry filtered by ``scope_paths`` (recursive) for UBlueprint assets,
    loads each, compiles each, and returns aggregated results. Per-BP failure does NOT halt the
    batch by default (set ``fail_fast=true`` to opt in).

    Inner result (returned by ``job.result`` once Succeeded)::

        {
          "compiled":    int,                     # total BPs processed
          "succeeded":   int,                     # count with Blueprint->Status ∈ {BS_UpToDate,
                                                  #                                BS_UpToDateWithWarnings}
          "failed":      [{"path", "errors"}],    # per-failed-BP details
          "duration_ms": float
        }

    Use ``bp.compile`` for a single-BP synchronous variant.
    """
    if not isinstance(args, dict):
        raise ValueError("args: expected object")
    scope_paths = args.get("scope_paths")
    if not isinstance(scope_paths, list) or not scope_paths:
        raise ValueError("scope_paths: required non-empty array of /Game/... path strings")
    for i, p in enumerate(scope_paths):
        if not isinstance(p, str) or not p:
            raise ValueError(f"scope_paths[{i}]: must be a non-empty string")
        if not p.startswith("/"):
            raise ValueError(
                f"scope_paths[{i}]='{p}': must start with '/' (e.g. '/Game' or '/Game/MCPTest')"
            )

    fwd: Dict[str, Any] = {"scope_paths": scope_paths}
    if "fail_fast" in args:
        fwd["fail_fast"] = bool(args["fail_fast"])
    return dispatch_internal("bp._compile_all_dirty_internal", fwd)
