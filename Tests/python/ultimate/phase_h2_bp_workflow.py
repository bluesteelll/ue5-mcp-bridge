#!/usr/bin/env python3
"""Phase H2 — Real workflow: BP authoring end-to-end.

Goal: create a functional Blueprint via MCP only, verify each step
returned the expected result, and the final BP exists with correct
structure.

Workflow:
  1. bp.create_blueprint /Game/PhT_H2/BP_TestActor parent=/Script/Engine.Actor
  2. bp.add_variable Health (Real/float, default 100)
  3. bp.add_variable Damage (Real/float, default 10)
  4. bp.add_variable IsAlive (Boolean, default true)
  5. bp.list_variables → contains all 3
  6. bp.add_function ApplyDamage
  7. bp.add_function_parameter Amount (Real/float)
  8. bp.list_function_parameters ApplyDamage → contains Amount
  9. bp.compile → succeeds
  10. bp.list_functions → contains ApplyDamage
  11. bp.add_interface (skipped if no test interface)
  12. cb.delete cleanup at end

PASS: every step succeeds, structure is correct.

Exit codes: 0=PASS, 1=FAIL (any step), 2=preflight.
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
    is_transport_failure,
    latest_crash_dump,
    preflight,
    random_suffix,
)

PHASE = "h2"
NAME = "bp_workflow"

ROOT = f"/Game/PhT_H2_{random_suffix(6)}"
BP_PATH = f"{ROOT}/BP_TestActor"


def _step(log: TestLogger, name: str, method: str, args: Dict[str, Any],
          timeout: float = 15.0) -> Optional[Dict[str, Any]]:
    """Execute one workflow step. Returns the result dict on success, None on failure.
    Failure logs FAIL and returns None."""
    t0 = time.monotonic()
    try:
        r = call(method, args, timeout=timeout)
    except Exception as e:
        log.case(name, "FAIL", f"exception: {e}",
                 duration_ms=(time.monotonic() - t0) * 1000.0)
        return None
    dt = (time.monotonic() - t0) * 1000.0
    if is_transport_failure(r):
        log.case(name, "FAIL", f"transport: {r.get('_err')}", duration_ms=dt)
        return None
    if not is_ok(r):
        c = err_code(r)
        log.case(name, "FAIL", f"{method}: code={c}: {err_message(r)[:60]}",
                 duration_ms=dt, code=c)
        return None
    log.case(name, "PASS", f"{method} ok", duration_ms=dt)
    return r.get("result", {}) or {}


def cleanup() -> None:
    """Best-effort cleanup. Delete blueprint then folder."""
    call("cb.delete", {"path": BP_PATH, "force": True}, timeout=10.0)
    call("folder.delete", {"folder_path": ROOT, "recursive": True}, timeout=10.0)


def main() -> int:
    if not preflight(PHASE):
        return 2
    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()

    print(f"[H2] BP authoring end-to-end (root={ROOT})…", flush=True)

    cleanup()  # in case from prior run

    # 1. Create root folder
    r = _step(log, "1_create_folder", "folder.create", {"folder_path": ROOT})
    if r is None:
        log.write()
        cleanup()
        return 1

    # 2. Create blueprint
    r = _step(log, "2_bp_create", "bp.create_blueprint",
              {"dest_path": BP_PATH, "parent_class_path": "/Script/Engine.Actor"})
    if r is None:
        log.write()
        cleanup()
        return 1

    # 3-5. Add 3 variables
    for vname, pin_type, default in [
        ("Health", {"category": "Real", "subcategory": "float"}, 100.0),
        ("Damage", {"category": "Real", "subcategory": "float"}, 10.0),
        ("IsAlive", {"category": "Boolean"}, True),
    ]:
        args = {
            "blueprint_path": BP_PATH,
            "variable_name": vname,
            "pin_type": pin_type,
        }
        r = _step(log, f"3_add_var_{vname}", "bp.add_variable", args)
        if r is None:
            log.write()
            cleanup()
            return 1

    # 6. List variables should include all 3
    r = _step(log, "6_list_vars", "bp.list_variables", {"blueprint_path": BP_PATH})
    if r is None:
        log.write()
        cleanup()
        return 1
    var_names = [v.get("name") for v in (r.get("variables") or [])]
    if not all(n in var_names for n in ["Health", "Damage", "IsAlive"]):
        log.case("6_verify_vars", "FAIL",
                 f"variables missing from list: got {var_names}")
        log.write()
        cleanup()
        return 1
    log.case("6_verify_vars", "PASS", f"all 3 variables present: {var_names[:5]}")

    # 7. Add function ApplyDamage
    r = _step(log, "7_add_function", "bp.add_function",
              {"blueprint_path": BP_PATH, "function_name": "ApplyDamage"})
    if r is None:
        log.write()
        cleanup()
        return 1

    # 8. Add parameter to function
    r = _step(log, "8_add_function_param", "bp.add_function_parameter",
              {"blueprint_path": BP_PATH,
               "function_name": "ApplyDamage",
               "param_name": "Amount",
               "direction": "input",
               "pin_type": {"category": "Real", "subcategory": "float"}})
    if r is None:
        log.write()
        cleanup()
        return 1

    # 9. List function parameters
    r = _step(log, "9_list_function_params", "bp.list_function_parameters",
              {"blueprint_path": BP_PATH, "function_name": "ApplyDamage"})
    if r is None:
        log.write()
        cleanup()
        return 1
    # API returns inputs/outputs as separate arrays, NOT 'parameters'.
    inputs = r.get("inputs") or []
    outputs = r.get("outputs") or []
    all_param_names = [p.get("name") for p in inputs + outputs]
    if "Amount" not in all_param_names:
        log.case("9_verify_params", "FAIL",
                 f"Amount not in params: inputs={[p.get('name') for p in inputs]} outputs={[p.get('name') for p in outputs]}")
        log.write()
        cleanup()
        return 1
    log.case("9_verify_params", "PASS", f"Amount param confirmed (direction=input)")

    # 10. Compile
    r = _step(log, "10_compile", "bp.compile", {"blueprint_path": BP_PATH})
    if r is None:
        log.write()
        cleanup()
        return 1

    # 11. List functions
    r = _step(log, "11_list_functions", "bp.list_functions",
              {"blueprint_path": BP_PATH})
    if r is None:
        log.write()
        cleanup()
        return 1
    func_names = [f.get("name") for f in (r.get("functions") or [])]
    if "ApplyDamage" not in func_names:
        log.case("11_verify_functions", "FAIL",
                 f"ApplyDamage missing from function list: got {func_names[:5]}")
        log.write()
        cleanup()
        return 1
    log.case("11_verify_functions", "PASS", f"ApplyDamage confirmed in function list")

    # 12. Final asset exists check
    r = _step(log, "12_asset_exists", "asset.exists",
              {"path": BP_PATH})
    if r is None:
        log.write()
        cleanup()
        return 1
    if not r.get("exists"):
        log.case("12_verify_exists", "FAIL", f"asset.exists returned exists=false")
        log.write()
        cleanup()
        return 1
    log.case("12_verify_exists", "PASS", "BP exists in asset registry")

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
    print(f"[H2] PASS={cc['PASS']} FAIL={cc['FAIL']} TOTAL={cc['TOTAL']}")
    print(f"     log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    if cc["FAIL"] > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
