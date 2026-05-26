#!/usr/bin/env python3
"""Phase A4 — Type coercion / strictness.

Goal: characterise per-surface tolerance to "reasonable" JSON-type
confusion. Some Bridge tools coerce friendly values (`"42"` → 42, `1.0` → 1);
others enforce strict types. The plan says document per-surface
behaviour, flag deviations from the documented contract.

For each (method, field, type) discovered by A2's chain walker, run
the coercion matrix below. Coverage = method × field × probe.

Coercion matrix
---------------

  Field type | Coercion probe                | Expected outcome
  -----------+-------------------------------+-------------------------
  int/uint   | 1.5 (float)                   | accept-and-truncate OR
                                               -32602 strict-reject
  int/uint   | "42" (string)                 | most surfaces reject
  number     | true (bool)                   | usually accepted as 1
  bool       | 1 (int)                       | usually accepted as true
  bool       | "true" (string)               | mixed
  string     | 42 (number)                   | TryGetStringField coerces
  string     | true (bool)                   | TryGetStringField coerces
  array      | "x" (scalar)                  | strictly reject -32602
  object     | [] (array)                    | strictly reject -32602

PASS criteria
-------------
Either OK or any structured error code. FAIL on transport/timeout/crash.
Each case logs which path was taken (`coerced`/`strict-reject`/`other`)
for the final report's coercion matrix.

Exit codes: 0=PASS, 1=FAIL (only on transport/crash), 2=editor died.
"""

from __future__ import annotations

import json
import re
import sys
import time
from pathlib import Path
from typing import Any, Dict, List, Tuple

sys.path.insert(0, str(Path(__file__).parent))
from mcp_test_harness import (
    LOG_ROOT,
    Connection,
    TestLogger,
    call,
    cleanup_phantom_assets,
    discover_chain_via_probes,
    discover_chains_static,
    dummy_value,
    err_code,
    err_message,
    health,
    is_ok,
    is_transport_failure,
    latest_crash_dump,
    preflight,
)

PHASE = "a4"
NAME = "type_coercion"

RE_MISSING = re.compile(
    r"missing(?: or empty)? required (?:(?:non-empty|valid|numeric) )?(\w+)?\s*field '([A-Za-z0-9_]+)'",
    re.IGNORECASE,
)

# Per-type coercion matrix
COERCE: Dict[str, List[Tuple[str, Any, str]]] = {
    "string":  [
        ("number_as_string", 42,    "number where string expected"),
        ("bool_as_string",   True,  "bool where string expected"),
    ],
    "number":  [
        ("string_as_number", "42",  "valid numeric string"),
        ("bool_as_number",   True,  "bool where number expected"),
        ("float_as_int",     1.5,   "float where int expected"),
    ],
    "int":     [
        ("string_as_int", "42",     "string-as-int"),
        ("float_as_int",  1.5,      "float-as-int"),
    ],
    "uint":    [
        ("string_as_uint", "42",    "string-as-uint"),
        ("float_as_uint",  1.5,     "float-as-uint"),
    ],
    "float":   [
        ("string_as_float", "1.5",  "string-as-float"),
        ("int_as_float",    1,      "int-as-float (expected pass)"),
    ],
    "double":  [
        ("string_as_double","1.5",  "string-as-double"),
        ("int_as_double",   1,      "int-as-double (expected pass)"),
    ],
    "bool":    [
        ("int_as_bool",    1,        "int 1 as bool"),
        ("string_as_bool", "true",   "string 'true' as bool"),
        ("zero_as_bool",   0,        "int 0 as bool false"),
    ],
    "array":   [
        ("scalar_as_array", "x",     "scalar where array expected"),
        ("object_as_array", {"a":1}, "object where array expected"),
    ],
    "object":  [
        ("array_as_object", [],      "array where object expected"),
        ("scalar_as_object","x",     "scalar where object expected"),
    ],
}


# Static chain cache, populated lazily at first use.
_STATIC_CHAINS: Optional[Dict[str, List[Tuple[str, str]]]] = None


def discover_chain_keepalive(conn: Connection, method: str) -> List[Tuple[str, str]]:
    """Hybrid chain discovery — static source-parse first, multi-probe live
    fallback (up to 5 iters) when static chain incomplete. Avoids Lane A
    saturation: each probe short-circuits at first missing field. See A2
    for full rationale.
    """
    global _STATIC_CHAINS
    if _STATIC_CHAINS is None:
        _STATIC_CHAINS = discover_chains_static()
    initial = _STATIC_CHAINS.get(method, [])
    return discover_chain_via_probes(conn, method, max_iter=6, initial=initial)


def classify_outcome(r: Dict[str, Any]) -> str:
    """coerced / strict-reject / pie-blocked / not-found / other / transport-failed"""
    if is_transport_failure(r):
        return "transport-failed"
    if is_ok(r):
        return "coerced"
    c = err_code(r)
    if c == -32602:
        return "strict-reject"
    if c in (-32027, -32028, -32038):
        return "pie-blocked"
    if c in (-32004, -32005, -32011):
        return "not-found"
    return f"other({c})"


def main() -> int:
    if not preflight(PHASE):
        return 2

    needs_args_path = LOG_ROOT / "a1_needs_args.json"
    if not needs_args_path.exists():
        print(f"FATAL: {needs_args_path} not found — run phase_a1 first", file=sys.stderr)
        return 2
    methods = [m["method"] for m in json.loads(needs_args_path.read_text(encoding="utf-8"))["methods"]]
    if "--limit" in sys.argv:
        try:
            n = int(sys.argv[sys.argv.index("--limit") + 1])
            methods = methods[:n]
            print(f"[A4] LIMITED to first {n} methods", flush=True)
        except (ValueError, IndexError):
            pass
    print(f"[A4] loaded {len(methods)} methods", flush=True)

    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()
    fail_total = 0
    case_total = 0
    outcomes_by_typ: Dict[str, Dict[str, int]] = {}

    for m_idx, method in enumerate(methods):
        try:
            conn = Connection()
            conn.__enter__()
        except OSError as e:
            log.case(f"{method}/connect", "FAIL", f"connect failed: {e}", alive=False)
            fail_total += 1
            continue
        try:
            chain = discover_chain_keepalive(conn, method)
            baseline = {f: dummy_value(t, f) for (t, f) in chain}
            for (typ, field) in chain:
                probes = COERCE.get(typ, [])
                for probe_id, probe_val, probe_desc in probes:
                    case_id = f"{method}#{field}({typ})/{probe_id}"
                    case_total += 1
                    args = dict(baseline)
                    args[field] = probe_val
                    t0 = time.monotonic()
                    try:
                        r = conn.call_keepalive(method, args, timeout=6.0)
                    except Exception as e:
                        r = {"_err": "socket_died", "_exc": str(e)}
                    dur_ms = (time.monotonic() - t0) * 1000.0
                    outcome = classify_outcome(r)
                    outcomes_by_typ.setdefault(typ, {}).setdefault(outcome, 0)
                    outcomes_by_typ[typ][outcome] += 1
                    if outcome == "transport-failed":
                        log.case(case_id, "FAIL", f"transport: {r.get('_err')}",
                                 duration_ms=dur_ms)
                        fail_total += 1
                    else:
                        log.case(case_id, "PASS", f"{outcome}: {probe_desc}",
                                 duration_ms=dur_ms, outcome=outcome,
                                 code=err_code(r))
        finally:
            try:
                conn.__exit__(None, None, None)
            except Exception:
                pass
        if (m_idx + 1) % 10 == 0:
            if not health(timeout=3.0):
                log.note(f"editor died at method {m_idx+1}/{len(methods)}")
                log.write()
                return 2
            if (m_idx + 1) % 50 == 0:
                print(f"  [{m_idx+1}/{len(methods)}] cases={case_total} fails={fail_total}", flush=True)
                cs = cleanup_phantom_assets()
                print(f"  cleanup@{m_idx+1}: folders={cs['folders_deleted']} actors={cs['actors_destroyed']}",
                      flush=True)

    # Stash outcome matrix
    (LOG_ROOT / "a4_coercion_matrix.json").write_text(
        json.dumps(outcomes_by_typ, indent=2, sort_keys=True), encoding="utf-8")

    crash = latest_crash_dump(since=crash_baseline)
    if crash:
        log.note(f"NEW CRASH DUMP: {crash}")
        fail_total += 1
    summary = log.write()
    c = summary["counts"]
    print()
    print(f"[A4] PASS={c['PASS']} FAIL={c['FAIL']} TOTAL={c['TOTAL']}")
    print(f"     coercion matrix: {LOG_ROOT}/a4_coercion_matrix.json")
    print(f"     log: {log.md_path}")
    if not summary["final_health"]:
        return 2
    if fail_total > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
