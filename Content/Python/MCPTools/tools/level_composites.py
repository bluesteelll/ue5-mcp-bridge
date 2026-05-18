"""Phase 3 Days 11-14 — Category D Python composite tools (5 tools).

**ALL composites are async-only — they return ``{job_id}`` and the AI client polls externally
via ``job.status`` / ``job.result``.**

These are THIN wrappers around the C++ Level/Actor internal handlers registered in
``LevelCompositeTools.cpp``. Each composite is one line: validate args (when necessary) +
``dispatch_internal('level._..._internal' | 'actor._..._internal', args)`` + return the
``{job_id}`` envelope.

Why all-async?
==============

The composite Python function runs inside ``FMCPPythonEval::CallPythonTool`` which executes on
the **game thread** (Python's GIL is pinned to the GT). Three things conspire to require the
all-async pattern:

  1. Game thread ownership — once the composite enters, the GT is blocked until the composite
     returns.
  2. UE 5.7 ``UWorld::Actors`` / actor enumeration require the GT.
  3. Job-body GT dispatch — a job submitted with ``bGameThreadRequired=true`` cannot run while
     the GT is still owned by the composite.

If a composite tries to block on its OWN job's result (the pre-Phase-2-Hotfix-3 pattern), the
GT can never drain the job body → 60s timeout deadlock. Promoting ``job.result`` to Lane B
didn't help — the composite STILL owns the GT while sleeping between polls.

Resolution: composites NEVER poll. They submit + return ``{job_id}``. The AI client polls
``job.status`` / ``job.result`` from outside the GT (external TCP socket on its own thread).
This is the pattern ``asset.batch_metadata_async`` has used since Phase 2 Day 12 — it always
worked. All Phase 2/3 composites now follow it uniformly.

Output schemas
==============

Every composite returns the SAME shape:

  ``{"job_id": "<uuid>"}``

The AI client then calls ``job.result {"job_id": ..., "wait_timeout_s": <N>}`` (Lane B, returns
when the job reaches terminal state) and reads ``.result`` from the response. Inner result
payloads match the schema documented on each composite below — that's what the AI client gets
back from ``job.result``, not from this composite call directly.

Caps
====

  - ``level.full_actor_dump`` / ``level.find_actors_with_class`` — MAX_ACTORS_PER_DUMP=5000
    (enforced inside C++ body, not at submit time because counting requires the GT). AI client
    sees Failed with the OVERLY_BROAD_QUERY message on first poll.
  - ``actor.batch_spawn`` / ``actor.batch_destroy`` / ``actor.batch_set_property`` —
    MAX_BATCH_ITEMS=1000 (enforced at C++ submit time — pure array-length check is Lane B-safe).
    Surfaced as INPUT_TOO_LARGE (-32017) synchronously, no job created.

PIE behaviour
=============

All 3 batch_* composites refuse to mutate during PIE. The PIE-guard fires INSIDE the C++ job
body (not at submit time) because PIE state can transition between listener-thread submit and
game-thread execution. Surfaced as PIE_ACTIVE (-32027) with the frozen message from
``kMCPMessagePIEActive``. The 2 read composites (``level.full_actor_dump`` /
``level.find_actors_with_class``) work transparently in PIE — they see the editor world (NOT
the PIE world); a future ``pie.full_actor_dump`` would target ``GEditor->PlayWorld``.
"""

from __future__ import annotations

from typing import Any, Dict

from MCPTools.registry import tool
from MCPTools.tools.asset_tools import dispatch_internal


# ─── level.full_actor_dump (async-only — submits job, returns {job_id}) ─────────────────────
@tool(
    name="level.full_actor_dump",
    schema_in={
        "type": "object",
        "properties": {
            "map_path": {
                "type": "string",
                "description": "Sublevel package path. Empty/omitted = editor world's persistent level.",
            },
        },
    },
    schema_out={
        "type": "object",
        "properties": {"job_id": {"type": "string"}},
        "required": ["job_id"],
    },
    thread_safe=False,
    failure_modes=[
        {"code": "OVERLY_BROAD_QUERY",
         "when": "level contains > 5000 actors",
         "recovery": "Use level.get_persistent_level_actors with pagination instead."},
        {"code": "LEVEL_NOT_FOUND",
         "when": "map_path doesn't resolve to a loaded sublevel",
         "recovery": "Call level.list_loaded to enumerate, then pick a valid map_path."},
        {"code": "WORLD_PARTITION_NOT_SUPPORTED",
         "when": "target world is partitioned",
         "recovery": "Phase 5 will ship wp.* tools for partitioned maps."},
        {"code": "JOB_SUBMIT_FAILED",
         "when": "Job registry not initialized",
         "recovery": "Retry after 1s"},
    ],
)
def full_actor_dump(args: Dict[str, Any]) -> Dict[str, Any]:
    """Async-only: submits job, returns {job_id}. Poll via job.status / job.result.

    Dumps a compact ~200 B/actor summary for every actor in the resolved level. Capped at
    MAX_ACTORS_PER_DUMP=5000 — the cap protects against unbounded /Game scans that would
    blow the 64 MiB wire frame limit AND lock the game thread for minutes.

    Inner result (returned by ``job.result`` once Succeeded)::

        {
          "map_path":  "/Game/Maps/X",
          "actors":    [{"actor_path", "class", "label", "location",
                         "rotation", "tag_count", "component_count"}, ...],
          "total":     N
        }

    For deeper per-actor data (components, tags array, owner chain) use ``actor.get`` on
    individual actor_path values from this dump.
    """
    # No args required — empty/missing map_path defaults to persistent level inside the body.
    fwd: Dict[str, Any] = {}
    if isinstance(args, dict) and isinstance(args.get("map_path"), str):
        fwd["map_path"] = args["map_path"]
    return dispatch_internal("level._full_actor_dump_internal", fwd)


# ─── level.find_actors_with_class (async-only — submits job, returns {job_id}) ──────────────
@tool(
    name="level.find_actors_with_class",
    schema_in={
        "type": "object",
        "properties": {
            "class_path": {
                "type": "string",
                "description": "/Script/<Module>.<ClassName> OR /Game/.../BP_X.BP_X_C",
            },
            "recursive_classes": {
                "type": "boolean",
                "default": True,
                "description": "true = IsA semantics (includes subclasses); false = exact class match.",
            },
            "level_paths": {
                "type": "array",
                "items": {"type": "string"},
                "description": "Optional: restrict search to these sublevel package paths. "
                               "Absent = walk every loaded level.",
            },
        },
        "required": ["class_path"],
    },
    schema_out={
        "type": "object",
        "properties": {"job_id": {"type": "string"}},
        "required": ["job_id"],
    },
    thread_safe=False,
    failure_modes=[
        {"code": "INVALID_CLASS_PATH",
         "when": "class_path doesn't start with '/' or contains '\\'",
         "recovery": "Use /Script/<Module>.<Class> or /Game/.../BP.BP_C form."},
        {"code": "CLASS_NOT_FOUND",
         "when": "LoadObject(class_path) failed even after appending _C",
         "recovery": "Check the path matches an actually-loaded class; Blueprints need _C suffix."},
        {"code": "WRONG_CLASS_FAMILY",
         "when": "class_path resolves to a non-AActor UClass",
         "recovery": "Pass an AActor subclass — UComponents are listed via actor.list_components."},
        {"code": "OVERLY_BROAD_QUERY",
         "when": "candidate scope > 5000 actors",
         "recovery": "Pass level_paths to narrow OR use actor.find_by_class with pagination."},
        {"code": "JOB_SUBMIT_FAILED",
         "when": "Job registry not initialized",
         "recovery": "Retry after 1s"},
    ],
)
def find_actors_with_class(args: Dict[str, Any]) -> Dict[str, Any]:
    """Async-only: submits job, returns {job_id}. Poll via job.status / job.result.

    Enumerates actors matching ``class_path`` across all loaded levels (or restricted via
    ``level_paths``). Capped at MAX_ACTORS_PER_DUMP=5000 candidate actors — narrow with
    level_paths or use ``actor.find_by_class`` for paginated single-page reads if you expect
    larger result sets.

    Inner result (returned by ``job.result`` once Succeeded)::

        {
          "class_path":    "/Script/Engine.Light",
          "actors":        [{"actor_path", "class", ...summary, ...}, ...],
          "total":         N,
          "scanned_count": M  // candidate actors examined (matches loaded-world size)
        }
    """
    if not isinstance(args, dict):
        raise ValueError("args: expected object")
    class_path = args.get("class_path")
    if not isinstance(class_path, str) or not class_path:
        raise ValueError("class_path: required non-empty string")

    fwd: Dict[str, Any] = {"class_path": class_path}
    if "recursive_classes" in args:
        fwd["recursive_classes"] = bool(args["recursive_classes"])
    if "level_paths" in args:
        if not isinstance(args["level_paths"], list):
            raise ValueError("level_paths: expected array of strings")
        fwd["level_paths"] = args["level_paths"]
    return dispatch_internal("level._find_actors_with_class_internal", fwd)


# ─── actor.batch_spawn (async-only — submits job, returns {job_id}) ─────────────────────────
@tool(
    name="actor.batch_spawn",
    schema_in={
        "type": "object",
        "properties": {
            "spawns": {
                "type": "array",
                "minItems": 1,
                "maxItems": 1000,
                "items": {
                    "type": "object",
                    "properties": {
                        "class_path": {"type": "string"},
                        "location":   {"type": "object",
                                       "properties": {"x": {"type": "number"},
                                                      "y": {"type": "number"},
                                                      "z": {"type": "number"}}},
                        "rotation":   {"type": "object",
                                       "properties": {"pitch": {"type": "number"},
                                                      "yaw":   {"type": "number"},
                                                      "roll":  {"type": "number"}}},
                        "scale":      {"type": "object",
                                       "properties": {"x": {"type": "number"},
                                                      "y": {"type": "number"},
                                                      "z": {"type": "number"}}},
                        "label":      {"type": "string"},
                        "name":       {"type": "string"},
                    },
                    "required": ["class_path"],
                },
            },
        },
        "required": ["spawns"],
    },
    schema_out={
        "type": "object",
        "properties": {"job_id": {"type": "string"}},
        "required": ["job_id"],
    },
    thread_safe=False,
    failure_modes=[
        {"code": "INPUT_TOO_LARGE",
         "when": "spawns.length > 1000",
         "recovery": "Split into multiple batches of <= 1000."},
        {"code": "PIE_ACTIVE",
         "when": "PIE is running when the job body executes",
         "recovery": "Stop PIE first; Phase 5 will ship pie.* tools for PIE-world mutation."},
        {"code": "JOB_SUBMIT_FAILED",
         "when": "Job registry not initialized",
         "recovery": "Retry after 1s"},
    ],
)
def batch_spawn(args: Dict[str, Any]) -> Dict[str, Any]:
    """Async-only: submits job, returns {job_id}. Poll via job.status / job.result.

    Spawn up to 1000 actors in a single async job. Per-spawn failure does NOT halt the batch —
    each spawn gets its own FScopedTransaction (so users can undo individual rows) and any
    failure surfaces in the failed[] array of the inner result.

    Inner result (returned by ``job.result`` once Succeeded)::

        {
          "succeeded": [{"index", "actor_path"}, ...],
          "failed":    [{"index", "error_code", "error_message"}, ...],
          "total":     N
        }

    Per-spawn args mirror ``actor.spawn`` (class_path required, location/rotation/scale/label/
    name optional). Spawn-time options not currently supported in batch:
    target_level, template_actor_path, owner_path, collision_handling, spawn_in_folder, tags.
    Use ``actor.spawn`` per-item for those.
    """
    if not isinstance(args, dict):
        raise ValueError("args: expected object")
    spawns = args.get("spawns")
    if not isinstance(spawns, list) or not spawns:
        raise ValueError("spawns: required non-empty array")
    # Runtime enforcer for the schema's maxItems=1000 (schema is metadata for AI clients; the
    # C++ submitter also enforces, but we surface earlier here for cleaner error attribution).
    if len(spawns) > 1000:
        raise ValueError(
            f"INPUT_TOO_LARGE: spawns.length={len(spawns)} > 1000 — split into smaller batches"
        )
    return dispatch_internal("actor._batch_spawn_internal", {"spawns": spawns})


# ─── actor.batch_destroy (async-only — submits job, returns {job_id}) ───────────────────────
@tool(
    name="actor.batch_destroy",
    schema_in={
        "type": "object",
        "properties": {
            "actor_paths": {
                "type": "array",
                "minItems": 1,
                "maxItems": 1000,
                "items": {"type": "string"},
            },
        },
        "required": ["actor_paths"],
    },
    schema_out={
        "type": "object",
        "properties": {"job_id": {"type": "string"}},
        "required": ["job_id"],
    },
    thread_safe=False,
    failure_modes=[
        {"code": "INPUT_TOO_LARGE",
         "when": "actor_paths.length > 1000",
         "recovery": "Split into multiple batches of <= 1000."},
        {"code": "PIE_ACTIVE",
         "when": "PIE is running when the job body executes",
         "recovery": "Stop PIE first; Phase 5 will ship pie.* tools for PIE-world mutation."},
        {"code": "JOB_SUBMIT_FAILED",
         "when": "Job registry not initialized",
         "recovery": "Retry after 1s"},
    ],
)
def batch_destroy(args: Dict[str, Any]) -> Dict[str, Any]:
    """Async-only: submits job, returns {job_id}. Poll via job.status / job.result.

    Destroy up to 1000 actors. Idempotent per item — an actor_path that resolves to nothing
    (already destroyed) counts as success with ``was_already_gone=true`` in the entry.

    Inner result (returned by ``job.result`` once Succeeded)::

        {
          "succeeded": [{"index", "actor_path", "was_already_gone"}, ...],
          "failed":    [{"index", "error_code", "error_message"}, ...],
          "total":     N
        }

    Each destroy gets its own FScopedTransaction so users can undo individual rows.
    """
    if not isinstance(args, dict):
        raise ValueError("args: expected object")
    actor_paths = args.get("actor_paths")
    if not isinstance(actor_paths, list) or not actor_paths:
        raise ValueError("actor_paths: required non-empty array")
    if len(actor_paths) > 1000:
        raise ValueError(
            f"INPUT_TOO_LARGE: actor_paths.length={len(actor_paths)} > 1000 — split into smaller batches"
        )
    return dispatch_internal("actor._batch_destroy_internal", {"actor_paths": actor_paths})


# ─── actor.batch_set_property (async-only — submits job, returns {job_id}) ──────────────────
@tool(
    name="actor.batch_set_property",
    schema_in={
        "type": "object",
        "properties": {
            "mutations": {
                "type": "array",
                "minItems": 1,
                "maxItems": 1000,
                "items": {
                    "type": "object",
                    "properties": {
                        "actor_path":    {"type": "string"},
                        "property_path": {"type": "string"},
                        "value":         {"description": "Any JSON-serialisable value matching the property's type."},
                    },
                    "required": ["actor_path", "property_path", "value"],
                },
            },
        },
        "required": ["mutations"],
    },
    schema_out={
        "type": "object",
        "properties": {"job_id": {"type": "string"}},
        "required": ["job_id"],
    },
    thread_safe=False,
    failure_modes=[
        {"code": "INPUT_TOO_LARGE",
         "when": "mutations.length > 1000",
         "recovery": "Split into multiple batches of <= 1000."},
        {"code": "PIE_ACTIVE",
         "when": "PIE is running when the job body executes",
         "recovery": "Stop PIE first; Phase 5 will ship pie.* tools for PIE-world mutation."},
        {"code": "JOB_SUBMIT_FAILED",
         "when": "Job registry not initialized",
         "recovery": "Retry after 1s"},
    ],
)
def batch_set_property(args: Dict[str, Any]) -> Dict[str, Any]:
    """Async-only: submits job, returns {job_id}. Poll via job.status / job.result.

    Apply up to 1000 property mutations in a single async job. Each mutation goes through:

      1. Actor path resolution (rejected for PIE actors AND hidden sublevels)
      2. Property path resolution via ``FMCPReflection::ResolvePropertyPath`` (supports dotted
         paths for nested structs / object refs)
      3. The canonical 3-flag edit-const gate: ``CPF_BlueprintReadOnly | CPF_EditConst |
         CPF_DisableEditOnInstance``. Same gate as ``actor.set_property`` post-hotfix.
      4. RAII FMCPWritePropertyScope (PreEditChange + Modify + FScopedTransaction +
         PostEditChangeProperty) so each row is independently undoable.

    Per-mutation failure does NOT halt the batch — each entry's verdict goes in
    succeeded[] or failed[].

    Inner result (returned by ``job.result`` once Succeeded)::

        {
          "succeeded": [{"index", "actor_path", "property_path", "value": <post-write echo>}, ...],
          "failed":    [{"index", "error_code", "error_message"}, ...],
          "total":     N
        }

    Common failure codes per-row: -32004 OBJECT_NOT_FOUND (actor gone), -32005 PROPERTY_NOT_FOUND,
    -32006 PROPERTY_TYPE_MISMATCH (value JSON doesn't match property type), -32007
    PROPERTY_ACCESS_DENIED (edit-const).
    """
    if not isinstance(args, dict):
        raise ValueError("args: expected object")
    mutations = args.get("mutations")
    if not isinstance(mutations, list) or not mutations:
        raise ValueError("mutations: required non-empty array")
    if len(mutations) > 1000:
        raise ValueError(
            f"INPUT_TOO_LARGE: mutations.length={len(mutations)} > 1000 — split into smaller batches"
        )
    return dispatch_internal("actor._batch_set_property_internal", {"mutations": mutations})
