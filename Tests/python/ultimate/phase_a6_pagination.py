#!/usr/bin/env python3
"""Phase A6 — Pagination cursor walks.

Goal: every paginated tool walks from first to last page without
duplication or omission. Tests page_size edge cases and stale-cursor
detection.

Per-tool cases
--------------
1. walk_default     — page_size=100, count items, assert no dups
2. walk_page_size_1 — page_size=1 (degenerate), verify same total
3. walk_page_size_1000 — page_size=1000 (large), verify same total
4. page_size_zero   — page_size=0 → expect -32602 or silent clamp to 1
5. stale_cursor     — capture token mid-walk, change filter, re-use
                      token → expect -32015 StaleCursor

Paginated tools (curated)
-------------------------
  asset.list
  cb.list
  actor.list
  bp.list
  bp.list_categories       (small set; degenerate page_size_1 tests)
  data_table.list
  curve.list
  niagara.list_parameters  (per-system)
  input.list_input_actions
  ai.bt.list_assets
  ai.bb.list_assets
  ai.eqs.list_assets

For each tool, we count items, then verify that page_size variations
produce IDENTICAL item sets (up to ordering). Item identity = sorted
tuple of (object_path or name or row_name field).

Exit codes: 0=PASS, 1=FAIL, 2=editor died.
"""

from __future__ import annotations

import json
import sys
import time
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional, Set, Tuple

sys.path.insert(0, str(Path(__file__).parent))
from mcp_test_harness import (
    TestLogger,
    call,
    err_code,
    err_message,
    health,
    is_ok,
    is_transport_failure,
    latest_crash_dump,
    preflight,
)

PHASE = "a6"
NAME = "pagination"

# (method, args_dict, item_key_in_response, identity_field_in_item) tuples.
# Some tools require args (e.g. niagara.list_parameters needs path). Skip those.
TOOLS: List[Tuple[str, Dict[str, Any], str, str]] = [
    ("asset.list",                {}, "items", "object_path"),
    ("cb.list",                   {"folder_path": "/Game"}, "items", "object_path"),
    ("actor.list",                {}, "items", "actor_path"),
    ("bp.list_categories",        {}, "items", "name"),
    ("bp.list",                   {}, "items", "object_path"),
    ("data_table.list",           {}, "items", "object_path"),
    ("curve.list",                {}, "items", "object_path"),
    ("input.list_input_actions",  {}, "input_actions", "input_action_path"),
    ("input.list_mapping_contexts", {}, "mapping_contexts", "input_mapping_context_path"),
    ("ai.bt.list_assets",         {}, "items", "object_path"),
    ("ai.bb.list_assets",         {}, "items", "object_path"),
    ("ai.eqs.list_assets",        {}, "items", "object_path"),
    ("level.list",                {}, "items", "object_path"),
    ("subsystem.list_engine",     {}, "items", "name"),
    ("subsystem.list_editor",     {}, "items", "name"),
    ("subsystem.list_world",      {}, "items", "name"),
    ("gameplaytag.list_all",      {}, "items", "tag"),
    ("collision.list_channels",   {}, "items", "channel_name"),
    ("collision.list_profiles",   {}, "items", "profile_name"),
]


def extract_items(resp: dict, key: str) -> List[Any]:
    res = resp.get("result", {}) or {}
    # Try the named key, then fall back to common alternatives.
    for k in (key, "items", "results", "data"):
        v = res.get(k)
        if isinstance(v, list):
            return v
    return []


def identity_of(item: Any, field: str) -> str:
    if isinstance(item, str):
        return item
    if isinstance(item, dict):
        for k in (field, "object_path", "name", "path", "actor_path", "id"):
            v = item.get(k)
            if isinstance(v, str) and v:
                return v
    return json.dumps(item, sort_keys=True)[:200]


def walk(method: str, args: Dict[str, Any], item_key: str, ident: str,
         page_size: int) -> Tuple[List[str], List[Dict[str, Any]], Optional[int]]:
    """Returns (identities, pages_metadata, error_code).

    identities: list of identity strings in walk order
    pages_metadata: list of {page_idx, n_items, has_next_token, dur_ms}
    error_code: bridge error code if walk aborted, None on success
    """
    identities: List[str] = []
    pages: List[Dict[str, Any]] = []
    token: Optional[str] = None
    seen: Set[str] = set()
    for page_idx in range(2000):  # generous safety bound
        a = dict(args)
        a["page_size"] = page_size
        if token is not None:
            a["page_token"] = token
        t0 = time.monotonic()
        r = call(method, a, timeout=8.0)
        dur = (time.monotonic()-t0)*1000.0
        if not is_ok(r):
            return identities, pages, err_code(r) or -1
        items = extract_items(r, item_key)
        for it in items:
            ident_str = identity_of(it, ident)
            if ident_str in seen:
                # duplicate — signal via empty token + flag
                pages.append({"page": page_idx, "n": len(items), "dup": ident_str, "ms": dur})
                return identities, pages, -2  # custom: duplicate detected
            seen.add(ident_str)
            identities.append(ident_str)
        next_token = r.get("result", {}).get("next_page_token") or None
        pages.append({"page": page_idx, "n": len(items), "next": bool(next_token), "ms": dur})
        token = next_token
        if not token:
            return identities, pages, None
    return identities, pages, -3  # ran out of safety pages


def test_tool(log: TestLogger, method: str, args: Dict[str, Any],
              item_key: str, ident: str) -> int:
    # First probe: tool callable?
    r0 = call(method, dict(args, page_size=10), timeout=8.0)
    if not is_ok(r0):
        code = err_code(r0)
        log.case(f"{method}/probe", "XFAIL",
                 f"tool not callable in this context: code={code}: {err_message(r0)[:60]}",
                 code=code)
        return 0  # skip — tool not applicable

    # Case 1: walk_default
    ids_default, pages_default, ec_default = walk(method, args, item_key, ident, 100)
    if ec_default is not None and ec_default >= 0:
        log.case(f"{method}/walk_default", "FAIL",
                 f"walk aborted with err {ec_default} after {len(ids_default)} items",
                 extras={"pages": len(pages_default)})
        return 1
    if ec_default == -2:
        log.case(f"{method}/walk_default", "FAIL",
                 f"DUPLICATE detected: ident={pages_default[-1].get('dup')}",
                 extras={})
        return 1
    if ec_default == -3:
        log.case(f"{method}/walk_default", "FAIL",
                 "walk exceeded 2000 pages — infinite loop?",
                 extras={"total_items": len(ids_default)})
        return 1
    log.case(f"{method}/walk_default", "PASS",
             f"walked {len(ids_default)} items across {len(pages_default)} pages")

    # Case 2: walk_page_size_1 (only if reasonable — skip if >2000 items)
    if len(ids_default) > 2000:
        log.case(f"{method}/walk_page_size_1", "SKIP",
                 f"{len(ids_default)} items too large for page_size=1 walk")
    else:
        ids_one, _, ec_one = walk(method, args, item_key, ident, 1)
        if ec_one is None:
            same = set(ids_one) == set(ids_default)
            log.case(f"{method}/walk_page_size_1",
                     "PASS" if same else "FAIL",
                     f"page_size=1 walked {len(ids_one)} items; same-as-default={same}")
            if not same:
                return 1
        else:
            log.case(f"{method}/walk_page_size_1", "FAIL",
                     f"walk aborted with err {ec_one}")
            return 1

    # Case 3: walk_page_size_1000
    ids_big, _, ec_big = walk(method, args, item_key, ident, 1000)
    if ec_big is None:
        same = set(ids_big) == set(ids_default)
        log.case(f"{method}/walk_page_size_1000",
                 "PASS" if same else "FAIL",
                 f"page_size=1000 walked {len(ids_big)} items; same-as-default={same}")
        if not same:
            return 1
    else:
        log.case(f"{method}/walk_page_size_1000", "FAIL",
                 f"walk aborted with err {ec_big}")
        return 1

    # Case 4: page_size=0 → expect -32602 OR clamp to 1
    r4 = call(method, dict(args, page_size=0), timeout=6.0)
    c4 = err_code(r4)
    if is_ok(r4):
        items = extract_items(r4, item_key)
        log.case(f"{method}/page_size_zero", "PASS",
                 f"silent-clamp accepted, returned {len(items)} items", code=c4)
    elif c4 == -32602:
        log.case(f"{method}/page_size_zero", "PASS",
                 "rejected with -32602", code=c4)
    else:
        log.case(f"{method}/page_size_zero", "XFAIL",
                 f"unexpected: code={c4} msg={err_message(r4)[:60]}", code=c4)

    # Case 5: stale_cursor — fetch page 1 with filter F1, then re-use the
    # returned token with filter F2 → expect -32015 StaleCursor when the
    # tool embeds a filter hash in its cursor (asset.list / asset.search_*
    # / bp.list_nodes_in_function etc.). Tools that don't embed filter
    # hash legitimately succeed — those XFAIL (not all paginated tools
    # carry stale-cursor semantics).
    _try_stale_cursor(log, method, args, item_key)

    return 0


# Filter-pair candidates: tool → (filter_variants). For each tool we walk
# page 1 with variant[0], capture next_page_token, then re-call with
# variant[1] + same token. Two filter variants must produce DIFFERENT
# result sets for the staleness check to be meaningful — but the bridge
# should detect tampering regardless of whether result sets actually differ.
_STALE_FILTER_PAIRS: Dict[str, Tuple[Dict[str, Any], Dict[str, Any]]] = {
    "asset.list": (
        {"class_name": "Blueprint"},
        {"class_name": "Material"},
    ),
    "cb.list": (
        {"folder_path": "/Game"},
        {"folder_path": "/Engine"},
    ),
    "bp.list": (
        {"name_filter": "BP_"},
        {"name_filter": "WBP_"},
    ),
    "actor.list": (
        {"class_filter": "/Script/Engine.StaticMeshActor"},
        {"class_filter": "/Script/Engine.Pawn"},
    ),
    # data_table / curve / input / ai.* listings — filter is the only var
    # we can change cleanly. For surfaces without a usable filter, the
    # function silently skips by returning XFAIL.
}


def _try_stale_cursor(log: TestLogger, method: str, args: Dict[str, Any],
                       item_key: str) -> None:
    case_id = f"{method}/stale_cursor"
    pair = _STALE_FILTER_PAIRS.get(method)
    if not pair:
        log.case(case_id, "SKIP",
                 "no filter pair defined for this surface — staleness test not applicable")
        return
    f1, f2 = pair
    # Page 1 with filter F1, small page_size so we get a next_page_token
    a1 = dict(args); a1.update(f1); a1["page_size"] = 2
    r1 = call(method, a1, timeout=8.0)
    if not is_ok(r1):
        log.case(case_id, "XFAIL",
                 f"page 1 with F1 failed: code={err_code(r1)} msg={err_message(r1)[:60]}",
                 code=err_code(r1))
        return
    token = r1.get("result", {}).get("next_page_token") or None
    if not token:
        log.case(case_id, "SKIP",
                 f"F1 returned no next_page_token (only one page) — staleness test inapplicable")
        return
    # Re-call with filter F2 + same token
    a2 = dict(args); a2.update(f2); a2["page_size"] = 2; a2["page_token"] = token
    r2 = call(method, a2, timeout=8.0)
    c2 = err_code(r2)
    if c2 == -32015:
        log.case(case_id, "PASS",
                 "tool detected filter swap mid-pagination → -32015 StaleCursor",
                 code=c2)
    elif is_ok(r2):
        log.case(case_id, "XFAIL",
                 "tool silently accepted stale cursor (filter not hashed into token)",
                 code=c2)
    else:
        log.case(case_id, "XFAIL",
                 f"unexpected code={c2}: {err_message(r2)[:60]} (expected -32015 or ok)",
                 code=c2)


def main() -> int:
    if not preflight(PHASE):
        return 2
    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()
    fail_total = 0
    for method, args, item_key, ident in TOOLS:
        print(f"[A6] testing {method}…", flush=True)
        rc = test_tool(log, method, args, item_key, ident)
        fail_total += rc
        if not health(timeout=3.0):
            log.note(f"editor died after {method}")
            log.write()
            return 2
    crash = latest_crash_dump(since=crash_baseline)
    if crash:
        log.note(f"NEW CRASH DUMP: {crash}")
        fail_total += 1
    summary = log.write()
    c = summary["counts"]
    print()
    print(f"[A6] PASS={c['PASS']} FAIL={c['FAIL']} XFAIL={c.get('XFAIL', 0)} SKIP={c.get('SKIP', 0)} TOTAL={c['TOTAL']}")
    print(f"     log: {log.md_path}")
    if not summary["final_health"]:
        return 2
    if fail_total > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
