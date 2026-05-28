#!/usr/bin/env python3
"""Phase F3 — Cross-asset corruption check.

Goal: mutation of asset A doesn't accidentally mutate asset B.

If the Bridge ever shares state between asset operations (e.g., a stale
UPackage pointer, a cached resolver, a global IAssetTools mutex held
beyond intended scope), this would surface as B's properties changing
when only A was touched.

Probes:
  P1 — create BP_A and BP_B (both empty Actor BPs)
  P2 — read both initial states (function counts, variable counts)
  P3 — add variable to BP_A; verify BP_B unchanged
  P4 — add function to BP_A; verify BP_B unchanged
  P5 — compile BP_A; verify BP_B compiled count unchanged
  P6 — delete BP_A; verify BP_B still exists

PASS: BP_B remains byte-equal in shape across all mutations to BP_A.

Exit codes: 0=PASS, 1=FAIL, 2=preflight.
"""

from __future__ import annotations

import sys
import time
from pathlib import Path
from typing import Any, Dict, List, Optional

sys.path.insert(0, str(Path(__file__).parent))
from mcp_test_harness import (
    LOG_ROOT,
    TestLogger,
    call,
    err_code,
    err_message,
    health,
    is_ok,
    latest_crash_dump,
    preflight,
    random_suffix,
)

PHASE = "f3"
NAME = "cross_corruption"

ROOT = f"/Game/PhT_F3_{random_suffix(6)}"
BP_A = f"{ROOT}/BP_A"
BP_B = f"{ROOT}/BP_B"


def _bp_shape(bp_path: str) -> Optional[Dict[str, Any]]:
    """Snapshot a BP's shape: var names, function names, exists flag."""
    out: Dict[str, Any] = {"vars": [], "funcs": [], "exists": False}
    r_exists = call("asset.exists", {"path": bp_path}, timeout=5.0)
    out["exists"] = bool(is_ok(r_exists) and r_exists.get("result", {}).get("exists"))
    if not out["exists"]:
        return out
    r_vars = call("bp.list_variables", {"blueprint_path": bp_path}, timeout=8.0)
    if is_ok(r_vars):
        out["vars"] = sorted([v.get("name", "")
                               for v in (r_vars.get("result", {}).get("variables") or [])])
    r_funcs = call("bp.list_functions", {"blueprint_path": bp_path}, timeout=8.0)
    if is_ok(r_funcs):
        out["funcs"] = sorted([f.get("name", "")
                                for f in (r_funcs.get("result", {}).get("functions") or [])])
    return out


def cleanup() -> None:
    call("cb.delete", {"path": BP_A, "force": True}, timeout=10.0)
    call("cb.delete", {"path": BP_B, "force": True}, timeout=10.0)
    call("folder.delete", {"folder_path": ROOT, "recursive": True}, timeout=10.0)


def main() -> int:
    if not preflight(PHASE):
        return 2
    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()
    fail_total = 0

    print(f"[F3] cross-asset corruption (root={ROOT})…", flush=True)
    cleanup()

    # P1 — create both BPs
    call("folder.create", {"folder_path": ROOT}, timeout=8.0)
    for path, name in [(BP_A, "P1_create_bp_a"), (BP_B, "P1_create_bp_b")]:
        r = call("bp.create_blueprint",
                  {"dest_path": path,
                   "parent_class_path": "/Script/Engine.Actor"}, timeout=15.0)
        if not is_ok(r):
            log.case(name, "FAIL",
                     f"bp.create failed: {err_message(r)[:60]}")
            log.write(); cleanup(); return 1
        log.case(name, "PASS", f"created {path}")

    # P2 — snapshot initial state of B
    b_before = _bp_shape(BP_B)
    if b_before is None:
        log.case("P2_snapshot_b_initial", "FAIL", "couldn't snapshot BP_B")
        log.write(); cleanup(); return 1
    log.case("P2_snapshot_b_initial", "PASS",
             f"B: exists={b_before['exists']} vars={len(b_before['vars'])} "
             f"funcs={len(b_before['funcs'])}")

    # P3 — add variable to A; verify B unchanged
    r = call("bp.add_variable",
              {"blueprint_path": BP_A, "variable_name": "Health",
               "pin_type": {"category": "Real", "subcategory": "float"}},
              timeout=10.0)
    if not is_ok(r):
        log.case("P3_mutate_a_var", "FAIL", f"add_var failed: {err_message(r)[:60]}")
        log.write(); cleanup(); return 1
    log.case("P3_mutate_a_var", "PASS", "added var to A")
    b_after_p3 = _bp_shape(BP_B)
    if b_after_p3 != b_before:
        log.case("P3_verify_b_unchanged", "FAIL",
                 f"BP_B mutated by BP_A var add: before={b_before} after={b_after_p3}")
        fail_total += 1
    else:
        log.case("P3_verify_b_unchanged", "PASS",
                 "BP_B unchanged after BP_A var add")

    # P4 — add function to A; verify B unchanged
    r = call("bp.add_function",
              {"blueprint_path": BP_A, "function_name": "DoStuff"},
              timeout=10.0)
    if not is_ok(r):
        log.case("P4_mutate_a_func", "FAIL", f"add_func failed: {err_message(r)[:60]}")
        log.write(); cleanup(); return 1
    log.case("P4_mutate_a_func", "PASS", "added func to A")
    b_after_p4 = _bp_shape(BP_B)
    if b_after_p4 != b_before:
        log.case("P4_verify_b_unchanged", "FAIL",
                 f"BP_B mutated by BP_A func add: before={b_before} after={b_after_p4}")
        fail_total += 1
    else:
        log.case("P4_verify_b_unchanged", "PASS",
                 "BP_B unchanged after BP_A func add")

    # P5 — compile A; verify B unchanged
    r = call("bp.compile", {"blueprint_path": BP_A}, timeout=20.0)
    if not is_ok(r):
        log.case("P5_compile_a", "XFAIL", f"compile failed: {err_message(r)[:60]}")
    else:
        log.case("P5_compile_a", "PASS", "compiled A")
    b_after_p5 = _bp_shape(BP_B)
    if b_after_p5 != b_before:
        log.case("P5_verify_b_unchanged", "FAIL",
                 f"BP_B mutated by BP_A compile: before={b_before} after={b_after_p5}")
        fail_total += 1
    else:
        log.case("P5_verify_b_unchanged", "PASS",
                 "BP_B unchanged after BP_A compile")

    # P6 — delete A; verify B still exists with same shape
    r = call("cb.delete", {"path": BP_A, "force": True}, timeout=10.0)
    if not is_ok(r):
        log.case("P6_delete_a", "FAIL", f"delete A failed: {err_message(r)[:60]}")
    else:
        log.case("P6_delete_a", "PASS", "deleted A")
    b_after_p6 = _bp_shape(BP_B)
    if not b_after_p6["exists"]:
        log.case("P6_verify_b_exists", "FAIL",
                 "BP_B disappeared after BP_A delete (cross-contamination!)")
        fail_total += 1
    elif b_after_p6 != b_before:
        log.case("P6_verify_b_unchanged", "FAIL",
                 f"BP_B mutated by BP_A delete: before={b_before} after={b_after_p6}")
        fail_total += 1
    else:
        log.case("P6_verify_b_unchanged", "PASS",
                 "BP_B exists and unchanged after BP_A delete")

    crash = latest_crash_dump(since=crash_baseline)
    if crash:
        log.case("crash_check", "FAIL", f"CRASH DUMP: {crash}")
        log.write(); cleanup(); return 1

    cleanup()
    summary = log.write()
    cc = summary["counts"]
    print()
    print(f"[F3] PASS={cc['PASS']} FAIL={cc['FAIL']} XFAIL={cc.get('XFAIL', 0)} TOTAL={cc['TOTAL']}")
    print(f"     log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    if fail_total > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
