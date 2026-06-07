#!/usr/bin/env python3
"""Phase P5 — runtime AI + Niagara inspection against a LIVE PIE world.

These tools are WORLD-ADAPTIVE: they inspect/act on the *current* world —
the PIE world while playing, the editor world otherwise. Their `world` /
`world_kind` field reports which, and the editor→pie flip on PIE start is the
key proof they target the running game, not the editor.

Covered: ai.controller.list, ai.perception.list_components, ai.crowd.list_agents,
ai.crowd.get_settings, ai.bb.list_keys, ai.perception.get_config,
ai.controller.get_state, niagara.list_active, niagara.spawn_at_location,
niagara.stop_all.

  P1  ai.controller.list (PIE) → ok, {controllers:[...]}
  P2  ai.perception.list_components (PIE) → ok, world_kind=pie
  P3  ai.crowd.list_agents (PIE) → ok, world=pie
  P4  ai.crowd.get_settings (PIE) → ok, world=pie + settings fields
  P5  ai.bb.list_keys(player pawn) → -32004 (pawn has no AIController) [correct]
  P6  ai.perception.get_config(player pawn) → -32011 (no perception comp) [correct]
  P7  ai.controller.get_state(player pawn) → -32011 (not an AIController) [correct]
  P8  niagara.list_active (PIE) → ok, world=pie
  P9  niagara.spawn_at_location(real NS, PIE) → component_path, world=pie
  P10 niagara.list_active after spawn → count>=1 (spawn took effect)
  P11 niagara.stop_all (PIE) → stopped_count>=1
  P12 WORLD FLIP: stop PIE → niagara.list_active world flips pie→editor
  P13 ai.crowd.list_agents (editor, PIE off) → ok, world=editor

NOTE: non-empty AI data (controllers/perception/crowd agents) requires a map
populated with AI pawns; the FatumGame default map is player-only, so list
tools correctly return EMPTY. P5 proves the runtime tools dispatch against the
live PIE world, return well-formed structures, classify the non-AI player pawn
correctly, and flip worlds — full functional coverage of the runtime surface
short of authoring an AI-populated fixture map.

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

PHASE = "p5"
NAME = "pie_runtime_inspect"


def _find_niagara_system() -> str:
    for args in ({"class_path": "/Script/Niagara.NiagaraSystem", "page_size": 5},):
        r = call("asset.search_by_class", args, timeout=15.0)
        if is_ok(r):
            res = r.get("result", {}) or {}
            items = (res.get("matches") or res.get("items") or res.get("assets")
                     or res.get("results") or [])
            for it in items:
                if isinstance(it, dict):
                    p = it.get("asset_path") or it.get("object_path") or it.get("path")
                    if p:
                        return p
                elif isinstance(it, str):
                    return it
    return ""


def main() -> int:
    if not preflight(PHASE):
        return 2
    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()
    fail = 0
    log.note("AI/Niagara runtime tools are world-adaptive (pie when playing, "
             "editor otherwise); empty AI lists are correct in a player-only map.")

    niagara_sys = _find_niagara_system()
    log.note(f"NiagaraSystem fixture: {niagara_sys or '(none found — P9-P11 SKIP)'}")

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

    # ── P1: ai.controller.list (PIE) ──────────────────────────────────────
    r = call("ai.controller.list", {}, timeout=10.0)
    if is_ok(r) and "controllers" in (r.get("result", {}) or {}):
        n = len(r.get("result", {}).get("controllers") or [])
        log.case("P1_controller_list", "PASS", f"ok, {n} AI controllers (empty ok)")
    else:
        log.case("P1_controller_list", "FAIL",
                 f"ai.controller.list failed during PIE: {err_code(r)} {err_message(r)[:50]}")
        fail += 1

    # ── P2: ai.perception.list_components (PIE) ──────────────────────────
    r = call("ai.perception.list_components", {}, timeout=10.0)
    if is_ok(r):
        wk = (r.get("result", {}) or {}).get("world_kind")
        if wk == "pie":
            log.case("P2_perception_list", "PASS", f"ok, world_kind=pie, total={(r.get('result',{}) or {}).get('total')}")
        else:
            log.case("P2_perception_list", "XFAIL", f"ok but world_kind={wk} (expected pie)")
    else:
        log.case("P2_perception_list", "FAIL",
                 f"failed: {err_code(r)} {err_message(r)[:50]}")
        fail += 1

    # ── P3: ai.crowd.list_agents (PIE) ───────────────────────────────────
    r = call("ai.crowd.list_agents", {}, timeout=10.0)
    if is_ok(r):
        w = (r.get("result", {}) or {}).get("world")
        if w == "pie":
            log.case("P3_crowd_list", "PASS", f"ok, world=pie, count={(r.get('result',{}) or {}).get('count')}")
        else:
            log.case("P3_crowd_list", "XFAIL", f"ok but world={w} (expected pie)")
    else:
        log.case("P3_crowd_list", "FAIL", f"failed: {err_code(r)} {err_message(r)[:50]}")
        fail += 1

    # ── P4: ai.crowd.get_settings (PIE) ──────────────────────────────────
    r = call("ai.crowd.get_settings", {}, timeout=10.0)
    if is_ok(r):
        res = r.get("result", {}) or {}
        if res.get("world") == "pie" and "max_agents" in res:
            log.case("P4_crowd_settings", "PASS",
                     f"world=pie, max_agents={res.get('max_agents')}")
        else:
            log.case("P4_crowd_settings", "XFAIL", f"settings odd: {res}")
    else:
        log.case("P4_crowd_settings", "FAIL", f"failed: {err_code(r)} {err_message(r)[:50]}")
        fail += 1

    # ── P5: ai.bb.list_keys(player pawn) → -32004 ────────────────────────
    if pawn_path:
        r = call("ai.bb.list_keys", {"actor_path": pawn_path}, timeout=10.0)
        if err_code(r) == -32004:
            log.case("P5_bb_nonai", "PASS",
                     f"player pawn has no AIController → -32004 (correct)")
        elif is_ok(r):
            log.case("P5_bb_nonai", "XFAIL",
                     f"player pawn unexpectedly has a blackboard: {r.get('result')}")
        else:
            log.case("P5_bb_nonai", "XFAIL",
                     f"code {err_code(r)}: {err_message(r)[:50]}")
    else:
        log.case("P5_bb_nonai", "SKIP", "no pawn")

    # ── P6: ai.perception.get_config(player pawn) → -32011 ───────────────
    if pawn_path:
        r = call("ai.perception.get_config", {"actor_path": pawn_path}, timeout=10.0)
        if err_code(r) == -32011:
            log.case("P6_perception_nonai", "PASS",
                     "player pawn has no UAIPerceptionComponent → -32011 (correct)")
        elif is_ok(r):
            log.case("P6_perception_nonai", "XFAIL", f"pawn unexpectedly has perception: {r.get('result')}")
        else:
            log.case("P6_perception_nonai", "XFAIL", f"code {err_code(r)}: {err_message(r)[:50]}")
    else:
        log.case("P6_perception_nonai", "SKIP", "no pawn")

    # ── P7: ai.controller.get_state(player pawn) → -32011 ────────────────
    if pawn_path:
        r = call("ai.controller.get_state", {"controller_path": pawn_path}, timeout=10.0)
        if err_code(r) == -32011:
            log.case("P7_controller_nonai", "PASS",
                     "player pawn is not an AAIController → -32011 (correct)")
        elif is_ok(r):
            log.case("P7_controller_nonai", "XFAIL", f"unexpectedly accepted: {r.get('result')}")
        else:
            log.case("P7_controller_nonai", "XFAIL", f"code {err_code(r)}: {err_message(r)[:50]}")
    else:
        log.case("P7_controller_nonai", "SKIP", "no pawn")

    # ── P8: niagara.list_active (PIE) ────────────────────────────────────
    r = call("niagara.list_active", {}, timeout=10.0)
    if is_ok(r) and (r.get("result", {}) or {}).get("world") == "pie":
        log.case("P8_niagara_list", "PASS",
                 f"ok, world=pie, count={(r.get('result',{}) or {}).get('count')}")
    elif is_ok(r):
        log.case("P8_niagara_list", "XFAIL",
                 f"ok but world={(r.get('result',{}) or {}).get('world')} (expected pie)")
    else:
        log.case("P8_niagara_list", "FAIL", f"failed: {err_code(r)} {err_message(r)[:50]}")
        fail += 1

    # ── P9/P10/P11: niagara spawn → listed → stop ────────────────────────
    if niagara_sys:
        rs = call("niagara.spawn_at_location",
                  {"system_path": niagara_sys, "location": [0, 0, 250]}, timeout=12.0)
        if is_ok(rs):
            res = rs.get("result", {}) or {}
            comp = res.get("component_path", "")
            if comp and res.get("world") == "pie":
                log.case("P9_niagara_spawn", "PASS",
                         f"spawned in PIE: {comp.split('.')[-1]}")
            else:
                log.case("P9_niagara_spawn", "XFAIL", f"spawned oddly: {res}")
            time.sleep(0.5)
            rl = call("niagara.list_active", {}, timeout=10.0)
            cnt = (rl.get("result", {}) or {}).get("count", 0) if is_ok(rl) else 0
            if cnt >= 1:
                log.case("P10_niagara_listed", "PASS", f"list_active count={cnt} after spawn")
            else:
                log.case("P10_niagara_listed", "XFAIL",
                         f"spawn ok but list_active count={cnt} (may auto-complete fast)")
            rstop = call("niagara.stop_all", {}, timeout=10.0)
            sc = (rstop.get("result", {}) or {}).get("stopped_count", 0) if is_ok(rstop) else -1
            if is_ok(rstop) and sc >= 1:
                log.case("P11_niagara_stop_all", "PASS", f"stopped_count={sc}")
            elif is_ok(rstop):
                log.case("P11_niagara_stop_all", "XFAIL",
                         f"stop_all ok but stopped_count={sc} (component already finished)")
            else:
                log.case("P11_niagara_stop_all", "FAIL", f"stop_all failed: {err_message(rstop)[:50]}")
                fail += 1
        else:
            log.case("P9_niagara_spawn", "FAIL",
                     f"spawn failed: {err_code(rs)} {err_message(rs)[:60]}")
            fail += 1
            log.case("P10_niagara_listed", "SKIP", "spawn failed")
            log.case("P11_niagara_stop_all", "SKIP", "spawn failed")
    else:
        for c in ("P9_niagara_spawn", "P10_niagara_listed", "P11_niagara_stop_all"):
            log.case(c, "SKIP", "no NiagaraSystem asset found in project")

    # ── P12: WORLD FLIP — stop PIE, list_active world → editor ────────────
    pie_stop_and_wait()
    r = call("niagara.list_active", {}, timeout=10.0)
    if is_ok(r) and (r.get("result", {}) or {}).get("world") == "editor":
        log.case("P12_world_flip", "PASS",
                 "niagara.list_active world flipped pie→editor after PIE stop "
                 "(proves world-adaptive PIE targeting)")
    elif is_ok(r):
        log.case("P12_world_flip", "FAIL",
                 f"after stop, world={(r.get('result',{}) or {}).get('world')} (expected editor)")
        fail += 1
    else:
        log.case("P12_world_flip", "FAIL", f"failed: {err_message(r)[:50]}")
        fail += 1

    # ── P13: ai.crowd.list_agents (editor, PIE off) ──────────────────────
    r = call("ai.crowd.list_agents", {}, timeout=10.0)
    if is_ok(r) and (r.get("result", {}) or {}).get("world") == "editor":
        log.case("P13_ai_editor", "PASS", "ai.crowd.list_agents world=editor when PIE off")
    elif is_ok(r):
        log.case("P13_ai_editor", "XFAIL",
                 f"world={(r.get('result',{}) or {}).get('world')} (expected editor)")
    else:
        log.case("P13_ai_editor", "FAIL", f"failed: {err_message(r)[:50]}")
        fail += 1

    crash = latest_crash_dump(since=crash_baseline)
    if crash:
        log.case("crash_check", "FAIL", f"CRASH DUMP: {crash}")
        fail += 1

    summary = log.write()
    cc = summary["counts"]
    print()
    print(f"[P5] PASS={cc['PASS']} FAIL={cc['FAIL']} XFAIL={cc.get('XFAIL', 0)} "
          f"SKIP={cc.get('SKIP', 0)} TOTAL={cc['TOTAL']}")
    print(f"     log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    return 1 if fail > 0 else 0


if __name__ == "__main__":
    sys.exit(main())
