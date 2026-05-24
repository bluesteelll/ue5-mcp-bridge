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

## Status (post-S+15, HEAD 80062cb)

Initial wave covers Category A (functional baseline). Other categories planned
per `ULTIMATE_TEST_PLAN.md`.

| Phase | Script | LOC est. | Coverage |
|---|---|---|---|
| A1 | phase_a1_inventory.py | 200 | 431 methods × 1 dispatch each |
| A2 | phase_a2_required_args.py | 400 | ~800 required-arg cases |
| A3 | phase_a3_optional_defaults.py | 350 | ~300 default-value cases |
| A4 | phase_a4_type_coercion.py | 300 | ~150 strict-type cases |
| A5 | phase_a5_roundtrip.py | 500 | ~50 write→read pairs |
| A6 | phase_a6_pagination.py | 250 | ~30 paginated tools |
| A7 | phase_a7_error_codes.py | 350 | 20 documented error codes |
