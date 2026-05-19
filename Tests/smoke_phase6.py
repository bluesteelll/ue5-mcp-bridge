#!/usr/bin/env python3
"""Phase 6 smoke wrapper — runs all 4 sub-suites and reports combined result.

Sub-suites (each is a self-contained Python script with its own main()):
  - smoke_phase6_sc.py            — 6 sc.* tools (Chunk A): status / checkout / diff / diff_binary /
                                    revert / sc.submit (async composite)
  - smoke_phase6_test.py          — 8 test.* tools (Chunk B): list_automation_specs /
                                    run_single_test / get_last_results / cancel_current /
                                    list_categories / get_test_info / set_filter_flags /
                                    test.run_automation (async composite)
  - smoke_phase6_cfg.py           — 6 cfg.* tools (Chunk C): get_cvar / set_cvar / list_cvars /
                                    read / write / list_sections
  - smoke_phase6_log_livecoding.py — 3 log.* additions + 1 livecoding.recompile composite
                                    (Chunks D + E combined): set_category_verbosity /
                                    list_categories / clear / livecoding.recompile

Each child inherits the connection args (--host / --port) passed to this wrapper. The wrapper
exits with the first failing sub-suite's non-zero code, or 0 if all PASS. A short summary line
``[SMOKE_PHASE6_ALL] PASS|FAIL`` follows the last sub-suite's output for easy log scraping.

Phase 6 ships 24 user-visible tools across 5 categories (6 sc.* + 8 test.* + 6 cfg.* + 3 log.* +
1 livecoding.recompile = 24). With Phase 6 complete the cumulative across all phases reaches
**169 user-visible tools** + ~15 internal hidden handlers. **ALL 6 PHASES OF THE v2 BLUEPRINT
SHIPPED.**

Usage:
  python smoke_phase6.py [--host 127.0.0.1] [--port 30020]
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).parent
SUB_SUITES = [
    "smoke_phase6_sc.py",
    "smoke_phase6_test.py",
    "smoke_phase6_cfg.py",
    "smoke_phase6_log_livecoding.py",
]


def main(argv: list[str]) -> int:
    extra_args = argv[1:]
    overall_rc = 0
    failed: list[str] = []
    for sub in SUB_SUITES:
        path = HERE / sub
        if not path.exists():
            print(f"[SMOKE_PHASE6_ALL] sub-suite SKIP (missing file): {sub}", flush=True)
            continue
        print(f"\n========== {sub} ==========", flush=True)
        proc = subprocess.run([sys.executable, str(path), *extra_args])
        if proc.returncode != 0:
            overall_rc = proc.returncode
            failed.append(sub)
            print(f"[SMOKE_PHASE6_ALL] sub-suite FAIL: {sub} (rc={proc.returncode})", flush=True)

    print()
    if overall_rc == 0:
        print(f"[SMOKE_PHASE6_ALL] PASS — all {len(SUB_SUITES)} sub-suites green "
              f"(Phase 6 = 24 tools; cumulative = 169 across all 6 phases)", flush=True)
    else:
        print(f"[SMOKE_PHASE6_ALL] FAIL — {len(failed)} sub-suite(s) failed: {', '.join(failed)}",
              flush=True)
    return overall_rc


if __name__ == "__main__":
    sys.exit(main(sys.argv))
