# Project Experiment Log

This file records the project history that is currently documented in this
repository: design attempts, benchmark campaigns, results, and conclusions.
It intentionally does not cover undocumented discussions or experiments.

## Source Records

The notes below are based on:

- `report/phase1_vs_phase2_report.md`
- `report/phase2b_to_phase_2e_comparison.md`
- `report/phase3_hft_benchmark_design.md`
- `report/phase4_price_level_storage_strategy.md`
- `benchmark/results/campaign_20260601_1319/`
- `server_results/macro_op_profile_cloud_t1/`

## Phase 1: Correctness-First Baseline

### Design

Phase 1 established the first correct order book implementation.

The core storage model was:

- price levels stored in an ordered map
- each price level stored as `std::list<Order>`
- cancel and modify implemented by scanning the book

This design was simple and useful as a correctness reference, but it had two
major performance costs:

- every resting order lived in a separately allocated list node
- cancel and modify required O(N) traversal

### Testing

The early benchmark suite measured seven legacy scenarios plus an overall mixed
workload. The mixed workload used:

- 35% cancel
- 30% modify
- 25% limit rest
- 5% limit cross
- 5% market

### Result

Phase 1 was retained as a correctness baseline, not as a performance target.
The benchmark report shows that traversal-heavy paths suffered from high cache
misses and poor scaling as the book grew.

## Phase 2a: Pool Storage + Intrusive Per-Level List

### Design

Phase 2a replaced `std::list<Order>` with a pool-backed intrusive list.

The main goal was to remove per-order heap allocation and improve traversal
locality while preserving price-time priority.

The storage model became:

- preallocated `std::vector<Order>` order pool
- `IntrusiveList` per price level
- still O(N) cancel lookup by scanning

### Benchmark Result

At `orders=100000`, `levels=1000`, Phase 2a improved nearly every legacy
scenario against Phase 1:

| Scenario | Main result |
|---|---:|
| `lmt_rest` | throughput +19% |
| `lmt_cross_shallow` | throughput +37% |
| `lmt_cross_deep` | throughput +43% |
| `mkt_sweep_deep` | throughput +45% |
| `cxl_hit` | throughput +98%, cache misses -87% |
| `cxl_miss` | throughput +92%, cache misses -91% |

### Conclusion

The pool-backed intrusive list was a clear improvement over `std::list`.

The biggest wins came from:

- eliminating allocator work on normal order insertion
- making traversal much more cache friendly

However, cancel and modify were still fundamentally O(N), so the architecture
could not handle realistic cancel-heavy workloads.

## Phase 2b: O(1) Cancel Index

### Design

Phase 2b added an ID-to-order index:

```cpp
std::unordered_map<OrderId, Order*> id_to_order_;
```

Each `Order` also carried enough metadata to be removed from its intrusive
level in O(1). This changed cancel from a full book scan into:

1. hash lookup by order ID
2. intrusive unlink from the owning level
3. hash erase

Modify benefited because it is implemented as cancel plus add.

### Benchmark Result

At `orders=100000`, `levels=1000`:

| Scenario | Main result |
|---|---:|
| `cxl_hit` | throughput 17K/s -> 5.8M/s |
| `cxl_miss` | throughput 8.5K/s -> 15.3M/s |
| `lmt_rest` | regressed about 38% |
| cross/market paths | regressed about 40-80% |

The overall mixed workload changed from:

| Phase | ops/s |
|---|---:|
| Phase 1 | 3,691 |
| Phase 2a | 5,033 |
| Phase 2b | 4,499,000 |

### Conclusion

The O(1) cancel index was transformative for realistic mixed workloads.

The cost was real: every add and every matched maker now pays hash-table
insert/erase overhead. But the workload was cancel/modify dominated, so the
O(N) -> O(1) cancel improvement dominated the regression in pure matching
paths.

## Phase 2c-2e: Cancel-Index Hash Table Engineering

### Design Attempts

After Phase 2b, the next question was whether `std::unordered_map` could be
replaced by a faster hash table for `id_to_order_`.

The documented attempts were:

| Phase | Cancel index |
|---|---|
| 2b | `std::unordered_map` |
| 2c | custom open addressing with tombstones |
| 2d | Robin Hood hashing with backward-shift deletion |
| 2e | `absl::flat_hash_map` |

### HFT Macro Result

The Phase 3 HFT macro benchmark became the decisive workload. On Hetzner CCX23,
with 10 trials, `orders=100000`, `levels=100`:

| Phase | Macro ops/s | ns/op | CPI | cache miss/op |
|---|---:|---:|---:|---:|
| 2b | 11.0M | 91 | 0.41 | 0.87 |
| 2c | 7.8M | 128 | 0.75 | 2.50 |
| 2d | 7.8M | 129 | 0.78 | 2.23 |
| 2e | 11.9M | 84 | 0.43 | 0.97 |

### Conclusion

The custom hash tables looked plausible in isolation but regressed badly under
the realistic cancel-heavy macro workload.

The main failure mode was memory behavior:

- tombstones and probe chains increased cache misses
- backward-shift deletion reduced tombstones but still paid higher CPI
- lower instruction count did not compensate for worse stalls

`absl::flat_hash_map` became the recommended cancel-index implementation.

## Phase 3: HFT Benchmark Redesign

### Motivation

The old benchmark mix was ad hoc and did not model HFT order flow well enough.

Phase 3 introduced a benchmark suite based on several workload properties:

- high cancel rate
- near-best price locality
- short order lifetime
- cancel clustering
- non-flat depth profile

### Design

The new benchmark suite split HFT behavior into micro scenarios and a macro
scenario.

Micro scenarios include:

- `hft_add_near`
- `hft_add_far`
- `hft_cancel_hot`
- `hft_cancel_cold`
- `hft_modify_near`
- `hft_cxl_miss`
- `hft_market_small`
- `hft_market_large`

The macro benchmark uses a zero-intelligence style event stream:

- 45% limit add
- 48% cancel
- 5% modify
- 2% market

Timed operations are pre-generated so that measurement focuses on order-book
operations instead of random parameter generation.

### Conclusion

The HFT macro benchmark became the primary decision metric.

It changed the interpretation of earlier hash-table experiments: designs that
looked competitive under old or isolated benchmarks could regress sharply under
realistic mixed HFT access patterns.

## Phase 4: Price-Level Storage Strategy

### Starting Point

The Phase 4 baseline is:

```cpp
std::map<price, PriceLevel>
```

wrapped behind a `SideBook` abstraction.

The important current components are:

- pool-backed intrusive order storage
- `absl::flat_hash_map<id, Order*>` cancel/modify index
- ordered price-level storage
- HFT micro and macro benchmark suite

The current `phase4a` baseline commit is:

```text
ce7e7c20bc6cb010457b7c297c3de990c08343ad
```

### Price-Range Finding

The Phase 4 report records a key correctness constraint:

- HFT micro benchmarks use predictable price ranges around `1000`
- `hft_macro` does not have a fixed price range
- best price can drift
- modify can move orders by several ticks

Therefore, a pure fixed-size `std::vector<PriceLevel>` is not a correct final
design unless the workload is clamped. The report explicitly treats clamping as
a workload change, not a pure engine optimization.

### Strategy

The documented Phase 4 strategy is incremental:

1. restore and validate the `std::map` baseline
2. wrap price-level storage behind `SideBook`
3. benchmark after each structural change
4. only add a hot contiguous structure if data justifies the complexity
5. keep a cold ordered path for arbitrary prices

### `absl::btree_map`

The Phase 4 strategy considered `absl::btree_map` as a possible ordered
container replacement. The current direction is not to use it as the next
implementation target because price-level pointer stability is a correctness
constraint in the current design.

The safer baseline remains `std::map` until the ownership and cancel metadata
model is changed enough that moving price-level values cannot invalidate live
order metadata.

## ChunkPool Experiment

### Design Hypothesis

A later experiment tested whether per-price-level chunk storage could improve
cache locality. The motivating hypothesis was:

- a single large order pool scatters orders from the same hot price level
- grouping orders into chunks owned by price levels may reduce cache misses
- different `kChunkSize` values may expose a better locality/overhead tradeoff

### Recorded Benchmark Result

The current repository keeps a 10-trial campaign under:

```text
benchmark/results/campaign_20260601_1319/
```

The overall summary compares `phase4a` against chunk sizes 16, 32, 64, 128,
and 256.

For the headline HFT macro scenario:

| Version | `hft_macro` ops/s |
|---|---:|
| `phase4a` | 15,958,480 |
| `master_chunk16` | 15,809,470 |
| `master_chunk32` | 15,563,010 |
| `master_chunk64` | 16,075,480 |
| `master_chunk128` | 15,425,790 |
| `master_chunk256` | 15,677,070 |

Several micro scenarios regressed relative to `phase4a`, for example:

| Scenario | Best chunk result | `phase4a` |
|---|---:|---:|
| `hft_add_near` | 21,438,083 | 23,863,634 |
| `hft_cancel_hot` | 10,706,453 | 13,167,589 |
| `hft_modify_near` | 6,430,392 | 7,622,224 |
| `hft_market_small` | 65,819,824 | 75,896,495 |

### Conclusion

The recorded results do not support adopting ChunkPool as the next baseline.

For macro HFT, the difference was close to noise. For several micro paths, the
chunk design regressed. This suggests that the extra chunk bookkeeping and
pointer operations were not compensated by better locality under the benchmarked
workload.

The important process lesson is that the cache-miss hypothesis was not strong
enough on its own. Future design work should start from profiler evidence on
the actual macro workload.

## Current Track: HFT Macro Operation Profiling

### Design

The current master branch adds per-operation profiling to `hft_macro`, guarded
by:

```cpp
LLMES_PROFILE_HFT_MACRO_OPS
```

The profiler separates mixed macro latency into operation classes:

- `add_rest`
- `add_cross`
- `cancel_hit`
- `cancel_miss`
- `modify_hit`
- `modify_miss`
- `market`

It records count/share, mean latency, p50/p95/p99 latency, mean cycles, and
weighted contribution to total macro time.

The helper script is:

```text
benchmark/scripts/run_hft_macro_op_profile.sh
```

### Cloud Smoke Result

A one-trial cloud run is stored under:

```text
server_results/macro_op_profile_cloud_t1/
```

Run configuration:

| Field | Value |
|---|---:|
| commit | `a138ad1` |
| orders | 100000 |
| levels | 100 |
| batch size | 100000 |
| trials | 1 |
| seed | 42 |

Overall:

| Metric | Value |
|---|---:|
| avg ns/op | 134.538 |
| ops/s | 7.43283M |
| ok | 112277 |

Operation table:

| Operation | Share | mean ns | p99 ns | weighted time share |
|---|---:|---:|---:|---:|
| `add_rest` | 48.30% | 75.58 | 230.00 | 39.92% |
| `cancel_miss` | 43.69% | 79.81 | 170.00 | 38.14% |
| `market` | 1.85% | 725.39 | 2654.90 | 14.69% |
| `modify_miss` | 3.92% | 117.23 | 270.00 | 5.02% |
| `cancel_hit` | 1.32% | 82.76 | 370.00 | 1.19% |
| `add_cross` | 0.83% | 94.96 | 380.00 | 0.86% |
| `modify_hit` | 0.09% | 172.17 | 573.60 | 0.17% |

Market intensity:

| Metric | Value |
|---|---:|
| market levels mean | 5.45 |
| market levels p95 | 11.00 |
| market levels p99 | 15.00 |
| market filled quantity mean | 330.02 |
| market filled quantity p95 | 971.00 |
| market filled quantity p99 | 1367.00 |

### Current Interpretation

The profiling result shows that `add_rest` and `cancel_miss` dominate weighted
macro time in the current measured batch.

The unexpectedly low `cancel_hit` share is itself an important finding. It
suggests the benchmark's pre-generated operation stream or its live-order
tracking may be drifting away from the actual book state. Before optimizing the
engine based on this table, the benchmark accounting should be reviewed and
made trustworthy.

### Follow-up Fix (Local Working Tree)

A follow-up benchmark fix was implemented in
`benchmark/src/hft/bench_hft_macro.cpp` using the same operation-mix policy
(random draw with 45/48/5/2 percentages), but replacing predictive batch
generation with a planning replay model:

- warmup still runs once
- two books are rebuilt from the same warmup snapshot:
  - `planning_book_` (untimed, used only in `Setup()`)
  - `book_` (timed, used only in `RunOp()`)
- each pending op is first executed on `planning_book_`
- tracking is updated from real execution outputs (`trades`,
  `remaining_quantity`) instead of prediction

This removes the major source of false `cancel_miss`:

- market-order maker fills are now reflected in tracking
- modify outcomes (including crossing/fully-filled cases) are reflected
- cluster cancels are validated against the planning book before enqueue

Local smoke validation (`TRIALS=1`, `BATCH_SIZE=20000`) shows:

| Operation | Share |
|---|---:|
| `add_rest` | 47.83% |
| `add_cross` | 0.44% |
| `cancel_hit` | 46.20% |
| `cancel_miss` | 0.00% |
| `modify_hit` | 3.79% |
| `modify_miss` | 0.00% |
| `market` | 1.75% |

This result supports the earlier diagnosis that high `cancel_miss` was mainly a
benchmark-accounting artifact rather than an engine behavior signal.

## Current Status

As of the current repository state:

- `master` is based on `phase4a`
- per-operation HFT macro profiling has been added
- ChunkPool benchmark artifacts are recorded but the design is not the active
  baseline
- macro workload accounting fix is implemented in the local working tree and
  smoke-tested; full cloud `TRIALS=10` verification is the next step
