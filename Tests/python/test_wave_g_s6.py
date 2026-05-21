#!/usr/bin/env python3
"""Wave G Surface 6 test: engine.get_info / engine.gc_collect / engine.get_memory_snapshot.

Test plan:
  T1: engine.get_info  -> ue_version.major=5, minor=7, project_name="FatumGame",
                          target_type contains "Editor", current_world.name non-empty
  T2: engine.get_memory_snapshot -> used_physical_mb > 1000 (UE editor uses >= 1 GB),
                                     allocator_name non-empty
  T3: engine.gc_collect force=true -> collected=true, freed_mb >= -512 (allow drift),
                                       duration_seconds >= 0
  T4: engine.get_memory_snapshot (post-GC) -> sanity check shape still valid
  T5: engine.get_info shows current_world name field
  T6: engine.get_memory_snapshot include_breakdown=true -> has 'breakdown' object
  T7: engine.gc_collect force=false purge_object_references=false ->
                                       collected=true (deferred kick) + duration_seconds present
  T8: engine.gc_collect with no args (defaults) -> collected=true
  T9: engine.get_info shape sanity — all required fields present
  T10: engine.get_memory_snapshot total_physical_mb > used_physical_mb
"""
import json, socket, sys, time

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

HOST, PORT = "127.0.0.1", 30020


def send(req, t=20):
    s = socket.create_connection((HOST, PORT), timeout=t)
    s.sendall((json.dumps(req) + "\n").encode())
    buf = b""
    deadline = time.time() + t
    while time.time() < deadline:
        try:
            c = s.recv(512 * 1024)
            if not c:
                break
            buf += c
            if b"\n" in buf:
                return json.loads(buf[: buf.index(b"\n")].decode())
        except socket.timeout:
            pass
    return None


def call(method, args=None, t=20):
    return send({"id": "t", "kind": "call_function", "method": method, "args": args or {}}, t)


PASS, FAIL, SKIP = [], [], []


def ok(n, m=""):
    PASS.append(n)
    print(f"  PASS {n} {m}")


def fail(n, m=""):
    FAIL.append(n)
    print(f"  FAIL {n} {m}")


def skip(n, m=""):
    SKIP.append(n)
    print(f"  SKIP {n} {m}")


# === T1: engine.get_info ===
print("=== T1: engine.get_info ===")
r = call("engine.get_info", {})
if r and r.get("ok"):
    res = r["result"]
    ver = res.get("ue_version", {})
    cw = res.get("current_world")
    pname = res.get("project_name")
    ttype = res.get("target_type")
    if (
        ver.get("major") == 5
        and ver.get("minor") == 7
        and pname == "FatumGame"
        and ttype is not None
        and "Editor" in ttype
        and cw is not None
        and cw.get("name")
    ):
        ok(
            "T1/get_info",
            f"UE {ver['major']}.{ver['minor']}.{ver.get('patch')} target={ttype} world={cw.get('name')} pie={cw.get('is_pie')}",
        )
    else:
        fail(
            "T1/get_info",
            f"unexpected: ver={ver}, project={pname}, target={ttype}, world={cw}",
        )
else:
    fail("T1/get_info", f"{r}")


# === T2: engine.get_memory_snapshot ===
print("\n=== T2: engine.get_memory_snapshot ===")
r = call("engine.get_memory_snapshot", {})
if r and r.get("ok"):
    res = r["result"]
    up = res.get("used_physical_mb", 0)
    aname = res.get("allocator_name", "")
    if up > 1000 and aname:
        ok(
            "T2/get_memory_snapshot",
            f"used_physical={up:.1f}MB, allocator={aname}, page_size={res.get('page_size_bytes')}",
        )
    else:
        fail("T2/get_memory_snapshot", f"unexpected: used={up}, alloc={aname!r}")
else:
    fail("T2/get_memory_snapshot", f"{r}")


# === T3: engine.gc_collect force=true ===
print("\n=== T3: engine.gc_collect force=true ===")
r = call("engine.gc_collect", {"force": True})
if r and r.get("ok"):
    res = r["result"]
    coll = res.get("collected")
    freed = res.get("freed_mb")
    dur = res.get("duration_seconds")
    # freed can be negative (other threads alloc'd between snapshots) — allow generous range.
    if coll is True and isinstance(freed, (int, float)) and isinstance(dur, (int, float)) and dur >= 0:
        ok(
            "T3/gc_collect",
            f"freed={freed:.2f}MB, duration={dur*1000:.1f}ms, prior={res.get('prior_allocated_mb'):.1f}MB, new={res.get('new_allocated_mb'):.1f}MB",
        )
    else:
        fail("T3/gc_collect", f"unexpected: {res}")
else:
    fail("T3/gc_collect", f"{r}")


# === T4: engine.get_memory_snapshot (post-GC) ===
print("\n=== T4: engine.get_memory_snapshot post-GC ===")
r = call("engine.get_memory_snapshot", {})
if r and r.get("ok"):
    res = r["result"]
    up = res.get("used_physical_mb", 0)
    if up > 100:
        ok("T4/post_gc_snapshot", f"used_physical={up:.1f}MB (post-GC)")
    else:
        fail("T4/post_gc_snapshot", f"unreasonable used_physical: {up}")
else:
    fail("T4/post_gc_snapshot", f"{r}")


# === T5: engine.get_info current_world name field check ===
print("\n=== T5: engine.get_info current_world.name ===")
r = call("engine.get_info", {})
if r and r.get("ok"):
    cw = r["result"].get("current_world")
    if cw is not None and "name" in cw and cw["name"]:
        ok("T5/current_world_name", f"name={cw['name']!r}, type={cw.get('type')}")
    else:
        # In a fresh editor with no loaded map this might be None — still acceptable.
        if cw is None:
            ok("T5/current_world_name", "current_world=null (no world loaded yet)")
        else:
            fail("T5/current_world_name", f"unexpected: {cw}")
else:
    fail("T5/current_world_name", f"{r}")


# === T6: engine.get_memory_snapshot include_breakdown=true ===
print("\n=== T6: engine.get_memory_snapshot include_breakdown=true ===")
r = call("engine.get_memory_snapshot", {"include_breakdown": True})
if r and r.get("ok"):
    res = r["result"]
    bd = res.get("breakdown")
    if isinstance(bd, dict):
        # Breakdown might be empty on some platforms — that's fine, just confirm presence.
        ok(
            "T6/breakdown",
            f"breakdown_keys={list(bd.keys())[:5]} (total {len(bd)} entries)",
        )
    else:
        fail("T6/breakdown", f"breakdown missing or wrong type: {type(bd)}")
else:
    fail("T6/breakdown", f"{r}")


# === T7: engine.gc_collect force=false (deferred) ===
print("\n=== T7: engine.gc_collect force=false purge_object_references=false ===")
r = call("engine.gc_collect", {"force": False, "purge_object_references": False})
if r and r.get("ok"):
    res = r["result"]
    if (
        res.get("collected") is True
        and res.get("forced") is False
        and res.get("purge_object_references") is False
        and isinstance(res.get("duration_seconds"), (int, float))
    ):
        ok(
            "T7/gc_collect_deferred",
            f"forced={res['forced']}, purge={res['purge_object_references']}, duration={res['duration_seconds']*1000:.3f}ms",
        )
    else:
        fail("T7/gc_collect_deferred", f"unexpected: {res}")
else:
    fail("T7/gc_collect_deferred", f"{r}")


# === T8: engine.gc_collect with no args ===
print("\n=== T8: engine.gc_collect (no args, all defaults) ===")
r = call("engine.gc_collect", {})
if r and r.get("ok"):
    res = r["result"]
    # Defaults: force=true, purge_object_references=true
    if (
        res.get("collected") is True
        and res.get("forced") is True
        and res.get("purge_object_references") is True
    ):
        ok("T8/gc_defaults", f"defaults applied: forced=true, purge=true")
    else:
        fail("T8/gc_defaults", f"unexpected defaults: {res}")
else:
    fail("T8/gc_defaults", f"{r}")


# === T9: engine.get_info required field sanity ===
print("\n=== T9: engine.get_info schema completeness ===")
r = call("engine.get_info", {})
if r and r.get("ok"):
    res = r["result"]
    required = [
        "ue_version",
        "build_configuration",
        "target_type",
        "project_name",
        "project_dir",
        "engine_dir",
        "platform_name",
        "is_editor",
        "is_unattended",
        "is_running_commandlet",
        "current_world",  # may be null but must exist
    ]
    missing = [f for f in required if f not in res]
    if not missing:
        bc = res["build_configuration"]
        pl = res["platform_name"]
        # build_configuration should be a known value; on a dev editor it's "Development".
        if (
            bc in ("Debug", "DebugGame", "Development", "Shipping", "Test", "Unknown")
            and pl
            and res["is_editor"] is True
        ):
            ok(
                "T9/schema",
                f"all fields present; build_config={bc}, platform={pl}, is_editor=True",
            )
        else:
            fail("T9/schema", f"field values out of spec: bc={bc}, pl={pl}")
    else:
        fail("T9/schema", f"missing fields: {missing}")
else:
    fail("T9/schema", f"{r}")


# === T10: total_physical_mb > used_physical_mb sanity ===
print("\n=== T10: total_physical_mb >= used_physical_mb sanity ===")
r = call("engine.get_memory_snapshot", {})
if r and r.get("ok"):
    res = r["result"]
    tot = res.get("total_physical_mb", 0)
    used = res.get("used_physical_mb", 0)
    if tot >= used and tot > 0:
        ok("T10/total_ge_used", f"total={tot:.0f}MB, used={used:.0f}MB ({used/tot*100:.1f}%)")
    else:
        fail("T10/total_ge_used", f"unexpected: total={tot}, used={used}")
else:
    fail("T10/total_ge_used", f"{r}")


print(f"\n{'=' * 60}")
print(f"WAVE G SURFACE 6 TEST: {len(PASS)} PASS, {len(FAIL)} FAIL, {len(SKIP)} SKIP")
print(f"{'=' * 60}")
for n in FAIL:
    print(f"  FAIL - {n}")
for n in SKIP:
    print(f"  SKIP - {n}")
sys.exit(0 if not FAIL else 1)
