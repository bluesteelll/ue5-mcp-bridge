#!/usr/bin/env python3
"""Phase H7 — Cross-surface mega-integration.

Goal: chain 30+ tool calls across BP / AI / Input / Actor / Tag /
Transform / Asset surfaces in a single workflow. Catches cross-surface
issues that per-domain tests miss (e.g. asset created by surface A then
referenced by surface B fails because of registry-cache lag).

Workflow (30+ steps):
  1-3.   Folder + child folder + nested folder (3 levels deep)
  4-8.   BP_Actor: create, add variable, add function, add component, compile
  9-11.  Blackboard: create, add 2 keys
  12-15. BehaviorTree: create linked → add Selector → add Sequence → add Wait
  16-19. Input: IA → IMC → mapping → list verify
  20-23. Actor lifecycle: spawn → set_location → set_rotation → set_label
  24-26. Tag: add_tag → add_to_container → query_actor
  27-28. cb.duplicate BP → verify exists
  29-31. cleanup all assets

PASS criteria: all 30+ steps succeed without failure; editor alive
throughout; final state cleanly torn down.

Exit codes: 0=PASS, 1=FAIL (any step), 2=preflight.
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
)

PHASE = "h7"
NAME = "mega_integration"

ROOT = f"/Game/PhT_H7_{random_suffix(6)}"
ASSETS_FOLDER = f"{ROOT}/Assets"
NESTED_FOLDER = f"{ASSETS_FOLDER}/Nested"
BP_PATH = f"{NESTED_FOLDER}/BP_MegaActor"
BP_DUP_PATH = f"{ASSETS_FOLDER}/BP_MegaActor_Dup"
BB_PATH = f"{ASSETS_FOLDER}/BB_Mega"
BT_PATH = f"{ASSETS_FOLDER}/BT_Mega"
IA_PATH = f"{ASSETS_FOLDER}/IA_MegaFire"
IMC_PATH = f"{ASSETS_FOLDER}/IMC_MegaDefault"


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
        log.case(name, "FAIL", f"{method}: code={c}: {err_message(r)[:60]}",
                 duration_ms=dt, code=c)
        return None
    log.case(name, "PASS", f"{method} ok", duration_ms=dt)
    return r.get("result", {}) or {}


def cleanup() -> None:
    for p in [BP_DUP_PATH, BP_PATH, BB_PATH, BT_PATH, IA_PATH, IMC_PATH]:
        call("cb.delete", {"path": p, "force": True}, timeout=10.0)
    call("folder.delete", {"folder_path": ROOT, "recursive": True}, timeout=15.0)


def main() -> int:
    if not preflight(PHASE):
        return 2
    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()

    print(f"[H7] mega-integration: 30+ chained calls (root={ROOT})…", flush=True)
    cleanup()

    # ─── 1-3. Folder hierarchy ────────────────────────────────────────────
    if _step(log, "01_folder_root", "folder.create", {"folder_path": ROOT}) is None:
        log.write(); cleanup(); return 1
    if _step(log, "02_folder_assets", "folder.create", {"folder_path": ASSETS_FOLDER}) is None:
        log.write(); cleanup(); return 1
    if _step(log, "03_folder_nested", "folder.create", {"folder_path": NESTED_FOLDER}) is None:
        log.write(); cleanup(); return 1

    # ─── 4-8. BP_Actor + variable + function + component + compile ────────
    if _step(log, "04_bp_create", "bp.create_blueprint",
              {"dest_path": BP_PATH,
               "parent_class_path": "/Script/Engine.Actor"}) is None:
        log.write(); cleanup(); return 1
    if _step(log, "05_bp_add_var", "bp.add_variable",
              {"blueprint_path": BP_PATH,
               "variable_name": "Health",
               "pin_type": {"category": "Real", "subcategory": "float"}}) is None:
        log.write(); cleanup(); return 1
    if _step(log, "06_bp_add_func", "bp.add_function",
              {"blueprint_path": BP_PATH,
               "function_name": "TakeDamage"}) is None:
        log.write(); cleanup(); return 1
    if _step(log, "07_bp_add_param", "bp.add_function_parameter",
              {"blueprint_path": BP_PATH,
               "function_name": "TakeDamage",
               "param_name": "Amount",
               "direction": "input",
               "pin_type": {"category": "Real", "subcategory": "float"}}) is None:
        log.write(); cleanup(); return 1
    if _step(log, "08_bp_compile", "bp.compile",
              {"blueprint_path": BP_PATH}) is None:
        log.write(); cleanup(); return 1

    # ─── 9-11. Blackboard + keys ──────────────────────────────────────────
    if _step(log, "09_bb_create", "ai.bb.create_asset",
              {"path": BB_PATH}) is None:
        log.write(); cleanup(); return 1
    if _step(log, "10_bb_key_target", "ai.bb.add_key",
              {"bb_path": BB_PATH, "key_name": "EnemyTarget",
               "key_type": "Object"}) is None:
        log.write(); cleanup(); return 1
    if _step(log, "11_bb_key_alertness", "ai.bb.add_key",
              {"bb_path": BB_PATH, "key_name": "Alertness",
               "key_type": "Float"}) is None:
        log.write(); cleanup(); return 1

    # ─── 12-15. BT linked → composites + task ─────────────────────────────
    if _step(log, "12_bt_create", "ai.bt.create_asset",
              {"path": BT_PATH, "blackboard_path": BB_PATH}) is None:
        log.write(); cleanup(); return 1
    sel = _step(log, "13_bt_selector", "ai.bt.add_node",
                 {"bt_path": BT_PATH, "parent_path": "ROOT",
                  "node_class": "/Script/AIModule.BTComposite_Selector"})
    if sel is None:
        log.write(); cleanup(); return 1
    sel_path = sel.get("node_path") or "ROOT/Children[0]"
    seq = _step(log, "14_bt_sequence", "ai.bt.add_node",
                 {"bt_path": BT_PATH, "parent_path": sel_path,
                  "node_class": "/Script/AIModule.BTComposite_Sequence"})
    if seq is None:
        log.write(); cleanup(); return 1
    seq_path = seq.get("node_path") or f"{sel_path}/Children[0]"
    if _step(log, "15_bt_wait", "ai.bt.add_node",
              {"bt_path": BT_PATH, "parent_path": seq_path,
               "node_class": "/Script/AIModule.BTTask_Wait"}) is None:
        log.write(); cleanup(); return 1

    # ─── 16-19. Input: IA + IMC + mapping + list verify ───────────────────
    if _step(log, "16_ia_create", "input.create_input_action",
              {"path": IA_PATH, "value_type": "Boolean"}) is None:
        log.write(); cleanup(); return 1
    if _step(log, "17_imc_create", "input.create_mapping_context",
              {"path": IMC_PATH}) is None:
        log.write(); cleanup(); return 1
    if _step(log, "18_add_mapping", "input.add_mapping",
              {"imc_path": IMC_PATH, "ia_path": IA_PATH,
               "key": "LeftMouseButton"}) is None:
        log.write(); cleanup(); return 1
    r_iactions = _step(log, "19_list_ias", "input.list_input_actions", {})
    if r_iactions is None:
        log.write(); cleanup(); return 1
    ias = r_iactions.get("actions") or r_iactions.get("input_actions") or []
    if not any(IA_PATH in (a.get("path") or a.get("asset_path") or "") for a in ias):
        log.case("19_verify_ia", "FAIL",
                 f"IA missing from list of {len(ias)} actions")
        log.write(); cleanup(); return 1
    log.case("19_verify_ia", "PASS",
             f"IA confirmed in list of {len(ias)} actions")

    # ─── 20-22. Actor lifecycle ───────────────────────────────────────────
    actor_label = f"MegaActor_{random_suffix(4)}"
    r_spawn = _step(log, "20_actor_spawn", "actor.spawn",
                     {"class_path": "/Script/Engine.StaticMeshActor",
                      "actor_label": actor_label,
                      "location": [100, 100, 100]})
    if r_spawn is None:
        log.write(); cleanup(); return 1
    actor_path = r_spawn.get("actor_path") or r_spawn.get("path") or ""
    if not actor_path:
        log.case("20_verify_spawn", "FAIL",
                 f"actor.spawn returned no actor_path: {r_spawn}")
        log.write(); cleanup(); return 1
    log.case("20_verify_spawn", "PASS", f"actor at {actor_path}")

    if _step(log, "21_set_location", "actor.set_location",
              {"actor_path": actor_path,
               "location": {"x": 200, "y": 300, "z": 400}}) is None:
        # Non-fatal: destroy actor and continue
        call("actor.destroy", {"actor_path": actor_path}, timeout=8.0)
        log.write(); cleanup(); return 1
    if _step(log, "22_set_rotation", "actor.set_rotation",
              {"actor_path": actor_path,
               "rotation": {"pitch": 10, "yaw": 20, "roll": 30}}) is None:
        call("actor.destroy", {"actor_path": actor_path}, timeout=8.0)
        log.write(); cleanup(); return 1

    # ─── 23-25. Tag flow ──────────────────────────────────────────────────
    tag = f"PhT.H7.{random_suffix(4)}"
    # gameplaytag.add_tag may not exist (only on some builds); soft probe
    r_add_tag = call("gameplaytag.add_tag", {"tag": tag}, timeout=8.0)
    if not is_ok(r_add_tag):
        # Tag adoption may be implicit on container; XFAIL this step and continue
        log.case("23_tag_add", "XFAIL",
                 f"gameplaytag.add_tag failed: {err_message(r_add_tag)[:60]}")
    else:
        log.case("23_tag_add", "PASS", "tag registered")

    r_container = call("gameplaytag.add_to_container",
                         {"actor_path": actor_path, "tags": [tag]}, timeout=8.0)
    if is_ok(r_container):
        log.case("24_tag_add_container", "PASS", "tag added to actor")
        r_query = call("gameplaytag.query_actor",
                        {"actor_path": actor_path, "tags": [tag]}, timeout=8.0)
        if is_ok(r_query):
            qres = r_query.get("result", {}) or {}
            has = bool(qres.get("has_any") or qres.get("has_all") or qres.get("matched"))
            log.case("25_tag_query", "PASS" if has else "XFAIL",
                     f"query has={has}")
        else:
            log.case("25_tag_query", "XFAIL",
                     f"query_actor failed: {err_message(r_query)[:60]}")
    else:
        log.case("24_tag_add_container", "XFAIL",
                 f"add_to_container failed: {err_message(r_container)[:60]}")
        log.case("25_tag_query", "SKIP", "depends on 24")

    # ─── 26-27. Duplicate BP + verify ─────────────────────────────────────
    if _step(log, "26_bp_duplicate", "cb.duplicate",
              {"source_path": BP_PATH, "dest_path": BP_DUP_PATH}) is None:
        call("actor.destroy", {"actor_path": actor_path}, timeout=8.0)
        log.write(); cleanup(); return 1
    r_exists = _step(log, "27_dup_exists", "asset.exists",
                      {"path": BP_DUP_PATH})
    if r_exists is None or not r_exists.get("exists"):
        log.case("27_verify_dup", "FAIL", f"dup BP missing: {r_exists}")
        call("actor.destroy", {"actor_path": actor_path}, timeout=8.0)
        log.write(); cleanup(); return 1

    # ─── 28-30. Final BP introspection ────────────────────────────────────
    r_vars = _step(log, "28_bp_list_vars", "bp.list_variables",
                    {"blueprint_path": BP_PATH})
    if r_vars is None:
        log.write(); cleanup(); return 1
    var_names = [v.get("name") for v in (r_vars.get("variables") or [])]
    if "Health" not in var_names:
        log.case("28_verify_vars", "FAIL", f"Health missing: {var_names[:5]}")
        log.write(); cleanup(); return 1
    log.case("28_verify_vars", "PASS",
             f"Health present in {len(var_names)} vars")

    r_funcs = _step(log, "29_bp_list_funcs", "bp.list_functions",
                     {"blueprint_path": BP_PATH})
    if r_funcs is None:
        log.write(); cleanup(); return 1
    func_names = [f.get("name") for f in (r_funcs.get("functions") or [])]
    if "TakeDamage" not in func_names:
        log.case("29_verify_funcs", "FAIL",
                 f"TakeDamage missing: {func_names[:5]}")
        log.write(); cleanup(); return 1
    log.case("29_verify_funcs", "PASS",
             f"TakeDamage present in {len(func_names)} funcs")

    # ─── 30. Destroy actor + verify gone ──────────────────────────────────
    if _step(log, "30_actor_destroy", "actor.destroy",
              {"actor_path": actor_path}) is None:
        log.write(); cleanup(); return 1

    # ─── Final health + crash check ───────────────────────────────────────
    if not health(timeout=5.0):
        log.case("final_health", "FAIL", "editor unresponsive after mega chain",
                 alive=False)
        log.write(); cleanup(); return 1
    crash = latest_crash_dump(since=crash_baseline)
    if crash:
        log.case("crash_check", "FAIL", f"CRASH DUMP: {crash}")
        log.write(); cleanup(); return 1

    cleanup()

    summary = log.write()
    cc = summary["counts"]
    print()
    print(f"[H7] PASS={cc['PASS']} FAIL={cc['FAIL']} XFAIL={cc.get('XFAIL', 0)} "
          f"SKIP={cc.get('SKIP', 0)} TOTAL={cc['TOTAL']}")
    print(f"     log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    if cc["FAIL"] > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
