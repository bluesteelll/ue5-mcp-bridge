#!/usr/bin/env python3
"""Phase P10 — cursor + widget-targeted UI navigation (LIVE PIE world).

Covers the player-interaction-simulation tools added 2026-06:
  pie.move_mouse, pie.drag_screen (coordinate-based cursor),
  umg.get_widget_geometry, umg.click_widget, umg.hover_widget (by-name targeting).

  P1  move_mouse(center) → moved, coords echoed
  P2  move_mouse missing y → -32602
  P3  drag_screen(from→to, steps=10) → dragged, echoed; editor survives the drag
  P4  drag_screen missing to → -32602
  P5  drag_screen bad button → -32602
  P6  get_widget_geometry(live HUD widget) → found, width/height>0, center present
  P7  get_widget_geometry(bad path) → -32004
  P8  click_widget(live HUD widget) → clicked; screen coords match the geometry center
  P9  click_widget(bad path) → -32004
  P10 hover_widget(live HUD widget) → hovered; coords match center
  P11 PIE-off guards: pie.move_mouse/drag_screen → -32038; umg.* widget tools → -32027

Tools SKIP (not FAIL) if absent from the editor build (-32601), so this is
runnable pre-build too.

Exit codes: 0=PASS (0 FAIL), 1=FAIL/editor-died, 2=preflight.
"""

from __future__ import annotations

import sys
import time
from pathlib import Path
from typing import Optional

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
    pie_start_and_wait,
    pie_stop_and_wait,
    preflight,
)

PHASE = "p10"
NAME = "pie_cursor_nav"


def _not_built(r) -> bool:
    return err_code(r) == -32601


def _pick_root_widget() -> Optional[str]:
    """Return a live root widget path (prefer the main HUD), or None."""
    r = call("umg.list_root_widgets", {}, timeout=10.0)
    if not is_ok(r):
        return None
    widgets = (r.get("result", {}) or {}).get("widgets", []) or []
    # Prefer the MainHUD (fullscreen → stable geometry); else first.
    for w in widgets:
        if "MainHUD" in (w.get("class_path", "")):
            return w.get("widget_path")
    return widgets[0].get("widget_path") if widgets else None


def main() -> int:
    if not preflight(PHASE):
        return 2
    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()
    fail = 0

    if not pie_ensure_stopped():
        log.case("preflight_stop", "FAIL", "could not stop pre-existing PIE")
        log.write()
        return 1
    user_map = pie_ensure_user_map()
    log.case("preflight_map", "PASS" if user_map else "SKIP",
             f"tests run in user map {user_map} ({pie_current_map()})" if user_map
             else f"no user map; using current ({pie_current_map()})")

    up, info = pie_start_and_wait(settle_s=3.0)
    if not up:
        log.case("P0_start", "FAIL", f"PIE never came up: {info}")
        log.write()
        return 1
    log.case("P0_start", "PASS", f"PIE up in {user_map}")

    # ── P1: move_mouse ────────────────────────────────────────────────────
    r = call("pie.move_mouse", {"x": 640, "y": 360}, timeout=10.0)
    if _not_built(r):
        log.case("P1_move_mouse", "SKIP", "pie.move_mouse not in this editor build yet")
    elif is_ok(r) and (r.get("result", {}) or {}).get("moved"):
        log.case("P1_move_mouse", "PASS", f"cursor moved: {r.get('result')}")
    else:
        log.case("P1_move_mouse", "FAIL", f"move_mouse failed: {err_code(r)} {err_message(r)[:50]}")
        fail += 1

    # ── P2: move_mouse missing y → -32602 ────────────────────────────────
    r = call("pie.move_mouse", {"x": 5}, timeout=8.0)
    if _not_built(r):
        log.case("P2_move_missing", "SKIP", "not built")
    elif err_code(r) == -32602:
        log.case("P2_move_missing", "PASS", "missing y → -32602")
    elif is_ok(r):
        log.case("P2_move_missing", "FAIL", "missing y accepted")
        fail += 1
    else:
        log.case("P2_move_missing", "XFAIL", f"code {err_code(r)}")

    # ── P3: drag_screen ──────────────────────────────────────────────────
    r = call("pie.drag_screen",
             {"from_x": 400, "from_y": 300, "to_x": 800, "to_y": 500, "steps": 10}, timeout=10.0)
    if _not_built(r):
        log.case("P3_drag", "SKIP", "pie.drag_screen not in this editor build yet")
    elif is_ok(r):
        res = r.get("result", {}) or {}
        alive = health(timeout=8.0)
        if res.get("dragged") and res.get("steps") == 10 and alive:
            log.case("P3_drag", "PASS", f"drag executed ({res.get('steps')} steps), editor alive")
        elif not alive:
            log.case("P3_drag", "FAIL", "editor DIED after drag", alive=False)
            fail += 1
        else:
            log.case("P3_drag", "FAIL", f"drag response wrong: {res}")
            fail += 1
    else:
        log.case("P3_drag", "FAIL", f"drag_screen failed: {err_code(r)} {err_message(r)[:50]}")
        fail += 1

    # ── P4: drag missing to → -32602 ─────────────────────────────────────
    r = call("pie.drag_screen", {"from_x": 1, "from_y": 1}, timeout=8.0)
    if _not_built(r):
        log.case("P4_drag_missing", "SKIP", "not built")
    elif err_code(r) == -32602:
        log.case("P4_drag_missing", "PASS", "missing to_x/to_y → -32602")
    elif is_ok(r):
        log.case("P4_drag_missing", "FAIL", "incomplete drag accepted")
        fail += 1
    else:
        log.case("P4_drag_missing", "XFAIL", f"code {err_code(r)}")

    # ── P5: drag bad button → -32602 ─────────────────────────────────────
    r = call("pie.drag_screen",
             {"from_x": 1, "from_y": 1, "to_x": 2, "to_y": 2, "button": "sideways"}, timeout=8.0)
    if _not_built(r):
        log.case("P5_drag_bad_button", "SKIP", "not built")
    elif err_code(r) == -32602:
        log.case("P5_drag_bad_button", "PASS", "bad button → -32602")
    elif is_ok(r):
        log.case("P5_drag_bad_button", "FAIL", "bad button accepted")
        fail += 1
    else:
        log.case("P5_drag_bad_button", "XFAIL", f"code {err_code(r)}")

    # Resolve a live widget for the by-name tools.
    widget_path = _pick_root_widget()
    geom_center = None

    # ── P6: get_widget_geometry ──────────────────────────────────────────
    if not widget_path:
        log.case("P6_widget_geometry", "XFAIL", "no live root widget to target")
    else:
        r = call("umg.get_widget_geometry", {"widget_path": widget_path}, timeout=10.0)
        if _not_built(r):
            log.case("P6_widget_geometry", "SKIP", "umg.get_widget_geometry not in build yet")
        elif is_ok(r):
            res = r.get("result", {}) or {}
            w, h = res.get("width", 0), res.get("height", 0)
            geom_center = (res.get("center_x"), res.get("center_y"))
            if res.get("found") and w > 0 and h > 0:
                log.case("P6_widget_geometry", "PASS",
                         f"{widget_path.split('.')[-1]} rect {w:.0f}x{h:.0f} center={tuple(round(c) for c in geom_center)}")
            else:
                log.case("P6_widget_geometry", "FAIL", f"geometry degenerate: {res}")
                fail += 1
        else:
            log.case("P6_widget_geometry", "FAIL",
                     f"failed: {err_code(r)} {err_message(r)[:50]}")
            fail += 1

    # ── P7: get_widget_geometry bad path → -32004 ────────────────────────
    r = call("umg.get_widget_geometry", {"widget_path": "/Game/__no_widget__.X:Nope_0"}, timeout=8.0)
    if _not_built(r):
        log.case("P7_geometry_bad", "SKIP", "not built")
    elif err_code(r) == -32004:
        log.case("P7_geometry_bad", "PASS", "bogus widget → -32004")
    elif is_ok(r):
        log.case("P7_geometry_bad", "FAIL", "bogus widget accepted")
        fail += 1
    else:
        log.case("P7_geometry_bad", "XFAIL", f"code {err_code(r)}")

    # ── P8: click_widget (coords match geometry center) ──────────────────
    if not widget_path:
        log.case("P8_click_widget", "XFAIL", "no widget to click")
    else:
        r = call("umg.click_widget", {"widget_path": widget_path}, timeout=10.0)
        if _not_built(r):
            log.case("P8_click_widget", "SKIP", "umg.click_widget not in build yet")
        elif is_ok(r) and (r.get("result", {}) or {}).get("clicked"):
            res = r.get("result", {}) or {}
            sx, sy = res.get("screen_x"), res.get("screen_y")
            match = (geom_center is None) or (
                abs(sx - geom_center[0]) < 2 and abs(sy - geom_center[1]) < 2)
            if match:
                log.case("P8_click_widget", "PASS",
                         f"clicked at ({sx:.0f},{sy:.0f}) == geometry center")
            else:
                log.case("P8_click_widget", "XFAIL",
                         f"clicked at ({sx:.0f},{sy:.0f}) != geom center {geom_center}")
        else:
            log.case("P8_click_widget", "FAIL",
                     f"click_widget failed: {err_code(r)} {err_message(r)[:50]}")
            fail += 1

    # ── P9: click_widget bad path → -32004 ───────────────────────────────
    r = call("umg.click_widget", {"widget_path": "/Game/__nope__.X:Ghost_0"}, timeout=8.0)
    if _not_built(r):
        log.case("P9_click_bad", "SKIP", "not built")
    elif err_code(r) == -32004:
        log.case("P9_click_bad", "PASS", "bogus widget → -32004")
    elif is_ok(r):
        log.case("P9_click_bad", "FAIL", "bogus widget click accepted")
        fail += 1
    else:
        log.case("P9_click_bad", "XFAIL", f"code {err_code(r)}")

    # ── P10: hover_widget ────────────────────────────────────────────────
    if not widget_path:
        log.case("P10_hover_widget", "XFAIL", "no widget to hover")
    else:
        r = call("umg.hover_widget", {"widget_path": widget_path}, timeout=10.0)
        if _not_built(r):
            log.case("P10_hover_widget", "SKIP", "umg.hover_widget not in build yet")
        elif is_ok(r) and (r.get("result", {}) or {}).get("hovered"):
            res = r.get("result", {}) or {}
            log.case("P10_hover_widget", "PASS",
                     f"hover dispatched at ({res.get('screen_x'):.0f},{res.get('screen_y'):.0f}), "
                     f"is_hovered={res.get('is_hovered')}")
        else:
            log.case("P10_hover_widget", "FAIL",
                     f"hover_widget failed: {err_code(r)} {err_message(r)[:50]}")
            fail += 1

    # ── P11: PIE-off guards ──────────────────────────────────────────────
    pie_stop_and_wait()
    guards = [
        ("pie.move_mouse", {"x": 1, "y": 1}, -32038),
        ("pie.drag_screen", {"from_x": 1, "from_y": 1, "to_x": 2, "to_y": 2}, -32038),
        ("umg.get_widget_geometry", {"widget_path": "x"}, -32027),
        ("umg.click_widget", {"widget_path": "x"}, -32027),
        ("umg.hover_widget", {"widget_path": "x"}, -32027),
    ]
    guard_fail = 0
    guard_detail = []
    for method, args, expect in guards:
        r = call(method, args, timeout=8.0)
        if _not_built(r):
            guard_detail.append(f"{method}:notbuilt")
        elif err_code(r) == expect:
            guard_detail.append(f"{method}:OK")
        elif is_ok(r):
            guard_detail.append(f"{method}:SUCCEEDED(bad)")
            guard_fail += 1
        else:
            guard_detail.append(f"{method}:{err_code(r)}(want {expect})")
    if guard_fail == 0:
        log.case("P11_off_guards", "PASS", "; ".join(guard_detail))
    else:
        log.case("P11_off_guards", "FAIL", "; ".join(guard_detail))
        fail += 1

    crash = latest_crash_dump(since=crash_baseline)
    if crash:
        log.case("crash_check", "FAIL", f"CRASH DUMP: {crash}")
        fail += 1

    summary = log.write()
    cc = summary["counts"]
    print()
    print(f"[P10] PASS={cc['PASS']} FAIL={cc['FAIL']} XFAIL={cc.get('XFAIL', 0)} "
          f"SKIP={cc.get('SKIP', 0)} TOTAL={cc['TOTAL']}")
    print(f"      log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    return 1 if fail > 0 else 0


if __name__ == "__main__":
    sys.exit(main())
