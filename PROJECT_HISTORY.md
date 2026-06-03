# Project Experiment Log

This file records the project history that is currently documented in this repository: design attempts, benchmark campaigns, results, and conclusions. It intentionally does not cover undocumented discussions or experiments.

## Source Records

The notes below are based on:

- `report/phase1_vs_phase2_report.md`
- `report/phase2b_to_phase_2e_comparison.md`
- `report/phase3_hft_benchmark_design.md`
- `report/phase4_price_level_storage_strategy.md`
- `report/phase4_hft_macro_optimization_priority.md`
- `report/phase5_macro_profiling_plan.md`
- `benchmark/results/campaign_20260601_1319/`
- `benchmark/results/hft_macro_perf_record_cloud_20260601/`
- `server_results/macro_op_profile_cloud_t1/`

## Phase 1: Correctness-First Baseline

### Design

Phase 1 established the first correct order book implementation.

The core storage model was:

- price levels stored in an ordered map
- each price level stored as `std::list<Order>`
- cancel and modify implemented by scanning the book

This design was simple and useful as a correctness reference, but it had two major performance costs:

- every resting order lived in a separately allocated list node
- cancel and modify required O(N) traversal

### Testing

The early benchmark suite measured seven legacy scenarios plus an overall mixed workload. The mixed workload used:

- 35% cancel
- 30% modify
- 25% limit rest
- 5% limit cross
- 5% market

### Result

Phase 1 was retained as a correctness baseline, not as a performance target. The benchmark report shows that traversal-heavy paths suffered from high cache misses and poor scaling as the book grew.

## Phase 2a: Pool Storage + Intrusive Per-Level List

### Design

Phase 2a replaced `std::list<Order>` with a pool-backed intrusive list.

The main goal was to remove per-order heap allocation and improve traversal locality while preserving price-time priority.

The storage model became:

- preallocated `std::vector<Order>` order pool
- `IntrusiveList` per price level
- still O(N) cancel lookup by scanning

### Benchmark Result

At `orders=100000`, `levels=1000`, Phase 2a improved nearly every legacy scenario against Phase 1:

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

However, cancel and modify were still fundamentally O(N), so the architecture could not handle realistic cancel-heavy workloads.

## Phase 2b: O(1) Cancel Index

### Design

Phase 2b added an ID-to-order index:

```cpp
std::unordered_map<OrderId, Order*> id_to_order_;
```

Each `Order` also carried enough metadata to be removed from its intrusive level in O(1). This changed cancel from a full book scan into:

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

The cost was real: every add and every matched maker now pays hash-table insert/erase overhead. But the workload was cancel/modify dominated, so the O(N) -> O(1) cancel improvement dominated the regression in pure matching paths.

## Phase 2c-2e: Cancel-Index Hash Table Engineering

### Design Attempts

After Phase 2b, the next question was whether `std::unordered_map` could be replaced by a faster hash table for `id_to_order_`.

The documented attempts were:

| Phase | Cancel index |
|---|---|
| 2b | `std::unordered_map` |
| 2c | custom open addressing with tombstones |
| 2d | Robin Hood hashing with backward-shift deletion |
| 2e | `absl::flat_hash_map` |

### HFT Macro Result

The Phase 3 HFT macro benchmark became the decisive workload. On Hetzner CCX23, with 10 trials, `orders=100000`, `levels=100`:

| Phase | Macro ops/s | ns/op | CPI | cache miss/op |
|---|---:|---:|---:|---:|
| 2b | 11.0M | 91 | 0.41 | 0.87 |
| 2c | 7.8M | 128 | 0.75 | 2.50 |
| 2d | 7.8M | 129 | 0.78 | 2.23 |
| 2e | 11.9M | 84 | 0.43 | 0.97 |

### Conclusion

The custom hash tables looked plausible in isolation but regressed badly under the realistic cancel-heavy macro workload.

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

The new benchmark suite split HFT behavior into micro scenarios and a macro scenario.

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

Timed operations are pre-generated so that measurement focuses on order-book operations instead of random parameter generation.

### Conclusion

The HFT macro benchmark became the primary decision metric.

It changed the interpretation of earlier hash-table experiments: designs that looked competitive under old or isolated benchmarks could regress sharply under realistic mixed HFT access patterns.

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

Therefore, a pure fixed-size `std::vector<PriceLevel>` is not a correct final design unless the workload is clamped. The report explicitly treats clamping as a workload change, not a pure engine optimization.

### Strategy

The documented Phase 4 strategy is incremental:

1. restore and validate the `std::map` baseline
2. wrap price-level storage behind `SideBook`
3. benchmark after each structural change
4. only add a hot contiguous structure if data justifies the complexity
5. keep a cold ordered path for arbitrary prices

### `absl::btree_map`

The Phase 4 strategy considered `absl::btree_map` as a possible ordered container replacement. The current direction is not to use it as the next implementation target because price-level pointer stability is a correctness constraint in the current design.

The safer baseline remains `std::map` until the ownership and cancel metadata model is changed enough that moving price-level values cannot invalidate live order metadata.

## ChunkPool Experiment

### Design Hypothesis

A later experiment tested whether per-price-level chunk storage could improve cache locality. The motivating hypothesis was:

- a single large order pool scatters orders from the same hot price level
- grouping orders into chunks owned by price levels may reduce cache misses
- different `kChunkSize` values may expose a better locality/overhead tradeoff

### Recorded Benchmark Result

The current repository keeps a 10-trial campaign under:

```text
benchmark/results/campaign_20260601_1319/
```

The overall summary compares `phase4a` against chunk sizes 16, 32, 64, 128, and 256.

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

For macro HFT, the difference was close to noise. For several micro paths, the chunk design regressed. This suggests that the extra chunk bookkeeping and pointer operations were not compensated by better locality under the benchmarked workload.

The important process lesson is that the cache-miss hypothesis was not strong enough on its own. Future design work should start from profiler evidence on the actual macro workload.

## Current Track: HFT Macro Operation Profiling

### Design

The current master branch adds per-operation profiling to `hft_macro`, guarded by:

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

It records count/share, mean latency, p50/p95/p99 latency, mean cycles, and weighted contribution to total macro time.

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

The profiling result shows that `add_rest` and `cancel_miss` dominate weighted macro time in the current measured batch.

The unexpectedly low `cancel_hit` share is itself an important finding. It suggests the benchmark's pre-generated operation stream or its live-order tracking may be drifting away from the actual book state. Before optimizing the engine based on this table, the benchmark accounting should be reviewed and made trustworthy.

### Follow-up Fix and Cloud Validation

A follow-up benchmark fix was implemented in `benchmark/src/hft/bench_hft_macro.cpp` using the same operation-mix policy (random draw with 45/48/5/2 percentages), but replacing predictive batch generation with a planning replay model:

- warmup still runs once
- two books are rebuilt from the same warmup snapshot:
  - `planning_book_` (untimed, used only in `Setup()`)
  - `book_` (timed, used only in `RunOp()`)
- each pending op is first executed on `planning_book_`
- tracking is updated from real execution outputs (`trades`, `remaining_quantity`) instead of prediction

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

This result supports the earlier diagnosis that high `cancel_miss` was mainly a benchmark-accounting artifact rather than an engine behavior signal.

A later cloud validation ran the repaired profiling on `phase4-finale`:

```text
branch:       phase4-finale
commit:       efc1b67
trials:       1
orders:       100000
levels:       100
batch_size:   100000
warmup_iters: 1
iters:        1
```

Artifacts:

```text
server_results/macro_op_profile_cloud_phase4_finale_t1/
report/phase4_hft_macro_optimization_priority.md
```

Overall:

| Metric | Value |
|---|---:|
| avg ns/op | 110.793 |
| ops/s | 9.026M |
| ok | 199977 |

Operation profile:

| Operation | Share | mean ns | p99 ns | weighted time share |
|---|---:|---:|---:|---:|
| `add_rest` | 47.867% | 69.848 | 160.0 | 52.977% |
| `add_cross` | 0.469% | 76.311 | 190.0 | 0.567% |
| `cancel_hit` | 46.153% | 48.459 | 70.0 | 35.438% |
| `cancel_miss` | 0.000% | 0.000 | 0.0 | 0.000% |
| `modify_hit` | 3.919% | 131.357 | 310.0 | 8.157% |
| `modify_miss` | 0.000% | 0.000 | 0.0 | 0.000% |
| `market` | 1.592% | 113.417 | 431.8 | 2.861% |

The main findings from this repaired profile are:

- `cancel_miss` and `modify_miss` are both zero, confirming that the earlier miss-heavy profile was a benchmark-accounting artifact.
- `add_rest` is the dominant measured cost center: it contributes 52.977% of total weighted macro time.
- `cancel_hit` remains important at 35.438% weighted time, but its latency is already low and tight-tailed (`mean=48.459ns`, `p99=70ns`).
- `modify_hit` is individually expensive (`mean=131.357ns`) but low-frequency, contributing 8.157% weighted time.
- `market` is not the current macro bottleneck: it contributes only 2.861% weighted time, with `market_levels_p99=1` and `market_filled_qty_p99=5`.
- The result weakens the original ChunkPool cache-locality hypothesis for macro performance. The measured workload is dominated by high-frequency resting-add fixed costs, not by deep sweeps or cancel traversal.
- From an HFT engineering perspective, the most profit-sensitive path exposed by this profile is `add_rest`, because it is both the largest weighted cost and the quote-placement path affecting FIFO queue priority.

## Phase 5: add_rest Stage Profiling (Created, Then Retired)

### Design

To move from operation-level attribution to function-level attribution inside the dominant `add_rest` path, an instrumentation feature was added behind `LLMES_PROFILE_ADD_REST_STAGES`. It wrapped each sub-stage of `add_limit_order` in a `__rdtsc()` + `steady_clock::now()` timer pair:

- `validation`
- `match`
- `pool_acquire`
- `node_init`
- `level_lookup_existing` / `level_create_new`
- `fifo_append`
- `id_index_insert`

The `get_or_create()` return type was changed to `std::pair<PriceLevel*, bool>` so the instrumentation could distinguish an existing-level lookup from a new-level creation.

### Recorded Result and Conclusion

A cloud run (commit `237820e`, batch 100000) reported each stage at roughly 139-150 cycles, with `level_lookup_existing` the cheapest at 63.89 cycles. This led to the (later overturned) conclusion that the `std::map` price-level container was not a binding constraint.

### Why It Was Retired

The production `perf record` run (next section) contradicted the stage data. The reconciliation showed the stage profiler was unreliable: the operations being timed are ~5-40 ns each, while a `__rdtsc()` + `steady_clock::now()` pair is itself tens of ns. The probe cost was comparable to or larger than the probed region, so the measurement reported mostly its own overhead and **flattened** the real spread. The evidence is internal to the stage data itself: `node_init` (a 4-field struct assignment), `fifo_append` (a 4-pointer linked-list append), and `pool_acquire` (a free-list pop) all reported ~140 cycles, which none of those operations can genuinely cost.

The conclusion was that per-sub-stage timing instrumentation is not a viable profiling method for this engine. The entire feature was removed: `add_rest_stage_profile.{hpp,cpp}`, the `order_book.cpp` and `bench_hft_macro.cpp` instrumentation, the CMake options, and `run_hft_macro_add_rest_stage_profile.sh`. Operation-level profiling (`LLMES_PROFILE_HFT_MACRO_OPS`) was kept, because it times whole operations with a single timer pair and its op shares are corroborated by the perf function-level breakdown.

## Phase 5: Window-Isolated `perf record` Production Profiling

### Design

The next profiling step was a production-mode `perf record` for instruction- and function-level attribution with no instrumentation overhead. The problem is that `bench_hft_macro`'s `Setup()` is heavy and runs on every measured iteration (~5,000 seed adds, a 500,000-event warmup replay, two book rebuilds, and full batch pre-generation). A naive `perf record ./bench_hft_macro` would be dominated by that scaffolding.

The solution reuses the existing PMC enable/disable window idea via `perf record --control=fifo`. A new env-guarded helper in the runner (`benchmark_runner.cpp::PerfRecordControl`) enables sampling only around the measured `RunOp` batch; warmup, `Setup()`, and `Teardown()` run with sampling disabled. The helper is inert (zero overhead) unless the FIFO paths are provided via `LLMES_PERF_CTL_FIFO` / `LLMES_PERF_ACK_FIFO`. Driver script:

```text
benchmark/scripts/run_hft_macro_perf_record.sh
```

The profiling binary is built Release + `-g` with no `LLMES_PROFILE_*` macros, so the recorded code path is the exact production engine.

### Cloud Run

```text
commit:       635f1c8
events:       cycles, branch-misses (freq 8000, call-graph dwarf)
orders:       100000
levels:       100
batch_size:   1000000
warmup_iters: 1
iters:        40
window:       RunOp batch only (perf --control=fifo, -D -1)
```

Artifacts:

```text
benchmark/results/hft_macro_perf_record_cloud_20260601/
```

The run was clean: 40 paired enable/disable transitions, a clean 18.47M ops/s @ 54.1 ns/op (no instrumentation overhead), 30,354 samples. (`annotate_*.txt` came out empty because at `-O3` the engine is fully inlined into `RunOp`, so there is no standalone `add_limit_order` symbol; the call-graph report still gives full function-level attribution.)

### Function-Level Findings (share of all `RunOp` samples)

| Function / sub-cost | cycles % | branch-miss % |
|---|---:|---:|
| `add_limit_order` | 50.5% | 50.4% |
| ┣ `get_or_create` (`std::map`) | 17.8% | 18.9% |
| ┣ `contains` (id-index dup check) | 11.2% | 10.6% |
| ┣ `emplace` (id-index insert) | 9.9% | 9.9% |
| `cancel_order` | 29.8% | 29% |
| `modify_order` | 8.0% | 8% |

Two main findings:

1. **`id_to_order_` (`absl::flat_hash_map`) dominates the whole macro.** Summed across operations (add 21.1%, cancel 27.6%, plus modify), roughly half of all macro cycles are spent in the cancel index. The add path probes the same key twice: `contains` (duplicate guard, always misses on `add_rest`) then `emplace` (re-hashes and re-probes). The `contains` cost (~11% of cycles) is recoverable by merging into a single `lazy_emplace` / `find_or_prepare_insert`.

2. **The `std::map` level container is NOT cheap in a long run.** `get_or_create` is 35% of `add_limit_order` (17.8% of all cycles) and the single largest branch-miss source (`_Rb_tree_insert_and_rebalance` plus `lower_bound`), with `operator new` / `malloc` on level creation and `cfree` / rebalance on `erase_best`. This contradicts the stage-profiling conclusion. The contradiction is a measurement artifact (see the retired stage-profiling section), not a workload difference; the production `perf record` profile is the authoritative measurement.

Full analysis is recorded in `report/phase5_macro_profiling_plan.md`.

### Production-Evidence-Ranked Optimization Plan

An earlier draft proposed merging the add-path `contains` + `emplace` into one `lazy_emplace` call. That idea was **withdrawn**: the dup-check runs before matching and the insert runs after, so they serve two different correctness purposes and cannot be merged without changing semantics; and `absl::flat_hash_map` ignores the iterator hint, so on the (always-miss) `add_rest` path the insert re-probes from scratch regardless. Under the current id contract (arbitrary external `uint64_t` id + reject-duplicate-before-match), the hash map's ~50% is essentially irreducible.

The real lever is to **replace** the cancel-index hash map, not optimize it in place:

- **Step 1 — Replace `id_to_order_` with a generational slotmap on the order pool (largest lever).** In a real venue the exchange gateway assigns the order id, so id assignment is engine-controlled and can be made dense. The `OrderPool` is already a contiguous `std::vector<Order>` slab, so `slot_index = &order - &pool_[0]` is free. A handle packs `{generation, slot_index}`: lookup becomes one array load (no hash, no probe), the add-path dup-check disappears (slot allocation is the uniqueness check), and a per-slot generation counter rejects stale handles (ABA safety after slot reuse). `id_to_order_` is deleted; `Order.id` is kept only for trade reporting. This recovers most of the ~50% cancel-index cost. Requires: deciding whether to drop `pending_cancel_ids_` (cancel-before-insert cannot occur with engine-issued handles), mandatory generation correctness, and a `bench_hft_macro` change to track engine-returned handles.
- **Step 2 — Pooled allocator for the `std::map` price levels (safe).** Remove level create/destroy `operator new` / `malloc` / `cfree` churn while keeping pointer stability (`Order::parent_level`).
- **Step 3 — Structural price-level replacement (deferred).** Only if Step 2 leaves material `lower_bound` + rebalance cost; replacing `std::map` with `absl::btree_map` or a contiguous structure first requires an ownership-model change because value-moving containers would invalidate `Order::parent_level`.

Each step is validated with a 10-trial production PMC comparison plus a re-run of the window-isolated `perf record`.

## Current Status

As of the current repository state:

- `master` is based on `phase4a`
- per-operation HFT macro profiling (`LLMES_PROFILE_HFT_MACRO_OPS`) is retained
- the `add_rest` stage-profiling feature was added and then **removed** as a non-viable measurement method (probe overhead dwarfed the probed region)
- a window-isolated production `perf record` path was added (`PerfRecordControl` + `benchmark/scripts/run_hft_macro_perf_record.sh`) and cloud-validated
- ChunkPool benchmark artifacts are recorded but the design is not the active baseline
- the production profile shows the cancel-index hash map (~50% of macro cycles) and the `std::map` level container (~24% of cycles, top branch-miss source) as the two cost centers; the planned response is to replace `id_to_order_` with a generational slotmap on the order pool, then add a pooled allocator to the `std::map` price levels
- the latest profiling analysis is recorded in `report/phase5_macro_profiling_plan.md` (and the earlier `report/phase4_hft_macro_optimization_priority.md`)
- Phase 6 (`OrderHandle`) is implemented on `master`; benchmarks migrated per `report/phase6_benchmark_handle_migration.md`
- unified Phase 1–6 narrative + Jun 2026 devalidated `hft_macro` table: `report/phase_evolution_phase1_to_phase6.md`, CSV `server_results/hft_macro_cross_phase_summary_20260603.csv`

## Jun 2026 Unified `hft_macro` Campaign (Devalidated + Phase 6)

Cloud runner: `benchmark/scripts/run_remote_compare.sh` on Hetzner CCX23.

| Artifact | Contents |
|---|---|
| `server_results/devalidated_hft_macro_20260603_150410/` | 8 branches: p1, p2a–e, p4a, p4-finale (all `*-devalidated`) |
| `server_results/compare_master_vs_phase5_20260603_143852/` | `master` vs `phase5-finale-devalidated` (skipped re-run) |
| `server_results/hft_macro_cross_phase_summary_20260603.csv` | Merged headline latency/PMC row per phase |

Headline `hft_macro` at `orders=100000`, `levels=100`, 10 trials (devalidated unless noted):

| Phase tag | avg ns/op | ops/s |
|---|---:|---:|
| p1-deval | 2170 | 0.47M |
| p2a-deval | 2137 | 0.47M |
| p2b-deval | 48.3 | 20.7M |
| p2c-deval | 70.6 | 14.2M |
| p2d-deval | 71.8 | 13.9M |
| p2e-deval | 39.8 | 25.2M |
| p4a-deval | 39.3 | 25.5M |
| p4fin-deval | 40.2 | 24.9M |
| phase5-deval | 34.4 | 29.1M |
| master (Phase 6) | 29.3 | 34.1M |

Phase 1–2a rows are O(N)-cancel bound and not comparable to 2b+ for ranking. Phase 6 uses handle-aware benchmark scope (handles resolved in `Setup()`).
