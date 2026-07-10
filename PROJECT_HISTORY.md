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
- `report/phase7_hot_ring_cold_map_design.md`
- `report/phase7_benchmark_results.md`
- `report/phase8_fixed_array_design.md`
- `report/phase8_array_side_book_results.md`
- `report/phase9_per_scenario_benchmark.md`
- `report/phase10_progress.md`
- `report/phase11_lto_pgo_results.md`
- `benchmark/results/campaign_20260601_1319/`
- `benchmark/results/hft_macro_perf_record_cloud_20260601/`
- `server_results/macro_op_profile_cloud_t1/`
- `server_results/hft_macro_perf_record_master_20260603_153306/`
- `server_results/compare_master_vs_phase6a_20260603_173405/`
- `server_results/compare_master_vs_phase6a_20260605_182321/`
- `server_results/master_ring_size_sweep_trials30_20260605_185129/`
- `server_results/compare_master_vs_phase7b_20260606_184425/`
- `server_results/compare_master_vs_phase7c_newvm_20260610_172132/`
- `server_results/compare_master_vs_phase8b_20260610_183431/`
- `server_results/hft_macro/scenarios_tuned/hft_macro_scenarios_tuned_20260612_002441/`
- `server_results/hft_macro/scenarios_tuned/hft_macro_scenarios_tuned_20260612_005335/`
- `server_results/hft_macro/scenarios_tuned/hft_macro_scenarios_tuned_20260612_014047/`
- `server_results/nohz_full_setup_20260612_023844/`
- `server_results/hft_macro/scenarios_tuned/hft_macro_scenarios_tuned_20260613_162525/`
- `server_results/hft_macro/scenarios_tuned/hft_macro_scenarios_tuned_20260613_193319/`
- `server_results/hft_macro/pgo_compare/pgo_compare_20260614_113205/`
- `server_results/hft_macro/perf_record/hft_macro_perf_record_20260614_115103/`
- `server_results/hft_macro/scenarios_tuned/hft_macro_scenarios_tuned_20260614_120210/`

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
- `PriceLevel` per price level
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

The conclusion was that per-sub-stage timing instrumentation is not a viable profiling method for this engine. The entire feature was removed: `add_rest_stage_profile.{hpp,cpp}`, the `order_book.cpp` and `bench_hft_macro.cpp` instrumentation, the CMake options, and `run_hft_macro_add_rest_stage_profile.sh`. The operation-level profiling path (`LLMES_PROFILE_HFT_MACRO_OPS` / `LLMES_PROFILE_HFT_MACRO_OP_PMCS`) was later removed as well, because even whole-operation inline timing/PMC attribution still changes the measured hot path and is superseded by window-isolated production `perf record`.

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

## Phase 6a: Gateway-Owned Identity, No Hash on the Matching Hot Path

Phase 6a implements the handle refactor planned in `report/phase6_engine_handle_refactor_plan.md`. The goal is not to delete order-id lookup from the **system**, but to **relocate** it out of the single-threaded matching core so the engine hot path never pays for a hash table.

### Design

**Boundary split:**

| Layer | Responsibility |
|---|---|
| **Exchange gateway** (out of repo / ingress) | Accept `client_order_id`, enforce duplicate-id and cancel-before-add rules, maintain `client_order_id → OrderHandle` when clients do not echo the exchange token, forward **resolved handles** to the engine on cancel/modify |
| **Matching core** (`OrderBook`) | Price-time matching only: pool-backed orders, intrusive FIFO per level, `std::map` price levels via `SideBook` |

**Matching-core API after 6a:**

- `add_limit_order(...)` still takes a business `order_id` for trade reporting; on rest it returns `AddResult::handle` (engine-issued pool slot index).
- `cancel_order(OrderHandle)` / `modify_order(OrderHandle, ...)` — no `order_id` on the cancel/modify entry points.
- Lookup is `OrderPool::resolve(handle)` → pointer into the contiguous pool (**O(1) index**, no hash probe).
- `absl::flat_hash_map` **`id_to_order_` removed** from `OrderBook`; `pending_cancel_ids_` and in-core duplicate-id checks removed with gateway-owned validation.

**Handle model (current code):** `OrderHandle = std::uint32_t` slot index into `OrderPool` (`types.hpp`). Production can later add a generation field for ABA safety when slots are reused; the performance mechanism is the slot index, not the hash map.

**Honest scope:** If cancel still arrives keyed only by arbitrary `client_order_id`, **some** component must hash or map that id — Phase 6a moves that work to the gateway (parallelizable / shardable / pipelined off the critical path). When the client echoes the exchange-issued token, the gateway can decode the handle with **no** hash lookup. The matching-core benchmark measures only the engine contract: handles are already resolved before the timed window.

### Implementation (key commits on `phase6a` / `master`)

| Commit | Change |
|---|---|
| `eaa4eb6` | Replace `id_to_order_` with `OrderPool` handles; handle-based cancel/modify |
| `0e3498d` | `bench_hft_macro` / `bench_overall`: precompute `target_handle` in `Setup()`; no id→handle map in `RunOp()` |
| `71ed912` | Trim legacy micro-benches; focus benchmark suite on HFT paths |
| `00f8f8e` | Rename `IntrusiveList` → `PriceLevel` (semantics unchanged) |

Docs: `report/phase6_benchmark_handle_migration.md`, `report/phase6_engine_handle_refactor_plan.md`.

### Benchmark contract

- `bench_hft_macro` uses deterministic dual-build: replay the same op stream in `Setup()` to capture handles, rebuild the book, then time `RunOp()` with `cancel_order(op.target_handle)` only.
- Putting `client_order_id → handle` inside `RunOp()` would **reintroduce** the hash cost into the measured path and invalidate comparisons — explicitly forbidden by the plan.

### Measured outcome

**Macro (cloud, Jun 2026):** `master` / Phase 6a vs `phase5-finale-devalidated` at `orders=100k`, `levels=100`, 10 trials — **29.3 vs 34.4 ns/op** (~17.6% faster); see `server_results/compare_master_vs_phase5_20260603_143852/`.

**Production `perf record` on `master` @ `f77e051` (window-isolated RunOp):** `server_results/hft_macro_perf_record_master_20260603_153306/SUMMARY.md`

| Operation | cycles % (Phase 6a) | cycles % (pre-handle, Phase 5 profile) |
|---|---:|---:|
| `add_limit_order` | 55.3% | ~50.5% |
| `cancel_order` | **9.1%** | **~29.8%** |
| `modify_order` | 10.7% | ~8.0% |

- **No `flat_hash_map` / `contains` / `find` on the cancel index** appears in the Phase 6a profile.
- `cancel_order` is mostly `resolve` + intrusive `erase` + pool `release`.
- Dominant remaining cost inside add: **`std::map` `get_or_create`** (~28% of all `RunOp` cycles) — the next optimization target after 6a.

### Conclusion

Phase 6a closes the Phase 5 perf-record action item (“replace cancel-index hash map”): hash-table work for cancel/modify identity is **relocated to the gateway** (or avoided via exchange-issued tokens), and the **matching hot path** uses pool-index handles only. Price-level `std::map` work remains on the hot path and is the primary lever for Phase 6b+.

**Branch tag:** `phase6a` points at this milestone (same tree as `master` at branch creation).

## Current Status

As of the Phase 11 endpoint:

- **`phase6a`** tags the gateway/handle milestone; the active architecture and benchmark campaign have advanced through Phase 11
- matching core: **no** `id_to_order_`; cancel/modify are handle-based; gateway owns id validation and id→handle mapping off the hot path
- active price-level storage: `ArraySideBook<IsAsk>` with 4096 direct-addressed levels per side and a fixed two-level `OccupancyTree`
- `PriceLevel` is 16 bytes; orders remain pool-backed and linked intrusively within each price level
- Phase 8b's lazy ghost cleanup remains active; the Phase 8c eager-retirement metadata experiment was rejected
- intrusive per-operation HFT macro profiling (`LLMES_PROFILE_HFT_MACRO_OPS` / `LLMES_PROFILE_HFT_MACRO_OP_PMCS`) has been removed from benchmark code and scripts
- the `add_rest` stage-profiling feature was added and then **removed** as a non-viable measurement method (probe overhead dwarfed the probed region)
- the benchmark suite now builds the batched `hft_macro` benchmark and the diagnostic `hft_macro_scenarios` collector; the older legacy/HFT micro executables have been removed
- per-scenario collection records every `add_rest_existing_level`, `add_rest_new_level`, and `cancel_order` sample to CSV using isolated workload replays
- cloud measurement runs on Hetzner CCX23; `benchmark/scripts/remote/compare.sh` applies the tested CPU/NUMA/IRQ/workqueue/realtime tuning after boot-time isolation setup
- `nohz_full` reduced benchmark-CPU softirqs by roughly an order of magnitude but did not improve p99, so Linux-system tuning is considered complete for this VM
- Phase 10 rejected PriceLevel and order-slot cache reuse as explanations for the common `add_rest_new_level` p99/p999 tail; per-call timing remains diagnostic only
- Phase 11 selected LTO as the performance Release configuration: 15.630 ns/op, 63.99M ops/s, and 94.81 instructions/op across the 50-seed comparison
- PGO and LTO+PGO did not beat LTO alone; PGO benchmark support was removed
- the matching-engine and order-book core is frozen; future work moves to SPSC event transport, thread ownership, networking, and persistence
- ChunkPool benchmark artifacts are recorded but the design is not the active baseline
- PMR price-level node pooling was tested after Phase 6a; it reduced cache misses but increased instruction count and regressed macro latency, so it is not the active direction
- unified Phase 1–6 narrative: `report/phase_evolution_phase1_to_phase6.md`, CSV `server_results/hft_macro_cross_phase_summary_20260603.csv`
- Phase 7 narrative and benchmark report: `report/phase7_hot_ring_cold_map_design.md`, `report/phase7_benchmark_results.md`
- Phase 8 rationale and result: `report/phase8_fixed_array_design.md`, `report/phase8_array_side_book_results.md`
- Phase 9 per-scenario and system-tuning report: `report/phase9_per_scenario_benchmark.md`
- Phase 10 attribution report: `report/phase10_progress.md`
- Phase 11 compiler-optimization report: `report/phase11_lto_pgo_results.md`

## Jun 2026 Unified `hft_macro` Campaign (Devalidated + Phase 6/7)

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
| phase6a / master snapshot | 29.3 | 34.1M |
| Phase 7a (ring + cold map) | 23.2 | 43.1M |
| Phase 7b (+ PriceLevelPool) | 21.2 | 47.3M |
| Phase 7c (+ short-function inline) | 19.3 | 51.7M |

Phase 1–2a rows are O(N)-cancel bound and not comparable to 2b+ for ranking. Phase 6 uses handle-aware benchmark scope (handles resolved in `Setup()`). Phase 7 adds the hot ring / cold map price-level storage on top of the handle-based core, then a dedicated price-level pool, then header-only forced inlining for the small pool/handle helpers. See the Phase 7 section below for the full progression table.

## Phase 6b Candidate: PMR Price-Level Node Pool (Rejected)

### Design Hypothesis

After the Phase 6a handle migration, window-isolated `perf record` showed that the dominant remaining macro cost moved to `add_limit_order`, especially `std::map` price-level `get_or_create`. A natural follow-up hypothesis was that frequent `std::map` node allocation and free on price-level creation/destruction might still be hurting the hot path.

The tested implementation kept the `std::map` design and pointer stability, but changed the allocator:

- `std::map<price, PriceLevel>` became `std::pmr::map<price, PriceLevel>`
- each `SideBook` owned a fixed local buffer
- the map used `std::pmr::unsynchronized_pool_resource` backed by a `std::pmr::monotonic_buffer_resource`
- the intent was to avoid hot-path `new` / `delete` for map nodes without changing matching semantics

### Benchmark Result

The cloud comparison is stored under:

```text
server_results/compare_master_vs_phase6a_20260603_173405/
```

Configuration: `hft_macro`, `orders=100000`, `levels=100`, `batch_size=100000`, 10 trials, latency + PMC.

| Version | Meaning | avg ns/op | ops/s | instr/op | cache miss/op |
|---|---|---:|---:|---:|---:|
| `phase6a` @ `d778e4f` | no PMR map experiment | 29.21 | 34.24M | 183.6 | 0.042 |
| `master` @ `7a25934` | PMR node pool for `std::map` | 31.51 | 31.80M | 218.1 | 0.033 |

PMR did improve the cache-miss metric:

- cache misses/op fell from `0.042` to `0.033` (about 21% lower)
- CPI also fell (`0.579` to `0.519`)

But the total hot-path work increased more:

- instructions/op rose from `183.6` to `218.1` (about 19% higher)
- branches/op rose from `41.36` to `47.87` (about 16% higher)
- average latency regressed from `29.21ns` to `31.51ns` (about 8% slower)

### Interpretation

This result rejects the PMR node-pool change as a Phase 6b direction. It is directionally useful, but not a win:

- PMR can reduce cache misses for the map-node allocator path.
- The extra allocator abstraction, pool-resource metadata, and control flow increase instruction count and branch count.
- The macro workload is already warm by the time the measured `RunOp` window begins.

The last point is important. In the current `hft_macro` setup, the book is seeded, then driven through a long untimed warmup (`500000` generated events in the current code), then rebuilt before the measured batch. By that point, most active price levels are likely already represented in the book. That means true price-level `new` / `delete` frequency during the measured window may be much lower than the raw `get_or_create` profile initially suggests.

This hidden factor is plausible but not worth spending a separate validation campaign on right now. Even if verified, it would only explain why allocator pooling has limited upside. The higher-potential problem is still the ordered-map operation itself: price lookup, branchy tree traversal, `try_emplace`, and RB-tree maintenance.

### Optimization Direction

The Phase 6b+ direction is therefore:

- prioritize reducing instruction count and branchy ordered-map work
- attack `get_or_create` by reducing binary/tree search and `std::map` operations
- explore a ring-buffer or indexed hot price window so common price-level access becomes index arithmetic
- keep a cold ordered path only if the design still needs arbitrary out-of-window prices

Unless new evidence shows a real production p99 memory-stall problem, future optimization work should not target cache misses as the primary objective. Cache-locality work is only justified as a side effect of a structure that also reduces instructions, not as the main design goal.

## Phase 7: Hot Ring Buffer + Cold Map Price-Level Storage

Phase 7 implements the ring-buffer direction identified after the PMR rejection. The goal is to remove branchy `std::map` lookup from the dominant near-best resting-add path while retaining correctness for arbitrary price drift in `hft_macro`.

### Design

Each side book is now:

```text
CachedSideBook<IsAsk>
├── RingBuffer<IsAsk> hot_
├── std::map<price, PriceLevel*, PriceCompare<IsAsk>> cold_
└── PriceLevelPool pool_      (Phase 7b; Phase 7a used new/delete per level)
```

Important implementation choices:

- `OrderBook` still uses the same side-book interface: `empty()`, `best_price()`, `best_level()`, `get_or_create(price)`, and `erase_best()`.
- Hot prices are addressed by directed rank from the current best: ask uses `price - best`, bid uses `best - price`.
- `RingSize` is currently 16. The physical slot index is `(anchor + rank) & (RingSize - 1)`.
- `RingBuffer` tracks live slots with `live_mask_`, and the mask type is selected at compile time through `uint_from_size<RingSize>` (`uint16_t` for the current configuration).
- `next_live_offset()` uses `std::rotr` plus `std::countr_zero` to find the next live price level after the best is drained.
- `PriceLevel` objects are pointer-stable: moving a level between hot and cold only moves the pointer, never the pointee, so `Order::parent_level` remains valid. Phase 7a held levels via `std::unique_ptr` (one `new`/`delete` per level, matching the `std::map` baseline for a fair comparison); Phase 7b replaced that with `PriceLevelPool`, a free-list pool that removes all hot-path `new`/`delete`.

The current implementation keeps a strict invariant:

```text
cold_ only contains prices outside the current hot window.
```

When the best price advances toward worse prices, `erase_best()` promotes cold entries that newly fall inside the hot window. Earlier discussion considered lazy addition / resident-cache semantics, but that direction is not currently adopted. The reason is workload evidence: the `erase_best()` / matching path is low-frequency (about 2% in the current macro mix), while strict hot-window maintenance keeps the dominant `get_or_create()` path simple and avoids duplicate same-price levels across hot and cold tiers.

### Implementation Milestones

| Commit | Change |
|---|---|
| `f283b3d` | First ring-buffer implementation |
| `c8adf9b` | First `CachedSideBook` WIP integration |
| `bc70159` | Finished hot ring buffer + cold map design (Phase 7a) |
| `fc971b9` | `live_mask_` type trait and bit-manipulation cleanup |
| `1096ad5` | Phase 7 benchmark report |
| `e8c4f29` | `PriceLevelPool`: remove hot-path `new`/`delete` (Phase 7b) |
| `397e80a` | Header-only pools + `always_inline` short hot helpers (Phase 7c) |

### Benchmark Result: Phase 7 vs Phase 6a

Cloud run:

```text
server_results/compare_master_vs_phase6a_20260605_182321/
```

Configuration: `hft_macro`, `orders=100000`, `levels=100`, `batch_size=100000`, 10 trials.

| Version | Meaning | avg ns/op | ops/s | instr/op | branch miss/op |
|---|---|---:|---:|---:|---:|
| `phase6a` @ `d778e4f` | handle-based core, `std::map` side book | 30.30 | 33.0M | 183.6 | 2.17 |
| `master` @ `00e6470` | hot ring + cold map | 24.26 | 41.2M | 177.3 | 1.48 |

Main result:

- latency improved by about 19.9%
- throughput improved by about 24.9%
- branch misses dropped by about 32.2%
- CPI dropped by about 15.1%

The speedup is primarily a reduction in branchy ordered-map work and pointer-chasing on the near-best add path. Cache misses rose slightly, but the instruction-path and CPI wins dominate.

### RingSize Sweep

A 30-trial sweep tested `RingSize = 8, 16, 32, 64` at a single commit:

```text
server_results/master_ring_size_sweep_trials30_20260605_185129/
```

| RingSize | avg ns/op | 95% CI | ops/s |
|---:|---:|---|---:|
| 16 | 24.091 | [23.998, 24.184] | 41.5M |
| 32 | 24.100 | [24.015, 24.186] | 41.5M |
| 64 | 24.245 | [24.145, 24.344] | 41.2M |
| 8 | 25.057 | [24.967, 25.148] | 39.9M |

Conclusion:

- `RingSize=8` is too small for the current HFT macro locality profile and is about 4% slower.
- `RingSize=16` and `RingSize=32` are statistically indistinguishable.
- `RingSize=64` shows a weak regression trend, likely due to extra instruction/codegen cost rather than cache footprint.
- `RingSize=16` is the current choice because it matches 32's performance with half the ring footprint.

### Phase 7b: PriceLevelPool

Phase 7a left one allocator cost on the hot path: each new price level was a `make_unique<PriceLevel>` (one `malloc`, freed on level removal). Phase 7b replaces this with `PriceLevelPool`, a free-list pool of pre-allocated `PriceLevel` objects. `acquire()`/`release()` are a few pointer operations; cold-map values become raw `PriceLevel*` owned by the pool.

Cloud run (`server_results/compare_master_vs_phase7a_20260606_171238/`), `hft_macro`, `orders=100000`, `levels=100`, 10 trials:

| Version | Meaning | avg ns/op | ops/s | instr/op | cache miss/op |
|---|---|---:|---:|---:|---:|
| `phase7a` @ `da9be8c` | hot ring + cold map, `make_unique` | 23.18 | 43.1M | 177.5 | 0.041 |
| `master` @ `e8c4f29` | + `PriceLevelPool` | 21.16 | 47.3M | 153.8 | 0.033 |

Pool is about 8.7% faster, 95% CIs do not overlap. Unlike the rejected PMR experiment, this is a clean win on **both** axes: instructions dropped 13.4% **and** cache misses dropped 19.3%. PMR added an allocator-abstraction layer that raised instruction count; the pool's free-list and contiguous storage reduce both. (CPI rose 6.6% as a denominator artifact — fewer total instructions leave the expensive ones a larger fraction — but absolute cycles/op still fell 7.7%.)

A fresh window-isolated `perf record` (`server_results/hft_macro_perf_record_master_20260606_161810/`, commit `e8c4f29`) confirms the allocator block is gone: `get_or_create` fell from ~12.3% to ~9.5% of RunOp cycles and hot-path `malloc` disappeared (`PriceLevelPool::acquire` is 1.71%). The newly dominant `add_limit_order` cost centers are `match_against` (~10%), `reanchor_to` (~4.8%), and `AddResult`/`vector<Trade>` construct-destruct (~1.9%) — candidate targets for Phase 8.

### Phase 7c: Header-Only Short Helpers and Forced Inlining

After Phase 7b, perf still showed several tiny helpers as visible hot-path calls. These functions do very little work individually, but they occur on extremely high-frequency paths:

- `OrderPool::acquire()`
- `OrderPool::release()`
- `OrderPool::resolve()`
- `PriceLevelPool::acquire()`
- `PriceLevelPool::release()`

Phase 7c moved the hot short function bodies into headers and marked them with `[[gnu::always_inline]]`. The final patch also applied the attribute to other small ring/price-level helpers, but the main expected source of gain was the pool acquire/release and handle resolve functions that prior perf reports had explicitly shown were not consistently inlined.

Cloud comparison:

```text
server_results/compare_master_vs_phase7b_20260606_184425/
```

Configuration: `hft_macro`, `orders=100000`, `levels=100`, `batch_size=100000`, 10 trials.

| Version | Meaning | avg ns/op | ops/s | instr/op | cycles/op |
|---|---|---:|---:|---:|---:|
| `phase7b` @ `79031d5` | PriceLevelPool baseline | 21.36 | 46.8M | 153.8 | 77.7 |
| `master` @ `397e80a` | header-only pools + short-function `always_inline` | 19.33 | 51.7M | 137.1 | 71.6 |

Result:

- latency improved by about 9.5% (`21.36 -> 19.33 ns/op`)
- throughput improved by about 10.5% (`46.8M -> 51.7M ops/s`)
- instructions/op dropped by about 10.9% (`153.8 -> 137.1`)
- cycles/op dropped by about 7.9% (`77.7 -> 71.6`)

This is a large result for an inlining-only change, but it is consistent with the profile evidence: high-frequency helpers with tiny bodies are exactly where function-call overhead and missed cross-call optimization are expensive relative to useful work. The result should not be interpreted as a general endorsement of blanket `always_inline`; it worked here because the profile had already identified these helpers as visible hot-path call sites.

Build note: the first remote attempt at `0d0422f` failed because the pool `.cpp` files had been removed but were still listed in CMake. Commit `397e80a` removed those stale CMake entries and produced the successful run.

### Current Interpretation

Phase 7 validates the Phase 6b+ hypothesis: the next useful lever after handle-based identity was not allocator pooling of the ordered map, but removing ordered-map work from the near-best price-level path (Phase 7a), followed by direct level pooling (Phase 7b), and then eliminating missed inlining on tiny high-frequency pool/handle helpers (Phase 7c).

### Cross-Phase headline `hft_macro` progression

| Phase | avg ns/op | ops/s | Key change |
|---|---:|---:|---|
| Phase 1 | 2170 | 0.47M | `std::list`, O(N) cancel |
| Phase 2a | 2137 | 0.47M | pool-backed intrusive list |
| Phase 2b | 48.3 | 20.7M | `unordered_map` O(1) cancel index |
| Phase 2e | 39.8 | 25.2M | `absl::flat_hash_map` (Swiss Table) |
| Phase 4a | 39.3 | 25.5M | `SideBook` abstraction |
| Phase 5 | 34.4 | 29.1M | production profiling baseline |
| Phase 6a | 30.3 | 33.0M | handle-based cancel, no in-core id hash |
| Phase 7a | 23.2 | 43.1M | hot ring buffer + cold map |
| Phase 7b | 21.2 | 47.3M | + PriceLevelPool |
| Phase 7c | 19.3 | 51.7M | + header-only/always_inline short helpers |
| Phase 8b | 17.2 | 58.1M | unified array side book + fixed occupancy tree |

Phase 1 → Phase 8b: roughly `2170 -> 17.2 ns/op`, about a 124x throughput improvement on the headline macro workload. Phase 1–2a rows are O(N)-cancel bound and not comparable to 2b+ for ranking.

The next step was a fresh production profile on Phase 7c before choosing a Phase 8 target. The Phase 7b profile remained useful for broad shape, but its pool acquire/release/resolve costs were partially superseded by Phase 7c. The older Phase 6a profile was superseded because its `std::map get_or_create` bottleneck had been structurally replaced.

## Phase 8: Unified Array Side Book

### Motivation

The Phase 7 hot ring made near-best lookup cheap, but the side book still coupled two different data structures. The production profile showed that hot-ring lookup itself consumed only about 5.5% of cycles, while best-level removal and ring re-anchoring consumed comparable shares:

- `erase_best`: about 4.37%
- `reanchor_to`: about 4.65%

This suggested that the next structural win was not a larger or more elaborate hot cache. It was removing the hot/cold boundary and using one direct-addressed representation for the benchmark's known price range.

### Design

Phase 8 replaced the Phase 7 `hot ring + cold map` side book with:

```text
ArraySideBook<IsAsk>
├── fixed-range array of PriceLevel objects
└── OccupancyTree bitmap hierarchy
```

The array provides direct price-to-index access. The occupancy tree summarizes non-empty price indices in 64-bit words, allowing next-best lookup without scanning every price slot. Bid/ask direction is encoded as a template parameter so side-specific traversal does not require a runtime branch.

### Phase 8a: First Working Version

Commit:

```text
81ab90488de2f6e2ebf4d7ab917c69e4f5711e70
```

Artifact:

```text
server_results/compare_master_vs_phase7c_trials50_20260608_210512/
server_results/hft_macro_perf_record_master_20260608_201629/
```

Phase 8a validated the architecture but was performance-neutral against Phase 7c:

| Metric | Phase 8a | Phase 7c |
|---|---:|---:|
| avg ns/op | 19.30 | 19.57 |
| cycles/op | 70.3 | 71.0 |
| instructions/op | 158.3 | 137.1 |
| branch misses/op | 1.351 | 1.497 |
| cache misses/op | 0.022 | 0.034 |

The array improved CPI, branch behavior, and cache behavior, but added about 21 instructions/op. Perf attributed the excess work mainly to ghost-level cleanup on pure read paths and generic occupancy-tree operations.

### Phase 8b: Fixed Occupancy Tree and Pure Read Paths

Commit:

```text
71f1ee191fbe40ad67d69572ccbc01c825d98b99
```

Artifacts:

```text
server_results/compare_master_vs_phase7c_newvm_20260610_172132/
server_results/hft_macro_perf_record_master_20260610_162529/
```

Phase 8b removed ghost cleanup from `empty()`, `best_price()`, and `best_level()`. It also fixed the occupancy tree to the known 65536-price range with `std::array` storage and unrolled the three-level set/clear/find operations.

| Metric | Phase 8b | Phase 7c | Change |
|---|---:|---:|---:|
| avg ns/op | 17.21 | 19.27 | -10.7% |
| cycles/op | 62.82 | 70.26 | -10.6% |
| instructions/op | 130.05 | 137.13 | -5.2% |
| branch misses/op | 1.229 | 1.496 | -17.8% |
| cache misses/op | 0.0202 | 0.0333 | -39.3% |

Phase 8b is the active performance baseline. It reaches about 58.1M macro operations/s on the comparable Hetzner CCX23 run.

### Phase 8c: Eager Empty-Level Retirement (Rejected)

Artifact:

```text
server_results/compare_master_vs_phase8b_20260610_183431/
```

Phase 8c tried to clear occupancy state immediately when cancel/modify removed the final order from a level. This required owner-side/index metadata and extra work on common mutation paths.

| Metric | Phase 8c | Phase 8b | Change |
|---|---:|---:|---:|
| avg ns/op | 17.95 | 17.05 | +5.2% slower |
| cycles/op | 65.93 | 62.39 | +3.54 cycles/op |
| instructions/op | 139.55 | 130.05 | +9.50 instructions/op |

The eager mechanism cost more than the lazy ghost cleanup it removed. Phase 8c was reverted conceptually and recorded as a negative result. Phase 8 stops at the Phase 8b design.

## Phase 9: Per-Scenario Macro Benchmark and Linux Isolation

### Motivation and Design

The normal `hft_macro` benchmark remains the release-level metric. It measures a whole batch with one timing window, keeping instrumentation overhead amortized. Phase 9 added a diagnostic benchmark that preserves the same pre-generated macro workload but records every call for three basic single-operation scenarios:

- `add_rest_existing_level`
- `add_rest_new_level`
- `cancel_order`

Crossing limit orders and market orders are replayed but not timed because one submitted operation can contain many internal matches. Modify is omitted because it is semantically cancel plus add.

To prevent one scenario's instrumentation from perturbing another, `focus=all` replays the deterministic workload separately for each measured scenario. The CSV records raw and overhead-adjusted cycles and elapsed nanoseconds for every measured call.

### Initial Finding

The first complete result contained 937,410 measured calls across 10 trials. It showed that:

- `cancel_order` is extremely compact and stable;
- adding to an existing level is also controlled;
- `add_rest_new_level` is the dominant tail-latency source among the measured basic operations.

This validated the existing/new-level split and identified the new-level activation path as the most useful future engine target.

### Linux System-Level Tuning

All final Phase 9 measurements were collected on a Hetzner Cloud CCX23 KVM VM with an AMD EPYC-Milan CPU. The tuning campaign progressively added:

- fixed CPU and NUMA binding;
- SMT-sibling-aware CPU selection;
- performance governor where exposed;
- background timer/service suppression;
- IRQ affinity observation and migration;
- workqueue cpumask migration;
- realtime FIFO scheduling via `chrt -f 95`;
- watchdog and RT-throttling changes;
- `/dev/cpu_dma_latency` locking;
- boot-time `nohz_full`, `rcu_nocbs`, `isolcpus`, `irqaffinity`, and `kthread_cpus`.

The final boot configuration isolated CPU2-3 and kept housekeeping on CPU0-1. Verification recorded:

```text
nohz_full=2,3
rcu_nocbs=2,3
isolcpus=nohz,domain,managed_irq,2,3
irqaffinity=0,1
kthread_cpus=0,1
```

### Final Result

The system tuning worked mechanically. On benchmark CPU2:

| Stage | LOC | RCU | TIMER | SCHED | softirq total |
|---|---:|---:|---:|---:|---:|
| dynamic CPU + IRQ move | 15590 | 1239 | 635 | 380 | 2254 |
| aggressive isolation | 15319 | 1332 | 639 | 368 | 2339 |
| `nohz_full` | 4695 | 117 | 11 | 91 | 219 |

Despite the large reduction in local timer and softirq activity, p99 did not improve materially:

| Scenario | dynamic CPU + IRQ move | aggressive isolation | `nohz_full` |
|---|---:|---:|---:|
| `add_rest_existing_level` | 22 ns | 22 ns | 22 ns |
| `add_rest_new_level` | 71 ns | 71 ns | 80 ns |
| `cancel_order` | 21 ns | 21 ns | 21 ns |

The p99 cycle values remained 44, 154, and 44 respectively across all three stages. A pinned `virtio5-request` IRQ remained on CPU2, but its frequency was too low to explain the stable p99 boundary.

### Conclusion

Phase 9 established a reusable per-scenario diagnostic framework and a reproducible cloud benchmark environment. It also produced a useful negative result: after ordinary IRQ, scheduler, workqueue, RCU, and periodic-tick noise were substantially reduced, p99 did not move.

Linux-system tuning is therefore closed for the current environment. The next optimization target is matching-engine instruction count, especially the `add_rest_new_level` path. The standard batched `hft_macro` benchmark remains the final decision metric; per-scenario results are diagnostic because per-call timestamp collection is intrusive.

## Phase 10: New-Level Tail Attribution

### Goal

Phase 10 investigated why `add_rest_new_level` had a much larger tail than adding to an existing level. The working hypotheses were occupancy-tree propagation, PriceLevel cache reuse, and order-pool slot reuse.

The benchmark generator was first fixed so every trial used a different deterministic random stream. Earlier multi-trial scenario runs had largely repeated the same workload.

### Occupancy-Tree Paths

The scenario benchmark attributed each new-level add to the exact work performed by `OccupancyTree::set()`.

| Bitmap path | Share | p50 cycles | p99 cycles | p999 cycles |
|---|---:|---:|---:|---:|
| Target bit already set | 63.7% | 44 | 66 | 110 |
| L1 only | 33.1% | 44 | 198 | 308 |
| Reached L2 | 2.0% | 88 | 330 | 550 |
| Reached L3 | 1.2% | 44 | 264 | 738 |

Upper-level propagation is visibly more expensive, but it is too rare to explain the whole common tail. Large outliers also appeared on the cheapest path.

### Layout and Cache Tests

`PriceLevel::size_` was unused and removed, reducing `PriceLevel` from 24 to 16 bytes. Four levels now fit in one 64-byte cache line.

Two reuse-distance studies then tested the cache hypothesis directly:

| Measured object | Spearman correlation with cycles | Conclusion |
|---|---:|---|
| PriceLevel | 0.107 | Weak relationship |
| Order-pool slot | 0.010 | No useful relationship |

First-touch samples were expensive, but rare. About 97% of acquired order slots had been used within the previous 100 operations because the pool's LIFO free list rapidly recycles a small hot set.

Manual PriceLevel prefetch was also tested. It did not produce a reliable end-to-end gain and sometimes worsened cancel tail latency, so no prefetch remains on the active order path.

### Smaller Price Range

The configured side-book range was reduced from 65536 to 4096 prices, removing the third bitmap level and reducing PriceLevel storage per side from 1 MiB to 64 KiB.

| Metric | 4096 levels | 65536 levels | Change |
|---|---:|---:|---:|
| Average ns/op | 18.099 | 17.953 | +0.8% |
| Cycles/op | 66.161 | 65.763 | +0.6% |
| Instructions/op | 128.494 | 130.049 | -1.2% |
| Cache misses/op | 0.02099 | 0.02198 | -4.5% |

The smaller structure was kept because it uses less memory and has a simpler two-level tree, not because it improved macro latency. The active benchmark touches only about 100 nearby prices, so both allocations already have a small hot working set.

### Conclusion

Phase 10 closed the `add_rest_new_level` p99/p999 investigation. Bitmap work changes the distribution, but neither PriceLevel reuse nor order-slot reuse explains the common tail. Per-call timestamp measurements are too intrusive and quantized to justify further optimization around their percentiles.

Future decisions return to the batched macro metrics: average ns/op, cycles/op, and instructions/op.

## Phase 11: LTO, PGO, and Matching-Core Freeze

### Build Matrix

Phase 11 tested four GCC 15 Release configurations on Hetzner CCX23:

1. Baseline: `-O3 -DNDEBUG -march=native`
2. LTO
3. PGO
4. LTO + PGO

PGO used 10 training seeds. Validation used 50 different seeds, with the four modes rotated in execution order for each paired trial.

Primary artifact:

```text
server_results/hft_macro/pgo_compare/pgo_compare_20260614_113205/
```

### Result

| Build | Average ns/op | Throughput | Cycles/op | Instructions/op | Branches/op |
|---|---:|---:|---:|---:|---:|
| Baseline | 17.589 | 56.86M | 64.62 | 127.67 | 25.06 |
| **LTO** | **15.630** | **63.99M** | **57.25** | **94.81** | **17.07** |
| LTO + PGO | 15.790 | 63.34M | 58.03 | 93.93 | 16.04 |
| PGO | 17.815 | 56.14M | 65.58 | 122.70 | 21.79 |

LTO reduced average latency by 11.1%, cycles by 11.4%, instructions by 25.7%, and branches by 31.9%. It beat baseline on all 50 paired validation seeds. Text size also fell from 95,810 to 83,491 bytes.

The win came from executing less code across translation-unit boundaries. CPI increased from 0.506 to 0.604 and branch misses stayed almost flat, so this was not a cache or branch-prediction improvement.

PGO alone was 1.3% slower than baseline. LTO+PGO was 1.0% slower than LTO despite executing slightly fewer instructions. Both configurations were rejected, and PGO-specific benchmark code and scripts were removed.

### Profiling and Scenario Checks

The zero-loss LTO `perf record` run captured 1320 samples. It remained useful for broad hotspot discovery, but cross-translation-unit inlining folded much of the engine into callers, making small function-level percentages unreliable.

Per-scenario cycle percentiles were unchanged by LTO:

| Scenario | p50 | p99 | p999 |
|---|---:|---:|---:|
| Existing-level add | 22 | 44 | 66 |
| New-level add | 44 | 176 | 286 |
| Cancel | 22 | 44 | 44 |

This does not conflict with the macro improvement. Per-call `rdtscp` measurement quantizes results into roughly 22-cycle steps and hides small changes inside an individual operation. LTO primarily optimized dispatch, calls, result handling, and surrounding executable-level work.

### Final Decision

LTO is the final performance Release configuration. PGO is not retained.

The matching-engine and order-book core is now considered complete for this project stage. Further assembly-level tuning would cost more time than the remaining gains justify. The next work moves to the system around the core:

- SPSC event and trade transport;
- single-owner matching-thread integration;
- network input/output;
- journal and recovery;
- execution and risk components.
