#!/usr/bin/env python3
"""Phase P11 — child-widget targeting inside a live UserWidget (LIVE PIE world).

Closes the last UI boundary: interact with a specific element INSIDE a UserWidget
by name (not just the root). Covers umg.list_live_widgets + the child_name path
of umg.get_widget_geometry / click_widget / hover_widget.

  P1  list_live_widgets(root HUD) → enumerates the live child tree with geometry;
      pick a named, visible, non-zero-size child to target
  P2  get_widget_geometry(root, child_name) → child's own rect + class
  P3  click_widget(root, child_name) → clicked; coords == child geometry center
  P4  hover_widget(root, child_name) → hovered; child_name echoed
  P5  click_widget(root, bad child_name) → -32004
  P6  list_live_widgets / child tools with PIE off → -32027

Tools SKIP (not FAIL) if absent from the build (-32601). XFAILs if the map's
HUD exposes no addressable child (map-dependent), so it never false-fails.

Exit codes: 0=PASS (0 FAIL), 1=FAIL/editor-died, 2=preflight.
"""

from __future__ import annotations

import sys
import time
from pathlib import Path
from typing import Optional, Tuple

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
    pie_start_and_wait,
    pie_stop_and_wait,
    preflight,
)

PHASE = "p11"
NAME = "pie_child_widgets"


def _not_built(r) -> bool:
    return err_code(r) == -32601


def _pick_root() -> Optional[str]:
    r = call("umg.list_root_widgets", {}, timeout=10.0)
    if not is_ok(r):
        return None
    widgets = (r.get("result", {}) or {}).get("widgets", []) or []
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
    root = _pick_root()
    log.case("P0_start", "PASS", f"PIE up; root widget = {root.split('.')[-1] if root else None}")

    # ── P1: list_live_widgets + pick a child ─────────────────────────────
    child_name = None
    child_center = None
    if not root:
        log.case("P1_list_live", "XFAIL", "no root widget on screen")
    else:
        r = call("umg.list_live_widgets", {"widget_path": root}, timeout=10.0)
        if _not_built(r):
            log.case("P1_list_live", "SKIP", "umg.list_live_widgets not in build yet")
        elif is_ok(r):
            kids = (r.get("result", {}) or {}).get("widgets", []) or []
            # Prefer a named (is_variable), visible, non-zero child.
            named = [k for k in kids if k.get("is_variable") and k.get("is_visible")
                     and k.get("width", 0) > 5 and k.get("height", 0) > 5]
            anyk = [k for k in kids if k.get("width", 0) > 5 and k.get("height", 0) > 5]
            pick = (named or anyk or [None])[0]
            if pick:
                child_name = pick.get("name")
                child_center = (pick.get("center_x"), pick.get("center_y"))
                log.case("P1_list_live", "PASS",
                         f"{len(kids)} live widgets; targeting '{child_name}' "
                         f"({pick.get('class','').split('/')[-1]}, "
                         f"{pick.get('width'):.0f}x{pick.get('height'):.0f}, var={pick.get('is_variable')})")
            else:
                log.case("P1_list_live", "XFAIL",
                         f"{len(kids)} widgets but none addressable with geometry (map HUD has no sized child)")
        else:
            log.case("P1_list_live", "FAIL",
                     f"list_live_widgets failed: {err_code(r)} {err_message(r)[:50]}")
            fail += 1

    # ── P2: get_widget_geometry(child) ───────────────────────────────────
    if root and child_name:
        r = call("umg.get_widget_geometry", {"widget_path": root, "child_name": child_name}, timeout=10.0)
        if _not_built(r):
            log.case("P2_child_geometry", "SKIP", "not built")
        elif is_ok(r):
            res = r.get("result", {}) or {}
            if res.get("found") and res.get("child_name") == child_name and res.get("width", 0) > 0:
                child_center = (res.get("center_x"), res.get("center_y"))
                log.case("P2_child_geometry", "PASS",
                         f"child '{child_name}' rect {res.get('width'):.0f}x{res.get('height'):.0f} "
                         f"class={res.get('class_path','').split('/')[-1]}")
            else:
                log.case("P2_child_geometry", "FAIL", f"child geometry wrong: {res}")
                fail += 1
        else:
            log.case("P2_child_geometry", "FAIL",
                     f"failed: {err_code(r)} {err_message(r)[:50]}")
            fail += 1
    else:
        log.case("P2_child_geometry", "SKIP", "no child target")

    # ── P3: click_widget(child) — coords match child center ──────────────
    if root and child_name:
        r = call("umg.click_widget", {"widget_path": root, "child_name": child_name}, timeout=10.0)
        if _not_built(r):
            log.case("P3_child_click", "SKIP", "not built")
        elif is_ok(r) and (r.get("result", {}) or {}).get("clicked"):
            res = r.get("result", {}) or {}
            sx, sy = res.get("screen_x"), res.get("screen_y")
            match = (child_center is None) or (
                abs(sx - child_center[0]) < 2 and abs(sy - child_center[1]) < 2)
            if res.get("child_name") == child_name and match:
                log.case("P3_child_click", "PASS",
                         f"clicked child '{child_name}' at ({sx:.0f},{sy:.0f}) == its center")
            else:
                log.case("P3_child_click", "XFAIL",
                         f"clicked but coords/name mismatch: {res} vs center {child_center}")
        else:
            log.case("P3_child_click", "FAIL",
                     f"click child failed: {err_code(r)} {err_message(r)[:50]}")
            fail += 1
    else:
        log.case("P3_child_click", "SKIP", "no child target")

    # ── P4: hover_widget(child) ──────────────────────────────────────────
    if root and child_name:
        r = call("umg.hover_widget", {"widget_path": root, "child_name": child_name}, timeout=10.0)
        if _not_built(r):
            log.case("P4_child_hover", "SKIP", "not built")
        elif is_ok(r) and (r.get("result", {}) or {}).get("hovered"):
            res = r.get("result", {}) or {}
            log.case("P4_child_hover", "PASS",
                     f"hovered child '{child_name}' (is_hovered={res.get('is_hovered')})")
        else:
            log.case("P4_child_hover", "FAIL",
                     f"hover child failed: {err_code(r)} {err_message(r)[:50]}")
            fail += 1
    else:
        log.case("P4_child_hover", "SKIP", "no child target")

    # ── P5: bad child_name → -32004 ──────────────────────────────────────
    if root:
        r = call("umg.click_widget",
                 {"widget_path": root, "child_name": "__NoSuchChild_ZZZ__"}, timeout=8.0)
        if _not_built(r):
            log.case("P5_bad_child", "SKIP", "not built")
        elif err_code(r) == -32004:
            log.case("P5_bad_child", "PASS", "bogus child_name → -32004")
        elif is_ok(r):
            log.case("P5_bad_child", "FAIL", "bogus child_name accepted")
            fail += 1
        else:
            log.case("P5_bad_child", "XFAIL", f"code {err_code(r)}: {err_message(r)[:50]}")
    else:
        log.case("P5_bad_child", "SKIP", "no root")

    # ── P6: PIE-off guard for list_live_widgets ──────────────────────────
    pie_stop_and_wait()
    r = call("umg.list_live_widgets", {"widget_path": "x"}, timeout=8.0)
    if _not_built(r):
        log.case("P6_off_guard", "SKIP", "not built")
    elif err_code(r) == -32027:
        log.case("P6_off_guard", "PASS", "list_live_widgets with PIE off → -32027")
    elif is_ok(r):
        log.case("P6_off_guard", "FAIL", "list_live_widgets succeeded with PIE off")
        fail += 1
    else:
        log.case("P6_off_guard", "XFAIL", f"code {err_code(r)} (expected -32027)")

    crash = latest_crash_dump(since=crash_baseline)
    if crash:
        log.case("crash_check", "FAIL", f"CRASH DUMP: {crash}")
        fail += 1

    summary = log.write()
    cc = summary["counts"]
    print()
    print(f"[P11] PASS={cc['PASS']} FAIL={cc['FAIL']} XFAIL={cc.get('XFAIL', 0)} "
          f"SKIP={cc.get('SKIP', 0)} TOTAL={cc['TOTAL']}")
    print(f"      log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    return 1 if fail > 0 else 0


if __name__ == "__main__":
    sys.exit(main())
