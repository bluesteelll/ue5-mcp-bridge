"""Phase 2 Day 11-12 — Python shared helpers for asset.* composites.

This module exposes no @tool-decorated functions of its own. It holds path/coercion
utility helpers shared by `asset_composites.py` so the composite wrappers stay short
and intent-revealing.

Importing this module is a side-effect-free no-op for tool registration, but it
WILL be imported transitively by `asset_composites` so the bootstrap path picks it up.
"""

from __future__ import annotations

from typing import Any, Dict, List, Sequence


def coerce_string_array(arg: Any, field_name: str) -> List[str]:
    """Normalise an array-of-strings argument shape.

    Accepts: list[str], tuple[str], or single str (wrapped to single-item list).
    Raises ValueError on any other shape — the caller surfaces as INVALID_PARAMS.

    Used by composites that accept ``package_paths`` / ``paths`` arrays from JSON
    where a permissive caller might pass a bare string by mistake.
    """
    if isinstance(arg, str):
        return [arg]
    if isinstance(arg, (list, tuple)):
        out: List[str] = []
        for entry in arg:
            if not isinstance(entry, str):
                raise ValueError(f"{field_name}: expected array of strings, got {type(entry).__name__}")
            out.append(entry)
        return out
    raise ValueError(f"{field_name}: expected string or array of strings, got {type(arg).__name__}")


def basename_no_class(asset_path: str) -> str:
    """Extract the short asset name from a full asset_path.

    Handles both forms:
      ``/Game/Foo/Bar.Bar``    → ``Bar`` (strip class suffix and folder prefix)
      ``/Game/Foo/Bar``        → ``Bar`` (folder prefix only)
    """
    last_slash = asset_path.rfind("/")
    leaf = asset_path[last_slash + 1:] if last_slash >= 0 else asset_path
    last_dot = leaf.rfind(".")
    return leaf[:last_dot] if last_dot >= 0 else leaf


def dispatch_internal(method: str, args: Dict[str, Any]) -> Dict[str, Any]:
    """Invoke a C++ internal handler (or any registered tool) via the in-process bridge.

    Used by composites to call ``asset._find_unused_internal`` etc. in one round-trip.

    Internally synthesises an FMCPRequest-shaped dict and calls the Python evaluator
    bridge's CallTool helper. Returns the inner result dict on success, raises
    ``RuntimeError`` on dispatch failure (caller surfaces the error to the wire).

    Implementation detail: we go through the dispatch-queue's in-process API rather
    than opening a TCP socket back to ourselves. That path is exposed via the
    ``unreal_mcp_bridge`` C++ helper that the bootstrap may expose later; for now
    we use a thin Python proxy.
    """
    import json
    try:
        import unreal  # noqa: WPS433 — runtime UE binding
    except ImportError as exc:
        raise RuntimeError("dispatch_internal: unreal module unavailable") from exc

    # Synthesise a transient request envelope. The internal dispatcher recognises
    # a CallFunction kind for any method name registered via FMCPDispatchQueue.
    request_id = "py-composite-internal"
    request_obj = {
        "id": request_id,
        "kind": "call_function",
        "method": method,
        "args": args or {},
    }

    # Loop back to ourselves via the TCP listener — the simplest reliable path that
    # works from inside Python without exposing a new C++ API to the python binding.
    # Listener is loopback-only, so this is safe.
    import socket
    payload = (json.dumps(request_obj, separators=(",", ":")) + "\n").encode("utf-8")
    with socket.create_connection(("127.0.0.1", 30020), timeout=60.0) as sock:
        sock.settimeout(60.0)
        sock.sendall(payload)
        buf = bytearray()
        while True:
            chunk = sock.recv(65536)
            if not chunk:
                break
            buf.extend(chunk)
            nl = buf.find(b"\n")
            if nl >= 0:
                response = json.loads(bytes(buf[:nl]).decode("utf-8"))
                break
        else:
            raise RuntimeError(f"dispatch_internal({method}): no newline-terminated response received")

    if response.get("ok") is not True:
        err = response.get("error") or {}
        raise RuntimeError(
            f"dispatch_internal({method}) error code={err.get('code')} message={err.get('message')!r}"
        )
    result = response.get("result")
    if not isinstance(result, dict):
        raise RuntimeError(f"dispatch_internal({method}): result is not an object, got {type(result).__name__}")
    return result


def wait_for_job_and_return_result(job_id: str, timeout_s: float = 120.0,
                                   poll_initial_s: float = 0.02,
                                   poll_max_s: float = 0.5) -> Dict[str, Any]:
    """Poll ``job.result`` until terminal, return the inner result payload.

    Used by composite tools (``asset.find_unused``, ``asset.size_report``,
    ``asset.batch_metadata_async``) that submitted a Lane-B C++ handler returning a job_id and
    need to return the actual data back to the caller in the same composite call.

    Polling rules:
      - Each call passes ``wait_timeout_s=0`` to ``job.result`` so the server returns
        immediately (no in-server blocking that would stall game-thread Drain for
        bGameThreadRequired=true jobs).
      - Sleep between polls grows exponentially from poll_initial_s up to poll_max_s. The sleep
        is CRITICAL — it releases the game thread back to UE so AsyncTask(GameThread, …) job
        bodies can advance, otherwise we'd starve our own job.
      - Raises RuntimeError on Failed / Cancelled / timeout / unexpected response shape.
      - Returns the dict that was the original handler's result.

    ``job.result`` and ``job.status`` MUST be Lane B for this loop to be deadlock-free; both
    were promoted in hotfix 2 (see C++ FMCPDay7Handlers.cpp / UnrealMCPBridge.cpp). If either
    is reverted to Lane A this helper will hang because the poll request queues back to the
    same game thread the composite is running on.
    """
    import time

    deadline = time.monotonic() + max(0.0, float(timeout_s))
    sleep_s = max(0.001, float(poll_initial_s))
    sleep_max = max(sleep_s, float(poll_max_s))

    while True:
        resp = dispatch_internal("job.result", {"job_id": job_id, "wait_timeout_s": 0})
        state = resp.get("state")
        if state == "Succeeded":
            inner = resp.get("result")
            if not isinstance(inner, dict):
                raise RuntimeError(
                    f"wait_for_job({job_id}): Succeeded but result is not an object — "
                    f"got {type(inner).__name__}"
                )
            return inner
        if state == "Failed":
            raise RuntimeError(
                f"wait_for_job({job_id}): Failed — {resp.get('error') or '(no error message)'}"
            )
        if state == "Cancelled":
            raise RuntimeError(
                f"wait_for_job({job_id}): Cancelled — {resp.get('message') or '(no message)'}"
            )
        # Still Pending / Running — keep polling unless we're past the deadline.
        if time.monotonic() >= deadline:
            raise RuntimeError(
                f"wait_for_job({job_id}): timeout after {timeout_s:.1f}s; last state={state!r} "
                f"progress={resp.get('progress')}"
            )
        time.sleep(sleep_s)
        sleep_s = min(sleep_max, sleep_s * 1.5)
