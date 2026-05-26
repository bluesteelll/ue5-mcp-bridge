#!/usr/bin/env python3
"""Phase B1 — FName overflow hostile probes.

Goal: every tool that turns a user-string into an FName MUST guard the
length BEFORE FName::Init is called. UE's FName has a hard 1023-char
limit; passing longer triggers a fatal assert that crashes the editor.

Plugin fixes already shipped (S+10..S+16) added `ValidateFNameLength`
checks to ~12 sites. This phase re-tests EVERY one + sweeps for missing
ones across all 369+ tools.

Probe template: send a 1100-char ASCII string into each FName-bearing
field. Expected outcome — one of:
  - -32602  InvalidParams      (S+10-style centralized guard)
  - -32011  WrongClass         (path normalizer rejects malformed)
  - -32010  InvalidPath        (mount-point guard fires first)
  - any other Bridge error code
  - PASS — handler ran, returned ok=true / non-error (rare)
  - FAIL — editor died (Lane A unresponsive / crash dump appears)

Each case is followed by `health()` + `latest_crash_dump()` check. The
first case that drops the editor flags B1 FAIL and the script aborts.

Field-by-field coverage (per-surface, all FName-coercing inputs)
----------------------------------------------------------------

  actor.spawn          actor_label                  → ValidateFNameLength
  actor.set_label      label / new_label
  actor.set_folder     folder_path (NOT FName)      — skipped (path str)
  bp.add_variable      variable_name                → S+6 covered
  bp.add_function      function_name                → S+6 covered
  bp.add_function_parameter param_name              → S+10 covered
  bp.add_component     variable_name                → S+6 covered
  bp.change_variable_type variable_name
  bp.set_pin_default   pin_name                     → may need guard
  bp.set_node_property property_name
  bp.set_variable_metadata key
  bp.set_function_metadata key
  bp.set_component_default variable_name + property_name
  ai.bb.add_key        key_name                     → S+6 covered (probably)
  ai.bb.set_value      key_name (runtime)
  ai.bt.add_node       node_class_path (path normalizer, not FName)
  ai.controller.set_focal_point actor_path (path)
  cfg.set_cvar         cvar                         → S+13 covered
  cfg.get_cvar         cvar                         → S+13 covered
  log.set_category_verbosity category               → S+14 covered
  niagara.set_user_param name                       → S+15 covered
  gameplaytag.add_tag  tag (uses FGameplayTag::RequestGameplayTag — may FName internally)
  input.create_input_action path (FAssetData → FName)
  input.create_input_mapping_context path
  input.add_trigger    trigger_class_path (UClass path)
  collision.set_profile_response  profile_name → FName
  mesh.set_lod_screen_size lod_index... (no FName?)
  texture.set_compression_setting setting_value (enum FName?)
  subsystem.* names (mostly paths, no FName)
  data_table.set_row   row_name → FName
  curve.set_row_value  row_name → FName

Exit codes: 0=PASS, 1=FAIL (editor died OR crash dump appeared),
            2=editor died at preflight.
"""

from __future__ import annotations

import json
import sys
import time
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

sys.path.insert(0, str(Path(__file__).parent))
from mcp_test_harness import (
    LOG_ROOT,
    TestLogger,
    call,
    cleanup_phantom_assets,
    err_code,
    err_message,
    health,
    is_ok,
    is_transport_failure,
    latest_crash_dump,
    preflight,
    random_suffix,
)

PHASE = "b1"
NAME = "fname_overflow"

# 1100 chars — well past FName's 1023 limit + leaves room for prefix/suffix
HOSTILE = "A" * 1100
HOSTILE_NAME = "X" * 1100
HOSTILE_PATH = f"/Game/{'X' * 1090}"


# Per-tool probe: (method, args_with_hostile, label)
PROBES: List[Tuple[str, Dict[str, Any], str]] = [
    # ── Actor labels (S+10 covered) ─────────────────────────────────────
    ("actor.spawn",
     {"class_path": "/Script/Engine.StaticMeshActor",
      "actor_label": HOSTILE,
      "location": [0, 0, 0]},
     "actor.spawn actor_label"),

    # ── Blueprint identifiers (S+6, S+10 covered) ───────────────────────
    ("bp.add_variable",
     {"blueprint_path": "/Game/_phantom_bp/X",
      "variable_name": HOSTILE_NAME,
      "pin_type": {"category": "Real", "subcategory": "float"}},
     "bp.add_variable variable_name"),
    ("bp.add_function",
     {"blueprint_path": "/Game/_phantom_bp/X", "function_name": HOSTILE_NAME},
     "bp.add_function function_name"),
    ("bp.add_function_parameter",
     {"blueprint_path": "/Game/_phantom_bp/X", "function_name": "F",
      "param_name": HOSTILE_NAME,
      "pin_type": {"category": "Real", "subcategory": "float"}},
     "bp.add_function_parameter param_name"),
    ("bp.add_component",
     {"blueprint_path": "/Game/_phantom_bp/X",
      "component_class_path": "/Script/Engine.PointLightComponent",
      "variable_name": HOSTILE_NAME},
     "bp.add_component variable_name"),
    ("bp.change_variable_type",
     {"blueprint_path": "/Game/_phantom_bp/X",
      "variable_name": HOSTILE_NAME,
      "pin_type": {"category": "Real", "subcategory": "float"}},
     "bp.change_variable_type variable_name"),
    ("bp.set_variable_metadata",
     {"blueprint_path": "/Game/_phantom_bp/X",
      "variable_name": "V", "key": HOSTILE, "value": "v"},
     "bp.set_variable_metadata key"),
    ("bp.set_function_metadata",
     {"blueprint_path": "/Game/_phantom_bp/X",
      "function_name": "F", "key": HOSTILE, "value": "v"},
     "bp.set_function_metadata key"),
    ("bp.set_pin_default",
     {"blueprint_path": "/Game/_phantom_bp/X",
      "function_name": "F", "node_guid": "00000000",
      "pin_name": HOSTILE_NAME, "value": "x"},
     "bp.set_pin_default pin_name"),

    # ── AI Blackboard (S+6 covered for FName-internal keys) ─────────────
    ("ai.bb.add_key",
     {"bb_path": "/Game/_phantom_bb/X",
      "key_name": HOSTILE_NAME, "key_type": "Float"},
     "ai.bb.add_key key_name"),

    # ── CVar (S+13 covered) ─────────────────────────────────────────────
    ("cfg.set_cvar",
     {"cvar": HOSTILE, "value": "1"},
     "cfg.set_cvar cvar"),
    ("cfg.get_cvar",
     {"cvar": HOSTILE},
     "cfg.get_cvar cvar"),

    # ── Log categories (S+14 covered) ───────────────────────────────────
    ("log.set_category_verbosity",
     {"category": HOSTILE, "verbosity": "Log"},
     "log.set_category_verbosity category"),

    # ── Niagara param (S+15 covered) ────────────────────────────────────
    ("niagara.set_user_param",
     {"niagara_system_path": "/Script/Engine.Default__Actor",
      "name": HOSTILE_NAME, "value": 1.0},
     "niagara.set_user_param name"),

    # ── Gameplay tags ───────────────────────────────────────────────────
    ("gameplaytag.add_tag",
     {"tag": HOSTILE, "comment": ""},
     "gameplaytag.add_tag tag"),

    # ── Asset/CB long paths (NOT FName but mount + path normalizer) ─────
    ("asset.create_data_asset",
     {"path": HOSTILE_PATH, "class_path": "/Script/Engine.PrimaryDataAsset"},
     "asset.create_data_asset path"),
    ("cb.create_folder",
     {"path": HOSTILE_PATH},
     "cb.create_folder path"),

    # ── Collision profile / object channel ──────────────────────────────
    ("collision.set_profile_response",
     {"profile_name": HOSTILE, "channel": "WorldStatic", "response": "Block"},
     "collision.set_profile_response profile_name"),

    # ── Data table row name (FName) ────────────────────────────────────
    ("data_table.set_row",
     {"data_table_path": "/Game/_phantom_dt/X",
      "row_name": HOSTILE, "row_data": {}},
     "data_table.set_row row_name"),

    # ── Curve row name ──────────────────────────────────────────────────
    ("curve.set_row_value",
     {"curve_path": "/Game/_phantom_curve/X",
      "row_name": HOSTILE, "time": 0.0, "value": 0.0},
     "curve.set_row_value row_name"),

    # ── Input mapping (path-style, but tests for any FName conversion) ──
    ("input.create_input_action",
     {"path": HOSTILE_PATH, "value_type": "Boolean"},
     "input.create_input_action path"),

    # ── Folder operations (S+8 PIE-guard tested; long path) ─────────────
    ("folder.create",
     {"folder_path": HOSTILE},
     "folder.create folder_path"),

    # ── BP set_node_property (property_name FName field) ────────────────
    ("bp.set_node_property",
     {"blueprint_path": "/Game/_phantom_bp/X", "function_name": "F",
      "node_guid": "00000000",
      "property_name": HOSTILE_NAME, "value": "x"},
     "bp.set_node_property property_name"),

    # ── Spawning with class path (long enough to overflow path internals) ──
    ("actor.spawn",
     {"class_path": HOSTILE_PATH, "location": [0, 0, 0]},
     "actor.spawn class_path (path normalizer)"),
]


def main() -> int:
    if not preflight(PHASE):
        return 2

    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()
    fail_total = 0

    print(f"[B1] running {len(PROBES)} FName-overflow probes (1100-char strings)…",
          flush=True)
    for idx, (method, args, label) in enumerate(PROBES):
        t0 = time.monotonic()
        try:
            r = call(method, args, timeout=8.0)
        except Exception as e:
            r = {"_err": "exception", "_exc": str(e)}
        dt = (time.monotonic() - t0) * 1000.0
        c = err_code(r)

        # Pre-probe liveness check (cheap Lane B health)
        alive = health(timeout=3.0)
        if not alive:
            log.case(label, "FAIL",
                     f"EDITOR DIED — Lane B unresponsive after {method}",
                     alive=False, duration_ms=dt, code=c)
            log.write()
            print(f"  [B1] EDITOR CRASHED on {method}", file=sys.stderr)
            return 1

        # Crash dump check
        crash = latest_crash_dump(since=crash_baseline)
        if crash:
            log.case(label, "FAIL",
                     f"CRASH DUMP after probe: {crash}",
                     alive=alive, duration_ms=dt, code=c)
            log.write()
            return 1

        # Outcome classification
        if is_transport_failure(r):
            log.case(label, "FAIL",
                     f"transport: {r.get('_err')} (probably editor death)",
                     alive=alive, duration_ms=dt)
            fail_total += 1
            continue

        if c is None and is_ok(r):
            # Handler succeeded — unusual but not necessarily wrong (e.g. SafeCloseTag)
            log.case(label, "PASS",
                     f"handler accepted 1100-char input (lenient guard)",
                     alive=alive, duration_ms=dt)
            continue

        # Any structured error code is PASS — guard fired before FName::Init
        if c is not None and -32700 <= c <= -32000:
            log.case(label, "PASS",
                     f"guard fired: code={c}: {err_message(r)[:60]}",
                     alive=alive, duration_ms=dt, code=c)
            continue

        log.case(label, "FAIL",
                 f"unexpected response shape: code={c} ok={is_ok(r)} msg={err_message(r)[:60]}",
                 alive=alive, duration_ms=dt, code=c)
        fail_total += 1

        if (idx + 1) % 5 == 0:
            cs = cleanup_phantom_assets()
            print(f"  [B1] {idx+1}/{len(PROBES)} done, "
                  f"fails={fail_total}, cleanup={cs}", flush=True)

    summary = log.write()
    c_counts = summary["counts"]
    print()
    print(f"[B1] PASS={c_counts['PASS']} FAIL={c_counts['FAIL']} "
          f"XFAIL={c_counts.get('XFAIL', 0)} TOTAL={c_counts['TOTAL']}")
    print(f"     log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    if fail_total > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
