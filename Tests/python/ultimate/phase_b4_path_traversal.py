#!/usr/bin/env python3
"""Phase B4 — Path traversal hostile probes.

Goal: every tool that accepts a path argument MUST normalize and
validate against directory traversal patterns. Bypasses can:
  - Read files outside the project sandbox (PathEscape)
  - Write to engine-restricted mounts via relative paths
  - Pass control characters to FName::Init (crash)
  - Crash UE's path parser on malformed input
  - Cause infinite loops in path normalization

Plugin fixes already shipped:
  - S+7  IsWriteableMountPoint blocks /Engine/Script/Memory writes
  - S+8  cb.create_folder PIE guard
  - S+11/S+12 mesh.duplicate / level.duplicate writeable-mount guards
  - S+16 FTopLevelAssetPath pre-validate (asset.search_by_class)

This phase sweeps for missed patterns: ../, ..\\, null bytes, URL
encoding, control chars, mixed slashes, drive letters, UNC paths,
empty segments, dot-only segments, name with embedded \\, etc.

Probe template per tool that accepts a path-like field:
  - Send each hostile path
  - Verify: editor alive, response is structured error (not ok=true)
  - PASS = -32010 InvalidPath / -32011 WrongClass / -32013 PathEscape
  - FAIL = write succeeded (silent corruption) or editor crashed

Hostile path patterns
---------------------

  rel_escape_1       /Game/../Engine/X
  rel_escape_2       /Game/../../Engine/X
  rel_escape_3       /Game/Foo/../../../Engine/X
  win_disk           C:/Windows/X
  unc_path           //server/share/X
  null_byte          /Game/X\x00.png
  control_char       /Game/X\x01\x02
  empty_segment      /Game//X
  dotdot_segment     /Game/./X
  drive_only         C:/
  url_encoded        /Game/%2E%2E/Engine/X
  mixed_slash        \\Game\\../Engine/X
  trailing_dotdot    /Game/X/..
  long_dotdot        /Game/../../../../../../../../../Engine/X
  embedded_backslash /Game/Foo\\Bar
  unicode_traversal  /Game/‮.png  (right-to-left override)
  null_only          \x00

Tools (sample — high-volume path consumers)
-------------------------------------------

  asset.exists                  asset_path
  asset.get_property            asset_path
  bp.create_blueprint           dest_path / parent_class_path
  asset.create_data_asset       path
  cb.duplicate                  source_path / dest_path
  cb.export                     path / dest_file (disk path → PathEscape)
  cb.import                     source_file (disk → PathEscape)
  thumbnail.regen               asset_path
  asset.get_thumbnail_to_disk   asset_path / output_path
  screenshot.capture_to_disk    output_path
  folder.create                 folder_path
  folder.delete                 folder_path
  actor.spawn                   class_path
  level.duplicate               source_map / dest_map

Exit codes: 0=PASS, 1=FAIL, 2=preflight fail.
"""

from __future__ import annotations

import sys
import time
from pathlib import Path
from typing import Any, Dict, List, Tuple

sys.path.insert(0, str(Path(__file__).parent))
from mcp_test_harness import (
    LOG_ROOT,
    TestLogger,
    call,
    err_code,
    err_message,
    health,
    is_ok,
    is_transport_failure,
    latest_crash_dump,
    preflight,
    random_suffix,
)

PHASE = "b4"
NAME = "path_traversal"


# Hostile path patterns. Some target /Game (UE virtual path), others target
# disk (for tools like cb.export/screenshot.capture_to_disk).
HOSTILE_VIRTUAL_PATHS = [
    ("/Game/../Engine/_PhT_B4_rel1", "rel_escape_1"),
    ("/Game/../../Engine/_PhT_B4_rel2", "rel_escape_2"),
    ("/Game/Foo/../../../Engine/_PhT_B4_rel3", "rel_escape_3"),
    ("/Game//_PhT_B4_emptyseg", "empty_segment"),
    ("/Game/./_PhT_B4_dot", "dot_segment"),
    ("/Game/%2E%2E/Engine/_PhT_B4_url", "url_encoded"),
    ("\\Game\\..\\Engine\\_PhT_B4_mixed", "mixed_slash"),
    ("/Game/_PhT_B4/..", "trailing_dotdot"),
    ("/Game/../../../../../../../../../Engine/_PhT_B4_deep", "long_dotdot"),
    ("/Game/Foo\\Bar_PhT_B4", "embedded_backslash"),
    ("/Game/X\x01\x02_PhT_B4", "control_chars"),
]

HOSTILE_DISK_PATHS = [
    ("C:/Windows/System32/_PhT_B4_disk1.png", "win_system_dir"),
    ("//server/share/_PhT_B4_unc.png", "unc_path"),
    ("C:/", "drive_root"),
    ("../../../../_PhT_B4_rel.png", "relative_above"),
    ("/_PhT_B4_root.png", "absolute_root"),
]


# (method, args_template, path_field_name, label_prefix, hostile_set)
# Note: folder.create / folder.delete are EXCLUDED — they're for world outliner
# FActorFolders (in-memory labels, not asset paths). Per CLAUDE.md they
# intentionally accept any string label and are idempotent. They cannot
# trigger path-traversal vulnerabilities because they never touch disk or
# the asset registry. Use cb.create_folder / cb.delete for asset-disk-folder
# operations — those properly normalize.
PROBES: List[Tuple[str, Dict[str, Any], str, str, str]] = [
    ("asset.exists", {}, "asset_path", "asset.exists", "virtual"),
    ("asset.get_property", {"property_path": "X"}, "asset_path",
     "asset.get_property", "virtual"),
    ("bp.create_blueprint",
     {"parent_class_path": "/Script/Engine.Actor"}, "dest_path",
     "bp.create_blueprint", "virtual"),
    ("asset.create_data_asset",
     {"class_path": "/Script/Engine.PrimaryDataAsset"}, "path",
     "asset.create_data_asset", "virtual"),
    ("cb.duplicate",
     {"dest_path": f"/Game/_PhT_B4_dst_{random_suffix(4)}"}, "source_path",
     "cb.duplicate source", "virtual"),
    ("cb.duplicate",
     {"source_path": "/Engine/BasicShapes/Cube.Cube"}, "dest_path",
     "cb.duplicate dest", "virtual"),
    # cb.create_folder is the asset-disk-folder variant (properly normalised).
    # folder.create is world-outliner labelling — excluded, see comment above.
    ("cb.create_folder", {}, "path", "cb.create_folder", "virtual"),
    ("actor.spawn", {"location": [0, 0, 0]}, "class_path",
     "actor.spawn", "virtual"),
    ("level.duplicate",
     {"source_map": "/Engine/Maps/Templates/OpenWorld"}, "dest_map",
     "level.duplicate dest", "virtual"),
    # Disk-path tools
    ("screenshot.capture_to_disk", {}, "output_path",
     "screenshot.capture_to_disk", "disk"),
    ("asset.get_thumbnail_to_disk",
     {"asset_path": "/Engine/BasicShapes/Cube.Cube"}, "output_path",
     "asset.get_thumbnail_to_disk", "disk"),
    ("cb.export",
     {"path": "/Engine/BasicShapes/Cube.Cube"}, "dest_file",
     "cb.export", "disk"),
]


def main() -> int:
    if not preflight(PHASE):
        return 2
    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()
    fail_total = 0

    total_probes = sum(
        len(HOSTILE_VIRTUAL_PATHS if kind == "virtual" else HOSTILE_DISK_PATHS)
        for (_, _, _, _, kind) in PROBES
    )
    print(f"[B4] running {total_probes} path-traversal probes "
          f"across {len(PROBES)} tools…", flush=True)

    for (method, base_args, path_field, label_prefix, kind) in PROBES:
        hostile_set = HOSTILE_VIRTUAL_PATHS if kind == "virtual" else HOSTILE_DISK_PATHS
        for (hostile_path, pattern_name) in hostile_set:
            label = f"{label_prefix} :: {pattern_name}"
            args = dict(base_args)
            args[path_field] = hostile_path

            t0 = time.monotonic()
            try:
                r = call(method, args, timeout=6.0)
            except Exception as e:
                r = {"_err": "exception", "_exc": str(e)}
            dt = (time.monotonic() - t0) * 1000.0
            c = err_code(r)
            alive = health(timeout=3.0)

            if not alive:
                log.case(label, "FAIL", f"EDITOR DIED on {method} with {pattern_name}",
                         alive=False, duration_ms=dt)
                log.write()
                print(f"  [B4] EDITOR CRASHED on {method} :: {pattern_name}",
                      file=sys.stderr)
                return 1
            crash = latest_crash_dump(since=crash_baseline)
            if crash:
                log.case(label, "FAIL", f"CRASH DUMP: {crash}",
                         alive=alive, duration_ms=dt, code=c)
                log.write()
                return 1
            if is_transport_failure(r):
                log.case(label, "FAIL", f"transport: {r.get('_err')}",
                         alive=alive, duration_ms=dt)
                fail_total += 1
                continue
            if is_ok(r):
                # Should NEVER happen for hostile paths
                log.case(label, "FAIL",
                         "HANDLER ACCEPTED HOSTILE PATH — no normalization guard",
                         alive=alive, duration_ms=dt)
                fail_total += 1
                continue
            # Any structured error is PASS — guard rejected the path
            if c is not None and -32700 <= c <= -32000:
                log.case(label, "PASS",
                         f"guard fired: {c}: {err_message(r)[:50]}",
                         alive=alive, duration_ms=dt, code=c)
                continue
            log.case(label, "FAIL",
                     f"unexpected response: code={c}: {err_message(r)[:60]}",
                     alive=alive, duration_ms=dt, code=c)
            fail_total += 1

    summary = log.write()
    cc = summary["counts"]
    print()
    print(f"[B4] PASS={cc['PASS']} FAIL={cc['FAIL']} XFAIL={cc.get('XFAIL', 0)} "
          f"TOTAL={cc['TOTAL']}")
    print(f"     log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    if fail_total > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
