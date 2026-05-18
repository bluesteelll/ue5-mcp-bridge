"""Phase 2 Days 11-12 — Category D Python composite tools (6 tools).

These are THIN wrappers around the C++ Asset Registry / Content Browser tools
registered in `AssetRegistryTools.cpp` / `ContentBrowserTools.cpp` / `AssetCompositeTools.cpp`.

Composites either:
  (a) Route to a single C++ internal handler in one round-trip (find_unused,
      size_report, batch_metadata_async) — recommended path for any composite that
      would otherwise need per-asset iteration.
  (b) Loop over a small bounded set of C++ calls in pure Python (batch_metadata sync,
      find_broken_references, find_duplicates_by_name) — acceptable for sub-200
      iteration counts where Lane B latency keeps round-trip cost down.

All composites are registered as ``thread_safe=False`` because they may invoke
asynchronous tools (batch_metadata_async returns a job_id) or chain multiple round-trips.
"""

from __future__ import annotations

import collections
from typing import Any, Dict, List

from MCPTools.registry import tool
from MCPTools.tools.asset_tools import (
    basename_no_class,
    dispatch_internal,
    wait_for_job_and_return_result,
)


# ─── asset.find_unused (thin wrapper around _find_unused_internal) ──────────────────────────
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
        "properties": {
            "unused": {"type": "array"},
            "scanned_count": {"type": "integer"},
        },
        "required": ["unused", "scanned_count"],
    },
    thread_safe=False,
    failure_modes=[
        {"code": "INVALID_PATH", "when": "any package_paths malformed", "recovery": "/Game/..."},
        {"code": "QUERY_TOO_LARGE", "when": "scope contains more than 5000 entries",
         "recovery": "Narrow package_paths or run multiple smaller calls"},
    ],
)
def find_unused(args: Dict[str, Any]) -> Dict[str, Any]:
    """Static-analysis only — see C++ Tool_FindUnusedInternal description for caveats.

    Submits the AR-enumeration work as a game-thread-required job, polls ``job.result`` until
    terminal, and returns the inner ``{unused, scanned_count}`` payload. The two-stage flow
    (submit + poll) is required because the sync handler runs on the listener thread (Lane B)
    while the actual enumeration must run on the game thread per UE 5.7's AR contract — and a
    direct Lane-A handler would deadlock the composite (game thread blocked on socket.recv).
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

    submit_resp = dispatch_internal("asset._find_unused_internal", fwd)
    job_id = submit_resp.get("job_id")
    if not isinstance(job_id, str) or not job_id:
        raise RuntimeError(
            f"asset.find_unused: handler returned no job_id (got {submit_resp!r})"
        )
    return wait_for_job_and_return_result(job_id, timeout_s=120.0)


# ─── asset.size_report (thin wrapper around _size_report_internal) ──────────────────────────
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
        "properties": {
            "top": {"type": "array"},
            "total_bytes": {"type": "integer"},
        },
        "required": ["top", "total_bytes"],
    },
    thread_safe=False,
    failure_modes=[
        {"code": "INVALID_PATH", "when": "malformed", "recovery": "/Game/..."},
    ],
)
def size_report(args: Dict[str, Any]) -> Dict[str, Any]:
    """Returns top N largest assets by on-disk size + total.

    Submits a game-thread-required AR-enumeration job, polls ``job.result`` until terminal.
    Same submit + poll flow as ``find_unused`` for the same deadlock-avoidance reason.
    """
    package_paths = args.get("package_paths")
    if not isinstance(package_paths, list) or not package_paths:
        raise ValueError("package_paths: required non-empty array")

    fwd: Dict[str, Any] = {"package_paths": package_paths}
    if "top_n" in args:
        fwd["top_n"] = args["top_n"]

    submit_resp = dispatch_internal("asset._size_report_internal", fwd)
    job_id = submit_resp.get("job_id")
    if not isinstance(job_id, str) or not job_id:
        raise RuntimeError(
            f"asset.size_report: handler returned no job_id (got {submit_resp!r})"
        )
    return wait_for_job_and_return_result(job_id, timeout_s=120.0)


# ─── asset.batch_metadata (sync, ≤200 paths) ────────────────────────────────────────────────
@tool(
    name="asset.batch_metadata",
    schema_in={
        "type": "object",
        "properties": {
            "paths": {"type": "array", "items": {"type": "string"}, "minItems": 1, "maxItems": 200},
        },
        "required": ["paths"],
    },
    schema_out={
        "type": "object",
        "properties": {
            "assets": {"type": "array"},
            "failed": {"type": "array"},
        },
        "required": ["assets", "failed"],
    },
    thread_safe=False,
    failure_modes=[
        {"code": "INVALID_PATH", "when": "any path malformed", "recovery": "/Game/..."},
        {"code": "INPUT_TOO_LARGE", "when": "paths.length > 200",
         "recovery": "Use asset.batch_metadata_async for larger batches"},
    ],
)
def batch_metadata(args: Dict[str, Any]) -> Dict[str, Any]:
    """Synchronous metadata fetch for ≤200 paths. Loops asset.metadata per path."""
    paths = args.get("paths")
    if not isinstance(paths, list) or not paths:
        raise ValueError("paths: required non-empty array")
    if len(paths) > 200:
        # Hard cap surfaced as INPUT_TOO_LARGE per the schema. Python doesn't have direct
        # access to MCP error codes, so raise an exception whose message is recognised by
        # the CallPythonTool wrapper as INPUT_TOO_LARGE.
        raise ValueError(
            f"INPUT_TOO_LARGE: paths.length={len(paths)} > 200 — use asset.batch_metadata_async"
        )

    assets: List[Dict[str, Any]] = []
    failed: List[Dict[str, Any]] = []
    for path in paths:
        if not isinstance(path, str):
            failed.append({"path": str(path), "error": "non-string entry"})
            continue
        try:
            entry = dispatch_internal("asset.metadata", {"path": path})
            assets.append(entry)
        except RuntimeError as exc:
            failed.append({"path": path, "error": str(exc)})

    return {"assets": assets, "failed": failed}


# ─── asset.batch_metadata_async (returns {job_id}) ──────────────────────────────────────────
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
    """Async variant: submits a worker-pool job, returns job_id. Lane B-safe body."""
    paths = args.get("paths")
    if not isinstance(paths, list) or not paths:
        raise ValueError("paths: required non-empty array")

    return dispatch_internal("asset._batch_metadata_internal", {"paths": paths})


# ─── asset.find_broken_references (Day 12, Python iteration is acceptable) ──────────────────
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
        "properties": {
            "broken": {"type": "array"},
            "scanned_count": {"type": "integer"},
        },
        "required": ["broken", "scanned_count"],
    },
    thread_safe=False,
    failure_modes=[
        {"code": "INVALID_PATH", "when": "malformed paths", "recovery": "/Game/..."},
        {"code": "QUERY_TOO_LARGE", "when": "scan would exceed 5000 entries",
         "recovery": "Narrow package_paths"},
    ],
)
def find_broken_references(args: Dict[str, Any]) -> Dict[str, Any]:
    """For each asset in scope: walk hard deps; flag any dep whose package is missing from AR.

    Per-asset round-trips — acceptable because scope is typically narrow (recent subfolder)
    and Lane B keeps each round-trip sub-millisecond. If profiling shows it's slow in
    practice, promote to C++ internal `asset._find_broken_references_internal` later.
    """
    package_paths = args.get("package_paths")
    if not isinstance(package_paths, list) or not package_paths:
        raise ValueError("package_paths: required non-empty array")

    # First: enumerate all assets in scope using asset.list with recursive_paths=true.
    # Page through until cursor exhausted, accumulating up to a 5000-entry cap.
    all_assets: List[Dict[str, Any]] = []
    next_token = None
    cap = 5000

    while True:
        list_args: Dict[str, Any] = {
            "filter": {
                "package_paths": package_paths,
                "recursive_paths": True,
            },
            "page_size": 200,
        }
        if next_token is not None:
            list_args["page_token"] = next_token

        try:
            page = dispatch_internal("asset.list", list_args)
        except RuntimeError as exc:
            # Surface the underlying error verbatim (OVERLY_BROAD_QUERY etc.)
            raise

        page_assets = page.get("assets", [])
        all_assets.extend(page_assets)
        if len(all_assets) > cap:
            raise ValueError(
                f"QUERY_TOO_LARGE: scope exceeds {cap} entries (currently {len(all_assets)})"
            )

        next_token = page.get("next_page_token")
        if not next_token:
            break

    # For each asset, query hard dependents and check each dep package exists.
    broken: List[Dict[str, Any]] = []
    for asset in all_assets:
        asset_path = asset.get("asset_path")
        if not isinstance(asset_path, str):
            continue

        missing_paths: List[str] = []
        # Page through dependents.
        dep_token = None
        while True:
            dep_args: Dict[str, Any] = {
                "path": asset_path,
                "include_hard": True,
                "include_soft": False,
                "page_size": 200,
            }
            if dep_token is not None:
                dep_args["page_token"] = dep_token

            try:
                dep_page = dispatch_internal("asset.find_dependents", dep_args)
            except RuntimeError:
                # If find_dependents fails for this asset, treat as zero deps; skip.
                break

            for dep_entry in dep_page.get("dependents", []):
                dep_pkg = dep_entry.get("package_path")
                if not isinstance(dep_pkg, str) or not dep_pkg:
                    continue
                # /Script/ dependencies are native classes — always present, skip.
                if dep_pkg.startswith("/Script/"):
                    continue
                try:
                    exists_result = dispatch_internal("asset.exists", {"path": dep_pkg})
                    if not exists_result.get("exists", False):
                        missing_paths.append(dep_pkg)
                except RuntimeError:
                    # If asset.exists itself errors (malformed dep path), treat as missing.
                    missing_paths.append(dep_pkg)

            dep_token = dep_page.get("next_page_token")
            if not dep_token:
                break

        if missing_paths:
            broken.append({"asset_path": asset_path, "missing_paths": missing_paths})

    return {"broken": broken, "scanned_count": len(all_assets)}


# ─── asset.find_duplicates_by_name (Day 12, Python group-by) ────────────────────────────────
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
        "properties": {"duplicates": {"type": "array"}},
        "required": ["duplicates"],
    },
    thread_safe=False,
    failure_modes=[
        {"code": "INVALID_PATH", "when": "malformed paths", "recovery": "/Game/..."},
    ],
)
def find_duplicates_by_name(args: Dict[str, Any]) -> Dict[str, Any]:
    """Group assets by short-name across scopes; emit groups with count>1."""
    package_paths = args.get("package_paths")
    if not isinstance(package_paths, list) or not package_paths:
        raise ValueError("package_paths: required non-empty array")

    ignore_class = bool(args.get("ignore_class", True))

    # Enumerate all assets in all scopes via paginated asset.list.
    all_assets: List[Dict[str, Any]] = []
    next_token = None
    while True:
        list_args: Dict[str, Any] = {
            "filter": {
                "package_paths": package_paths,
                "recursive_paths": True,
            },
            "page_size": 500,
        }
        if next_token is not None:
            list_args["page_token"] = next_token

        page = dispatch_internal("asset.list", list_args)
        all_assets.extend(page.get("assets", []))
        next_token = page.get("next_page_token")
        if not next_token:
            break

    # Group by basename. If ignore_class=False, the key includes the class path.
    groups: Dict[str, List[Dict[str, str]]] = collections.defaultdict(list)
    for asset in all_assets:
        asset_path = asset.get("asset_path")
        class_path = asset.get("class")
        if not isinstance(asset_path, str) or not isinstance(class_path, str):
            continue
        base = basename_no_class(asset_path)
        key = base if ignore_class else f"{base}::{class_path}"
        groups[key].append({"asset_path": asset_path, "class": class_path})

    duplicates: List[Dict[str, Any]] = []
    for key, entries in groups.items():
        if len(entries) <= 1:
            continue
        # When ignore_class=False, strip the class suffix back off for the wire `name` field.
        name = key.split("::", 1)[0] if "::" in key else key
        duplicates.append({"name": name, "paths": entries})

    return {"duplicates": duplicates}
