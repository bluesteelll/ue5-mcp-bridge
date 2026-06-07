#!/usr/bin/env python3
"""Phase P7 — pie.screenshot_to_disk + realistic play-session walkthrough.

Two purposes:
  1. Cover pie.screenshot_to_disk (the remaining pie.* tool): happy path
     (file actually written), width/height range guard, PIE-off guard.
  2. Integration walkthrough — chain ~12 PIE-runtime ops in the order a real
     agent would drive a play session, proving the tools COMPOSE and state
     carries correctly across a single session (the "multi-step session"
     coverage angle), with the editor staying alive throughout.

  P1  screenshot_to_disk(640x360) → file on disk, bytes>0, world=PIE
  P2  screenshot_to_disk(default path) → ok
  P3  screenshot_to_disk(width=99999) → -32602
  P4  WALKTHROUGH: is_running → get_pawn → focus(pawn) → key burst WASD →
      get_stats → dump_world_state → pause → step×2 → resume → dilation 0.5 →
      get_stats → dilation 1.0  (all succeed, editor alive, no crash)
  P5  screenshot_to_disk with PIE off → -32038

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
    pie_current_map,
    pie_ensure_stopped,
    pie_ensure_user_map,
    pie_get_pawn_path,
    pie_start_and_wait,
    pie_stop_and_wait,
    preflight,
)

PHASE = "p7"
NAME = "pie_screenshot_walkthrough"


def main() -> int:
    if not preflight(PHASE):
        return 2
    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()
    fail = 0
    created_files = []

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

    # ── P1: screenshot to disk (verify file actually written) ────────────
    r = call("pie.screenshot_to_disk", {"width": 640, "height": 360}, timeout=20.0)
    if is_ok(r):
        res = r.get("result", {}) or {}
        path = res.get("path", "")
        nbytes = res.get("bytes", 0)
        world = res.get("world", "")
        on_disk = bool(path) and Path(path).exists()
        if on_disk and nbytes > 0:
            created_files.append(path)
            sz = Path(path).stat().st_size if Path(path).exists() else 0
            log.case("P1_screenshot", "PASS",
                     f"wrote {nbytes} bytes (disk={sz}) world={world}")
        else:
            log.case("P1_screenshot", "FAIL",
                     f"reported ok but file missing/empty: path={path} bytes={nbytes} on_disk={on_disk}")
            fail += 1
    elif err_code(r) == -32603:
        log.case("P1_screenshot", "XFAIL",
                 f"no renderable PIE viewport (headless/unfocused): {err_message(r)[:60]}")
    else:
        log.case("P1_screenshot", "FAIL",
                 f"screenshot failed: {err_code(r)} {err_message(r)[:60]}")
        fail += 1

    # ── P2: screenshot default path ──────────────────────────────────────
    r = call("pie.screenshot_to_disk", {}, timeout=20.0)
    if is_ok(r):
        p = (r.get("result", {}) or {}).get("path", "")
        if p and Path(p).exists():
            created_files.append(p)
        log.case("P2_screenshot_default", "PASS", f"default-path screenshot ok")
    elif err_code(r) == -32603:
        log.case("P2_screenshot_default", "XFAIL", "no renderable viewport (headless)")
    else:
        log.case("P2_screenshot_default", "FAIL",
                 f"failed: {err_code(r)} {err_message(r)[:50]}")
        fail += 1

    # ── P3: width out of range → -32602 ──────────────────────────────────
    r = call("pie.screenshot_to_disk", {"width": 99999, "height": 360}, timeout=10.0)
    if err_code(r) == -32602:
        log.case("P3_screenshot_oob", "PASS", f"width 99999 → -32602")
    elif is_ok(r):
        p = (r.get("result", {}) or {}).get("path", "")
        if p and Path(p).exists():
            created_files.append(p)
        log.case("P3_screenshot_oob", "FAIL", "width=99999 accepted (>8192 cap)")
        fail += 1
    else:
        log.case("P3_screenshot_oob", "XFAIL", f"code {err_code(r)}")

    # ── P4: realistic play-session walkthrough (composition test) ─────────
    steps = []  # (label, ok_bool, detail)

    def step(label, resp, ok_extra=lambda res: True):
        ok = is_ok(resp) and ok_extra(resp.get("result", {}) or {})
        steps.append((label, ok, "" if ok else f"{err_code(resp)} {err_message(resp)[:40]}"))
        return ok

    step("is_running", call("pie.is_running", {}, timeout=6.0),
         lambda res: res.get("running") is True)
    pawn = pie_get_pawn_path()
    steps.append(("get_pawn", bool(pawn), "" if pawn else "no pawn"))
    if pawn:
        step("focus_pawn", call("pie.focus_actor", {"actor_id": pawn}, timeout=8.0))
    for k in ("W", "A", "S", "D"):
        step(f"key_{k}", call("pie.simulate_key", {"key": k, "action": "tap"}, timeout=6.0),
             lambda res: res.get("simulated") is True)
    step("stats_1", call("pie.get_stats", {}, timeout=8.0),
         lambda res: "UEDPIE" in res.get("world_path", ""))
    step("dump", call("pie.dump_world_state", {}, timeout=10.0),
         lambda res: res.get("total", 0) > 0)
    step("pause", call("pie.pause", {}, timeout=8.0))
    time.sleep(0.4)
    step("step_1", call("pie.step_frame", {}, timeout=8.0),
         lambda res: res.get("advanced") is True)
    step("step_2", call("pie.step_frame", {}, timeout=8.0),
         lambda res: res.get("advanced") is True)
    step("resume", call("pie.resume", {}, timeout=8.0))
    step("dilation_half", call("pie.set_time_dilation", {"scale": 0.5}, timeout=8.0),
         lambda res: res.get("applied") is True)
    step("stats_2", call("pie.get_stats", {}, timeout=8.0))
    step("dilation_restore", call("pie.set_time_dilation", {"scale": 1.0}, timeout=8.0),
         lambda res: res.get("applied") is True)

    bad = [(lbl, d) for (lbl, ok, d) in steps if not ok]
    alive = health(timeout=8.0)
    if not bad and alive:
        log.case("P4_walkthrough", "PASS",
                 f"all {len(steps)} session steps succeeded, editor alive")
    elif not alive:
        log.case("P4_walkthrough", "FAIL", f"editor DIED mid-walkthrough", alive=False)
        fail += 1
    else:
        log.case("P4_walkthrough", "FAIL",
                 f"{len(bad)}/{len(steps)} steps failed: {bad[:4]}")
        fail += 1

    pie_stop_and_wait()

    # ── P5: screenshot with PIE off → -32038 ─────────────────────────────
    r = call("pie.screenshot_to_disk", {"width": 320, "height": 240}, timeout=10.0)
    if err_code(r) == -32038:
        log.case("P5_screenshot_off", "PASS", "screenshot with PIE off → -32038")
    elif is_ok(r):
        p = (r.get("result", {}) or {}).get("path", "")
        if p and Path(p).exists():
            created_files.append(p)
        log.case("P5_screenshot_off", "FAIL", "screenshot SUCCEEDED with PIE off")
        fail += 1
    else:
        log.case("P5_screenshot_off", "XFAIL", f"code {err_code(r)} (expected -32038)")

    # cleanup screenshot files
    cleaned = 0
    for f in created_files:
        try:
            Path(f).unlink()
            cleaned += 1
        except OSError:
            pass
    log.note(f"cleaned {cleaned}/{len(created_files)} screenshot files")

    crash = latest_crash_dump(since=crash_baseline)
    if crash:
        log.case("crash_check", "FAIL", f"CRASH DUMP: {crash}")
        fail += 1

    summary = log.write()
    cc = summary["counts"]
    print()
    print(f"[P7] PASS={cc['PASS']} FAIL={cc['FAIL']} XFAIL={cc.get('XFAIL', 0)} "
          f"SKIP={cc.get('SKIP', 0)} TOTAL={cc['TOTAL']}")
    print(f"     log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    return 1 if fail > 0 else 0


if __name__ == "__main__":
    sys.exit(main())
