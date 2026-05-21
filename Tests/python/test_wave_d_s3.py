"""Wave D Surface 3 — mesh.* live smoke test.

Covers: list (pagination + path_prefix), get_info, list_lods, set_material_slot (round-trip),
duplicate (success + -32014 PathInUse), set_material_slot OOB (-32026), get_info on non-mesh
(-32011). Cleanup via cb.delete force=true.
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
            FAIL.append(f"{name}: predicate failed — {msg}")
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


# ─── 1) mesh.list page_size=10 ────────────────────────────────────────────────
r1 = call("mesh.list", {"page_size": 10})
assert_ok("mesh.list page_size=10",
          r1,
          lambda res: (
              len(res.get("meshes", [])) <= 10 and "total_known" in res,
              f"meshes count={len(res.get('meshes', []))}, total_known={res.get('total_known')}",
          ))

total_meshes = (r1.get("result") or {}).get("total_known", 0)
print(f"[info] total UStaticMesh assets in project: {total_meshes}")

first_mesh_path = None
if r1.get("result") and r1["result"].get("meshes"):
    first_mesh_path = r1["result"]["meshes"][0]["asset_path"]
    print(f"[info] first mesh: {first_mesh_path}")


# ─── 2) mesh.list pagination round-trip ────────────────────────────────────────
if total_meshes >= 11:
    next_tok = r1["result"].get("next_page_token")
    if next_tok:
        r2 = call("mesh.list", {"page_size": 10, "page_token": next_tok})
        ok2 = assert_ok("mesh.list page 2 token round-trip",
                        r2,
                        lambda res: (
                            len(res.get("meshes", [])) > 0,
                            f"page 2 returned {len(res.get('meshes', []))} items",
                        ))
        if ok2 and r1["result"]["meshes"] and r2.get("result") and r2["result"].get("meshes"):
            first_p1 = r1["result"]["meshes"][0]["asset_path"]
            first_p2 = r2["result"]["meshes"][0]["asset_path"]
            if first_p1 != first_p2:
                PASS.append("mesh.list pages distinct (page1[0] != page2[0])")
            else:
                FAIL.append(f"mesh.list pages NOT distinct: both start with {first_p1}")
    else:
        SKIP.append(f"mesh.list pagination — no next_page_token despite total_known={total_meshes}")
else:
    SKIP.append(f"mesh.list pagination — only {total_meshes} meshes (need >=11)")


# ─── 3) mesh.list path_prefix filter ───────────────────────────────────────────
r3 = call("mesh.list", {"path_prefix": "/Game", "page_size": 5})
assert_ok("mesh.list path_prefix=/Game",
          r3,
          lambda res: (
              all(m["asset_path"].startswith("/Game") for m in res.get("meshes", [])),
              f"non-/Game items leaked: {[m['asset_path'] for m in res.get('meshes', []) if not m['asset_path'].startswith('/Game')]}",
          ))


# ─── 4) mesh.get_info ──────────────────────────────────────────────────────────
if first_mesh_path:
    r4 = call("mesh.get_info", {"mesh_path": first_mesh_path})
    assert_ok("mesh.get_info first mesh",
              r4,
              lambda res: (
                  "bounds" in res
                  and "lod_count" in res
                  and "material_slots" in res
                  and "vertex_count" in res
                  and "triangle_count" in res
                  and "source_model_count" in res,
                  f"missing fields: {set(['bounds','lod_count','material_slots','vertex_count','triangle_count','source_model_count']) - set(res.keys())}",
              ))
    if r4.get("result"):
        b = r4["result"].get("bounds", {})
        slot_count = len(r4["result"].get("material_slots", []))
        print(f"[info] {first_mesh_path}: lod_count={r4['result'].get('lod_count')}, "
              f"vertex_count={r4['result'].get('vertex_count')}, "
              f"slot_count={slot_count}, "
              f"sphere_radius={b.get('sphere_radius')}")
else:
    SKIP.append("mesh.get_info — no first mesh available")


# ─── 5) mesh.list_lods ─────────────────────────────────────────────────────────
if first_mesh_path:
    r5 = call("mesh.list_lods", {"mesh_path": first_mesh_path})
    assert_ok("mesh.list_lods first mesh",
              r5,
              lambda res: (
                  "lods" in res and isinstance(res["lods"], list),
                  f"lods missing or wrong type: {res.get('lods')}",
              ))
    if r5.get("result") and r5["result"].get("lods"):
        print(f"[info] LODs: {len(r5['result']['lods'])} (LOD0 verts={r5['result']['lods'][0].get('vertex_count')})")


# ─── 6) mesh.set_material_slot round-trip ──────────────────────────────────────
mesh_with_slots = None
test_slot_idx = None
test_prior_mat = None
test_new_mat = None
if first_mesh_path and r4.get("result"):
    slots = r4["result"].get("material_slots", [])
    if slots:
        # Find a slot with an existing material to round-trip with itself (safest — any non-empty slot
        # works because we restore to the SAME material at the end).
        for s in slots:
            if s.get("material_path"):
                mesh_with_slots = first_mesh_path
                test_slot_idx = s["slot_index"]
                test_prior_mat = s["material_path"]
                test_new_mat = test_prior_mat  # re-set same material ->trivially safe round-trip
                break

if mesh_with_slots and test_new_mat:
    # Round-trip: set to existing material, verify prior matches, then nothing to restore.
    r6 = call("mesh.set_material_slot", {
        "mesh_path": mesh_with_slots,
        "slot_index": test_slot_idx,
        "material_path": test_new_mat,
    })
    assert_ok(f"mesh.set_material_slot[{test_slot_idx}]=self",
              r6,
              lambda res: (
                  res.get("prior_material") == test_prior_mat
                  and res.get("new_material") == test_new_mat,
                  f"prior={res.get('prior_material')} new={res.get('new_material')} expected_prior={test_prior_mat}",
              ))
else:
    SKIP.append("mesh.set_material_slot — no mesh with non-empty material slot found")


# ─── 7) mesh.duplicate ─────────────────────────────────────────────────────────
dup_path = None
if first_mesh_path:
    rand_suffix = random.randint(100000, 999999)
    dup_path = f"/Game/MCPTest/Mesh_Tier3_Test_{rand_suffix}"
    r7 = call("mesh.duplicate", {
        "source_mesh_path": first_mesh_path,
        "dest_path": dup_path,
        "save": True,  # save to disk so cb.delete (needs on-disk package) can clean up
    })
    dup_ok = assert_ok("mesh.duplicate success",
                       r7,
                       lambda res: (
                           res.get("created") is True and "asset_path" in res and res.get("saved") is True,
                           f"created={res.get('created')} saved={res.get('saved')} asset_path={res.get('asset_path')}",
                       ))

    if dup_ok:
        # Second call to same path ->-32014 PathInUse
        r7b = call("mesh.duplicate", {
            "source_mesh_path": first_mesh_path,
            "dest_path": dup_path,
        })
        assert_error("mesh.duplicate double-create ->-32014", r7b, -32014)

        # Cleanup
        rdel = call("cb.delete", {"path": dup_path, "force": True})
        if rdel.get("error"):
            FAIL.append(f"cleanup cb.delete {dup_path} ->{rdel['error']}")
        else:
            PASS.append("cleanup cb.delete duplicated mesh")
else:
    SKIP.append("mesh.duplicate — no source mesh available")


# ─── 8) mesh.set_material_slot OOB (-32026) ────────────────────────────────────
if first_mesh_path:
    # Try slot 999 — virtually guaranteed OOB. Need a valid material_path to get past the load
    # step; reuse test_new_mat if we have one, otherwise fabricate a path (validation order in the
    # tool is: load mesh ->check slot range ->load material; OOB triggers BEFORE material load).
    mat_arg = test_new_mat if test_new_mat else "/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial"
    r8 = call("mesh.set_material_slot", {
        "mesh_path": first_mesh_path,
        "slot_index": 999,
        "material_path": mat_arg,
    })
    assert_error("mesh.set_material_slot slot=999 ->-32026", r8, -32026)
else:
    SKIP.append("mesh.set_material_slot OOB — no first mesh")


# ─── 9) mesh.get_info on non-mesh asset ->-32011 ───────────────────────────────
# A material is virtually certain to exist; use /Engine/EngineMaterials/DefaultMaterial.
r9 = call("mesh.get_info", {"mesh_path": "/Engine/EngineMaterials/DefaultMaterial"})
assert_error("mesh.get_info non-mesh ->-32011", r9, -32011)


# ─── Summary ──────────────────────────────────────────────────────────────────
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
