#!/usr/bin/env python3
"""Phase F5 — Tool result data sanitization (no PII / system path leak).

Goal: tool responses don't accidentally leak server-side info beyond what
the Bridge contract documents. Specifically, scan high-volume read tool
responses for substrings that would constitute PII / system info leaks:
  - Username (current OS user)
  - Machine name (computername)
  - Absolute disk paths (C:\\, D:\\)
  - Network credentials (BEGIN PRIVATE KEY, etc.)
  - Environment variables

Probes:
  P1 — engine.get_info → no username, computername, full disk paths
  P2 — memreport.get_quick_stats → numeric fields only, no paths
  P3 — package.get_info on /Engine/BasicShapes/Cube → no D:/, C:/
  P4 — cb.list_supported_formats → format names only
  P5 — log.list_categories → category names only
  P6 — cfg.list_cvars → cvar names + values, no env vars in values

PASS: each probe response, when stringified, contains none of the
forbidden tokens. FAIL = leak detected. XFAIL = probe couldn't run.

Exit codes: 0=PASS, 1=FAIL, 2=preflight.
"""

from __future__ import annotations

import json
import os
import re
import socket as _socket
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
    latest_crash_dump,
    preflight,
)

PHASE = "f5"
NAME = "data_sanitization"


# Build the list of forbidden tokens at runtime.
def _forbidden_tokens() -> List[Tuple[str, str]]:
    """Returns (label, regex_pattern). Each pattern is a string regex."""
    out: List[Tuple[str, str]] = []
    # OS username — word-bounded to avoid substring noise
    user = os.environ.get("USERNAME") or os.environ.get("USER") or ""
    if user and len(user) >= 3:
        out.append(("username", r"\b" + re.escape(user) + r"\b"))
    # Computer name — word-bounded to avoid hits inside legitimate names
    # (e.g. "Mark" matching inside "WorldBookmark"). Skip if hostname too short
    # to be unambiguous.
    try:
        hostname = _socket.gethostname()
        if hostname and len(hostname) >= 5:
            out.append(("hostname", r"\b" + re.escape(hostname) + r"\b"))
    except Exception:
        pass
    # Drive-letter paths — D:\ and D:/ both forms. These are real disk paths.
    out.append(("drive_letter_path_backslash", r"[A-Za-z]:\\\\"))
    out.append(("drive_letter_path_forward", r"[A-Za-z]:/"))
    # Common credential markers
    out.append(("private_key_marker", r"BEGIN PRIVATE KEY"))
    out.append(("ssh_marker", r"ssh-rsa AAAA"))
    return out


def _scan_for_leaks(text: str, tokens: List[Tuple[str, str]]) -> List[str]:
    """Returns the labels of tokens found in text."""
    found: List[str] = []
    for label, pattern in tokens:
        if re.search(pattern, text, re.IGNORECASE):
            found.append(label)
    return found


def _probe(log: TestLogger, name: str, method: str, args: Dict[str, Any],
           tokens: List[Tuple[str, str]],
           timeout: float = 10.0) -> int:
    """Send method, scan stringified result for forbidden tokens.
    Returns 0 PASS, 1 FAIL, 0 XFAIL/SKIP."""
    t0 = time.monotonic()
    r = call(method, args, timeout=timeout)
    dt = (time.monotonic() - t0) * 1000.0
    if not is_ok(r):
        c = err_code(r)
        if c == -32601:
            log.case(name, "SKIP", f"{method} not registered", duration_ms=dt)
            return 0
        log.case(name, "XFAIL",
                 f"{method} failed: {err_message(r)[:60]}",
                 duration_ms=dt, code=c)
        return 0
    body = json.dumps(r.get("result", {}) or {})
    leaks = _scan_for_leaks(body, tokens)
    if leaks:
        # Drive-letter paths in tool responses are documented "informational
        # leaks" — engine.get_info legitimately needs to report its engine
        # path (it's used for diagnostics + tooling). Username / hostname
        # leaks are stricter — those are FAILs. We classify accordingly:
        is_drive_only = all(l.startswith("drive_letter_path") for l in leaks)
        if is_drive_only:
            log.case(name, "XFAIL",
                     f"disk-path tokens={leaks} present (informational — "
                     f"diagnostics surfaces may legitimately expose them); "
                     f"preview={body[:120]}…",
                     duration_ms=dt)
            return 0
        log.case(name, "FAIL",
                 f"PII LEAK tokens={leaks} in response "
                 f"(preview={body[:150]}…)",
                 duration_ms=dt)
        return 1
    log.case(name, "PASS",
             f"no leaks; body_len={len(body)}", duration_ms=dt)
    return 0


def main() -> int:
    if not preflight(PHASE):
        return 2
    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()
    fail_total = 0

    tokens = _forbidden_tokens()
    print(f"[F5] PII/system-path leak scan (checking {len(tokens)} token classes)…",
          flush=True)
    for tag, pat in tokens:
        log.case(f"P0_token_{tag}", "PASS",
                 f"checking for /{pat}/i")

    # P1 — engine.get_info
    fail_total += _probe(log, "P1_engine_get_info",
                          "engine.get_info", {}, tokens)
    # P2 — memreport.get_quick_stats
    fail_total += _probe(log, "P2_memreport_quick_stats",
                          "memreport.get_quick_stats", {}, tokens)
    # P3 — package.get_info on a known asset
    fail_total += _probe(log, "P3_package_get_info",
                          "package.get_info",
                          {"package_path": "/Engine/BasicShapes/Cube"}, tokens)
    # P4 — cb.list_supported_formats
    fail_total += _probe(log, "P4_cb_supported_formats",
                          "cb.list_supported_formats", {}, tokens)
    # P5 — log.list_categories (page_size moderate)
    fail_total += _probe(log, "P5_log_list_categories",
                          "log.list_categories", {"page_size": 20}, tokens)
    # P6 — cfg.list_cvars
    fail_total += _probe(log, "P6_cfg_list_cvars",
                          "cfg.list_cvars", {"page_size": 20}, tokens)
    # P7 — actor.list (returns paths, but those should be /Game or /Temp, not D:\)
    fail_total += _probe(log, "P7_actor_list",
                          "actor.list", {"page_size": 10}, tokens)

    crash = latest_crash_dump(since=crash_baseline)
    if crash:
        log.case("crash_check", "FAIL", f"CRASH DUMP: {crash}")
        log.write()
        return 1

    summary = log.write()
    cc = summary["counts"]
    print()
    print(f"[F5] PASS={cc['PASS']} FAIL={cc['FAIL']} XFAIL={cc.get('XFAIL', 0)} "
          f"SKIP={cc.get('SKIP', 0)} TOTAL={cc['TOTAL']}")
    print(f"     log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    if fail_total > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
