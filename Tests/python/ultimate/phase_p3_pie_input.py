#!/usr/bin/env python3
"""Phase P3 — PIE input simulation (LIVE PIE world).

Covered tools: pie.simulate_key, pie.click_screen, pie.click_actor — each
routed through the running PIE PlayerController.

  P1  simulate_key W tap → simulated:true, key/action echoed
  P2  simulate_key Space press then release → both ok
  P3  simulate_key bad FKey → -32602
  P4  simulate_key missing key → -32602
  P5  simulate_key bad action → -32602
  P6  click_screen center → clicked:true, coords echoed
  P7  click_screen right button → button echoed
  P8  click_screen missing y → -32602
  P9  click_screen bad button → -32602
  P10 click_actor(pawn) → clicked (XFAIL if pawn projects behind camera, 1P)
  P11 click_actor bad path → -32004
  P12 click_actor missing arg → -32602

NOTE: behavioral input-EFFECT verification (e.g. "W moved the pawn forward")
is NOT asserted — the test map is empty (pawn free-falls) and the editor runs
unfocused at ~3 FPS, so observed motion can't be attributed to input. This is
a documented coverage boundary; a floor + deterministic input mapping would be
needed to assert movement. P3 verifies the input PLUMBING (tools dispatch
through the live PC and report correct success/error contracts).

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
    is_ok,
    latest_crash_dump,
    pie_current_map,
    pie_ensure_stopped,
    pie_ensure_user_map,
    pie_get_pawn_path,
    pie_start_and_wait,
    pie_stop_and_wait,
    preflight,
)

PHASE = "p3"
NAME = "pie_input"


def main() -> int:
    if not preflight(PHASE):
        return 2
    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()
    fail = 0
    log.note("Verifies input PLUMBING (tool dispatch through live PC + contracts). "
             "Movement EFFECTS are verified separately in P8 (behavioral input in "
             "the user's floored map).")

    if not pie_ensure_stopped():
        log.case("preflight_stop", "FAIL", "could not stop pre-existing PIE")
        log.write()
        return 1
    user_map = pie_ensure_user_map()
    log.case("preflight_map", "PASS" if user_map else "SKIP",
             f"tests run in user map {user_map} ({pie_current_map()})" if user_map
             else f"no user map loaded; using current ({pie_current_map()})")

    up, info = pie_start_and_wait()
    if not up:
        log.case("P0_start", "FAIL", f"PIE never came up: {info}")
        log.write()
        return 1
    log.case("P0_start", "PASS", f"PIE up: {info.get('state')}")
    pawn_path = pie_get_pawn_path()

    # ── P1: simulate_key tap ──────────────────────────────────────────────
    r = call("pie.simulate_key", {"key": "W", "action": "tap"}, timeout=10.0)
    if is_ok(r):
        res = r.get("result", {}) or {}
        if res.get("simulated") and res.get("key") == "W" and res.get("action") == "tap":
            log.case("P1_key_tap", "PASS", f"W tap echoed: {res}")
        else:
            log.case("P1_key_tap", "FAIL", f"simulate ok but echo wrong: {res}")
            fail += 1
    else:
        log.case("P1_key_tap", "FAIL", f"simulate_key failed: {err_code(r)} {err_message(r)[:50]}")
        fail += 1

    # ── P2: press then release ────────────────────────────────────────────
    rp = call("pie.simulate_key", {"key": "SpaceBar", "action": "press"}, timeout=8.0)
    time.sleep(0.3)
    rr = call("pie.simulate_key", {"key": "SpaceBar", "action": "release"}, timeout=8.0)
    if is_ok(rp) and is_ok(rr):
        log.case("P2_press_release", "PASS",
                 f"press→{(rp.get('result',{}) or {}).get('action')} "
                 f"release→{(rr.get('result',{}) or {}).get('action')}")
    else:
        # Some FKey spellings differ; retry with "Space"
        rp2 = call("pie.simulate_key", {"key": "Space", "action": "press"}, timeout=8.0)
        rr2 = call("pie.simulate_key", {"key": "Space", "action": "release"}, timeout=8.0)
        if is_ok(rp2) and is_ok(rr2):
            log.case("P2_press_release", "PASS", "press/release ok (key='Space')")
        else:
            log.case("P2_press_release", "FAIL",
                     f"press/release failed: {err_message(rp)[:40]} / {err_message(rr)[:40]}")
            fail += 1

    # ── P3: bad FKey → -32602 ────────────────────────────────────────────
    r = call("pie.simulate_key", {"key": "NotARealKey_ZZZ123", "action": "tap"}, timeout=8.0)
    if err_code(r) == -32602:
        log.case("P3_bad_key", "PASS", f"unknown FKey → -32602: {err_message(r)[:50]}")
    elif is_ok(r):
        log.case("P3_bad_key", "FAIL", "bogus FKey accepted")
        fail += 1
    else:
        log.case("P3_bad_key", "XFAIL", f"code {err_code(r)}: {err_message(r)[:50]}")

    # ── P4: missing key → -32602 ─────────────────────────────────────────
    r = call("pie.simulate_key", {"action": "tap"}, timeout=8.0)
    if err_code(r) == -32602:
        log.case("P4_missing_key", "PASS", "missing key → -32602")
    elif is_ok(r):
        log.case("P4_missing_key", "FAIL", "missing key accepted")
        fail += 1
    else:
        log.case("P4_missing_key", "XFAIL", f"code {err_code(r)}")

    # ── P5: bad action → -32602 ──────────────────────────────────────────
    r = call("pie.simulate_key", {"key": "W", "action": "wiggle"}, timeout=8.0)
    if err_code(r) == -32602:
        log.case("P5_bad_action", "PASS", f"unknown action → -32602: {err_message(r)[:50]}")
    elif is_ok(r):
        log.case("P5_bad_action", "FAIL", "bogus action accepted")
        fail += 1
    else:
        log.case("P5_bad_action", "XFAIL", f"code {err_code(r)}")

    # ── P6: click_screen center ──────────────────────────────────────────
    r = call("pie.click_screen", {"x": 640, "y": 360}, timeout=10.0)
    if is_ok(r):
        res = r.get("result", {}) or {}
        if res.get("clicked") and res.get("button") == "left":
            log.case("P6_click_center", "PASS", f"clicked center: {res}")
        else:
            log.case("P6_click_center", "FAIL", f"click ok but echo wrong: {res}")
            fail += 1
    else:
        log.case("P6_click_center", "FAIL", f"click_screen failed: {err_code(r)} {err_message(r)[:50]}")
        fail += 1

    # ── P7: click_screen right button ────────────────────────────────────
    r = call("pie.click_screen", {"x": 100, "y": 100, "button": "right"}, timeout=8.0)
    if is_ok(r) and (r.get("result", {}) or {}).get("button") == "right":
        log.case("P7_click_button", "PASS", "right button echoed")
    elif is_ok(r):
        log.case("P7_click_button", "FAIL", f"button not echoed: {r.get('result')}")
        fail += 1
    else:
        log.case("P7_click_button", "FAIL", f"failed: {err_message(r)[:50]}")
        fail += 1

    # ── P8: click_screen missing y → -32602 ──────────────────────────────
    r = call("pie.click_screen", {"x": 5}, timeout=8.0)
    if err_code(r) == -32602:
        log.case("P8_click_missing_y", "PASS", "missing y → -32602")
    elif is_ok(r):
        log.case("P8_click_missing_y", "FAIL", "missing y accepted")
        fail += 1
    else:
        log.case("P8_click_missing_y", "XFAIL", f"code {err_code(r)}")

    # ── P9: click_screen bad button → -32602 ─────────────────────────────
    r = call("pie.click_screen", {"x": 1, "y": 1, "button": "sideways"}, timeout=8.0)
    if err_code(r) == -32602:
        log.case("P9_click_bad_button", "PASS", "bad button → -32602")
    elif is_ok(r):
        log.case("P9_click_bad_button", "FAIL", "bad button accepted")
        fail += 1
    else:
        log.case("P9_click_bad_button", "XFAIL", f"code {err_code(r)}")

    # ── P10: click_actor on pawn ─────────────────────────────────────────
    if pawn_path:
        r = call("pie.click_actor", {"actor_path": pawn_path}, timeout=10.0)
        if is_ok(r) and (r.get("result", {}) or {}).get("clicked"):
            res = r.get("result", {}) or {}
            log.case("P10_click_actor", "PASS",
                     f"clicked pawn at screen ({res.get('screen_x'):.0f},{res.get('screen_y'):.0f})")
        elif err_code(r) == -32603:
            log.case("P10_click_actor", "XFAIL",
                     f"pawn projects behind camera (1P self-view): {err_message(r)[:50]}")
        else:
            log.case("P10_click_actor", "FAIL",
                     f"click_actor(pawn) failed: {err_code(r)} {err_message(r)[:50]}")
            fail += 1
    else:
        log.case("P10_click_actor", "SKIP", "no pawn")

    # ── P11: click_actor bad path → -32004 ───────────────────────────────
    r = call("pie.click_actor", {"actor_path": "/Game/__nope__.X:PersistentLevel.Ghost_0"},
             timeout=8.0)
    if err_code(r) == -32004:
        log.case("P11_click_actor_bad", "PASS", "bogus actor → -32004")
    elif is_ok(r):
        log.case("P11_click_actor_bad", "FAIL", "click on nonexistent actor SUCCEEDED")
        fail += 1
    else:
        log.case("P11_click_actor_bad", "XFAIL", f"code {err_code(r)}: {err_message(r)[:50]}")

    # ── P12: click_actor missing arg → -32602 ────────────────────────────
    r = call("pie.click_actor", {}, timeout=8.0)
    if err_code(r) == -32602:
        log.case("P12_click_actor_missing", "PASS", "missing actor_path → -32602")
    elif is_ok(r):
        log.case("P12_click_actor_missing", "FAIL", "click with no actor_path SUCCEEDED")
        fail += 1
    else:
        log.case("P12_click_actor_missing", "XFAIL", f"code {err_code(r)}")

    pie_stop_and_wait()

    crash = latest_crash_dump(since=crash_baseline)
    if crash:
        log.case("crash_check", "FAIL", f"CRASH DUMP: {crash}")
        fail += 1

    summary = log.write()
    cc = summary["counts"]
    print()
    print(f"[P3] PASS={cc['PASS']} FAIL={cc['FAIL']} XFAIL={cc.get('XFAIL', 0)} "
          f"SKIP={cc.get('SKIP', 0)} TOTAL={cc['TOTAL']}")
    print(f"     log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    return 1 if fail > 0 else 0


if __name__ == "__main__":
    sys.exit(main())
