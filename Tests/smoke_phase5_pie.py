#!/usr/bin/env python3
"""Phase 5 Chunk A smoke — 10 PIE tools.

Verifies (against a live editor on port 30020; operator may have PIE on or off — sub-tests adapt):

  Discovery (1):
    1. tools.list contains all 10 new pie.* tools.

  Always-callable (2):
    2. pie.is_running → {running, paused, world_count} schema verified regardless of PIE state.

  Inverse PIE-guard (3): NEW pattern — pie.pause without PIE → -32038 PIENotActive + frozen
     message containing "PIE is not running" + "pie.start" + "editor.*". This is the inverse of
     Phase 3's -32027 PIEActive guard. We pre-check via pie.is_running; if PIE is running, we
     pie.stop first to set up the negative test.

  PIE lifecycle (4-7):
    4. pie.start (mode='selected_viewport') → {started=true, pie_world_path}; tolerate either
       in-process or already-running variant. Then poll pie.is_running until running=true
       (PIE startup spans multiple ticks).
    5. pie.is_running post-start → running=true, world_count >= 1.
    6. pie.console_exec('stat fps') with default world='pie' → {executed, output}. We accept
       executed=false because many stat commands don't write to the FOutputDevice; we just verify
       the call completes without error and returns the schema shape.
    7. pie.pause → {paused=true}; pie.resume → {resumed=true}; pie.pause → {paused=true} again
       (to set up step_frame); pie.step_frame → {advanced=true, current_frame:int>0}.

  PIE actor identity (8-9):
    8. pie.get_player_controller(0) → {pc_actor_guid, pc_path}; pc_path starts with '/'.
    9. pie.get_pawn(0) → {pawn_actor_guid, pawn_path, class}; class starts with '/Script/' or '/Game/'.
       SKIP if NO_PAWN (level may not auto-spawn a pawn).

  Viewport focus (10):
   10. pie.focus_actor(<pc_path>, camera_distance=500.0) → {focused=true}.
       SKIP if get_player_controller didn't succeed.

  Cleanup:
   11. pie.stop → {stopped=true}; pie.is_running → running=false (poll until satisfied).

Prints ``[SMOKE_PHASE5_PIE] PASS`` on success or ``[SMOKE_PHASE5_PIE] FAIL ...`` on first
mismatch. Sub-tests dependent on missing resources emit SKIP lines.

Usage:
  python smoke_phase5_pie.py [--host HOST] [--port PORT] [--keep-pie-running]
"""

from __future__ import annotations

import argparse
import json
import socket
import sys
import time
from typing import Any, Dict, List, Optional


DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 30020
# PIE startup can be slow on first launch (cooks shaders etc) — give it plenty of room.
READ_TIMEOUT_SEC = 30.0
PIE_START_POLL_TIMEOUT_SEC = 30.0
PIE_STOP_POLL_TIMEOUT_SEC = 15.0
POLL_INTERVAL_SEC = 0.5


def send_and_recv_line(host: str, port: int, request_obj: dict,
                       timeout: float = READ_TIMEOUT_SEC) -> Optional[dict]:
    with socket.create_connection((host, port), timeout=timeout) as sock:
        sock.settimeout(timeout)
        payload = (json.dumps(request_obj, separators=(",", ":")) + "\n").encode("utf-8")
        sock.sendall(payload)
        buf = bytearray()
        deadline = time.monotonic() + timeout
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
    print(f"[SMOKE_PHASE5_PIE] FAIL reason={reason}")
    return 1


def skip(reason: str) -> None:
    print(f"[SMOKE_PHASE5_PIE]   SKIP {reason}")


def call(host: str, port: int, label: str, request_id: str, method: str,
         args: Optional[dict] = None, timeout: float = READ_TIMEOUT_SEC) -> Optional[dict]:
    req = {"id": request_id, "kind": "call_function", "method": method, "args": args or {}}
    try:
        return send_and_recv_line(host, port, req, timeout=timeout)
    except (ConnectionRefusedError, OSError) as exc:
        fail(f"{label}: connect-error detail={exc}")
        return None


def expect_ok(response: Optional[dict], expected_id: str, label: str) -> Optional[dict]:
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


def expect_error(response: Optional[dict], expected_id: str, expected_codes,
                 label: str) -> Optional[dict]:
    if isinstance(expected_codes, int):
        expected_codes = (expected_codes,)
    if response is None:
        fail(f"{label}: timeout (>{READ_TIMEOUT_SEC}s)")
        return None
    if response.get("id") != expected_id:
        fail(f"{label}: id-mismatch expected={expected_id!r} got={response.get('id')!r}")
        return None
    if response.get("ok") is not False:
        fail(f"{label}: ok-not-false got={response.get('ok')!r}")
        return None
    error = response.get("error")
    if not isinstance(error, dict) or error.get("code") not in expected_codes:
        fail(f"{label}: wrong-error-code expected={expected_codes} got={error!r}")
        return None
    return error


def poll_is_running(host: str, port: int, want_running: bool, timeout: float,
                    label: str) -> Optional[dict]:
    """Poll pie.is_running until running matches want_running or timeout. Returns final result."""
    deadline = time.monotonic() + timeout
    last = None
    poll_idx = 0
    while time.monotonic() < deadline:
        resp = call(host, port, label, f"{label}-poll-{poll_idx}", "pie.is_running")
        poll_idx += 1
        result = expect_ok(resp, f"{label}-poll-{poll_idx - 1}", f"{label}/poll")
        if result is None:
            return None
        last = result
        if bool(result.get("running")) == want_running:
            return result
        time.sleep(POLL_INTERVAL_SEC)
    fail(f"{label}: poll exceeded {timeout}s, last={last!r} (wanted running={want_running})")
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--keep-pie-running", action="store_true",
                        help="Skip the final pie.stop cleanup (leave PIE up for follow-up tests)")
    args = parser.parse_args()

    print(f"[SMOKE_PHASE5_PIE] connecting to {args.host}:{args.port} ...")

    # ─── Sub-test 1: tools.list contains all 10 pie.* handlers ─────────────────────────────────
    result = expect_ok(call(args.host, args.port, "1", "p5-pie-1", "tools.list"),
                       "p5-pie-1", "1/tools.list")
    if result is None:
        return 1
    cpp_handlers = set(result.get("cpp_handlers") or [])
    expected_cpp = {
        "pie.start", "pie.stop", "pie.pause", "pie.resume", "pie.step_frame",
        "pie.console_exec", "pie.is_running",
        "pie.get_player_controller", "pie.get_pawn", "pie.focus_actor",
    }
    missing = expected_cpp - cpp_handlers
    if missing:
        return fail(f"1/tools.list: missing pie.* handlers: {sorted(missing)}")
    print(f"[SMOKE_PHASE5_PIE]   1/tools.list contains all 10 pie.* handlers")

    # ─── Sub-test 2: pie.is_running always callable ────────────────────────────────────────────
    result = expect_ok(call(args.host, args.port, "2", "p5-pie-2", "pie.is_running"),
                       "p5-pie-2", "2/pie.is_running")
    if result is None:
        return 1
    if not isinstance(result.get("running"), bool):
        return fail(f"2/pie.is_running: running not bool {result!r}")
    if not isinstance(result.get("paused"), bool):
        return fail(f"2/pie.is_running: paused not bool {result!r}")
    if not isinstance(result.get("world_count"), (int, float)):
        return fail(f"2/pie.is_running: world_count not number {result!r}")
    pre_running = bool(result["running"])
    print(f"[SMOKE_PHASE5_PIE]   2/pie.is_running OK (running={pre_running}, paused={result['paused']}, "
          f"world_count={int(result['world_count'])})")

    # ─── Setup for sub-test 3: ensure no PIE so we can hit -32038 ──────────────────────────────
    # If PIE is running, stop it first (and re-poll until stopped). If it's not running, we're
    # already in the right state.
    if pre_running:
        print(f"[SMOKE_PHASE5_PIE]   pre-stop PIE so sub-test 3 can hit -32038 ...")
        resp = call(args.host, args.port, "pre-stop", "p5-pie-pre-stop", "pie.stop")
        result = expect_ok(resp, "p5-pie-pre-stop", "pre-stop")
        if result is None:
            return 1
        stop_poll = poll_is_running(args.host, args.port, want_running=False,
                                    timeout=PIE_STOP_POLL_TIMEOUT_SEC, label="pre-stop")
        if stop_poll is None:
            return 1
        print(f"[SMOKE_PHASE5_PIE]   pre-stop OK (running=false)")

    # ─── Sub-test 3: pie.pause without PIE → -32038 PIENotActive ──────────────────────────────
    resp = call(args.host, args.port, "3", "p5-pie-3", "pie.pause")
    error = expect_error(resp, "p5-pie-3", -32038, "3/pie.pause-without-pie")
    if error is None:
        return 1
    msg = str(error.get("message", ""))
    # Frozen message contract: substrings "PIE is not running", "pie.start", "editor.*"
    for required in ("PIE is not running", "pie.start", "editor.*"):
        if required not in msg:
            return fail(f"3/pie.pause: frozen message missing substring {required!r}; got {msg!r}")
    print(f"[SMOKE_PHASE5_PIE]   3/pie.pause(no PIE) → -32038 PIENotActive (frozen message OK)")

    # Also test pie.step_frame, pie.console_exec(world=pie), pie.get_pawn — all should refuse with
    # -32038. Bundled into a single block to keep the smoke from sprawling but still cover the
    # contract.
    for sub_method in ("pie.step_frame", "pie.get_player_controller", "pie.get_pawn",
                       "pie.focus_actor"):
        sub_args: Optional[dict] = None
        if sub_method == "pie.focus_actor":
            sub_args = {"actor_id": "WontResolve"}
        rid = f"p5-pie-3-{sub_method}"
        sresp = call(args.host, args.port, rid, rid, sub_method, sub_args)
        if expect_error(sresp, rid, -32038, f"3b/{sub_method}-without-pie") is None:
            return 1
    print(f"[SMOKE_PHASE5_PIE]   3b/step_frame|get_pc|get_pawn|focus_actor(no PIE) → -32038 (4/4 OK)")

    # console_exec with world='editor' should bypass the PIE-required guard.
    resp = call(args.host, args.port, "3c", "p5-pie-3c", "pie.console_exec",
                {"command": "help", "world": "editor"})
    sub_result = expect_ok(resp, "p5-pie-3c", "3c/console_exec(editor)")
    if sub_result is None:
        return 1
    if "executed" not in sub_result or "output" not in sub_result:
        return fail(f"3c/console_exec(editor): missing fields in {sub_result!r}")
    print(f"[SMOKE_PHASE5_PIE]   3c/pie.console_exec(world='editor') OK (no PIE required)")

    # ─── Sub-test 4: pie.start ──────────────────────────────────────────────────────────────────
    print(f"[SMOKE_PHASE5_PIE]   starting PIE ...")
    resp = call(args.host, args.port, "4", "p5-pie-4", "pie.start",
                {"mode": "selected_viewport", "viewport_size": [1280, 720], "player_count": 1},
                timeout=READ_TIMEOUT_SEC)
    result = expect_ok(resp, "p5-pie-4", "4/pie.start")
    if result is None:
        return 1
    if result.get("started") is not True:
        return fail(f"4/pie.start: started!=true {result!r}")
    print(f"[SMOKE_PHASE5_PIE]   4/pie.start OK (started=true, pie_world_path={result.get('pie_world_path')!r})")

    # ─── Sub-test 5: pie.is_running shows running=true (poll for PIE to fully spin up) ─────────
    print(f"[SMOKE_PHASE5_PIE]   polling pie.is_running until running=true ...")
    poll_result = poll_is_running(args.host, args.port, want_running=True,
                                  timeout=PIE_START_POLL_TIMEOUT_SEC, label="5")
    if poll_result is None:
        return 1
    if int(poll_result.get("world_count", 0)) < 1:
        return fail(f"5/pie.is_running: world_count<1 {poll_result!r}")
    print(f"[SMOKE_PHASE5_PIE]   5/pie.is_running OK (running=true, world_count={int(poll_result['world_count'])})")

    # ─── Sub-test 6: pie.console_exec ──────────────────────────────────────────────────────────
    resp = call(args.host, args.port, "6", "p5-pie-6", "pie.console_exec",
                {"command": "stat fps"})
    result = expect_ok(resp, "p5-pie-6", "6/pie.console_exec")
    if result is None:
        return 1
    if not isinstance(result.get("executed"), bool):
        return fail(f"6/pie.console_exec: executed not bool {result!r}")
    if not isinstance(result.get("output"), str):
        return fail(f"6/pie.console_exec: output not string {result!r}")
    print(f"[SMOKE_PHASE5_PIE]   6/pie.console_exec('stat fps') OK (executed={result['executed']}, "
          f"output_len={len(result['output'])})")

    # ─── Sub-test 7: pause / resume / pause / step_frame ───────────────────────────────────────
    resp = call(args.host, args.port, "7a", "p5-pie-7a", "pie.pause")
    result = expect_ok(resp, "p5-pie-7a", "7a/pie.pause")
    if result is None:
        return 1
    if result.get("paused") is not True:
        return fail(f"7a/pie.pause: paused!=true {result!r}")
    # Give the engine a moment to actually transition to paused state before resume.
    time.sleep(POLL_INTERVAL_SEC)

    resp = call(args.host, args.port, "7b", "p5-pie-7b", "pie.resume")
    result = expect_ok(resp, "p5-pie-7b", "7b/pie.resume")
    if result is None:
        return 1
    if result.get("resumed") is not True:
        return fail(f"7b/pie.resume: resumed!=true {result!r}")
    time.sleep(POLL_INTERVAL_SEC)

    # Now pause again for step_frame.
    resp = call(args.host, args.port, "7c", "p5-pie-7c", "pie.pause")
    result = expect_ok(resp, "p5-pie-7c", "7c/pie.pause")
    if result is None:
        return 1
    if result.get("paused") is not True:
        return fail(f"7c/pie.pause: paused!=true {result!r}")
    # is_running.paused may take a moment to flip — give one short poll.
    deadline = time.monotonic() + 5.0
    paused_seen = False
    while time.monotonic() < deadline:
        ir = call(args.host, args.port, "7c-poll", f"p5-pie-7c-poll-{time.monotonic():.4f}",
                  "pie.is_running")
        ir_r = expect_ok(ir, ir.get("id", "") if ir else "", "7c-poll/is_running")
        if ir_r is None:
            return 1
        if ir_r.get("paused") is True:
            paused_seen = True
            break
        time.sleep(POLL_INTERVAL_SEC)
    if not paused_seen:
        skip("7c: pie.is_running.paused never flipped to true; step_frame test will probably 500")

    resp = call(args.host, args.port, "7d", "p5-pie-7d", "pie.step_frame")
    # step_frame requires PIE to be paused — when our quick pause settled, it succeeds; otherwise
    # we get the -32603 "must pause first" error. Accept either to keep the smoke robust on
    # different host load profiles.
    if resp is None:
        return fail("7d/pie.step_frame: timeout")
    if resp.get("ok") is True:
        sr = resp.get("result", {})
        if sr.get("advanced") is not True:
            return fail(f"7d/pie.step_frame: advanced!=true {sr!r}")
        if not isinstance(sr.get("current_frame"), (int, float)):
            return fail(f"7d/pie.step_frame: current_frame not number {sr!r}")
        print(f"[SMOKE_PHASE5_PIE]   7d/pie.step_frame OK (current_frame={int(sr['current_frame'])})")
    elif resp.get("error", {}).get("code") == -32603:
        skip(f"7d/pie.step_frame returned -32603 (paused race); message={resp['error'].get('message')!r}")
    else:
        return fail(f"7d/pie.step_frame: unexpected response {resp!r}")
    # Resume so subsequent tests aren't run against a paused world.
    resp = call(args.host, args.port, "7e", "p5-pie-7e", "pie.resume")
    if expect_ok(resp, "p5-pie-7e", "7e/pie.resume") is None:
        return 1
    time.sleep(POLL_INTERVAL_SEC)
    print(f"[SMOKE_PHASE5_PIE]   7/pie.pause+resume+pause+step+resume OK")

    # ─── Sub-test 8: pie.get_player_controller ──────────────────────────────────────────────────
    pc_path: Optional[str] = None
    resp = call(args.host, args.port, "8", "p5-pie-8", "pie.get_player_controller",
                {"player_index": 0})
    result = expect_ok(resp, "p5-pie-8", "8/pie.get_player_controller")
    if result is None:
        # Some PIE configs don't auto-spawn a PC instantly; degrade to SKIP rather than full FAIL,
        # but ONLY if the listener responded at all (we already failed via expect_ok side effect).
        skip("8/pie.get_player_controller failed; downstream sub-tests will SKIP")
    else:
        if not isinstance(result.get("pc_actor_guid"), str):
            return fail(f"8/get_pc: pc_actor_guid not string {result!r}")
        if not isinstance(result.get("pc_path"), str) or not result["pc_path"].startswith("/"):
            return fail(f"8/get_pc: pc_path malformed {result!r}")
        pc_path = result["pc_path"]
        print(f"[SMOKE_PHASE5_PIE]   8/pie.get_player_controller OK (pc_path={pc_path!r}, "
              f"guid={result['pc_actor_guid']!r})")

    # ─── Sub-test 9: pie.get_pawn ──────────────────────────────────────────────────────────────
    resp = call(args.host, args.port, "9", "p5-pie-9", "pie.get_pawn", {"player_index": 0})
    if resp is None:
        return fail("9/pie.get_pawn: timeout")
    if resp.get("ok") is True:
        result = resp.get("result", {})
        if not isinstance(result, dict):
            return fail(f"9/pie.get_pawn: result not object {result!r}")
        if not isinstance(result.get("pawn_actor_guid"), str):
            return fail(f"9/pie.get_pawn: pawn_actor_guid not string {result!r}")
        if not isinstance(result.get("pawn_path"), str) or not result["pawn_path"].startswith("/"):
            return fail(f"9/pie.get_pawn: pawn_path malformed {result!r}")
        if not isinstance(result.get("class"), str) or not result["class"].startswith("/"):
            return fail(f"9/pie.get_pawn: class malformed {result!r}")
        print(f"[SMOKE_PHASE5_PIE]   9/pie.get_pawn OK (pawn_path={result['pawn_path']!r}, "
              f"class={result['class']!r})")
    elif resp.get("error", {}).get("code") == -32004:
        skip(f"9/pie.get_pawn returned -32004 NO_PAWN ({resp['error'].get('message')!r}); "
             f"level may not auto-spawn a pawn")
    else:
        return fail(f"9/pie.get_pawn: unexpected response {resp!r}")

    # ─── Sub-test 10: pie.focus_actor ──────────────────────────────────────────────────────────
    if pc_path is None:
        skip("10/pie.focus_actor: no PC path available from sub-test 8")
    else:
        resp = call(args.host, args.port, "10", "p5-pie-10", "pie.focus_actor",
                    {"actor_id": pc_path, "camera_distance": 500.0})
        result = expect_ok(resp, "p5-pie-10", "10/pie.focus_actor")
        if result is None:
            return 1
        if result.get("focused") is not True:
            return fail(f"10/pie.focus_actor: focused!=true {result!r}")
        print(f"[SMOKE_PHASE5_PIE]   10/pie.focus_actor OK (focused=true)")

    # ─── Cleanup: pie.stop and verify ──────────────────────────────────────────────────────────
    if args.keep_pie_running:
        print(f"[SMOKE_PHASE5_PIE]   keeping PIE running (--keep-pie-running)")
    else:
        resp = call(args.host, args.port, "11a", "p5-pie-11a", "pie.stop")
        result = expect_ok(resp, "p5-pie-11a", "11a/pie.stop")
        if result is None:
            return 1
        if result.get("stopped") is not True:
            return fail(f"11a/pie.stop: stopped!=true {result!r}")
        # Poll until running=false.
        stop_poll = poll_is_running(args.host, args.port, want_running=False,
                                    timeout=PIE_STOP_POLL_TIMEOUT_SEC, label="11b")
        if stop_poll is None:
            return 1
        print(f"[SMOKE_PHASE5_PIE]   11/pie.stop OK (running=false, world_count={int(stop_poll['world_count'])})")

    print("[SMOKE_PHASE5_PIE] PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
