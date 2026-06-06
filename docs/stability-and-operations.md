---
title: Stability & Operations
layout: default
nav_order: 4
---

# Stability & Operations
{: .no_toc }

Operational characteristics of the bridge under sustained and concurrent load,
distilled from an exhaustive stress campaign (62-phase "ULTIMATE" suite across
10 categories: functional, crash-safety, concurrency, protocol, long-run
stability, security, edge/recovery, workflows, regression, observability).

1. TOC
{:toc}

## Summary

- **Steady-state is rock-solid.** Sustained mixed Lane A/B traffic for a full
  hour shows **zero UObject growth** and flat/shrinking working set. Multiple
  back-to-back 60-minute soaks completed at **100 % success** with **zero
  leak** (cumulative ~600 k requests).
- **The connection model is leak-free at scale.** The bridge spawns one
  handler thread per TCP connection; over **40 000** connect→call→close cycles
  the editor's OS-handle count stayed flat (~2 920, slope −0.17 / 1 k) and
  thread count stayed flat (~160, slope −0.07 / 1 k). No per-connection thread
  or handle leak.
- **No bridge memory leak.** Across 200+ asset create/delete cycles and 5 000
  path-reuse churns, the post-GC UObject delta is ~0. Asset path reuse
  (delete → recreate same path) is registry/GC-consistent with no spurious
  `PathInUse`.
- **The one operational limit is environmental**, not a bridge defect — see
  below.

## Long-uptime behavior (memory-constrained hosts)

On a **16 GB-class host** running the editor under *continuous heavy* MCP load
(tens of thousands of requests per hour), the **editor process** — not the
bridge — can be terminated by the OS after several hours of accumulated editor
+ transient-asset state. Two modes were observed:

| Mode | When | Signature |
|---|---|---|
| Hard OOM-kill | ~6 h uptime | process vanishes mid-traffic, **no crash dump**, log ends abruptly |
| Graceful low-memory shutdown | variable | clean `LogExit: Exiting` in the log, no crash dump |

Both are **environmental memory pressure**, confirmed by: no UE crash dump
(an OS-level termination, not a UE fatal error), a flat/shrinking editor
working set right up to the wall, and the leak-free results above. The bridge
itself does not leak.

**Guidance for long autonomous sessions on ≤ 16 GB hosts:**

- **Restart the editor periodically** (every ~4–5 h is comfortable) during
  unattended multi-hour runs. A clean relaunch takes ~80–165 s.
- Prefer **steady, paced** traffic over continuous maximal-rate bursts —
  paced load (a few requests/second) ran indefinitely clean; back-to-back
  maximal soaks accelerate the environmental wall.
- On hosts with more RAM (32 GB+) the wall moves out substantially or
  disappears for practical session lengths.

A test client should treat a dropped connection / `no_connect` as a signal to
re-probe and, if the editor is gone, relaunch — not as a bridge error.

## Concurrency & connection limits

- **`FTcpListener` serial-accept ceiling (~50 simultaneous connects).** The
  listener accepts connections serially and spawns a thread each. A burst of
  ~50+ *simultaneous* new connections will see some connects refused
  (`no_connect`); they succeed on retry. This is a throughput ceiling on
  *connection establishment*, not on dispatch — already-open connections and
  moderate concurrency are unaffected. Under very high uptime the burst success
  rate degrades somewhat (e.g. a 100-way simultaneous burst dropped from ~70 %
  to ~57 % accepted). **Mitigation for high-concurrency clients:** reuse a
  keep-alive connection, or cap simultaneous *new* connects to a few dozen and
  retry refusals.
- **No cross-talk.** Under heavy concurrent load (per-connection IDs, identical
  IDs, empty IDs) responses are never mis-routed between connections; per-
  connection routing is correct.
- **Lane A is a serial game-thread queue.** While a *long* Lane A operation
  runs (e.g. a full `memreport.dump`, which can occupy the game thread for
  minutes), the bridge is transiently unable to service **new** connections
  until it drains; it recovers fully the instant the op completes. Avoid
  pathologically long single Lane A calls if you need the bridge responsive
  to new clients meanwhile.

## Determinism under contention

- **PIE vs. Lane A is deterministic.** `pie.start` and concurrent editor-world
  mutators (e.g. `bp.compile`) serialise through the Lane A queue; once PIE is
  active, editor-world mutations are correctly rejected (`-32027 PIEActive`).
  No race, no half-state.
- **Live Coding contention is graceful.** A `livecoding` recompile (Lane B)
  concurrent with a Lane A `bp.compile` stalls the in-flight compile during the
  patch-apply window but does not crash or corrupt; the editor recovers. Note a
  real recompile briefly occupies the game thread — don't trigger it on a hot
  path. (A `modules` value that passes validation kicks off a real compile;
  malformed/missing `modules` rejects cleanly with `-32602` before compiling.)

## Security posture

- **Mount guards hold.** Every asset-creator surface (blueprint, data asset,
  niagara, umg, material/curve/data-table via `asset.create`, duplicates,
  input.*, ai.*) rejects writes into read-only mounts (`/Engine`, `/Engine/
  Script`, `/Memory`, engine `/Plugins`, and `..`-relative escapes) with a
  structured error — no write escapes the guard.
- **Path traversal is blocked.** `..`, `//`, `/./`, URL-encoded (`%2E`/`%2F`),
  backslash, control chars, and over-length paths are rejected by the path
  normaliser before any filesystem touch.
- **Deeply nested / malformed JSON is bounded.** A pre-parse depth cap rejects
  pathologically nested frames (`-32700`) before the recursive deserializer can
  overflow the stack.
- **No PII leak.** Tool responses do not leak credentials or environment
  variables; engine-path strings appear only in diagnostic surfaces
  (`engine.get_info`) by design.

## Running the test suite

The ULTIMATE suite lives in `Tests/python/ultimate/`. Each phase script
self-preflights (probes the bridge, auto-relaunches the editor if needed) and
writes a per-phase Markdown + JSON log.

```bash
# one phase
python Tests/python/ultimate/phase_a7_error_codes.py

# full regression sweep (skips the heavy sweep + long-by-design phases;
# add --include-slow for A1–A4 / g6 / k3)
python Tests/python/ultimate/run_all.py

# aggregate report across all phase logs
python Tests/python/ultimate/phase_j2_final_report.py
```

- `run_all.py` runs the fast/medium phases in sequence and prints a per-phase
  PASS/FAIL line, then aggregates with `phase_j2_final_report.py`.
- The four "sweep every tool" phases (A1–A4) and the long-by-design phases
  (`g6_recovery` = real editor relaunch; `k3_conn_resource_growth` = 40 k
  connection churn) are excluded from the default pass; run them explicitly or
  with `--include-slow`.
- `phase_e1_sustained_soak.py --minutes N` is the dedicated long-duration soak.

**Result vocabulary:** `PASS` / `FAIL` (a real defect) / `XFAIL` (a documented
limitation or design-limit, e.g. accept-saturation under extreme concurrency) /
`SKIP` (tool not registered / precondition absent). A green suite is **0 FAIL**;
XFAIL/SKIP entries are expected and documented.
