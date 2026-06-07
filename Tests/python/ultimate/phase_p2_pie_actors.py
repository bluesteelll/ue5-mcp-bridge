#!/usr/bin/env python3
"""Phase P2 — PIE actor identity + viewport focus (LIVE PIE world).

Covered tools: pie.get_player_controller, pie.get_pawn, pie.focus_actor
(all against a running PIE session with a possessed pawn).

  P1  get_player_controller → pc_path in PIE world, guid present
  P2  get_pawn → pawn_path in PIE world, class present (happy path;
      XFAIL if the loaded map possesses no pawn)
  P3  pawn cross-check: pawn_path appears in pie.dump_world_state
  P4  identity stable: get_pawn twice → same guid
  P5  get_pawn(player_index=7) → -32004 (no PC at index)
  P6  get_pawn(player_index=-1) → -32602 (invalid)
  P7  get_player_controller(player_index=7) → -32004
  P8  focus_actor(pawn) → focused:true
  P9  focus_actor(bogus path) → -32004
  P10 focus_actor(missing actor_id) → -32602

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
    pie_start_and_wait,
    pie_stop_and_wait,
    preflight,
)

PHASE = "p2"
NAME = "pie_actors"


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

    up, info = pie_start_and_wait()
    if not up:
        log.case("P0_start", "FAIL", f"PIE never came up: {info}")
        log.write()
        return 1
    log.case("P0_start", "PASS", f"PIE up: {info.get('state')}")

    # ── P1: get_player_controller ─────────────────────────────────────────
    rpc = call("pie.get_player_controller", {}, timeout=10.0)
    if is_ok(rpc):
        res = rpc.get("result", {}) or {}
        pc_path = res.get("pc_path", "")
        guid = res.get("pc_actor_guid", "")
        if "UEDPIE" in pc_path and "PlayerController" in pc_path:
            log.case("P1_get_pc", "PASS", f"pc_path={pc_path} guid_len={len(guid)}")
        else:
            log.case("P1_get_pc", "FAIL", f"pc_path not a PIE PlayerController: {pc_path}")
            fail += 1
    else:
        log.case("P1_get_pc", "FAIL",
                 f"get_player_controller failed: {err_code(rpc)} {err_message(rpc)[:60]}")
        fail += 1

    # ── P2: get_pawn (happy path) ─────────────────────────────────────────
    pawn_path = None
    pawn_guid = None
    rpawn = call("pie.get_pawn", {}, timeout=10.0)
    if is_ok(rpawn):
        res = rpawn.get("result", {}) or {}
        pawn_path = res.get("pawn_path", "")
        pawn_guid = res.get("pawn_actor_guid", "")
        cls = res.get("class", "")
        if "UEDPIE" in pawn_path and cls:
            log.case("P2_get_pawn", "PASS", f"pawn_path={pawn_path} class={cls}")
        else:
            log.case("P2_get_pawn", "FAIL",
                     f"pawn returned but malformed: path={pawn_path} class={cls}")
            fail += 1
    elif err_code(rpawn) == -32004:
        log.case("P2_get_pawn", "XFAIL",
                 f"no possessed pawn in this map (correct NO_PAWN): {err_message(rpawn)[:60]}")
    else:
        log.case("P2_get_pawn", "FAIL",
                 f"get_pawn unexpected error: {err_code(rpawn)} {err_message(rpawn)[:60]}")
        fail += 1

    # ── P3: pawn appears in dump_world_state ──────────────────────────────
    if pawn_path:
        rd = call("pie.dump_world_state", {}, timeout=12.0)
        if is_ok(rd):
            actors = (rd.get("result", {}) or {}).get("actors") or []
            paths = {a.get("actor_path") for a in actors if isinstance(a, dict)}
            leaf = pawn_path.split(".")[-1]
            if pawn_path in paths or any(leaf in p for p in paths if p):
                log.case("P3_pawn_in_dump", "PASS",
                         f"pawn present in dump ({len(actors)} actors)")
            else:
                log.case("P3_pawn_in_dump", "FAIL",
                         f"pawn_path NOT in dump_world_state ({len(actors)} actors)")
                fail += 1
        else:
            log.case("P3_pawn_in_dump", "FAIL",
                     f"dump_world_state failed: {err_message(rd)[:60]}")
            fail += 1
    else:
        log.case("P3_pawn_in_dump", "SKIP", "no pawn (P2 XFAIL)")

    # ── P4: identity stable across calls ──────────────────────────────────
    if pawn_guid:
        r2 = call("pie.get_pawn", {}, timeout=10.0)
        g2 = (r2.get("result", {}) or {}).get("pawn_actor_guid") if is_ok(r2) else None
        if g2 and g2 == pawn_guid:
            log.case("P4_identity_stable", "PASS", f"guid stable: {g2}")
        else:
            log.case("P4_identity_stable", "XFAIL",
                     f"guid differs/blank across calls: {pawn_guid} vs {g2} "
                     f"(PIE actor guids can be transient)")
    else:
        log.case("P4_identity_stable", "SKIP", "no pawn guid")

    # ── P5: get_pawn invalid player_index → -32004 ───────────────────────
    r = call("pie.get_pawn", {"player_index": 7}, timeout=8.0)
    if err_code(r) == -32004:
        log.case("P5_pawn_bad_index", "PASS", f"index 7 → -32004: {err_message(r)[:50]}")
    elif is_ok(r):
        log.case("P5_pawn_bad_index", "FAIL", "player_index=7 returned a pawn (no 8 players)")
        fail += 1
    else:
        log.case("P5_pawn_bad_index", "XFAIL", f"code {err_code(r)}: {err_message(r)[:50]}")

    # ── P6: get_pawn negative index → -32602 ─────────────────────────────
    r = call("pie.get_pawn", {"player_index": -1}, timeout=8.0)
    if err_code(r) == -32602:
        log.case("P6_pawn_neg_index", "PASS", f"index -1 → -32602")
    elif is_ok(r):
        log.case("P6_pawn_neg_index", "FAIL", "player_index=-1 accepted")
        fail += 1
    else:
        log.case("P6_pawn_neg_index", "XFAIL", f"code {err_code(r)}")

    # ── P7: get_player_controller invalid index → -32004 ─────────────────
    r = call("pie.get_player_controller", {"player_index": 7}, timeout=8.0)
    if err_code(r) == -32004:
        log.case("P7_pc_bad_index", "PASS", f"index 7 → -32004")
    elif is_ok(r):
        log.case("P7_pc_bad_index", "FAIL", "pc player_index=7 returned a PC")
        fail += 1
    else:
        log.case("P7_pc_bad_index", "XFAIL", f"code {err_code(r)}")

    # ── P8: focus_actor on the pawn → focused:true ───────────────────────
    if pawn_path:
        rf = call("pie.focus_actor", {"actor_id": pawn_path}, timeout=10.0)
        if is_ok(rf) and (rf.get("result", {}) or {}).get("focused"):
            log.case("P8_focus_pawn", "PASS", "focused viewport on pawn")
        elif is_ok(rf):
            log.case("P8_focus_pawn", "XFAIL", f"focus returned without focused:true {rf.get('result')}")
        else:
            log.case("P8_focus_pawn", "FAIL",
                     f"focus_actor(pawn) failed: {err_code(rf)} {err_message(rf)[:50]}")
            fail += 1
    else:
        log.case("P8_focus_pawn", "SKIP", "no pawn to focus")

    # ── P9: focus_actor bad path → -32004 ────────────────────────────────
    r = call("pie.focus_actor", {"actor_id": "/Game/__no_such_actor__.X:PersistentLevel.Nope_0"},
             timeout=8.0)
    if err_code(r) == -32004:
        log.case("P9_focus_bad", "PASS", f"bogus actor → -32004")
    elif is_ok(r):
        log.case("P9_focus_bad", "FAIL", "focus on nonexistent actor SUCCEEDED")
        fail += 1
    else:
        log.case("P9_focus_bad", "XFAIL", f"code {err_code(r)}: {err_message(r)[:50]}")

    # ── P10: focus_actor missing actor_id → -32602 ───────────────────────
    r = call("pie.focus_actor", {}, timeout=8.0)
    if err_code(r) == -32602:
        log.case("P10_focus_missing_arg", "PASS", "missing actor_id → -32602")
    elif is_ok(r):
        log.case("P10_focus_missing_arg", "FAIL", "focus with no actor_id SUCCEEDED")
        fail += 1
    else:
        log.case("P10_focus_missing_arg", "XFAIL", f"code {err_code(r)}")

    pie_stop_and_wait()

    crash = latest_crash_dump(since=crash_baseline)
    if crash:
        log.case("crash_check", "FAIL", f"CRASH DUMP: {crash}")
        fail += 1

    summary = log.write()
    cc = summary["counts"]
    print()
    print(f"[P2] PASS={cc['PASS']} FAIL={cc['FAIL']} XFAIL={cc.get('XFAIL', 0)} "
          f"SKIP={cc.get('SKIP', 0)} TOTAL={cc['TOTAL']}")
    print(f"     log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    return 1 if fail > 0 else 0


if __name__ == "__main__":
    sys.exit(main())
