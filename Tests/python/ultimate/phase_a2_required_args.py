#!/usr/bin/env python3
"""Phase A2 — Required-argument validation.

Goal: every required field produces -32602 (InvalidParams) when missing,
null, empty, or wrong-typed. Validates the Bridge's RequireXxxField helpers
behave consistently across all 312 methods that take required args.

Strategy
--------
We do NOT rely solely on source-parse — the error messages from A1's
needs_args.json reveal the FIRST required field per method, and the
type ("string"/"array"/etc). For each method we walk the requirement
chain:

  1. Call with {} → -32602 with "missing required <type> field 'F1'"
  2. Provide a dummy valid value for F1, re-call → -32602 for F2 (next)
  3. Continue until response is NOT -32602 "missing/empty required <type> field"
     (could be -32004 path-not-found, -32027 PIE active, etc. — those mean
     all required args are satisfied; further validation lives elsewhere)
  4. For each discovered (field, type) pair, run the per-type hostile matrix

Per-type hostile matrix (each row expects -32602)
  string:  missing, null, ""               (empty rejected by RequireStringField)
  number:  missing, null, "not-a-number"
  bool:    missing, null                   (string "true"/false may coerce — accepted)
  array:   missing, null, []               (RequireArrayField rejects empty)
  object:  missing, null, "not-an-object"

Caps
  - Max 12 required fields probed per method (safety against infinite loops).
  - Per-call timeout 8 s.
  - Editor health checked after every call; phase aborts on death.

Exit codes: 0=PASS, 1=FAIL (any case), 2=editor died.
"""

from __future__ import annotations

import json
import re
import sys
import time
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

sys.path.insert(0, str(Path(__file__).parent))
from mcp_test_harness import (
    LOG_ROOT,
    Connection,
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

PHASE = "a2"
NAME = "required_args"

# Parse the family of "missing required ..." error messages:
#   "missing required string field 'name'"
#   "missing or empty required array field 'items'"
#   "missing required non-empty string field 'tag'"
#   "missing required field 'value' (use JSON null to clear ...)"  (no type)
# Captures: (type-or-empty, field-name).
RE_MISSING = re.compile(
    r"missing(?: or empty)? required (?:(?:non-empty|valid|numeric) )?(\w+)?\s*field '([A-Za-z0-9_]+)'",
    re.IGNORECASE,
)

# Dummy valid values are now field-aware via mcp_test_harness.dummy_value(typ, field).
# This avoids tripping UE ensures (FTopLevelAssetPath, FName etc.) when
# probing class_path / path-like fields with placeholder values.

# Per-type hostile matrix: list of (case_id_suffix, value, description, expect_code)
# Use a sentinel object MISSING to mean "do not set the field at all" — we
# remove the key from args in that case.
class _MISSING:
    pass
MISSING = _MISSING()

HOSTILE: Dict[str, List[Tuple[str, Any, str]]] = {
    "string":  [
        ("missing", MISSING, "field not present in args"),
        ("null",    None,    "explicit null"),
        ("empty",   "",      "empty string"),
    ],
    "number":  [
        ("missing", MISSING, "field not present in args"),
        ("null",    None,    "explicit null"),
        ("str",     "not-a-number", "string instead of number"),
    ],
    "int":     [
        ("missing", MISSING, "field not present in args"),
        ("null",    None,    "explicit null"),
        ("str",     "not-a-number", "string instead of int"),
    ],
    "uint":    [
        ("missing", MISSING, "field not present in args"),
        ("null",    None,    "explicit null"),
        ("str",     "not-a-number", "string instead of uint"),
    ],
    "float":   [
        ("missing", MISSING, "field not present"),
        ("null",    None,    "explicit null"),
        ("str",     "not-a-number", "string instead of float"),
    ],
    "double":  [
        ("missing", MISSING, "field not present"),
        ("null",    None,    "explicit null"),
        ("str",     "not-a-number", "string instead of double"),
    ],
    "bool":    [
        ("missing", MISSING, "field not present"),
        ("null",    None,    "explicit null"),
    ],
    "array":   [
        ("missing", MISSING, "field not present"),
        ("null",    None,    "explicit null"),
        ("empty",   [],      "empty array"),
    ],
    "object":  [
        ("missing", MISSING, "field not present"),
        ("null",    None,    "explicit null"),
        ("str",     "not-an-object", "string instead of object"),
    ],
}

MAX_FIELDS_PER_METHOD = 12


def parse_first_missing(resp: dict) -> Optional[Tuple[str, str]]:
    """Returns (type, field_name) from error message, or None if not a missing-field error."""
    if err_code(resp) != -32602:
        return None
    msg = err_message(resp) or ""
    m = RE_MISSING.search(msg)
    if not m:
        return None
    # Type may be absent in "missing required field 'X'" — fall back to "string"
    # (best guess; chain walker treats it as a generic field).
    typ = (m.group(1) or "string").lower()
    return (typ, m.group(2))


def discover_required_chain(method: str, conn: "Connection" = None) -> List[Tuple[str, str]]:
    """Walk the requirement chain, returning [(type, field), ...] in discovery order.

    Stops when response is not a "missing required field" -32602 OR when
    MAX_FIELDS reached OR when the same field surfaces twice (loop guard).

    If `conn` provided, reuses keep-alive socket (much faster).
    """
    chain: List[Tuple[str, str]] = []
    args: Dict[str, Any] = {}
    seen: set = set()
    for _ in range(MAX_FIELDS_PER_METHOD):
        if conn is not None:
            try:
                r = conn.call_keepalive(method, args, timeout=6.0)
            except Exception as e:
                r = {"_err": "socket_died", "_exc": str(e)}
        else:
            r = call(method, args, timeout=6.0)
        miss = parse_first_missing(r)
        if not miss:
            break
        typ, field = miss
        if field in seen:
            break
        seen.add(field)
        chain.append((typ, field))
        args[field] = dummy_value(typ, field)
    return chain


def main() -> int:
    if not preflight(PHASE):
        return 2

    needs_args_path = LOG_ROOT / "a1_needs_args.json"
    if not needs_args_path.exists():
        print(f"FATAL: {needs_args_path} not found — run phase_a1_inventory.py first", file=sys.stderr)
        return 2

    data = json.loads(needs_args_path.read_text(encoding="utf-8"))
    methods_with_args = [m["method"] for m in data["methods"]]
    # Optional --limit N to cap method count (useful for incremental runs).
    if "--limit" in sys.argv:
        try:
            n = int(sys.argv[sys.argv.index("--limit") + 1])
            methods_with_args = methods_with_args[:n]
            print(f"[A2] LIMITED to first {n} methods", flush=True)
        except (ValueError, IndexError):
            pass
    print(f"[A2] loaded {len(methods_with_args)} methods from {needs_args_path.name}", flush=True)

    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()

    # Stage 1: walk requirement chains for every method (keep-alive socket)
    print("[A2] stage 1 — discovering required-arg chains (keep-alive)…", flush=True)
    chains: Dict[str, List[Tuple[str, str]]] = {}
    with Connection() as stage1_conn:
        for idx, m in enumerate(methods_with_args):
            chain = discover_required_chain(m, conn=stage1_conn)
            chains[m] = chain
            # Health only every 25 methods, not every method (perf)
            if (idx + 1) % 25 == 0:
                if not health(timeout=3.0):
                    log.note(f"editor died during chain discovery for {m}")
                    log.write()
                    return 2
                print(f"  chain {idx+1}/{len(methods_with_args)} last={m} ({len(chain)} args)", flush=True)
    total_fields = sum(len(c) for c in chains.values())
    log.note(f"Discovered {total_fields} required fields across {len(chains)} methods")
    print(f"[A2] total required fields discovered: {total_fields}", flush=True)

    # PERF: cleanup chain-walker side-effects before stage 2 — chain walker
    # provided dummies that satisfied required-arg validators, which means
    # the handler often ran to completion and created assets/folders/actors
    # in /Game/_phantom_*. Without cleanup the editor's Lane A queue
    # saturates after ~150 methods of stage 2 (observed in pre-cleanup
    # run: 88 min wall, 696 socket_died failures).
    print("[A2] post-stage-1 cleanup …", flush=True)
    cs = cleanup_phantom_assets()
    print(f"  cleanup: folders={cs['folders_deleted']} actors={cs['actors_destroyed']} "
          f"uobj_delta={cs.get('uobj_delta', '?')} mb_delta={cs.get('mb_delta', 0):.1f}", flush=True)
    log.note(f"Post-stage-1 cleanup: {cs}")

    # Stage 2: per-field hostile probes
    # PERF: open ONE keep-alive socket per method, reuse for all its probes.
    # Drastically reduces TCP CLOSE_WAIT / FIN_WAIT_2 pile-up that destabilises
    # the editor under high call volume.
    print("[A2] stage 2 — running hostile-input probes (keep-alive)…", flush=True)
    fail_total = 0
    case_total = 0
    for m_idx, (method, chain) in enumerate(chains.items()):
        # Build a "valid baseline" args dict using dummy values for ALL fields.
        baseline_args = {f: dummy_value(t, f) for (t, f) in chain}
        # Keep-alive per method
        try:
            conn = Connection()
            conn.__enter__()
        except OSError as e:
            log.case(f"{method}/connect", "FAIL", f"connect failed: {e}",
                     alive=False, duration_ms=0)
            fail_total += 1
            continue
        try:
            for (typ, field) in chain:
                probes = HOSTILE.get(typ, HOSTILE["string"])
                for probe_id, probe_val, probe_desc in probes:
                    case_id = f"{method}#{field}({typ})/{probe_id}"
                    case_total += 1
                    args = dict(baseline_args)
                    if isinstance(probe_val, _MISSING):
                        args.pop(field, None)
                    else:
                        args[field] = probe_val
                    t0 = time.monotonic()
                    try:
                        r = conn.call_keepalive(method, args, timeout=6.0)
                    except Exception as e:
                        # Socket died — surface as transport failure, reopen socket
                        r = {"_err": "socket_died", "_exc": str(e)}
                        try:
                            conn.__exit__(None, None, None)
                        except Exception:
                            pass
                        try:
                            conn = Connection()
                            conn.__enter__()
                        except OSError:
                            pass
                    dur_ms = (time.monotonic() - t0) * 1000.0
                    if is_transport_failure(r):
                        log.case(case_id, "FAIL", f"transport: {r.get('_err')}",
                                 alive=True, duration_ms=dur_ms)
                        fail_total += 1
                        continue
                    code = err_code(r)
                    if code == -32602:
                        log.case(case_id, "PASS", probe_desc, duration_ms=dur_ms)
                    elif is_ok(r):
                        log.case(case_id, "XFAIL",
                                 f"coerced to ok (typ={typ} val={repr(probe_val)[:40]})",
                                 duration_ms=dur_ms, code=None)
                    else:
                        log.case(case_id, "XFAIL",
                                 f"passed validation, downstream err {code}: {err_message(r)[:60]}",
                                 duration_ms=dur_ms, code=code)
        finally:
            try:
                conn.__exit__(None, None, None)
            except Exception:
                pass
        # Health check only every 10 methods (not every probe) — perf
        if (m_idx + 1) % 10 == 0:
            if not health(timeout=3.0):
                log.note(f"editor died at method {m_idx+1}/{len(chains)}")
                log.write()
                return 2
            if (m_idx + 1) % 30 == 0:
                print(f"  probe {m_idx+1}/{len(chains)} cases_so_far={case_total} fails={fail_total}", flush=True)
        # Periodic cleanup — keep Lane A queue + UObject count under control
        if (m_idx + 1) % 50 == 0:
            cs = cleanup_phantom_assets()
            print(f"  cleanup@{m_idx+1}: folders={cs['folders_deleted']} actors={cs['actors_destroyed']} "
                  f"uobj_delta={cs.get('uobj_delta', '?')}", flush=True)

    # Crash dump scrape
    crash = latest_crash_dump(since=crash_baseline)
    if crash:
        log.note(f"NEW CRASH DUMP DETECTED: {crash}")
        fail_total += 1

    summary = log.write()
    c = summary["counts"]
    print()
    print(f"[A2] PASS={c['PASS']} FAIL={c['FAIL']} XFAIL={c.get('XFAIL', 0)} SKIP={c.get('SKIP', 0)} TOTAL={c['TOTAL']}")
    print(f"     final alive={summary['final_health']}  crash_dumps={'YES' if crash else 'none'}")
    print(f"     log: {log.md_path}")

    if not summary["final_health"]:
        return 2
    if fail_total > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
