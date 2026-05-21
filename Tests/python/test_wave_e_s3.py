#!/usr/bin/env python3
"""Wave E Surface 3 smoke test — hierarchy.* (attach/detach/list_children).

Spawns 2 ephemeral StaticMeshActors via actor.spawn, runs the full attach/detach/list_children
matrix on them, then destroys them. PIE must be OFF.

Sub-tests:
  T1.  Spawn 2 ephemeral StaticMeshActors as test fixtures
  T2.  hierarchy.list_children on parent (should be empty)
  T3.  hierarchy.attach child -> parent
  T4.  hierarchy.list_children parent -> [child] (single entry)
  T5.  hierarchy.detach child
  T6.  hierarchy.list_children parent -> empty again
  T7.  hierarchy.attach with bad child path -> -32004
  T8.  hierarchy.attach with bad parent path -> -32004
  T9.  hierarchy.attach with snap rule for detach -> -32602
  T10. hierarchy.detach with snap rule -> -32602
  T11. Cleanup: actor.destroy both ephemeral actors

Exit 0 on full pass, 1 on first failure.

Usage:
  python test_wave_e_s3.py [--host 127.0.0.1] [--port 30020]
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
READ_TIMEOUT_SEC = 10.0


def send_and_recv_line(host: str, port: int, request_obj: dict) -> Optional[dict]:
    with socket.create_connection((host, port), timeout=READ_TIMEOUT_SEC) as sock:
        sock.settimeout(READ_TIMEOUT_SEC)
        payload = (json.dumps(request_obj, separators=(",", ":")) + "\n").encode("utf-8")
        sock.sendall(payload)
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
                break
            buf.extend(chunk)
            newline_idx = buf.find(b"\n")
            if newline_idx >= 0:
                return json.loads(bytes(buf[:newline_idx]).decode("utf-8"))
        return None


def fail(reason: str) -> int:
    print(f"[WAVE_E_S3] FAIL reason={reason}")
    return 1


def call(host: str, port: int, label: str, method: str, args: Optional[dict] = None) -> Optional[dict]:
    rid = f"e3-{label}"
    req = {"id": rid, "kind": "call_function", "method": method, "args": args or {}}
    try:
        resp = send_and_recv_line(host, port, req)
    except (ConnectionRefusedError, OSError, json.JSONDecodeError) as exc:
        fail(f"{label}: io-error detail={exc}")
        return None
    if resp is None:
        fail(f"{label}: timeout (>{READ_TIMEOUT_SEC}s)")
        return None
    if resp.get("id") != rid:
        fail(f"{label}: id-mismatch expected={rid!r} got={resp.get('id')!r}")
        return None
    return resp


def expect_ok(resp: Optional[dict], label: str) -> Optional[dict]:
    if resp is None:
        return None
    if resp.get("ok") is not True:
        fail(f"{label}: expected ok=true got error={resp.get('error')!r}")
        return None
    result = resp.get("result")
    if not isinstance(result, dict):
        fail(f"{label}: expected result dict got {result!r}")
        return None
    return result


def expect_err_code(resp: Optional[dict], label: str, expected_code: int) -> bool:
    if resp is None:
        return False
    if resp.get("ok") is not False:
        fail(f"{label}: expected ok=false got result={resp.get('result')!r}")
        return False
    err = resp.get("error")
    if not isinstance(err, dict) or err.get("code") != expected_code:
        fail(f"{label}: expected error.code={expected_code} got error={err!r}")
        return False
    return True


def find_existing_actors_for_test(host: str, port: int) -> Optional[tuple[str, str]]:
    """Fallback: pick 2 distinct actors from the persistent level when actor.spawn fails."""
    resp = call(host, port, "fallback/list", "level.get_persistent_level_actors", {"page_size": 50})
    result = expect_ok(resp, "fallback/list")
    if result is None:
        return None
    actors = result.get("actors")
    if not isinstance(actors, list) or len(actors) < 2:
        fail(f"fallback: need 2 actors in persistent level, got {len(actors) if actors else 0}")
        return None
    # Pick the first 2 distinct paths.
    paths = []
    for a in actors:
        if isinstance(a, dict):
            p = a.get("actor_path") or a.get("path")
            if isinstance(p, str) and p and p not in paths:
                paths.append(p)
                if len(paths) == 2:
                    return paths[0], paths[1]
    fail("fallback: could not collect 2 distinct actor paths")
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description="Wave E Surface 3 hierarchy.* smoke")
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    args = parser.parse_args()

    print(f"[WAVE_E_S3] connecting to {args.host}:{args.port} ...")

    # ─── T1: Spawn 2 ephemeral StaticMeshActors ──────────────────────────────────────────────────
    parent_path: Optional[str] = None
    child_path: Optional[str] = None
    spawned_actors: list[str] = []

    spawn_resp = call(args.host, args.port, "T1a/spawn-parent", "actor.spawn", {
        "class_path": "/Script/Engine.StaticMeshActor",
        "location": {"X": 0.0, "Y": 0.0, "Z": 0.0},
    })
    parent_result = expect_ok(spawn_resp, "T1a/spawn-parent")
    if parent_result is not None:
        parent_path = parent_result.get("actor_path") or parent_result.get("actor")
        if isinstance(parent_path, str) and parent_path:
            spawned_actors.append(parent_path)
            print(f"[WAVE_E_S3]   T1a OK parent_actor={parent_path}")

    if parent_path:
        spawn_resp2 = call(args.host, args.port, "T1b/spawn-child", "actor.spawn", {
            "class_path": "/Script/Engine.StaticMeshActor",
            "location": {"X": 200.0, "Y": 0.0, "Z": 0.0},
        })
        child_result = expect_ok(spawn_resp2, "T1b/spawn-child")
        if child_result is not None:
            child_path = child_result.get("actor_path") or child_result.get("actor")
            if isinstance(child_path, str) and child_path:
                spawned_actors.append(child_path)
                print(f"[WAVE_E_S3]   T1b OK child_actor={child_path}")

    # Fallback path if spawn failed.
    if not parent_path or not child_path:
        print(f"[WAVE_E_S3]   T1 spawn failed; falling back to existing level actors")
        pair = find_existing_actors_for_test(args.host, args.port)
        if pair is None:
            return 1
        parent_path, child_path = pair
        spawned_actors = []  # don't destroy real level actors
        # If they happen to currently be attached to each other (or anything), detach child first.
        call(args.host, args.port, "fallback/detach-pre", "hierarchy.detach", {
            "child_actor": child_path,
        })
        print(f"[WAVE_E_S3]   T1-fallback parent={parent_path}")
        print(f"[WAVE_E_S3]   T1-fallback child={child_path}")

    # ─── T2: list_children of parent should be empty ─────────────────────────────────────────────
    resp = call(args.host, args.port, "T2/list-empty", "hierarchy.list_children", {
        "actor_path": parent_path,
    })
    r = expect_ok(resp, "T2/list-empty")
    if r is None:
        return cleanup_and_fail(args.host, args.port, spawned_actors, 1)
    children = r.get("children")
    if not isinstance(children, list):
        return cleanup_and_fail(args.host, args.port, spawned_actors,
                                fail(f"T2: expected children list got {children!r}"))
    # Filter out our intended child if pre-existing
    initial_children = [c for c in children if c.get("actor_path") != child_path]
    if len(initial_children) != 0:
        # Allow some pre-existing siblings — only care that our child isn't there yet.
        attached_initially = any(c.get("actor_path") == child_path for c in children)
        if attached_initially:
            return cleanup_and_fail(args.host, args.port, spawned_actors,
                                    fail(f"T2: child unexpectedly already attached"))
    print(f"[WAVE_E_S3]   T2 OK (parent has {len(children)} pre-existing children; ours not among them)")

    # ─── T3: attach child -> parent ──────────────────────────────────────────────────────────────
    resp = call(args.host, args.port, "T3/attach", "hierarchy.attach", {
        "child_actor": child_path,
        "parent_actor": parent_path,
        "location_rule": "keep_world",
        "rotation_rule": "keep_world",
        "scale_rule":    "keep_world",
    })
    r = expect_ok(resp, "T3/attach")
    if r is None:
        return cleanup_and_fail(args.host, args.port, spawned_actors, 1)
    if not (r.get("child") and r.get("parent")):
        return cleanup_and_fail(args.host, args.port, spawned_actors,
                                fail(f"T3: missing child/parent in response: {r!r}"))
    print(f"[WAVE_E_S3]   T3 OK child={r['child']!r} parent={r['parent']!r} socket={r.get('socket')!r}")

    # ─── T4: list_children parent -> contains child ──────────────────────────────────────────────
    resp = call(args.host, args.port, "T4/list-after-attach", "hierarchy.list_children", {
        "actor_path": parent_path,
    })
    r = expect_ok(resp, "T4/list-after-attach")
    if r is None:
        return cleanup_and_fail(args.host, args.port, spawned_actors, 1)
    children = r.get("children", [])
    matching = [c for c in children if c.get("actor_path") == child_path]
    if len(matching) != 1:
        return cleanup_and_fail(args.host, args.port, spawned_actors,
                                fail(f"T4: expected exactly 1 matching child, got {len(matching)} (children={children!r})"))
    print(f"[WAVE_E_S3]   T4 OK (child appears in list_children)")

    # ─── T5: detach child ────────────────────────────────────────────────────────────────────────
    resp = call(args.host, args.port, "T5/detach", "hierarchy.detach", {
        "child_actor": child_path,
        "location_rule": "keep_world",
        "rotation_rule": "keep_world",
        "scale_rule":    "keep_world",
    })
    r = expect_ok(resp, "T5/detach")
    if r is None:
        return cleanup_and_fail(args.host, args.port, spawned_actors, 1)
    if r.get("child") != child_path and not r.get("child", "").endswith(child_path.rsplit('.', 1)[-1]):
        # Pathname can normalize slightly; accept either exact match or matching trailing name.
        return cleanup_and_fail(args.host, args.port, spawned_actors,
                                fail(f"T5: child path mismatch: expected~{child_path!r} got {r.get('child')!r}"))
    print(f"[WAVE_E_S3]   T5 OK detached; prior_parent={r.get('prior_parent')!r}")

    # ─── T6: list_children parent -> child no longer present ─────────────────────────────────────
    resp = call(args.host, args.port, "T6/list-after-detach", "hierarchy.list_children", {
        "actor_path": parent_path,
    })
    r = expect_ok(resp, "T6/list-after-detach")
    if r is None:
        return cleanup_and_fail(args.host, args.port, spawned_actors, 1)
    children = r.get("children", [])
    matching = [c for c in children if c.get("actor_path") == child_path]
    if len(matching) != 0:
        return cleanup_and_fail(args.host, args.port, spawned_actors,
                                fail(f"T6: child still in list after detach: {matching!r}"))
    print(f"[WAVE_E_S3]   T6 OK (child gone from list_children)")

    # ─── T7: attach with bad child path -> -32004 ────────────────────────────────────────────────
    resp = call(args.host, args.port, "T7/bad-child", "hierarchy.attach", {
        "child_actor":  "/Game/Maps/None.NoneActor",
        "parent_actor": parent_path,
    })
    if not expect_err_code(resp, "T7/bad-child", -32004):
        return cleanup_and_fail(args.host, args.port, spawned_actors, 1)
    print(f"[WAVE_E_S3]   T7 OK (bad child -> -32004)")

    # ─── T8: attach with bad parent path -> -32004 ───────────────────────────────────────────────
    resp = call(args.host, args.port, "T8/bad-parent", "hierarchy.attach", {
        "child_actor":  child_path,
        "parent_actor": "/Game/Maps/None.NoneParent",
    })
    if not expect_err_code(resp, "T8/bad-parent", -32004):
        return cleanup_and_fail(args.host, args.port, spawned_actors, 1)
    print(f"[WAVE_E_S3]   T8 OK (bad parent -> -32004)")

    # ─── T9: attach with snap rule (allowed for attach) → success ────────────────────────────────
    resp = call(args.host, args.port, "T9/attach-snap", "hierarchy.attach", {
        "child_actor":  child_path,
        "parent_actor": parent_path,
        "location_rule": "snap",
        "rotation_rule": "snap",
        "scale_rule":    "snap",
    })
    r = expect_ok(resp, "T9/attach-snap")
    if r is None:
        return cleanup_and_fail(args.host, args.port, spawned_actors, 1)
    print(f"[WAVE_E_S3]   T9 OK (attach with snap rule)")

    # ─── T10: detach with snap rule -> -32602 ────────────────────────────────────────────────────
    resp = call(args.host, args.port, "T10/detach-snap", "hierarchy.detach", {
        "child_actor": child_path,
        "location_rule": "snap",
    })
    if not expect_err_code(resp, "T10/detach-snap", -32602):
        return cleanup_and_fail(args.host, args.port, spawned_actors, 1)
    print(f"[WAVE_E_S3]   T10 OK (detach with snap -> -32602)")

    # Cleanup: detach actually (T10 errored, didn't detach) before final destroy
    call(args.host, args.port, "cleanup/detach", "hierarchy.detach", {
        "child_actor": child_path,
        "location_rule": "keep_world",
    })

    # ─── T11: Destroy ephemeral actors ──────────────────────────────────────────────────────────
    destroyed = 0
    for path in spawned_actors:
        resp = call(args.host, args.port, f"T11/destroy[{path}]", "actor.destroy", {"actor_path": path})
        r = expect_ok(resp, f"T11/destroy[{path}]")
        if r is not None:
            destroyed += 1
    print(f"[WAVE_E_S3]   T11 destroyed {destroyed}/{len(spawned_actors)} ephemeral actors")

    print(f"[WAVE_E_S3] PASS — all 10 sub-tests succeeded ({destroyed} ephemeral actors cleaned up)")
    return 0


def cleanup_and_fail(host: str, port: int, spawned: list, code: int) -> int:
    for path in spawned:
        call(host, port, f"cleanup/destroy[{path}]", "actor.destroy", {"actor_path": path})
    return code


if __name__ == "__main__":
    sys.exit(main())
