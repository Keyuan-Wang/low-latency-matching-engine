# Phase 11 Final Report: LTO and PGO

## Result in One Page

Phase 11 tested compiler-level optimization after the matching engine and order-book data structures had stabilized.

Four Release builds were compared:

1. Baseline: `-O3 -DNDEBUG -march=native`
2. Baseline + LTO
3. Baseline + PGO
4. Baseline + LTO + PGO

The result is clear: **LTO is the only configuration worth keeping.**

| Metric | Baseline | LTO | Change |
|---|---:|---:|---:|
| Average latency | 17.589 ns/op | **15.630 ns/op** | **-11.1%** |
| Throughput | 56.86 Mops/s | **63.99 Mops/s** | **+12.5%** |
| Cycles/op | 64.62 | **57.25** | **-11.4%** |
| Instructions/op | 127.67 | **94.81** | **-25.7%** |
| Branches/op | 25.06 | **17.07** | **-31.9%** |
| Binary text size | 95,810 B | **83,491 B** | **-12.9%** |

The latency improvement was present on **all 50 validation seeds**. The worst paired seed still improved by 5.2%.

PGO did not help:

| Build | Average latency | vs baseline | vs LTO |
|---|---:|---:|---:|
| Baseline | 17.589 ns/op | - | +12.5% |
| **LTO** | **15.630 ns/op** | **-11.1%** | - |
| LTO + PGO | 15.790 ns/op | -10.2% | +1.0% |
| PGO | 17.815 ns/op | +1.3% | +14.0% |

Final decisions:

- Use LTO for performance Release builds.
- Do not use PGO in the default build.
- Stop using intrusive per-scenario p99/p999 as the main optimization target.
- Freeze the current matching-engine and order-book core.
- Move future work to the surrounding system: SPSC queues, event output, threading, networking, and persistence.

## Scope

Phase 11 did not change the matching engine or order-book data structures. It added compiler experiment support and benchmark automation.

| Commit | Change |
|---|---|
| `9701fdc` | Add GCC LTO/PGO build matrix, training window, validation seeds, and cross-seed aggregation |
| `cbfb874` | Reorganize benchmark scripts and add LTO support to the scenario runner |

There is no `core/matching_core` difference between the Phase 10 endpoint and the Phase 11 benchmark commits.

## Test Environment

| Field | Value |
|---|---|
| Server | Hetzner Cloud CCX23 |
| CPU | AMD EPYC Milan virtual CPU |
| Compiler | GCC 15.2.0 |
| Benchmark CPU | CPU 2 |
| NUMA binding | Node 0 |
| Run prefix | `numactl --physcpubind=2 --membind=0` |
| Linux isolation | Enabled |
| `nohz_full` | CPUs 2-3 |
| Validation trials | 50 |
| Validation seeds | 50 distinct seeds |
| PGO training seeds | 10 distinct seeds |
| Orders | 100,000 |
| Levels | 100 |
| Validation batch | 100,000 operations |
| PGO training batch | 1,000,000 operations |

The four modes were run in rotating order for each validation seed. No mode always ran first or last.

PGO training excluded Setup and Teardown. The generated profile covered only the operation replay window.

Primary artifact:

```text
server_results/hft_macro/pgo_compare/pgo_compare_20260614_113205/
```

## Full Build Matrix

### Latency and throughput

| Mode | Average ns/op | 95% CI | Throughput |
|---|---:|---:|---:|
| Baseline | 17.589 | 17.531-17.647 | 56.86 Mops/s |
| **LTO** | **15.630** | **15.571-15.689** | **63.99 Mops/s** |
| LTO + PGO | 15.790 | 15.723-15.857 | 63.34 Mops/s |
| PGO | 17.815 | 17.771-17.859 | 56.14 Mops/s |

The baseline and LTO confidence intervals do not overlap.

### Hardware counters

| Mode | Cycles/op | Instructions/op | Branches/op | Branch misses/op | Cache misses/op | CPI |
|---|---:|---:|---:|---:|---:|---:|
| Baseline | 64.62 | 127.67 | 25.06 | 1.220 | 0.02043 | 0.506 |
| **LTO** | **57.25** | **94.81** | **17.07** | 1.228 | 0.02096 | 0.604 |
| LTO + PGO | 58.03 | 93.93 | 16.04 | 1.275 | 0.02180 | 0.618 |
| PGO | 65.58 | 122.70 | 21.79 | 1.265 | 0.02324 | 0.534 |

### Binary size

| Mode | Text size | vs baseline |
|---|---:|---:|
| Baseline | 95,810 B | - |
| LTO | 83,491 B | -12.9% |
| PGO | 61,383 B | -36.0% |
| LTO + PGO | 52,057 B | -45.7% |

## Why LTO Worked

LTO did not improve CPI. CPI increased from `0.506` to `0.604`.

The speedup came from doing substantially less work:

- Instructions/op fell by 25.7%.
- Branches/op fell by 31.9%.
- Cycles/op fell by 11.4%.
- Binary text size fell by 12.9%.

The compiler could optimize across translation-unit boundaries. In the LTO binary, standalone `cancel_order()` and `modify_order()` symbols disappeared because their code was folded into callers. It could also remove unused code and specialize paths for the final executable.

Branch misses did not improve. They increased slightly from `1.220` to `1.228` per operation. The branch-miss rate rose because the total number of branches fell sharply while the absolute miss count stayed almost flat.

This is an instruction-count win, not a cache or branch-prediction win.

### Paired-seed stability

LTO was compared against baseline using the same 50 validation seeds.

| Metric | Mean paired change | LTO wins |
|---|---:|---:|
| Average ns/op | -11.1% | 50/50 |
| Cycles/op | -11.4% | 50/50 |
| Instructions/op | -25.7% | 50/50 |
| Branches/op | -31.9% | 50/50 |

For average latency, the paired improvement ranged from 5.2% to 13.8%. The result is not being driven by a small number of favorable trials.

## Why PGO Was Rejected

PGO alone reduced instructions and branches, but made the complete workload slower:

| PGO vs baseline | Change |
|---|---:|
| Average ns/op | +1.3% |
| Cycles/op | +1.5% |
| Instructions/op | -3.9% |
| Branches/op | -13.1% |
| Branch misses/op | +3.7% |
| Cache misses/op | +13.9% |
| CPI | +5.6% |

LTO + PGO also lost to LTO alone:

| LTO + PGO vs LTO | Change |
|---|---:|
| Average ns/op | +1.0% |
| Cycles/op | +1.4% |
| Instructions/op | -0.9% |
| Branches/op | -6.0% |
| Branch misses/op | +3.8% |
| CPI | +2.3% |

LTO + PGO executed slightly fewer instructions than LTO, but the generated layout had worse execution efficiency. It was faster than LTO on only 9 of 50 latency trials and on 3 of 50 cycles trials.

No further PGO seed, batch-size, or training-distribution tuning is planned. The current result is sufficient to reject it as a default build option.

## LTO Perf Record

Artifact:

```text
server_results/hft_macro/perf_record/hft_macro_perf_record_20260614_115103/
```

| Field | Value |
|---|---:|
| Event | `cycles:u` |
| Sampling frequency | 2,000 Hz |
| Lost samples | 0 |
| Captured samples | 1,320 |
| Enabled-window operations | 41 million |
| Wall latency with perf attached | 16.38 ns/op |
| Approximate sampled cycles/op | 54.3 |

The profile still identifies broad work areas:

- limit-order handling remains the largest path;
- matching and ghost cleanup remain visible;
- order acquisition, level lookup, and FIFO append remain visible;
- `std::vector<Trade>` allocation and release remain visible.

Function percentages are less reliable after LTO. Cross-translation-unit inlining folded much of the engine into `main`, `RunScenario`, and `execute_pending`. `main` alone received 41.7% self attribution, and the named `perf annotate` outputs were empty.

For this reason, the LTO perf report should be used for broad hotspot discovery only. It should not be used to compare small function-level percentages with the non-LTO profile.

## Per-Scenario Results

LTO scenario artifact:

```text
server_results/hft_macro/scenarios_tuned/
  hft_macro_scenarios_tuned_20260614_120210/
```

The Phase 11 scenario commit contains benchmark changes but no matching-core changes relative to the earlier non-LTO reference.

### Cycle percentiles

| Scenario | Non-LTO p50 | LTO p50 | Non-LTO p99 | LTO p99 | Non-LTO p999 | LTO p999 |
|---|---:|---:|---:|---:|---:|---:|
| Existing level add | 22 | 22 | 44 | 44 | 66 | 66 |
| New level add | 44 | 44 | 176 | 176 | 286 | 286 |
| Cancel | 22 | 22 | 44 | 44 | 44 | 44 |

The pooled cycle percentiles are identical before and after LTO.

Mean cycles also changed very little:

| Scenario | Non-LTO mean | LTO mean | Change |
|---|---:|---:|---:|
| Existing level add | 31.50 | 31.51 | +0.02% |
| New level add | 41.04 | 41.11 | +0.17% |
| Cancel | 23.38 | 22.54 | -3.57% |

### Interpretation

The per-scenario result does not contradict the 11% macro improvement.

The macro benchmark benefits from cross-unit optimization of dispatch, calls, result handling, and surrounding glue. The per-scenario benchmark inserts timestamp collection around each selected operation, so the measured values fall into coarse 22-cycle steps. That instrumentation hides small changes inside individual calls.

The unchanged p99/p999 values do not prove that the complete p99 is caused by system scheduling:

- `add_rest_new_level` p99 covers tens of thousands of calls, too many to classify as rare scheduler interruptions;
- its bitmap paths still have different latency distributions;
- timestamp overhead and cycle quantization strongly shape the histogram.

System noise mainly explains the extreme maximum values in the tens of thousands of cycles. The ordinary p99/p999 contains operation-path differences, measurement overhead, and quantization as well.

Final decision: keep per-scenario measurement for diagnosis, but do not use its intrusive p99/p999 as the primary performance gate.

## New Performance Baseline

The Phase 11 LTO build becomes the performance reference for future system work:

| Metric | New baseline |
|---|---:|
| Average latency | 15.63 ns/op |
| Throughput | 63.99 Mops/s |
| Cycles/op | 57.25 |
| Instructions/op | 94.81 |
| Branches/op | 17.07 |
| Branch misses/op | 1.228 |
| Cache misses/op | 0.02096 |

These values are specific to the Hetzner CCX23 environment, GCC 15.2.0, and the current HFT macro workload.

## Final Decision

Phase 11 closes optimization work on the current matching-engine and order-book core.

The project has reached the point where further gains would require expensive assembly-level work or another architecture change. Neither is justified by the remaining engine profile.

The matching core is now considered stable:

- fixed-array side books;
- two-level occupancy tree;
- pooled intrusive orders;
- compile-time book-side specialization;
- LTO performance build;
- validated macro and diagnostic scenario benchmarks.

Future work moves outside the core:

1. SPSC queue implementation
2. Trade and order-event output pipeline
3. Matching-thread ownership model
4. Network input and output
5. Journal and recovery path
6. End-to-end system benchmarks

`AddResult::trades` still uses `std::vector`, and allocator activity remains visible in the LTO perf report. It will be addressed as part of the future event-output architecture rather than by adding another temporary buffer inside the current API.

## Artifacts

- Compiler matrix: `server_results/hft_macro/pgo_compare/pgo_compare_20260614_113205/`
- LTO perf record: `server_results/hft_macro/perf_record/hft_macro_perf_record_20260614_115103/`
- LTO per-scenario run: `server_results/hft_macro/scenarios_tuned/hft_macro_scenarios_tuned_20260614_120210/`
- Non-LTO per-scenario reference: `server_results/hft_macro/scenarios_tuned/hft_macro_scenarios_tuned_20260613_193319/`
- Phase 10 report: `report/phase10_progress.md`
