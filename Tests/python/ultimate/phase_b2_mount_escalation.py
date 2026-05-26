#!/usr/bin/env python3
"""Phase B2 — Writeable-mount escalation hostile probes.

Goal: every tool that writes to disk MUST guard against writes to
read-only mount points (/Engine, /Engine/Script, /Engine/Memory,
/Plugins/<engine-plugin>, etc.). User input that names a forbidden
mount target MUST be rejected with -32010 InvalidPath, not silently
allow the editor to corrupt engine assets.

Plugin fixes already shipped (S+7, S+11, S+12) wrapped writeable-mount
guards around mesh.duplicate, level.duplicate, and direct IsWriteable
calls in disk-write code. This phase sweeps for missing guards across
all asset-write tools.

Probe template: target each write tool with a path that escalates into
a read-only mount. Expected outcome:
  - -32010 InvalidPath        (mount-point guard fires)
  - -32027 PIEActive          (PIE blocks before mount check)
  - -32004 ObjectNotFound     (handler short-circuits at lookup)
  - -32011 WrongClass         (path normalizer rejects format)
  - any structured Bridge error code
  - FAIL — if write succeeds (would write to engine OR silently no-op)
  - FAIL — if editor crashes

Each case: probe → liveness check → cleanup if write went through.

Mount escalation paths
----------------------

  /Engine/EditorBlueprintResources/X        — engine BP folder
  /Engine/EditorMeshes/X                    — engine mesh folder
  /Engine/Maps/X                            — engine map folder
  /Engine/Materials/X                       — engine material folder
  /Engine/Script/Engine.X                   — script reflection (NO writes)
  /Plugins/Editor/X                         — editor plugin mount
  /Memory/X                                 — memory-only mount
  /Game/../Engine/X                         — relative-path escape attempt
  /Game/../../Engine/X                      — multi-level escape

Tools probed
------------

  asset.create_data_asset      dest path → engine mount
  asset.create                  dest path → engine mount
  asset.batch_metadata          paths array → engine mount
  asset.set_property            path → engine mount
  bp.create_blueprint           dest_path → engine mount
  bp.duplicate                  dest_path → engine mount
  mesh.duplicate                dest_path → engine mount (S+11 covered)
  level.duplicate               dest_map → engine mount (S+12 covered)
  cb.duplicate                  dest_path → engine mount
  cb.move                       dest_folder → engine mount
  cb.rename                     new_path → engine mount
  cb.create_folder              path → engine mount (S+8 covered)
  cb.import                     dest_path → engine mount
  cb.bulk_import                dest_folder → engine mount
  curve.create_asset            path → engine mount
  data_table.create             — no creator tool, skip
  input.create_input_action     path → engine mount
  input.create_input_mapping_context path → engine mount
  ai.bb.create_asset            path → engine mount
  ai.bt.create_asset            path → engine mount
  ai.eqs.create_asset           path → engine mount
  niagara.create_system         (if exists)
  thumbnail.regen_to_disk       output_path → escape
  screenshot.capture_to_disk    output_path → escape (likely via PathEscape -32013)
  asset.get_thumbnail_to_disk   output_path → escape (-32013)
  cb.export                     dest_file → outside sandbox (-32013)

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

PHASE = "b2"
NAME = "mount_escalation"

# Engine mounts (must reject)
ENGINE_BP = "/Engine/EditorBlueprintResources/_PhT_HostileBP_"
ENGINE_MESH = "/Engine/EditorMeshes/_PhT_HostileMesh_"
ENGINE_MAP = "/Engine/Maps/_PhT_HostileMap_"
ENGINE_MATERIAL = "/Engine/Materials/_PhT_HostileMat_"
SCRIPT_MOUNT = "/Engine/Script/_PhT_HostileScript_"
MEMORY_MOUNT = "/Memory/_PhT_HostileMem_"
PLUGINS_ENGINE = "/Plugins/Editor/_PhT_HostilePlugin_"
RELATIVE_ESCAPE = "/Game/../Engine/_PhT_HostileEsc_"
DOUBLE_ESCAPE = "/Game/../../Engine/_PhT_HostileEscDeep_"

# Disk paths (for PathEscape -32013)
DISK_OUTSIDE = "C:/Windows/System32/_PhT_HostileEscDisk.png"
DISK_PARENT = "C:/Users/_PhT_HostileEscParent.png"


# Per-tool probe: (method, args_with_hostile_path, expected_codes, label)
PROBES: List[Tuple[str, Dict[str, Any], List[int], str]] = [
    # ── Asset creators ──────────────────────────────────────────────────
    ("asset.create_data_asset",
     {"path": f"{ENGINE_BP}{random_suffix(4)}",
      "class_path": "/Script/Engine.PrimaryDataAsset"},
     [-32010, -32011], "asset.create_data_asset → /Engine"),
    ("asset.create",
     {"path": f"{ENGINE_MAP}{random_suffix(4)}",
      "class_path": "/Script/Engine.World"},
     [-32010, -32011], "asset.create → /Engine/Maps"),
    ("bp.create_blueprint",
     {"dest_path": f"{ENGINE_BP}{random_suffix(4)}",
      "parent_class_path": "/Script/Engine.Actor"},
     [-32010, -32011], "bp.create_blueprint → /Engine/EditorBlueprintResources"),
    ("bp.create_blueprint",
     {"dest_path": f"{SCRIPT_MOUNT}{random_suffix(4)}",
      "parent_class_path": "/Script/Engine.Actor"},
     [-32010, -32011], "bp.create_blueprint → /Engine/Script (S+7)"),
    ("bp.create_blueprint",
     {"dest_path": f"{MEMORY_MOUNT}{random_suffix(4)}",
      "parent_class_path": "/Script/Engine.Actor"},
     [-32010, -32011], "bp.create_blueprint → /Memory (S+7)"),
    ("bp.create_blueprint",
     {"dest_path": f"{PLUGINS_ENGINE}{random_suffix(4)}",
      "parent_class_path": "/Script/Engine.Actor"},
     [-32010, -32011], "bp.create_blueprint → /Plugins/Editor"),

    # ── Mesh / Level / CB duplicate (S+11, S+12 covered) ───────────────
    ("mesh.duplicate",
     {"source_path": "/Engine/BasicShapes/Cube.Cube",
      "dest_path": f"{ENGINE_MESH}{random_suffix(4)}"},
     [-32010, -32011], "mesh.duplicate → /Engine (S+11)"),
    ("level.duplicate",
     {"source_map": "/Engine/Maps/Templates/OpenWorld",
      "dest_map": f"{ENGINE_MAP}{random_suffix(4)}"},
     [-32010, -32011, -32004], "level.duplicate → /Engine (S+12)"),
    ("cb.duplicate",
     {"source_path": "/Engine/BasicShapes/Cube.Cube",
      "dest_path": f"{ENGINE_MESH}{random_suffix(4)}"},
     [-32010, -32011], "cb.duplicate → /Engine"),

    # ── CB move/rename ──────────────────────────────────────────────────
    ("cb.rename",
     {"path": "/Game/_no_such_src",
      "new_path": f"{ENGINE_BP}{random_suffix(4)}"},
     [-32010, -32011, -32004], "cb.rename new_path → /Engine"),
    ("cb.move",
     {"paths": ["/Game/_no_such_src"],
      "dest_folder": f"{ENGINE_BP}{random_suffix(4)}"},
     [-32010, -32011, -32004], "cb.move dest_folder → /Engine"),

    # ── CB folder creation (S+8 PIE-guarded; also mount-guarded) ────────
    ("cb.create_folder",
     {"path": f"{ENGINE_MESH}{random_suffix(4)}"},
     [-32010, -32011, -32027], "cb.create_folder → /Engine"),

    # ── Input authoring (paths to engine mount) ────────────────────────
    ("input.create_input_action",
     {"path": f"{ENGINE_BP}{random_suffix(4)}",
      "value_type": "Boolean"},
     [-32010, -32011], "input.create_input_action → /Engine"),
    ("input.create_input_mapping_context",
     {"path": f"{ENGINE_BP}{random_suffix(4)}"},
     [-32010, -32011], "input.create_input_mapping_context → /Engine"),

    # ── AI asset creators ──────────────────────────────────────────────
    ("ai.bb.create_asset",
     {"path": f"{ENGINE_BP}{random_suffix(4)}"},
     [-32010, -32011], "ai.bb.create_asset → /Engine"),
    ("ai.bt.create_asset",
     {"path": f"{ENGINE_BP}{random_suffix(4)}"},
     [-32010, -32011], "ai.bt.create_asset → /Engine"),
    ("ai.eqs.create_asset",
     {"path": f"{ENGINE_BP}{random_suffix(4)}"},
     [-32010, -32011], "ai.eqs.create_asset → /Engine"),

    # ── Relative path escapes (../ tricks) ──────────────────────────────
    ("bp.create_blueprint",
     {"dest_path": RELATIVE_ESCAPE,
      "parent_class_path": "/Script/Engine.Actor"},
     [-32010, -32011, -32013], "bp.create_blueprint → relative ../Engine"),
    ("bp.create_blueprint",
     {"dest_path": DOUBLE_ESCAPE,
      "parent_class_path": "/Script/Engine.Actor"},
     [-32010, -32011, -32013], "bp.create_blueprint → relative ../../Engine"),

    # ── Disk-write sandbox escapes (PathEscape -32013) ──────────────────
    ("screenshot.capture_to_disk",
     {"output_path": DISK_OUTSIDE},
     [-32013, -32010, -32602], "screenshot.capture_to_disk → System32"),
    ("asset.get_thumbnail_to_disk",
     {"asset_path": "/Engine/BasicShapes/Cube.Cube",
      "output_path": DISK_OUTSIDE},
     [-32013, -32010, -32602], "asset.get_thumbnail_to_disk → System32"),
    ("cb.export",
     {"path": "/Engine/BasicShapes/Cube.Cube",
      "dest_file": DISK_PARENT},
     [-32013, -32010, -32602], "cb.export dest_file → C:/Users/"),
]


def main() -> int:
    if not preflight(PHASE):
        return 2

    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()
    fail_total = 0

    print(f"[B2] running {len(PROBES)} mount-escalation probes…", flush=True)
    for idx, (method, args, expected_codes, label) in enumerate(PROBES):
        t0 = time.monotonic()
        try:
            r = call(method, args, timeout=8.0)
        except Exception as e:
            r = {"_err": "exception", "_exc": str(e)}
        dt = (time.monotonic() - t0) * 1000.0
        c = err_code(r)
        alive = health(timeout=3.0)

        if not alive:
            log.case(label, "FAIL",
                     f"EDITOR DIED after {method}: code={c}",
                     alive=False, duration_ms=dt)
            log.write()
            print(f"  [B2] EDITOR CRASHED on {method}", file=sys.stderr)
            return 1

        crash = latest_crash_dump(since=crash_baseline)
        if crash:
            log.case(label, "FAIL",
                     f"CRASH DUMP after probe: {crash}",
                     alive=alive, duration_ms=dt, code=c)
            log.write()
            return 1

        if is_transport_failure(r):
            log.case(label, "FAIL",
                     f"transport: {r.get('_err')}",
                     alive=alive, duration_ms=dt)
            fail_total += 1
            continue

        if is_ok(r):
            # WRITE SUCCEEDED into hostile mount → SECURITY HOLE
            log.case(label, "FAIL",
                     "WRITE SUCCEEDED INTO RESTRICTED MOUNT — missing guard",
                     alive=alive, duration_ms=dt)
            fail_total += 1
            continue

        if c in expected_codes:
            log.case(label, "PASS",
                     f"guard fired: {c}: {err_message(r)[:50]}",
                     alive=alive, duration_ms=dt, code=c)
            continue

        # Any other Bridge error — XFAIL (rejected for wrong reason, but rejected)
        if c is not None and -32700 <= c <= -32000:
            log.case(label, "XFAIL",
                     f"rejected but unexpected code {c}: {err_message(r)[:50]}",
                     alive=alive, duration_ms=dt, code=c)
            continue

        log.case(label, "FAIL",
                 f"unknown response: code={c} msg={err_message(r)[:60]}",
                 alive=alive, duration_ms=dt, code=c)
        fail_total += 1

    summary = log.write()
    cc = summary["counts"]
    print()
    print(f"[B2] PASS={cc['PASS']} FAIL={cc['FAIL']} XFAIL={cc.get('XFAIL', 0)} TOTAL={cc['TOTAL']}")
    print(f"     log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    if fail_total > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
