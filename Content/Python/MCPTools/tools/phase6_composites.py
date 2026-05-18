"""Phase 6 — Python composite tools for the SC + Test + Config + Logs + LiveCoding surface.

**Async-only — composites return ``{job_id}`` and the AI client polls externally via
``job.status`` / ``job.result``.**

This module is a THIN wrapper around the C++ internal handlers registered in
``SourceControlCompositeTools.cpp`` (Chunk A) — later chunks will add ``TestCompositeTools.cpp``
(Chunk B test.run_automation), and ``LiveCodingTools.cpp`` (Chunk E livecoding.recompile).

Why async-only?
================

Same rationale as Phase 3-5 composites (level_composites.py, blueprint_composites.py). The
composite Python function runs inside ``FMCPPythonEval::CallPythonTool`` which executes on the
game thread (Python's GIL is pinned to the GT). Per-tool body inside the job REQUIRES the game
thread (SC ``Provider.Execute(FCheckIn)`` calls into provider plugins that touch UObject /
package state). If a composite tried to block on its own job's result, the GT would never drain
the job body → 60s timeout deadlock.

Resolution: composites NEVER poll. They submit + return ``{job_id}``. The AI client polls
``job.status`` / ``job.result`` from outside the GT (external TCP socket on its own thread).

Chunk A — Source Control composites (1 tool)
=============================================

``sc.submit`` — commit + push a batch of changed files. Provider RPC (Perforce / Git) can take
5-60s on large changelists, so sync would block GT + listener thread.
"""

from __future__ import annotations

from typing import Any, Dict

from MCPTools.registry import tool
from MCPTools.tools.asset_tools import dispatch_internal


# ─── sc.submit (async-only — submits job, returns {job_id}) ─────────────────────────────────
@tool(
    name="sc.submit",
    schema_in={
        "type": "object",
        "properties": {
            "file_paths": {
                "type": "array",
                "minItems": 1,
                "items": {"type": "string"},
                "description": "Files to submit. Disk paths OR /Game/... package paths. "
                               "All paths sandboxed (project / saved / intermediate / engine).",
            },
            "description": {
                "type": "string",
                "minLength": 1,
                "description": "Commit message / changelist description. Required non-empty.",
            },
        },
        "required": ["file_paths", "description"],
    },
    schema_out={
        "type": "object",
        "properties": {"job_id": {"type": "string"}},
        "required": ["job_id"],
    },
    thread_safe=False,
    failure_modes=[
        {"code": "INVALID_PARAMS",
         "when": "file_paths missing/empty/not array of strings OR description missing/empty",
         "recovery": "Pass file_paths as a non-empty list of path strings AND description as a "
                     "non-empty commit message."},
        {"code": "INVALID_PATH",
         "when": "file_paths entry couldn't be resolved (unknown mount, malformed)",
         "recovery": "Use disk paths or /Game/<MountPoint>/... package paths."},
        {"code": "PATH_ESCAPE",
         "when": "file_paths entry resolves outside the sandbox whitelist",
         "recovery": "Pass paths under <Project> / <Saved> / <Intermediate> / <Engine> only."},
        {"code": "SOURCE_CONTROL_PROVIDER_UNAVAILABLE",
         "when": "ISourceControlModule not enabled OR provider not available",
         "recovery": "Configure SC in Editor → Source Control (Git / Perforce / etc.)."},
        {"code": "JOB_SUBMIT_FAILED",
         "when": "Job registry not initialized",
         "recovery": "Retry after 1s"},
    ],
)
def submit(args: Dict[str, Any]) -> Dict[str, Any]:
    """Async-only: submits job, returns {job_id}. Poll via job.status / job.result.

    Commits + submits a batch of files to source control with the given description. The actual
    provider RPC (Perforce / Git) runs inside the job body on the game thread; this composite
    only validates inputs + dispatches the job.

    Inner result (returned by ``job.result`` once Succeeded)::

        {
          "submitted":    bool,                  # FCheckIn returned Succeeded
          "changelist":   str,                   # Perforce CL / Git SHA / provider success msg;
                                                 # empty string if provider doesn't expose one
          "conflicts":    [str],                 # post-submit paths in conflict state
          "duration_ms":  float,                 # wall-clock RPC duration
          "provider":     str,                   # provider name (Git, Perforce, etc.)
          "error":        str (optional)         # failure message if submitted=false
        }

    Per Phase 6 plan risk #7: changelist is always returned as a string (not int). Caller parses
    as int if known-Perforce, hash if known-Git.

    No PIE guard — SC ops are workspace-level and orthogonal to PIE state. Even during PIE the
    editor can submit (workflow concern, not correctness).
    """
    if not isinstance(args, dict):
        raise ValueError("args: expected object")

    file_paths = args.get("file_paths")
    if not isinstance(file_paths, list) or not file_paths:
        raise ValueError("file_paths: required non-empty array of path strings")
    for i, p in enumerate(file_paths):
        if not isinstance(p, str) or not p:
            raise ValueError(f"file_paths[{i}]: must be a non-empty string")

    description = args.get("description")
    if not isinstance(description, str) or not description:
        raise ValueError("description: required non-empty string (commit message)")

    fwd: Dict[str, Any] = {
        "file_paths":  file_paths,
        "description": description,
    }
    return dispatch_internal("sc._submit_internal", fwd)
