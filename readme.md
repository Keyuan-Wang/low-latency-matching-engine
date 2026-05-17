# llmes — Low-Latency Matching & Execution Simulator

A C++20 simulator developed incrementally from correctness-first matching logic to later performance-focused redesigns.

**Current implementation scope: Phase 1 / Matching Core only.**

---

## Current Status

| Area | Status |
|------|--------|
| Matching Core (Phase 1) | Done |
| Data structure | `std::map<price, std::list<Order>>` (price level + FIFO queue) |
| Order types | Limit / Market / Cancel |
| Trade output | `Trade` + `AddResult` |
| Failure-mode handling | pending-cancel, market remainder cancel, duplicate ID |
| Unit tests | rest / match / market sweep / pending cancel |
| Performance rewrite (Phase 2+) | Not started |
| Market Data / Execution / Risk | Not started |
| tslib / lfutils | Not started |

---

## What Is Implemented (Phase 1)

Baseline central limit order book:

- **Ask book:** ascending price; `begin()` = best ask
- **Bid book:** descending price; `begin()` = best bid
- **FIFO:** `std::list` per price level
- **API:** `add_limit_order`, `add_market_order`, `cancel_order`
- **Results:** `Trade`, `AddResult`, `ErrorCode`

### Entry points

- `core/matching_core/include/matching/order_book.hpp`
- `core/matching_core/src/order_book.cpp`
- `core/matching_core/tests/order_book_test.cpp`

---

## Build & Test

**Requirements:** CMake >= 3.20, C++20 (GCC 13+ or Clang 16+ recommended)

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

If you see `No test configuration file found`, run `ctest` with `--test-dir build` (or `cd build` then `ctest`).

---

## Benchmark (Phase 1 Baseline)

The repository includes a reproducible benchmark pipeline for Phase 1.

### Benchmark modes

- **Latency mode** (`--mode latency`): records `avg/p50/p95/p99` and `ops/s`.
- **PMC mode** (`--mode pmc`): reads hardware counters in-process via `perf_event_open`,
  with counters enabled only around the measured operation (`cycles`, `instructions`,
  `branches`, `branch-misses`, `cache-misses`).

### Scenarios

- `lmt_rest`
- `lmt_cross_deep`
- `mkt_sweep_deep`
- `cxl_miss`
- `dup_reject`

### Measurement protocol

- Multi-trial runs: default `TRIALS=5` independent process runs per parameter tuple.
- Warmup before recording: default `WARMUP_ITERS=200`.
- Measured iterations per trial: default `ITERS=2000`.
- Parameter grid:
  - `orders`: `1000, 10000, 100000`
  - `levels`: `10, 100, 1000` (skip invalid combinations where `levels > orders`)

### Run commands

```bash
# latency raw trials
bash scripts/bench/run_baseline.sh

# pmc raw trials
bash scripts/bench/run_perf.sh

# merge latency + pmc and compute mean/std/cv/95% CI
python3 scripts/bench/merge_metrics.py

# generate plots from aggregated metrics
python3 scripts/bench/plot_baseline.py
```

Override defaults if needed:

```bash
TRIALS=5 ITERS=3000 WARMUP_ITERS=500 bash scripts/bench/run_baseline.sh
TRIALS=5 ITERS=3000 WARMUP_ITERS=500 bash scripts/bench/run_perf.sh
```

### Output artifacts

- Raw latency trials: `bench/results/phase1_baseline_raw_trials.csv`
- Raw PMC trials: `bench/results/phase1_pmc_raw_trials.csv`
- Merged raw trials: `bench/results/phase1_merged_raw_trials.csv`
- Aggregated stats (mean/std/cv/95% CI): `bench/results/phase1_merged_agg.csv`
- Plots: `bench/results/plots/*.png`

### Notes

- `run_perf.sh` requires Linux perf support and permissions.
- On some virtualized cloud CPUs (Hetzner), `LLC-load-misses` / `LLC-store-misses` are not exposed.
  This baseline currently uses `cache-misses` instead; LLC load/store metrics are deferred.
- Recommended for final reporting: run on a stable Linux server with fixed environment
  and rerun the same scripts for each phase/commit.

---

## Phase 1 Behavior

### Limit order

- Matches opposite side while price crosses.
- Remainder rests on the same side at limit price.
- FIFO within each price level.

### Market order

- Consumes opposite liquidity until filled or book empty.
- Unfilled remainder is not posted; returns `MarketRemainderCancelled`.

### Cancel

- Found on book -> removed, `Success`.
- Not found -> `UnknownOrderId`, id added to pending-cancel set.
- Later insert with same id -> `PendingCancelExists`.

### Time Complexity

#### Notation
- `N`: total number of resting orders in the book (bid + ask).
- `Pb` / `Pa`: number of price levels on bid / ask side.
- `P`: number of price levels on the side touched by this operation (use `max(Pb, Pa)` for an upper bound).
- `K`: number of maker orders actually visited/matched during this request.
Assumption: `std::unordered_set` operations are average-case `O(1)` under normal hash distribution.
---

#### Time Complexity (Phase 1 Implementation)
| Function | Time Complexity (average) | Notes |
|---|---:|---|
| `OrderBook()` | `O(1)` | Empty container initialization |
| `pending_cancel_count()` | `O(1)` | `unordered_set::size()` |
| `cancel_order()` | `O(N)` | Linear scan across both books (`map` levels + `list` queues) |
| `add_limit_order()` | `O(K + log P)` | Match `K` makers; if remainder exists, insert into `map` level (`log P`) |
| `add_market_order()` | `O(K)` | Match only; no remainder posting |
| `can_cross_limit()` | `O(1)` | Single price comparison |
---


## Failure-Mode Coverage

| Scenario | Behavior |
|----------|----------|
| Cancel before insert | Pending cancel; later insert rejected |
| Duplicate order id | `DuplicateOrderId` |
| Market sweeps book | Partial fill; remainder cancelled |

---

## Project Layout

```text
llmes/
├── CMakeLists.txt
├── readme.md
├── readme_archive.md
├── requirements.txt
├── core/
│   └── matching_core/
│       ├── CMakeLists.txt
│       ├── include/
│       │   └── matching/
│       │       └── order_book.hpp
│       ├── src/
│       │   └── order_book.cpp
│       ├── tests/
│       │   └── order_book_test.cpp
│       ├── bench/
│       │   └── phase1_benchmark.cpp
│       └── compile_flags.txt
├── scripts/
│   └── bench/
│       ├── run_baseline.sh
│       ├── run_perf.sh
│       ├── merge_metrics.py
│       └── plot_baseline.py
├── bench/
│   └── results/             # generated by benchmark scripts
└── build/                  # generated by CMake
```

---

## Design Notes

- Intentional minimal Phase 1 baseline (correctness first).
- Cancel scans price levels (expected for this phase).
- Later: intrusive list, id index, skip list, SoA, pmr, benchmarks.

---

## Roadmap

1. Phase 1 (current): functional core + tests
2. Phase 2: intrusive queue, O(1) cancel index, skip list
3. Phase 3: SoA, cache alignment, pmr experiments
4. Phase 4: perf / benchmark (p50/p99, cache miss; LLC load/store where available)
5. Market data, execution, risk, tslib, lfutils

---


## Disclaimer

README distinguishes implemented vs planned features as the project grows.
