#!/usr/bin/env python3
"""Phase H5 — Real workflow: Input mapping → action binding.

Goal: create an Enhanced Input pipeline (Action + MappingContext +
key binding) via MCP only, then verify the asset registry contains
both and the mapping is linked.

Workflow:
  1. folder.create /Game/PhT_H5_<rand>
  2. input.create_input_action /Game/PhT_H5/IA_Shoot (Boolean)
  3. input.create_input_mapping_context /Game/PhT_H5/IMC_Default
  4. input.add_mapping_to_context IMC + IA_Shoot + LeftMouseButton
  5. input.list_mapping_contexts → contains IMC
  6. input.list_input_actions → contains IA_Shoot
  7. cleanup

PASS: all steps + verification succeed.

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
    is_transport_failure,
    latest_crash_dump,
    preflight,
    random_suffix,
)

PHASE = "h5"
NAME = "input_workflow"

ROOT = f"/Game/PhT_H5_{random_suffix(6)}"
IA_PATH = f"{ROOT}/IA_Shoot"
IMC_PATH = f"{ROOT}/IMC_Default"


def _step(log: TestLogger, name: str, method: str, args: Dict[str, Any],
          timeout: float = 15.0) -> Optional[Dict[str, Any]]:
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
    call("cb.delete", {"path": IA_PATH, "force": True}, timeout=10.0)
    call("cb.delete", {"path": IMC_PATH, "force": True}, timeout=10.0)
    call("folder.delete", {"folder_path": ROOT, "recursive": True}, timeout=10.0)


def main() -> int:
    if not preflight(PHASE):
        return 2
    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()

    print(f"[H5] Input mapping workflow (root={ROOT})…", flush=True)
    cleanup()

    # 1. Folder
    r = _step(log, "1_folder", "folder.create", {"folder_path": ROOT})
    if r is None:
        log.write(); cleanup(); return 1

    # 2. Create IA (Boolean — typical for trigger action)
    r = _step(log, "2_ia_create", "input.create_input_action",
              {"path": IA_PATH, "value_type": "Boolean"})
    if r is None:
        log.write(); cleanup(); return 1

    # 3. Create IMC (tool name is input.create_mapping_context — no "input_" prefix)
    r = _step(log, "3_imc_create", "input.create_mapping_context",
              {"path": IMC_PATH})
    if r is None:
        log.write(); cleanup(); return 1

    # 4. Add mapping IA + LeftMouseButton → IMC.
    # API uses imc_path + ia_path (NOT context_path / action_path).
    r = _step(log, "4_add_mapping", "input.add_mapping",
              {"imc_path": IMC_PATH,
               "ia_path": IA_PATH,
               "key": "LeftMouseButton"})
    if r is None:
        log.write(); cleanup(); return 1

    # 5. List IMCs and verify
    r = _step(log, "5_list_imcs", "input.list_mapping_contexts", {})
    if r is None:
        log.write(); cleanup(); return 1
    imcs = r.get("contexts") or r.get("mapping_contexts") or r.get("input_mapping_contexts") or []
    imc_paths = [c.get("path") or c.get("asset_path") for c in imcs]
    found_imc = any(IMC_PATH in (p or "") for p in imc_paths)
    if not found_imc:
        log.case("5_verify_imc", "FAIL",
                 f"IMC not in list of {len(imc_paths)} contexts; first 5: {imc_paths[:5]}")
        log.write(); cleanup(); return 1
    log.case("5_verify_imc", "PASS", f"IMC found in list of {len(imc_paths)} contexts")

    # 6. List IAs and verify
    r = _step(log, "6_list_ias", "input.list_input_actions", {})
    if r is None:
        log.write(); cleanup(); return 1
    ias = r.get("actions") or r.get("input_actions") or []
    ia_paths = [a.get("path") or a.get("asset_path") for a in ias]
    found_ia = any(IA_PATH in (p or "") for p in ia_paths)
    if not found_ia:
        log.case("6_verify_ia", "FAIL",
                 f"IA not in list of {len(ia_paths)} actions; first 5: {ia_paths[:5]}")
        log.write(); cleanup(); return 1
    log.case("6_verify_ia", "PASS", f"IA found in list of {len(ia_paths)} actions")

    # 7. Final asset.exists checks
    r = _step(log, "7_ia_exists", "asset.exists", {"path": IA_PATH})
    if r is None or not r.get("exists"):
        log.case("7_ia_exists_verify", "FAIL", f"IA doesn't exist: {r}")
        log.write(); cleanup(); return 1
    log.case("7_ia_exists_verify", "PASS", "IA in asset registry")

    r = _step(log, "8_imc_exists", "asset.exists", {"path": IMC_PATH})
    if r is None or not r.get("exists"):
        log.case("8_imc_exists_verify", "FAIL", f"IMC doesn't exist: {r}")
        log.write(); cleanup(); return 1
    log.case("8_imc_exists_verify", "PASS", "IMC in asset registry")

    crash = latest_crash_dump(since=crash_baseline)
    if crash:
        log.case("crash_check", "FAIL", f"CRASH DUMP: {crash}")
        log.write(); cleanup(); return 1

    cleanup()

    summary = log.write()
    cc = summary["counts"]
    print()
    print(f"[H5] PASS={cc['PASS']} FAIL={cc['FAIL']} TOTAL={cc['TOTAL']}")
    print(f"     log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    if cc["FAIL"] > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
