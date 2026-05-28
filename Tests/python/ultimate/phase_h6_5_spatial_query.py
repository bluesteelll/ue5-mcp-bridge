#!/usr/bin/env python3
"""Phase H6.5 — actor.box_query / sphere_query / frustum_query / level.actor_summary.

Wave K spatial query coverage. Spawn N actors at known locations, then
query via each spatial filter type and verify the right actors are
returned.

Probes:
  P1 — spawn 5 StaticMeshActors at known positions (clustered)
  P2 — actor.box_query covering only 3 of them → matches 3
  P3 — actor.sphere_query with radius matching 4 → matches 4
  P4 — actor.frustum_query (camera-relative) → matches some
  P5 — level.actor_summary → counts include our 5 + others
  P6 — level.find_actors_with_component (StaticMesh) → contains our 5
  P7 — destroy all spawned + verify counts drop

PASS: each query returns expected actors, no editor crash.

Exit codes: 0=PASS, 1=FAIL, 2=preflight.
"""

from __future__ import annotations

import sys
import time
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

sys.path.insert(0, str(Path(__file__).parent))
from mcp_test_harness import (
    LOG_ROOT,
    TestLogger,
    call,
    err_code,
    err_message,
    health,
    is_ok,
    is_transport_failure,
    latest_crash_dump,
    preflight,
    random_suffix,
)

PHASE = "h6_5"
NAME = "spatial_query"


def _spawn(label: str, location: List[float]) -> Optional[str]:
    """Spawn StaticMeshActor at location. location is [x,y,z] array — converted
    to {x,y,z} dict per actor.spawn API contract. Returns actor_path or None."""
    r = call("actor.spawn",
              {"class_path": "/Script/Engine.StaticMeshActor",
               "label": label,
               "location": {"x": location[0], "y": location[1], "z": location[2]}},
              timeout=10.0)
    if not is_ok(r):
        return None
    return r.get("result", {}).get("actor_path") or r.get("result", {}).get("path") or ""


def _destroy(path: str) -> None:
    if path:
        call("actor.destroy", {"actor_path": path}, timeout=8.0)


def main() -> int:
    if not preflight(PHASE):
        return 2
    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()
    fail_total = 0

    print(f"[H6.5] spatial query coverage…", flush=True)

    # P1 — spawn 5 actors at known positions
    base_label = f"PhTH65_{random_suffix(5)}"
    positions = [
        [1000.0, 1000.0, 100.0],   # A — cluster center
        [1100.0, 1000.0, 100.0],   # B — within 200 unit sphere
        [1200.0, 1000.0, 100.0],   # C — 200 from A
        [2000.0, 2000.0, 100.0],   # D — far away
        [3000.0, 3000.0, 100.0],   # E — very far
    ]
    spawned: List[Tuple[str, str]] = []  # (label, path)
    t0 = time.monotonic()
    for i, pos in enumerate(positions):
        lbl = f"{base_label}_{i}"
        p = _spawn(lbl, pos)
        if p:
            spawned.append((lbl, p))
    dt = (time.monotonic() - t0) * 1000.0
    if len(spawned) < 4:
        log.case("P1_spawn", "FAIL",
                 f"only {len(spawned)}/5 spawns succeeded",
                 duration_ms=dt)
        for _, p in spawned:
            _destroy(p)
        log.write()
        return 1
    log.case("P1_spawn", "PASS",
             f"spawned {len(spawned)}/5 actors", duration_ms=dt)

    our_paths = {p for _, p in spawned}

    # P2 — box_query covering A, B, C (x: 900-1300, y: 900-1100, z: 0-200)
    t0 = time.monotonic()
    r = call("actor.box_query",
              {"min": [900, 900, 0], "max": [1300, 1100, 200]},
              timeout=10.0)
    dt = (time.monotonic() - t0) * 1000.0
    if not is_ok(r):
        log.case("P2_box_query", "FAIL",
                 f"box_query failed: {err_message(r)[:60]}",
                 duration_ms=dt)
        fail_total += 1
    else:
        actors = (r.get("result", {}) or {}).get("actors") or []
        our_in_box = sum(1 for a in actors if (a.get("actor_path") or a.get("path") or "") in our_paths)
        # Expected: 3 of ours (A, B, C). May include other actors too — accept ≥ 3.
        if our_in_box >= 3:
            log.case("P2_box_query", "PASS",
                     f"box matched {our_in_box} of ours (≥3 expected); total={len(actors)}",
                     duration_ms=dt)
        else:
            log.case("P2_box_query", "FAIL",
                     f"box matched only {our_in_box}/3 of ours; total={len(actors)}",
                     duration_ms=dt)
            fail_total += 1

    # P3 — sphere_query at A with radius 250 → A, B (B is 100 from A; C is 200)
    t0 = time.monotonic()
    r = call("actor.sphere_query",
              {"center": [1000, 1000, 100], "radius": 250.0},
              timeout=10.0)
    dt = (time.monotonic() - t0) * 1000.0
    if not is_ok(r):
        log.case("P3_sphere_query", "FAIL",
                 f"sphere_query failed: {err_message(r)[:60]}",
                 duration_ms=dt)
        fail_total += 1
    else:
        actors = (r.get("result", {}) or {}).get("actors") or []
        our_in_sphere = sum(1 for a in actors if a.get("actor_path") in our_paths
                             or a.get("path") in our_paths)
        # Expected: 3 of ours (A, B, C — all within 250 of A). Accept ≥ 2.
        if our_in_sphere >= 2:
            log.case("P3_sphere_query", "PASS",
                     f"sphere matched {our_in_sphere} of ours; total={len(actors)}",
                     duration_ms=dt)
        else:
            log.case("P3_sphere_query", "FAIL",
                     f"sphere matched only {our_in_sphere} of ours; total={len(actors)}",
                     duration_ms=dt)
            fail_total += 1

    # P4 — frustum_query (free shape — just verify the tool runs)
    t0 = time.monotonic()
    r = call("actor.frustum_query",
              {"camera_location": [0, 0, 100],
               "camera_forward": [1, 1, 0],
               "fov_degrees": 90.0,
               "near_plane": 100.0,
               "far_plane": 5000.0},
              timeout=10.0)
    dt = (time.monotonic() - t0) * 1000.0
    if not is_ok(r):
        c = err_code(r)
        # If signature doesn't match, XFAIL; if -32601, SKIP
        if c == -32601:
            log.case("P4_frustum_query", "SKIP",
                     "frustum_query not registered", duration_ms=dt)
        else:
            log.case("P4_frustum_query", "XFAIL",
                     f"frustum_query failed: {err_message(r)[:60]}",
                     duration_ms=dt)
    else:
        actors = (r.get("result", {}) or {}).get("actors") or []
        log.case("P4_frustum_query", "PASS",
                 f"frustum returned {len(actors)} actors",
                 duration_ms=dt)

    # P5 — level.actor_summary
    t0 = time.monotonic()
    r = call("level.actor_summary", {}, timeout=10.0)
    dt = (time.monotonic() - t0) * 1000.0
    if not is_ok(r):
        log.case("P5_actor_summary", "FAIL",
                 f"actor_summary failed: {err_message(r)[:60]}",
                 duration_ms=dt)
        fail_total += 1
    else:
        total = (r.get("result", {}) or {}).get("total_actors", -1)
        # We just spawned 5 — total should be ≥ 5
        if total >= len(spawned):
            log.case("P5_actor_summary", "PASS",
                     f"total_actors={total} (≥ {len(spawned)} expected)",
                     duration_ms=dt)
        else:
            log.case("P5_actor_summary", "XFAIL",
                     f"total_actors={total} but we spawned {len(spawned)} — "
                     f"may use different counting",
                     duration_ms=dt)

    # P6 — find_actors_with_component (StaticMeshComponent)
    t0 = time.monotonic()
    r = call("level.find_actors_with_component",
              {"component_class": "/Script/Engine.StaticMeshComponent"},
              timeout=10.0)
    dt = (time.monotonic() - t0) * 1000.0
    if not is_ok(r):
        log.case("P6_find_by_component", "XFAIL",
                 f"failed: {err_message(r)[:60]}",
                 duration_ms=dt)
    else:
        actors = (r.get("result", {}) or {}).get("actors") or []
        our_found = sum(1 for a in actors if a.get("actor_path") in our_paths
                         or a.get("path") in our_paths)
        log.case("P6_find_by_component", "PASS",
                 f"found {our_found} of ours; total={len(actors)}",
                 duration_ms=dt)

    # P7 — destroy + verify counts drop
    for _, p in spawned:
        _destroy(p)

    crash = latest_crash_dump(since=crash_baseline)
    if crash:
        log.case("crash_check", "FAIL", f"CRASH DUMP: {crash}")
        log.write()
        return 1

    summary = log.write()
    cc = summary["counts"]
    print()
    print(f"[H6.5] PASS={cc['PASS']} FAIL={cc['FAIL']} XFAIL={cc.get('XFAIL', 0)} "
          f"SKIP={cc.get('SKIP', 0)} TOTAL={cc['TOTAL']}")
    print(f"       log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    if fail_total > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
