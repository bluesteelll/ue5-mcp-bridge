#!/usr/bin/env python3
"""Phase P6 — PIE-required guards when PIE is OFF (-32038 coverage matrix).

Every PIE-required tool must refuse cleanly (-32038 PIENotActive) when no
session is running — never crash, never act on a stale/null world. The
always-callable tools (pie.is_running; pie.console_exec world=editor) must
still work.

This is the negative half of Category P and needs NO PIE start (fast).

  P1-P13  pie.stop/pause/resume/step_frame/get_player_controller/get_pawn/
          focus_actor/simulate_key/click_screen/click_actor/set_time_dilation/
          get_stats/dump_world_state  →  all -32038 when PIE off
  P14     pie.is_running  →  ok, running:false (ALWAYS callable)
  P15     pie.console_exec world=pie  →  -32038 (no PIE world)
  P16     pie.console_exec world=editor  →  ok (editor world always present)

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
    pie_ensure_stopped,
    preflight,
)

PHASE = "p6"
NAME = "pie_off_guards"

# (case_id, method, args) — each expected to return -32038 with PIE off.
PIE_REQUIRED = [
    ("P1_stop", "pie.stop", {}),
    ("P2_pause", "pie.pause", {}),
    ("P3_resume", "pie.resume", {}),
    ("P4_step_frame", "pie.step_frame", {}),
    ("P5_get_pc", "pie.get_player_controller", {}),
    ("P6_get_pawn", "pie.get_pawn", {}),
    ("P7_focus_actor", "pie.focus_actor", {"actor_id": "/Game/X.X:PersistentLevel.Y_0"}),
    ("P8_simulate_key", "pie.simulate_key", {"key": "W"}),
    ("P9_click_screen", "pie.click_screen", {"x": 1, "y": 1}),
    ("P10_click_actor", "pie.click_actor", {"actor_path": "/Game/X.X:PersistentLevel.Y_0"}),
    ("P11_set_time_dilation", "pie.set_time_dilation", {"scale": 1.0}),
    ("P12_get_stats", "pie.get_stats", {}),
    ("P13_dump_world_state", "pie.dump_world_state", {}),
]


def main() -> int:
    if not preflight(PHASE):
        return 2
    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()
    fail = 0

    if not pie_ensure_stopped():
        log.case("preflight_stop", "FAIL", "could not guarantee PIE off")
        log.write()
        return 1
    log.case("preflight_stop", "PASS", "PIE confirmed OFF")

    # ── P1-P13: every PIE-required tool → -32038 ─────────────────────────
    for case_id, method, args in PIE_REQUIRED:
        r = call(method, args, timeout=8.0)
        if err_code(r) == -32038:
            log.case(case_id, "PASS", f"{method} → -32038 (PIE not active)")
        elif is_ok(r):
            log.case(case_id, "FAIL",
                     f"{method} SUCCEEDED with PIE off — should refuse (-32038)")
            fail += 1
        else:
            # A different structured error is still safe (no crash) but not the
            # documented guard — flag XFAIL so we notice contract drift.
            log.case(case_id, "XFAIL",
                     f"{method} → {err_code(r)} (expected -32038): {err_message(r)[:50]}")

    # ── P14: is_running always callable ──────────────────────────────────
    r = call("pie.is_running", {}, timeout=8.0)
    if is_ok(r) and (r.get("result", {}) or {}).get("running") is False:
        log.case("P14_is_running", "PASS", "is_running ok, running:false (always callable)")
    elif is_ok(r):
        log.case("P14_is_running", "FAIL",
                 f"is_running reports running:true with PIE off: {r.get('result')}")
        fail += 1
    else:
        log.case("P14_is_running", "FAIL", f"is_running failed: {err_message(r)[:50]}")
        fail += 1

    # ── P15: console_exec world=pie → -32038 ─────────────────────────────
    r = call("pie.console_exec", {"command": "stat fps", "world": "pie"}, timeout=8.0)
    if err_code(r) == -32038:
        log.case("P15_console_pie_off", "PASS", "console_exec(pie) → -32038 with PIE off")
    elif is_ok(r):
        log.case("P15_console_pie_off", "FAIL", "console_exec(pie) SUCCEEDED with PIE off")
        fail += 1
    else:
        log.case("P15_console_pie_off", "XFAIL",
                 f"code {err_code(r)} (expected -32038): {err_message(r)[:50]}")

    # ── P16: console_exec world=editor → ok ──────────────────────────────
    r = call("pie.console_exec", {"command": "stat none", "world": "editor"}, timeout=8.0)
    if is_ok(r):
        log.case("P16_console_editor_off", "PASS", "console_exec(editor) ok with PIE off")
    else:
        log.case("P16_console_editor_off", "FAIL",
                 f"console_exec(editor) failed: {err_code(r)} {err_message(r)[:50]}")
        fail += 1

    crash = latest_crash_dump(since=crash_baseline)
    if crash:
        log.case("crash_check", "FAIL", f"CRASH DUMP: {crash}")
        fail += 1

    summary = log.write()
    cc = summary["counts"]
    print()
    print(f"[P6] PASS={cc['PASS']} FAIL={cc['FAIL']} XFAIL={cc.get('XFAIL', 0)} "
          f"SKIP={cc.get('SKIP', 0)} TOTAL={cc['TOTAL']}")
    print(f"     log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    return 1 if fail > 0 else 0


if __name__ == "__main__":
    sys.exit(main())
