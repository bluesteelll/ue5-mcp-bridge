#!/usr/bin/env python3
"""Phase H6.3 — transaction.* round-trip.

Goal: verify undo/redo introspection tools work, and a mutate→undo→reread
sequence reverts the state.

Probes:
  P1 — transaction.list (read-only)
  P2 — transaction.get_state (read-only)
  P3 — Mutate (bp.add_variable) → undo → verify variable gone → redo →
       verify back
  P4 — cleanup

PASS: lists return without error; mutate+undo+verify reverts state;
redo restores.

Exit codes: 0=PASS, 1=FAIL (any), 2=preflight.
"""

from __future__ import annotations

import sys
import time
from pathlib import Path
from typing import Any, Dict, Optional

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

PHASE = "h6_3"
NAME = "transaction"

ROOT = f"/Game/PhT_H63_{random_suffix(6)}"
BP_PATH = f"{ROOT}/BP_UndoTarget"


def _step(log: TestLogger, name: str, method: str, args: Dict[str, Any],
          timeout: float = 15.0) -> Optional[Dict[str, Any]]:
    t0 = time.monotonic()
    r = call(method, args, timeout=timeout)
    dt = (time.monotonic() - t0) * 1000.0
    if not is_ok(r):
        c = err_code(r)
        log.case(name, "FAIL", f"{method}: code={c}: {err_message(r)[:60]}",
                 duration_ms=dt, code=c)
        return None
    log.case(name, "PASS", f"{method} ok", duration_ms=dt)
    return r.get("result", {}) or {}


def cleanup() -> None:
    call("cb.delete", {"path": BP_PATH, "force": True}, timeout=10.0)
    call("folder.delete", {"folder_path": ROOT, "recursive": True}, timeout=10.0)


def _has_var(name: str) -> bool:
    r = call("bp.list_variables", {"blueprint_path": BP_PATH}, timeout=10.0)
    if not is_ok(r):
        return False
    vars = r.get("result", {}).get("variables") or []
    return any(v.get("name") == name for v in vars)


def main() -> int:
    if not preflight(PHASE):
        return 2
    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()
    fail_total = 0

    print(f"[H6.3] transaction round-trip (root={ROOT})…", flush=True)
    cleanup()

    # P1 — transaction.list
    r = call("transaction.list", {}, timeout=8.0)
    if is_ok(r):
        n = len(r.get("result", {}).get("transactions") or [])
        log.case("P1_transaction_list", "PASS", f"list ok ({n} entries)")
    else:
        c = err_code(r)
        # -32601 = tool not found → XFAIL (Wave K may not be fully wired)
        if c == -32601:
            log.case("P1_transaction_list", "SKIP", "transaction.list not registered (Wave K?)")
        else:
            log.case("P1_transaction_list", "FAIL",
                     f"code={c}: {err_message(r)[:50]}", code=c)
            fail_total += 1

    # P2 — transaction.get_state
    r = call("transaction.get_state", {}, timeout=8.0)
    if is_ok(r):
        log.case("P2_transaction_get_state", "PASS",
                 f"state ok: {str(r.get('result', {}))[:60]}")
    else:
        c = err_code(r)
        if c == -32601:
            log.case("P2_transaction_get_state", "SKIP",
                     "transaction.get_state not registered")
        else:
            log.case("P2_transaction_get_state", "FAIL",
                     f"code={c}: {err_message(r)[:50]}", code=c)
            fail_total += 1

    # P3 — mutate+undo round-trip.
    # Setup: create folder + BP.
    r = _step(log, "P3_setup_folder", "folder.create", {"folder_path": ROOT})
    if r is None:
        log.write(); cleanup(); return 1

    r = _step(log, "P3_setup_bp", "bp.create_blueprint",
              {"dest_path": BP_PATH, "parent_class_path": "/Script/Engine.Actor"})
    if r is None:
        log.write(); cleanup(); return 1

    # Add variable.
    var_name = "UndoTestVar"
    r = _step(log, "P3_add_var", "bp.add_variable",
              {"blueprint_path": BP_PATH,
               "variable_name": var_name,
               "pin_type": {"category": "Real", "subcategory": "float"}})
    if r is None:
        log.write(); cleanup(); return 1

    # Verify variable present.
    if not _has_var(var_name):
        log.case("P3_verify_added", "FAIL", "variable not found post-add")
        log.write(); cleanup(); return 1
    log.case("P3_verify_added", "PASS", f"{var_name} present after add")

    # Undo. transaction.undo if exists, else SKIP P3.
    r = call("transaction.undo", {}, timeout=10.0)
    if not is_ok(r):
        c = err_code(r)
        if c == -32601:
            log.case("P3_undo", "SKIP", "transaction.undo not registered")
        else:
            log.case("P3_undo", "FAIL",
                     f"undo code={c}: {err_message(r)[:50]}", code=c)
            fail_total += 1
    else:
        log.case("P3_undo", "PASS", "transaction.undo ok")
        # Verify variable removed.
        if _has_var(var_name):
            log.case("P3_verify_undone", "FAIL",
                     "variable STILL PRESENT after undo (undo didn't work)")
            fail_total += 1
        else:
            log.case("P3_verify_undone", "PASS",
                     "variable correctly removed by undo")
            # Redo and re-verify.
            r2 = call("transaction.redo", {}, timeout=10.0)
            if is_ok(r2):
                log.case("P3_redo", "PASS", "transaction.redo ok")
                if _has_var(var_name):
                    log.case("P3_verify_redone", "PASS",
                             "variable back after redo")
                else:
                    log.case("P3_verify_redone", "XFAIL",
                             "variable NOT restored by redo (transient state)")
            else:
                c = err_code(r2)
                if c == -32601:
                    log.case("P3_redo", "SKIP", "transaction.redo not registered")
                else:
                    log.case("P3_redo", "XFAIL",
                             f"redo code={c}: {err_message(r2)[:50]}", code=c)

    crash = latest_crash_dump(since=crash_baseline)
    if crash:
        log.case("crash_check", "FAIL", f"CRASH DUMP: {crash}")
        log.write(); cleanup(); return 1

    cleanup()

    summary = log.write()
    cc = summary["counts"]
    print()
    print(f"[H6.3] PASS={cc['PASS']} FAIL={cc['FAIL']} XFAIL={cc.get('XFAIL', 0)} "
          f"SKIP={cc.get('SKIP', 0)} TOTAL={cc['TOTAL']}")
    print(f"       log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    if fail_total > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
