#!/usr/bin/env python3
"""Phase 2 Day 0b — Lane B audit harness for the UnrealMCPBridge plugin.

This script is the GATE for which of the 11 candidate Phase 2 Asset-Registry read tools
get to register with ``bThreadSafe=true`` (Lane B, listener-thread inline dispatch) vs.
``bThreadSafe=false`` (Lane A, game-thread Drain queue).

# WHEN TO RUN

Run this AFTER the Phase 2 Lane B tool implementations land (Days 1-4). Until then, the
script will report all 11 tools as ``NOT_REGISTERED`` and exit non-zero. That is the
EXPECTED Day 0 state — the Lane B router infrastructure is in place (FMCPDispatchQueue
``bThreadSafe`` flag + ``DispatchInline()`` + ``FMCPConnection::HandleFrame`` short-circuit),
but the tools themselves don't exist yet.

The script is committed in the Day 0 commit so it is ready and grep-able the moment the
first Lane B handler registers.

# WHAT IT DOES

For each of the 11 candidate Lane B tools, the script:

  1. Fires 1000 ``call_function`` requests in a tight loop (single socket, sequential).
  2. Tracks: successful responses, error responses by code, max round-trip latency,
     connection disconnects (= listener-thread crash signal).
  3. Concurrently (on a background thread) hammers ``tools.list`` (a Lane A C++ tool that
     queues through the game-thread Drain) once per ~50 ms — this puts continuous load
     on the game thread so that any Lane B call that incorrectly falls back to Lane A
     will be visibly contended.

# DECISION RULE per tool

  - ``KEEP`` — ``failed == 0`` AND at least one ``succeeded`` response observed. Tool is
    Lane-B safe at the hot-loop pressure level; register with ``bThreadSafe=true``.
  - ``DEMOTE`` — any non-``NOT_REGISTERED`` failure (other than -32601). Tool tripped an
    assertion, crashed, returned an unexpected error, or timed out. Register with
    ``bThreadSafe=false``.
  - ``NOT_REGISTERED`` — every call returned -32601 (method not found). Tool isn't
    implemented yet. This is the Day 0 baseline state; treat as INDETERMINATE for the
    purposes of the Lane B audit gate. Re-run after implementation lands.

# OUTPUT

  - Markdown table to stdout, one row per tool:
    ``tool_name | attempts | succeeded | failed | max_latency_ms | DECISION``
  - If ``--write-results`` is passed, the same table is written to
    ``Tests/lane_b_audit_results.md`` for check-in next to this script.

# EXIT CODE

  - 0 — every tool reports ``KEEP``. Day 0 audit gate passes; all 11 stay Lane B.
  - non-0 — at least one tool reports ``DEMOTE`` OR ``NOT_REGISTERED``. Update the
    Phase 2 register call list (``AssetRegistryTools::Register``) accordingly. NOT_REGISTERED
    is non-zero because that means audit could not complete — re-run after impl.

# CONCURRENT LOAD CAVEAT

The Day 0 spike-plan says: "Runs each tool's stub during an active editor scenario where
the AR is being mutated: kick off a find_in_blueprints operation in the editor while the
spike is running, AND a force-redirector-resolve, AND a content-browser background
discovery." This script does as much load as it can from the wire (tools.list flood), but
mid-session AR mutations (creating/deleting assets, force-redirector-resolve) cannot be
triggered from a plain TCP client.

For the FULL Day 0b audit per the plan, the operator SHOULD manually trigger these inside
the editor while this script is running:

  1. Right-click a UMaterial / UTexture in the Content Browser → "Find References" (heavy
     AR walk + main-thread blocking).
  2. From the menu: Tools → Find in Blueprints, search for a common identifier (e.g.
     "OnEndFrame"). This forces a full Blueprint AR scan.
  3. Right-click a folder with assets → "Fix Up Redirectors in Folder" (AR mutation).
  4. Content Browser: navigate to ``/All`` and let it discover.

The audit is considered SOUND only if the spike was run alongside those operations. If
none were triggered, this script still validates the absence of obvious races but does
NOT exercise the full mid-mutation pressure.

# USAGE

  python lane_b_spike.py
  python lane_b_spike.py --write-results
  python lane_b_spike.py --host 127.0.0.1 --port 30020 --iterations 1000
"""

from __future__ import annotations

import argparse
import json
import socket
import sys
import threading
import time
from contextlib import contextmanager
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional


DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 30020
DEFAULT_ITERATIONS = 1000

# Read timeout per call. Lane B handlers should round-trip in single-digit milliseconds; a
# 2-second cap leaves headroom for cold-cache AR queries (documented Phase 2 caveat) while
# catching genuine deadlocks.
READ_TIMEOUT_SEC = 2.0

# Method-not-found error code (JSON-RPC). Treated as "tool stub not implemented yet" by
# the decision rule below, NOT as an audit failure.
ERR_METHOD_NOT_FOUND = -32601

# Concurrent load cadence: spam tools.list once every CONCURRENT_LOAD_INTERVAL seconds on a
# background thread to keep the game-thread Drain queue churning while the Lane B tools run.
CONCURRENT_LOAD_INTERVAL = 0.05


# The 11 Lane B tool candidates from Phase 2 §D3 + tool-count-reconciliation table.
# Each entry: (method_name, args_factory) where args_factory builds a minimal valid args dict
# suitable for the hot loop. Stubs MAY return error responses for invalid args during the
# audit — those are tracked as "failed" in the decision rule, which correctly catches a
# tool whose schema is so strict that 1000 calls cannot all succeed.
#
# Args are chosen to be cheap-to-validate and survive a missing FatumGame asset (so the
# script works on a clean project too). If tighter args are needed once tools are real,
# update the args_factory entries below.
LANE_B_CANDIDATES: list[tuple[str, "ArgsFactory"]] = [
    ("asset.exists", lambda i: {"path": "/Game/Maps/Tutorial"}),
    ("asset.metadata", lambda i: {"path": "/Game/Maps/Tutorial"}),
    ("asset.list", lambda i: {
        "filter": {
            "package_paths": ["/Game/MCPTest"],
            "recursive_paths": False,
        },
        "page_size": 5,
    }),
    ("asset.find_references", lambda i: {
        "path": "/Game/Maps/Tutorial",
        "recursive": False,
    }),
    ("asset.find_dependents", lambda i: {
        "path": "/Game/Maps/Tutorial",
    }),
    ("asset.search_by_class", lambda i: {
        "class_path": "/Script/Engine.StaticMesh",
        "package_paths": ["/Game/MCPTest"],
        "recursive_paths": False,
        "page_size": 5,
    }),
    ("asset.search_by_tag", lambda i: {
        "tag_key": "NativeParentClass",
        "tag_value": "",
        "page_size": 5,
    }),
    ("asset.search_by_name", lambda i: {
        "name_pattern": "Test",
        "page_size": 5,
    }),
    ("asset.get_class_hierarchy", lambda i: {"path": "/Game/Maps/Tutorial"}),
    ("asset.get_outermost_package", lambda i: {"path": "/Game/Maps/Tutorial"}),
    ("cb.list_folders", lambda i: {
        "parent_path": "/Game",
        "recursive": False,
    }),
]
# Dedupe in case the candidate list above accidentally re-lists a name; the eventual
# 11-tool roster from §D3 has unique names, but a defensive dedupe avoids double-reporting
# during development as the list churns.
seen: set[str] = set()
_uniq: list[tuple[str, "ArgsFactory"]] = []
for _name, _factory in LANE_B_CANDIDATES:
    if _name in seen:
        continue
    seen.add(_name)
    _uniq.append((_name, _factory))
LANE_B_CANDIDATES = _uniq


# Type alias used above.
ArgsFactory = "callable"  # noqa: pyflakes — purely documentary


# ───────────────────────────────────────────────────────────────────────────────────────
# Per-tool result accounting
# ───────────────────────────────────────────────────────────────────────────────────────


@dataclass
class ToolResult:
    """Aggregated outcome of N hot-loop iterations against one Lane B candidate."""

    method: str
    attempts: int = 0
    succeeded: int = 0
    failed: int = 0
    not_registered: int = 0
    disconnected: int = 0
    max_latency_ms: float = 0.0
    error_codes: dict[int, int] = field(default_factory=dict)

    def record_success(self, latency_ms: float) -> None:
        self.attempts += 1
        self.succeeded += 1
        if latency_ms > self.max_latency_ms:
            self.max_latency_ms = latency_ms

    def record_not_registered(self, latency_ms: float) -> None:
        # -32601 isn't a Lane B failure — it just means the tool hasn't shipped yet. Counted
        # separately so the decision rule can distinguish "real audit failure" from "audit
        # could not run".
        self.attempts += 1
        self.not_registered += 1
        if latency_ms > self.max_latency_ms:
            self.max_latency_ms = latency_ms

    def record_failure(self, latency_ms: float, code: Optional[int]) -> None:
        self.attempts += 1
        self.failed += 1
        if code is not None:
            self.error_codes[code] = self.error_codes.get(code, 0) + 1
        if latency_ms > self.max_latency_ms:
            self.max_latency_ms = latency_ms

    def record_disconnect(self) -> None:
        # Disconnect mid-call is the smoking-gun for a listener-thread crash on a Lane B
        # handler. We bump both attempts and disconnected so the total still reconciles.
        self.attempts += 1
        self.disconnected += 1
        self.failed += 1

    def decide(self) -> str:
        """Apply the Day 0b decision rule. See module docstring."""
        if self.attempts == 0:
            return "NO_DATA"
        if self.succeeded == 0 and self.failed == 0 and self.not_registered == self.attempts:
            return "NOT_REGISTERED"
        if self.failed == 0 and self.succeeded > 0:
            return "KEEP"
        return "DEMOTE"


# ───────────────────────────────────────────────────────────────────────────────────────
# TCP helpers (mirror smoke_ping.py)
# ───────────────────────────────────────────────────────────────────────────────────────


@contextmanager
def open_socket(host: str, port: int):
    """Open a connected, timeout-armed socket. Auto-close on exit."""
    sock = socket.create_connection((host, port), timeout=READ_TIMEOUT_SEC)
    try:
        sock.settimeout(READ_TIMEOUT_SEC)
        yield sock
    finally:
        try:
            sock.close()
        except OSError:
            pass


def send_recv_line(sock: socket.socket, request_obj: dict) -> Optional[dict]:
    """Send one line-framed JSON request on a pre-opened socket, read one response line.

    Returns the parsed response dict on success, None on timeout / EOF / decode error.
    """
    payload = (json.dumps(request_obj, separators=(",", ":")) + "\n").encode("utf-8")
    try:
        sock.sendall(payload)
    except OSError:
        return None

    buf = bytearray()
    deadline = time.monotonic() + READ_TIMEOUT_SEC
    while True:
        if time.monotonic() > deadline:
            return None
        try:
            chunk = sock.recv(64 * 1024)
        except socket.timeout:
            return None
        except OSError:
            return None
        if not chunk:
            # Graceful EOF before newline — peer closed mid-response.
            return None
        buf.extend(chunk)
        newline_idx = buf.find(b"\n")
        if newline_idx >= 0:
            line = bytes(buf[:newline_idx])
            try:
                return json.loads(line.decode("utf-8"))
            except (UnicodeDecodeError, json.JSONDecodeError):
                return None


# ───────────────────────────────────────────────────────────────────────────────────────
# Concurrent load thread (keeps game-thread Drain busy)
# ───────────────────────────────────────────────────────────────────────────────────────


class ConcurrentLoadGenerator:
    """Background thread that fires ``tools.list`` continuously, on a dedicated socket.

    This is the wire-side approximation of "keep the editor's game thread under load while
    the Lane B audit runs". Cannot replace in-editor AR mutations (see module docstring)
    but does ensure the Drain queue is never empty — a Lane B handler that incorrectly
    falls back to Lane A will show inflated latency in this scenario.
    """

    def __init__(self, host: str, port: int) -> None:
        self._host = host
        self._port = port
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, name="LaneBLoadGen", daemon=True)
        self.calls_sent = 0
        self.errors = 0

    def start(self) -> None:
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        # Don't block on thread join — daemon will die with the process; the load gen is
        # advisory only.

    def _run(self) -> None:
        counter = 0
        # Keep a single persistent socket to avoid handshake noise dominating the load.
        try:
            sock = socket.create_connection((self._host, self._port), timeout=READ_TIMEOUT_SEC)
            sock.settimeout(READ_TIMEOUT_SEC)
        except OSError:
            # Bridge isn't up; the main loop will report this clearly.
            self.errors += 1
            return

        try:
            while not self._stop.is_set():
                counter += 1
                req = {"id": f"load-{counter}", "kind": "call_function",
                       "method": "tools.list", "args": {}}
                resp = send_recv_line(sock, req)
                if resp is None:
                    self.errors += 1
                    # Socket likely dead; re-open.
                    try:
                        sock.close()
                    except OSError:
                        pass
                    try:
                        sock = socket.create_connection((self._host, self._port),
                                                        timeout=READ_TIMEOUT_SEC)
                        sock.settimeout(READ_TIMEOUT_SEC)
                    except OSError:
                        return
                    continue
                self.calls_sent += 1
                if self._stop.wait(timeout=CONCURRENT_LOAD_INTERVAL):
                    break
        finally:
            try:
                sock.close()
            except OSError:
                pass


# ───────────────────────────────────────────────────────────────────────────────────────
# Hot loop per tool
# ───────────────────────────────────────────────────────────────────────────────────────


def run_hot_loop(host: str, port: int, method: str, args_factory, iterations: int) -> ToolResult:
    """Fire ``iterations`` calls against ``method`` on a single socket. Return aggregated stats."""

    result = ToolResult(method=method)

    # Single persistent socket — match the production case where a Python tool client
    # holds one connection open and pipelines calls. If the listener thread crashes on a
    # Lane B handler, this is the failure mode that will surface as `disconnected`.
    sock: Optional[socket.socket] = None
    try:
        sock = socket.create_connection((host, port), timeout=READ_TIMEOUT_SEC)
        sock.settimeout(READ_TIMEOUT_SEC)
    except OSError as exc:
        # Bridge isn't reachable. Mark every iteration as disconnected so the operator sees
        # the pattern, but only attempt once — there's no point in iterating against a
        # closed port.
        print(f"  [{method}] connect failed: {exc} — skipping all iterations")
        for _ in range(iterations):
            result.record_disconnect()
        return result

    try:
        for i in range(iterations):
            try:
                args = args_factory(i)
            except Exception as exc:  # pragma: no cover — args_factory bug, fail loudly
                print(f"  [{method}] args_factory raised at i={i}: {exc!r}")
                result.record_failure(0.0, code=None)
                continue

            req = {
                "id": f"laneb-{method}-{i}",
                "kind": "call_function",
                "method": method,
                "args": args,
            }

            t0 = time.perf_counter()
            resp = send_recv_line(sock, req)
            latency_ms = (time.perf_counter() - t0) * 1000.0

            if resp is None:
                # Socket dead or response missing newline — listener-thread crash signal.
                result.record_disconnect()
                # Try to re-open so we can continue collecting samples. If re-open fails,
                # bail out cleanly.
                try:
                    sock.close()
                except OSError:
                    pass
                try:
                    sock = socket.create_connection((host, port), timeout=READ_TIMEOUT_SEC)
                    sock.settimeout(READ_TIMEOUT_SEC)
                except OSError:
                    # Bridge gone; abort remaining iterations.
                    remaining = iterations - (i + 1)
                    for _ in range(remaining):
                        result.record_disconnect()
                    return result
                continue

            ok = resp.get("ok")
            if ok is True:
                result.record_success(latency_ms)
            else:
                err = resp.get("error")
                code = err.get("code") if isinstance(err, dict) else None
                if code == ERR_METHOD_NOT_FOUND:
                    result.record_not_registered(latency_ms)
                else:
                    result.record_failure(latency_ms, code)
    finally:
        if sock is not None:
            try:
                sock.close()
            except OSError:
                pass

    return result


# ───────────────────────────────────────────────────────────────────────────────────────
# Reporting
# ───────────────────────────────────────────────────────────────────────────────────────


def format_markdown_table(results: list[ToolResult]) -> str:
    header = (
        "| tool_name | attempts | succeeded | failed | not_registered | max_latency_ms | DECISION |\n"
        "|---|---|---|---|---|---|---|"
    )
    rows = [header]
    for r in results:
        rows.append(
            f"| `{r.method}` | {r.attempts} | {r.succeeded} | {r.failed} | "
            f"{r.not_registered} | {r.max_latency_ms:.2f} | **{r.decide()}** |"
        )
    return "\n".join(rows)


def format_error_summary(results: list[ToolResult]) -> str:
    """Per-tool error-code distribution for any tool that observed failures."""
    chunks: list[str] = []
    for r in results:
        if r.failed == 0 and r.disconnected == 0:
            continue
        items = ", ".join(f"{code}: {count}" for code, count in sorted(r.error_codes.items()))
        chunks.append(
            f"- `{r.method}` — failed={r.failed} (codes: {items or 'none'}), "
            f"disconnected={r.disconnected}"
        )
    if not chunks:
        return "_No tool reported any failure or disconnect._"
    return "**Error breakdown:**\n" + "\n".join(chunks)


def write_results_file(path: Path, table_md: str, error_summary: str, load_calls: int) -> None:
    body = (
        "# Lane B audit results — Phase 2 Day 0b\n\n"
        f"_Generated by `Tests/lane_b_spike.py` at {time.strftime('%Y-%m-%d %H:%M:%S')}._\n\n"
        f"Concurrent-load thread sent {load_calls} `tools.list` calls during the audit.\n\n"
        f"{table_md}\n\n"
        f"{error_summary}\n\n"
        "## Decision rule\n\n"
        "- **KEEP** — `failed == 0` and at least one success. Register the tool with "
        "`bThreadSafe=true` (Lane B).\n"
        "- **DEMOTE** — any non-NOT_REGISTERED failure. Register with `bThreadSafe=false` "
        "(Lane A).\n"
        "- **NOT_REGISTERED** — tool implementation not yet shipped. Re-run after the Phase 2 "
        "tools land.\n"
    )
    path.write_text(body, encoding="utf-8")


# ───────────────────────────────────────────────────────────────────────────────────────
# Entrypoint
# ───────────────────────────────────────────────────────────────────────────────────────


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--iterations", type=int, default=DEFAULT_ITERATIONS,
                        help="Iterations per tool (default: 1000)")
    parser.add_argument("--write-results", action="store_true",
                        help="Write results to Tests/lane_b_audit_results.md")
    parser.add_argument("--no-concurrent-load", action="store_true",
                        help="Disable background tools.list load generator (debug only)")
    args = parser.parse_args()

    print(f"[LANE_B_SPIKE] target={args.host}:{args.port} iterations/tool={args.iterations} "
          f"tools={len(LANE_B_CANDIDATES)}")
    print(f"[LANE_B_SPIKE] NOTE: Run this AFTER Phase 2 Lane B tool implementations land.")
    print(f"[LANE_B_SPIKE]       Until then expect every tool to report NOT_REGISTERED.")

    load_gen: Optional[ConcurrentLoadGenerator] = None
    if not args.no_concurrent_load:
        load_gen = ConcurrentLoadGenerator(args.host, args.port)
        load_gen.start()
        print(f"[LANE_B_SPIKE] concurrent load thread started ({CONCURRENT_LOAD_INTERVAL*1000:.0f}ms cadence)")

    results: list[ToolResult] = []
    overall_t0 = time.monotonic()

    for method, args_factory in LANE_B_CANDIDATES:
        print(f"[LANE_B_SPIKE]   running {method} × {args.iterations} ...", flush=True)
        t0 = time.monotonic()
        result = run_hot_loop(args.host, args.port, method, args_factory, args.iterations)
        elapsed_s = time.monotonic() - t0
        print(
            f"[LANE_B_SPIKE]     done in {elapsed_s:.2f}s — "
            f"ok={result.succeeded} fail={result.failed} nr={result.not_registered} "
            f"disc={result.disconnected} max_ms={result.max_latency_ms:.2f}"
        )
        results.append(result)

    if load_gen is not None:
        load_gen.stop()
        load_calls = load_gen.calls_sent
        load_errors = load_gen.errors
        print(f"[LANE_B_SPIKE] concurrent load: {load_calls} calls, {load_errors} errors")
    else:
        load_calls = 0

    total_elapsed = time.monotonic() - overall_t0
    print(f"[LANE_B_SPIKE] total elapsed: {total_elapsed:.2f}s\n")

    table_md = format_markdown_table(results)
    print(table_md)
    print()
    err_summary = format_error_summary(results)
    print(err_summary)

    if args.write_results:
        out_path = Path(__file__).resolve().parent / "lane_b_audit_results.md"
        write_results_file(out_path, table_md, err_summary, load_calls)
        print(f"\n[LANE_B_SPIKE] wrote {out_path}")

    # Exit code: 0 only if every tool is KEEP. NOT_REGISTERED, DEMOTE, NO_DATA all fail.
    decisions = [r.decide() for r in results]
    not_keep = [d for d in decisions if d != "KEEP"]
    if not not_keep:
        print(f"\n[LANE_B_SPIKE] PASS — all {len(results)} tools audited as KEEP.")
        return 0

    not_registered_count = sum(1 for d in decisions if d == "NOT_REGISTERED")
    demote_count = sum(1 for d in decisions if d == "DEMOTE")
    no_data_count = sum(1 for d in decisions if d == "NO_DATA")
    print(
        f"\n[LANE_B_SPIKE] FAIL — {len(not_keep)}/{len(results)} tools not KEEP "
        f"(NOT_REGISTERED={not_registered_count}, DEMOTE={demote_count}, NO_DATA={no_data_count})."
    )
    if not_registered_count == len(results):
        print(
            "[LANE_B_SPIKE]   All tools are NOT_REGISTERED — this is the EXPECTED Day 0 state. "
            "Re-run after Phase 2 tool implementations land."
        )
    return 1


if __name__ == "__main__":
    sys.exit(main())
