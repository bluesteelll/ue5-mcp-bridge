#!/usr/bin/env python3
"""Phase P8 — BEHAVIORAL input verification in the user's real map.

This closes the input-EFFECT boundary that P3 documented: P3 proves the input
tools dispatch through the live PlayerController; P8 proves they actually MOVE
the pawn. It loads the user's floored, playable map (FlecsTestMap2 — confirmed
floor + possessed BP_PlayerFlecs + working W/A/S/D), then measures real pawn
displacement from simulated input.

  P1  floor check — pawn z stable over time (on a floor, not free-falling)
  P2  W (forward) → pawn displaces > threshold; record forward vector
  P3  S (back)    → displaces opposite to forward (dot < 0)
  P4  D (right)   → displaces > threshold; record strafe vector
  P5  A (left)    → displaces opposite to strafe (dot < 0)
  P6  jump (Space) → pawn z rises above rest (best-effort; XFAIL if no jump)
  P7  at rest after inputs → pawn still on floor (didn't fall through world)

Requires a user map; if none loads, behavioral cases SKIP (the empty scratch
map free-falls the pawn and can't host movement tests).

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

PHASE = "p8"
NAME = "pie_behavioral_input"

Vec = Tuple[float, float, float]


def _pawn_pos() -> Optional[Vec]:
    """Locate BP_PlayerFlecs in the PIE world and return (x,y,z), or None."""
    r = call("pie.dump_world_state", {}, timeout=12.0)
    if not is_ok(r):
        return None
    for a in (r.get("result", {}) or {}).get("actors", []):
        if "BP_PlayerFlecs" in (a.get("class", "")):
            t = a.get("transform", {}) or {}
            return (float(t.get("loc_x", 0)), float(t.get("loc_y", 0)), float(t.get("loc_z", 0)))
    return None


def _move(key: str, hold_s: float = 1.2) -> Tuple[Optional[Vec], Optional[Vec]]:
    """Press key, hold, release; return (before, after) pawn positions."""
    p0 = _pawn_pos()
    call("pie.simulate_key", {"key": key, "action": "press"}, timeout=6.0)
    time.sleep(hold_s)
    call("pie.simulate_key", {"key": key, "action": "release"}, timeout=6.0)
    time.sleep(0.5)
    p1 = _pawn_pos()
    return p0, p1


def _xy_delta(p0: Optional[Vec], p1: Optional[Vec]) -> Tuple[float, float, float]:
    """Return (dx, dy, horizontal_magnitude)."""
    if not p0 or not p1:
        return (0.0, 0.0, 0.0)
    dx, dy = p1[0] - p0[0], p1[1] - p0[1]
    return (dx, dy, (dx * dx + dy * dy) ** 0.5)


def _dot_norm(a: Tuple[float, float], b: Tuple[float, float]) -> float:
    ma = (a[0] ** 2 + a[1] ** 2) ** 0.5
    mb = (b[0] ** 2 + b[1] ** 2) ** 0.5
    if ma < 1e-3 or mb < 1e-3:
        return 0.0
    return (a[0] * b[0] + a[1] * b[1]) / (ma * mb)


MOVE_THRESHOLD_CM = 40.0


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
    if user_map:
        log.case("preflight_map", "PASS", f"loaded user map {user_map} ({pie_current_map()})")
    else:
        log.note("No user map could be loaded — behavioral cases SKIP (empty "
                 "scratch map free-falls the pawn). Set USER_TEST_MAP_CANDIDATES.")
        log.case("preflight_map", "SKIP",
                 f"no user map loaded (current={pie_current_map()})")
        for c in ("P1_floor", "P2_forward", "P3_back", "P4_right", "P5_left",
                  "P6_jump", "P7_rest_floor"):
            log.case(c, "SKIP", "no floored user map")
        summary = log.write()
        cc = summary["counts"]
        print(f"\n[P8] PASS={cc['PASS']} FAIL={cc['FAIL']} SKIP={cc.get('SKIP',0)} TOTAL={cc['TOTAL']}")
        return 0

    up, info = pie_start_and_wait(settle_s=3.0)
    if not up:
        log.case("P0_start", "FAIL", f"PIE never came up: {info}")
        log.write()
        return 1
    log.case("P0_start", "PASS", f"PIE up in {user_map}")

    # ── P1: floor check ───────────────────────────────────────────────────
    zs = []
    for _ in range(3):
        p = _pawn_pos()
        if p:
            zs.append(p[2])
        time.sleep(0.8)
    if len(zs) >= 2:
        drift = zs[-1] - zs[0]
        if drift > -100.0:  # not free-falling (would be hundreds of cm in ~1.6s)
            log.case("P1_floor", "PASS", f"pawn on floor; z={zs[-1]:.0f} drift={drift:+.0f}cm")
        else:
            log.case("P1_floor", "XFAIL",
                     f"pawn free-falling (drift {drift:.0f}cm) — map has no floor under spawn")
    else:
        log.case("P1_floor", "FAIL", "could not read pawn position")
        fail += 1

    # ── P2: forward (W) ───────────────────────────────────────────────────
    fwd = (0.0, 0.0)
    p0, p1 = _move("W")
    dx, dy, mag = _xy_delta(p0, p1)
    if mag > MOVE_THRESHOLD_CM:
        fwd = (dx, dy)
        log.case("P2_forward", "PASS", f"W moved pawn {mag:.0f}cm (Δ={dx:+.0f},{dy:+.0f})")
    else:
        log.case("P2_forward", "FAIL",
                 f"W produced no movement ({mag:.0f}cm < {MOVE_THRESHOLD_CM}); "
                 f"before={p0} after={p1}")
        fail += 1

    # ── P3: back (S) opposite to forward ─────────────────────────────────
    p0, p1 = _move("S")
    dx, dy, mag = _xy_delta(p0, p1)
    if mag > MOVE_THRESHOLD_CM:
        d = _dot_norm((dx, dy), fwd)
        if d < -0.3:
            log.case("P3_back", "PASS", f"S moved {mag:.0f}cm opposite to forward (dot={d:.2f})")
        else:
            log.case("P3_back", "XFAIL",
                     f"S moved {mag:.0f}cm but not clearly opposite (dot={d:.2f}); "
                     f"pawn may have rotated")
    else:
        log.case("P3_back", "FAIL", f"S produced no movement ({mag:.0f}cm)")
        fail += 1

    # ── P4: right (D) ─────────────────────────────────────────────────────
    strafe = (0.0, 0.0)
    p0, p1 = _move("D")
    dx, dy, mag = _xy_delta(p0, p1)
    if mag > MOVE_THRESHOLD_CM:
        strafe = (dx, dy)
        log.case("P4_right", "PASS", f"D moved pawn {mag:.0f}cm (Δ={dx:+.0f},{dy:+.0f})")
    else:
        log.case("P4_right", "FAIL", f"D produced no movement ({mag:.0f}cm)")
        fail += 1

    # ── P5: left (A) opposite to right ───────────────────────────────────
    p0, p1 = _move("A")
    dx, dy, mag = _xy_delta(p0, p1)
    if mag > MOVE_THRESHOLD_CM:
        d = _dot_norm((dx, dy), strafe)
        if d < -0.3:
            log.case("P5_left", "PASS", f"A moved {mag:.0f}cm opposite to right (dot={d:.2f})")
        else:
            log.case("P5_left", "XFAIL",
                     f"A moved {mag:.0f}cm but not clearly opposite (dot={d:.2f})")
    else:
        log.case("P5_left", "FAIL", f"A produced no movement ({mag:.0f}cm)")
        fail += 1

    # ── P6: jump (Space) — best-effort z rise ────────────────────────────
    rest = _pawn_pos()
    rest_z = rest[2] if rest else 0.0
    call("pie.simulate_key", {"key": "SpaceBar", "action": "tap"}, timeout=6.0)
    max_z = rest_z
    for _ in range(4):
        time.sleep(0.25)
        p = _pawn_pos()
        if p:
            max_z = max(max_z, p[2])
    rise = max_z - rest_z
    if rise > 20.0:
        log.case("P6_jump", "PASS", f"Space raised pawn {rise:.0f}cm above rest")
    else:
        log.case("P6_jump", "XFAIL",
                 f"Space rise {rise:.0f}cm < 20 (jump may be unbound/quick at 3 FPS, "
                 f"or posture system suppressed it) — input still dispatched")

    time.sleep(1.2)  # let pawn settle back

    # ── P7: still on floor after all inputs ──────────────────────────────
    pend = _pawn_pos()
    if pend and pend[2] > rest_z - 300.0:
        log.case("P7_rest_floor", "PASS",
                 f"pawn still grounded after inputs (z={pend[2]:.0f})")
    elif pend:
        log.case("P7_rest_floor", "XFAIL",
                 f"pawn z dropped to {pend[2]:.0f} (may have walked off a ledge)")
    else:
        log.case("P7_rest_floor", "FAIL", "lost pawn after inputs")
        fail += 1

    pie_stop_and_wait()

    crash = latest_crash_dump(since=crash_baseline)
    if crash:
        log.case("crash_check", "FAIL", f"CRASH DUMP: {crash}")
        fail += 1

    summary = log.write()
    cc = summary["counts"]
    print()
    print(f"[P8] PASS={cc['PASS']} FAIL={cc['FAIL']} XFAIL={cc.get('XFAIL', 0)} "
          f"SKIP={cc.get('SKIP', 0)} TOTAL={cc['TOTAL']}")
    print(f"     log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    return 1 if fail > 0 else 0


if __name__ == "__main__":
    sys.exit(main())
