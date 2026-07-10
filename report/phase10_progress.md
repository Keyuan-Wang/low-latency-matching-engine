# Phase 10 Final Report

## Result in One Page

Phase 10 tried to explain the high tail latency of `add_rest_new_level`.

The main result is negative but useful: neither the price-level array nor the order pool explains the common p99/p999 tail.

| Result | Number |
|---|---:|
| `add_rest_new_level` baseline p50 | 44 cycles |
| `add_rest_new_level` baseline p99 | 176 cycles |
| `add_rest_new_level` baseline p999 | 286 cycles |
| Warm ghost-bit path p99 | 66 cycles |
| L1-only bitmap set p99 | 198 cycles |
| L2 propagation p99 | 330 cycles |
| PriceLevel reuse-distance Spearman correlation | 0.107 |
| Order-slot reuse-distance Spearman correlation | 0.010 |
| Price-array shrink: avg latency change | +0.8% |
| Price-array shrink: instructions/op change | -1.2% |
| Price-array shrink: cache misses/op change | -4.5% |

Changes kept in the final code:

- `PriceLevel` is 16 bytes instead of 24 bytes.
- Multi-trial runs use different random streams.
- The price range is 4096 levels instead of 65536.
- `OccupancyTree` has two bitmap levels instead of three.
- The benchmark can attribute bitmap paths, PriceLevel reuse, and order-slot reuse.
- Remote analysis installs SciPy and pools all trials.

The prefetch experiment was not kept on the active order path. `prefetch_level()` still exists as an unused helper, but `OrderBook` does not call it.

The p99/p999 investigation ends here. Future optimization should use the full macro benchmark and focus on average `ns/op`, cycles/op, and instructions/op.

## Test Setup

| Field | Value |
|---|---|
| Server | Hetzner Cloud CCX23 |
| Benchmark CPU | CPU 2 |
| Kernel setup | `nohz_full=2-3` |
| Run prefix | `chrt -f 95 numactl --physcpubind=2 --membind=0` |
| Scenario trials | 50 |
| Seed | 42, mixed with `trial_id` |
| Orders | 100,000 |
| Batch size | 100,000 |
| Scenario runner | `benchmark/scripts/run_remote_hft_macro_scenarios_tuned.sh` |

The per-scenario benchmark measures every operation with timestamp reads. It is useful for comparing scenario distributions, but it is more intrusive than the full macro benchmark. Final engine decisions therefore use the full macro comparison where available.

## Changes Made

| Commit | Change | Final status |
|---|---|---|
| `d894232` | Prefetch target PriceLevel | Experiment only |
| `ffbf3dc` | Prefetch the tail byte of PriceLevel | Rejected |
| `443145c` | Mix `trial_id` into both RNG streams | Kept |
| `7456828` | Remove unused `PriceLevel::size_` | Kept |
| `c96fbbc` | Restore direct PriceLevel prefetch | Later removed from call sites |
| `28eed17` | Add bitmap and PriceLevel attribution | Kept |
| `bac4429` | Reduce price array to 4096 and tree to two levels | Kept |
| `08d60f9` | Add order-slot reuse attribution | Kept |
| `077ae4d` | Add SciPy to remote requirements | Kept |

Phase 9 reference: `1d3383d`.

## 1. PriceLevel Layout and Prefetch

### PriceLevel size

`PriceLevel::size_` was unused and removed in `7456828`.

| Layout | Size |
|---|---:|
| Before | 24 bytes |
| After | 16 bytes |
| Objects per 64-byte cache line | 4 |

This is a clear layout improvement and remains in the code. The benchmark did not show a standalone p99 improvement from this change.

### Prefetch experiment

Per-scenario `add_rest_new_level` results:

| Commit | Relevant state | p50 | p99 | p999 |
|---|---|---:|---:|---:|
| `443145c` | Initial direct-prefetch baseline | 44 | 154 | 264 |
| `7456828` | No active prefetch call | 44 | 176 | 286 |
| `c96fbbc` | Direct prefetch restored | 44 | 154 | 286 |
| `28eed17` | Attribution added; prefetch calls removed | 44 | 176 | 286 |

The two runs with prefetch had a lower p99, but the experiment was not clean enough to prove causation. `28eed17` changed both benchmark attribution and the prefetch call sites. A later run also showed no useful gain and a worse `cancel_order` p999.

Final status: no manual PriceLevel prefetch on the order path.

## 2. Multi-Trial RNG Fix

Before `443145c`, every trial used nearly the same event stream. Pooling 50 trials mostly repeated one workload.

The seeds now include `trial_id`:

```cpp
event_seed = seed + trial_id * 1000003 + iter_idx * 9973;
param_seed = seed * 1337 + trial_id * 500009 + iter_idx * 331;
```

Trial 1 and trial 50 now contain different prices and quantities. All Phase 10 attribution results use the fixed generator.

## 3. OccupancyTree Path Attribution

Artifact:

```text
server_results/hft_macro/scenarios_tuned/
  hft_macro_scenarios_tuned_20260613_162525/
```

Pooled scenario results:

| Scenario | p50 | p99 | p999 |
|---|---:|---:|---:|
| `add_rest_existing_level` | 22 | 44 | 154 |
| `add_rest_new_level` | 44 | 176 | 286 |
| `cancel_order` | 22 | 44 | 44 |

`add_rest_new_level` split by the bitmap work performed by `set()`:

| Bitmap path | Share | p50 | p99 | p999 |
|---|---:|---:|---:|---:|
| `target_already_set` | 63.7% | 44 | 66 | 110 |
| `l1_only` | 33.1% | 44 | 198 | 308 |
| `reached_l2` | 2.0% | 88 | 330 | 550 |
| `reached_l3` | 1.2% | 44 | 264 | 738 |

Direct conclusions:

- Bitmap propagation is visible in the latency distribution.
- The common warm path has p99 = 66 cycles.
- `l1_only` is one third of new-level adds and has p99 = 198 cycles.
- L2/L3 propagation is expensive but only 3.2% of new-level adds.

`target_already_set` does not mean an existing live level. A cancelled non-best level can leave its occupancy bit set. Reusing that empty level is tagged as `add_rest_new_level`, while the bitmap path is `target_already_set`.

## 4. PriceLevel Reuse Distance

The benchmark records the number of operations since the same side and price were last touched.

| Reuse distance | Samples | p50 | p99 | p999 |
|---|---:|---:|---:|---:|
| 1-10 ops | 756,516 | 44 | 176 | 264 |
| 11-100 ops | 860,468 | 44 | 176 | 286 |
| 101-1,000 ops | 249,998 | 44 | 154 | 264 |
| 1,001-10,000 ops | 44,429 | 44 | 220 | 330 |
| More than 10,000 ops | 5,896 | 44 | 286 | 464 |
| First touch in batch | 6,312 | 44 | 704 | 1,937 |

| Correlation with cycles | Value |
|---|---:|
| Pearson, log2 reuse distance | 0.014 |
| Spearman | 0.107 |

The normal reuse bins do not show a strong monotonic relationship. First touch is expensive, but it is only about 0.3% of samples.

Result: PriceLevel reuse distance does not explain the common p99/p999 tail.

## 5. Price Array: 65536 to 4096

Commit `bac4429` changed:

| Item | Before | After |
|---|---:|---:|
| Price levels per side | 65,536 | 4,096 |
| PriceLevel storage per side | 1 MiB | 64 KiB |
| OccupancyTree levels | 3 | 2 |
| L3 propagation | Possible | Removed |

Per-path results before and after the change:

| Path | 65536 p99 | 4096 p99 | 65536 p999 | 4096 p999 |
|---|---:|---:|---:|---:|
| `target_already_set` | 66 | 44 | 110 | 88 |
| `l1_only` | 198 | 176 | 308 | 264 |
| `reached_l2` | 330 | 286 | 550 | 588 |
| `reached_l3` | 264 | Removed | 738 | Removed |

The individual paths became slightly cheaper, but the full macro benchmark did not improve.

50-trial macro comparison:

| Metric | 4096, `bac4429` | 65536, Phase 9 | Change |
|---|---:|---:|---:|
| Average ns/op | 18.099 | 17.953 | +0.8% |
| Cycles/op | 66.161 | 65.763 | +0.6% |
| Instructions/op | 128.494 | 130.049 | -1.2% |
| Cache misses/op | 0.02099 | 0.02198 | -4.5% |
| CPI | 0.515 | 0.506 | +1.8% |

The benchmark uses roughly 100 nearby price levels. At 16 bytes per level, the active PriceLevel data is about 1.6 KiB. Both array sizes keep that active region in cache.

Result: keep 4096 for the smaller allocation and simpler two-level tree, but do not count it as a performance win.

## 6. Order-Pool Slot Reuse

Artifact:

```text
server_results/hft_macro/scenarios_tuned/
  hft_macro_scenarios_tuned_20260613_193319/
```

The benchmark performs an untimed replay to determine which pool slot each measured add will acquire.

| Slot reuse distance | Samples | p99 | p999 | Mean |
|---|---:|---:|---:|---:|
| 1-10 ops | 250,114 | 176 | 286 | 41.2 |
| 11-100 ops | 124,561 | 154 | 286 | 41.0 |
| 101-1,000 ops | 9,436 | 110 | 188 | 36.1 |
| 1,001-10,000 ops | 659 | 88 | 185 | 35.2 |
| More than 10,000 ops | 50 | 99 | 109 | 38.7 |
| First touch | 108 | 1,418 | 1,469 | 165.4 |

| Correlation with cycles | Value |
|---|---:|
| Pearson, log2 reuse distance | -0.003 |
| Spearman | 0.010 |

About 97% of acquired slots were last acquired within 100 operations. The pool uses a LIFO free list, so recently released slots are normally reused immediately.

Result: the order pool's total allocation is large, but the active recycle window is small and hot. Slot reuse does not explain the common tail.

## 7. What the Data Supports

Supported by the measurements:

- A new bitmap bit costs more than reusing a set bit.
- Propagating into upper bitmap levels costs more again.
- Genuine first-touch samples have very high latency.
- PriceLevel and order-slot reuse distance have little correlation with normal latency.
- Shrinking the allocated price array reduces instructions and cache misses slightly, but does not improve end-to-end latency.
- Very large outliers also occur on the cheapest path. `target_already_set` recorded a maximum of 64,284 cycles.

Not supported by the measurements:

- The 1 MiB price-array allocation is the cause of p99.
- The 5.6 MiB order-pool allocation is the cause of p99.
- Manual prefetch is a reliable improvement.
- Removing the third bitmap level improves the full workload.

The important distinction is allocated size versus active working set. The benchmark touches a small price range and rapidly recycles a small set of order slots.

## Final Decision

Phase 10 closes the `add_rest_new_level` p99/p999 investigation.

The bitmap path changes latency, but the expensive upper-level paths are rare. The two main cache hypotheses were tested directly and did not explain the common tail. Further single-operation timestamp experiments are unlikely to produce a stable engine optimization.

The next optimization phase should use the full HFT macro benchmark as the primary gate:

1. Average `ns/op`
2. Cycles/op
3. Instructions/op
4. Branches and branch misses per operation
5. Cache misses per operation

Per-scenario data remains useful for locating regressions, but p99/p999 from intrusive single-operation measurement is no longer the main optimization target.

## Final Code State

| Area | State at the end of Phase 10 |
|---|---|
| `PriceLevel` | 16 bytes; unused `size_` removed |
| Price range | 4096 levels |
| `OccupancyTree` | Two levels, fixed `std::array`, unrolled operations |
| Manual PriceLevel prefetch | Not called by `OrderBook` |
| Trial RNG | Different stream for each `trial_id` |
| Bitmap attribution | Committed |
| PriceLevel reuse attribution | Committed |
| Order-slot reuse attribution | Committed |
| SciPy remote dependency | Added |
| Server tests | 8/8 passed in recorded artifacts |

## Artifacts

- Phase 9 report: `report/phase9_per_scenario_benchmark.md`
- Workload and attribution: `benchmark/src/hft/hft_macro_workload.hpp`
- Scenario collector: `benchmark/src/hft/bench_hft_macro_scenarios.cpp`
- Analysis script: `benchmark/scripts/analyze_hft_macro_attribution.py`
- 65536 / three-level attribution: `server_results/hft_macro/scenarios_tuned/hft_macro_scenarios_tuned_20260613_162525/`
- 4096 / two-level attribution: `server_results/hft_macro/scenarios_tuned/hft_macro_scenarios_tuned_20260613_185700/`
- Order-slot attribution: `server_results/hft_macro/scenarios_tuned/hft_macro_scenarios_tuned_20260613_193319/`
- 4096 vs Phase 9 macro comparison: `server_results/compare/compare_master_vs_phase9_trials50_20260613_200147/`
