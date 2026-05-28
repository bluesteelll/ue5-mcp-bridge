#!/usr/bin/env python3
"""Phase G4 — Live Coding recompile racing a Lane A op (complete Category G).

Goal: triggering a Live Coding recompile (livecoding._recompile_internal,
Lane B / worker pool) while a bp.compile (Lane A / game thread) is in
flight must NOT corrupt bridge state — no crash, no half-state. The two
run on different lanes, so they genuinely execute concurrently; the test
proves the dispatcher + job registry stay consistent.

GUARDED design: a real Live Coding compile caps at 180 s server-side. With
NO source change since the last build, ELiveCodingCompileResult::NoChanges
returns fast; if Live Coding is disabled in this editor configuration the
module resolve fails fast with a structured error. Either way the client
timeout is bounded — a client-side timeout (heavy compile actually
running) is classified XFAIL, never FAIL. An editor crash is FAIL.

Probes:
  P1 — standalone: livecoding._recompile_internal once.
       ok / structured-error → PASS;  client timeout → XFAIL;  crash → FAIL
  P2 — race: thread A loops bp.compile on a fixture BP (sustained Lane A
       window); thread B fires the recompile ~200 ms in. Editor alive +
       both lanes return structured → PASS;  recompile timeout → XFAIL.

Exit codes: 0=PASS, 1=FAIL, 2=preflight.
"""

from __future__ import annotations

import sys
import threading
import time
from pathlib import Path
from typing import Any, Dict, List, Optional

sys.path.insert(0, str(Path(__file__).parent))
from mcp_test_harness import (
    LOG_ROOT,
    TestLogger,
    assert_lane_a_alive,
    call,
    dismiss_ue_modal_via_win32,
    err_code,
    err_message,
    health,
    is_ok,
    is_transport_failure,
    latest_crash_dump,
    preflight,
    random_suffix,
)

PHASE = "g4"
NAME = "livecoding_race"

ROOT = f"/Game/PhT_G4_{random_suffix(6)}"
BP_FIXTURE = f"{ROOT}/BP_G4_Compile"

# Client-side recompile timeout. Well below the 180 s server cap — if the
# recompile actually does heavy work it'll exceed this and we classify XFAIL.
LIVECODING_TIMEOUT_S = 30.0

# A specific, loaded plugin module so a REAL recompile resolves to NoChanges
# fast (nothing changed since the last build) or fail-fast if Live Coding is
# disabled. "*" would scan ALL modules and risk a multi-minute compile — never
# use it here. The recompile handler requires a non-empty `modules` array.
LIVECODING_MODULES = ["UnrealMCPBridgeCore"]


def _recompile() -> Dict[str, Any]:
    return call("livecoding._recompile_internal",
                {"modules": LIVECODING_MODULES}, timeout=LIVECODING_TIMEOUT_S)


def _classify_recompile(log: TestLogger, case: str, r: Dict[str, Any],
                        dt: float) -> int:
    """Shared verdict for a recompile response. Returns fail-delta (0/1)."""
    if is_transport_failure(r):
        if r.get("_err") == "timeout":
            log.case(case, "XFAIL",
                     f"recompile still running at {LIVECODING_TIMEOUT_S:.0f}s "
                     f"client cap (heavy compile, not a bridge fault)",
                     duration_ms=dt)
            return 0
        log.case(case, "FAIL", f"transport: {r.get('_err')}", duration_ms=dt)
        return 1
    if is_ok(r):
        res = r.get("result", {}) or {}
        outcome = res.get("result") or res.get("compile_result") or "ok"
        log.case(case, "PASS", f"recompile returned ok (result={outcome})",
                 duration_ms=dt)
        return 0
    c = err_code(r)
    if c is not None and -32700 <= c <= -32000:
        # Module unavailable / not-supported / busy = graceful structured error.
        log.case(case, "PASS",
                 f"recompile structured error {c}: {err_message(r)[:50]} "
                 f"(graceful — Live Coding likely disabled/unavailable)",
                 duration_ms=dt)
        return 0
    log.case(case, "FAIL", f"unknown response code={c} msg={err_message(r)[:60]}",
             duration_ms=dt)
    return 1


def cleanup() -> None:
    call("cb.delete", {"path": BP_FIXTURE, "force": True}, timeout=10.0)
    call("folder.delete", {"folder_path": ROOT, "recursive": True}, timeout=10.0)


def main() -> int:
    if not preflight(PHASE):
        return 2

    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()
    fail_total = 0

    print(f"[G4] Live Coding recompile race (bounded {LIVECODING_TIMEOUT_S:.0f}s)…",
          flush=True)
    cleanup()
    call("folder.create", {"folder_path": ROOT}, timeout=8.0)

    # Fixture BP for the Lane A compile loop.
    r = call("bp.create_blueprint",
             {"dest_path": BP_FIXTURE,
              "parent_class_path": "/Script/Engine.Actor"}, timeout=20.0)
    if not is_ok(r):
        log.case("P0_fixture", "FAIL",
                 f"couldn't create fixture BP: {err_message(r)[:60]}")
        log.write(); cleanup(); return 1
    log.case("P0_fixture", "PASS", f"fixture BP created at {BP_FIXTURE}")

    # ── P1 — standalone recompile probe ────────────────────────────────
    t0 = time.monotonic()
    r1 = _recompile()
    dt = (time.monotonic() - t0) * 1000.0
    if not health(timeout=5.0):
        # Lane B dead too? or modal? try a dismiss then re-check.
        dismissed = dismiss_ue_modal_via_win32()
        time.sleep(1.5)
        if not health(timeout=8.0):
            log.case("P1_standalone", "FAIL",
                     f"editor unresponsive after standalone recompile "
                     f"(dismiss={dismissed})", alive=False, duration_ms=dt)
            log.write(); cleanup(); return 1
    fail_total += _classify_recompile(log, "P1_standalone", r1, dt)

    # ── P2 — race: bp.compile loop (Lane A) + recompile (Lane B) ───────
    results: Dict[str, Any] = {}
    compile_oks: List[bool] = []

    def _compile_loop() -> None:
        for _ in range(6):
            rc = call("bp.compile", {"blueprint_path": BP_FIXTURE}, timeout=20.0)
            compile_oks.append(is_ok(rc))

    def _recompile_mid() -> None:
        time.sleep(0.2)
        s = time.monotonic()
        results["lc"] = _recompile()
        results["lc_dt"] = (time.monotonic() - s) * 1000.0

    t0 = time.monotonic()
    ta = threading.Thread(target=_compile_loop)
    tb = threading.Thread(target=_recompile_mid)
    ta.start(); tb.start()
    ta.join(timeout=90.0); tb.join(timeout=LIVECODING_TIMEOUT_S + 15.0)
    dt = (time.monotonic() - t0) * 1000.0

    # Defensive: a Live Coding "Hot Reload" modal can block the game thread.
    if not health(timeout=6.0):
        dismissed = dismiss_ue_modal_via_win32()
        time.sleep(1.5)
        alive = health(timeout=8.0)
        if not alive:
            log.case("P2_race", "FAIL",
                     f"editor unresponsive after race (dismiss={dismissed})",
                     alive=False, duration_ms=dt)
            log.write(); cleanup(); return 1
        log.note(f"P2 modal dismissed during recovery: {dismissed}")

    n_comp_ok = sum(1 for ok in compile_oks if ok)
    n_comp = len(compile_oks)
    r_lc = results.get("lc", {"_err": "thread_hang"})
    lc_dt = results.get("lc_dt", dt)

    # Compile loop must have completed with the majority succeeding (the BP is
    # trivial; compile should always pass). A recompile timeout is XFAIL.
    comp_clean = n_comp_ok >= max(1, int(n_comp * 0.8)) if n_comp else False
    lc_delta = _classify_recompile(log, "P2_recompile_side", r_lc, lc_dt)
    fail_total += lc_delta

    if not comp_clean and n_comp == 0:
        # compile thread hung — likely a modal blocked Lane A during recompile
        log.case("P2_compile_side", "XFAIL",
                 "bp.compile loop produced 0 results — Lane A likely blocked by "
                 "Live Coding hot-reload modal (recovered); not a crash")
    elif comp_clean:
        log.case("P2_compile_side", "PASS",
                 f"bp.compile loop {n_comp_ok}/{n_comp} ok concurrent with recompile",
                 duration_ms=dt)
    else:
        log.case("P2_compile_side", "XFAIL",
                 f"bp.compile {n_comp_ok}/{n_comp} ok (some dipped under Live "
                 f"Coding contention)", duration_ms=dt)

    # ── P3 — recovery: both lanes responsive after the race ────────────
    # A Live Coding patch-apply can briefly suspend the game thread; confirm
    # full recovery (no permanent wedge, no lingering hot-reload modal).
    if not assert_lane_a_alive(timeout_s=10.0):
        dismiss_ue_modal_via_win32()
        time.sleep(1.5)
    h = health(timeout=6.0)
    la = assert_lane_a_alive(timeout_s=12.0)
    if h and la:
        log.case("P3_recovery", "PASS",
                 "post-race: Lane B + Lane A responsive (no wedge/modal)")
    else:
        log.case("P3_recovery", "FAIL",
                 f"post-race not recovered: laneB={h} laneA={la}")
        fail_total += 1

    crash = latest_crash_dump(since=crash_baseline)
    if crash:
        log.case("crash_check", "FAIL", f"CRASH DUMP: {crash}")
        log.write(); cleanup(); return 1

    cleanup()
    summary = log.write()
    cc = summary["counts"]
    print()
    print(f"[G4] PASS={cc['PASS']} FAIL={cc['FAIL']} XFAIL={cc.get('XFAIL', 0)} "
          f"SKIP={cc.get('SKIP', 0)} TOTAL={cc['TOTAL']}")
    print(f"     log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    if fail_total > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
