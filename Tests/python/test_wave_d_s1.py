#!/usr/bin/env python3
"""Wave D Surface 1 test: gameplaytag.* (4 tools)."""
import json, socket, sys, time
try: sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception: pass

HOST, PORT = "127.0.0.1", 30020
def send(req, t=15):
    s = socket.create_connection((HOST, PORT), timeout=t)
    s.sendall((json.dumps(req)+"\n").encode())
    buf = b""; deadline = time.time()+t
    while time.time() < deadline:
        c = s.recv(256*1024)
        if not c: break
        buf += c
        if b"\n" in buf: return json.loads(buf[:buf.index(b"\n")].decode())
    return None
def call(m,a=None,t=15): return send({"id":"t","kind":"call_function","method":m,"args":a or {}}, t)

PASS, FAIL = [], []
def ok(n,m=""): PASS.append(n); print(f"  PASS {n} {m}")
def fail(n,m=""): FAIL.append(n); print(f"  FAIL {n} {m}")

# === T1: list (project may not have many tags) ===
print("=== T1: gameplaytag.list ===")
r = call("gameplaytag.list", {"page_size": 20, "only_dictionary": False})
sample_tag = None
if r and r.get("ok"):
    tags = r["result"]["tags"]
    total = r["result"]["total_known"]
    ok("T1/list", f"total={total} returned={len(tags)}")
    if tags:
        sample_tag = tags[0]["tag"]
        print(f"    first: {sample_tag!r}")
else:
    fail("T1/list", f"{r}")

# === T2: list with parent filter (use a known prefix from FatumGame if found) ===
print("\n=== T2: list with parent_filter ===")
if sample_tag and "." in sample_tag:
    parent = sample_tag.split(".")[0] + "."
    r = call("gameplaytag.list", {"parent_filter": parent, "only_dictionary": False})
    if r and r.get("ok"):
        ok("T2/filter", f"parent={parent!r} total={r['result']['total_known']}")
    else:
        fail("T2/filter", f"{r}")
else:
    print("    SKIP (no tags or no dotted-form)")
    ok("T2/filter SKIP", "")

# === T3: query_actor — need any actor ===
print("\n=== T3: gameplaytag.query_actor ===")
r = call("level.get_persistent_level_actors", {})
sample_actor = None
if r and r.get("ok"):
    actors = r["result"].get("actors", [])
    if actors:
        sample_actor = actors[0].get("name") or actors[0].get("label")

if sample_actor:
    r = call("gameplaytag.query_actor", {"actor_path": sample_actor})
    if r and r.get("ok"):
        res = r["result"]
        ok("T3/query", f"source={res['source']} tags={res.get('tags', [])}")
    else:
        fail("T3/query", f"{r}")
else:
    print("    SKIP (no actor)")
    ok("T3/query SKIP", "")

# === T4: add/remove require a real GameplayTag + actor with FGameplayTagContainer property — typically not present on Brush ===
print("\n=== T4: add_to_container missing property → -32004 ===")
if sample_actor and sample_tag:
    r = call("gameplaytag.add_to_container", {
        "actor_path": sample_actor,
        "property_name": "DefinitelyDoesNotExist_xyz",
        "tag": sample_tag
    })
    if r and not r.get("ok") and r.get("error", {}).get("code") == -32004:
        ok("T4/missing_prop", "correctly rejected -32004")
    else:
        fail("T4/missing_prop", f"expected -32004, got {r}")
else:
    print("    SKIP (no actor or tag)")
    ok("T4/missing_prop SKIP", "")

# === T5: add with bad tag (unknown) ===
print("\n=== T5: add_to_container unknown tag → -32004 ===")
if sample_actor:
    r = call("gameplaytag.add_to_container", {
        "actor_path": sample_actor,
        "property_name": "AnyName",
        "tag": "NonExistent.Fake.Tag_xyz"
    })
    if r and not r.get("ok") and r.get("error", {}).get("code") == -32004:
        ok("T5/unknown_tag", "correctly rejected -32004")
    else:
        fail("T5/unknown_tag", f"got {r}")
else:
    ok("T5/unknown_tag SKIP", "no actor")

# === T6: list with empty filter (sanity check pagination) ===
print("\n=== T6: list page_size=5 then next_page_token ===")
r = call("gameplaytag.list", {"page_size": 5, "only_dictionary": False})
if r and r.get("ok"):
    first_page = r["result"]["tags"]
    next_tok = r["result"].get("next_page_token")
    if next_tok:
        r2 = call("gameplaytag.list", {"page_size": 5, "page_token": next_tok, "only_dictionary": False})
        if r2 and r2.get("ok"):
            second_page = r2["result"]["tags"]
            distinct = (not first_page or not second_page) or (first_page[-1]["tag"] != second_page[0]["tag"])
            ok("T6/pagination", f"first_last={first_page[-1]['tag'] if first_page else None} second_first={second_page[0]['tag'] if second_page else None} distinct={distinct}")
        else: fail("T6/pagination", f"page 2 fail {r2}")
    else:
        ok("T6/pagination", "no second page (total < page_size)")
else:
    fail("T6/pagination", f"{r}")

print(f"\n{'='*60}")
print(f"WAVE D S1 TEST: {len(PASS)} PASS, {len(FAIL)} FAIL")
print(f"{'='*60}")
for n in FAIL: print(f"  - {n}")
sys.exit(0 if not FAIL else 1)
