#!/usr/bin/env python3
"""Phase B5 — Unicode poison strings.

Goal: every tool that accepts a string MUST handle these without
crashing:
  - High Unicode (CJK, emoji, surrogate pairs)
  - RTL override / bidi marks (U+202E, U+2066)
  - Zero-width chars / joiner abuse (U+200B, U+200D)
  - Mixed normalization forms (NFC vs NFD)
  - Combining diacritics over-stacked ("Zalgo")
  - Control characters (U+0000..U+001F, U+007F..U+009F)
  - Reserved code points (U+FFFE, U+FFFF)
  - Modifier sequences (skin-tone modifiers without base)

UE's FName uses 8-bit ASCII default (TCHAR=WIDECHAR=UTF-16). Strings
passing through FName::Init must be valid UTF-16. Passing invalid
sequences should be REJECTED, not crash the editor.

Asset paths /Game/<unicode>/ — UE filesystem layer must reject paths
that contain control chars or reserved code points.

Probe template: each string-field-accepting tool gets each poison
pattern. Expected outcome:
  - Structured error code (-32602 / -32011 / etc.)
  - Or ok=true (handler tolerated normalization)
  - NEVER: editor crash, FName assert, file-system error

Exit codes: 0=PASS, 1=FAIL, 2=preflight fail.
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

PHASE = "b5"
NAME = "unicode_poison"


# Poison strings. All non-printable / risky chars use explicit escape
# sequences so the source file stays plain UTF-8 ASCII where possible.
POISON_STRINGS: List[Tuple[str, str]] = [
    # Emoji burst — 3 high BMP codepoints
    ("\U0001F600\U0001F4A9\U0001F525", "emoji_burst"),
    # CJK ideographs
    ("中文测试_CJK", "cjk_chars"),
    # RTL override U+202E
    ("‮_RTL_OVERRIDE_TEST", "rtl_override"),
    # Bidi isolates U+2066 U+2068
    ("⁦⁨_BIDI_ISOLATES", "bidi_isolates"),
    # Zero-width: U+200B U+200C U+200D
    ("​‌‍_ZWSP_ZWNJ_ZWJ", "zero_width_chars"),
    # Zalgo: base char + many combining marks
    ("Test́̂̃̄̅̆Zalgo", "zalgo_stack"),
    # Null byte prefix — must be escaped, not literal
    ("\x00_NULL_BYTE_PREFIX", "null_byte_prefix"),
    # Inline control chars
    ("Tab\x09Newline\x0AReturn\x0D", "control_chars_inline"),
    # Reserved code points U+FFFE U+FFFF
    ("￾￿_RESERVED", "reserved_codepoints"),
    # Over-combining: precomposed + 3 extra combining acutes
    ("é́́́", "over_combining"),
    # Complex emoji with ZWJ (family)
    ("\U0001F468‍\U0001F469‍\U0001F467", "complex_emoji_zwj"),
    # Mixed normalization: precomposed E-acute + e+combining acute
    ("É" + "é", "mixed_nfc_nfd"),
    # Variation selectors U+FE0F U+FE0E
    ("️︎_VARIATION_SELECTORS", "variation_selectors"),
]


# Per-tool probes: (method, args_template, target_field, label_prefix)
PROBES: List[Tuple[str, Dict[str, Any], str, str]] = [
    ("actor.spawn",
     {"class_path": "/Script/Engine.StaticMeshActor",
      "location": [0, 0, 0]},
     "actor_label", "actor.spawn label"),
    ("bp.add_variable",
     {"blueprint_path": "/Game/_phantom_bp/X",
      "pin_type": {"category": "Real", "subcategory": "float"}},
     "variable_name", "bp.add_variable name"),
    ("bp.add_function",
     {"blueprint_path": "/Game/_phantom_bp/X"},
     "function_name", "bp.add_function name"),
    ("bp.set_variable_metadata",
     {"blueprint_path": "/Game/_phantom_bp/X",
      "variable_name": "V", "value": "v"},
     "key", "bp.set_variable_metadata key"),
    ("ai.bb.add_key",
     {"bb_path": "/Game/_phantom_bb/X", "key_type": "Float"},
     "key_name", "ai.bb.add_key name"),
    ("gameplaytag.add_tag",
     {"comment": ""},
     "tag", "gameplaytag.add_tag tag"),
    ("cfg.set_cvar",
     {"value": "1"},
     "cvar", "cfg.set_cvar name"),
    ("log.set_category_verbosity",
     {"verbosity": "Log"},
     "category", "log.set_category_verbosity cat"),
    ("niagara.set_user_param",
     {"niagara_system_path": "/Script/Engine.Default__Actor",
      "value": 0.0},
     "name", "niagara.set_user_param name"),
    ("data_table.set_row",
     {"data_table_path": "/Game/_phantom_dt/X", "row_data": {}},
     "row_name", "data_table.set_row row_name"),
    ("curve.set_row_value",
     {"curve_path": "/Game/_phantom_curve/X",
      "time": 0.0, "value": 0.0},
     "row_name", "curve.set_row_value row_name"),
    ("folder.create",
     {},
     "folder_path", "folder.create folder_path"),
    ("collision.set_profile_response",
     {"channel": "WorldStatic", "response": "Block"},
     "profile_name", "collision.set_profile_response name"),
]


def main() -> int:
    if not preflight(PHASE):
        return 2
    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()
    fail_total = 0

    total = len(PROBES) * len(POISON_STRINGS)
    print(f"[B5] running {total} unicode-poison probes "
          f"({len(PROBES)} tools x {len(POISON_STRINGS)} poison patterns)...",
          flush=True)

    for (method, base_args, field, label_prefix) in PROBES:
        for (poison, pattern_name) in POISON_STRINGS:
            label = f"{label_prefix} :: {pattern_name}"
            args = dict(base_args)
            args[field] = poison

            t0 = time.monotonic()
            try:
                r = call(method, args, timeout=6.0)
            except Exception as e:
                r = {"_err": "exception", "_exc": str(e)}
            dt = (time.monotonic() - t0) * 1000.0
            c = err_code(r)
            alive = health(timeout=3.0)

            if not alive:
                log.case(label, "FAIL",
                         f"EDITOR DIED on {method} with {pattern_name}",
                         alive=False, duration_ms=dt)
                log.write()
                print(f"  [B5] EDITOR CRASHED on {method} :: {pattern_name}",
                      file=sys.stderr)
                return 1
            crash = latest_crash_dump(since=crash_baseline)
            if crash:
                log.case(label, "FAIL", f"CRASH DUMP: {crash}",
                         alive=alive, duration_ms=dt, code=c)
                log.write()
                return 1
            if is_transport_failure(r):
                log.case(label, "FAIL", f"transport: {r.get('_err')}",
                         alive=alive, duration_ms=dt)
                fail_total += 1
                continue

            # Both ok=true (handler tolerated) and structured error are
            # acceptable — what matters is NO CRASH.
            if is_ok(r):
                log.case(label, "PASS",
                         "handler accepted unicode (sanitized internally)",
                         alive=alive, duration_ms=dt)
            elif c is not None and -32700 <= c <= -32000:
                log.case(label, "PASS",
                         f"guard fired: {c}: {err_message(r)[:50]}",
                         alive=alive, duration_ms=dt, code=c)
            else:
                log.case(label, "FAIL",
                         f"unexpected response: code={c}: {err_message(r)[:60]}",
                         alive=alive, duration_ms=dt, code=c)
                fail_total += 1

    summary = log.write()
    cc = summary["counts"]
    print()
    print(f"[B5] PASS={cc['PASS']} FAIL={cc['FAIL']} XFAIL={cc.get('XFAIL', 0)} "
          f"TOTAL={cc['TOTAL']}")
    print(f"     log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    if fail_total > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
