#!/usr/bin/env python3
"""Phase G5 — Save during tool call.

Goal: triggering a project save (cb.save / level.save) during another
in-flight tool call doesn't crash. Lane A serialization should serialize
the save behind the original work.

Probes:
  P1 — Setup: create scratch BP
  P2 — fire bp.compile (Lane A, ~1s) AND cb.save (Lane A) in parallel
       from two threads → both should complete with structured responses
  P3 — repeat 5 cycles to catch transient races
  P4 — cleanup

PASS: editor alive, all save+work pairs return structured responses,
no new crash dump.

Exit codes: 0=PASS, 1=FAIL, 2=preflight.
"""

from __future__ import annotations

import concurrent.futures
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

PHASE = "g5"
NAME = "save_during_call"

ROOT = f"/Game/PhT_G5_{random_suffix(6)}"


def _classify(r: Dict[str, Any]) -> str:
    if is_transport_failure(r):
        return f"transport:{r.get('_err')}"
    if is_ok(r):
        return "ok"
    c = err_code(r)
    return f"err:{c}"


def cleanup() -> None:
    call("folder.delete", {"folder_path": ROOT, "recursive": True}, timeout=15.0)


def main() -> int:
    if not preflight(PHASE):
        return 2
    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()
    fail_total = 0

    print(f"[G5] save-during-call (root={ROOT})…", flush=True)
    cleanup()
    call("folder.create", {"folder_path": ROOT}, timeout=8.0)

    pair_results: List[Tuple[int, str, str]] = []
    n_cycles = 5

    for i in range(n_cycles):
        bp_path = f"{ROOT}/BP_Save_{i:03d}"
        # Setup BP
        rc = call("bp.create_blueprint",
                  {"dest_path": bp_path,
                   "parent_class_path": "/Script/Engine.Actor"}, timeout=10.0)
        if not is_ok(rc):
            pair_results.append((i, "setup_fail", _classify(rc)))
            continue

        # Add a variable so compile has real work
        call("bp.add_variable",
             {"blueprint_path": bp_path, "variable_name": "Health",
              "pin_type": {"category": "Real", "subcategory": "float"}},
             timeout=8.0)

        # Now race: bp.compile + cb.save_all_dirty in parallel
        t0 = time.monotonic()
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as ex:
            f_compile = ex.submit(call, "bp.compile",
                                   {"blueprint_path": bp_path}, 20.0)
            f_save = ex.submit(call, "cb.save",
                                {"path": bp_path}, 15.0)
            r_compile = f_compile.result(timeout=30.0)
            r_save = f_save.result(timeout=30.0)
        dt = (time.monotonic() - t0) * 1000.0

        pair_results.append((i, _classify(r_compile), _classify(r_save)))

        if not health(timeout=5.0):
            log.case(f"P{i+1}_pair", "FAIL",
                     f"editor died on cycle {i}; results={pair_results}",
                     alive=False, duration_ms=dt)
            cleanup()
            log.write()
            return 1

        # Both should have completed (ok or structured error). Transport timeout = FAIL.
        if "transport" in _classify(r_compile) or "transport" in _classify(r_save):
            log.case(f"P{i+1}_pair", "FAIL",
                     f"transport failure on cycle {i}; compile={_classify(r_compile)} save={_classify(r_save)}",
                     duration_ms=dt)
            fail_total += 1
        else:
            log.case(f"P{i+1}_pair", "PASS",
                     f"compile={_classify(r_compile)} save={_classify(r_save)}",
                     duration_ms=dt)

        # Cleanup BP after cycle
        call("cb.delete", {"path": bp_path, "force": True}, timeout=10.0)

    # Summary case
    n_ok_pairs = sum(1 for (_, c, s) in pair_results
                       if "transport" not in c and "transport" not in s
                       and "setup_fail" not in c)
    log.case("summary_all_cycles",
             "PASS" if n_ok_pairs >= n_cycles - 1 else "XFAIL",
             f"{n_ok_pairs}/{n_cycles} cycles completed cleanly; "
             f"detail={pair_results}")

    crash = latest_crash_dump(since=crash_baseline)
    if crash:
        log.case("crash_check", "FAIL", f"CRASH DUMP: {crash}")
        log.write()
        cleanup()
        return 1

    cleanup()

    summary = log.write()
    cc = summary["counts"]
    print()
    print(f"[G5] PASS={cc['PASS']} FAIL={cc['FAIL']} XFAIL={cc.get('XFAIL', 0)} TOTAL={cc['TOTAL']}")
    print(f"     log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    if fail_total > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
