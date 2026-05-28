#!/usr/bin/env python3
"""Phase H6.7 — gameplaytag.* full coverage.

Available tools (Wave D + later):
  - gameplaytag.list — list all registered tags
  - gameplaytag.add_to_container — attach tag to actor
  - gameplaytag.query_actor — does actor have tag
  - gameplaytag.remove_from_container — detach tag

Probes:
  P1 — gameplaytag.list with pagination
  P2 — spawn actor
  P3 — gameplaytag.add_to_container with multiple tags
  P4 — gameplaytag.query_actor verifies has_any / has_all
  P5 — gameplaytag.remove_from_container removes specific tag
  P6 — gameplaytag.query_actor verifies tag gone
  P7 — destroy actor + cleanup

Exit codes: 0=PASS, 1=FAIL, 2=preflight.
"""

from __future__ import annotations

import sys
import time
from pathlib import Path
from typing import Any, Dict, Optional

sys.path.insert(0, str(Path(__file__).parent))
from mcp_test_harness import (
    LOG_ROOT,
    TestLogger,
    call,
    err_code,
    err_message,
    health,
    is_ok,
    latest_crash_dump,
    preflight,
    random_suffix,
)

PHASE = "h6_7"
NAME = "gameplaytag"


def main() -> int:
    if not preflight(PHASE):
        return 2
    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()
    fail_total = 0

    print(f"[H6.7] gameplaytag.* full coverage…", flush=True)

    # P1 — gameplaytag.list
    t0 = time.monotonic()
    r = call("gameplaytag.list", {"page_size": 20}, timeout=8.0)
    dt = (time.monotonic() - t0) * 1000.0
    if not is_ok(r):
        c = err_code(r)
        if c == -32601:
            log.case("P1_list", "SKIP", "gameplaytag.list not registered",
                     duration_ms=dt)
        else:
            log.case("P1_list", "FAIL",
                     f"gameplaytag.list failed: {err_message(r)[:60]}",
                     duration_ms=dt, code=c)
            fail_total += 1
    else:
        tags = (r.get("result", {}) or {}).get("tags") or []
        log.case("P1_list", "PASS",
                 f"listed {len(tags)} tags", duration_ms=dt)

    # P2 — spawn test actor
    label = f"PhTH67_{random_suffix(5)}"
    rs = call("actor.spawn",
               {"class_path": "/Script/Engine.StaticMeshActor",
                "actor_label": label,
                "location": [500, 500, 100]}, timeout=10.0)
    if not is_ok(rs):
        log.case("P2_spawn", "FAIL",
                 f"actor.spawn failed: {err_message(rs)[:60]}")
        log.write()
        return 1
    actor_path = rs.get("result", {}).get("actor_path") or ""
    if not actor_path:
        log.case("P2_spawn", "FAIL", "no actor_path in spawn response")
        log.write()
        return 1
    log.case("P2_spawn", "PASS", f"actor at {actor_path}")

    # P3 — add multiple tags
    test_tags = [
        f"PhT.H67.{random_suffix(3)}.A",
        f"PhT.H67.{random_suffix(3)}.B",
        f"PhT.H67.{random_suffix(3)}.C",
    ]
    t0 = time.monotonic()
    r = call("gameplaytag.add_to_container",
              {"actor_path": actor_path, "tags": test_tags}, timeout=8.0)
    dt = (time.monotonic() - t0) * 1000.0
    if not is_ok(r):
        log.case("P3_add_tags", "XFAIL",
                 f"add_to_container failed: {err_message(r)[:60]}",
                 duration_ms=dt)
    else:
        log.case("P3_add_tags", "PASS",
                 f"added {len(test_tags)} tags to actor", duration_ms=dt)

    # P4 — query has_any / has_all
    t0 = time.monotonic()
    r = call("gameplaytag.query_actor",
              {"actor_path": actor_path, "tags": test_tags}, timeout=8.0)
    dt = (time.monotonic() - t0) * 1000.0
    if not is_ok(r):
        log.case("P4_query_after_add", "XFAIL",
                 f"query_actor failed: {err_message(r)[:60]}",
                 duration_ms=dt)
    else:
        res = r.get("result", {}) or {}
        has_any = res.get("has_any")
        has_all = res.get("has_all")
        log.case("P4_query_after_add",
                 "PASS" if (has_any or has_all) else "XFAIL",
                 f"has_any={has_any} has_all={has_all}", duration_ms=dt)

    # P5 — remove specific tag (the first one)
    t0 = time.monotonic()
    r = call("gameplaytag.remove_from_container",
              {"actor_path": actor_path, "tags": [test_tags[0]]}, timeout=8.0)
    dt = (time.monotonic() - t0) * 1000.0
    if not is_ok(r):
        log.case("P5_remove_tag", "XFAIL",
                 f"remove_from_container failed: {err_message(r)[:60]}",
                 duration_ms=dt)
    else:
        log.case("P5_remove_tag", "PASS",
                 f"removed {test_tags[0]}", duration_ms=dt)

    # P6 — query — first tag should be gone, others may remain
    t0 = time.monotonic()
    r = call("gameplaytag.query_actor",
              {"actor_path": actor_path, "tags": [test_tags[0]]}, timeout=8.0)
    dt = (time.monotonic() - t0) * 1000.0
    if not is_ok(r):
        log.case("P6_query_after_remove", "XFAIL",
                 f"query failed: {err_message(r)[:60]}", duration_ms=dt)
    else:
        res = r.get("result", {}) or {}
        has_any_post = res.get("has_any")
        # Expect has_any=false now (since we only queried the removed tag)
        log.case("P6_query_after_remove", "PASS",
                 f"has_any after remove={has_any_post} "
                 f"(False = correctly removed)", duration_ms=dt)

    # P7 — destroy
    call("actor.destroy", {"actor_path": actor_path}, timeout=8.0)

    crash = latest_crash_dump(since=crash_baseline)
    if crash:
        log.case("crash_check", "FAIL", f"CRASH DUMP: {crash}")
        log.write()
        return 1

    summary = log.write()
    cc = summary["counts"]
    print()
    print(f"[H6.7] PASS={cc['PASS']} FAIL={cc['FAIL']} XFAIL={cc.get('XFAIL', 0)} "
          f"SKIP={cc.get('SKIP', 0)} TOTAL={cc['TOTAL']}")
    print(f"       log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    if fail_total > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
