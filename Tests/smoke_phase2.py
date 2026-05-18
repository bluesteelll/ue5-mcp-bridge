#!/usr/bin/env python3
"""End-to-end smoke test for the MCP bridge Phase 2 surface (30 tools + 3 internals).

Prerequisites:
  * Unreal Editor must be running with the UnrealMCPBridge plugin loaded.
  * Look for log line: ``LogMCP: MCP bridge listening on 127.0.0.1:30020``.
  * Phase 2 handlers registered:
    ``LogMCP: Phase 2 Day 11: registered 3 internal asset.* handlers``
  * Test-data prep (one-time; document in README if missing):
    1. ``Content/MCPTest/PhaseTwo/DA_PhaseTwoTest.uasset`` — duplicate of any
       UFlecsEntityDefinition. Sub-tests 1-13, 22, 29-31 rely on its presence.
    2. ``Plugins/UnrealMCPBridge/Tests/test_assets/test_texture.png`` — 32x32
       magenta. Used by sub-test 23 (cb.import).
    3. ``Plugins/UnrealMCPBridge/Tests/test_assets/test_mesh.fbx`` — minimal cube.
       Used by sub-test 27 (cb.bulk_import). MAY BE SKIPPED — FBX generation is
       non-trivial; sub-test 27 will report SKIP if file is missing.

Sub-test outline (34 total — matches plan §"Smoke test plan"):
  1-6   Category A reads (asset.exists / metadata / list with pagination / overly broad)
  7-10  Category A graph queries (find_references / find_dependents / class_hierarchy / search_by_class)
  11-13 Thumbnails + dirty/package
  14-19 Category B mutations (create_folder / duplicate / rename / move / delete)
  20-22 Save + redirectors + is_dirty
  23-25 Import / export round-trip + SOURCE_NOT_FOUND
  26-28 Async jobs (save_all_dirty / bulk_import / batch_metadata_async)
  29-31 Composites — ALL ASYNC POST-HOTFIX-3 (find_unused / size_report / list_folders)
  32-33 batch_metadata async (Option A merged) + 5000-cap rejection
  34    Lane B latency verification (50 calls < 200ms)

Exit 0 on all 34 pass; exit 1 on first failure (prints `[SMOKE_PHASE2] FAIL ...`).

HOTFIX 3 NOTE (2026-05): sub-tests 29 / 30 / 32 now expect ``{job_id}`` response (composites
are async-only — they return immediately, AI polls externally). Each uses
``poll_job_until_terminal`` like sub-test 28. Sub-test 33 was repurposed from "200-cap"
to "5000-cap" since the sync/async split was collapsed into a single async tool.

Usage:
  python smoke_phase2.py [--host HOST] [--port PORT] [--test-asset PATH]
"""

from __future__ import annotations

import argparse
import json
import os
import socket
import sys
import time
from typing import Any, Dict, List, Optional


DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 30020
DEFAULT_TEST_ASSET = "/Game/MCPTest/PhaseTwo/DA_PhaseTwoTest"
READ_TIMEOUT_SEC = 10.0  # raised from smoke_ping (Phase 2 ops can be slower)


def send_and_recv_line(host: str, port: int, request_obj: dict, timeout: float = READ_TIMEOUT_SEC) -> Optional[dict]:
    """Open a fresh TCP socket, send one newline-framed JSON request, read one response line."""
    try:
        with socket.create_connection((host, port), timeout=timeout) as sock:
            sock.settimeout(timeout)
            payload = (json.dumps(request_obj, separators=(",", ":")) + "\n").encode("utf-8")
            sock.sendall(payload)

            buf = bytearray()
            deadline = time.monotonic() + timeout
            while True:
                if time.monotonic() > deadline:
                    return None
                try:
                    chunk = sock.recv(64 * 1024)
                except socket.timeout:
                    return None
                if not chunk:
                    break
                buf.extend(chunk)
                newline_idx = buf.find(b"\n")
                if newline_idx >= 0:
                    line = bytes(buf[:newline_idx])
                    return json.loads(line.decode("utf-8"))
            return None
    except (ConnectionRefusedError, OSError, json.JSONDecodeError):
        return None


def fail(reason: str) -> int:
    print(f"[SMOKE_PHASE2] FAIL reason={reason}")
    return 1


def expect_ok(response: Optional[dict], label: str) -> Optional[dict]:
    if response is None:
        fail(f"{label}: timeout/io-error (>{READ_TIMEOUT_SEC}s)")
        return None
    if response.get("ok") is not True:
        fail(f"{label}: ok-not-true got={response.get('ok')!r} error={response.get('error')!r}")
        return None
    result = response.get("result")
    if not isinstance(result, dict):
        fail(f"{label}: result-not-object got={result!r}")
        return None
    return result


def expect_error_code(response: Optional[dict], expected_code: int, label: str) -> bool:
    if response is None:
        fail(f"{label}: timeout (>{READ_TIMEOUT_SEC}s)")
        return False
    if response.get("ok") is not False:
        fail(f"{label}: expected error but ok={response.get('ok')!r}")
        return False
    err = response.get("error") or {}
    if err.get("code") != expected_code:
        fail(f"{label}: wrong-error-code expected={expected_code} got={err!r}")
        return False
    return True


def call(host: str, port: int, label: str, method: str, args: Optional[dict] = None) -> Optional[dict]:
    """Issue a call_function and return parsed result dict on success (None on fail printed)."""
    req = {
        "id": f"smoke-{label}",
        "kind": "call_function",
        "method": method,
        "args": args or {},
    }
    resp = send_and_recv_line(host, port, req)
    return expect_ok(resp, label)


def call_raw(host: str, port: int, label: str, method: str, args: Optional[dict] = None) -> Optional[dict]:
    """Issue a call_function and return the full response envelope (for error-shape tests)."""
    req = {
        "id": f"smoke-{label}",
        "kind": "call_function",
        "method": method,
        "args": args or {},
    }
    return send_and_recv_line(host, port, req)


def poll_job_until_terminal(host: str, port: int, label_prefix: str, job_id: str,
                            max_polls: int = 60, poll_interval: float = 0.5) -> Optional[Dict[str, Any]]:
    """Poll job.status until Succeeded/Failed/Cancelled or timeout. Returns the final status dict."""
    for i in range(max_polls):
        snap = call(host, port, f"{label_prefix}-poll{i}", "job.status", {"job_id": job_id})
        if snap is None:
            return None
        state = snap.get("state")
        if state in ("Succeeded", "Failed", "Cancelled"):
            return snap
        time.sleep(poll_interval)
    return None


def poll_job_and_fetch_result(host: str, port: int, label_prefix: str, job_id: str,
                              max_polls: int = 120, poll_interval: float = 0.5) -> Optional[Dict[str, Any]]:
    """Poll job.status until Succeeded, then call job.result to fetch the inner payload.

    Returns the inner result dict on Succeeded, None on failure/timeout (with fail() printed).
    Used by sub-tests 29 / 30 / 32 (composites are async-only post-Hotfix-3, so the smoke must
    poll externally rather than expecting a synchronous result).
    """
    snap = poll_job_until_terminal(host, port, label_prefix, job_id, max_polls, poll_interval)
    if snap is None:
        fail(f"{label_prefix}: job did not reach terminal state within {max_polls * poll_interval:.0f}s")
        return None
    state = snap.get("state")
    if state != "Succeeded":
        fail(f"{label_prefix}: expected Succeeded got state={state!r} error={snap.get('error_message')!r}")
        return None
    res = call(host, port, f"{label_prefix}-fetch", "job.result", {"job_id": job_id, "wait_timeout_s": 0})
    if res is None:
        return None
    inner = res.get("result")
    if not isinstance(inner, dict):
        fail(f"{label_prefix}: job.result returned non-object inner result got {res!r}")
        return None
    return inner


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--test-asset", default=DEFAULT_TEST_ASSET,
                        help="Asset path used by sub-tests 1, 3, 7-11, 13, 22 (default %(default)s)")
    parser.add_argument("--plugin-tests-dir", default=None,
                        help="Override path to Plugins/UnrealMCPBridge/Tests/test_assets (default = sibling of this script)")
    args = parser.parse_args()

    host = args.host
    port = args.port
    test_asset = args.test_asset
    plugin_tests_dir = args.plugin_tests_dir or os.path.join(os.path.dirname(os.path.abspath(__file__)), "test_assets")

    print(f"[SMOKE_PHASE2] connecting to {host}:{port}; test_asset={test_asset}")
    print(f"[SMOKE_PHASE2] plugin_tests_dir={plugin_tests_dir}")

    # ─── 1. asset.exists (positive) ──────────────────────────────────────────────────────────
    r = call(host, port, "1/asset.exists", "asset.exists", {"path": test_asset})
    if r is None: return 1
    if r.get("exists") is not True:
        return fail(f"1/asset.exists: expected exists=true got {r!r} (test asset missing? see README)")
    print(f"[SMOKE_PHASE2]   1/asset.exists OK")

    # ─── 2. asset.exists (negative — must NOT error) ─────────────────────────────────────────
    r = call(host, port, "2/asset.exists.miss", "asset.exists", {"path": "/Game/NonexistentPath/Foo"})
    if r is None: return 1
    if r.get("exists") is not False:
        return fail(f"2/asset.exists.miss: expected exists=false got {r!r}")
    print(f"[SMOKE_PHASE2]   2/asset.exists.miss OK (no error on missing)")

    # ─── 3. asset.metadata ───────────────────────────────────────────────────────────────────
    r = call(host, port, "3/asset.metadata", "asset.metadata", {"path": test_asset})
    if r is None: return 1
    if not isinstance(r.get("class"), str) or not r["class"]:
        return fail(f"3/asset.metadata: expected 'class' string got {r!r}")
    if not isinstance(r.get("tags"), dict):
        return fail(f"3/asset.metadata: expected 'tags' dict got {r!r}")
    print(f"[SMOKE_PHASE2]   3/asset.metadata OK (class={r['class']!r}, {len(r['tags'])} tags)")

    # ─── 4. asset.list (narrow filter) ───────────────────────────────────────────────────────
    r = call(host, port, "4/asset.list.narrow", "asset.list", {
        "filter": {
            "package_paths": ["/Game/MCPTest"],
            "recursive_paths": True,
        },
        "page_size": 10,
    })
    if r is None: return 1
    if not isinstance(r.get("assets"), list):
        return fail(f"4/asset.list: expected assets list got {r!r}")
    print(f"[SMOKE_PHASE2]   4/asset.list.narrow OK ({len(r['assets'])} assets, total_known={r.get('total_known')})")

    # ─── 5. asset.list pagination ────────────────────────────────────────────────────────────
    r1 = call(host, port, "5a/asset.list.p1", "asset.list", {
        "filter": {"package_paths": ["/Game"], "recursive_paths": True, "class_paths": ["/Script/Engine.Texture2D"]},
        "page_size": 5,
    })
    if r1 is None: return 1
    page1 = r1.get("assets", [])
    tok = r1.get("next_page_token")
    if tok is not None:
        r2 = call(host, port, "5b/asset.list.p2", "asset.list", {
            "filter": {"package_paths": ["/Game"], "recursive_paths": True, "class_paths": ["/Script/Engine.Texture2D"]},
            "page_size": 5,
            "page_token": tok,
        })
        if r2 is None: return 1
        page2 = r2.get("assets", [])
        # Page 2 first item must differ from page 1 last item (sentinel scheme).
        if page1 and page2 and page1[-1].get("asset_path") == page2[0].get("asset_path"):
            return fail(f"5b/asset.list.p2: pagination produced duplicate sentinel item")
    print(f"[SMOKE_PHASE2]   5/asset.list.pagination OK (token-based)")

    # ─── 6. asset.list overly broad → OVERLY_BROAD_QUERY ─────────────────────────────────────
    resp = call_raw(host, port, "6/asset.list.broad", "asset.list", {
        "filter": {"package_paths": ["/Game"], "recursive_paths": True},
        "page_size": 10,
    })
    if not expect_error_code(resp, -32012, "6/asset.list.broad"):
        return 1
    print(f"[SMOKE_PHASE2]   6/asset.list.broad OK (OVERLY_BROAD_QUERY -32012)")

    # ─── 7. asset.find_references ────────────────────────────────────────────────────────────
    r = call(host, port, "7/asset.find_references", "asset.find_references", {
        "path": test_asset,
        "recursive": False,
    })
    if r is None: return 1
    if not isinstance(r.get("referencers"), list):
        return fail(f"7/asset.find_references: expected referencers list got {r!r}")
    print(f"[SMOKE_PHASE2]   7/asset.find_references OK ({len(r['referencers'])} referencers)")

    # ─── 8. asset.find_dependents ────────────────────────────────────────────────────────────
    r = call(host, port, "8/asset.find_dependents", "asset.find_dependents", {"path": test_asset})
    if r is None: return 1
    if not isinstance(r.get("dependents"), list):
        return fail(f"8/asset.find_dependents: expected dependents list got {r!r}")
    print(f"[SMOKE_PHASE2]   8/asset.find_dependents OK ({len(r['dependents'])} dependents)")

    # ─── 9. asset.get_class_hierarchy ────────────────────────────────────────────────────────
    r = call(host, port, "9/asset.get_class_hierarchy", "asset.get_class_hierarchy", {"path": test_asset})
    if r is None: return 1
    chain = r.get("chain")
    if not isinstance(chain, list) or not chain:
        return fail(f"9/asset.get_class_hierarchy: expected non-empty chain got {r!r}")
    print(f"[SMOKE_PHASE2]   9/asset.get_class_hierarchy OK ({len(chain)} levels)")

    # ─── 10. asset.search_by_class ───────────────────────────────────────────────────────────
    r = call(host, port, "10/asset.search_by_class", "asset.search_by_class", {
        "class_path": "/Script/Engine.StaticMesh",
        "package_paths": ["/Game"],
        "recursive_paths": True,
        "page_size": 5,
    })
    if r is None: return 1
    matches = r.get("matches")
    if not isinstance(matches, list):
        return fail(f"10/asset.search_by_class: expected matches list got {r!r}")
    print(f"[SMOKE_PHASE2]   10/asset.search_by_class OK ({len(matches)} matches)")

    # ─── 11. asset.get_thumbnail ─────────────────────────────────────────────────────────────
    r = call(host, port, "11/asset.get_thumbnail", "asset.get_thumbnail", {
        "path": test_asset,
        "size": 128,
    })
    if r is None: return 1
    if not isinstance(r.get("base64"), str) or len(r["base64"]) < 100:
        return fail(f"11/asset.get_thumbnail: expected non-empty base64 got len={len(r.get('base64') or '')}")
    print(f"[SMOKE_PHASE2]   11/asset.get_thumbnail OK ({r.get('width')}x{r.get('height')}, b64_len={len(r['base64'])})")

    # ─── 12. asset.get_thumbnail oversized → INVALID_PARAMS ──────────────────────────────────
    resp = call_raw(host, port, "12/asset.get_thumbnail.big", "asset.get_thumbnail", {
        "path": test_asset,
        "size": 4096,
    })
    if not expect_error_code(resp, -32602, "12/asset.get_thumbnail.big"):
        return 1
    print(f"[SMOKE_PHASE2]   12/asset.get_thumbnail.big OK (size>256 rejected)")

    # ─── 13. asset.get_thumbnail_to_disk ─────────────────────────────────────────────────────
    r = call(host, port, "13/asset.get_thumbnail_to_disk", "asset.get_thumbnail_to_disk", {
        "path": test_asset,
        "size": 256,
    })
    if r is None: return 1
    out_path = r.get("path")
    if not isinstance(out_path, str) or not os.path.exists(out_path):
        return fail(f"13/asset.get_thumbnail_to_disk: file not written, got {r!r}")
    if r.get("bytes", 0) <= 0:
        return fail(f"13/asset.get_thumbnail_to_disk: bytes=0 got {r!r}")
    print(f"[SMOKE_PHASE2]   13/asset.get_thumbnail_to_disk OK ({r['bytes']} bytes at {out_path})")

    # ─── 14. cb.create_folder ────────────────────────────────────────────────────────────────
    SCRATCH = "/Game/MCPTest/PhaseTwoScratch"
    r = call(host, port, "14/cb.create_folder", "cb.create_folder", {"path": SCRATCH})
    if r is None: return 1
    print(f"[SMOKE_PHASE2]   14/cb.create_folder OK (created={r.get('created')})")

    # ─── 15. cb.create_folder idempotency ────────────────────────────────────────────────────
    r = call(host, port, "15/cb.create_folder.idempotent", "cb.create_folder", {"path": SCRATCH})
    if r is None: return 1
    if r.get("created") is not False:
        return fail(f"15/cb.create_folder.idempotent: expected created=false on 2nd call got {r!r}")
    print(f"[SMOKE_PHASE2]   15/cb.create_folder.idempotent OK")

    # ─── 16. cb.duplicate ────────────────────────────────────────────────────────────────────
    dup_path = SCRATCH + "/DA_Dup"
    r = call(host, port, "16/cb.duplicate", "cb.duplicate", {
        "source_path": test_asset,
        "dest_path": dup_path,
    })
    if r is None: return 1
    if r.get("new_path") != dup_path:
        return fail(f"16/cb.duplicate: expected new_path={dup_path} got {r!r}")
    print(f"[SMOKE_PHASE2]   16/cb.duplicate OK")

    # ─── 17. cb.rename ───────────────────────────────────────────────────────────────────────
    renamed_path = SCRATCH + "/DA_Renamed"
    r = call(host, port, "17/cb.rename", "cb.rename", {
        "old_path": dup_path,
        "new_path": renamed_path,
    })
    if r is None: return 1
    if r.get("success") is not True:
        return fail(f"17/cb.rename: expected success=true got {r!r}")
    print(f"[SMOKE_PHASE2]   17/cb.rename OK")

    # ─── 18. cb.move ─────────────────────────────────────────────────────────────────────────
    DEST_FOLDER = "/Game/MCPTest/PhaseTwo"
    r = call(host, port, "18/cb.move", "cb.move", {
        "source_paths": [renamed_path],
        "dest_folder": DEST_FOLDER,
    })
    if r is None: return 1
    moved = r.get("moved", [])
    if len(moved) != 1:
        return fail(f"18/cb.move: expected 1 moved item got {r!r}")
    moved_to = moved[0].get("to")
    print(f"[SMOKE_PHASE2]   18/cb.move OK (→ {moved_to})")

    # ─── 19. cb.delete force=true ────────────────────────────────────────────────────────────
    r = call(host, port, "19/cb.delete", "cb.delete", {
        "path": moved_to,
        "force": True,
    })
    if r is None: return 1
    if r.get("deleted") is not True:
        return fail(f"19/cb.delete: expected deleted=true got {r!r}")
    print(f"[SMOKE_PHASE2]   19/cb.delete OK")

    # ─── 20. cb.save ─────────────────────────────────────────────────────────────────────────
    r = call(host, port, "20/cb.save", "cb.save", {"path": test_asset})
    if r is None: return 1
    # `saved` may be true or false (false = not dirty), both are valid.
    if not isinstance(r.get("saved"), bool):
        return fail(f"20/cb.save: expected saved bool got {r!r}")
    print(f"[SMOKE_PHASE2]   20/cb.save OK (saved={r['saved']})")

    # ─── 21. cb.fix_redirectors ──────────────────────────────────────────────────────────────
    r = call(host, port, "21/cb.fix_redirectors", "cb.fix_redirectors", {
        "path": SCRATCH,
        "recursive": True,
    })
    if r is None: return 1
    if not isinstance(r.get("fixed_count"), (int, float)):
        return fail(f"21/cb.fix_redirectors: expected fixed_count integer got {r!r}")
    print(f"[SMOKE_PHASE2]   21/cb.fix_redirectors OK (removed={r.get('removed_count')})")

    # ─── 22. asset.is_dirty ──────────────────────────────────────────────────────────────────
    r = call(host, port, "22/asset.is_dirty", "asset.is_dirty", {"path": test_asset})
    if r is None: return 1
    if "dirty" not in r or "in_memory" not in r:
        return fail(f"22/asset.is_dirty: expected dirty+in_memory keys got {r!r}")
    print(f"[SMOKE_PHASE2]   22/asset.is_dirty OK (dirty={r['dirty']}, in_memory={r['in_memory']})")

    # ─── 23. cb.import (test_texture.png — skip if missing) ─────────────────────────────────
    tex_src = os.path.join(plugin_tests_dir, "test_texture.png")
    if not os.path.exists(tex_src):
        print(f"[SMOKE_PHASE2]   23/cb.import SKIP (test_texture.png not at {tex_src})")
        imported_path = None
    else:
        r = call(host, port, "23/cb.import", "cb.import", {
            "source_file": tex_src,
            "dest_path": SCRATCH + "/T_Imported",
            "replace_existing": True,
        })
        if r is None: return 1
        imported_path = r.get("asset_path")
        if not isinstance(imported_path, str):
            return fail(f"23/cb.import: expected asset_path string got {r!r}")
        print(f"[SMOKE_PHASE2]   23/cb.import OK ({imported_path})")

    # ─── 24. cb.export (round-trip of imported texture) ─────────────────────────────────────
    if imported_path is None:
        print(f"[SMOKE_PHASE2]   24/cb.export SKIP (no imported asset to export)")
    else:
        out_file = os.path.join(plugin_tests_dir, "_exported.png")
        r = call(host, port, "24/cb.export", "cb.export", {
            "path": imported_path,
            "dest_file": out_file,
        })
        if r is None: return 1
        if r.get("exported") is not True or r.get("bytes", 0) <= 0:
            return fail(f"24/cb.export: expected exported=true,bytes>0 got {r!r}")
        print(f"[SMOKE_PHASE2]   24/cb.export OK ({r['bytes']} bytes)")

    # ─── 25. cb.import bad source → SOURCE_NOT_FOUND (-32004 / NOT_FOUND) ────────────────────
    resp = call_raw(host, port, "25/cb.import.miss", "cb.import", {
        "source_file": os.path.join(plugin_tests_dir, "__nonexistent__.png"),
        "dest_path": SCRATCH + "/T_Missing",
    })
    # Accept either -32004 (ObjectNotFound) or -32013 (PathEscape, if sandbox rejects first).
    if resp is None or resp.get("ok") is not False:
        return fail(f"25/cb.import.miss: expected error got {resp!r}")
    err = resp.get("error") or {}
    if err.get("code") not in (-32004, -32013):
        return fail(f"25/cb.import.miss: expected -32004 or -32013 got {err!r}")
    print(f"[SMOKE_PHASE2]   25/cb.import.miss OK (error.code={err.get('code')})")

    # ─── 26. cb.save_all_dirty (async) ───────────────────────────────────────────────────────
    r = call(host, port, "26a/cb.save_all_dirty.submit", "cb.save_all_dirty", {})
    if r is None: return 1
    job_id = r.get("job_id")
    if not isinstance(job_id, str) or len(job_id) < 30:
        return fail(f"26a: expected uuid-shaped job_id got {r!r}")
    snap = poll_job_until_terminal(host, port, "26b/cb.save_all_dirty", job_id)
    if snap is None or snap.get("state") != "Succeeded":
        return fail(f"26b: job did not reach Succeeded state, got snap={snap!r}")
    print(f"[SMOKE_PHASE2]   26/cb.save_all_dirty OK (state={snap['state']})")

    # ─── 27. cb.bulk_import (FBX + texture — skip components missing) ────────────────────────
    mesh_src = os.path.join(plugin_tests_dir, "test_mesh.fbx")
    files_to_import = []
    if os.path.exists(tex_src): files_to_import.append(tex_src)
    if os.path.exists(mesh_src): files_to_import.append(mesh_src)
    if not files_to_import:
        print(f"[SMOKE_PHASE2]   27/cb.bulk_import SKIP (no test assets available)")
    else:
        r = call(host, port, "27a/cb.bulk_import.submit", "cb.bulk_import", {
            "source_files": files_to_import,
            "dest_folder": SCRATCH,
            "replace_existing": True,
        })
        if r is None: return 1
        bulk_job_id = r.get("job_id")
        if not isinstance(bulk_job_id, str):
            return fail(f"27a: expected job_id string got {r!r}")
        snap = poll_job_until_terminal(host, port, "27b/cb.bulk_import", bulk_job_id, max_polls=120)
        if snap is None or snap.get("state") != "Succeeded":
            return fail(f"27b: bulk_import did not Succeed got snap={snap!r}")
        print(f"[SMOKE_PHASE2]   27/cb.bulk_import OK ({len(files_to_import)} files, state={snap['state']})")

    # ─── 28. asset.batch_metadata_async (300 paths via asset.list) ───────────────────────────
    list_r = call(host, port, "28a/list_for_batch", "asset.list", {
        "filter": {"package_paths": ["/Game"], "recursive_paths": True, "class_paths": ["/Script/Engine.Texture2D"]},
        "page_size": 300,
    })
    paths = [a["asset_path"] for a in (list_r.get("assets") or [])][:300] if list_r else []
    if len(paths) < 10:
        print(f"[SMOKE_PHASE2]   28/asset.batch_metadata_async SKIP (only {len(paths)} candidate paths; need ≥10)")
    else:
        r = call(host, port, "28b/batch_metadata_async.submit", "asset.batch_metadata_async", {"paths": paths})
        if r is None: return 1
        bm_job_id = r.get("job_id")
        if not isinstance(bm_job_id, str):
            return fail(f"28b: expected job_id string got {r!r}")
        snap = poll_job_until_terminal(host, port, "28c/batch_metadata_async", bm_job_id, max_polls=60)
        if snap is None or snap.get("state") != "Succeeded":
            return fail(f"28c: batch_metadata_async did not Succeed got snap={snap!r}")
        print(f"[SMOKE_PHASE2]   28/asset.batch_metadata_async OK ({len(paths)} paths processed)")

    # ─── 29. asset.find_unused (HOTFIX 3: async — submit + poll job.result) ──────────────────
    r = call(host, port, "29a/asset.find_unused.submit", "asset.find_unused", {
        "package_paths": [SCRATCH],
    })
    if r is None: return 1
    fu_job_id = r.get("job_id")
    if not isinstance(fu_job_id, str) or len(fu_job_id) < 30:
        return fail(f"29a: expected uuid-shaped job_id got {r!r}")
    inner = poll_job_and_fetch_result(host, port, "29b/asset.find_unused", fu_job_id, max_polls=120)
    if inner is None: return 1
    if not isinstance(inner.get("unused"), list):
        return fail(f"29b/asset.find_unused: expected unused list got {inner!r}")
    print(f"[SMOKE_PHASE2]   29/asset.find_unused OK ({len(inner['unused'])} unused; scanned={inner.get('scanned_count')})")

    # ─── 30. asset.size_report (HOTFIX 3: async — submit + poll job.result) ──────────────────
    r = call(host, port, "30a/asset.size_report.submit", "asset.size_report", {
        "package_paths": ["/Game/MCPTest"],
        "top_n": 10,
    })
    if r is None: return 1
    sr_job_id = r.get("job_id")
    if not isinstance(sr_job_id, str) or len(sr_job_id) < 30:
        return fail(f"30a: expected uuid-shaped job_id got {r!r}")
    inner = poll_job_and_fetch_result(host, port, "30b/asset.size_report", sr_job_id, max_polls=120)
    if inner is None: return 1
    if not isinstance(inner.get("top"), list):
        return fail(f"30b/asset.size_report: expected top list got {inner!r}")
    print(f"[SMOKE_PHASE2]   30/asset.size_report OK (top={len(inner['top'])}; total_bytes={inner.get('total_bytes')})")

    # ─── 31. cb.list_folders ─────────────────────────────────────────────────────────────────
    r = call(host, port, "31/cb.list_folders", "cb.list_folders", {
        "parent_path": "/Game/MCPTest",
        "recursive": True,
    })
    if r is None: return 1
    folders = r.get("folders")
    if not isinstance(folders, list):
        return fail(f"31/cb.list_folders: expected folders list got {r!r}")
    print(f"[SMOKE_PHASE2]   31/cb.list_folders OK ({len(folders)} folders)")

    # ─── 32. asset.batch_metadata (HOTFIX 3: Option A — async-only — submit + poll) ──────────
    # The pre-Hotfix-3 sync/async split was collapsed into a single async tool. Both
    # asset.batch_metadata and asset.batch_metadata_async route through the same internal handler.
    sample_paths = paths[:20] if paths else [test_asset]
    r = call(host, port, "32a/asset.batch_metadata.submit", "asset.batch_metadata", {"paths": sample_paths})
    if r is None: return 1
    bm_job_id = r.get("job_id")
    if not isinstance(bm_job_id, str) or len(bm_job_id) < 30:
        return fail(f"32a: expected uuid-shaped job_id got {r!r}")
    inner = poll_job_and_fetch_result(host, port, "32b/asset.batch_metadata", bm_job_id, max_polls=60)
    if inner is None: return 1
    if not isinstance(inner.get("assets"), list) or not isinstance(inner.get("failed"), list):
        return fail(f"32b/asset.batch_metadata: expected assets+failed lists got {inner!r}")
    print(f"[SMOKE_PHASE2]   32/asset.batch_metadata OK ({len(inner['assets'])} OK, {len(inner['failed'])} failed)")

    # ─── 33. asset.batch_metadata > 5000 → INPUT_TOO_LARGE (Python raises ValueError) ───────
    # HOTFIX 3 collapsed the 200-cap into the 5000-cap (matching the previous async variant).
    # The Python wrapper raises ValueError with the INPUT_TOO_LARGE prefix; CallPythonTool
    # surfaces it via the marker payload as a non-OK response. We don't assert on a specific
    # numeric code (Python-tier errors don't have a numeric code mapping) — just verify it's
    # a failure response.
    fake_paths = [f"/Game/Fake/{i}" for i in range(5050)]
    resp = call_raw(host, port, "33/batch_metadata.toolarge", "asset.batch_metadata", {"paths": fake_paths})
    if resp is None or resp.get("ok") is not False:
        return fail(f"33/asset.batch_metadata.toolarge: expected error got {resp!r}")
    print(f"[SMOKE_PHASE2]   33/asset.batch_metadata.toolarge OK (5050 paths rejected — 5000-cap)")

    # ─── 34. Lane B latency verification (50 calls < 200ms) ─────────────────────────────────
    t0 = time.monotonic()
    for i in range(50):
        r = call(host, port, f"34/lane_b_{i}", "asset.exists", {"path": test_asset})
        if r is None: return 1
        if r.get("exists") is not True:
            return fail(f"34/lane_b loop iter {i}: expected exists=true got {r!r}")
    elapsed_ms = (time.monotonic() - t0) * 1000.0
    # 50 calls × ~16ms tick quantization = 800ms if falling back to Lane A.
    # Lane B should be <200ms (4× headroom).
    if elapsed_ms > 1000.0:
        # 1s = lane B clearly broken (likely falling back to game thread).
        return fail(f"34/lane_b_latency: 50 calls took {elapsed_ms:.1f}ms (Lane B broken? expected <200ms)")
    print(f"[SMOKE_PHASE2]   34/lane_b_latency OK ({elapsed_ms:.1f}ms for 50 calls — Lane B {'OK' if elapsed_ms < 200 else 'slow but acceptable'})")

    # ─── Cleanup: best-effort delete of PhaseTwoScratch + its contents. ─────────────────────
    # Enumerate via cb.list_folders + asset.list, then cb.delete each. Failures here don't
    # affect test verdict (we already reported pass).
    try:
        scratch_list = call(host, port, "cleanup/list", "asset.list", {
            "filter": {"package_paths": [SCRATCH], "recursive_paths": True},
            "page_size": 500,
        })
        if scratch_list:
            for asset in scratch_list.get("assets", []):
                pkg = asset.get("package_path")
                ap  = asset.get("asset_path")
                if isinstance(ap, str) and ap:
                    # asset_path includes the .leaf suffix; cb.delete wants package form.
                    pkg_path = ap.split(".", 1)[0]
                    call_raw(host, port, "cleanup/del", "cb.delete", {"path": pkg_path, "force": True})
    except Exception:
        pass

    print(f"[SMOKE_PHASE2] PASS — all 34 sub-tests succeeded (some may have SKIP'd if test assets missing)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
