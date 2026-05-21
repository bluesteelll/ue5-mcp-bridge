#!/usr/bin/env python3
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
def call(m, a=None, t=15): return send({"id":"t","kind":"call_function","method":m,"args":a or {}}, t)

PASS, FAIL = [], []
def ok(n, m=""): PASS.append(n); print(f"  PASS {n} {m}")
def fail(n, m=""): FAIL.append(n); print(f"  FAIL {n} {m}")

# T1: draw_line
print("=== T1: debug.draw_line ===")
r = call("debug.draw_line", {"start": [0,0,0], "end": [100,0,0], "color": [1,0,0,1], "thickness": 2.0, "lifetime": 5.0})
if r and r.get("ok"): ok("T1/line", f"world={r['result'].get('world','?')}")
else: fail("T1/line", f"{r}")

# T2: draw_sphere
print("\n=== T2: debug.draw_sphere ===")
r = call("debug.draw_sphere", {"center": [0,0,100], "radius": 50, "color": [0,1,0,1], "segments": 12, "lifetime": 5.0})
if r and r.get("ok"): ok("T2/sphere", "")
else: fail("T2/sphere", f"{r}")

# T3: draw_box
print("\n=== T3: debug.draw_box ===")
r = call("debug.draw_box", {"center": [200,0,0], "extent": [50,50,50], "color": [0,0,1,1], "lifetime": 5.0})
if r and r.get("ok"): ok("T3/box", "")
else: fail("T3/box", f"{r}")

# T3b: draw_box with rotation
print("\n=== T3b: debug.draw_box with rotation ===")
r = call("debug.draw_box", {"center": [300,0,0], "extent": [50,50,50], "rotation": [0,45,0], "color": [1,0,1,1], "lifetime": 5.0})
if r and r.get("ok"): ok("T3b/box_rotated", "")
else: fail("T3b/box_rotated", f"{r}")

# T4: draw_arrow
print("\n=== T4: debug.draw_arrow ===")
r = call("debug.draw_arrow", {"start": [0,0,0], "end": [0,200,0], "arrow_size": 30, "color": [1,1,0,1], "lifetime": 5.0})
if r and r.get("ok"): ok("T4/arrow", "")
else: fail("T4/arrow", f"{r}")

# T5: draw_text
print("\n=== T5: debug.draw_text ===")
r = call("debug.draw_text", {"location": [0,0,300], "text": "Hello MCP Debug!", "color": [1,1,1,1], "lifetime": 5.0})
if r and r.get("ok"): ok("T5/text", "")
else: fail("T5/text", f"{r}")

# T5b: draw_text with shadow + scale
print("\n=== T5b: debug.draw_text with shadow + font_scale ===")
r = call("debug.draw_text", {"location": [0,0,400], "text": "Shadowed Big Text", "color": [0.5,0.8,1.0,1], "lifetime": 5.0, "draw_shadow": True, "font_scale": 2.0})
if r and r.get("ok"): ok("T5b/text_extras", "")
else: fail("T5b/text_extras", f"{r}")

# T6: clear
print("\n=== T6: debug.clear ===")
r = call("debug.clear")
if r and r.get("ok"): ok("T6/clear", "")
else: fail("T6/clear", f"{r}")

# T7: invalid args (missing 'end')
print("\n=== T7: invalid args (missing 'end') → -32602 ===")
r = call("debug.draw_line", {"start": [0,0,0]})
if r and not r.get("ok") and r.get("error", {}).get("code") == -32602:
    ok("T7/invalid_missing", f"msg={r['error'].get('message','')[:60]}")
else: fail("T7/invalid_missing", f"{r}")

# T8: malformed vector (wrong array length)
print("\n=== T8: malformed vector (4 entries) → -32602 ===")
r = call("debug.draw_line", {"start": [0,0,0,0], "end": [100,0,0]})
if r and not r.get("ok") and r.get("error", {}).get("code") == -32602:
    ok("T8/invalid_shape", f"msg={r['error'].get('message','')[:60]}")
else: fail("T8/invalid_shape", f"{r}")

# T9: sphere missing radius
print("\n=== T9: sphere missing radius → -32602 ===")
r = call("debug.draw_sphere", {"center": [0,0,0]})
if r and not r.get("ok") and r.get("error", {}).get("code") == -32602:
    ok("T9/missing_radius", f"msg={r['error'].get('message','')[:60]}")
else: fail("T9/missing_radius", f"{r}")

# T10: text missing text field
print("\n=== T10: text missing 'text' field → -32602 ===")
r = call("debug.draw_text", {"location": [0,0,0]})
if r and not r.get("ok") and r.get("error", {}).get("code") == -32602:
    ok("T10/missing_text", f"msg={r['error'].get('message','')[:60]}")
else: fail("T10/missing_text", f"{r}")

# T11: tools.list — confirm 6 debug tools registered
print("\n=== T11: tools.list — check 6 debug.* tools ===")
r = call("tools.list", {}, t=20)
if r and r.get("ok"):
    cpp = r["result"].get("cpp_handlers", [])
    debug_tools = sorted([t for t in cpp if t.startswith("debug.")])
    expected = {"debug.draw_line", "debug.draw_sphere", "debug.draw_box", "debug.draw_arrow", "debug.draw_text", "debug.clear"}
    if set(debug_tools) == expected:
        ok("T11/registered", f"all 6 tools: {debug_tools}")
    else:
        fail("T11/registered", f"expected={expected}, got={debug_tools}")
else: fail("T11/registered", f"{r}")

print(f"\n{'='*60}")
print(f"WAVE D S2 TEST: {len(PASS)} PASS, {len(FAIL)} FAIL")
print(f"{'='*60}")
for n in FAIL: print(f"  - {n}")
sys.exit(0 if not FAIL else 1)
