#!/usr/bin/env python3
"""Phase B3 — PIE-state guard sweep.

Goal: every tool that mutates editor assets MUST reject calls while
PIE is active (-32027 PIEActive), and every runtime-introspection tool
MUST reject calls when PIE is NOT running (-32038 PIENotActive).
Bypassing either creates use-after-free / saved-asset-corruption
opportunities (see S+8, S+9 history).

This phase does NOT start PIE — that's done in dedicated PIE-stress
phases. Instead it audits the current state and probes the boundary:

  Mode 1 — PIE OFF:
    Every PIE-runtime-only tool (`pie.console_exec`, `ai.bb.set_value`
    with actor_path, `ai.controller.set_focal_point`, etc.) MUST
    return -32038. PASS = -32038 received.

  Mode 2 — PIE ON (only if user starts PIE before this phase runs):
    Every asset-write tool (`bp.add_function`, `actor.spawn`,
    `asset.create_data_asset`, `cb.duplicate`, ...) MUST return -32027.
    PASS = -32027 received.

This phase determines current PIE state via `pie.is_running` and runs
only the applicable half. The OTHER half is SKIPPED with a note.

Probes are intentionally lightweight: each calls with the MINIMUM args
needed to pass the validator and reach the PIE guard. No state mutation
goal — we want the guard to fire, not for the operation to succeed.

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

PHASE = "b3"
NAME = "pie_state_guards"


# Tools that MUST reject when PIE is NOT active (-32038 PIENotActive)
PIE_RUNTIME_ONLY: List[Tuple[str, Dict[str, Any], str]] = [
    ("pie.console_exec",
     {"command": "stat unit"},
     "pie.console_exec"),
    ("pie.toggle_pause",
     {},
     "pie.toggle_pause"),
    ("pie.simulate_input",
     {"key": "F", "event": "Pressed"},
     "pie.simulate_input"),
    # AI runtime accessors (PIE world only)
    ("ai.bb.set_value",
     {"actor_path": "/Game/_phantom/AIChar", "key_name": "K", "value": 1.0},
     "ai.bb.set_value (runtime)"),
    ("ai.bb.get_value",
     {"actor_path": "/Game/_phantom/AIChar", "key_name": "K"},
     "ai.bb.get_value (runtime)"),
    ("ai.bb.list_keys",
     {"actor_path": "/Game/_phantom/AIChar"},
     "ai.bb.list_keys (actor_path runtime form)"),
    ("ai.controller.set_focal_point",
     {"controller_path": "/Game/_phantom/AICon", "location": [0, 0, 0]},
     "ai.controller.set_focal_point"),
    ("ai.controller.move_to",
     {"controller_path": "/Game/_phantom/AICon", "location": [0, 0, 0]},
     "ai.controller.move_to"),
    ("ai.eqs.run_query",
     {"eqs_path": "/Game/_phantom/EQS", "querier_path": "/Game/_phantom/Q"},
     "ai.eqs.run_query"),
    ("ai.perception.get_known_actors",
     {"actor_path": "/Game/_phantom/AICon"},
     "ai.perception.get_known_actors"),
    ("ai.crowd.set_avoidance_quality",
     {"quality": "High"},
     "ai.crowd.set_avoidance_quality"),
]


# Tools that MUST reject when PIE IS active (-32027 PIEActive)
ASSET_WRITE_PIE_BLOCKED: List[Tuple[str, Dict[str, Any], str]] = [
    ("actor.spawn",
     {"class_path": "/Script/Engine.StaticMeshActor", "location": [0, 0, 0]},
     "actor.spawn (asset world)"),
    ("actor.destroy",
     {"actor_path": "/Game/_phantom/X"},
     "actor.destroy"),
    ("bp.create_blueprint",
     {"dest_path": f"/Game/PhT_B3/BP_{random_suffix(4)}",
      "parent_class_path": "/Script/Engine.Actor"},
     "bp.create_blueprint"),
    ("bp.add_variable",
     {"blueprint_path": "/Game/_phantom_bp/X",
      "variable_name": "V",
      "pin_type": {"category": "Real", "subcategory": "float"}},
     "bp.add_variable"),
    ("bp.add_function",
     {"blueprint_path": "/Game/_phantom_bp/X", "function_name": "F"},
     "bp.add_function"),
    ("bp.compile",
     {"blueprint_path": "/Game/_phantom_bp/X"},
     "bp.compile"),
    ("asset.create_data_asset",
     {"path": f"/Game/PhT_B3/DA_{random_suffix(4)}",
      "class_path": "/Script/Engine.PrimaryDataAsset"},
     "asset.create_data_asset"),
    ("cb.duplicate",
     {"source_path": "/Engine/BasicShapes/Cube.Cube",
      "dest_path": f"/Game/PhT_B3/Dup_{random_suffix(4)}"},
     "cb.duplicate"),
    ("cb.delete",
     {"path": "/Game/_no_such_target"},
     "cb.delete"),
    ("cb.rename",
     {"path": "/Game/_no_such_src",
      "new_path": "/Game/_no_such_dst"},
     "cb.rename"),
    ("cb.create_folder",
     {"path": f"/Game/PhT_B3_Folder_{random_suffix(4)}"},
     "cb.create_folder (S+8)"),
    ("mesh.duplicate",
     {"source_path": "/Engine/BasicShapes/Cube.Cube",
      "dest_path": f"/Game/PhT_B3/Mesh_{random_suffix(4)}"},
     "mesh.duplicate"),
    ("level.duplicate",
     {"source_map": "/Game/_no_such_src",
      "dest_map": f"/Game/PhT_B3/Map_{random_suffix(4)}"},
     "level.duplicate"),
    ("ai.bb.create_asset",
     {"path": f"/Game/PhT_B3/BB_{random_suffix(4)}"},
     "ai.bb.create_asset"),
    ("ai.bt.create_asset",
     {"path": f"/Game/PhT_B3/BT_{random_suffix(4)}"},
     "ai.bt.create_asset"),
    ("ai.bt.add_node",
     {"bt_path": "/Game/_phantom/BT", "parent_guid": "ROOT",
      "node_class_path": "/Script/AIModule.BTTask_Wait"},
     "ai.bt.add_node"),
    ("input.create_input_action",
     {"path": f"/Game/PhT_B3/IA_{random_suffix(4)}",
      "value_type": "Boolean"},
     "input.create_input_action"),
    ("gameplaytag.add_tag",
     {"tag": f"PhT_B3.{random_suffix(4)}"},
     "gameplaytag.add_tag"),
]


def main() -> int:
    if not preflight(PHASE):
        return 2
    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()
    fail_total = 0

    pie_state = call("pie.is_running", {}, timeout=4.0)
    pie_active = is_ok(pie_state) and pie_state.get("result", {}).get("is_running") is True
    print(f"[B3] PIE state: {'ACTIVE' if pie_active else 'INACTIVE'}", flush=True)

    if not pie_active:
        # Test PIE_RUNTIME_ONLY tools — must reject with -32038
        print(f"[B3] PIE off → probing {len(PIE_RUNTIME_ONLY)} runtime-only tools "
              "(expect -32038)…", flush=True)
        for method, args, label in PIE_RUNTIME_ONLY:
            t0 = time.monotonic()
            r = call(method, args, timeout=6.0)
            dt = (time.monotonic() - t0) * 1000.0
            c = err_code(r)
            alive = health(timeout=3.0)
            if not alive:
                log.case(label, "FAIL", f"EDITOR DIED on {method}",
                         alive=False, duration_ms=dt)
                log.write()
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
            if c == -32038:
                log.case(label, "PASS", "-32038 PIENotActive correctly fired",
                         alive=alive, duration_ms=dt, code=c)
            elif c in (-32004, -32011, -32602, -32601):
                # Validator (or method-not-found) fired before the PIE guard.
                # Acceptable — XFAIL. -32601 means the test catalogue named a
                # tool that doesn't actually exist; should be cleaned up.
                log.case(label, "XFAIL",
                         f"validator fired before PIE guard: {c}: {err_message(r)[:50]}",
                         alive=alive, duration_ms=dt, code=c)
            elif is_ok(r):
                log.case(label, "FAIL",
                         "RUNTIME TOOL SUCCEEDED WITH NO PIE — guard missing",
                         alive=alive, duration_ms=dt)
                fail_total += 1
            else:
                log.case(label, "FAIL",
                         f"expected -32038, got {c}: {err_message(r)[:60]}",
                         alive=alive, duration_ms=dt, code=c)
                fail_total += 1

        # Skip the other half
        for _, _, label in ASSET_WRITE_PIE_BLOCKED:
            log.case(label, "SKIP", "PIE not active — cannot test -32027 path")
    else:
        # Test ASSET_WRITE_PIE_BLOCKED tools — must reject with -32027
        print(f"[B3] PIE on → probing {len(ASSET_WRITE_PIE_BLOCKED)} write tools "
              "(expect -32027)…", flush=True)
        for method, args, label in ASSET_WRITE_PIE_BLOCKED:
            t0 = time.monotonic()
            r = call(method, args, timeout=6.0)
            dt = (time.monotonic() - t0) * 1000.0
            c = err_code(r)
            alive = health(timeout=3.0)
            if not alive:
                log.case(label, "FAIL", f"EDITOR DIED on {method}",
                         alive=False, duration_ms=dt)
                log.write()
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
            if c == -32027:
                log.case(label, "PASS", "-32027 PIEActive correctly fired",
                         alive=alive, duration_ms=dt, code=c)
            elif c in (-32004, -32011, -32602, -32010):
                log.case(label, "XFAIL",
                         f"validator fired before PIE guard: {c}: {err_message(r)[:50]}",
                         alive=alive, duration_ms=dt, code=c)
            elif is_ok(r):
                log.case(label, "FAIL",
                         "WRITE SUCCEEDED DURING PIE — guard missing (data loss risk)",
                         alive=alive, duration_ms=dt)
                fail_total += 1
            else:
                log.case(label, "FAIL",
                         f"expected -32027, got {c}: {err_message(r)[:60]}",
                         alive=alive, duration_ms=dt, code=c)
                fail_total += 1

        for _, _, label in PIE_RUNTIME_ONLY:
            log.case(label, "SKIP", "PIE active — cannot test -32038 path")

    summary = log.write()
    cc = summary["counts"]
    print()
    print(f"[B3] PASS={cc['PASS']} FAIL={cc['FAIL']} XFAIL={cc.get('XFAIL', 0)} "
          f"SKIP={cc.get('SKIP', 0)} TOTAL={cc['TOTAL']}")
    print(f"     log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    if fail_total > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
