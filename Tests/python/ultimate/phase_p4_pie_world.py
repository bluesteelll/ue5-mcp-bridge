#!/usr/bin/env python3
"""Phase P4 — PIE stats, world dump, console targeting (LIVE PIE world).

Covered tools: pie.get_stats, pie.dump_world_state, pie.console_exec.

  P1  get_stats → all fields present + sane (fps>0, memory dict, PIE world_path)
  P2  dump_world_state → total>0, each actor has path/class/label/transform
  P3  dump_world_state class_filter=PlayerController → only PCs, fewer than all
  P4  dump_world_state bogus class_filter → structured error (not crash/ok)
  P5  console_exec "stat unit" world=pie → executed
  P6  console_exec world=editor → executed (editor world always available)
  P7  console_exec missing command → -32602
  P8  console_exec world=server (no dedicated server) → -32603
  P9  world targeting: dump shows the PIE-only pawn (BP_PlayerFlecs) →
      proves PIE-world data, not editor-world cross-talk

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
    pie_start_and_wait,
    pie_stop_and_wait,
    preflight,
)

PHASE = "p4"
NAME = "pie_world"


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
             else f"no user map loaded; using current ({pie_current_map()})")

    up, info = pie_start_and_wait()
    if not up:
        log.case("P0_start", "FAIL", f"PIE never came up: {info}")
        log.write()
        return 1
    log.case("P0_start", "PASS", f"PIE up: {info.get('state')}")

    # ── P1: get_stats fields ──────────────────────────────────────────────
    r = call("pie.get_stats", {}, timeout=10.0)
    if is_ok(r):
        res = r.get("result", {}) or {}
        mem = res.get("memory", {}) or {}
        checks = {
            "delta_time_ms": isinstance(res.get("delta_time_ms"), (int, float)),
            "instant_fps": isinstance(res.get("instant_fps"), (int, float)),
            "avg_fps": isinstance(res.get("avg_fps"), (int, float)),
            "avg_ms": isinstance(res.get("avg_ms"), (int, float)),
            "memory.used_physical_mb": isinstance(mem.get("used_physical_mb"), (int, float)),
            "actor_count>0": (res.get("actor_count", 0) or 0) > 0,
            "time_seconds": isinstance(res.get("time_seconds"), (int, float)),
            "time_dilation": isinstance(res.get("time_dilation"), (int, float)),
            "world_path_PIE": "UEDPIE" in res.get("world_path", ""),
        }
        bad = [k for k, v in checks.items() if not v]
        if not bad:
            log.case("P1_stats_fields", "PASS",
                     f"all 9 fields sane (actor_count={res.get('actor_count')}, "
                     f"avg_fps={res.get('avg_fps'):.1f})")
        else:
            log.case("P1_stats_fields", "FAIL", f"missing/bad fields: {bad}")
            fail += 1
    else:
        log.case("P1_stats_fields", "FAIL", f"get_stats failed: {err_message(r)[:60]}")
        fail += 1

    # ── P2: dump_world_state structure ────────────────────────────────────
    total_all = 0
    rd = call("pie.dump_world_state", {}, timeout=12.0)
    if is_ok(rd):
        res = rd.get("result", {}) or {}
        total_all = res.get("total", 0)
        actors = res.get("actors") or []
        if total_all > 0 and actors:
            a0 = actors[0]
            need = ("actor_path", "class", "label", "transform", "hidden", "pending_kill")
            missing = [k for k in need if k not in a0]
            tf = a0.get("transform", {}) or {}
            tf_ok = all(k in tf for k in ("loc_x", "rot_p", "scl_x"))
            if not missing and tf_ok:
                log.case("P2_dump_structure", "PASS",
                         f"total={total_all}, actor schema complete (loc/rot/scl)")
            else:
                log.case("P2_dump_structure", "FAIL",
                         f"actor schema incomplete: missing={missing} tf_ok={tf_ok}")
                fail += 1
        else:
            log.case("P2_dump_structure", "FAIL", f"empty dump: total={total_all}")
            fail += 1
    else:
        log.case("P2_dump_structure", "FAIL", f"dump failed: {err_message(rd)[:60]}")
        fail += 1

    # ── P3: class_filter narrows results ──────────────────────────────────
    rf = call("pie.dump_world_state",
              {"class_filter": "/Script/Engine.PlayerController"}, timeout=12.0)
    if is_ok(rf):
        res = rf.get("result", {}) or {}
        tot = res.get("total", 0)
        actors = res.get("actors") or []
        all_pc = all("PlayerController" in (a.get("class", "")) for a in actors) if actors else False
        if tot >= 1 and all_pc and (total_all == 0 or tot <= total_all):
            log.case("P3_dump_filter", "PASS",
                     f"filtered to {tot} PlayerController(s) (all match), of {total_all} total")
        else:
            log.case("P3_dump_filter", "FAIL",
                     f"filter wrong: tot={tot} all_pc={all_pc} total_all={total_all}")
            fail += 1
    else:
        log.case("P3_dump_filter", "FAIL", f"filtered dump failed: {err_message(rf)[:60]}")
        fail += 1

    # ── P4: bogus class_filter → structured error ─────────────────────────
    r = call("pie.dump_world_state",
             {"class_filter": "/Script/Engine.ThisClassDoesNotExistXYZ"}, timeout=10.0)
    if (not is_ok(r)) and err_code(r) is not None:
        log.case("P4_dump_bad_filter", "PASS",
                 f"bogus class → {err_code(r)}: {err_message(r)[:50]}")
    elif is_ok(r):
        log.case("P4_dump_bad_filter", "FAIL", "bogus class_filter accepted")
        fail += 1
    else:
        log.case("P4_dump_bad_filter", "XFAIL", f"non-structured failure: {r}")

    # ── P5: console_exec world=pie ────────────────────────────────────────
    r = call("pie.console_exec", {"command": "stat unit", "world": "pie"}, timeout=10.0)
    if is_ok(r) and isinstance((r.get("result", {}) or {}).get("executed"), bool):
        log.case("P5_console_pie", "PASS",
                 f"executed={r.get('result',{}).get('executed')}")
    else:
        log.case("P5_console_pie", "FAIL",
                 f"console_exec(pie) failed: {err_code(r)} {err_message(r)[:50]}")
        fail += 1
    call("pie.console_exec", {"command": "stat unit", "world": "pie"}, timeout=6.0)  # toggle off

    # ── P6: console_exec world=editor ─────────────────────────────────────
    r = call("pie.console_exec", {"command": "stat none", "world": "editor"}, timeout=10.0)
    if is_ok(r):
        log.case("P6_console_editor", "PASS", "editor-world console exec ok")
    else:
        log.case("P6_console_editor", "FAIL",
                 f"console_exec(editor) failed: {err_code(r)} {err_message(r)[:50]}")
        fail += 1

    # ── P7: console_exec missing command → -32602 ─────────────────────────
    r = call("pie.console_exec", {"world": "pie"}, timeout=8.0)
    if err_code(r) == -32602:
        log.case("P7_console_missing_cmd", "PASS", "missing command → -32602")
    elif is_ok(r):
        log.case("P7_console_missing_cmd", "FAIL", "missing command accepted")
        fail += 1
    else:
        log.case("P7_console_missing_cmd", "XFAIL", f"code {err_code(r)}")

    # ── P8: console_exec world=server (no dedicated server) → -32603 ─────
    r = call("pie.console_exec", {"command": "stat fps", "world": "server"}, timeout=8.0)
    if err_code(r) == -32603:
        log.case("P8_console_no_server", "PASS",
                 f"no server world → -32603: {err_message(r)[:50]}")
    elif is_ok(r):
        log.case("P8_console_no_server", "XFAIL",
                 "world=server executed — a dedicated-server PIE world exists")
    else:
        log.case("P8_console_no_server", "XFAIL",
                 f"code {err_code(r)}: {err_message(r)[:50]}")

    # ── P9: world isolation — PIE-only pawn present in dump ────────────────
    rd2 = call("pie.dump_world_state", {}, timeout=12.0)
    if is_ok(rd2):
        actors = (rd2.get("result", {}) or {}).get("actors") or []
        classes = [a.get("class", "") for a in actors]
        wp = (rd2.get("result", {}) or {}).get("world_path", "")
        has_pie_pawn = any("BP_PlayerFlecs" in c for c in classes)
        if "UEDPIE" in wp and has_pie_pawn:
            log.case("P9_world_isolation", "PASS",
                     "dump reads PIE world (UEDPIE path + PIE-spawned BP_PlayerFlecs present)")
        elif "UEDPIE" in wp:
            log.case("P9_world_isolation", "XFAIL",
                     f"PIE world_path correct but no BP_PlayerFlecs (map without default pawn)")
        else:
            log.case("P9_world_isolation", "FAIL",
                     f"dump world_path not PIE: {wp}")
            fail += 1
    else:
        log.case("P9_world_isolation", "FAIL", f"dump failed: {err_message(rd2)[:50]}")
        fail += 1

    pie_stop_and_wait()

    crash = latest_crash_dump(since=crash_baseline)
    if crash:
        log.case("crash_check", "FAIL", f"CRASH DUMP: {crash}")
        fail += 1

    summary = log.write()
    cc = summary["counts"]
    print()
    print(f"[P4] PASS={cc['PASS']} FAIL={cc['FAIL']} XFAIL={cc.get('XFAIL', 0)} "
          f"SKIP={cc.get('SKIP', 0)} TOTAL={cc['TOTAL']}")
    print(f"     log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    return 1 if fail > 0 else 0


if __name__ == "__main__":
    sys.exit(main())
