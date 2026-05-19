"""Phase 6 — Python composite tools for the SC + Test + Config + Logs + LiveCoding surface.

**Async-only — composites return ``{job_id}`` and the AI client polls externally via
``job.status`` / ``job.result``.**

This module is a THIN wrapper around the C++ internal handlers registered in
``SourceControlCompositeTools.cpp`` (Chunk A) and ``TestCompositeTools.cpp`` (Chunk B). Later
chunks will add ``LiveCodingTools.cpp`` (Chunk E livecoding.recompile).

Why async-only?
================

Same rationale as Phase 3-5 composites (level_composites.py, blueprint_composites.py). The
composite Python function runs inside ``FMCPPythonEval::CallPythonTool`` which executes on the
game thread (Python's GIL is pinned to the GT). Per-tool body inside the job REQUIRES the game
thread (SC ``Provider.Execute(FCheckIn)`` calls into provider plugins that touch UObject /
package state; FAutomationTestFramework drives latent commands on the GT). If a composite tried
to block on its own job's result, the GT would never drain the job body → 60s timeout deadlock.

Resolution: composites NEVER poll. They submit + return ``{job_id}``. The AI client polls
``job.status`` / ``job.result`` from outside the GT (external TCP socket on its own thread).

Chunk A — Source Control composites (1 tool)
=============================================

``sc.submit`` — commit + push a batch of changed files. Provider RPC (Perforce / Git) can take
5-60s on large changelists, so sync would block GT + listener thread.

Chunk B — Automation Test composites (1 tool)
==============================================

``test.run_automation`` — sequentially run a batch of automation tests. Each test can take
10s-15min; entire batches can run for hours. Sequential because FAutomationTestFramework only
tracks one "current" test at a time.

Chunk E — Live Coding composites (1 tool — Phase 6 wrap-up)
============================================================

``livecoding.recompile`` — drive ``ILiveCodingModule::Compile()`` for a set of UE modules. The
compile typically runs 5-30s; large changesets can hit the 180s internal cap. Returns the
ELiveCodingCompileResult enum value verbatim (Success/NoChanges/Failure/etc.) plus best-effort
patched_modules / failed_modules extracted from LogLiveCoding entries. Windows desktop editor
only — non-Windows OR non-editor builds raise -32048 LiveCodingDisabled. PIE-guarded — refuses
with -32027 PIEActive when PIE is running (per Epic's "LC requires PIE off" requirement).
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


# ─── test.run_automation (async-only — submits job, returns {job_id}) ──────────────────────────
@tool(
    name="test.run_automation",
    schema_in={
        "type": "object",
        "properties": {
            "test_names": {
                "type": "array",
                "minItems": 1,
                "items": {"type": "string"},
                "description": "Full test paths (exact case-sensitive match against "
                               "FAutomationTestFramework's FullTestPath, e.g. "
                               "'System.Engine.Maps.PIE_DefaultMap'). Enumerate via "
                               "test.list_automation_specs.",
            },
            "run_smoke_filter": {
                "type": "string",
                "description": "Optional EAutomationTestFlags filter name (e.g. 'SmokeFilter', "
                               "'EngineFilter', 'ProductFilter') applied via "
                               "SetRequestedTestFilter BEFORE the batch runs. Persists past the "
                               "batch — caller can reset via test.set_filter_flags afterward. "
                               "Unknown name → INVALID_PARAMS.",
            },
        },
        "required": ["test_names"],
    },
    schema_out={
        "type": "object",
        "properties": {"job_id": {"type": "string"}},
        "required": ["job_id"],
    },
    thread_safe=False,
    failure_modes=[
        {"code": "INVALID_PARAMS",
         "when": "test_names missing/empty/not array of strings OR run_smoke_filter present "
                 "but not a known flag name",
         "recovery": "Pass test_names as a non-empty list of FullTestPath strings; if using "
                     "run_smoke_filter, choose from EAutomationTestFlags_GetTestFlagsMap names "
                     "(e.g. 'SmokeFilter', 'EngineFilter', 'ProductFilter')."},
        {"code": "TEST_NOT_FOUND",
         "when": "Any test_name entry doesn't exist in the framework's registry — ENTIRE batch "
                 "rejected pre-job (no partial submit)",
         "recovery": "Verify exact spelling via test.list_automation_specs; lookup is "
                     "case-sensitive against FullTestPath."},
        {"code": "JOB_SUBMIT_FAILED",
         "when": "Job registry refused (shutdown?)",
         "recovery": "Retry after 1s"},
    ],
)
def run_automation(args: Dict[str, Any]) -> Dict[str, Any]:
    """Async-only: submits job, returns {job_id}. Poll via job.status / job.result.

    Sequentially runs each test in ``test_names`` via FAutomationTestFramework on the game
    thread. Per-test execution captures errors / warnings into the result; the batch can be
    cancelled mid-flight via ``job.cancel`` (remaining tests go to ``skipped[]`` with reason
    'batch cancelled before this test ran').

    Inner result (returned by ``job.result`` once Succeeded)::

        {
          "succeeded": [{name, duration_secs, warning_count}],
          "failed":    [{name, duration_secs, error_count, errors[], warnings[],
                         cancelled_mid_test: bool}],
          "skipped":   [{name, reason}],
          "total":               int,
          "completed":           int,
          "failed_count":        int,
          "cancelled":           bool,
          "applied_filter":      str,     # run_smoke_filter echo (empty if not provided)
          "total_duration_secs": float
        }

    Each error / warning entry has the shape::

        {
          "type":      "error" | "warning" | "info",
          "message":   str,
          "context":   str,
          "filename":  str,    # source file where AddError/AddWarning was called
          "line":      int,    # source line; -1 if unknown
          "timestamp": str     # ISO 8601
        }

    No PIE guard — tests may themselves drive PIE start/stop via latent commands. Operators
    should ``pie.stop`` first if they want a clean editor-world start state.
    """
    if not isinstance(args, dict):
        raise ValueError("args: expected object")

    test_names = args.get("test_names")
    if not isinstance(test_names, list) or not test_names:
        raise ValueError("test_names: required non-empty array of FullTestPath strings")
    for i, name in enumerate(test_names):
        if not isinstance(name, str) or not name:
            raise ValueError(f"test_names[{i}]: must be a non-empty string")

    fwd: Dict[str, Any] = {"test_names": test_names}

    # Optional run_smoke_filter — pass through verbatim; C++ side validates against the
    # EAutomationTestFlags name table and surfaces -32602 on unknown.
    run_smoke_filter = args.get("run_smoke_filter")
    if run_smoke_filter is not None:
        if not isinstance(run_smoke_filter, str):
            raise ValueError("run_smoke_filter: must be a string if provided")
        if run_smoke_filter:
            fwd["run_smoke_filter"] = run_smoke_filter

    return dispatch_internal("test._run_automation_internal", fwd)


# ─── livecoding.recompile (async-only — submits job, returns {job_id}) ─────────────────────────
@tool(
    name="livecoding.recompile",
    schema_in={
        "type": "object",
        "properties": {
            "modules": {
                "type": "array",
                "minItems": 1,
                "items": {"type": "string"},
                "description": "UE module names to recompile (e.g. ['FatumGame', 'Barrage']) OR "
                               "['*'] for all dirty modules. Live Coding's own compile path "
                               "gracefully handles unknown module names (logged as warnings, not "
                               "errors) — we don't validate per-module existence at submit time.",
            },
        },
        "required": ["modules"],
    },
    schema_out={
        "type": "object",
        "properties": {"job_id": {"type": "string"}},
        "required": ["job_id"],
    },
    thread_safe=False,
    failure_modes=[
        {"code": "INVALID_PARAMS",
         "when": "modules missing/empty/not array of strings",
         "recovery": "Pass modules as a non-empty list of UE module-name strings, "
                     "or ['*'] for all dirty modules."},
        {"code": "PIE_ACTIVE",
         "when": "GEditor->PlayWorld != nullptr — Live Coding requires PIE off (D11; UE Editor "
                 "constraint, not a bridge policy)",
         "recovery": "Call pie.stop first, then retry livecoding.recompile."},
        {"code": "LIVE_CODING_DISABLED",
         "when": "Live Coding module is not loadable (non-Windows platform, non-editor build, "
                 "or LiveCoding console cannot be enabled in the current session)",
         "recovery": "Launch the editor on Windows desktop without -nolivecoding; enable Live "
                     "Coding via Editor Preferences → General → Live Coding."},
        {"code": "JOB_SUBMIT_FAILED",
         "when": "Job registry refused (shutdown?)",
         "recovery": "Retry after 1s"},
    ],
)
def recompile(args: Dict[str, Any]) -> Dict[str, Any]:
    """Async-only: submits job, returns {job_id}. Poll via job.status / job.result.

    Invokes ``ILiveCodingModule::Compile()`` on the game thread for the specified UE modules,
    polling ``IsCompiling()`` + ``Tick()`` until the OnPatchCompleteDelegate fires (or 180s cap).
    The actual compile work happens inside Live Coding's external console subprocess (LC_LiveCodingConsole.exe
    on Windows) which patches the .dll in-place. Returns once patch_complete fires.

    Inner result (returned by ``job.result`` once Succeeded)::

        {
          "recompiled":          bool,    // true if ELiveCodingCompileResult::Success returned
          "result":              str,     // enum name: "Success" | "NoChanges" | "InProgress"
                                          //  | "CompileStillActive" | "NotStarted"
                                          //  | "Failure" | "Cancelled"
          "patched_modules":     [str],   // best-effort, parsed from LogLiveCoding entries
          "failed_modules":      [{name, errors[]}],  // best-effort, also log-scraped
          "duration_secs":       float,   // wall-clock from compile start to delegate fire
          "wait_timeout_hit":    bool,    // true if 180s cap reached before delegate fired
          "live_coding_version": str,     // "LiveCoding" (module name echo)
          "modules_requested":   [str]    // echo of the input list
        }

    Per-tool caveats:
      - Live Coding has no Cancel API in UE 5.7 — ``job.cancel`` stops polling but the compile
        continues in the background. Next recompile attempt may see "CompileStillActive" until
        the prior run drains.
      - patched_modules / failed_modules are extracted by string-matching LogLiveCoding entries
        ("Compilation done for ..." / "Failed to patch ..."). Patterns may shift between UE
        versions — caller can inspect raw logs via ``log.tail category="LogLiveCoding"`` for
        authoritative diagnostics.
      - Live Coding's compile path runs in an external subprocess and writes the patched .dll
        directly into the running editor's address space. If the patch fails, the editor's
        in-memory UClass* state may be inconsistent — restart recommended for any "Failure"
        result with patched_modules non-empty.

    PIE guard: refuses with -32027 PIEActive when PIE is running (Epic constraint, not a bridge
    policy). Caller MUST ``pie.stop`` first.
    """
    if not isinstance(args, dict):
        raise ValueError("args: expected object")

    modules = args.get("modules")
    if not isinstance(modules, list) or not modules:
        raise ValueError("modules: required non-empty array of UE module-name strings "
                         "(or ['*'] for all dirty)")
    for i, m in enumerate(modules):
        if not isinstance(m, str) or not m:
            raise ValueError(f"modules[{i}]: must be a non-empty string")

    fwd: Dict[str, Any] = {"modules": modules}
    return dispatch_internal("livecoding._recompile_internal", fwd)
