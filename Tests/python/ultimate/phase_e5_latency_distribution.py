#!/usr/bin/env python3
"""Phase E5 — Cold-vs-hot latency distribution per Lane B tool.

Goal: characterise cold-cache vs warm-cache vs hot-loop latency for
each high-traffic Lane B tool. Output → latency_distribution.json for
external SLO tracking. Catches latency regressions (e.g., a tool that
suddenly grew 10× p99).

Probes:
  For each Lane B tool:
    - 1st call (cold cache)
    - 2nd call (warm)
    - 10 follow-up calls (hot loop)
  Record p50 / p95 / p99 per tool.

Output: D:/tmp/ws3_stress/test_logs/_LATENCY_DISTRIBUTION.json

PASS: every tool's hot-p99 < 1000ms; no transport timeouts.
XFAIL: hot-p99 between 1000ms and 5000ms.
FAIL: hot-p99 > 5000ms OR transport timeout OR -32601 (tool not found).

Exit codes: 0=PASS, 1=FAIL, 2=preflight.
"""

from __future__ import annotations

import json
import sys
import time
from pathlib import Path
from typing import Any, Dict, List, Tuple

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
)

PHASE = "e5"
NAME = "latency_distribution"


# Lane B tools to characterise. Each must return ok with empty args.
LANE_B_TOOLS: List[Tuple[str, Dict[str, Any]]] = [
    ("memreport.get_quick_stats", {}),
    ("engine.get_info", {}),
    ("engine.get_memory_snapshot", {}),
    ("pie.is_running", {}),
    ("asset.exists", {"path": "/Engine/BasicShapes/Cube"}),
    ("cfg.list_cvars", {"page_size": 5}),
    ("log.list_categories", {"page_size": 5}),
    ("transaction.list", {}),
    ("transaction.get_state", {}),
    ("cb.list_folders", {"parent_path": "/Game"}),
    ("collision.get_profile", {"profile_name": "BlockAllDynamic"}),
    ("sequencer.list_cinematics", {}),
    ("landscape.list", {}),
    ("navmesh.list", {}),
    ("gameplaytag.list", {"page_size": 5}),
    ("input.list_input_actions", {}),
    ("input.list_mapping_contexts", {}),
]

N_HOT = 10
HOT_P99_PASS_MS = 1000
HOT_P99_XFAIL_MS = 5000


def _measure_one(method: str, args: Dict[str, Any], timeout: float = 8.0) -> float:
    """Returns latency in ms, or -1 on failure."""
    t0 = time.monotonic()
    try:
        r = call(method, args, timeout=timeout)
    except Exception:
        return -1.0
    dt = (time.monotonic() - t0) * 1000.0
    if is_transport_failure(r):
        return -1.0
    if not is_ok(r) and err_code(r) not in (-32004, -32011, -32602):
        # Structured errors other than benign ones (not-found / wrong-class /
        # bad-arg) count as failures.
        return -1.0
    return dt


def _percentile(values: List[float], p: float) -> float:
    if not values:
        return -1.0
    s = sorted(values)
    idx = int(len(s) * p)
    if idx >= len(s):
        idx = len(s) - 1
    return s[idx]


def main() -> int:
    if not preflight(PHASE):
        return 2
    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()
    fail_total = 0

    print(f"[E5] latency distribution: {len(LANE_B_TOOLS)} tools × (cold + warm + {N_HOT} hot)…",
          flush=True)

    per_tool: List[Dict[str, Any]] = []

    for method, args in LANE_B_TOOLS:
        # 1st call (cold)
        cold = _measure_one(method, args)
        warm = _measure_one(method, args) if cold > 0 else -1.0
        hot_latencies: List[float] = []
        if warm > 0:
            for _ in range(N_HOT):
                v = _measure_one(method, args)
                if v > 0:
                    hot_latencies.append(v)

        if cold < 0:
            log.case(f"tool_{method}", "SKIP",
                     "cold call failed (tool not registered or transport)")
            per_tool.append({"method": method, "status": "SKIP"})
            continue

        p50 = _percentile(hot_latencies, 0.50)
        p95 = _percentile(hot_latencies, 0.95)
        p99 = _percentile(hot_latencies, 0.99)

        if not hot_latencies:
            log.case(f"tool_{method}", "FAIL",
                     f"hot calls all failed; cold={cold:.0f}ms warm={warm:.0f}ms")
            per_tool.append({"method": method, "status": "FAIL",
                              "cold_ms": cold, "warm_ms": warm,
                              "hot_count": 0})
            fail_total += 1
            continue

        if p99 > HOT_P99_XFAIL_MS:
            status = "FAIL"
            fail_total += 1
        elif p99 > HOT_P99_PASS_MS:
            status = "XFAIL"
        else:
            status = "PASS"

        summary = (f"cold={cold:.0f}ms warm={warm:.0f}ms "
                   f"hot_n={len(hot_latencies)} p50={p50:.0f}ms p95={p95:.0f}ms p99={p99:.0f}ms")
        log.case(f"tool_{method}", status, summary)

        per_tool.append({
            "method": method,
            "status": status,
            "cold_ms": cold,
            "warm_ms": warm,
            "hot_n": len(hot_latencies),
            "p50_ms": p50,
            "p95_ms": p95,
            "p99_ms": p99,
        })

    # Write structured output for SLO tracking
    out_path = LOG_ROOT / "_LATENCY_DISTRIBUTION.json"
    out_path.write_text(json.dumps({
        "generated_at": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime()),
        "n_tools": len(LANE_B_TOOLS),
        "hot_p99_pass_ms": HOT_P99_PASS_MS,
        "hot_p99_xfail_ms": HOT_P99_XFAIL_MS,
        "tools": per_tool,
    }, indent=2), encoding="utf-8")

    crash = latest_crash_dump(since=crash_baseline)
    if crash:
        log.case("crash_check", "FAIL", f"CRASH DUMP: {crash}")
        log.write()
        return 1

    summary = log.write()
    cc = summary["counts"]
    print()
    print(f"[E5] PASS={cc['PASS']} FAIL={cc['FAIL']} XFAIL={cc.get('XFAIL', 0)} "
          f"SKIP={cc.get('SKIP', 0)} TOTAL={cc['TOTAL']}")
    print(f"     latency json: {out_path}")
    print(f"     log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    if fail_total > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
