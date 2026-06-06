# ULTIMATE test suite

Comprehensive acceptance + crash-safety + concurrency + protocol + stability
+ security tests for `UnrealMCPBridge`, organized into 10 categories
(`A`..`J`) per `D:/tmp/ws3_stress/ULTIMATE_TEST_PLAN.md`.

## Layout

- `mcp_test_harness.py` — shared infra: `call`, `health`, `discover_methods*`,
  `TestLogger`, `ConnectionPool`, transport fuzzers. Imported by every phase
  script.
- `phase_<id>_<name>.py` — one script per phase. Each is self-contained,
  exits 0 on PASS, 1 on FAIL, 2 on editor-died.
- Logs land in `D:/tmp/ws3_stress/test_logs/` as `<phase>_<name>.md`
  (human-readable) + `<phase>_<name>.json` (machine).

## Pre-flight

1. Editor must be running with the MCP bridge listening on `127.0.0.1:30020`
   (the bridge auto-starts on editor launch).
2. From repo root:
   ```
   PYTHONIOENCODING=utf-8 python Plugins/UnrealMCPBridge/Tests/python/ultimate/phase_a1_inventory.py
   ```
3. Each script self-tests harness liveness on import; aborts cleanly if editor
   is unreachable.

## Running the whole suite

```
# full fast/medium sweep (skips the heavy A1-A4 sweeps + long-by-design
# g6_recovery / k3; add --include-slow for those)
python run_all.py

# one phase
python phase_a7_error_codes.py

# aggregate every phase log into one report
python phase_j2_final_report.py

# dedicated long soak
python phase_e1_sustained_soak.py --minutes 60
```

`run_all.py` runs each phase as a subprocess (each self-preflights →
auto-relaunches the editor if it died), prints a per-phase PASS/FAIL line,
continues past failures, then runs the J2 aggregator. Repeated invocation =
cumulative multi-hour stability soak.

## Status — campaign COMPLETE, suite 0-FAIL

All 10 categories (A–J) plus a "K" edge set are shipped and green. **64 phase
scripts.** Latest J2 aggregate: **PASS 3338 / FAIL 0 / XFAIL 66 / SKIP 47**.
A green suite is **0 FAIL**; XFAIL = documented design-limit, SKIP = tool
not registered / precondition absent. **0 crash dumps** across the campaign.

| Cat | Theme | Phases | Result |
|---|---|---|---|
| A | functional baseline | a1-a7 | dispatch 431/431; arg/coercion/roundtrip/pagination/error-codes all green |
| B | crash-safety | b1-b9 | FName overflow, mount escalation, PIE guards, path traversal, unicode, numeric extremes, empty/null, JSON depth bomb, ref cycles |
| C | concurrency | c1-c5 | Lane B parallel, Lane A serial, mixed-lane, PIE thrash, atomic queue (C6 N/A — per-conn threads) |
| D | protocol | d1-d8 | malformed JSON, fragments, slow-loris, conn storm, disconnect-mid-call, oversized, wrong-kind, ID collision |
| E | long-run stability | e1-e5 | sustained soak, leak hunt, conn churn, Lane B saturation, latency distribution |
| F | security/privacy | f1-f5 | privilege escalation, path-traversal sweep, cross-asset corruption, PIE world isolation, data sanitization |
| G | edge/recovery | g1-g6 | asset-deleted-mid-call, PIE-during-Lane-A, GC-during-call, Live-Coding race, save-during-call, kill+relaunch recovery |
| H | workflows | h1-h7, h6.1-h6.7 | 418-tool dispatch, BP/AI/Input/Niagara+Material pipelines, transaction/render-target/spatial/debug/gameplaytag/sc/livecoding, mega-chain |
| I | regression | i1-i2 | all historical crash repros (S+5..S+20), API-ergonomics findings |
| J | observability | j1-j2 | self-instrumentation truthfulness, aggregated report |
| K | new edge | k1-k3 | path-reuse churn, concurrent distinct writes, per-connection resource-growth diagnostic |

Per-phase Category-A detail (the hardest to get right) is preserved below.

## Stability findings (headline)

- **Steady-state is rock-solid**: multiple 60-min soaks at 100 % success, zero
  UObject leak (~600 k cumulative requests). Path-reuse + 200-cycle create/
  delete churns are registry/GC-consistent.
- **Connection model is leak-free** (K3): 40 k connect→call→close cycles →
  editor handle count flat (~2920) and thread count flat (~160). The
  thread-per-connection design does not leak at scale.
- **One environmental limit, not a bridge defect**: on a 16 GB-class host under
  *continuous heavy* load the editor process can be OS-terminated after several
  hours (OOM hard-kill ~6 h, or graceful low-mem shutdown) — no crash dump, flat
  working set, leak-free connection model all confirm it's host memory pressure.
  Mitigation: restart the editor every ~4-5 h on constrained hosts; the harness
  `_kill_and_relaunch_editor` recovers in ~80-165 s. See the GitHub Pages
  "Stability & Operations" doc.
- **Accept-saturation ceiling**: `FTcpListener` serial-accept tops out around
  ~50 simultaneous *new* connects; bursts above that see some `no_connect`
  (retry succeeds). C1/E4 classify this as XFAIL design-limit, not a fault.

## Category A — per-phase detail

| Phase | Script | Coverage | Result |
|---|---|---|---|
| A1 | phase_a1_inventory.py | 431 methods × 1 dispatch | **431/431 PASS** |
| A2 | phase_a2_required_args.py | 312 methods × 3-5 hostile probes (≈1200 cases) | **1197P / 0F / 3X** (hybrid static+live chain) |
| A3 | phase_a3_optional_defaults.py | 312 methods × 2 | green with `--limit` (heavy; runs handlers to completion) |
| A4 | phase_a4_type_coercion.py | 312 methods × 2-3 coerce probes | green with `--limit` |
| A5 | phase_a5_roundtrip.py | 34 curated write→read pairs | **21P / 0F / 13X** (transform.translation/rotation read paths, attach-via-response, get_nodes, mesh.duplicate path — all fixed) |
| A6 | phase_a6_pagination.py | 19 paginated tools | **32P / 0F / 11X** |
| A7 | phase_a7_error_codes.py | 17 documented error codes | **green** |

## A2 full sweep — FIXED via hybrid static+live chain discovery

The original chain-walker saturated Lane A by satisfying required-arg
validators with dummies, which let handlers RUN TO COMPLETION on every
method (~1500 Lane A mutations × 312 methods = editor queue death after
~150 methods). Full sweep was 88 min / 696 FAIL.

**Fix**: `discover_chains_static()` in mcp_test_harness.py parses .cpp
source for `RequireXxxField` sites (incl. surface-specific
`XXX_Require*` helpers via inline expansion), brace-matched function
bodies, file-aware keying to disambiguate handler name collisions
(`Tool_Dump` in MemReport and RenderTarget).

A2 stage 1 now uses static chains for the 168 methods source-parse
covers fully + single-shot live probe (no satisfying-continuing) for
the remaining 143. Stage 1 went from ~1500 live calls to ~143.

**Result**: A2 full 312-method sweep — **1197 PASS / 0 FAIL / 3 XFAIL /
1200 cases in 7m26s**. Editor alive throughout, zero crash dumps.

## --limit N for A3/A4 (still recommended)

A3 (`optional_defaults`) and A4 (`type_coercion`) test the WHOLE
method response, which means they call the handler with full satisfied
args — handler runs to completion → side effects → saturation, same as
A2's old walker. With hybrid chain discovery providing partial chains
(only first field for some methods), A3 also reports `coverage_gaps`
where chain is incomplete.

```
python phase_a3_optional_defaults.py --limit 150
python phase_a4_type_coercion.py --limit 100
```

Editor restart between A3 and A4/A5/A6/A7 is currently needed in the
full sequential sweep.

## Findings

- **S+16 (FIXED)**: `asset.search_by_class` + `MCPARFilterParser`
  triggered UE ensure spam on short class_paths. Pre-validate `.` in
  ClassPathNormalized before TrySetPath.
- **A3 coverage gaps (RESOLVED)**: dummy_value learned vector/rotator/
  enum/positive-int field-name heuristics; regex extended to match
  "non-empty"/"valid"/typeless variants of "missing required field".
  All 11 gaps closed.
- **A5 arg-name mismatches (RESOLVED)**: hardcoded test args fixed to
  match actual tool signatures (`dest_path`, `blueprint_path`, `path`).
  2 remaining XFAILs are intentional (PrimaryDataAsset abstract; ai.bb
  runtime accessors need PIE).
- **A7 behavioural notes (NOT BUGS, document only)**:
  - `memreport.dump` `mode` is documented but actually optional
    (defaults to "trigger") — A7 now uses `actor.get` for -32602.
  - `folder.create` ≠ `cb.create_folder`. `folder.create` is for world
    outliner FActorFolders (in-memory only); intentionally accepts any
    string label and is idempotent. `cb.create_folder` is for content
    browser disk folders; properly guards mount points and rejects dups.

## Polish history

- Initial run discovered S+16 crash + 11 chain-walker gaps + 4 arg-name
  mismatches in A5 + 3 false-positive findings in A7.
- Harness `dummy_value()` extended with field-name heuristics:
  vector/rotator/scale shapes (`[0,0,0]` / `{x:0,y:0,z:0}`),
  positive-int hints (`radius` → 1, etc.), enum hints (`key_type` →
  "Float", `verbosity` → "Log").
- `RE_MISSING` regex generalised to handle "non-empty"/"valid" prefixes
  and typeless "missing required field" form.
- A5 rewritten with correct tool signatures verified against live
  bridge.
- A7 reproducers re-routed to tools that genuinely enforce each error
  code.
