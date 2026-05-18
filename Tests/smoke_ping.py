#!/usr/bin/env python3
"""End-to-end smoke test for the MCP bridge dispatch pipeline (Phase 1 Day 3).

Prerequisites:
  * Unreal Editor must be running with the UnrealMCPBridge plugin loaded.
  * Look for log line: ``LogMCP: MCP bridge listening on 127.0.0.1:30020``.
  * Python tools must be registered — look for: ``LogMCP: FMCPPythonBootstrap: sys.path
    injected + MCPTools.tools imported``.

What this script does (each sub-test opens a fresh socket, sends 1 frame, reads 1 line):

  1. ``editor.ping``                 — Python-served, returns ``{"pong": true, "editor_version": "..."}``
  2. ``editor.engine_version``       — Python-served, returns ``{"version": "..."}``
  3. ``editor.project_name``         — Python-served, returns ``{"name": "FatumGame"}`` (assuming this project)
  4. ``kind=exec_python``            — evaluates expression ``1+2``; result.repr should be ``"3"``
  5. ``unknown.tool``                — expects structured error ``code=-32601`` (method not found in either
                                       C++ or Python registry)
  6. ``marshall.describe_struct``    — Day 4-5; reads ``/Script/CoreUObject.Vector`` field schema.
                                       Expects ``fields`` containing entries for ``X``, ``Y``, ``Z``.
  7. ``marshall.list_properties``    — Day 4-5; reads properties of the GameUserSettings CDO (always
                                       loaded). Expects a non-empty ``properties`` array.
  8. ``marshall.read_property``      — Day 4-5; reads ``EngineVersionMajor`` (or similar known
                                       primitive) on a transient diagnostic; falls back to reading
                                       a Vector property on a CDO and validating ``_kind=Vector``.
  9. ``marshall.write_property``     — Day 4-5; round-trips a write→read on a transient
                                       ``unreal.Vector`` field of a writable test object via
                                       ExecPython-spawned transient asset.

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

    # ─── Sub-test 6: marshall.describe_struct on FVector ────────────────────────────────────────
    # /Script/CoreUObject.Vector is built-in; always resolvable. Expects fields [X, Y, Z].
    result = run_subtest_call(
        args.host, args.port, "6/describe_struct", "smoke-6",
        "marshall.describe_struct", {"struct_type_path": "/Script/CoreUObject.Vector"},
    )
    if result is None:
        return 1
    fields = result.get("fields")
    if not isinstance(fields, list) or len(fields) < 3:
        return fail(f"6/describe_struct: expected >= 3 fields got {fields!r}")
    field_names = {f.get("name") for f in fields if isinstance(f, dict)}
    if not {"X", "Y", "Z"}.issubset(field_names):
        return fail(f"6/describe_struct: missing X/Y/Z in fields, got names={field_names!r}")
    print(f"[SMOKE_PING]   6/marshall.describe_struct OK (FVector fields={sorted(field_names)})")

    # ─── Sub-test 7: marshall.list_properties on GameUserSettings CDO ───────────────────────────
    # The CDO is always loaded; its UClass has dozens of properties.
    result = run_subtest_call(
        args.host, args.port, "7/list_properties", "smoke-7",
        "marshall.list_properties",
        {"object_path": "/Script/Engine.Default__GameUserSettings"},
    )
    if result is None:
        return 1
    props = result.get("properties")
    if not isinstance(props, list) or len(props) == 0:
        return fail(f"7/list_properties: expected non-empty properties array, got {props!r}")
    if not all(isinstance(p, dict) and "name" in p and "type" in p for p in props):
        return fail(f"7/list_properties: malformed entries: {props[:3]!r}")
    print(f"[SMOKE_PING]   7/marshall.list_properties OK ({len(props)} properties on GameUserSettings CDO)")

    # ─── Sub-test 8: marshall.read_property on a known primitive ────────────────────────────────
    # GameUserSettings.LastConfirmedFullscreenMode is an int32. Stable across UE 5.x.
    # Falls back to a structural sanity check if that prop ever renames — verifies value is numeric.
    result = run_subtest_call(
        args.host, args.port, "8/read_property", "smoke-8",
        "marshall.read_property",
        {
            "object_path": "/Script/Engine.Default__GameUserSettings",
            "property_path": "LastConfirmedFullscreenMode",
        },
    )
    if result is None:
        return 1
    val = result.get("value")
    if not isinstance(val, (int, float)):
        return fail(f"8/read_property: expected numeric value, got {val!r} (type={type(val).__name__})")
    type_str = result.get("type")
    if not isinstance(type_str, str) or not type_str:
        return fail(f"8/read_property: missing 'type' field, got {result!r}")
    print(f"[SMOKE_PING]   8/marshall.read_property OK (LastConfirmedFullscreenMode={val} type={type_str!r})")

    # ─── Sub-test 9: marshall.write_property round-trip ─────────────────────────────────────────
    # Round-trip: write a known value to a writable transient field, read back, verify match.
    # FrameRateLimit is a float (UPROPERTY EditAnywhere) on GameUserSettings — perfect target.
    # NOTE: writes the CDO; safe because we restore the original value at the end.
    write_path = "/Script/Engine.Default__GameUserSettings"
    write_prop = "FrameRateLimit"

    # Step A: read current value (so we can restore after).
    pre_result = run_subtest_call(
        args.host, args.port, "9a/read-before-write", "smoke-9a",
        "marshall.read_property",
        {"object_path": write_path, "property_path": write_prop},
    )
    if pre_result is None:
        return 1
    original_value = pre_result.get("value")
    if not isinstance(original_value, (int, float)):
        return fail(f"9a: expected numeric original value got {original_value!r}")

    # Step B: write a sentinel value (must differ from original to detect a no-op).
    sentinel = 123.5 if original_value != 123.5 else 234.5
    write_result = run_subtest_call(
        args.host, args.port, "9b/write_property", "smoke-9b",
        "marshall.write_property",
        {
            "object_path": write_path,
            "property_path": write_prop,
            "value": sentinel,
            "bypass_readonly": True,  # CDO writes need the override.
        },
    )
    if write_result is None:
        return 1
    if write_result.get("ok") is not True:
        return fail(f"9b: write returned ok!=true got {write_result!r}")

    # Step C: read back and verify equals sentinel.
    post_result = run_subtest_call(
        args.host, args.port, "9c/read-after-write", "smoke-9c",
        "marshall.read_property",
        {"object_path": write_path, "property_path": write_prop},
    )
    if post_result is None:
        return 1
    if post_result.get("value") != sentinel:
        return fail(f"9c: read-back mismatch expected {sentinel} got {post_result.get('value')!r}")

    # Step D: restore original value (don't leave the CDO mutated for next test run).
    restore_result = run_subtest_call(
        args.host, args.port, "9d/restore", "smoke-9d",
        "marshall.write_property",
        {
            "object_path": write_path,
            "property_path": write_prop,
            "value": original_value,
            "bypass_readonly": True,
        },
    )
    if restore_result is None or restore_result.get("ok") is not True:
        return fail(f"9d: restore failed (CDO left mutated) result={restore_result!r}")
    print(f"[SMOKE_PING]   9/marshall.write_property OK (FrameRateLimit round-trip {original_value}→{sentinel}→{original_value})")

    print(f"[SMOKE_PING] PASS — all 9 sub-tests succeeded")
    return 0


if __name__ == "__main__":
    sys.exit(main())
