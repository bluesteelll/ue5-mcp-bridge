#!/usr/bin/env python3
"""Phase A1 — Tool inventory + 100% dispatch.

Goal: every registered method dispatches. No -32601 MethodNotFound on the
canonical list (`tools.list`). Editor stays alive throughout.

PASS criteria (per method):
  - ok=true  (clean success), OR
  - error.code in the documented Bridge range (-32000..-32700) — meaning
    dispatcher accepted the method and the handler returned a structured
    error (missing required field, PIE blocked, asset not found, etc.).

FAIL only if:
  - error.code == -32601 (method not found — registration broken)
  - transport timeout / no-connect / malformed reply
  - editor health() flips to False at any point

Side artefact: writes `needs_args.json` listing methods that returned
-32602 InvalidParams — that file feeds Phase A2.

Exit codes: 0=PASS, 1=FAIL (any case), 2=editor died.
"""

from __future__ import annotations

import json
import sys
import time
from pathlib import Path

# Make local harness importable when run from anywhere.
sys.path.insert(0, str(Path(__file__).parent))

from mcp_test_harness import (
    LOG_ROOT,
    TestLogger,
    call,
    discover_methods,
    discover_methods_authoritative,
    discover_methods_live,
    err_code,
    err_message,
    health,
    is_ok,
    is_transport_failure,
    is_valid_bridge_error,
    latest_crash_dump,
    preflight,
    snapshot,
)

PHASE = "a1"
NAME = "tool_inventory"

# Codes the dispatcher returns when the method exists but cannot run cleanly
# under empty args. All of these prove dispatch succeeded — PASS.
ACCEPTABLE_ERROR_CODES = {
    -32602,  # InvalidParams (missing required field)
    -32603,  # InternalError (handler raised exception)
    -32004,  # ObjectNotFound (some handlers do path lookup before arg check)
    -32005,  # PropertyNotFound
    -32006,  # PropertyTypeMismatch
    -32010,  # InvalidPath
    -32011,  # WrongClass
    -32012,  # ObjectInvalid
    -32014,  # PathInUse
    -32015,  # StaleCursor
    -32021,  # ClassAbstract
    -32024,  # ContextInvalid
    -32027,  # PIEActive
    -32028,  # PIEAlreadyRunning
    -32029,  # WorldPartitionNotSupported
    -32034,  # InvalidWorld
    -32038,  # PIENotActive
    -32040,  # NiagaraParameterNotFound
    -32044,  # LandscapeNotFound
    -32047,  # CVarReadOnly
    -32056,  # FolderNotFound
    -32057,  # FunctionParameterDuplicate
    -32058,  # OperationFailed
}

# Codes that = dispatcher said "no such method" → FAIL.
HARD_FAIL_CODES = {-32601}


def main() -> int:
    if not preflight(PHASE):
        return 2

    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()

    live = discover_methods_live()
    src = discover_methods()
    auth = discover_methods_authoritative()

    log.note(f"tools.list reports {len(live)} methods")
    log.note(f"source parse reports {len(src)} methods")
    log.note(f"authoritative union: {len(auth)} methods")

    src_by_name = {m["name"]: m for m in src}
    src_only = sorted(set(m["name"] for m in src) - set(live))
    live_only = sorted(set(live) - set(m["name"] for m in src))
    if src_only:
        log.note(f"In source-parse but not in tools.list ({len(src_only)}): {src_only[:8]}{'…' if len(src_only) > 8 else ''}")
    if live_only:
        log.note(f"In tools.list but not in source-parse ({len(live_only)}): {live_only[:8]}{'…' if len(live_only) > 8 else ''}")

    needs_args: list[dict] = []
    methods = list(live)
    print(f"[A1] dispatching {len(methods)} methods (canonical from tools.list)…", flush=True)

    crashed = False
    fail_total = 0
    for idx, name in enumerate(methods):
        t0 = time.monotonic()
        r = call(name, {}, timeout=8.0)
        dur_ms = (time.monotonic() - t0) * 1000.0

        # Alive check after each call (cheap — uses memreport)
        alive = health(timeout=3.0)
        if not alive:
            crashed = True
            log.case(name, "FAIL", "EDITOR DIED on this call", alive=False, duration_ms=dur_ms)
            print(f"  CRASHED on {name}", file=sys.stderr)
            fail_total += 1
            break

        if is_transport_failure(r):
            log.case(name, "FAIL", f"transport: {r.get('_err')}", alive=alive, duration_ms=dur_ms, raw=str(r)[:120])
            fail_total += 1
            continue

        if is_ok(r):
            log.case(name, "PASS", "ok", alive=alive, duration_ms=dur_ms,
                     thread_safe=src_by_name.get(name, {}).get("thread_safe"))
            continue

        code = err_code(r)
        msg = err_message(r)
        if code in HARD_FAIL_CODES:
            log.case(name, "FAIL", f"-32601 method not found: {msg[:80]}",
                     alive=alive, duration_ms=dur_ms)
            fail_total += 1
            continue

        if code in ACCEPTABLE_ERROR_CODES:
            log.case(name, "PASS", f"err {code}: {msg[:80]}",
                     alive=alive, duration_ms=dur_ms, code=code)
            if code == -32602:
                needs_args.append({"method": name, "code": code, "msg": msg})
            continue

        if is_valid_bridge_error(r):
            # Unknown but in valid Bridge range — accept, log for review
            log.case(name, "PASS", f"err {code} (not in known list): {msg[:80]}",
                     alive=alive, duration_ms=dur_ms, code=code, uncatalogued=True)
            continue

        log.case(name, "FAIL", f"unexpected: {str(r)[:120]}",
                 alive=alive, duration_ms=dur_ms)
        fail_total += 1

        # Progress beep every 50 methods
        if (idx + 1) % 50 == 0:
            print(f"  [{idx + 1}/{len(methods)}] last={name} fails_so_far={fail_total}", flush=True)

    # Write needs_args.json for Phase A2 consumption
    needs_args_path = LOG_ROOT / f"{PHASE}_needs_args.json"
    needs_args_path.write_text(
        json.dumps({"phase": PHASE, "count": len(needs_args), "methods": needs_args}, indent=2),
        encoding="utf-8",
    )
    log.note(f"Wrote {len(needs_args)} -32602 entries to {needs_args_path.name} (input to A2)")

    # Crash dump scrape
    crash = latest_crash_dump(since=crash_baseline)
    if crash:
        log.note(f"NEW CRASH DUMP DETECTED: {crash}")
        fail_total += 1

    summary = log.write()
    counts = summary["counts"]
    print()
    print(f"[A1] PASS={counts['PASS']} FAIL={counts['FAIL']} SKIP={counts.get('SKIP', 0)} TOTAL={counts['TOTAL']}")
    print(f"     final alive={summary['final_health']}  crash_dumps={'YES' if crash else 'none'}")
    print(f"     log: {log.md_path}")

    if crashed:
        return 2
    if fail_total > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
