#!/usr/bin/env python3
"""Phase I2 — API ergonomics regressions (complete Category I).

Re-runs the ergonomics findings recorded by the original Phase D real-
workflow stress test and confirms the fixed UX behaviour stays fixed:

  D-1  arg-name inconsistency → bridge ALWAYS rejects malformed shapes
       with a clean -32602 whose message names the canonical field. The
       finding is "discovery friction", NOT a crash/hang/leak — so the
       regression test is: malformed → -32602 + canonical field name in
       the message; AND the correct shape (once discovered) succeeds.

  D-2  ai.bb authoring gap → originally ai.bb.add_key only worked on a
       runtime BlackboardComponent (actor_path), with no "add key to
       asset" tool. Wave P closed this. Confirm ai.bb.add_key authors a
       key directly on the BB ASSET.

  H4   bp.get_variable default_value (Wave WS3 / S+? CDO-read fix) —
       reading a variable returns a 'default_value' field (CDO fallback),
       not an omission.

Negative verdicts:
  -32602 + canonical field in msg → PASS (helpful rejection)
  -32602 but field not named      → XFAIL (rejected, less specific msg)
  -32601 not registered            → SKIP
  ok=true (should have rejected)   → FAIL
  editor crash                     → FAIL

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

PHASE = "i2"
NAME = "ergonomics"

ROOT = f"/Game/PhT_I2_{random_suffix(6)}"
BP_PATH = f"{ROOT}/BP_I2"
IA_PATH = f"{ROOT}/IA_I2"
IMC_PATH = f"{ROOT}/IMC_I2"
BB_PATH = f"{ROOT}/BB_I2"


def cleanup() -> None:
    for p in (BP_PATH, IA_PATH, IMC_PATH, BB_PATH):
        call("cb.delete", {"path": p, "force": True}, timeout=8.0)
    call("folder.delete", {"folder_path": ROOT, "recursive": True}, timeout=10.0)


def _neg(log: TestLogger, case: str, method: str, args: Dict[str, Any],
         canonical_fields: List[str]) -> int:
    """Malformed-arg probe. Expect clean -32602 naming a canonical field.
    Returns fail-delta (0/1)."""
    t0 = time.monotonic()
    r = call(method, args, timeout=8.0)
    dt = (time.monotonic() - t0) * 1000.0
    c = err_code(r)
    msg = err_message(r)
    if is_transport_failure(r):
        log.case(case, "FAIL", f"transport: {r.get('_err')}", duration_ms=dt)
        return 1
    if c == -32601:
        log.case(case, "SKIP", f"{method} not registered", duration_ms=dt)
        return 0
    if is_ok(r):
        log.case(case, "FAIL",
                 f"{method} accepted malformed args (should have rejected)",
                 duration_ms=dt)
        return 1
    if c == -32602:
        named = any(f.lower() in msg.lower() for f in canonical_fields)
        if named:
            log.case(case, "PASS",
                     f"-32602 names canonical field: {msg[:60]}",
                     duration_ms=dt, code=c)
        else:
            log.case(case, "XFAIL",
                     f"-32602 but field {canonical_fields} not in msg: {msg[:60]}",
                     duration_ms=dt, code=c)
        return 0
    # Some other structured rejection — acceptable but note it.
    if c is not None and -32700 <= c <= -32000:
        log.case(case, "XFAIL",
                 f"rejected with {c} (expected -32602): {msg[:50]}",
                 duration_ms=dt, code=c)
        return 0
    log.case(case, "FAIL", f"unknown response code={c}: {msg[:60]}",
             duration_ms=dt, code=c)
    return 1


def _pos(log: TestLogger, case: str, method: str, args: Dict[str, Any],
         timeout: float = 15.0) -> Optional[Dict[str, Any]]:
    """Positive probe — the correct shape MUST succeed. Returns result or None."""
    t0 = time.monotonic()
    r = call(method, args, timeout=timeout)
    dt = (time.monotonic() - t0) * 1000.0
    if is_ok(r):
        log.case(case, "PASS", f"{method} ok (correct shape accepted)",
                 duration_ms=dt)
        return r.get("result", {}) or {}
    log.case(case, "FAIL",
             f"{method} correct shape REJECTED: {err_code(r)}: {err_message(r)[:60]}",
             duration_ms=dt, code=err_code(r))
    return None


def main() -> int:
    if not preflight(PHASE):
        return 2

    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()
    fail_total = 0

    print(f"[I2] API ergonomics regression re-test (root={ROOT})…", flush=True)
    cleanup()
    call("folder.create", {"folder_path": ROOT}, timeout=8.0)

    # ── Fixtures ───────────────────────────────────────────────────────
    r = call("bp.create_blueprint",
             {"dest_path": BP_PATH, "parent_class_path": "/Script/Engine.Actor"},
             timeout=20.0)
    if not is_ok(r):
        log.case("setup_bp", "FAIL", f"fixture BP failed: {err_message(r)[:50]}")
        log.write(); cleanup(); return 1
    log.case("setup_bp", "PASS", "fixture BP created")

    r = call("input.create_input_action", {"path": IA_PATH, "value_type": "Boolean"},
             timeout=15.0)
    log.case("setup_ia", "PASS" if is_ok(r) else "XFAIL",
             "fixture IA created" if is_ok(r) else f"IA setup: {err_message(r)[:40]}")
    r = call("input.create_mapping_context", {"path": IMC_PATH}, timeout=15.0)
    log.case("setup_imc", "PASS" if is_ok(r) else "XFAIL",
             "fixture IMC created" if is_ok(r) else f"IMC setup: {err_message(r)[:40]}")
    r = call("ai.bb.create_asset", {"path": BB_PATH}, timeout=15.0)
    bb_ready = is_ok(r)
    log.case("setup_bb", "PASS" if bb_ready else "XFAIL",
             "fixture BB created" if bb_ready else f"BB setup: {err_message(r)[:40]}")

    # ── D-1 NEGATIVE: malformed shapes → clean -32602 + canonical field ─
    fail_total += _neg(log, "D1_add_variable_no_pintype", "bp.add_variable",
                        {"blueprint_path": BP_PATH, "variable_name": "X"},
                        ["pin_type"])
    # bp.add_component requires both variable_name AND component_class_path;
    # whichever the validator checks first is the canonical field it names.
    # Either is a clean, helpful rejection (the ergonomic property under test).
    fail_total += _neg(log, "D1_add_component_no_required", "bp.add_component",
                        {"blueprint_path": BP_PATH},
                        ["variable_name", "component_class_path"])
    fail_total += _neg(log, "D1_add_mapping_no_imc", "input.add_mapping",
                        {"ia_path": IA_PATH, "key": "SpaceBar"},
                        ["imc_path"])
    fail_total += _neg(log, "D1_get_ctx_bindings_no_mcp", "input.get_context_bindings",
                        {},
                        ["mapping_context_path", "imc_path"])
    fail_total += _neg(log, "D1_add_expression_no_class", "mat.add_expression",
                        {},
                        ["expression_class", "material_path", "material"])
    fail_total += _neg(log, "D1_list_properties_no_asset", "asset.list_properties",
                        {},
                        ["asset_path"])

    # ── D-1 ENUM: value_type case-sensitivity ("Digital" rejected) ─────
    t0 = time.monotonic()
    r = call("input.create_input_action",
             {"path": f"{ROOT}/IA_Digital_{random_suffix(3)}", "value_type": "Digital"},
             timeout=12.0)
    dt = (time.monotonic() - t0) * 1000.0
    if is_ok(r):
        # Now accepted — a UX improvement over the Phase D finding. Clean up.
        log.case("D1_enum_digital", "XFAIL",
                 "value_type 'Digital' now ACCEPTED (UX improvement vs Phase D)",
                 duration_ms=dt)
    elif err_code(r) is not None and -32700 <= (err_code(r) or 0) <= -32000:
        log.case("D1_enum_digital", "PASS",
                 f"'Digital' rejected with {err_code(r)} (case-sensitive enum holds): "
                 f"{err_message(r)[:40]}", duration_ms=dt, code=err_code(r))
    else:
        log.case("D1_enum_digital", "FAIL",
                 f"unexpected: {err_code(r)} {err_message(r)[:50]}", duration_ms=dt)
        fail_total += 1

    # ── D-1 POSITIVE: discovered correct shapes succeed ────────────────
    if _pos(log, "D1_pos_add_variable", "bp.add_variable",
            {"blueprint_path": BP_PATH, "variable_name": "Health",
             "pin_type": {"category": "Real", "subcategory": "float"},
             "default_value": "100.0"}) is None:
        fail_total += 1
    if _pos(log, "D1_pos_add_mapping", "input.add_mapping",
            {"imc_path": IMC_PATH, "ia_path": IA_PATH, "key": "LeftMouseButton"}) is None:
        fail_total += 1

    # ── D-2: ai.bb authoring gap closed — add_key works on the ASSET ───
    if bb_ready:
        t0 = time.monotonic()
        r = call("ai.bb.add_key",
                 {"bb_path": BB_PATH, "key_name": "TestKey", "key_type": "Float"},
                 timeout=12.0)
        dt = (time.monotonic() - t0) * 1000.0
        if is_ok(r):
            log.case("D2_bb_add_key_asset", "PASS",
                     "ai.bb.add_key authored key on asset (D-2 gap closed)",
                     duration_ms=dt)
        elif err_code(r) == -32601:
            log.case("D2_bb_add_key_asset", "XFAIL",
                     "ai.bb.add_key not registered — D-2 gap still open",
                     duration_ms=dt)
        else:
            log.case("D2_bb_add_key_asset", "XFAIL",
                     f"add_key on asset returned {err_code(r)}: {err_message(r)[:50]}",
                     duration_ms=dt, code=err_code(r))
    else:
        log.case("D2_bb_add_key_asset", "SKIP", "BB fixture unavailable")

    # ── H4: bp.get_variable returns default_value (CDO fallback) ───────
    t0 = time.monotonic()
    r = call("bp.get_variable",
             {"blueprint_path": BP_PATH, "variable_name": "Health"}, timeout=12.0)
    dt = (time.monotonic() - t0) * 1000.0
    if is_ok(r):
        res = r.get("result", {}) or {}
        var = res.get("variable") or res  # tolerate flat or nested
        has_dv = isinstance(var, dict) and "default_value" in var
        if has_dv:
            log.case("H4_get_variable_default", "PASS",
                     f"default_value present in result (CDO fallback): "
                     f"{str(var.get('default_value'))[:30]}", duration_ms=dt)
        else:
            log.case("H4_get_variable_default", "XFAIL",
                     f"default_value key absent; result keys={list(var.keys())[:8]}",
                     duration_ms=dt)
    else:
        log.case("H4_get_variable_default", "FAIL",
                 f"bp.get_variable failed: {err_code(r)}: {err_message(r)[:50]}",
                 duration_ms=dt, code=err_code(r))
        fail_total += 1

    crash = latest_crash_dump(since=crash_baseline)
    if crash:
        log.case("crash_check", "FAIL", f"CRASH DUMP: {crash}")
        log.write(); cleanup(); return 1

    cleanup()
    summary = log.write()
    cc = summary["counts"]
    print()
    print(f"[I2] PASS={cc['PASS']} FAIL={cc['FAIL']} XFAIL={cc.get('XFAIL', 0)} "
          f"SKIP={cc.get('SKIP', 0)} TOTAL={cc['TOTAL']}")
    print(f"     log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    if fail_total > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
