#!/usr/bin/env python3
"""Phase A3 — Optional-argument defaults.

Goal: verify that omitting optional args is GRACEFUL — the handler does
not surface -32602 for any field NOT discovered as required by A2's
chain walker.

Strategy (adapted from plan)
----------------------------
The plan calls for parsing each tool's header comment for `Args: { ... }`
to extract documented defaults. In practice plugin doc format is
inconsistent / single-line ("// foo.bar(mode='trigger'). Lane B.") and
not machine-parseable. So we test the WEAKER but cleaner property:

  When called with EXACTLY the required-arg set (no extras), every
  method either:
    (a) returns ok=true (defaults sufficed), OR
    (b) returns a non-(-32602) error (e.g. -32027 PIE-active,
        -32004 ObjectNotFound — the handler ran past arg validation)

A method that surfaces "missing required field 'X'" here means A2's
chain discovery did NOT find X — a coverage gap.

A method that returns an undocumented -32602 here is a DOC_GAP.

Per-method cases:
  1. minimal_call    — args = baseline (dummy values for required only)
  2. with_extras     — args = baseline + 3 obviously-unknown optional
                        keys (`_phantom_a`, `_phantom_b`, `_phantom_c`)
                        Expect: same response code as #1 (unknown args
                        silently ignored, not error)

Total cases: ~600 (312 methods × 2 probes).

Exit codes: 0=PASS, 1=FAIL, 2=editor died.
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
    cleanup_phantom_assets,
    dummy_value,
    err_code,
    err_message,
    health,
    is_ok,
    is_transport_failure,
    latest_crash_dump,
    preflight,
)

PHASE = "a3"
NAME = "optional_defaults"

# Strangely-named extras to verify "unknown fields are ignored, not errors"
EXTRAS_BAG = {
    "_phantom_optional_a": "ignored",
    "_phantom_optional_b": 42,
    "_phantom_optional_c": True,
}


def baseline_args_from_chain(chain: List[Tuple[str, str]]) -> Dict[str, Any]:
    return {f: dummy_value(t, f) for (t, f) in chain}


def discover_chain(method: str) -> List[Tuple[str, str]]:
    """Same logic as A2 chain discovery — short version inlined here so A3 is
    self-contained when A2 hasn't run."""
    import re
    # See phase_a2_required_args.py for the full regex rationale.
    RE_MISSING = re.compile(
        r"missing(?: or empty)? required (?:(?:non-empty|valid|numeric) )?(\w+)?\s*field '([A-Za-z0-9_]+)'",
        re.IGNORECASE,
    )
    chain: List[Tuple[str, str]] = []
    args: Dict[str, Any] = {}
    seen: set = set()
    for _ in range(12):
        r = call(method, args, timeout=6.0)
        if err_code(r) != -32602:
            break
        m = RE_MISSING.search(err_message(r) or "")
        if not m:
            break
        typ = (m.group(1) or "string").lower()
        field = m.group(2)
        if field in seen:
            break
        seen.add(field)
        chain.append((typ, field))
        args[field] = dummy_value(typ, field)
    return chain


def main() -> int:
    if not preflight(PHASE):
        return 2

    # Reuse A2's needs_args.json if available (fast path). Otherwise we'd
    # need to call tools.list and re-walk all chains.
    needs_args_path = LOG_ROOT / "a1_needs_args.json"
    if not needs_args_path.exists():
        print(f"FATAL: {needs_args_path} not found — run phase_a1_inventory.py first",
              file=sys.stderr)
        return 2
    methods_with_args = [m["method"] for m in json.loads(needs_args_path.read_text(encoding="utf-8"))["methods"]]
    if "--limit" in sys.argv:
        try:
            n = int(sys.argv[sys.argv.index("--limit") + 1])
            methods_with_args = methods_with_args[:n]
            print(f"[A3] LIMITED to first {n} methods", flush=True)
        except (ValueError, IndexError):
            pass
    print(f"[A3] loaded {len(methods_with_args)} methods", flush=True)

    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()
    fail_total = 0

    print("[A3] walking chains + probing optional-default behaviour…", flush=True)
    coverage_gaps: List[Dict[str, Any]] = []
    doc_gaps: List[Dict[str, Any]] = []

    for idx, method in enumerate(methods_with_args):
        chain = discover_chain(method)
        baseline = baseline_args_from_chain(chain)

        # ---- Case 1: minimal_call ---------------------------------------
        t0 = time.monotonic()
        r1 = call(method, baseline, timeout=8.0)
        dur1 = (time.monotonic() - t0) * 1000.0
        alive = health(timeout=3.0)
        if not alive:
            log.case(f"{method}/minimal", "FAIL", "EDITOR DIED", alive=False, duration_ms=dur1)
            log.write()
            print(f"  CRASHED on {method}/minimal", file=sys.stderr)
            return 2
        if is_transport_failure(r1):
            log.case(f"{method}/minimal", "FAIL", f"transport: {r1.get('_err')}",
                     alive=alive, duration_ms=dur1)
            fail_total += 1
            continue
        code1 = err_code(r1)
        if code1 == -32602:
            # Chain discovery was incomplete — A2 coverage gap.
            log.case(f"{method}/minimal", "FAIL",
                     f"-32602 with discovered baseline (coverage gap): {err_message(r1)[:80]}",
                     alive=alive, duration_ms=dur1, code=code1)
            coverage_gaps.append({"method": method, "msg": err_message(r1)})
            fail_total += 1
            continue
        else:
            # PASS — defaults sufficed OR downstream non-arg error fired.
            label = "ok" if is_ok(r1) else f"err {code1}"
            log.case(f"{method}/minimal", "PASS", label, alive=alive, duration_ms=dur1, code=code1)

        # ---- Case 2: with_extras (unknown fields should be ignored) ----
        with_extras = dict(baseline)
        with_extras.update(EXTRAS_BAG)
        t0 = time.monotonic()
        r2 = call(method, with_extras, timeout=8.0)
        dur2 = (time.monotonic() - t0) * 1000.0
        alive = health(timeout=3.0)
        if not alive:
            log.case(f"{method}/extras", "FAIL", "EDITOR DIED on unknown-keys probe",
                     alive=False, duration_ms=dur2)
            log.write()
            return 2
        if is_transport_failure(r2):
            log.case(f"{method}/extras", "FAIL", f"transport: {r2.get('_err')}",
                     alive=alive, duration_ms=dur2)
            fail_total += 1
            continue
        code2 = err_code(r2)
        # Expected: r2 has SAME outcome shape as r1 (unknown keys ignored).
        if is_ok(r1) and is_ok(r2):
            log.case(f"{method}/extras", "PASS", "unknown keys ignored (both ok)",
                     alive=alive, duration_ms=dur2)
        elif code1 == code2:
            log.case(f"{method}/extras", "PASS", f"unknown keys ignored (both err {code1})",
                     alive=alive, duration_ms=dur2)
        elif code2 in (-32602, -32600):
            log.case(f"{method}/extras", "XFAIL",
                     f"surface rejects unknown keys with {code2} (uncommon but valid)",
                     alive=alive, duration_ms=dur2, code=code2)
            doc_gaps.append({"method": method, "code": code2, "msg": err_message(r2)})
        else:
            # Different result code — could be a sensitivity to extra keys
            # (state side-effect) — flag as XFAIL for investigation.
            log.case(f"{method}/extras", "XFAIL",
                     f"outcome diverged: minimal={code1} extras={code2}",
                     alive=alive, duration_ms=dur2)
            doc_gaps.append({"method": method, "minimal_code": code1, "extras_code": code2})

        if (idx + 1) % 30 == 0:
            print(f"  [{idx+1}/{len(methods_with_args)}] last={method} "
                  f"coverage_gaps={len(coverage_gaps)} doc_gaps={len(doc_gaps)} fails={fail_total}",
                  flush=True)
        # Periodic cleanup to prevent Lane A queue saturation from chain-walker
        # side-effects (see A2 doc for rationale).
        if (idx + 1) % 50 == 0:
            cs = cleanup_phantom_assets()
            print(f"  cleanup@{idx+1}: folders={cs['folders_deleted']} actors={cs['actors_destroyed']}",
                  flush=True)

    # Stash gaps for follow-up
    if coverage_gaps:
        (LOG_ROOT / "a3_coverage_gaps.json").write_text(
            json.dumps({"count": len(coverage_gaps), "items": coverage_gaps}, indent=2),
            encoding="utf-8",
        )
        log.note(f"Wrote {len(coverage_gaps)} A2 coverage gaps to a3_coverage_gaps.json")
    if doc_gaps:
        (LOG_ROOT / "a3_doc_gaps.json").write_text(
            json.dumps({"count": len(doc_gaps), "items": doc_gaps}, indent=2),
            encoding="utf-8",
        )
        log.note(f"Wrote {len(doc_gaps)} DOC_GAP entries to a3_doc_gaps.json")

    crash = latest_crash_dump(since=crash_baseline)
    if crash:
        log.note(f"NEW CRASH DUMP DETECTED: {crash}")
        fail_total += 1

    summary = log.write()
    c = summary["counts"]
    print()
    print(f"[A3] PASS={c['PASS']} FAIL={c['FAIL']} XFAIL={c.get('XFAIL', 0)} TOTAL={c['TOTAL']}")
    print(f"     final alive={summary['final_health']}  crash_dumps={'YES' if crash else 'none'}")
    print(f"     coverage_gaps={len(coverage_gaps)}  doc_gaps={len(doc_gaps)}")
    print(f"     log: {log.md_path}")

    if not summary["final_health"]:
        return 2
    if fail_total > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
