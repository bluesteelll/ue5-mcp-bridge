"""Wave E Surface 5 — input.* live smoke test.

Covers: list_mapping_contexts (pagination + path_prefix), list_input_actions, get_context_bindings
(round-trip on first IMC), list_player_contexts (editor world — likely no PC; documents that),
get_context_bindings WrongClass (-32011), list_mapping_contexts pagination round-trip.
"""

import io
import json
import random
import socket
import sys
import time

# Force UTF-8 on Windows console so any embedded unicode in tool messages doesn't blow up.
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
sys.stderr = io.TextIOWrapper(sys.stderr.buffer, encoding="utf-8", errors="replace")

HOST = "127.0.0.1"
PORT = 30020
TIMEOUT = 10.0

PASS = []
FAIL = []
SKIP = []


def call(method, args=None, idtok=None):
    """Send one MCP request, return parsed response dict."""
    payload = {
        "id": idtok or f"t{random.randint(0, 1_000_000)}",
        "kind": "call_function",
        "method": method,
        "args": args or {},
    }
    s = socket.create_connection((HOST, PORT), timeout=TIMEOUT)
    s.sendall((json.dumps(payload) + "\n").encode())
    buf = b""
    deadline = time.time() + TIMEOUT
    while time.time() < deadline:
        chunk = s.recv(65536)
        if not chunk:
            break
        buf += chunk
        if b"\n" in buf:
            break
    s.close()
    line = buf.split(b"\n", 1)[0]
    return json.loads(line.decode())


def assert_ok(name, resp, predicate=None):
    if resp.get("error"):
        FAIL.append(f"{name}: ERROR {resp['error']}")
        return False
    if predicate is not None:
        ok, msg = predicate(resp.get("result"))
        if not ok:
            FAIL.append(f"{name}: predicate failed - {msg}")
            return False
    PASS.append(name)
    return True


def assert_error(name, resp, expected_code):
    err = resp.get("error")
    if not err:
        FAIL.append(f"{name}: expected error {expected_code}, got success {resp.get('result')}")
        return False
    if err.get("code") != expected_code:
        FAIL.append(f"{name}: expected code {expected_code}, got {err.get('code')} ({err.get('message')})")
        return False
    PASS.append(f"{name} (code={expected_code})")
    return True


# T1: input.list_mapping_contexts page_size=10
r1 = call("input.list_mapping_contexts", {"page_size": 10})
assert_ok("input.list_mapping_contexts page_size=10",
          r1,
          lambda res: (
              isinstance(res.get("mapping_contexts"), list) and "total_known" in res,
              f"mapping_contexts type={type(res.get('mapping_contexts'))}, total_known={res.get('total_known')}",
          ))

total_imc = (r1.get("result") or {}).get("total_known", 0)
print(f"[info] total UInputMappingContext assets in project: {total_imc}")

first_imc_path = None
if r1.get("result") and r1["result"].get("mapping_contexts"):
    first_imc_path = r1["result"]["mapping_contexts"][0]["asset_path"]
    print(f"[info] first IMC: {first_imc_path}")


# T2: input.list_input_actions page_size=10
r2 = call("input.list_input_actions", {"page_size": 10})
assert_ok("input.list_input_actions page_size=10",
          r2,
          lambda res: (
              isinstance(res.get("input_actions"), list) and "total_known" in res,
              f"input_actions type={type(res.get('input_actions'))}, total_known={res.get('total_known')}",
          ))

total_ia = (r2.get("result") or {}).get("total_known", 0)
print(f"[info] total UInputAction assets in project: {total_ia}")


# T3: input.get_context_bindings on first IMC
if first_imc_path:
    r3 = call("input.get_context_bindings", {"mapping_context_path": first_imc_path})
    assert_ok(f"input.get_context_bindings({first_imc_path})",
              r3,
              lambda res: (
                  res.get("mapping_context") is not None
                  and isinstance(res.get("mappings"), list)
                  and "mapping_count" in res,
                  f"mapping_context={res.get('mapping_context')}, mappings_count={len(res.get('mappings', []))}",
              ))
    if r3.get("result"):
        mc = r3["result"].get("mapping_count", 0)
        mappings = r3["result"].get("mappings", [])
        print(f"[info] {first_imc_path}: mapping_count={mc}")
        if mappings:
            sample = mappings[0]
            print(f"[info] sample mapping: action={sample.get('action')}, key={sample.get('key')}, "
                  f"modifiers={sample.get('modifiers')}, triggers={sample.get('triggers')}")
else:
    SKIP.append("input.get_context_bindings - no first IMC available")


# T4: input.list_player_contexts (no args; editor world likely no PC)
r4 = call("input.list_player_contexts", {})
ok4 = assert_ok("input.list_player_contexts (default)",
                r4,
                lambda res: (
                    isinstance(res.get("contexts"), list) and "context_count" in res,
                    f"contexts type={type(res.get('contexts'))}, context_count={res.get('context_count')}",
                ))
if ok4 and r4.get("result"):
    pc = r4["result"].get("player_controller")
    cc = r4["result"].get("context_count")
    hint = r4["result"].get("hint")
    print(f"[info] player_controller={pc!r}, context_count={cc}, hint={hint!r}")


# T5: input.get_context_bindings on non-IMC asset (use any input action asset) -> -32011 WrongClass
if total_ia > 0 and r2.get("result") and r2["result"].get("input_actions"):
    non_imc_path = r2["result"]["input_actions"][0]["asset_path"]
    r5 = call("input.get_context_bindings", {"mapping_context_path": non_imc_path})
    assert_error(f"input.get_context_bindings(IA asset -> -32011)", r5, -32011)
else:
    # Fallback: use a known engine asset that is not an IMC
    r5 = call("input.get_context_bindings",
              {"mapping_context_path": "/Engine/EngineMaterials/DefaultMaterial"})
    assert_error("input.get_context_bindings(non-IMC -> -32011)", r5, -32011)


# T6: input.list_mapping_contexts pagination round-trip (skip if <11 IMCs)
if total_imc >= 11:
    next_tok = r1["result"].get("next_page_token")
    if next_tok:
        r6 = call("input.list_mapping_contexts", {"page_size": 10, "page_token": next_tok})
        ok6 = assert_ok("input.list_mapping_contexts page 2 token round-trip",
                        r6,
                        lambda res: (
                            len(res.get("mapping_contexts", [])) > 0,
                            f"page 2 returned {len(res.get('mapping_contexts', []))} items",
                        ))
        if (ok6 and r1["result"]["mapping_contexts"]
                and r6.get("result") and r6["result"].get("mapping_contexts")):
            first_p1 = r1["result"]["mapping_contexts"][0]["asset_path"]
            first_p2 = r6["result"]["mapping_contexts"][0]["asset_path"]
            if first_p1 != first_p2:
                PASS.append("input.list_mapping_contexts pages distinct (p1[0] != p2[0])")
            else:
                FAIL.append(f"input.list_mapping_contexts pages NOT distinct: both start with {first_p1}")
    else:
        SKIP.append(f"input.list_mapping_contexts pagination - no next_page_token despite total_known={total_imc}")
else:
    SKIP.append(f"input.list_mapping_contexts pagination - only {total_imc} IMCs (need >=11)")


# T7: input.get_context_bindings invalid path -> -32010
r7 = call("input.get_context_bindings", {"mapping_context_path": "not_a_valid_path"})
assert_error("input.get_context_bindings(invalid path -> -32010)", r7, -32010)


# T8: input.list_mapping_contexts with path_prefix filter
r8 = call("input.list_mapping_contexts", {"path_prefix": "/Game", "page_size": 50})
assert_ok("input.list_mapping_contexts path_prefix=/Game",
          r8,
          lambda res: (
              all(m["asset_path"].startswith("/Game") for m in res.get("mapping_contexts", [])),
              f"non-/Game items leaked: {[m['asset_path'] for m in res.get('mapping_contexts', []) if not m['asset_path'].startswith('/Game')]}",
          ))


# Summary
print()
print("=" * 72)
print(f"PASS: {len(PASS)}")
for p in PASS:
    print(f"  [ok] {p}")
print(f"FAIL: {len(FAIL)}")
for f in FAIL:
    print(f"  [FAIL] {f}")
print(f"SKIP: {len(SKIP)}")
for s in SKIP:
    print(f"  [skip] {s}")
print("=" * 72)
sys.exit(0 if not FAIL else 1)
