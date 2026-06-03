# llmes — Low-Latency Matching & Execution Simulator

A C++20 order-matching engine evolved incrementally from a correctness-first baseline through data-structure optimization and HFT workload profiling.

---

## Current Status

| Area | Status |
|------|--------|
| Matching core | Done |
| Data structure | `std::map<price, PriceLevel>` + `absl::flat_hash_map<id, Order*>` (phase2e) |
| Order types | Limit / Market / Cancel / Modify |
| Benchmark suite | 6 legacy + 8 HFT scenarios |
| Hash table engineering | phase2b (`std::unordered_map`) → 2c (open-addressing) → 2d (Robin Hood) → **2e (`absl::flat_hash_map`)** |
| HFT macro benchmark | Zero-Intelligence model with realistic order flow |
| Market Data / Execution / Risk | Not started |

---

## Hash Table Engineering Journey

The cancel path is the dominant operation in any realistic order-book workload. The engine evolved through five phases of hash-table optimization:

| Phase | ID Index | Macro ops/s | vs 2b | Key Limitation |
|---|---|---|---|---|
| **2b** | `std::unordered_map` | 11.0M | baseline | Node-based: pointer chase, cache- unfriendly |
| 2c | Custom open-addressing + tombstones | 7.8M | −29% | Tombstone buildup degrades lookup under cancel-heavy load |
| 2d | Robin Hood + backward-shift deletion | 7.8M | −29% | Probe chains regress under HFT spatial concentration |
| **2e** | `absl::flat_hash_map` (Swiss Table) | **11.9M** | **+8%** | — |

**Winner: phase2e** — the Swiss-table `absl::flat_hash_map` outperforms hand-rolled open-addressing by 52% and the baseline `std::unordered_map` by 8% under realistic HFT order flow. See `report/phase2b_to_phase_2e_comparison.md` for full analysis.

---

## HFT Benchmark Suite (Phase 3)

Seven micro benchmarks isolate individual data-structure paths under HFT-realistic access patterns. The macro benchmark (Zero-Intelligence model) measures sustained throughput under a continuous mixed stream:

| Scenario | What it stresses | HFT share |
|---|---|---|
| `hft_add_near` | Insert at best ±1 tick (hot path) | ~40% |
| `hft_add_far` | Cold-path insert at deep levels | ~3% |
| `hft_cancel_hot` | Erase from dense near-best level | ~45% |
| `hft_cancel_cold` | Erase from sparse deep level | ~3% |
| `hft_modify_near` | Erase + insert at hot price | ~5% |
| `hft_market_small` | Bulk erase, 1-2 levels | ~1.7% |
| `hft_market_large` | Bulk erase, 5+ levels | ~0.3% |
| `hft_macro` | ZI model (all of the above, mixed) | definitive metric |

### Key Design Features

- **Realistic depth profile**: `PrefillHftBook` distributes orders with exponential decay from the best price (20% at tick 0, 18% at tick 1, ...)
- **Spatial locality**: 90% of operations within ±5 ticks of the best price
- **Cancel clusters**: Power-law burst sizes with temporal autocorrelation
- **Normalized latency**: Market-order latency is divided by actual match count (`filled_quantity`) via `op_normalizer()` for fair comparison

Full design rationale: `report/phase3_hft_benchmark_design.md`

---

## Build & Test

**Requirements:** CMake >= 3.20, C++20 (GCC 13+ or Clang 16+ recommended), Linux (for PMC benchmarks)

```bash
cmake -S . -B build -DLLMES_BUILD_BENCHMARKS=ON
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

### Benchmark Pipeline

```bash
# 1. Full benchmark matrix (latency + PMC, all scenarios × trials)
bash benchmark/scripts/run_benchmarks.sh

# 2. Merge trials, compute mean/std/CV/95% CI per config
python3 benchmark/scripts/merge_benchmark_metrics.py

# 3. Parametric plots (metric vs. orders per scenario)
python3 benchmark/scripts/plot_benchmark.py

# 4. Version-comparison plots + bar charts + %-change heatmaps
python3 benchmark/scripts/plot_version_comparison.py
```

Override parameters via environment variables:

```bash
SCENARIOS=hft_add_near,hft_cancel_hot METRICS=latency,pmc \
VERSION_TAG=v2e COMMIT_SHA=<sha> TRIALS=5 \
  bash benchmark/scripts/run_benchmarks.sh
```

---

## Architecture

The benchmark suite uses the **Strategy** pattern: each scenario implements `IBenchScenario`, and the shared `benchmark_runner` handles measurement, CSV output, and command-line parsing.

```
                   benchmark_runner.cpp
                   (ParseArgs → warmup → measure → output)
                           │
                    IBenchScenario (virtual)
               ┌───────────┼───────────────┐
          bench_lmt_rest  bench_cxl_…  bench_hft_…
               │                          │
               └──────────┬───────────────┘
                          ▼
                   bench_common.hpp
              PrefillSellBook / PrefillHftBook
```

### Measurement Modes

- **Latency** (`--metric latency`): `avg / p50 / p95 / p99` nanoseconds per op and `ops/s`, normalized per batch.
- **PMC** (`--metric pmc`): In-process hardware counters via `perf_event_open` (cycles, instructions, branches, misses, cache misses). Derived: CPI, branch miss rate.

---

## Key Results

### Phase 1 → Phase 2a (Pool Allocator)

Pool-based `PriceLevel` replaces `std::list`: 22–38% fewer instructions per op. Cancel-heavy scenarios see 2× throughput, memory latency drops dramatically (CPI −59%, cache misses −87%).

### Phase 2a → Phase 2b (O(1) Cancel Index)

`std::unordered_map<id, Order*>` eliminates O(N) book scan: cancel throughput improves **300–1800×**. Cross/match scenarios regress 40–80% due to hash-map maintenance overhead, but in a realistic mixed workload the net effect is **894× overall throughput**.

### Phase 2b → 2e (Hash Table Engineering)

Under the HFT macro benchmark (48% cancel / 45% add), custom open-addressing (2c/2d) regresses 29% — tombstones and probe chains hurt cancel-heavy access. `absl::flat_hash_map` (2e) leads at 11.9M ops/s: 52% ahead of 2c/2d and 8% ahead of 2b.

Full reports:
- `report/phase1_vs_phase2_report.md` — Phase 1 → 2a → 2b comparison
- `report/phase2b_to_phase_2e_comparison.md` — Hash table engineering (2b–2e)
- `report/phase3_hft_benchmark_design.md` — HFT benchmark design

---

## Project Layout

```
llmes/
├── CMakeLists.txt
├── readme.md
├── core/matching_core/
│   ├── include/matching/
│   │   ├── order_book.hpp          # OrderBook class
│   │   ├── price_level.hpp         # Intrusive doubly-linked list per price
│   │   ├── order_pool.hpp          # Pre-allocated order pool
│   │   └── types.hpp               # Order, Trade, AddResult, ErrorCode
│   ├── src/
│   │   ├── order_book.cpp
│   │   └── order_pool.cpp
│   └── tests/
│       └── order_book_test.cpp
├── benchmark/
│   ├── CMakeLists.txt
│   ├── src/
│   │   ├── benchmark_runner.hpp    # IBenchScenario interface
│   │   ├── bench_common.hpp        # PrefillSellBook, PrefillHftBook, utilities
│   │   ├── legacy/                 # 6 legacy benchmarks
│   │   └── hft/                    # 8 HFT benchmarks (micro + macro)
│   ├── runner/benchmark_runner.cpp
│   ├── scripts/
│   │   ├── run_benchmarks.sh
│   │   ├── merge_benchmark_metrics.py
│   │   ├── plot_benchmark.py
│   │   └── plot_version_comparison.py
│   └── results/                    # generated CSV + plots
├── report/                         # design docs + analysis reports
└── server_results/                 # remote benchmark run artifacts
```

---

## Design Notes

- **Correctness first**: Phase 1 used `std::map` + `std::list` for a straightforward, verifiable implementation.
- **Phase 2a**: Pool allocator eliminates `malloc`/`free` from the hot path — instruction count drops 22–38%.
- **Phase 2b**: O(1) cancel index transforms the engine for cancel-dominated workloads. The trade-off (hash-map overhead on every match) is acceptable given 97% cancellation in real markets.
- **Phase 2e**: `absl::flat_hash_map`'s Swiss-table design handles HFT spatial locality better than both `std::unordered_map` and hand-rolled open-addressing.
- **Phase 3**: HFT benchmarks replace the original ad-hoc workload mix with empirically grounded order flow: exponential depth decay, spatial concentration at the best price, cancel clusters, and a Zero-Intelligence macro model.

---

## Time Complexity

| Function | Average | Notes |
|---|---|---|
| `cancel_order()` | O(1) | Hash index via `id_to_order_` + intrusive-list erase |
| `add_limit_order()` | O(K + log P) | Match K makers; insert into `std::map` level (log P) |
| `add_market_order()` | O(K) | Match only; no remainder |

N = total resting orders, P = price levels, K = makers matched.

---

## Roadmap

1. **Phase 1** ✓ — Functional core + tests + benchmark harness
2. **Phase 2** ✓ — Intrusive queue + O(1) cancel index + hash table engineering
3. **Phase 3** ✓ — HFT micro + macro benchmarks, realistic workload modeling
4. Phase 4 — SoA, cache alignment, pmr, advanced profiling (LLC, front-end stalls, roofline)
5. Market data, execution, risk, tslib, lfutils
