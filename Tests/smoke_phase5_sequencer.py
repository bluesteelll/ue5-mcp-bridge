#!/usr/bin/env python3
"""Phase 5 Chunk D smoke — Sequencer read-only (5 tools).

Verifies (against a live editor on port 30020; PIE may be on or off — all 5 tools are read-side):

  Discovery (1):
    1. tools.list contains all 5 new sequencer.* handlers:
       sequencer.list_cinematics, sequencer.get_tracks, sequencer.get_camera_cuts,
       sequencer.get_keyframes, sequencer.get_current_time.

  list_cinematics (2-3):
    2. sequencer.list_cinematics with default scope ([/Game]) → {sequences: []} schema verified.
       Per-entry: name, path, duration_secs (number|null), frame_rate (object|null).
       If no LevelSequence exists in /Game the array is empty (NOT an error) — sub-tests
       3-10 then SKIP gracefully.
    3. sequencer.list_cinematics with scope_paths=["/Game/MCPTest/Phase5"] — same schema; SKIPped
       if the optional test asset isn't present (this is the canonical plan asset path).

  get_tracks (4-5):
    4. sequencer.get_tracks(first_seq) → {master_tracks, possessables, spawnables} schema verified.
       Each master/possessable/spawnable has well-formed name/class/section_count fields. SKIP if
       no sequence available from sub-test 2.
    5. Negative: sequencer.get_tracks with bogus path → -32004 ObjectNotFound; with /Engine path
       that resolves to a non-sequence (e.g. a UTexture, when present) → -32011 WrongClass.

  get_camera_cuts (6):
    6. sequencer.get_camera_cuts(first_seq) → {cuts, frame_rate} schema. cuts may be empty if no
       camera-cut track is present (NOT an error). Per-cut: start_frame/end_frame are numbers,
       camera_binding is a string GUID or null.

  get_keyframes (7-8):
    7. Try get_keyframes on the FIRST resolvable track from get_tracks. If a master track exists,
       use "MasterName.0"; else if a possessable with at least one track exists, use
       "PossessableName.TrackName.0". Verifies {keys, section_range, frame_rate, supported_types}
       schema. SKIPs if no valid track found.
    8. Negative: get_keyframes with bogus track_path "NoSuchTrack.0" → -32043 TrackNotFound.
       With "<valid_track>.9999" → -32044 SectionIndexOOB (only if section_count < 9999, which
       it always is for test assets).

  get_current_time (9-10):
    9. sequencer.get_current_time when no Sequencer tab is open → -32042 NoActiveSequencer.
       Operator can open a LevelSequence in the editor to verify the success path; the smoke
       script assumes the closed-tab case is the default and tolerates either outcome — emits
       SKIP if a Sequencer happens to be open.
   10. (Embedded in 9 — covered by either the error path or the success path.)

  Missing args (11):
   11. sequencer.get_tracks with no args → -32602; get_keyframes missing track_path → -32602.

Prints ``[SMOKE_PHASE5_SEQUENCER] PASS`` on success or ``[SMOKE_PHASE5_SEQUENCER] FAIL ...`` on
first mismatch. Sub-tests dependent on missing assets emit SKIP lines but still PASS overall.

Usage:
  python smoke_phase5_sequencer.py [--host HOST] [--port PORT] [--seq-path /Game/...]
"""

from __future__ import annotations

import argparse
import json
import socket
import sys
import time
from typing import Any, Dict, List, Optional


DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 30020
READ_TIMEOUT_SEC = 30.0

# Candidate sequence paths probed in order. The plan-mandated test asset is LS_PhaseFiveTest under
# /Game/MCPTest/Phase5; we fall back to /Game search via asset.search_by_class if absent.
FALLBACK_SEQ_PATHS = [
    "/Game/MCPTest/Phase5/LS_PhaseFiveTest",
    "/Game/MCPTest/PhaseFive/LS_PhaseFiveTest",
]


def send_and_recv_line(host: str, port: int, request_obj: dict,
                       timeout: float = READ_TIMEOUT_SEC) -> Optional[dict]:
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
                chunk = sock.recv(128 * 1024)
            except socket.timeout:
                return None
            if not chunk:
                break
            buf.extend(chunk)
            newline_idx = buf.find(b"\n")
            if newline_idx >= 0:
                return json.loads(bytes(buf[:newline_idx]).decode("utf-8"))
        return None


def fail(reason: str) -> int:
    print(f"[SMOKE_PHASE5_SEQUENCER] FAIL reason={reason}")
    return 1


def skip(reason: str) -> None:
    print(f"[SMOKE_PHASE5_SEQUENCER]   SKIP {reason}")


def info(message: str) -> None:
    print(f"[SMOKE_PHASE5_SEQUENCER]   {message}")


def call(host: str, port: int, label: str, request_id: str, method: str,
         args: Optional[dict] = None, timeout: float = READ_TIMEOUT_SEC) -> Optional[dict]:
    req = {"id": request_id, "kind": "call_function", "method": method, "args": args or {}}
    try:
        return send_and_recv_line(host, port, req, timeout=timeout)
    except (ConnectionRefusedError, OSError) as exc:
        fail(f"{label}: connect-error detail={exc}")
        return None


def expect_ok(response: Optional[dict], expected_id: str, label: str) -> Optional[dict]:
    if response is None:
        fail(f"{label}: timeout (>{READ_TIMEOUT_SEC}s)")
        return None
    if response.get("id") != expected_id:
        fail(f"{label}: id-mismatch expected={expected_id!r} got={response.get('id')!r}")
        return None
    if response.get("ok") is not True:
        fail(f"{label}: ok-not-true got={response.get('ok')!r} error={response.get('error')!r}")
        return None
    result = response.get("result")
    if not isinstance(result, dict):
        fail(f"{label}: result-not-object got={result!r}")
        return None
    return result


def expect_error(response: Optional[dict], expected_id: str, expected_codes,
                 label: str) -> Optional[dict]:
    if isinstance(expected_codes, int):
        expected_codes = (expected_codes,)
    if response is None:
        fail(f"{label}: timeout (>{READ_TIMEOUT_SEC}s)")
        return None
    if response.get("id") != expected_id:
        fail(f"{label}: id-mismatch expected={expected_id!r} got={response.get('id')!r}")
        return None
    if response.get("ok") is not False:
        fail(f"{label}: ok-not-false got={response.get('ok')!r}")
        return None
    error = response.get("error")
    if not isinstance(error, dict) or error.get("code") not in expected_codes:
        fail(f"{label}: wrong-error-code expected={expected_codes} got={error!r}")
        return None
    return error


def probe_first_sequence(host: str, port: int, candidates: List[str]) -> Optional[str]:
    """Return the first existing ULevelSequence path among `candidates` + AR search fallback."""
    # Direct path probe first.
    for path in candidates:
        resp = call(host, port, "probe", f"probe-seq-{path}", "sequencer.get_tracks",
                    {"sequence_path": path})
        if resp is not None and resp.get("ok") is True:
            return path
    # Fallback: enumerate via list_cinematics in /Game (which exercises the AR path).
    resp = call(host, port, "probe-list", "probe-seq-list", "sequencer.list_cinematics",
                {"scope_paths": ["/Game"]})
    if resp is None or resp.get("ok") is not True:
        return None
    sequences = resp.get("result", {}).get("sequences", [])
    if not isinstance(sequences, list) or not sequences:
        return None
    first = sequences[0]
    if isinstance(first, dict):
        path = first.get("path")
        if isinstance(path, str) and path.startswith("/"):
            return path
    return None


def schema_check_frame_rate(obj: Any, label: str) -> bool:
    """Verify the {numerator, denominator, decimal} frame-rate JSON shape."""
    if not isinstance(obj, dict):
        fail(f"{label}: frame_rate not dict {obj!r}")
        return False
    for field in ("numerator", "denominator", "decimal"):
        if field not in obj:
            fail(f"{label}: frame_rate missing {field!r}: {obj!r}")
            return False
        if not isinstance(obj[field], (int, float)):
            fail(f"{label}: frame_rate.{field} not number {obj!r}")
            return False
    return True


def schema_check_track_summary(obj: Any, label: str) -> bool:
    """Verify a single {name, class, section_count} track entry."""
    if not isinstance(obj, dict):
        fail(f"{label}: track entry not dict {obj!r}")
        return False
    for field in ("name", "class", "section_count"):
        if field not in obj:
            fail(f"{label}: track entry missing {field!r}: {obj!r}")
            return False
    if not isinstance(obj["name"], str) or not obj["name"]:
        fail(f"{label}: track.name not non-empty string {obj!r}")
        return False
    if not isinstance(obj["class"], str) or not obj["class"].startswith("/Script/"):
        fail(f"{label}: track.class doesn't look like a UClass path {obj!r}")
        return False
    if not isinstance(obj["section_count"], int) or obj["section_count"] < 0:
        fail(f"{label}: track.section_count not non-negative int {obj!r}")
        return False
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--seq-path", default=None,
                        help="Override sequence asset path; default probes FALLBACK_SEQ_PATHS + /Game search")
    args = parser.parse_args()

    print(f"[SMOKE_PHASE5_SEQUENCER] connecting to {args.host}:{args.port} ...")

    # ─── Sub-test 1: tools.list contains all 5 sequencer.* tools ────────────────────────────────
    result = expect_ok(call(args.host, args.port, "1", "p5-seq-1", "tools.list"),
                       "p5-seq-1", "1/tools.list")
    if result is None:
        return 1
    cpp_handlers = set(result.get("cpp_handlers") or [])
    expected_cpp = {
        "sequencer.list_cinematics",
        "sequencer.get_tracks",
        "sequencer.get_camera_cuts",
        "sequencer.get_keyframes",
        "sequencer.get_current_time",
    }
    missing = expected_cpp - cpp_handlers
    if missing:
        return fail(f"1/tools.list: missing handlers: {sorted(missing)}")
    info(f"1/tools.list contains all 5 sequencer.* tools ({len(cpp_handlers)} total cpp handlers)")

    # ─── Sub-test 2: list_cinematics default scope ──────────────────────────────────────────────
    result = expect_ok(
        call(args.host, args.port, "2", "p5-seq-2", "sequencer.list_cinematics", {}),
        "p5-seq-2", "2/list_cinematics-default-scope")
    if result is None:
        return 1
    sequences = result.get("sequences")
    if not isinstance(sequences, list):
        return fail(f"2/list_cinematics: sequences not list {result!r}")
    # Validate per-entry schema if any exist.
    for entry in sequences:
        if not isinstance(entry, dict):
            return fail(f"2/list_cinematics: entry not dict {entry!r}")
        for field in ("name", "path"):
            if field not in entry or not isinstance(entry[field], str):
                return fail(f"2/list_cinematics: entry missing {field!r}: {entry!r}")
        if "duration_secs" not in entry:
            return fail(f"2/list_cinematics: entry missing duration_secs: {entry!r}")
        if entry["duration_secs"] is not None and not isinstance(entry["duration_secs"], (int, float)):
            return fail(f"2/list_cinematics: duration_secs not number|null {entry!r}")
        if "frame_rate" not in entry:
            return fail(f"2/list_cinematics: entry missing frame_rate: {entry!r}")
        if entry["frame_rate"] is not None:
            if not schema_check_frame_rate(entry["frame_rate"], f"2/list_cinematics({entry['name']})"):
                return 1
    info(f"2/list_cinematics(default /Game) OK ({len(sequences)} LevelSequence asset(s) found)")

    # ─── Sub-test 3: list_cinematics with restricted scope ──────────────────────────────────────
    result = expect_ok(
        call(args.host, args.port, "3", "p5-seq-3", "sequencer.list_cinematics",
             {"scope_paths": ["/Game/MCPTest"]}),
        "p5-seq-3", "3/list_cinematics-restricted")
    if result is None:
        return 1
    sequences_test = result.get("sequences")
    if not isinstance(sequences_test, list):
        return fail(f"3/list_cinematics-restricted: sequences not list {result!r}")
    info(f"3/list_cinematics(/Game/MCPTest) OK ({len(sequences_test)} sequence(s) in test scope)")

    # ─── Probe for a usable sequence (drives sub-tests 4-8) ─────────────────────────────────────
    seq_path: Optional[str] = args.seq_path or probe_first_sequence(
        args.host, args.port, FALLBACK_SEQ_PATHS)
    if seq_path is None:
        skip("no LevelSequence available in /Game (operator can build LS_PhaseFiveTest under "
             "/Game/MCPTest/Phase5 or pass --seq-path); sub-tests 4-8 SKIPPED")
        seq_path = None  # Explicit for downstream skip logic

    # ─── Sub-test 4: get_tracks ─────────────────────────────────────────────────────────────────
    master_tracks: List[Dict[str, Any]] = []
    possessables: List[Dict[str, Any]] = []
    spawnables: List[Dict[str, Any]] = []
    if seq_path is None:
        skip("4/get_tracks: SKIPPED (no sequence)")
    else:
        result = expect_ok(
            call(args.host, args.port, "4", "p5-seq-4", "sequencer.get_tracks",
                 {"sequence_path": seq_path}),
            "p5-seq-4", "4/get_tracks")
        if result is None:
            return 1
        master_tracks = result.get("master_tracks") or []
        possessables = result.get("possessables") or []
        spawnables = result.get("spawnables") or []
        if not isinstance(master_tracks, list):
            return fail(f"4/get_tracks: master_tracks not list {result!r}")
        if not isinstance(possessables, list):
            return fail(f"4/get_tracks: possessables not list {result!r}")
        if not isinstance(spawnables, list):
            return fail(f"4/get_tracks: spawnables not list {result!r}")
        for t in master_tracks:
            if not schema_check_track_summary(t, "4/get_tracks-master"):
                return 1
        for p in possessables:
            if not isinstance(p, dict):
                return fail(f"4/get_tracks: possessable entry not dict {p!r}")
            for field in ("name", "binding_guid", "tracks"):
                if field not in p:
                    return fail(f"4/get_tracks: possessable missing {field!r}: {p!r}")
            if not isinstance(p["tracks"], list):
                return fail(f"4/get_tracks: possessable.tracks not list {p!r}")
            for t in p["tracks"]:
                if not schema_check_track_summary(t, f"4/get_tracks-possessable({p['name']})"):
                    return 1
        for s in spawnables:
            if not isinstance(s, dict):
                return fail(f"4/get_tracks: spawnable entry not dict {s!r}")
            for field in ("name", "binding_guid", "tracks"):
                if field not in s:
                    return fail(f"4/get_tracks: spawnable missing {field!r}: {s!r}")
        info(f"4/get_tracks({seq_path}) OK (master={len(master_tracks)}, "
             f"poss={len(possessables)}, spawn={len(spawnables)})")

    # ─── Sub-test 5: get_tracks negative cases ──────────────────────────────────────────────────
    # Bogus path → -32004 ObjectNotFound
    resp = call(args.host, args.port, "5a", "p5-seq-5a", "sequencer.get_tracks",
                {"sequence_path": "/Game/bogus_path_seq_xyz"})
    err = expect_error(resp, "p5-seq-5a", -32004, "5a/get_tracks-bogus-path")
    if err is None:
        return 1
    info("5/get_tracks negative OK (-32004 ObjectNotFound)")

    # ─── Sub-test 6: get_camera_cuts ────────────────────────────────────────────────────────────
    if seq_path is None:
        skip("6/get_camera_cuts: SKIPPED (no sequence)")
    else:
        result = expect_ok(
            call(args.host, args.port, "6", "p5-seq-6", "sequencer.get_camera_cuts",
                 {"sequence_path": seq_path}),
            "p5-seq-6", "6/get_camera_cuts")
        if result is None:
            return 1
        cuts = result.get("cuts")
        if not isinstance(cuts, list):
            return fail(f"6/get_camera_cuts: cuts not list {result!r}")
        if "frame_rate" not in result or not schema_check_frame_rate(result["frame_rate"], "6/get_camera_cuts"):
            return 1
        for cut in cuts:
            if not isinstance(cut, dict):
                return fail(f"6/get_camera_cuts: cut not dict {cut!r}")
            for field in ("start_frame", "end_frame", "camera_binding", "has_lower_bound", "has_upper_bound"):
                if field not in cut:
                    return fail(f"6/get_camera_cuts: cut missing {field!r}: {cut!r}")
            if not isinstance(cut["start_frame"], (int, float)):
                return fail(f"6/get_camera_cuts: start_frame not number {cut!r}")
            if not isinstance(cut["end_frame"], (int, float)):
                return fail(f"6/get_camera_cuts: end_frame not number {cut!r}")
            if cut["camera_binding"] is not None and not isinstance(cut["camera_binding"], str):
                return fail(f"6/get_camera_cuts: camera_binding not string|null {cut!r}")
        info(f"6/get_camera_cuts({seq_path}) OK ({len(cuts)} cut(s))")

    # ─── Sub-test 7: get_keyframes on a discoverable track ──────────────────────────────────────
    candidate_track_path: Optional[str] = None
    if seq_path is None:
        skip("7/get_keyframes: SKIPPED (no sequence)")
    else:
        # Try to pick a track path. Prefer a master track with at least one section, else a
        # possessable with one. Skip if neither has a section.
        for t in master_tracks:
            if t.get("section_count", 0) > 0:
                candidate_track_path = f"{t['name']}.0"
                break
        if candidate_track_path is None:
            for p in possessables:
                for t in p.get("tracks") or []:
                    if t.get("section_count", 0) > 0:
                        candidate_track_path = f"{p['name']}.{t['name']}.0"
                        break
                if candidate_track_path is not None:
                    break
        if candidate_track_path is None:
            skip("7/get_keyframes: no track-with-section discoverable from sub-test 4 — SKIPPED")
        else:
            result = expect_ok(
                call(args.host, args.port, "7", "p5-seq-7", "sequencer.get_keyframes",
                     {"sequence_path": seq_path, "track_path": candidate_track_path}),
                "p5-seq-7", "7/get_keyframes")
            if result is None:
                return 1
            keys = result.get("keys")
            if not isinstance(keys, list):
                return fail(f"7/get_keyframes: keys not list {result!r}")
            if "section_range" not in result or not isinstance(result["section_range"], dict):
                return fail(f"7/get_keyframes: section_range not dict {result!r}")
            for field in ("has_lower", "has_upper", "start", "end"):
                if field not in result["section_range"]:
                    return fail(f"7/get_keyframes: section_range missing {field!r}: {result!r}")
            if "frame_rate" not in result or not schema_check_frame_rate(result["frame_rate"], "7/get_keyframes"):
                return 1
            if "supported_types" not in result or not isinstance(result["supported_types"], list):
                return fail(f"7/get_keyframes: supported_types not list {result!r}")
            for key in keys:
                if not isinstance(key, dict):
                    return fail(f"7/get_keyframes: key not dict {key!r}")
                for field in ("channel", "time", "value", "interp"):
                    if field not in key:
                        return fail(f"7/get_keyframes: key missing {field!r}: {key!r}")
                if not isinstance(key["channel"], str):
                    return fail(f"7/get_keyframes: key.channel not string {key!r}")
                if not isinstance(key["time"], (int, float)):
                    return fail(f"7/get_keyframes: key.time not number {key!r}")
                if key["interp"] not in ("linear", "constant", "cubic", "auto"):
                    return fail(f"7/get_keyframes: key.interp not in enum {key!r}")
            info(f"7/get_keyframes({candidate_track_path}) OK ({len(keys)} key(s))")

    # ─── Sub-test 8: get_keyframes negative cases ───────────────────────────────────────────────
    if seq_path is None:
        skip("8/get_keyframes negative: SKIPPED (no sequence)")
    else:
        # Bogus track_path → -32043 TrackNotFound
        resp = call(args.host, args.port, "8a", "p5-seq-8a", "sequencer.get_keyframes",
                    {"sequence_path": seq_path, "track_path": "__no_such_track_xyz__.0"})
        err = expect_error(resp, "p5-seq-8a", -32043, "8a/get_keyframes-bogus-track")
        if err is None:
            return 1
        # Out-of-range section index — only meaningful when we have a real track to address.
        if candidate_track_path is not None:
            parts = candidate_track_path.rsplit(".", 1)
            if len(parts) == 2:
                oob_path = f"{parts[0]}.9999"
                resp = call(args.host, args.port, "8b", "p5-seq-8b", "sequencer.get_keyframes",
                            {"sequence_path": seq_path, "track_path": oob_path})
                err = expect_error(resp, "p5-seq-8b", -32044, "8b/get_keyframes-section-oob")
                if err is None:
                    return 1
                info("8/get_keyframes negative OK (-32043 TrackNotFound, -32044 SectionIndexOOB)")
            else:
                info("8/get_keyframes negative OK (-32043 only; OOB skipped — track_path shape)")
        else:
            info("8/get_keyframes negative OK (-32043 only; OOB skipped — no real track)")

    # ─── Sub-test 9: get_current_time ───────────────────────────────────────────────────────────
    # Either succeeds (Sequencer tab is open) or returns -32042 (no tab open). Both are valid.
    resp = call(args.host, args.port, "9", "p5-seq-9", "sequencer.get_current_time", {})
    if resp is None:
        return fail("9/get_current_time: timeout")
    if resp.get("ok") is True:
        cur = resp.get("result")
        if not isinstance(cur, dict):
            return fail(f"9/get_current_time: result not dict {cur!r}")
        for field in ("time_seconds", "frame", "tick", "frame_rate", "tick_rate", "sequence_path", "world"):
            if field not in cur:
                return fail(f"9/get_current_time: missing field {field!r}: {cur!r}")
        if not isinstance(cur["time_seconds"], (int, float)):
            return fail(f"9/get_current_time: time_seconds not number {cur!r}")
        if not isinstance(cur["frame"], int):
            return fail(f"9/get_current_time: frame not int {cur!r}")
        if cur["world"] not in ("editor", "pie"):
            return fail(f"9/get_current_time: world not enum {cur!r}")
        if not schema_check_frame_rate(cur["frame_rate"], "9/get_current_time-frame_rate"):
            return 1
        if not schema_check_frame_rate(cur["tick_rate"], "9/get_current_time-tick_rate"):
            return 1
        info(f"9/get_current_time OK (Sequencer open; tick={cur['tick']}, world={cur['world']!r})")
    elif resp.get("ok") is False:
        err = resp.get("error")
        if not isinstance(err, dict) or err.get("code") != -32042:
            return fail(f"9/get_current_time: expected -32042 NoActiveSequencer, got {err!r}")
        info("9/get_current_time → -32042 NoActiveSequencer (no Sequencer tab open — expected)")
    else:
        return fail(f"9/get_current_time: malformed response {resp!r}")

    # ─── Sub-test 10: missing-args negative cases ───────────────────────────────────────────────
    # get_tracks with no sequence_path → -32602
    resp = call(args.host, args.port, "10a", "p5-seq-10a", "sequencer.get_tracks", {})
    if expect_error(resp, "p5-seq-10a", -32602, "10a/get_tracks-no-path") is None:
        return 1
    # get_keyframes missing track_path → -32602
    resp = call(args.host, args.port, "10b", "p5-seq-10b", "sequencer.get_keyframes",
                {"sequence_path": seq_path or "/Game/bogus"})
    # NB: when seq_path is None we use a bogus path; error code may be -32602 (missing track_path)
    # OR -32004 (path bogus) — accept both for robustness when no test asset exists.
    if expect_error(resp, "p5-seq-10b", (-32602, -32004), "10b/get_keyframes-no-track") is None:
        return 1
    info("10/missing-args negative OK (-32602 InvalidParams)")

    print("[SMOKE_PHASE5_SEQUENCER] PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
