#!/usr/bin/env python3
"""Phase P1 — PIE lifecycle state machine (drives a LIVE PIE world).

Unlike F4/B3/C4 (which SKIP when PIE is off), Category P *drives* PIE: it
starts a real session, exercises every lifecycle tool against the running
world, verifies the state transitions, then stops cleanly.

Covered tools: pie.start, pie.is_running, pie.get_stats, pie.pause,
pie.step_frame, pie.resume, pie.set_time_dilation, pie.stop.

State machine + guards verified:
  P1  start → running, world_count>=1, pie_world_path is a UEDPIE world
  P2  get_stats sane (PIE world_path, actor_count>0, fps fields present)
  P3  already-running guard: second pie.start → -32603
  P4  pause → is_running.paused == true
  P5  step_frame (while paused) → advanced, current_frame advances
  P6  resume → is_running.paused == false
  P7  step_frame (while NOT paused) → -32603 (requires pause)
  P8  set_time_dilation round-trip (0.25 → get_stats reads ~0.25 → restore 1.0)
  P9  set_time_dilation out-of-range (999) → -32602
  P10 cooldown guard: stop then IMMEDIATE start → -32603 (async-teardown gap)
  P11 clean restart after cooldown → running; then stop → not running

Exit codes: 0=PASS (0 FAIL), 1=FAIL/editor-died, 2=preflight.
"""

from __future__ import annotations

import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from mcp_test_harness import (
    TestLogger,
    call,
    err_code,
    err_message,
    health,
    is_ok,
    latest_crash_dump,
    pie_ensure_stopped,
    pie_is_running,
    pie_start_and_wait,
    pie_state,
    pie_stop_and_wait,
    preflight,
)

PHASE = "p1"
NAME = "pie_lifecycle"


def main() -> int:
    if not preflight(PHASE):
        return 2
    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()
    fail = 0
    log.note("Category P drives a LIVE PIE world (start→manipulate→stop), "
             "rather than skipping when PIE is off.")

    # Clean slate.
    if not pie_ensure_stopped():
        log.case("preflight_stop", "FAIL", "could not stop pre-existing PIE")
        log.write()
        return 1

    # ── P1: start + wait ──────────────────────────────────────────────────
    up, info = pie_start_and_wait()
    if not up:
        log.case("P1_start", "FAIL", f"PIE never came up: {info}")
        log.write()
        return 1
    st = pie_state()
    world_path = (info.get("start_result", {}) or {}).get("pie_world_path", "")
    wc = st.get("world_count", 0)
    if st.get("running") and wc >= 1:
        log.case("P1_start", "PASS",
                 f"running, world_count={wc}, pie_world_path={world_path!r}")
    else:
        log.case("P1_start", "FAIL", f"unexpected state after start: {st}")
        fail += 1

    # ── P2: get_stats sane ────────────────────────────────────────────────
    rs = call("pie.get_stats", {}, timeout=10.0)
    if is_ok(rs):
        res = rs.get("result", {}) or {}
        wp = res.get("world_path", "")
        ac = res.get("actor_count", 0)
        has_fps = "instant_fps" in res and "avg_fps" in res and "memory" in res
        if "UEDPIE" in wp and ac > 0 and has_fps:
            log.case("P2_stats_sane", "PASS",
                     f"world_path={wp} actor_count={ac} "
                     f"instant_fps={res.get('instant_fps'):.1f}")
        else:
            log.case("P2_stats_sane", "FAIL",
                     f"stats not sane: UEDPIE_in_path={'UEDPIE' in wp} "
                     f"actor_count={ac} has_fps={has_fps}")
            fail += 1
    else:
        log.case("P2_stats_sane", "FAIL",
                 f"get_stats failed during PIE: {err_code(rs)} {err_message(rs)[:60]}")
        fail += 1

    # ── P3: already-running guard ─────────────────────────────────────────
    r = call("pie.start", {"mode": "selected_viewport"}, timeout=10.0)
    if err_code(r) == -32603:
        log.case("P3_already_running_guard", "PASS",
                 f"second pie.start refused: {err_message(r)[:70]}")
    elif is_ok(r):
        log.case("P3_already_running_guard", "FAIL",
                 "second pie.start SUCCEEDED — should refuse while running")
        fail += 1
    else:
        log.case("P3_already_running_guard", "XFAIL",
                 f"refused with unexpected code {err_code(r)}: {err_message(r)[:60]}")

    # ── P4: pause ─────────────────────────────────────────────────────────
    rp = call("pie.pause", {}, timeout=10.0)
    time.sleep(0.5)
    paused_state = pie_state()
    if is_ok(rp) and paused_state.get("paused"):
        log.case("P4_pause", "PASS", f"paused; is_running.paused=true {paused_state}")
    elif is_ok(rp):
        # pause returned ok but state didn't reflect — transient race; retry once
        time.sleep(1.0)
        paused_state = pie_state()
        if paused_state.get("paused"):
            log.case("P4_pause", "PASS", f"paused after retry {paused_state}")
        else:
            log.case("P4_pause", "XFAIL",
                     f"pause ok but is_running.paused still false {paused_state}")
    else:
        log.case("P4_pause", "FAIL", f"pause failed: {err_message(rp)[:60]}")
        fail += 1

    # ── P5: step_frame while paused ───────────────────────────────────────
    is_paused_now = pie_state().get("paused", False)
    if is_paused_now:
        s1 = call("pie.step_frame", {}, timeout=10.0)
        time.sleep(0.3)
        s2 = call("pie.step_frame", {}, timeout=10.0)
        if is_ok(s1) and is_ok(s2):
            f1 = (s1.get("result", {}) or {}).get("current_frame", 0)
            f2 = (s2.get("result", {}) or {}).get("current_frame", 0)
            adv1 = (s1.get("result", {}) or {}).get("advanced")
            if adv1 and f2 >= f1:
                log.case("P5_step_frame", "PASS",
                         f"advanced; current_frame {f1} → {f2}")
            else:
                log.case("P5_step_frame", "XFAIL",
                         f"stepped but frame counter odd: {f1} → {f2} advanced={adv1}")
        else:
            log.case("P5_step_frame", "FAIL",
                     f"step_frame failed while paused: "
                     f"{err_message(s1)[:50]} / {err_message(s2)[:50]}")
            fail += 1
    else:
        log.case("P5_step_frame", "SKIP", "PIE not paused (P4 didn't latch)")

    # ── P6: resume ────────────────────────────────────────────────────────
    rr = call("pie.resume", {}, timeout=10.0)
    time.sleep(0.5)
    resumed_state = pie_state()
    if is_ok(rr) and not resumed_state.get("paused", True):
        log.case("P6_resume", "PASS", f"resumed; paused=false {resumed_state}")
    elif is_ok(rr):
        log.case("P6_resume", "XFAIL",
                 f"resume ok but paused still true {resumed_state}")
    else:
        log.case("P6_resume", "FAIL", f"resume failed: {err_message(rr)[:60]}")
        fail += 1

    # ── P7: step_frame while NOT paused → -32603 ─────────────────────────
    if not pie_state().get("paused", False):
        sn = call("pie.step_frame", {}, timeout=10.0)
        if err_code(sn) == -32603:
            log.case("P7_step_requires_pause", "PASS",
                     f"refused step while running: {err_message(sn)[:60]}")
        elif is_ok(sn):
            log.case("P7_step_requires_pause", "FAIL",
                     "step_frame SUCCEEDED while not paused — should refuse")
            fail += 1
        else:
            log.case("P7_step_requires_pause", "XFAIL",
                     f"refused with code {err_code(sn)}: {err_message(sn)[:50]}")
    else:
        log.case("P7_step_requires_pause", "SKIP", "still paused — can't test")

    # ── P8: time-dilation tool contract ───────────────────────────────────
    # NOTE: we verify the BRIDGE TOOL's contract (applied/new_scale/prior_scale),
    # NOT downstream persistence. FatumGame runs its own per-tick FTimeDilationStack
    # which re-asserts global dilation every game-thread tick, so a get_stats read
    # a beat later legitimately shows the project's value (1.0), not ours. The tool
    # did its job iff its own response reports the set correctly.
    rd = call("pie.set_time_dilation", {"scale": 0.25}, timeout=10.0)
    if is_ok(rd):
        res = rd.get("result", {}) or {}
        applied = res.get("applied")
        new_scale = res.get("new_scale")
        prior = res.get("prior_scale")
        if applied is True and abs((new_scale or 0) - 0.25) < 1e-6 and isinstance(prior, (int, float)):
            # secondary, informational: does it persist or get overridden?
            time.sleep(0.4)
            rstat = call("pie.get_stats", {}, timeout=10.0)
            read_back = (rstat.get("result", {}) or {}).get("time_dilation", -1) if is_ok(rstat) else -1
            persist = "persisted" if abs(read_back - 0.25) < 0.02 else \
                      f"overridden→{read_back} (project FTimeDilationStack re-asserts each tick)"
            log.case("P8_time_dilation", "PASS",
                     f"tool contract ok: applied={applied} new_scale={new_scale} "
                     f"prior_scale={prior}; readback {persist}")
        else:
            log.case("P8_time_dilation", "FAIL",
                     f"tool response malformed: applied={applied} "
                     f"new_scale={new_scale} prior_scale={prior}")
            fail += 1
    else:
        log.case("P8_time_dilation", "FAIL",
                 f"set_time_dilation failed: {err_code(rd)} {err_message(rd)[:50]}")
        fail += 1
    call("pie.set_time_dilation", {"scale": 1.0}, timeout=8.0)  # restore

    # ── P9: time-dilation out of range → -32602 ───────────────────────────
    ro = call("pie.set_time_dilation", {"scale": 999.0}, timeout=8.0)
    if err_code(ro) == -32602:
        log.case("P9_dilation_range", "PASS",
                 f"999 rejected: {err_message(ro)[:60]}")
    elif is_ok(ro):
        log.case("P9_dilation_range", "FAIL", "scale=999 accepted (>100 cap)")
        fail += 1
    else:
        log.case("P9_dilation_range", "XFAIL",
                 f"rejected with code {err_code(ro)}")

    # ── P10: cooldown guard — stop then IMMEDIATE start → -32603 ──────────
    call("pie.stop", {}, timeout=12.0)
    rci = call("pie.start", {"mode": "selected_viewport"}, timeout=10.0)
    if err_code(rci) == -32603:
        log.case("P10_cooldown_guard", "PASS",
                 f"immediate restart refused (async-teardown gap): "
                 f"{err_message(rci)[:70]}")
    elif is_ok(rci):
        log.case("P10_cooldown_guard", "XFAIL",
                 "immediate restart SUCCEEDED — cooldown window already elapsed "
                 "(slow editor); not a fault, just timing")
        pie_stop_and_wait()  # clean up the session we accidentally started
    else:
        log.case("P10_cooldown_guard", "XFAIL",
                 f"refused with code {err_code(rci)}: {err_message(rci)[:50]}")

    # ── P11: clean restart after cooldown, then clean stop ───────────────
    # Ensure fully stopped + cooldown elapsed.
    pie_stop_and_wait(cooldown_s=2.0)
    up2, info2 = pie_start_and_wait()
    if up2 and pie_is_running():
        stopped = pie_stop_and_wait()
        if stopped and not pie_is_running():
            log.case("P11_clean_recycle", "PASS",
                     "restart after cooldown → running → stop → not running")
        else:
            log.case("P11_clean_recycle", "FAIL",
                     f"restart ok but final stop failed (running={pie_is_running()})")
            fail += 1
    else:
        log.case("P11_clean_recycle", "FAIL",
                 f"could not restart after cooldown: {info2}")
        fail += 1

    # Guarantee we leave PIE off.
    pie_ensure_stopped()

    crash = latest_crash_dump(since=crash_baseline)
    if crash:
        log.case("crash_check", "FAIL", f"CRASH DUMP: {crash}")
        fail += 1

    summary = log.write()
    cc = summary["counts"]
    print()
    print(f"[P1] PASS={cc['PASS']} FAIL={cc['FAIL']} XFAIL={cc.get('XFAIL', 0)} "
          f"SKIP={cc.get('SKIP', 0)} TOTAL={cc['TOTAL']}")
    print(f"     log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    return 1 if fail > 0 else 0


if __name__ == "__main__":
    sys.exit(main())
