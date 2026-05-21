"""Wave E Surface 4 — texture.* live smoke test.

Covers: list (pagination), get_info (size + format + compression + mip_count + srgb + lod_group +
lod_bias + never_stream + address_x/y), set_compression (round-trip), generate_solid_color
(success + PathInUse), set_compression bad value, get_info on non-texture, cleanup via cb.delete.
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


# ─── T1) texture.list page_size=10 ────────────────────────────────────────────
r1 = call("texture.list", {"page_size": 10})
assert_ok("T1 texture.list page_size=10",
          r1,
          lambda res: (
              len(res.get("textures", [])) <= 10 and "total_known" in res,
              f"textures count={len(res.get('textures', []))}, total_known={res.get('total_known')}",
          ))

total_textures = (r1.get("result") or {}).get("total_known", 0)
print(f"[info] total UTexture2D assets in project: {total_textures}")

first_tex_path = None
if r1.get("result") and r1["result"].get("textures"):
    first_tex_path = r1["result"]["textures"][0]["asset_path"]
    print(f"[info] first texture: {first_tex_path}")


# ─── T2) texture.list pagination round-trip ────────────────────────────────────
if total_textures >= 11:
    next_tok = r1["result"].get("next_page_token")
    if next_tok:
        r2 = call("texture.list", {"page_size": 10, "page_token": next_tok})
        ok2 = assert_ok("T2 texture.list page 2 token round-trip",
                        r2,
                        lambda res: (
                            len(res.get("textures", [])) > 0,
                            f"page 2 returned {len(res.get('textures', []))} items",
                        ))
        if ok2 and r1["result"]["textures"] and r2.get("result") and r2["result"].get("textures"):
            first_p1 = r1["result"]["textures"][0]["asset_path"]
            first_p2 = r2["result"]["textures"][0]["asset_path"]
            if first_p1 != first_p2:
                PASS.append("T2b texture.list pages distinct (page1[0] != page2[0])")
            else:
                FAIL.append(f"T2b texture.list pages NOT distinct: both start with {first_p1}")
    else:
        SKIP.append(f"T2 texture.list pagination — no next_page_token despite total_known={total_textures}")
else:
    SKIP.append(f"T2 texture.list pagination — only {total_textures} textures (need >=11)")


# ─── T3) texture.get_info on first texture ─────────────────────────────────────
prior_compression = None
if first_tex_path:
    r3 = call("texture.get_info", {"texture_path": first_tex_path})
    ok3 = assert_ok("T3 texture.get_info first texture",
                    r3,
                    lambda res: (
                        "size" in res
                        and isinstance(res["size"], list) and len(res["size"]) == 2
                        and "pixel_format" in res
                        and "compression" in res
                        and "mip_count" in res
                        and "srgb" in res
                        and "lod_group" in res
                        and "lod_bias" in res
                        and "never_stream" in res
                        and "address_x" in res
                        and "address_y" in res,
                        f"missing fields: {set(['size','pixel_format','compression','mip_count','srgb','lod_group','lod_bias','never_stream','address_x','address_y']) - set(res.keys())}",
                    ))
    if ok3 and r3.get("result"):
        res = r3["result"]
        prior_compression = res.get("compression")
        print(f"[info] {first_tex_path}: size={res.get('size')}, pixel_format={res.get('pixel_format')}, "
              f"compression={prior_compression}, mip_count={res.get('mip_count')}, srgb={res.get('srgb')}, "
              f"lod_group={res.get('lod_group')}, address=({res.get('address_x')},{res.get('address_y')})")
else:
    SKIP.append("T3 texture.get_info — no first texture available")


# ─── T4) texture.set_compression round-trip ────────────────────────────────────
if first_tex_path and prior_compression:
    # Choose a target distinct from prior (TC_Normalmap unless prior IS Normalmap, then use TC_Default)
    target_compression = "TC_Normalmap" if prior_compression != "TC_Normalmap" else "TC_Default"

    r4a = call("texture.set_compression", {
        "texture_path": first_tex_path,
        "compression_settings": target_compression,
        "update_resource": False,  # skip UpdateResource — just flipping the field for the round-trip
    })
    ok4a = assert_ok(f"T4a texture.set_compression {prior_compression} -> {target_compression}",
                     r4a,
                     lambda res: (
                         res.get("prior_compression") == prior_compression
                         and res.get("new_compression") == target_compression,
                         f"prior={res.get('prior_compression')} new={res.get('new_compression')} "
                         f"expected_prior={prior_compression} expected_new={target_compression}",
                     ))

    if ok4a:
        # Verify via get_info that the field actually changed
        r4b = call("texture.get_info", {"texture_path": first_tex_path})
        assert_ok(f"T4b verify compression now {target_compression}",
                  r4b,
                  lambda res: (
                      res.get("compression") == target_compression,
                      f"compression after set={res.get('compression')} expected={target_compression}",
                  ))

        # Revert
        r4c = call("texture.set_compression", {
            "texture_path": first_tex_path,
            "compression_settings": prior_compression,
            "update_resource": False,
        })
        assert_ok(f"T4c texture.set_compression {target_compression} -> {prior_compression} (revert)",
                  r4c,
                  lambda res: (
                      res.get("prior_compression") == target_compression
                      and res.get("new_compression") == prior_compression,
                      f"revert prior={res.get('prior_compression')} new={res.get('new_compression')}",
                  ))
else:
    SKIP.append("T4 texture.set_compression — no first texture / no prior_compression")


# ─── T5) texture.generate_solid_color ──────────────────────────────────────────
rand_suffix = random.randint(100000, 999999)
gen_path = f"/Game/MCPTest/Tex_E4_{rand_suffix}"
r5 = call("texture.generate_solid_color", {
    "dest_path": gen_path,
    "color": [1.0, 0.0, 0.0, 1.0],  # solid red
    "size": 8,
    "save": True,
})
gen_ok = assert_ok("T5 texture.generate_solid_color red 8x8",
                   r5,
                   lambda res: (
                       res.get("created") is True
                       and "asset_path" in res
                       and res.get("saved") is True
                       and res.get("size") == 8,
                       f"created={res.get('created')} saved={res.get('saved')} size={res.get('size')} "
                       f"asset_path={res.get('asset_path')}",
                   ))


# ─── T6) texture.get_info on the generated texture ─────────────────────────────
if gen_ok:
    r6 = call("texture.get_info", {"texture_path": gen_path})
    assert_ok("T6 texture.get_info on generated",
              r6,
              lambda res: (
                  isinstance(res.get("size"), list)
                  and res["size"] == [8, 8]
                  and res.get("compression") == "TC_Default",
                  f"size={res.get('size')} expected=[8,8] compression={res.get('compression')} expected=TC_Default",
              ))


# ─── T7) texture.generate_solid_color same path -> -32014 ──────────────────────
if gen_ok:
    r7 = call("texture.generate_solid_color", {
        "dest_path": gen_path,
        "color": [0.0, 1.0, 0.0, 1.0],
        "size": 8,
    })
    assert_error("T7 texture.generate_solid_color double -> -32014", r7, -32014)


# ─── T8) cleanup via cb.delete ─────────────────────────────────────────────────
if gen_ok:
    rdel = call("cb.delete", {"path": gen_path, "force": True})
    if rdel.get("error"):
        FAIL.append(f"T8 cleanup cb.delete {gen_path} -> {rdel['error']}")
    else:
        PASS.append("T8 cleanup cb.delete generated texture")


# ─── T9) texture.set_compression bad value -> -32602 ───────────────────────────
if first_tex_path:
    r9 = call("texture.set_compression", {
        "texture_path": first_tex_path,
        "compression_settings": "TC_BogusInvalidValue",
    })
    assert_error("T9 texture.set_compression bad enum -> -32602", r9, -32602)
else:
    SKIP.append("T9 texture.set_compression bad value — no first texture")


# ─── T10) texture.get_info on non-texture asset -> -32011 ──────────────────────
# DefaultMaterial is guaranteed to exist; pass through texture.get_info to confirm WrongClass.
r10 = call("texture.get_info", {"texture_path": "/Engine/EngineMaterials/DefaultMaterial"})
assert_error("T10 texture.get_info non-texture -> -32011", r10, -32011)


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
