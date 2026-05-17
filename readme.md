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
| Benchmark harness | 5 scenarios x latency+PMC, version comparison plots |
| Performance rewrite (Phase 2+) | Planned |
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

## Benchmark

The benchmark suite measures per-operation latency and hardware performance counters
for specific order-book operations. Each scenario is a standalone executable that
implements the `IBenchScenario` interface via the **Strategy** pattern — the shared
measurement harness (`benchmark_runner.cpp`) has no knowledge of individual scenario logic.

### Architecture

```
┌──────────────────────────────────────────────────┐
│                benchmark_runner.cpp               │
│  ParseArgs() → warmup loop → measure loop → out  │
│  (zero knowledge of which scenario it runs)       │
└──────┬───────────────────────────────────────────┘
       │ virtual dispatch via IBenchScenario
       ▼
┌──────────────────┐ ┌──────────────┐ ┌───────────┐
│ bench_lmt_rest   │ │ bench_cxl_…  │ │ bench_…   │
│ IBenchScenario   │ │ …            │ │ …         │
└──────────────────┘ └──────────────┘ └───────────┘
       │                        │
       └──────────┬─────────────┘
                  ▼
┌──────────────────────────────────────────────────┐
│              bench_common.hpp (header-only)        │
│  PerfGroup • EnsureCsvHeader • Percentile          │
│  PrefillSellBook                                   │
└──────────────────────────────────────────────────┘
```

### Measurement modes

- **Latency** (`--metric latency`): records `avg / p50 / p95 / p99` nanoseconds per
  operation and `ops/s` throughput. Each iteration runs `batch_size` operations and
  divides the wall-clock duration by `batch_size` to amortise timer overhead.
- **PMC** (`--metric pmc`): reads hardware counters in-process via `perf_event_open`,
  enabled only during the measured batch (user-space only, `exclude_kernel=1`).
  Counters: `cycles`, `instructions`, `branches`, `branch-misses`, `cache-misses`.
  Derived metrics: `cpi`, `branch_miss_rate`.

### Scenario workloads

| Scenario | Prefill | Measured operation | What it stresses |
|---|---|---|---|
| `lmt_rest` | Empty book | Insert non-crossing buy limit | Resting limit insert path |
| `lmt_cross_deep` | Sell book, spread across `levels` | Aggressive buy limit crossing all levels | Price-time priority queue matching cost |
| `mkt_sweep_deep` | Sell book, spread across `levels` | Buy market order sweeping all levels | Sequential matching + queue removal |
| `cxl_miss` | Sell book, spread across `levels` | Cancel non-existent order ID | Worst-case cancel lookup (hash miss) |
| `dup_reject` | Sell book + one resting buy (ID=7) | Insert duplicate order ID=7 | Duplicate-detection lookup path |

`orders` controls total prefill size and `levels` controls price-level dispersion.

### Run pipeline

```bash
# 1. Build + run the full benchmark matrix (latency + PMC, all scenarios/trials)
bash benchmark/scripts/run_benchmarks.sh

# 2. Merge raw latency & PMC trials, compute mean/std/CV/95% CI per config
python3 benchmark/scripts/merge_benchmark_metrics.py

# 3. Baseline plots (metric vs. orders, one line per scenario)
python3 benchmark/scripts/plot_benchmark.py

# 4. Version-comparison plots (metric vs. orders, one line per version_tag;
#    bar charts at fixed config; %-change heatmaps)
python3 benchmark/scripts/plot_version_comparison.py
```

Override defaults if needed:

```bash
SCENARIOS=lmt_rest,cxl_miss METRICS=latency,pmc BATCH_SIZES=32,64 \
VERSION_TAG=phase2_step1 COMMIT_SHA=<sha> TRIALS=5 ITERS=1200 WARMUP_ITERS=200 \
  bash benchmark/scripts/run_benchmarks.sh
```

### Version-comparison plotting

When iterating on optimizations, assign each build a unique `VERSION_TAG` (e.g.
`v1.0`, `v2.0`, `v2.5`). After collecting multiple versions, run:

```bash
PLOT_METRICS=p99_ns,cpi,cache_misses_per_op \
PLOT_LEVEL=100 \
FIXED_ORDERS=10000 \
  python3 benchmark/scripts/plot_version_comparison.py
```

This generates three plot types:

1. **Line charts** — per (scenario x metric), one line per `version_tag` with CI error bands.
2. **Bar charts** — side-by-side bars at a fixed config (`FIXED_ORDERS`, `PLOT_LEVEL`),
   annotated with absolute values.
3. **Heatmaps** — percent change of each version relative to the first `version_tag`,
   color-coded (green = improvement, red = regression).

<!-- ### Remote execution

Run the full pipeline (clone, build, benchmark, merge, plot, download) on a remote
Linux server in one command:

```bash
SERVER_IP=1.2.3.4 \
REPO_URL=git@github.com:you/llmes.git \
  bash benchmark/scripts/run_remote_bench.sh
```

All benchmark campaign and plotting parameters can be overridden via env vars
(see the top of `run_remote_bench.sh`). -->

<!-- ### Infrastructure smoke test

Validates the benchmark harness itself (CSV header management, percentile
calculation, book prefill, end-to-end scenario dispatch):

```bash
cmake --build build --target benchmark_smoke_test
./build/benchmark/benchmark_smoke_test
``` -->

### Output artifacts

- Raw latency trials: `benchmark/results/{OUT_PREFIX}_latency_raw_trials.csv`
- Raw PMC trials: `benchmark/results/{OUT_PREFIX}_pmc_raw_trials.csv`
- Merged raw trials: `benchmark/results/{OUT_PREFIX}_merged_raw_trials.csv`
- Aggregated stats (mean/std/cv/95% CI): `benchmark/results/{OUT_PREFIX}_merged_agg.csv`
- Plots: `benchmark/results/plots/*.png`

### Notes

- PMC mode requires Linux perf support and permissions (`/proc/sys/kernel/perf_event_paranoid ≤ 1`).
- On some virtualized cloud CPUs (Hetzner), `LLC-load-misses` / `LLC-store-misses`
  are not exposed. The suite uses `cache-misses` instead; LLC metrics are deferred.
- CSV headers are written idempotently by `EnsureCsvHeader()` — the bash script
  only truncates the file before the campaign, and the C++ binary appends trial rows.
- The `version_tag` and `commit_sha` fields in every CSV row enable precise
  tracking of which build produced which numbers.

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
│       └── compile_flags.txt
├── benchmark/
│   ├── CMakeLists.txt
│   ├── src/
│   │   ├── benchmark_runner.hpp
│   │   ├── bench_common.hpp
│   │   ├── bench_lmt_rest.cpp
│   │   ├── bench_lmt_cross_deep.cpp
│   │   ├── bench_mkt_sweep_deep.cpp
│   │   ├── bench_cxl_miss.cpp
│   │   └── bench_dup_reject.cpp
│   ├── runner/
│   │   └── benchmark_runner.cpp
│   ├── tests/
│   │   └── benchmark_smoke_test.cpp
│   ├── scripts/
│   │   ├── run_benchmarks.sh
│   │   ├── run_remote_bench.sh
│   │   ├── merge_benchmark_metrics.py
│   │   ├── plot_benchmark.py
│   │   └── plot_version_comparison.py
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

1. Phase 1 (current): functional core + tests + benchmark harness
2. Phase 2: intrusive queue, O(1) cancel index, skip list
3. Phase 3: SoA, cache alignment, pmr experiments
4. Phase 4: advanced profiling (LLC load/store, front-end stalls, roofline)
5. Market data, execution, risk, tslib, lfutils

---


## Disclaimer

README distinguishes implemented vs planned features as the project grows.
