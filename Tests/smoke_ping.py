#!/usr/bin/env python3
"""End-to-end smoke test for the MCP bridge dispatch pipeline (Phase 1 Day 3).

Prerequisites:
  * Unreal Editor must be running with the UnrealMCPBridge plugin loaded.
  * Look for log line: ``LogMCP: MCP bridge listening on 127.0.0.1:30020``.
  * Python tools must be registered — look for: ``LogMCP: FMCPPythonBootstrap: sys.path
    injected + MCPTools.tools imported``.

What this script does (each sub-test opens a fresh socket, sends 1 frame, reads 1 line):

  1. ``editor.ping``               — Python-served, returns ``{"pong": true, "editor_version": "..."}``
  2. ``editor.engine_version``     — Python-served, returns ``{"version": "..."}``
  3. ``editor.project_name``       — Python-served, returns ``{"name": "FatumGame"}`` (assuming this project)
  4. ``kind=exec_python``          — evaluates expression ``1+2``; result.repr should be ``"3"``
  5. ``unknown.tool``              — expects structured error ``code=-32601`` (method not found in either
                                     C++ or Python registry)

Prints ``[SMOKE_PING] PASS`` (exit 0) on all-pass, otherwise ``[SMOKE_PING] FAIL ...`` (exit 1) at
the first failing sub-test.

Usage:
  python smoke_ping.py [--host HOST] [--port PORT]
"""

from __future__ import annotations

import argparse
import json
import socket
import sys
import time
from typing import Optional


DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 30020
READ_TIMEOUT_SEC = 5.0


def send_and_recv_line(host: str, port: int, request_obj: dict) -> Optional[dict]:
    """Open a fresh TCP socket, send one newline-framed JSON request, read one response line.

    Returns the parsed response dict, or None on timeout / connection error.
    """

    with socket.create_connection((host, port), timeout=READ_TIMEOUT_SEC) as sock:
        sock.settimeout(READ_TIMEOUT_SEC)
        payload = (json.dumps(request_obj, separators=(",", ":")) + "\n").encode("utf-8")
        sock.sendall(payload)

        # Read until we get a newline. Bridge sends exactly one response line per request.
        buf = bytearray()
        deadline = time.monotonic() + READ_TIMEOUT_SEC
        while True:
            if time.monotonic() > deadline:
                return None
            try:
                chunk = sock.recv(64 * 1024)
            except socket.timeout:
                return None
            if not chunk:
                # Peer closed before sending newline.
                break
            buf.extend(chunk)
            newline_idx = buf.find(b"\n")
            if newline_idx >= 0:
                line = bytes(buf[:newline_idx])
                return json.loads(line.decode("utf-8"))

        return None


def fail(reason: str) -> int:
    print(f"[SMOKE_PING] FAIL reason={reason}")
    return 1


def expect_ok(response: Optional[dict], expected_id: str, label: str) -> Optional[dict]:
    """Validate that response is a successful (ok=true) envelope with the expected id.

    Returns the inner result dict on success, None (after printing FAIL) on any mismatch.
    """
    if response is None:
        fail(f"{label}: timeout (>{READ_TIMEOUT_SEC}s)")
        return None
    if response.get("id") != expected_id:
        fail(f"{label}: id-mismatch expected={expected_id!r} got={response.get('id')!r}")
        return None
    if response.get("ok") is not True:
        fail(f"{label}: ok-not-true got={response.get('ok')!r} error={response.get('error')!r}")
        return None
    result = response.get("result")
    if not isinstance(result, dict):
        fail(f"{label}: result-not-object got={result!r}")
        return None
    return result


def expect_error(response: Optional[dict], expected_id: str, expected_code: int, label: str) -> bool:
    """Validate that response is a structured error envelope with the expected code/id."""
    if response is None:
        fail(f"{label}: timeout (>{READ_TIMEOUT_SEC}s)")
        return False
    if response.get("id") != expected_id:
        fail(f"{label}: id-mismatch expected={expected_id!r} got={response.get('id')!r}")
        return False
    if response.get("ok") is not False:
        fail(f"{label}: ok-not-false got={response.get('ok')!r} result={response.get('result')!r}")
        return False
    error = response.get("error")
    if not isinstance(error, dict) or error.get("code") != expected_code:
        fail(f"{label}: wrong-error-code expected={expected_code} got={error!r}")
        return False
    return True


def run_subtest_call(host: str, port: int, label: str, request_id: str, method: str,
                     args: Optional[dict] = None) -> Optional[dict]:
    """Send a call_function request and return its result dict on success (or None on fail)."""
    request = {
        "id": request_id,
        "kind": "call_function",
        "method": method,
        "args": args or {},
    }
    try:
        response = send_and_recv_line(host, port, request)
    except (ConnectionRefusedError, OSError) as exc:
        fail(f"{label}: connect-error detail={exc}")
        return None
    except json.JSONDecodeError as exc:
        fail(f"{label}: invalid-json-response detail={exc}")
        return None
    return expect_ok(response, request_id, label)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    args = parser.parse_args()

    print(f"[SMOKE_PING] connecting to {args.host}:{args.port} ...")

    # ─── Sub-test 1: editor.ping ────────────────────────────────────────────────────────────────
    result = run_subtest_call(args.host, args.port, "1/editor.ping", "smoke-1", "editor.ping")
    if result is None:
        return 1
    if result.get("pong") is not True:
        return fail(f"1/editor.ping: pong-not-true result={result!r}")
    if not isinstance(result.get("editor_version"), str) or not result["editor_version"]:
        return fail(f"1/editor.ping: missing editor_version string result={result!r}")
    print(f"[SMOKE_PING]   1/editor.ping OK (editor_version={result['editor_version']!r})")

    # ─── Sub-test 2: editor.engine_version ──────────────────────────────────────────────────────
    result = run_subtest_call(args.host, args.port, "2/engine_version", "smoke-2", "editor.engine_version")
    if result is None:
        return 1
    if not isinstance(result.get("version"), str) or not result["version"]:
        return fail(f"2/editor.engine_version: missing version string result={result!r}")
    print(f"[SMOKE_PING]   2/editor.engine_version OK (version={result['version']!r})")

    # ─── Sub-test 3: editor.project_name ────────────────────────────────────────────────────────
    result = run_subtest_call(args.host, args.port, "3/project_name", "smoke-3", "editor.project_name")
    if result is None:
        return 1
    if not isinstance(result.get("name"), str) or not result["name"]:
        return fail(f"3/editor.project_name: missing name string result={result!r}")
    print(f"[SMOKE_PING]   3/editor.project_name OK (name={result['name']!r})")

    # ─── Sub-test 4: kind=exec_python evaluating 1+2 ────────────────────────────────────────────
    request_id = "smoke-4"
    request = {
        "id": request_id,
        "kind": "exec_python",
        "args": {"expression": "1 + 2"},
    }
    try:
        response = send_and_recv_line(args.host, args.port, request)
    except (ConnectionRefusedError, OSError, json.JSONDecodeError) as exc:
        return fail(f"4/exec_python: io-error detail={exc}")
    result = expect_ok(response, request_id, "4/exec_python")
    if result is None:
        return 1
    if result.get("repr") != "3":
        return fail(f"4/exec_python: expected repr '3' got result={result!r}")
    print(f"[SMOKE_PING]   4/exec_python(1+2) OK (repr={result['repr']!r})")

    # ─── Sub-test 5: unknown method must surface as -32601 ──────────────────────────────────────
    request_id = "smoke-5"
    request = {
        "id": request_id,
        "kind": "call_function",
        "method": "unknown.tool.that.should.not.exist",
        "args": {},
    }
    try:
        response = send_and_recv_line(args.host, args.port, request)
    except (ConnectionRefusedError, OSError, json.JSONDecodeError) as exc:
        return fail(f"5/unknown-tool: io-error detail={exc}")
    if not expect_error(response, request_id, -32601, "5/unknown-tool"):
        return 1
    print(f"[SMOKE_PING]   5/unknown-tool OK (error.code=-32601 as expected)")

    print(f"[SMOKE_PING] PASS — all 5 sub-tests succeeded")
    return 0


if __name__ == "__main__":
    sys.exit(main())
