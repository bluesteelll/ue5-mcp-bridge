#!/usr/bin/env python3
"""Phase F1 — Privilege escalation re-test (broadened B2).

Security gate. After S+7/S+11/S+12 wrapped writeable-mount guards around
the disk-write code paths, confirm NO remaining asset-creator escapes the
guard. This sweep is B2 broadened to the FULL creator surface, including
tools B2 didn't cover:

  - niagara.create_emitter        → /Engine
  - umg.create_widget_blueprint   → /Engine
  - asset.create (CurveFloat / DataTable / MaterialInstanceConstant /
    World classes) → /Engine   (covers the "curve/data_table/mat_inst
    create" intent — those surfaces have NO dedicated creator, so the
    generic asset.create is the only authoring path and MUST be guarded)
  - bp.duplicate                  → /Engine (real source BP)
  - cb.import                     → /Engine (phantom source)

Plus the B2 baseline (asset.create_data_asset, bp.create_blueprint,
mesh/level/cb duplicate, cb.rename/move/create_folder, input.*, ai.*).

Mount targets swept per tool:
  /Engine/...   (read-only engine content)
  /Engine/Script/...  (reflection mount — S+7)
  /Memory/...   (memory-only mount — S+7)
  /Plugins/Editor/...  (engine plugin mount)
  /Game/../Engine/...  (relative escape)

VERDICT (per probe):
  -32010 / -32011 / -32013 / -32027 / -32004  → PASS  (guard fired)
  any other structured Bridge error            → XFAIL (rejected, odd code)
  -32601 (not registered)                      → SKIP
  ok=true (WRITE SUCCEEDED)                     → FAIL  (SECURITY HOLE)
  transport failure / editor crash             → FAIL

Exit codes: 0=PASS, 1=FAIL, 2=preflight.
"""

from __future__ import annotations

import sys
import time
from pathlib import Path
from typing import Any, Dict, List, Tuple

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
)

PHASE = "f1"
NAME = "privilege_escalation"

# Read-only mount targets — every write here MUST be rejected.
ENGINE_BP = "/Engine/EditorBlueprintResources/_PhT_F1_BP_"
ENGINE_MESH = "/Engine/EditorMeshes/_PhT_F1_Mesh_"
ENGINE_MAP = "/Engine/Maps/_PhT_F1_Map_"
ENGINE_CONTENT = "/Engine/_PhT_F1_Content_"
SCRIPT_MOUNT = "/Engine/Script/_PhT_F1_Script_"
MEMORY_MOUNT = "/Memory/_PhT_F1_Mem_"
PLUGINS_ENGINE = "/Plugins/Editor/_PhT_F1_Plugin_"
RELATIVE_ESCAPE = "/Game/../Engine/_PhT_F1_Esc_"

# Reject codes that mean "the guard worked" (rejected before any write).
GUARD_CODES = [-32010, -32011, -32013, -32027, -32004]

# A known-good source BP for duplicate probes (engine content always present).
SRC_CUBE = "/Engine/BasicShapes/Cube.Cube"

# Concrete, instantiable UDataAsset subclass — passes class-validation so
# asset.create_data_asset reaches the mount guard. PrimaryDataAsset (abstract)
# rejects at -32021 BEFORE the guard; PrimaryAssetLabel is concrete (proven
# in J1/E2 leak tests).
DATA_ASSET_CLASS = "/Script/Engine.PrimaryAssetLabel"


def _probe(tag: str) -> str:
    return f"{tag}{random_suffix(4)}"


# (method, args, label) — args carry a hostile dest path.
def _build_probes() -> List[Tuple[str, Dict[str, Any], str]]:
    P: List[Tuple[str, Dict[str, Any], str]] = []

    # ── NEW creators not covered by B2 ─────────────────────────────────
    # NB: these creators take `dest_path` (NOT `path`); using the wrong
    # arg name would reject at -32602 BEFORE reaching the mount guard,
    # making the probe inconclusive. dest_path lets the guard fire.
    P.append(("niagara.create_emitter",
              {"dest_path": _probe(ENGINE_BP)},
              "niagara.create_emitter → /Engine"))
    P.append(("niagara.create_system",
              {"dest_path": _probe(ENGINE_BP)},
              "niagara.create_system → /Engine"))
    P.append(("umg.create_widget_blueprint",
              {"dest_path": _probe(ENGINE_BP)},
              "umg.create_widget_blueprint → /Engine"))

    # asset.create — the ONLY authoring path for curve/data_table/mat_inst,
    # so guarding it covers all three "create" intents from the plan.
    for cls, tag in [
        ("/Script/Engine.CurveFloat", "CurveFloat"),
        ("/Script/Engine.DataTable", "DataTable"),
        ("/Script/Engine.MaterialInstanceConstant", "MatInstConst"),
        ("/Script/Engine.World", "World"),
        ("/Script/Engine.Material", "Material"),
    ]:
        P.append(("asset.create",
                  {"dest_path": _probe(ENGINE_CONTENT), "class_path": cls},
                  f"asset.create({tag}) → /Engine"))

    P.append(("bp.duplicate",
              {"source_path": SRC_CUBE, "dest_path": _probe(ENGINE_BP)},
              "bp.duplicate → /Engine"))
    P.append(("cb.import",
              {"source_path": "D:/tmp/_pht_f1_nonexistent.png",
               "dest_path": _probe(ENGINE_CONTENT)},
              "cb.import → /Engine"))

    # ── B2 baseline creators, re-swept (regression guard) ──────────────
    P.append(("asset.create_data_asset",
              {"dest_path": _probe(ENGINE_BP),
               "class_path": DATA_ASSET_CLASS},
              "asset.create_data_asset → /Engine"))
    P.append(("bp.create_blueprint",
              {"dest_path": _probe(ENGINE_BP),
               "parent_class_path": "/Script/Engine.Actor"},
              "bp.create_blueprint → /Engine"))
    P.append(("mesh.duplicate",
              {"source_path": SRC_CUBE, "dest_path": _probe(ENGINE_MESH)},
              "mesh.duplicate → /Engine (S+11)"))
    P.append(("level.duplicate",
              {"source_map": "/Engine/Maps/Templates/OpenWorld",
               "dest_map": _probe(ENGINE_MAP)},
              "level.duplicate → /Engine (S+12)"))
    P.append(("cb.duplicate",
              {"source_path": SRC_CUBE, "dest_path": _probe(ENGINE_MESH)},
              "cb.duplicate → /Engine"))
    P.append(("input.create_input_action",
              {"path": _probe(ENGINE_BP), "value_type": "Boolean"},
              "input.create_input_action → /Engine"))
    P.append(("input.create_mapping_context",
              {"path": _probe(ENGINE_BP)},
              "input.create_mapping_context → /Engine"))
    P.append(("ai.bb.create_asset", {"path": _probe(ENGINE_BP)},
              "ai.bb.create_asset → /Engine"))
    P.append(("ai.bt.create_asset", {"path": _probe(ENGINE_BP)},
              "ai.bt.create_asset → /Engine"))
    P.append(("ai.eqs.create_asset", {"path": _probe(ENGINE_BP)},
              "ai.eqs.create_asset → /Engine"))

    # ── Alternate mounts: /Engine/Script, /Memory, /Plugins, relative ──
    for mount, tag in [
        (SCRIPT_MOUNT, "/Engine/Script (S+7)"),
        (MEMORY_MOUNT, "/Memory (S+7)"),
        (PLUGINS_ENGINE, "/Plugins/Editor"),
        (RELATIVE_ESCAPE, "relative ../Engine"),
    ]:
        P.append(("bp.create_blueprint",
                  {"dest_path": _probe(mount),
                   "parent_class_path": "/Script/Engine.Actor"},
                  f"bp.create_blueprint → {tag}"))
        P.append(("asset.create_data_asset",
                  {"dest_path": _probe(mount),
                   "class_path": DATA_ASSET_CLASS},
                  f"asset.create_data_asset → {tag}"))

    return P


def _try_cleanup(method: str, args: Dict[str, Any]) -> None:
    """Best-effort delete if a write unexpectedly went through."""
    dest = (args.get("dest_path") or args.get("path") or args.get("dest_map") or "")
    if dest:
        call("cb.delete", {"path": dest, "force": True}, timeout=8.0)


def main() -> int:
    if not preflight(PHASE):
        return 2

    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()
    fail_total = 0

    probes = _build_probes()
    print(f"[F1] privilege-escalation re-test: {len(probes)} creator probes…",
          flush=True)

    for method, args, label in probes:
        t0 = time.monotonic()
        try:
            r = call(method, args, timeout=10.0)
        except Exception as e:
            r = {"_err": "exception", "_exc": str(e)}
        dt = (time.monotonic() - t0) * 1000.0
        c = err_code(r)

        if not health(timeout=3.0):
            log.case(label, "FAIL",
                     f"EDITOR DIED after {method}: code={c}",
                     alive=False, duration_ms=dt)
            log.write()
            print(f"  [F1] EDITOR CRASHED on {method}", file=sys.stderr)
            return 1

        crash = latest_crash_dump(since=crash_baseline)
        if crash:
            log.case(label, "FAIL", f"CRASH DUMP after {method}: {crash}",
                     duration_ms=dt, code=c)
            log.write()
            return 1

        if is_transport_failure(r):
            log.case(label, "FAIL", f"transport: {r.get('_err')}",
                     duration_ms=dt)
            fail_total += 1
            continue

        if is_ok(r):
            # WRITE SUCCEEDED into a restricted mount → SECURITY HOLE.
            _try_cleanup(method, args)
            log.case(label, "FAIL",
                     "WRITE SUCCEEDED INTO RESTRICTED MOUNT — guard regression!",
                     duration_ms=dt)
            fail_total += 1
            continue

        if c == -32601:
            log.case(label, "SKIP", f"{method} not registered", duration_ms=dt)
            continue

        if c in GUARD_CODES:
            log.case(label, "PASS",
                     f"guard fired {c}: {err_message(r)[:50]}",
                     duration_ms=dt, code=c)
            continue

        if c is not None and -32700 <= c <= -32000:
            log.case(label, "XFAIL",
                     f"rejected, unexpected code {c}: {err_message(r)[:50]}",
                     duration_ms=dt, code=c)
            continue

        log.case(label, "FAIL",
                 f"unknown response code={c} msg={err_message(r)[:60]}",
                 duration_ms=dt, code=c)
        fail_total += 1

    summary = log.write()
    cc = summary["counts"]
    print()
    print(f"[F1] PASS={cc['PASS']} FAIL={cc['FAIL']} XFAIL={cc.get('XFAIL', 0)} "
          f"SKIP={cc.get('SKIP', 0)} TOTAL={cc['TOTAL']}")
    print(f"     log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    if fail_total > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
