# Baseline: FW 2.2.1 (dev, before phases 1-2), 2026-10-24

Measured from `kontrx-remote` with `lat_test.ps1 -N 200`.

| Test                                  | p50     | p95     | p99     | max     | errors |
|---------------------------------------|---------|---------|---------|---------|--------|
| Modbus TCP FC03, idle                 | 17.8 ms | 28.6 ms | 30.9 ms | 58.7 ms | 0      |
| Modbus TCP FC03, during log pages     | 19.5 ms | 28.0 ms | 30.1 ms | 35.6 ms | 0      |
| HTTP `/api/status` (11.3 KB)          | 67.8 ms | 80.9 ms | 146 ms  | 146 ms  | 0      |
| HTTP `/api/logs?limit=50` page        | 736 ms  | 842 ms  | -       | 842 ms  | 0      |

`sys`: heap_free 6496 B of 45056, cpu_pct 12 (idle).
