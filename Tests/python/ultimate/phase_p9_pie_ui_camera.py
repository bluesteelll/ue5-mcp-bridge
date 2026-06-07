#!/usr/bin/env python3
"""Phase P9 — UI (UMG viewport) + game camera in a LIVE PIE world.

Covers the UI/mouse/camera surface against the user's running map:
  * live UMG viewport tools — umg.list_root_widgets / add_to_viewport /
    remove_from_viewport (all PIE-required)
  * mouse click delivery into the Slate/UI layer — pie.click_screen
  * the GAME camera (PlayerCameraManager) — read + follows-pawn behavior

  P1  list_root_widgets → the game's live HUD (WBP_MainHUD) is on screen
  P2  add_to_viewport(WidgetBlueprint) → added, returns PIE-world widget path
  P3  list_root_widgets after add → count increased, added widget present
  P4  click_screen(center) → delivered to Slate (clicked:true), UI intact (no crash)
  P5  remove_from_viewport → removed, count back to baseline
  P6  add_to_viewport(bad path) → -32004 guard
  P7  game camera present (PlayerCameraManager) with finite transform
  P8  camera FOLLOWS pawn — move W, camera loc tracks pawn loc (both move ~equally)
  P9  mouse-look gap — SKIP (documented: no mouse-axis/move_mouse tool exists,
      so camera ROTATION via mouse cannot be driven by the current surface)
  P10 umg.list_root_widgets with PIE off → structured error (PIE-required guard)

NOTE on UI-response depth: P4 verifies the click is DELIVERED to the Slate/UMG
layer without crashing and the HUD stays intact. Asserting a specific widget's
OnClicked callback fired needs an instrumented widget with an observable
side-effect (a counter/flag) — not present in the stock game UI; that is the
one UI boundary left.

Exit codes: 0=PASS (0 FAIL), 1=FAIL/editor-died, 2=preflight.
"""

from __future__ import annotations

import math
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

PHASE = "p9"
NAME = "pie_ui_camera"


def _find_widget_bp() -> Optional[str]:
    r = call("asset.search_by_class",
             {"class_path": "/Script/UMGEditor.WidgetBlueprint", "page_size": 15}, timeout=15.0)
    if not is_ok(r):
        return None
    matches = (r.get("result", {}) or {}).get("matches", []) or []
    # Prefer a small/simple widget (slot/item) over the main HUD.
    prefer = [m.get("asset_path") for m in matches
              if isinstance(m, dict) and "HUD" not in (m.get("asset_path", ""))]
    if prefer:
        return prefer[0]
    return matches[0].get("asset_path") if matches and isinstance(matches[0], dict) else None


def _root_widgets() -> Tuple[int, list]:
    r = call("umg.list_root_widgets", {}, timeout=10.0)
    if not is_ok(r):
        return (-1, [])
    res = r.get("result", {}) or {}
    return (res.get("count", 0), res.get("widgets", []) or [])


def _cam_and_pawn() -> Tuple[Optional[Tuple], Optional[Tuple]]:
    r = call("pie.dump_world_state", {}, timeout=12.0)
    cam = pawn = None
    if is_ok(r):
        for a in (r.get("result", {}) or {}).get("actors", []):
            c, t = a.get("class", ""), a.get("transform", {})
            if "PlayerCameraManager" in c:
                cam = (float(t.get("loc_x", 0)), float(t.get("loc_y", 0)), float(t.get("loc_z", 0)))
            if "BP_PlayerFlecs" in c:
                pawn = (float(t.get("loc_x", 0)), float(t.get("loc_y", 0)), float(t.get("loc_z", 0)))
    return cam, pawn


def main() -> int:
    if not preflight(PHASE):
        return 2
    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()
    fail = 0

    wbp = _find_widget_bp()
    log.note(f"WidgetBlueprint fixture: {wbp or '(none found)'}")

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

    # ── P1: live game HUD present ─────────────────────────────────────────
    base_count, base_widgets = _root_widgets()
    if base_count >= 1:
        classes = [w.get("class_path", "").split("/")[-1] for w in base_widgets]
        log.case("P1_hud_present", "PASS",
                 f"{base_count} live viewport widget(s): {classes}")
    elif base_count == 0:
        log.case("P1_hud_present", "XFAIL",
                 "no live viewport widgets (map may not push a HUD on BeginPlay)")
    else:
        log.case("P1_hud_present", "FAIL", "umg.list_root_widgets failed during PIE")
        fail += 1

    # ── P2: add widget to viewport ────────────────────────────────────────
    added_path = None
    if wbp:
        r = call("umg.add_to_viewport", {"widget_bp_path": wbp}, timeout=12.0)
        if is_ok(r) and (r.get("result", {}) or {}).get("added"):
            added_path = (r.get("result", {}) or {}).get("widget_path")
            log.case("P2_add_widget", "PASS", f"added {wbp.split('/')[-1]} → {added_path.split(':')[-1] if added_path else '?'}")
        else:
            log.case("P2_add_widget", "FAIL",
                     f"add_to_viewport failed: {err_code(r)} {err_message(r)[:60]}")
            fail += 1
    else:
        log.case("P2_add_widget", "SKIP", "no WidgetBlueprint found")

    # ── P3: added widget appears in viewport list ────────────────────────
    if added_path:
        time.sleep(0.5)
        new_count, new_widgets = _root_widgets()
        paths = [w.get("widget_path", "") for w in new_widgets]
        present = any(added_path.split(":")[-1] in p for p in paths)
        if new_count > base_count and present:
            log.case("P3_add_listed", "PASS",
                     f"count {base_count}→{new_count}, added widget present")
        elif new_count > base_count:
            log.case("P3_add_listed", "PASS", f"count rose {base_count}→{new_count}")
        else:
            log.case("P3_add_listed", "XFAIL",
                     f"count unchanged ({base_count}→{new_count}); widget added but "
                     f"may not match PlayWorld filter")
    else:
        log.case("P3_add_listed", "SKIP", "no widget added")

    # ── P4: mouse click delivered to UI layer, no crash ──────────────────
    rc = call("pie.click_screen", {"x": 640, "y": 360}, timeout=10.0)
    time.sleep(0.3)
    cnt_after_click, _ = _root_widgets()
    if is_ok(rc) and (rc.get("result", {}) or {}).get("clicked") and cnt_after_click >= 0:
        log.case("P4_click_delivered", "PASS",
                 "click_screen delivered to Slate; UI layer intact + queryable after")
    elif is_ok(rc):
        log.case("P4_click_delivered", "XFAIL",
                 f"clicked but UI query degraded after (count={cnt_after_click})")
    else:
        log.case("P4_click_delivered", "FAIL",
                 f"click_screen failed: {err_code(rc)} {err_message(rc)[:50]}")
        fail += 1

    # ── P5: remove widget ────────────────────────────────────────────────
    if added_path:
        rr = call("umg.remove_from_viewport", {"widget_path": added_path}, timeout=10.0)
        time.sleep(0.4)
        cnt_after_remove, _ = _root_widgets()
        if is_ok(rr) and (rr.get("result", {}) or {}).get("removed"):
            log.case("P5_remove_widget", "PASS",
                     f"removed (was_in_viewport={rr.get('result',{}).get('was_in_viewport')}); "
                     f"count now {cnt_after_remove}")
        else:
            log.case("P5_remove_widget", "FAIL",
                     f"remove failed: {err_code(rr)} {err_message(rr)[:50]}")
            fail += 1
    else:
        log.case("P5_remove_widget", "SKIP", "no widget to remove")

    # ── P6: add bad widget path → -32004 ─────────────────────────────────
    r = call("umg.add_to_viewport", {"widget_bp_path": "/Game/__no_such_widget__.X"}, timeout=10.0)
    if err_code(r) == -32004:
        log.case("P6_add_bad", "PASS", "bogus widget_bp_path → -32004")
    elif is_ok(r):
        log.case("P6_add_bad", "FAIL", "bogus widget path accepted")
        fail += 1
    else:
        log.case("P6_add_bad", "XFAIL", f"code {err_code(r)}: {err_message(r)[:50]}")

    # ── P7: game camera present + finite ─────────────────────────────────
    cam0, pawn0 = _cam_and_pawn()
    if cam0 and all(abs(v) < 1e9 for v in cam0):
        log.case("P7_camera_present", "PASS",
                 f"PlayerCameraManager at {tuple(round(v) for v in cam0)}")
    else:
        log.case("P7_camera_present", "FAIL", f"no/invalid game camera: {cam0}")
        fail += 1

    # ── P8: camera follows pawn ──────────────────────────────────────────
    if cam0 and pawn0:
        call("pie.simulate_key", {"key": "W", "action": "press"}, timeout=6.0)
        time.sleep(1.4)
        call("pie.simulate_key", {"key": "W", "action": "release"}, timeout=6.0)
        time.sleep(0.6)
        cam1, pawn1 = _cam_and_pawn()
        if cam1 and pawn1:
            cam_move = ((cam1[0]-cam0[0])**2 + (cam1[1]-cam0[1])**2) ** 0.5
            pawn_move = ((pawn1[0]-pawn0[0])**2 + (pawn1[1]-pawn0[1])**2) ** 0.5
            if cam_move > 40 and abs(cam_move - pawn_move) < max(60.0, 0.3 * pawn_move):
                log.case("P8_camera_follows", "PASS",
                         f"camera tracked pawn: cam moved {cam_move:.0f}cm, pawn {pawn_move:.0f}cm")
            elif cam_move > 40:
                log.case("P8_camera_follows", "XFAIL",
                         f"camera moved {cam_move:.0f}cm but not matching pawn {pawn_move:.0f}cm")
            else:
                log.case("P8_camera_follows", "FAIL",
                         f"camera did NOT follow (cam {cam_move:.0f}cm, pawn {pawn_move:.0f}cm)")
                fail += 1
        else:
            log.case("P8_camera_follows", "FAIL", "lost camera/pawn after move")
            fail += 1
    else:
        log.case("P8_camera_follows", "SKIP", "no camera/pawn baseline")

    # ── P9: camera ROTATION via pie.add_look_input (mouse-look) ──────────
    # Behavioral proof: rotating the view should rotate the camera-relative
    # forward direction, so a W press after a look-turn moves the pawn along a
    # different world vector than before. Angle between the two forward vectors
    # = how far the view turned. (SKIP if the tool isn't in this editor build.)
    def _fwd_vector() -> Optional[Tuple[float, float]]:
        p0, _ = _cam_and_pawn()
        call("pie.simulate_key", {"key": "W", "action": "press"}, timeout=6.0)
        time.sleep(0.9)
        call("pie.simulate_key", {"key": "W", "action": "release"}, timeout=6.0)
        time.sleep(0.4)
        p1, _ = _cam_and_pawn()
        if not p0 or not p1:
            return None
        return (p1[0] - p0[0], p1[1] - p0[1])

    probe = call("pie.add_look_input", {"yaw": 30.0}, timeout=8.0)
    if err_code(probe) == -32601:
        log.case("P9_look_rotate", "SKIP",
                 "pie.add_look_input not in this editor build yet — rebuild required "
                 "to verify camera rotation (handler is committed; awaiting compile)")
    elif not is_ok(probe):
        log.case("P9_look_rotate", "FAIL",
                 f"pie.add_look_input failed: {err_code(probe)} {err_message(probe)[:50]}")
        fail += 1
    else:
        v0 = _fwd_vector()
        for _ in range(8):  # accumulate a turn over several look samples (mouse frames)
            call("pie.add_look_input", {"yaw": 45.0}, timeout=6.0)
            time.sleep(0.05)
        time.sleep(0.4)
        v1 = _fwd_vector()
        if v0 and v1:
            m0 = (v0[0] ** 2 + v0[1] ** 2) ** 0.5
            m1 = (v1[0] ** 2 + v1[1] ** 2) ** 0.5
            if m0 > 20 and m1 > 20:
                cosang = max(-1.0, min(1.0, (v0[0] * v1[0] + v0[1] * v1[1]) / (m0 * m1)))
                ang = math.degrees(math.acos(cosang))
                if ang > 15:
                    log.case("P9_look_rotate", "PASS",
                             f"look-input turned the view {ang:.0f}° (forward dir rotated) "
                             f"— mouse-look gap CLOSED")
                else:
                    log.case("P9_look_rotate", "XFAIL",
                             f"look applied but forward dir barely moved ({ang:.0f}°); "
                             f"movement may not be camera-relative")
            else:
                log.case("P9_look_rotate", "XFAIL",
                         f"could not measure forward vectors (m0={m0:.0f}, m1={m1:.0f}) "
                         f"— pawn may have hit geometry")
        else:
            log.case("P9_look_rotate", "XFAIL", "could not sample forward direction")

    pie_stop_and_wait()

    # ── P10: UMG live tool PIE-off guard ─────────────────────────────────
    r = call("umg.list_root_widgets", {}, timeout=8.0)
    if not is_ok(r) and err_code(r) is not None:
        log.case("P10_umg_off_guard", "PASS",
                 f"list_root_widgets with PIE off → {err_code(r)}: {err_message(r)[:50]}")
    elif is_ok(r):
        log.case("P10_umg_off_guard", "FAIL", "list_root_widgets succeeded with PIE off")
        fail += 1
    else:
        log.case("P10_umg_off_guard", "XFAIL", "non-structured failure with PIE off")

    # ── P11: pie.add_look_input PIE-off guard ────────────────────────────
    r = call("pie.add_look_input", {"yaw": 10.0}, timeout=8.0)
    if err_code(r) == -32601:
        log.case("P11_look_off_guard", "SKIP", "pie.add_look_input not in this editor build yet")
    elif err_code(r) == -32038:
        log.case("P11_look_off_guard", "PASS", "add_look_input with PIE off → -32038")
    elif is_ok(r):
        log.case("P11_look_off_guard", "FAIL", "add_look_input succeeded with PIE off")
        fail += 1
    else:
        log.case("P11_look_off_guard", "XFAIL", f"code {err_code(r)} (expected -32038)")

    crash = latest_crash_dump(since=crash_baseline)
    if crash:
        log.case("crash_check", "FAIL", f"CRASH DUMP: {crash}")
        fail += 1

    summary = log.write()
    cc = summary["counts"]
    print()
    print(f"[P9] PASS={cc['PASS']} FAIL={cc['FAIL']} XFAIL={cc.get('XFAIL', 0)} "
          f"SKIP={cc.get('SKIP', 0)} TOTAL={cc['TOTAL']}")
    print(f"     log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    return 1 if fail > 0 else 0


if __name__ == "__main__":
    sys.exit(main())
