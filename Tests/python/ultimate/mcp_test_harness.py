#!/usr/bin/env python3
"""mcp_test_harness — foundation for ULTIMATE test suite.

Provides:
- call() / call_async()                 single-shot JSON-RPC request/response
- Connection                            keep-alive context manager
- ConnectionPool                        N parallel sockets
- health() / snapshot() / force_gc()    editor liveness + memory probes
- discover_methods()                    parse RegisterTool sites in plugin source
- discover_required_args()              parse RequireXxxField callsites
- paginate()                            page_token walker
- send_raw_bytes() / send_split()       transport-layer fuzz primitives
- editor_alive_check()                  before+after liveness assertion
- latest_crash_dump()                   scrape Saved/Crashes/ for new dumps
- TestLogger                            per-phase Markdown + JSON log writer

All phase scripts (phase_a1..phase_j2) import from this module.

Usage:
    from mcp_test_harness import call, health, TestLogger, discover_methods
    log = TestLogger("a1", "tool_inventory")
    for m in discover_methods():
        r = call(m, {})
        log.case(m, "PASS" if r.get("ok") or r.get("error") else "FAIL", str(r)[:80])
    log.write()
"""

from __future__ import annotations

import asyncio
import json
import os
import random
import re
import socket
import string
import sys
import threading
import time
import traceback
from contextlib import contextmanager
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Dict, Iterator, List, Optional, Sequence, Tuple

# ============================================================================
# Constants / configuration
# ============================================================================

HOST = "127.0.0.1"
PORT = 30020
DEFAULT_TIMEOUT_S = 30.0
DEFAULT_RECV_BUF = 65536
LOG_ROOT = Path("D:/tmp/ws3_stress/test_logs")
PLUGIN_SRC_ROOT = Path("D:/Unreal Engine Projects/FatumGame/Plugins/UnrealMCPBridge/Source")
PROJECT_CRASH_DIR = Path("D:/Unreal Engine Projects/FatumGame/Saved/Crashes")

# Make stdout sane on Windows console
try:
    sys.stdout.reconfigure(encoding="utf-8")  # type: ignore[attr-defined]
except Exception:
    pass

# ============================================================================
# Transport: low-level frame I/O
# ============================================================================

def _send_frame(sock: socket.socket, payload: bytes) -> None:
    """Send a single line-delimited JSON frame."""
    if not payload.endswith(b"\n"):
        payload = payload + b"\n"
    sock.sendall(payload)


def _recv_frame(sock: socket.socket, timeout_s: float = DEFAULT_TIMEOUT_S) -> bytes:
    """Read until first newline; returns frame WITHOUT trailing newline."""
    sock.settimeout(timeout_s)
    buf = bytearray()
    deadline = time.monotonic() + timeout_s
    while True:
        if time.monotonic() > deadline:
            raise socket.timeout(f"recv_frame timeout after {timeout_s}s")
        remaining = max(0.05, deadline - time.monotonic())
        sock.settimeout(remaining)
        try:
            chunk = sock.recv(DEFAULT_RECV_BUF)
        except socket.timeout:
            continue
        if not chunk:
            # Peer closed mid-frame — return what we have (caller decides)
            return bytes(buf)
        buf.extend(chunk)
        nl = buf.find(b"\n")
        if nl >= 0:
            return bytes(buf[:nl])

# ============================================================================
# Connection — keep-alive context manager
# ============================================================================

class Connection:
    """Context manager around a single TCP socket.

    Multiple call_keepalive() invocations reuse the same socket — useful for
    measuring per-connection state, ID collision tests, and saving connect cost
    in tight loops.
    """

    def __init__(self, host: str = HOST, port: int = PORT, connect_timeout: float = 5.0):
        self.host = host
        self.port = port
        self.connect_timeout = connect_timeout
        self.sock: Optional[socket.socket] = None

    def __enter__(self) -> "Connection":
        self.sock = socket.create_connection((self.host, self.port), timeout=self.connect_timeout)
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        try:
            if self.sock is not None:
                self.sock.close()
        finally:
            self.sock = None

    def call_keepalive(
        self,
        method: str,
        args: Optional[Dict[str, Any]] = None,
        timeout: float = DEFAULT_TIMEOUT_S,
        request_id: str = "x",
        kind: str = "call_function",
    ) -> Dict[str, Any]:
        """Send + recv one request on the held socket."""
        if self.sock is None:
            raise RuntimeError("Connection not entered")
        frame = json.dumps({
            "id": request_id,
            "kind": kind,
            "method": method,
            "args": args or {},
        }).encode("utf-8")
        _send_frame(self.sock, frame)
        resp = _recv_frame(self.sock, timeout)
        if not resp:
            return {"_err": "empty_response", "_no_connect": False}
        try:
            return json.loads(resp.decode("utf-8"))
        except Exception as e:
            return {"_err": "json_decode_failure", "_raw": resp[:200].decode("utf-8", "replace"), "_exc": str(e)}

# ============================================================================
# call() — top-level single-shot helper
# ============================================================================

def call(
    method: str,
    args: Optional[Dict[str, Any]] = None,
    timeout: float = DEFAULT_TIMEOUT_S,
    request_id: str = "x",
    kind: str = "call_function",
    retry: int = 0,
) -> Dict[str, Any]:
    """Single-shot request. Always returns a dict.

    Success/Bridge-error: returns parsed response.
    Transport failure:    returns {"_err": <reason>, ...}.
    """
    last_err: Dict[str, Any] = {}
    for attempt in range(retry + 1):
        try:
            with Connection() as conn:
                return conn.call_keepalive(method, args, timeout, request_id, kind)
        except (ConnectionRefusedError, OSError) as e:
            last_err = {"_err": "no_connect", "_attempt": attempt, "_exc": str(e)}
            if attempt < retry:
                time.sleep(0.5 * (attempt + 1))
                continue
            return last_err
        except socket.timeout as e:
            last_err = {"_err": "timeout", "_attempt": attempt, "_exc": str(e)}
            if attempt < retry:
                time.sleep(0.5 * (attempt + 1))
                continue
            return last_err
        except Exception as e:
            return {"_err": "exception", "_exc": str(e), "_tb": traceback.format_exc()[:500]}
    return last_err

# ============================================================================
# Async call (for concurrency phases C, E)
# ============================================================================

async def call_async(
    method: str,
    args: Optional[Dict[str, Any]] = None,
    timeout: float = DEFAULT_TIMEOUT_S,
    request_id: str = "x",
) -> Dict[str, Any]:
    """asyncio-friendly variant."""
    loop = asyncio.get_event_loop()
    return await loop.run_in_executor(None, lambda: call(method, args, timeout, request_id))

# ============================================================================
# Connection pool — for C-class concurrency
# ============================================================================

class ConnectionPool:
    """Pool of N persistent Connection objects.

    Not thread-safe to share a single Connection across threads — get() returns
    an exclusive lease (callers must release()). Better: use `with pool.lease():`.
    """

    def __init__(self, size: int):
        self.size = size
        self._pool: List[Connection] = []
        self._lock = threading.Lock()
        self._available = threading.Semaphore(size)

    def __enter__(self) -> "ConnectionPool":
        for _ in range(self.size):
            c = Connection()
            c.__enter__()
            self._pool.append(c)
        return self

    def __exit__(self, *a) -> None:
        for c in self._pool:
            try:
                c.__exit__(*a)
            except Exception:
                pass
        self._pool.clear()

    @contextmanager
    def lease(self) -> Iterator[Connection]:
        self._available.acquire()
        with self._lock:
            conn = self._pool.pop()
        try:
            yield conn
        finally:
            with self._lock:
                self._pool.append(conn)
            self._available.release()

# ============================================================================
# Health probes
# ============================================================================

def health(timeout: float = 5.0) -> bool:
    """Returns True iff editor responds to memreport.get_quick_stats with ok=true."""
    r = call("memreport.get_quick_stats", {}, timeout=timeout)
    return r.get("ok") is True


def snapshot() -> Dict[str, Any]:
    """Returns a memory + UObject snapshot, or {} on failure."""
    r = call("memreport.get_quick_stats", {}, timeout=5.0)
    if not r.get("ok"):
        return {}
    res = r.get("result", {}) or {}
    return {
        "used_physical_mb": res.get("used_physical_mb", 0.0),
        "peak_used_physical_mb": res.get("peak_used_physical_mb", 0.0),
        "available_physical_mb": res.get("available_physical_mb", 0.0),
        "used_virtual_mb": res.get("used_virtual_mb", 0.0),
        "live_uobject_slots": res.get("live_uobject_slots", 0),
        "seconds_since_last_gc": res.get("seconds_since_last_gc", 0),
    }


def force_gc(settle_s: float = 2.0) -> Dict[str, Any]:
    """Triggers gc, sleeps, returns (before, after, delta) snapshot dicts."""
    before = snapshot()
    r = call("engine.gc_collect", {}, timeout=15.0)
    time.sleep(settle_s)
    after = snapshot()
    delta = {
        k: (after.get(k, 0) - before.get(k, 0))
        for k in ("used_physical_mb", "live_uobject_slots")
    }
    return {"gc_ok": r.get("ok") is True, "before": before, "after": after, "delta": delta}


@contextmanager
def editor_alive_check(label: str = ""):
    """Asserts editor alive before and after the wrapped block.

    Raises AssertionError on death — callers may catch to record FAIL.
    """
    if not health():
        raise AssertionError(f"editor dead BEFORE {label}")
    try:
        yield
    finally:
        if not health():
            raise AssertionError(f"editor dead AFTER {label}")

# ============================================================================
# Crash dump scraping
# ============================================================================

def latest_crash_dump(crash_dir: Path = PROJECT_CRASH_DIR, since: Optional[float] = None) -> Optional[Path]:
    """Return path of newest crash dump folder/file, or None.

    If `since` (unix epoch) supplied, only returns dumps mtime > since.
    """
    if not crash_dir.exists():
        return None
    candidates: List[Tuple[float, Path]] = []
    for entry in crash_dir.iterdir():
        try:
            m = entry.stat().st_mtime
        except OSError:
            continue
        if since is not None and m <= since:
            continue
        candidates.append((m, entry))
    if not candidates:
        return None
    candidates.sort(key=lambda x: x[0], reverse=True)
    return candidates[0][1]

# ============================================================================
# Method discovery — parse plugin source
# ============================================================================

# Patterns: `RegisterTool(TEXT("namespace.method"), &Tool_X, /*comment*/ true|false);`
# Also handles wrapped names spanning lines, alternate registration variants.
_RE_REGISTER_TOOL = re.compile(
    r'RegisterTool\(\s*TEXT\("([a-zA-Z0-9_.]+)"\)\s*,\s*&([A-Za-z0-9_]+)\s*,\s*(?:/\*[^*]*\*/\s*)?(true|false)',
    re.MULTILINE,
)

# Some registration helpers vary by surface (esp. older ones, e.g. ContentBrowserTools)
_RE_REGISTER_HANDLER = re.compile(
    r'RegisterHandler\(\s*TEXT\("([a-zA-Z0-9_.]+)"\)\s*,\s*([A-Za-z0-9_:&]+)\s*,\s*(true|false)',
    re.MULTILINE,
)


def discover_methods(src_root: Path = PLUGIN_SRC_ROOT) -> List[Dict[str, Any]]:
    """Walk source tree, return list of {name, handler, thread_safe, file}.

    Deduplicates by name (last registration wins).
    Sorted by name.
    """
    found: Dict[str, Dict[str, Any]] = {}
    for root, _dirs, files in os.walk(src_root):
        for fn in files:
            if not fn.endswith(".cpp"):
                continue
            p = Path(root) / fn
            try:
                text = p.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            for regex in (_RE_REGISTER_TOOL, _RE_REGISTER_HANDLER):
                for m in regex.finditer(text):
                    name, handler, thread_safe = m.group(1), m.group(2), m.group(3)
                    found[name] = {
                        "name": name,
                        "handler": handler,
                        "thread_safe": thread_safe.lower() == "true",
                        "file": str(p.relative_to(src_root)).replace("\\", "/"),
                    }
    return sorted(found.values(), key=lambda d: d["name"])


def discover_methods_live() -> List[str]:
    """Query tools.list for canonical registered C++ handler names.

    Returns sorted list of method names (strings).
    Returns [] on failure (caller should fall back to discover_methods()).
    """
    r = call("tools.list", {}, timeout=10.0)
    if not r.get("ok"):
        return []
    res = r.get("result", {}) or {}
    methods = res.get("cpp_handlers") or res.get("tools") or res.get("methods") or []
    # Some return strings, some return objects.
    out: List[str] = []
    for m in methods:
        if isinstance(m, str):
            out.append(m)
        elif isinstance(m, dict):
            name = m.get("name") or m.get("method")
            if name:
                out.append(str(name))
    return sorted(set(out))


def discover_methods_authoritative() -> List[Dict[str, Any]]:
    """Cross-reference: live tools.list ∪ source parse, with thread-safe flag merged.

    Returns list of {name, handler?, thread_safe?, file?, source: 'live'|'src'|'both'}.
    """
    src = {m["name"]: m for m in discover_methods()}
    live = discover_methods_live()
    out: Dict[str, Dict[str, Any]] = {}
    for name in live:
        if name in src:
            d = dict(src[name])
            d["source"] = "both"
            out[name] = d
        else:
            out[name] = {"name": name, "handler": None, "thread_safe": None, "file": None, "source": "live"}
    for name, info in src.items():
        if name not in out:
            d = dict(info)
            d["source"] = "src"
            out[name] = d
    return sorted(out.values(), key=lambda d: d["name"])

# ============================================================================
# Required-arg discovery — parse RequireXxxField sites
# ============================================================================

# Matches: FMCPToolHelpers::RequireStringField(Req, TEXT("name"), Out, Err)
# Also bare `RequireStringField(...)` (inside FMCPToolHelpers namespace) and `Require<T>Field`.
_RE_REQUIRE_ARG = re.compile(
    r'(?:FMCPToolHelpers::)?Require(String|Number|Bool|Array|Object|Int|UInt|Float|Double)Field\s*\(\s*'
    r'[A-Za-z0-9_]+\s*,\s*TEXT\("([A-Za-z0-9_]+)"\)',
    re.MULTILINE,
)


def discover_required_args(src_root: Path = PLUGIN_SRC_ROOT) -> Dict[str, List[Tuple[str, str]]]:
    """Scan source for RequireXxxField sites. Returns {handler: [(field, type)]}.

    Note: handler is the C++ function name (e.g. "Tool_Spawn") NOT the registered
    method name — caller must correlate via discover_methods()'s 'handler' key.
    """
    by_handler: Dict[str, List[Tuple[str, str]]] = {}
    current_handler: Optional[str] = None
    _RE_FUNC_DEF = re.compile(r'^\s*(?:static\s+)?(?:FMCPResponse\s+|void\s+|FMCPResponse\s*&\s+)?(Tool_[A-Za-z0-9_]+)\s*\(', re.MULTILINE)
    for root, _dirs, files in os.walk(src_root):
        for fn in files:
            if not fn.endswith(".cpp"):
                continue
            p = Path(root) / fn
            try:
                text = p.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            # Walk line by line, tracking current Tool_X function context.
            for line in text.splitlines():
                fm = _RE_FUNC_DEF.match(line)
                if fm:
                    current_handler = fm.group(1)
                    continue
                rm = _RE_REQUIRE_ARG.search(line)
                if rm and current_handler:
                    typ, field = rm.group(1).lower(), rm.group(2)
                    by_handler.setdefault(current_handler, []).append((field, typ))
    # Dedup per handler
    for h, pairs in by_handler.items():
        seen = set()
        deduped = []
        for fn, ty in pairs:
            if fn not in seen:
                seen.add(fn)
                deduped.append((fn, ty))
        by_handler[h] = deduped
    return by_handler

# ============================================================================
# Pagination helper
# ============================================================================

def paginate(
    method: str,
    args: Optional[Dict[str, Any]] = None,
    page_size: int = 100,
    items_key: str = "items",
    cursor_in: str = "page_token",
    cursor_out: str = "next_page_token",
    max_pages: int = 1000,
) -> Iterator[Dict[str, Any]]:
    """Walk a paginated tool from first to last page yielding each item.

    Stops when next_page_token is missing/empty OR max_pages reached.
    Raises if any page returns an error.
    """
    args = dict(args or {})
    args["page_size"] = page_size
    token: Optional[str] = None
    for page_idx in range(max_pages):
        if token is not None:
            args[cursor_in] = token
        r = call(method, args)
        if not r.get("ok"):
            raise RuntimeError(f"{method} page {page_idx} failed: {r}")
        res = r.get("result", {}) or {}
        for item in res.get(items_key, []) or []:
            yield item
        token = res.get(cursor_out) or None
        if not token:
            return
    raise RuntimeError(f"{method} pagination exceeded max_pages={max_pages}")

# ============================================================================
# Raw transport fuzzing (used in B, D phases)
# ============================================================================

def send_raw_bytes(payload: bytes, expect_response: bool = True, timeout: float = 5.0) -> Optional[bytes]:
    """Send arbitrary bytes; optionally wait for a response frame."""
    try:
        with Connection(connect_timeout=timeout) as conn:
            if conn.sock is None:
                return None
            conn.sock.sendall(payload)
            if not expect_response:
                return None
            return _recv_frame(conn.sock, timeout)
    except (socket.timeout, OSError):
        return None


def send_split(obj: Dict[str, Any], chunk_size: int = 10, gap_ms: int = 50, timeout: float = 10.0) -> Dict[str, Any]:
    """Send a JSON frame split into chunks with delays — simulates packet fragmentation."""
    frame = (json.dumps(obj) + "\n").encode("utf-8")
    try:
        with Connection(connect_timeout=5.0) as conn:
            if conn.sock is None:
                return {"_err": "no_connect"}
            for i in range(0, len(frame), chunk_size):
                conn.sock.sendall(frame[i:i + chunk_size])
                time.sleep(gap_ms / 1000.0)
            resp = _recv_frame(conn.sock, timeout)
            if not resp:
                return {"_err": "empty_response"}
            try:
                return json.loads(resp.decode("utf-8"))
            except Exception:
                return {"_err": "json_decode_failure", "_raw": resp[:200].decode("utf-8", "replace")}
    except (socket.timeout, OSError) as e:
        return {"_err": "transport", "_exc": str(e)}

# ============================================================================
# TestLogger — per-phase Markdown + JSON
# ============================================================================

@dataclass
class _Case:
    case_id: str
    status: str          # PASS / FAIL / XFAIL / SKIP
    summary: str
    alive: bool = True
    duration_ms: float = 0.0
    extras: Dict[str, Any] = field(default_factory=dict)


class TestLogger:
    """Writes per-phase Markdown + JSON aggregations.

    Usage:
        log = TestLogger("a1", "tool_inventory")
        for case_id in [...]:
            t0 = time.monotonic()
            ...
            log.case(case_id, "PASS", "ok", duration_ms=(time.monotonic()-t0)*1000)
        log.write()
    """

    def __init__(self, phase_id: str, name: str, log_dir: Path = LOG_ROOT):
        self.phase_id = phase_id
        self.name = name
        self.log_dir = log_dir
        self.log_dir.mkdir(parents=True, exist_ok=True)
        self.md_path = log_dir / f"{phase_id}_{name}.md"
        self.json_path = log_dir / f"{phase_id}_{name}.json"
        self.cases: List[_Case] = []
        self.start_ts = time.time()
        self.initial_snapshot = snapshot()
        self.initial_health = health()
        self.notes: List[str] = []

    def note(self, msg: str) -> None:
        self.notes.append(msg)

    def case(
        self,
        case_id: str,
        status: str,
        summary: str = "",
        alive: bool = True,
        duration_ms: float = 0.0,
        **extras: Any,
    ) -> None:
        if status not in ("PASS", "FAIL", "XFAIL", "SKIP"):
            raise ValueError(f"bad status {status}")
        self.cases.append(_Case(case_id, status, summary, alive, duration_ms, dict(extras)))

    @property
    def counts(self) -> Dict[str, int]:
        out = {"PASS": 0, "FAIL": 0, "XFAIL": 0, "SKIP": 0, "TOTAL": len(self.cases)}
        for c in self.cases:
            out[c.status] = out.get(c.status, 0) + 1
        return out

    def write(self) -> Dict[str, Any]:
        end_ts = time.time()
        final_snapshot = snapshot()
        final_health = health()
        counts = self.counts

        # Markdown
        lines: List[str] = []
        lines.append(f"# Phase {self.phase_id.upper()} — {self.name}")
        lines.append("")
        lines.append(f"- Start: {time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(self.start_ts))}")
        lines.append(f"- Duration: {end_ts - self.start_ts:.2f}s")
        lines.append(f"- Initial health: {self.initial_health}")
        lines.append(f"- Final health:   {final_health}")
        lines.append(f"- UObject delta:  {final_snapshot.get('live_uobject_slots', 0) - self.initial_snapshot.get('live_uobject_slots', 0)}")
        lines.append(f"- Memory delta:   {final_snapshot.get('used_physical_mb', 0) - self.initial_snapshot.get('used_physical_mb', 0):.1f} MB")
        lines.append("")
        lines.append(f"## Summary")
        lines.append("")
        lines.append(f"- PASS={counts['PASS']}  FAIL={counts['FAIL']}  XFAIL={counts.get('XFAIL', 0)}  SKIP={counts.get('SKIP', 0)}  TOTAL={counts['TOTAL']}")
        if self.notes:
            lines.append("")
            lines.append(f"## Notes")
            lines.append("")
            for n in self.notes:
                lines.append(f"- {n}")
        lines.append("")
        lines.append("## Cases")
        lines.append("")
        for c in self.cases:
            extras = ""
            if c.extras:
                extras = " " + " ".join(f"{k}={v}" for k, v in c.extras.items())
            lines.append(f"- [{c.status}] `{c.case_id}` ({c.duration_ms:.1f}ms alive={c.alive}){extras}: {c.summary}")
        self.md_path.write_text("\n".join(lines), encoding="utf-8")

        # JSON
        summary_obj = {
            "phase_id": self.phase_id,
            "name": self.name,
            "start_ts": self.start_ts,
            "end_ts": end_ts,
            "duration_s": end_ts - self.start_ts,
            "initial_health": self.initial_health,
            "final_health": final_health,
            "initial_snapshot": self.initial_snapshot,
            "final_snapshot": final_snapshot,
            "counts": counts,
            "notes": self.notes,
            "cases": [
                {
                    "case_id": c.case_id,
                    "status": c.status,
                    "summary": c.summary,
                    "alive": c.alive,
                    "duration_ms": c.duration_ms,
                    "extras": c.extras,
                }
                for c in self.cases
            ],
        }
        self.json_path.write_text(json.dumps(summary_obj, indent=2, default=str), encoding="utf-8")
        return summary_obj

# ============================================================================
# Misc utilities
# ============================================================================

def random_suffix(length: int = 8) -> str:
    return "".join(random.choices(string.ascii_lowercase + string.digits, k=length))


def err_code(resp: Dict[str, Any]) -> Optional[int]:
    """Extract numeric error code from response, or None."""
    e = resp.get("error")
    if isinstance(e, dict):
        c = e.get("code")
        if isinstance(c, int):
            return c
    return None


def err_message(resp: Dict[str, Any]) -> str:
    e = resp.get("error")
    if isinstance(e, dict):
        return str(e.get("message", ""))
    return ""


def is_ok(resp: Dict[str, Any]) -> bool:
    return resp.get("ok") is True


def is_transport_failure(resp: Dict[str, Any]) -> bool:
    """True for {_err: ...} responses (no Bridge dispatch at all)."""
    return "_err" in resp and not is_ok(resp)


def is_valid_bridge_error(resp: Dict[str, Any]) -> bool:
    """True if response is a structured Bridge error (-32600..-32099 range)."""
    c = err_code(resp)
    if c is None:
        return False
    return -32700 <= c <= -32000

# ============================================================================
# Wait for editor to be live (used in CI / restart flows)
# ============================================================================

def wait_for_editor(timeout: float = 60.0, poll_s: float = 1.0) -> bool:
    """Poll until health() OR timeout. Returns True on success."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if health(timeout=2.0):
            return True
        time.sleep(poll_s)
    return False

# ============================================================================
# Self-test (run directly to verify harness)
# ============================================================================

if __name__ == "__main__":
    print("=== mcp_test_harness self-test ===")
    print(f"editor alive: {health()}")
    s = snapshot()
    print(f"snapshot: used_mb={s.get('used_physical_mb'):.1f} uobjects={s.get('live_uobject_slots')}")

    methods = discover_methods()
    print(f"discovered {len(methods)} methods from source")
    print("first 5:", [m["name"] for m in methods[:5]])

    req_args = discover_required_args()
    print(f"required-arg map covers {len(req_args)} handlers")

    # Quick round-trip
    r = call("memreport.get_quick_stats", {})
    print(f"sample call ok={r.get('ok')} error={r.get('error')}")

    print("=== self-test complete ===")
