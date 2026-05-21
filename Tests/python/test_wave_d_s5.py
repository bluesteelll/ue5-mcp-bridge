#!/usr/bin/env python3
"""Wave D Surface 5 — level_streaming.* test suite."""

import json
import socket
import sys
import time
import uuid


HOST = "127.0.0.1"
PORT = 30020


def call(method, args=None, timeout=15.0):
    rid = str(uuid.uuid4())
    payload = {"id": rid, "kind": "call_function", "method": method, "args": args or {}}
    msg = (json.dumps(payload) + "\n").encode()
    s = socket.create_connection((HOST, PORT), timeout=timeout)
    try:
        s.sendall(msg)
        buf = b""
        deadline = time.time() + timeout
        while time.time() < deadline:
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
            if b"\n" in buf:
                break
        if not buf:
            return {"error": {"code": -1, "message": "empty response"}}
        line = buf[: buf.index(b"\n")].decode("utf-8")
        return json.loads(line)
    finally:
        s.close()


class T:
    OK = "PASS"
    FAIL = "FAIL"
    SKIP = "SKIP"


def assert_ok(resp, label):
    if "error" in resp and resp["error"]:
        print(f"  {T.FAIL} {label}: error={resp['error']}")
        return False
    if "result" not in resp:
        print(f"  {T.FAIL} {label}: no result, resp={resp}")
        return False
    return True


def assert_err(resp, expected_code, label):
    err = resp.get("error")
    if not err:
        print(f"  {T.FAIL} {label}: expected error -{abs(expected_code)}, got result={resp.get('result')}")
        return False
    if err.get("code") != expected_code:
        print(f"  {T.FAIL} {label}: expected -{abs(expected_code)}, got {err.get('code')} message={err.get('message')}")
        return False
    return True


def main():
    passed = 0
    failed = 0
    skipped = 0

    # ── T1: level_streaming.list on current editor world (no path arg) ──
    print("[T1] level_streaming.list (default editor world)")
    r = call("level_streaming.list", {})
    if assert_ok(r, "T1"):
        res = r["result"]
        persistent = res.get("persistent", "?")
        streaming = res.get("streaming", [])
        count = res.get("count", -1)
        print(f"  PASS T1: persistent='{persistent}', streaming_count={count}")
        if streaming:
            sample = streaming[0]
            print(f"  sample entry: {json.dumps(sample, indent=4)}")
        passed += 1
    else:
        failed += 1

    # ── T2: level_streaming.list with bad path → -32004 ObjectNotFound ──
    print("[T2] level_streaming.list bad persistent_level_path (expect -32004)")
    r = call("level_streaming.list", {"persistent_level_path": "/Game/Maps/DoesNotExist_zZz9"})
    if assert_err(r, -32004, "T2"):
        print(f"  PASS T2: error code -32004 as expected, msg={r['error']['message'][:80]}")
        passed += 1
    else:
        failed += 1

    # ── T2b: level_streaming.list with non-UWorld path → -32011 WrongClass ──
    print("[T2b] level_streaming.list non-UWorld path (expect -32011)")
    # Use the LevelStreamingTools.h itself as a non-UWorld asset reference is hard; instead try
    # /Script/Engine which loads but isn't a UWorld.
    r = call("level_streaming.list", {"persistent_level_path": "/Script/Engine"})
    # /Script/Engine might fail at IsValidGameOrPlugin (it IS valid) but Cast<UWorld> should fail.
    # Acceptable: either -32004 ObjectNotFound or -32011 WrongClass.
    err = r.get("error")
    if err and err.get("code") in (-32004, -32011, -32010):
        print(f"  PASS T2b: error code {err['code']} (one of -32004/-32011/-32010) as expected")
        passed += 1
    else:
        print(f"  FAIL T2b: expected -32004/-32011/-32010 error, got resp={r}")
        failed += 1

    # ── T3 / T5: add + remove sublevel (requires spare unused asset) ──
    # Use M_Scratch.umap as persistent + M_ScratchDup.umap as the streaming sublevel.
    print("[T3] level_streaming.add — attempt to add M_ScratchDup as sublevel of M_Scratch")
    persistent_path = "/Game/MCPTest/Phase3/M_Scratch"
    sublevel_path = "/Game/MCPTest/Phase3/M_ScratchDup"

    # Pre-check: list current streaming levels of FlecsMyMap.
    r = call("level_streaming.list", {"persistent_level_path": persistent_path})
    if not assert_ok(r, "T3 pre-list"):
        print("  SKIP T3/T5: cannot list FlecsMyMap streaming levels")
        skipped += 2
        added_ok = False
    else:
        already_streaming = [s["sublevel_path"] for s in r["result"].get("streaming", [])]
        print(f"  T3 pre-list: existing streaming entries: {already_streaming}")
        if sublevel_path in already_streaming:
            print(f"  SKIP T3 add: sublevel '{sublevel_path}' is already streaming — would expect -32014")
            # Test the PathInUse path:
            r2 = call("level_streaming.add", {
                "persistent_level_path": persistent_path,
                "sublevel_asset_path":   sublevel_path,
            })
            if assert_err(r2, -32014, "T3 PathInUse"):
                print(f"  PASS T3 (PathInUse path): expected -32014 received")
                passed += 1
                added_ok = False
            else:
                failed += 1
                added_ok = False
        else:
            r = call("level_streaming.add", {
                "persistent_level_path": persistent_path,
                "sublevel_asset_path":   sublevel_path,
                "transform": {"location": [100, 200, 300], "rotation": [0, 45, 0], "scale": [1, 1, 1]},
            })
            if assert_ok(r, "T3"):
                added_ok = True
                print(f"  PASS T3: added '{r['result']['sublevel_path']}' (class={r['result'].get('level_class','?')})")
                passed += 1
            else:
                added_ok = False
                failed += 1

    # ── T4: set_loaded toggling (uses an entry from the streaming list if any) ──
    print("[T4] level_streaming.set_loaded toggling")
    r = call("level_streaming.list", {})
    if not assert_ok(r, "T4 pre-list"):
        print("  SKIP T4: cannot list streaming levels")
        skipped += 1
    else:
        streaming = r["result"].get("streaming", [])
        if not streaming:
            print("  SKIP T4: no streaming levels in current editor world to toggle")
            skipped += 1
        else:
            target = streaming[0]["sublevel_path"]
            prior_loaded  = streaming[0].get("should_be_loaded")
            prior_visible = streaming[0].get("should_be_visible")
            print(f"  Target: '{target}'  prior should_be_loaded={prior_loaded} should_be_visible={prior_visible}")

            # Toggle to (not prior_loaded, not prior_visible)
            new_loaded  = not bool(prior_loaded)
            new_visible = not bool(prior_visible)
            r1 = call("level_streaming.set_loaded", {
                "sublevel_path": target,
                "loaded":  new_loaded,
                "visible": new_visible,
            })
            if not assert_ok(r1, "T4 set#1"):
                failed += 1
            else:
                pr = r1["result"].get("prior", {})
                nw = r1["result"].get("new", {})
                flushed = r1["result"].get("flushed")
                print(f"  set#1: prior={pr} new={nw} flushed={flushed}")
                # Restore original state
                r2 = call("level_streaming.set_loaded", {
                    "sublevel_path": target,
                    "loaded":  bool(prior_loaded),
                    "visible": bool(prior_visible),
                })
                if not assert_ok(r2, "T4 restore"):
                    failed += 1
                else:
                    pr2 = r2["result"].get("prior", {})
                    nw2 = r2["result"].get("new", {})
                    print(f"  set#2 (restore): prior={pr2} new={nw2}")
                    print(f"  PASS T4: round-trip toggled + restored")
                    passed += 1

    # ── T4b: set_loaded with bad sublevel_path → -32004 ──
    print("[T4b] level_streaming.set_loaded bad sublevel_path (expect -32004)")
    r = call("level_streaming.set_loaded", {
        "sublevel_path": "/Game/Maps/NoSuchSublevel_zZz9",
        "loaded": True,
    })
    if assert_err(r, -32004, "T4b"):
        print(f"  PASS T4b: error code -32004 as expected")
        passed += 1
    else:
        failed += 1

    # ── T5: level_streaming.remove (matched pair with T3 add) ──
    print("[T5] level_streaming.remove (cleanup added sublevel)")
    if added_ok:
        r = call("level_streaming.remove", {
            "persistent_level_path": persistent_path,
            "sublevel_asset_path":   sublevel_path,
        })
        if assert_ok(r, "T5"):
            print(f"  PASS T5: removed='{r['result'].get('removed')}' path='{r['result'].get('sublevel_path')}'")
            passed += 1
        else:
            failed += 1
    else:
        print("  SKIP T5: T3 did not add a sublevel")
        skipped += 1

    # ── T5b: level_streaming.remove non-existent → -32004 ──
    print("[T5b] level_streaming.remove non-existent sublevel (expect -32004)")
    r = call("level_streaming.remove", {
        "persistent_level_path": persistent_path,
        "sublevel_asset_path":   "/Game/Maps/NotAStreamingLevel_zZz9",
    })
    err = r.get("error")
    # If the persistent path itself can't be loaded, the tool surfaces -32004 anyway. Either is fine.
    if assert_err(r, -32004, "T5b"):
        print(f"  PASS T5b: error code -32004 as expected (msg: {err['message'][:80]})")
        passed += 1
    else:
        failed += 1

    # ── T6: missing required args → -32602 InvalidParams ──
    print("[T6] level_streaming.add missing required args (expect -32602)")
    r = call("level_streaming.add", {})
    if assert_err(r, -32602, "T6"):
        print(f"  PASS T6: error code -32602 as expected")
        passed += 1
    else:
        failed += 1

    print(f"\n=== Summary: {passed} PASS, {failed} FAIL, {skipped} SKIP ===")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
