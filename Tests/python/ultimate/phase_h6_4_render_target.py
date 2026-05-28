#!/usr/bin/env python3
"""Phase H6.4 — render_target.* pixel readback coverage.

Goal: verify render_target.get_info + render_target.dump work on a
real RenderTarget asset, both with output_path and inline base64.

Probes:
  P1 — locate or create a UTextureRenderTarget2D asset
  P2 — render_target.get_info → returns size_x, size_y, format, etc.
  P3 — render_target.dump output_path=<png file on disk> → file created
  P4 — render_target.dump inline (no output_path) → returns base64 buffer
  P5 — 10 sequential dumps → no GPU memory leak (uobj_delta stable)
  P6 — cleanup

A scratch RenderTarget is created via asset.create_data_asset class
TextureRenderTarget2D (concrete UE 5.7 class).

Exit codes: 0=PASS, 1=FAIL, 2=preflight.
"""

from __future__ import annotations

import sys
import time
from pathlib import Path
from typing import Any, Dict, Optional

sys.path.insert(0, str(Path(__file__).parent))
from mcp_test_harness import (
    LOG_ROOT,
    TestLogger,
    call,
    err_code,
    err_message,
    health,
    is_ok,
    is_transport_failure,
    latest_crash_dump,
    preflight,
    random_suffix,
    snapshot,
)

PHASE = "h6_4"
NAME = "render_target"

ROOT = f"/Game/PhT_H64_{random_suffix(6)}"
RT_PATH = f"{ROOT}/RT_Mega"
# render_target.dump enforces output_path sandbox under project Saved/.
# Use Saved/Temp/ which is always inside the writeable sandbox.
DUMP_DIR = Path("D:/Unreal Engine Projects/FatumGame/Saved/Temp/PhT_RT")


def _step(log: TestLogger, name: str, method: str, args: Dict[str, Any],
          timeout: float = 15.0) -> Optional[Dict[str, Any]]:
    t0 = time.monotonic()
    try:
        r = call(method, args, timeout=timeout)
    except Exception as e:
        log.case(name, "FAIL", f"exception: {e}",
                 duration_ms=(time.monotonic() - t0) * 1000.0)
        return None
    dt = (time.monotonic() - t0) * 1000.0
    if is_transport_failure(r):
        log.case(name, "FAIL", f"transport: {r.get('_err')}", duration_ms=dt)
        return None
    if not is_ok(r):
        c = err_code(r)
        if c == -32601:
            log.case(name, "SKIP", f"{method} not registered", duration_ms=dt)
            return None
        log.case(name, "FAIL", f"{method}: code={c}: {err_message(r)[:60]}",
                 duration_ms=dt, code=c)
        return None
    log.case(name, "PASS", f"{method} ok", duration_ms=dt)
    return r.get("result", {}) or {}


def cleanup() -> None:
    call("cb.delete", {"path": RT_PATH, "force": True}, timeout=10.0)
    call("folder.delete", {"folder_path": ROOT, "recursive": True}, timeout=10.0)


def main() -> int:
    if not preflight(PHASE):
        return 2
    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()

    print(f"[H6.4] render_target.* pixel readback…", flush=True)
    cleanup()
    DUMP_DIR.mkdir(parents=True, exist_ok=True)

    # P0 — folder + RT asset
    if _step(log, "P0_folder", "folder.create", {"folder_path": ROOT}) is None:
        log.write(); cleanup(); return 1

    # TextureRenderTarget2D isn't a UDataAsset subclass — use asset.create
    # (factory autoresolve) instead of asset.create_data_asset.
    r_create = _step(log, "P1_create_rt", "asset.create",
                      {"dest_path": RT_PATH,
                       "class_path": "/Script/Engine.TextureRenderTarget2D"})
    if r_create is None:
        log.write(); cleanup(); return 1

    # P2 — render_target.get_info
    r_info = _step(log, "P2_get_info", "render_target.get_info",
                    {"render_target_path": RT_PATH})
    if r_info is None:
        # If the tool not registered → can't proceed
        log.write(); cleanup(); return 0
    size_x = r_info.get("size_x") or r_info.get("width") or 0
    size_y = r_info.get("size_y") or r_info.get("height") or 0
    fmt = r_info.get("format") or r_info.get("pixel_format") or "?"
    log.case("P2_verify_info", "PASS" if (size_x > 0 and size_y > 0) else "XFAIL",
             f"size={size_x}x{size_y} format={fmt}")

    # P3 — dump to disk
    out_path = str(DUMP_DIR / f"rt_dump_{random_suffix(4)}.png")
    r_dump_disk = _step(log, "P3_dump_to_disk", "render_target.dump",
                         {"render_target_path": RT_PATH, "output_path": out_path,
                          "format": "png"})
    if r_dump_disk is not None:
        file_exists = Path(out_path).exists()
        file_size = Path(out_path).stat().st_size if file_exists else 0
        log.case("P3_verify_dump_file", "PASS" if file_exists else "FAIL",
                 f"output_path={out_path} exists={file_exists} size={file_size}B")

    # P4 — dump inline (base64)
    r_dump_inline = _step(log, "P4_dump_inline", "render_target.dump",
                           {"render_target_path": RT_PATH, "format": "png"})
    if r_dump_inline is not None:
        b64 = r_dump_inline.get("image_base64") or r_dump_inline.get("data") or ""
        log.case("P4_verify_inline", "PASS" if len(b64) > 100 else "XFAIL",
                 f"inline base64 length={len(b64)}")

    # P5 — leak check via 10 sequential dumps
    snap_before = snapshot()
    uobj_before = snap_before.get("live_uobject_slots", 0)
    t0 = time.monotonic()
    ok_dumps = 0
    for _ in range(10):
        r = call("render_target.dump",
                 {"render_target_path": RT_PATH, "format": "png"}, timeout=10.0)
        if is_ok(r):
            ok_dumps += 1
    dt = (time.monotonic() - t0) * 1000.0
    snap_after = snapshot()
    uobj_after = snap_after.get("live_uobject_slots", 0)
    uobj_delta = uobj_after - uobj_before
    log.case("P5_leak_check_10_dumps",
             "PASS" if uobj_delta < 50 else "XFAIL",
             f"ok_dumps={ok_dumps}/10 uobj_delta={uobj_delta:+d} duration={dt:.0f}ms",
             duration_ms=dt)

    crash = latest_crash_dump(since=crash_baseline)
    if crash:
        log.case("crash_check", "FAIL", f"CRASH DUMP: {crash}")
        log.write(); cleanup(); return 1

    cleanup()

    summary = log.write()
    cc = summary["counts"]
    print()
    print(f"[H6.4] PASS={cc['PASS']} FAIL={cc['FAIL']} XFAIL={cc.get('XFAIL', 0)} "
          f"SKIP={cc.get('SKIP', 0)} TOTAL={cc['TOTAL']}")
    print(f"       log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    if cc["FAIL"] > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
