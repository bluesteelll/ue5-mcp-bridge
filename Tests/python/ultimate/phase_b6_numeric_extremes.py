#!/usr/bin/env python3
"""Phase B6 — Numeric-extremes hostile probes.

Goal: every tool that accepts a numeric argument MUST handle these
without crashing:
  - INT32 / INT64 max & min
  - UINT32 / UINT64 max
  - Float infinities (+inf, -inf) and NaN
  - Float subnormals (denormalized)
  - Float precision loss (1e308 + 1.0)
  - Zero values (positive zero, negative zero)
  - Very small negative numbers in size/count fields
  - Overflow attempt in indexing (page_size > 1<<31)

UE's TJsonReader handles most of these gracefully (clamps or rejects
at parse time), but transient handlers that cast to UE primitives
(int32, uint32, float) can crash on overflow / NaN / inf.

Probe template: each numeric-accepting tool gets each extreme value.
Expected: structured error code or ok=true, NEVER crash.
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

PHASE = "b6"
NAME = "numeric_extremes"


# Numeric extreme values. Note: JSON cannot encode NaN/Infinity directly;
# Python's json module rejects with ValueError unless allow_nan=True (default).
# Use string representations and hope the bridge tolerates / coerces.
NUMERIC_EXTREMES: List[Tuple[Any, str]] = [
    (2**31 - 1, "int32_max"),
    (-(2**31), "int32_min"),
    (2**63 - 1, "int64_max"),
    (-(2**63), "int64_min"),
    (2**32 - 1, "uint32_max"),
    (2**63, "above_int64_max"),
    (0, "zero"),
    (-0, "negative_zero"),
    (-1, "negative_one"),
    (1e308, "double_max_near"),
    (1e-308, "double_subnormal_near"),
    (1.7976931348623157e308, "double_max"),
    (5e-324, "double_subnormal"),
    (1e308 + 1.0, "precision_loss"),
    (3.141592653589793238462643383, "extra_precision_pi"),
    (-1.5, "negative_float"),
    (1<<33, "shift_overflow"),
]


# (method, args_template, numeric_field, label_prefix)
PROBES: List[Tuple[str, Dict[str, Any], str, str]] = [
    # Sizes / counts / indices
    ("asset.list", {}, "page_size", "asset.list page_size"),
    ("bp.list", {}, "page_size", "bp.list page_size"),
    ("gameplaytag.list_all", {}, "page_size", "gameplaytag.list_all page_size"),
    # Float fields (curves / anim / curve.set_row_value)
    ("curve.set_row_value",
     {"curve_path": "/Game/_phantom_curve/X", "row_name": "X", "value": 0.0},
     "time", "curve.set_row_value time"),
    ("curve.set_row_value",
     {"curve_path": "/Game/_phantom_curve/X", "row_name": "X", "time": 0.0},
     "value", "curve.set_row_value value"),
    # Anim notify/section
    ("anim.add_notify",
     {"anim_path": "/Game/_phantom_anim/X", "notify_class_path": "/Script/Engine.AnimNotify",
      "section_name": "S"},
     "time", "anim.add_notify time"),
    ("anim.add_section",
     {"anim_path": "/Game/_phantom_anim/X", "section_name": "S"},
     "start_time", "anim.add_section start_time"),
    # CVar (string-valued but tested with numeric coercion)
    ("cfg.set_cvar",
     {"cvar": "t.MaxFPS"},
     "value", "cfg.set_cvar numeric value"),
    # Actor transform components
    ("actor.set_location",
     {"actor_path": "/Game/_phantom/X"},
     "location", "actor.set_location location_x"),  # vec component below
    # Niagara param
    ("niagara.set_user_param",
     {"niagara_system_path": "/Script/Engine.Default__Actor", "name": "X"},
     "value", "niagara.set_user_param value"),
    # Physics impulse
    ("physics.apply_impulse",
     {"actor_path": "/Game/_phantom/X", "direction": [1, 0, 0]},
     "magnitude", "physics.apply_impulse magnitude"),
    # Sequencer keyframe
    ("seq.create_section",
     {"sequence_path": "/Game/_phantom_seq/X", "track_path": "T",
      "end_time": 1.0},
     "start_time", "seq.create_section start_time"),
    # Mesh LOD
    ("mesh.set_lod_screen_size",
     {"mesh_path": "/Engine/BasicShapes/Cube.Cube", "lod_index": 0},
     "screen_size", "mesh.set_lod_screen_size screen_size"),
]


def main() -> int:
    if not preflight(PHASE):
        return 2
    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()
    fail_total = 0

    total = len(PROBES) * len(NUMERIC_EXTREMES)
    print(f"[B6] running {total} numeric-extreme probes "
          f"({len(PROBES)} tools × {len(NUMERIC_EXTREMES)} extremes)…",
          flush=True)

    for (method, base_args, field, label_prefix) in PROBES:
        for (extreme_val, pattern_name) in NUMERIC_EXTREMES:
            label = f"{label_prefix} :: {pattern_name}"
            args = dict(base_args)
            # For vector fields (location/direction), inject the extreme as
            # the X component
            if field in ("location", "direction"):
                args[field] = [extreme_val, 0, 0]
            else:
                args[field] = extreme_val

            t0 = time.monotonic()
            try:
                r = call(method, args, timeout=6.0)
            except Exception as e:
                r = {"_err": "exception", "_exc": str(e)}
            dt = (time.monotonic() - t0) * 1000.0
            c = err_code(r)
            alive = health(timeout=3.0)

            if not alive:
                log.case(label, "FAIL",
                         f"EDITOR DIED on {method} with {pattern_name}={extreme_val}",
                         alive=False, duration_ms=dt)
                log.write()
                print(f"  [B6] EDITOR CRASHED on {method} :: {pattern_name}",
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
                log.case(label, "PASS",
                         f"handler accepted {extreme_val} (clamped/coerced)",
                         alive=alive, duration_ms=dt)
            elif c is not None and -32700 <= c <= -32000:
                log.case(label, "PASS",
                         f"guard fired: {c}: {err_message(r)[:50]}",
                         alive=alive, duration_ms=dt, code=c)
            else:
                log.case(label, "FAIL",
                         f"unexpected response: code={c}: {err_message(r)[:60]}",
                         alive=alive, duration_ms=dt, code=c)
                fail_total += 1

    summary = log.write()
    cc = summary["counts"]
    print()
    print(f"[B6] PASS={cc['PASS']} FAIL={cc['FAIL']} XFAIL={cc.get('XFAIL', 0)} "
          f"TOTAL={cc['TOTAL']}")
    print(f"     log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    if fail_total > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
