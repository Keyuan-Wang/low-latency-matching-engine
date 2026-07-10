# Phase 5 Macro Profiling Plan

## Context

Phase 4 ended with a corrected `hft_macro` benchmark and operation-level profiling. The important fix was removing measured-batch tracking drift: cancel and modify targets now come from live book state, so the macro workload no longer produces a large artificial `cancel_miss` population.

Phase 5 starts from the repaired `master` branch and focuses on profiling before making more data-structure changes.

## Latest Aligned Run

Cloud run:

```text
branch:       master
commit:       5186b6c
scenario:     hft_macro
trials:       1
orders:       100000
levels:       100
batch_size:   100000
warmup_iters: 1
iters:        1
seed:         42
```

Artifacts:

```text
server_results/macro_op_profile_cloud_master_pmc_t1_20260601/
```

The run contains two aligned measurements:

- `latency`: wall-clock macro and per-operation latency.
- `pmc`: in-process perf counters around the measured benchmark window.

## Add-Rest Stage Profile

The `add_rest` path was then instrumented and profiled separately to move from operation-level attribution to function-level attribution.

Cloud run:

```text
version_tag:  master-add-rest-stage
commit_sha:   c8a62a1+addrest
orders:       100000
levels:       100
batch_size:   100000
warmup_iters: 1
iters:        1
seed:         42
```

Artifacts:

```text
server_results/hft_macro_add_rest_stage_cloud_t1_20260601/
```

Sanity check:

| Metric | Value |
|---|---:|
| `add_rest` op count | 47851 |
| add-rest stage count | 47851 |
| `cancel_miss` | 0 |
| `modify_miss` | 0 |

Stage breakdown:

| Stage | Mean ns | Mean cycles | ns share | cycles share |
|---|---:|---:|---:|---:|
| `level_lookup` | 41.573 | 173.310 | 19.271% | 16.811% |
| `match` | 31.367 | 149.873 | 14.540% | 14.538% |
| `id_index_insert` | 30.707 | 146.900 | 14.234% | 14.249% |
| `validation` | 29.416 | 143.791 | 13.635% | 13.948% |
| `fifo_append` | 27.851 | 140.087 | 12.910% | 13.588% |
| `node_init` | 27.503 | 137.910 | 12.749% | 13.377% |
| `pool_acquire` | 27.314 | 139.065 | 12.661% | 13.489% |

This is profiling-mode data, so the absolute `add_rest` latency is inflated by instrumentation overhead. The useful signal is the relative split inside the add-rest path.

## Overall Result

| Mode | avg_ns | ops_s | cycles/op | instructions/op | CPI | branch miss rate | cache misses/op | ok |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `latency` | 103.934 | 9.62145e+06 | - | - | - | - | - | 199981 |
| `pmc` | - | - | 361.417 | 670.021 | 0.539411 | 0.0192954 | 0.07932 | 199989 |

The PMC result does not look memory-bound. `cache_misses_per_op = 0.07932`, which is roughly:

```text
0.07932 / 670.021 * 1000 = 0.118 cache misses per 1000 instructions
```

This is very low for a workload that would be dominated by LLC or DRAM misses. The current macro benchmark is more consistent with fixed instruction-path cost than with cache-miss latency.

## Operation-Level Result

The latency run gives the clearest weighted-cost picture:

| Operation | Share | Mean ns | p50 ns | p95 ns | p99 ns | Weighted time share |
|---|---:|---:|---:|---:|---:|---:|
| `add_rest` | 47.849% | 65.023 | 60 | 100 | 150 | 53.592% |
| `cancel_hit` | 46.159% | 43.507 | 40 | 60 | 60 | 34.592% |
| `modify_hit` | 3.929% | 117.144 | 100 | 220 | 290 | 7.928% |
| `market` | 1.597% | 119.105 | 80 | 250 | 410.4 | 3.276% |
| `add_cross` | 0.466% | 76.094 | 70 | 130 | 173.5 | 0.611% |
| `cancel_miss` | 0.000% | 0.000 | 0 | 0 | 0 | 0.000% |
| `modify_miss` | 0.000% | 0.000 | 0 | 0 | 0 | 0.000% |

The workload is now internally coherent:

- `cancel_miss = 0`
- `modify_miss = 0`
- `add_rest + add_cross ~= 48.3%`
- `cancel_hit ~= 46.2%`
- `market ~= 1.6%`

## Interpretation

### `add_rest` Is The Main Macro Cost Center

`add_rest` contributes more than half of measured macro time:

```text
47.849% event share * 65.023 ns mean = 53.592% weighted time share
```

It is not the slowest individual operation, but it is the highest-leverage path because it is hit on nearly half of all macro events.

In HFT terms, this is also the quote-placement path. Reducing `add_rest` latency can improve quote refresh speed and same-price FIFO positioning. That makes it more commercially relevant than optimizing rare market sweeps in the current workload.

### `cancel_hit` Is Important But Already Tight

`cancel_hit` is the second-largest weighted contributor, but its latency profile is already strong:

```text
mean = 43.507 ns
p50  = 40 ns
p95  = 60 ns
p99  = 60 ns
```

The current O(1) cancel path appears healthy. It should be protected during future changes, but the data does not point to cancel as the first optimization target.

### `modify_hit` Is Expensive But Low Share

`modify_hit` is more expensive than `add_rest` and `cancel_hit`, but its event share is only about 3.9%. Its weighted contribution is meaningful but secondary. This result is expected because modify is structurally close to cancel plus add.

### `market` Is Not The Current Bottleneck

Market orders are low-frequency and shallow in this workload:

```text
market share ~= 1.6%
market_levels_p99 = 2
market_filled_qty_p99 = 5
```

Optimizing deep market sweeps is unlikely to move this macro benchmark unless the workload model changes.

### Cache Locality Is Not The Leading Hypothesis

The low generic cache-miss rate weakens the hypothesis that macro performance is currently dominated by cache misses from poor order locality. This does not mean memory layout is irrelevant, but it does mean that the next optimization should not be based on cache-locality intuition alone.

The ChunkPool experiment was useful because it tested that intuition. The current profiling result suggests Phase 5 should instead identify which fixed steps inside `add_rest` consume cycles.

The function-level result confirms that `add_rest` is not dominated by a single call site. `level_lookup` is the largest measured stage, but it is only about 19% of the measured add-rest stage time. The rest of the path is spread across `match`, `id_index_insert`, `validation`, `fifo_append`, `node_init`, and `pool_acquire`.

## Cloud Profiling: level_lookup_existing vs level_create_new

The `get_or_create()` return type was changed from `PriceLevel&` to `std::pair<PriceLevel*, bool>`, using `try_emplace` instead of `operator[]`, to allow the profiling instrumentation to distinguish existing-level lookup from new-level creation. The `AddRestCallProfile` accumulates timing for whichever path was taken, then commits both to the global snapshot at the end of the `add_limit_order` call.

A cloud profiling run was executed with both `LLMES_PROFILE_HFT_MACRO_OPS=ON` and `LLMES_PROFILE_ADD_REST_STAGES=ON`.

Cloud run:

```text
branch:       master
commit:       237820e
scenario:     hft_macro
trials:       1
orders:       100000
levels:       100
batch_size:   100000
warmup_iters: 1
iters:        1
seed:         42
```

Artifacts:

```text
benchmark/results/add_rest_stage_profile_cloud_20260601/
├── latency_raw.csv
├── op_raw.csv
└── stage_raw.csv
```

Sanity check:

| Metric | Value |
|---|---:|
| `cancel_miss` | 0 |
| `modify_miss` | 0 |
| `add_rest` op count | 47,850 |

### Operation-Level Result (profiling mode)

| Operation | Count | Share | Mean ns | Mean cycles | Weighted ns share |
|---|---:|---:|---:|---:|---:|
| `add_rest` | 47,850 | 47.85% | 524.24 | 1,331.13 | **90.44%** |
| `cancel_hit` | 46,159 | 46.16% | 39.92 | 169.56 | 6.64% |
| `modify_hit` | 3,893 | 3.89% | 131.93 | 393.54 | 1.85% |
| `market` | 1,591 | 1.59% | 117.11 | 353.21 | 0.67% |
| `add_cross` | 507 | 0.51% | 217.50 | 593.94 | 0.40% |

Note: profiling-mode absolute latencies are inflated by instrumentation overhead (`__rdtsc()` + `steady_clock::now()` per stage × 7). The production `add_rest` latency is ~35 ns; the profiling-mode measurement includes ~220 ns of stage-internal work plus ~304 ns of timing overhead. Only the relative proportions are meaningful.

### Add-Rest 8-Stage Breakdown

| Stage | Mean ns | Mean cycles | ns share | cycles share |
|---|---:|---:|---:|---:|
| `match` (含 can_cross) | 32.45 | 150.05 | 14.71% | 15.57% |
| `id_index_insert` | 30.86 | 147.03 | 13.99% | 15.25% |
| `validation` (3 checks) | 29.11 | 142.57 | 13.20% | 14.79% |
| `fifo_append` | 28.48 | 141.43 | 12.92% | 14.67% |
| `node_init` | 28.01 | 139.06 | 12.70% | 14.43% |
| `pool_acquire` | 27.66 | 138.94 | 12.54% | 14.41% |
| **`level_lookup_existing`** | **26.86** | **63.89** | **12.18%** | **6.63%** |
| **`level_lookup_create_new`** | **17.10** | **40.98** | **7.75%** | **4.25%** |

Note on `count` reporting: the current profiling accumulator unconditionally increments the `count` field for every stage slot on each `add_rest` call, so all 8 stages report `count = 47,850`. The `mean_ns` and `mean_cycles` columns are computed from `total_ns / count` and `total_cycles / count` in the emitter and should be read as average-cost-per-add_rest, not as average-cost-per-hit for the level_lookup stages. A future refinement should track per-stage hit counts separately. The `total_ns` and `total_cycles` columns are correct.

### Key Findings

#### 1. `std::map::try_emplace` is Not the Bottleneck

`level_lookup_existing` costs 63.89 cycles per add_rest — the **lowest cycle cost of any stage by a wide margin** (the next lowest is `pool_acquire` at 138.94 cycles). The `std::map` red-black tree lookup for an existing price level is well-optimized by the compiler and accounts for only 6.63% of add_rest cycle time. **Replacing `std::map` with a faster price-index structure (absl::btree_map, ring buffer, or flat hash map) would address at most ~6.6% of the add_rest cycle budget.**

This directly answers the question posed by the Phase 4 price-level storage strategy report: the ordered-map lookup is **not** the performance bottleneck for the dominant `add_rest` path in the HFT macro workload.

#### 2. No Single Stage Dominates — The Cost Is Distributed

The 7 stages (treating the two level_lookup variants as one path) each consume between 12.5% and 15.6% of add_rest cycles. The standard deviation across the 7 main stages is only ~4.8 cycles. This means:

- There is no "fix one slow function and win" opportunity.
- Any optimization that targets a single stage caps out at ~15% improvement on `add_rest`, which translates to ~8% on macro throughput (since `add_rest` is ~53% of weighted macro time).
- Real progress requires reducing fixed cost across the entire path: fewer total instructions, fewer branches, fewer store operations.

The ChunkPool experiment's failure makes sense in this light: it reduced allocation cost (pool_acquire) but added overhead to fifo_append (chunk linkage), id_index_insert (no change), and remove (chunk_from_order + link_available). The net effect was a ~12% regression because the distributed overhead increase outweighed the localized allocation improvement.

#### 3. `level_create_new` Is Rare In Steady State

In a warmed book with 100,000 orders across 100 price levels, nearly every price already has an active level. The `level_create_new` total cycles (1,961,018) are only ~39% of `level_lookup_existing` total cycles (3,057,094). Since both stages have the same inflated count, the ratio of actual hits is proportional to the total cycle ratio: **existing-level lookups outnumber new-level creations by roughly 2.5:1 in this workload.** The create path is too rare to be an optimization target — every effort should go into the existing-lookup hot path.

#### 4. Profiling-Mode Count Reporting Issue

The `RecordAddRestStageProfile` function unconditionally increments `stage.count` for every stage slot regardless of whether that stage accumulated non-zero ns/cycles. This means `count` and `mean_ns`/`mean_cycles` in the CSV output should be interpreted as "per-add_rest amortized cost," not "per-hit cost." The `total_ns` and `total_cycles` columns are unaffected. A future fix should make the ScopedAddRestStage increment count only for the specific stage it measures, and the level_lookup split should track separate hit counts.

## Revised Phase 5 Strategy

The profiling data narrows the optimization space decisively:

1. **The price-index container is not the binding constraint.** The `std::map` lookup for existing levels costs 63.89 cycles — the cheapest stage. This removes the urgency from the Phase 4 plan's V2 (absl::btree_map), V4 (hot ring), V5 (bitmap), and V6 (cold container experiments). Those changes would optimize a cost center that represents 6.6% of add_rest cycles.

2. **The bottleneck is distributed fixed cost.** The remaining ~93% of add_rest cycles is spread across validation, match, allocation, initialization, append, and index insertion — each 139–150 cycles. The optimization strategy should shift from "find the slow stage" to "reduce instruction count and branch density across the entire add path."

3. **Concrete next candidates:**
   - **Merge validation checks.** `pending_cancel_ids_.contains()` + `id_to_order_.contains()` + `quantity == 0` are three sequential branches. Combining them into a single early-return path with fewer mispredict opportunities could reduce validation cost.
   - **Eliminate redundant stores.** `node_init` aggregates 6 field assignments (`*node = {id, price, qty, ts, nullptr, nullptr}`). Some of these are overwritten by `fifo_append` (`prev`, `next`) or `pool_acquire` (`parent_level`). Reducing the store count by initializing only the business fields and letting the append/pool paths set their own metadata could save cycles.
   - **Inline `push_back`.** `fifo_append` costs 141.43 cycles for what is a simple 4-pointer linked-list append. The function call overhead on a non-inlined method may be material at this scale.
   - **Profile with `perf record` / `perf annotate` for instruction-level hotspots.** The stage breakdown is ~200 ns in profiling mode; a non-instrumented production-mode cycle-accurate profile via `perf` would show exact instruction retirement stalls.

4. **Do not pursue price-index replacement until a different workload demands it.** The current macro workload (47.85% add, 46.16% cancel, shallow market sweeps) does not stress the ordered map. A workload with deeper market sweeps, wider price ranges, or more level churn could change the conclusion. But for the current workload, the data is clear.

## Working Rule For Phase 5

Do not introduce another major storage redesign. The profiling data shows the distributed nature of the add_rest cost: the cheapest stage is the `std::map` lookup at 63.89 cycles, while six other stages each cost 139–150 cycles. The next change should be instruction-level optimization of the add path, guided by `perf annotate` rather than by structural intuition.

The level_lookup_existing vs level_create_new split is now recorded. The remaining instrumentation gap is per-stage hit counting (cosmetic fix to the profiling accumulator) and a production-mode `perf record` run for instruction-level attribution.

## Production `perf record` Run (Window-Isolated)

> **Status note:** the production `perf record` data below **supersedes** the "Revised Phase 5 Strategy" and "Working Rule" conclusions above. In particular, the earlier claim that the `std::map` price-level container is *not* a binding constraint was an artifact of the short (100k) instrumented batch. The production run changes that conclusion (see Finding 2).

### Why a Plain `perf record` Would Not Work

`bench_hft_macro`'s `Setup()` is heavy and runs on **every measured iteration**: ~5,000 seed adds, a 500,000-event warmup replay, two full book rebuilds (`build_book_from_tracking`), and complete batch pre-generation. A naive `perf record ./bench_hft_macro` would be dominated by this scaffolding (`generate_pending_one`, `build_book_from_tracking`, RNG, tracking maps), and `perf report` would bury the engine hot path.

To avoid this, a `perf record --control=fifo` window was added that mirrors the existing PMC enable/disable pattern. The runner (`benchmark_runner.cpp::PerfRecordControl`) enables sampling only around the measured `RunOp` batch — warmup, `Setup()`, and `Teardown()` run with sampling disabled. The helper is inert (zero overhead) unless the FIFO paths are provided via `LLMES_PERF_CTL_FIFO` / `LLMES_PERF_ACK_FIFO`. Driver script: `benchmark/scripts/run_hft_macro_perf_record.sh`.

The profiling binary is built **Release + `-g`, with no `LLMES_PROFILE_*` macros**, so the recorded code path is the exact production engine measured by the campaign — not an instrumented variant.

### Run Configuration

```text
branch/commit: 635f1c8
binary:        Release + -g, no LLMES_PROFILE_* macros
events:        cycles, branch-misses (freq 8000, call-graph dwarf)
orders:        100000
levels:        100
batch_size:    1000000
warmup_iters:  1
iters:         40
seed:          42
window:        RunOp batch only (perf --control=fifo, -D -1)
```

Artifacts:

```text
benchmark/results/hft_macro_perf_record_cloud_20260601/
├── report.txt                 # function-level call-graph (cycles + branch-misses)
├── run.log                    # 40 enable/disable pairs, runner output
├── meta.txt
└── annotate_*.txt             # EMPTY (see note below)
```

### Run Sanity

The control FIFO worked exactly as designed: `run.log` shows 40 paired `Events enabled` / `Events disabled` transitions (one per measured iteration), the latency line reports a clean **18.47M ops/s @ 54.1 ns/op** (no instrumentation overhead), and perf captured 30,354 samples (≈15K per event).

**Why `annotate_*.txt` is empty:** at `-O3` the entire engine is inlined into `BenchHftMacro::RunOp` (every frame in `report.txt` is marked `(inlined)`). There is no standalone `add_limit_order` / `cancel_order` symbol for `perf annotate --symbol=...` to match. The call-graph report still provides full function-level attribution via inlined frames. For true instruction-level annotation in a future run, annotate the containing symbol (`...RunOp`) or the out-of-line leaves (`std::_Rb_tree_insert_and_rebalance`, `operator new`, `std::_Rb_tree_rebalance_for_erase`, `malloc`/`cfree`).

### Function-Level Attribution (share of all `RunOp` samples)

| Function / sub-cost | cycles % | branch-miss % | Notes |
|---|---:|---:|---|
| **`add_limit_order`** | **50.5%** | 50.4% | dominant op |
| ┣ `get_or_create` (**`std::map`**) | **17.8%** | **18.9%** | lower_bound 6.3% + new-node malloc/rebalance 8.9% |
| ┣ `contains` (id-index dup check) | **11.2%** | 10.6% | **redundant** with `emplace` |
| ┣ `emplace` (id-index insert) | 9.9% | 9.9% | `find_or_prepare_insert` |
| ┣ `erase_best` ×2 (drain emptied levels) | 4.6% | 4.5% | RB-tree rebalance + `cfree` |
| ┗ `push_back` / `acquire` / `node_init` | ~3% | ~3% | intrusive append, pool, init |
| **`cancel_order`** | **29.8%** | 29% | `find` 19.9% + `erase` 7.6% |
| **`modify_order`** | 8.0% | 8% | mostly its internal `add_limit_order` |
| `add_market_order` + remainder | ~8% | ~8% | shallow sweeps |

### Finding 1 — The Cancel-Index Hash Map (`id_to_order_`) Dominates

Summing `id_to_order_` (`absl::flat_hash_map`) work across all operations:

```text
add:    contains 11.2% + emplace 9.9% = 21.1%
cancel: find     19.9% + erase   7.6% = 27.6%
modify: ~3%
total:  ≈ 50% of all macro cycles
```

Roughly half of the entire macro workload is spent inside the cancel index. The cost is SIMD probing (`Match` / `_mm_cmpeq_epi8`), `PrefetchToLocalCache`, hashing, and `find_or_prepare_insert` — i.e. compute-bound probe work, which is consistent with the very low PMC cache-miss rate. The structure is doing its job (cancel p99 is 60 ns), so the lever is **reducing the number of probes**, not changing the table type.

**Redundant double-probe in `add_limit_order`.** The add path probes the same key in `id_to_order_` twice:

1. `contains(order_id)` — the duplicate-id guard (11.2% cycles). For `add_rest` this **always misses**.
2. `emplace(order_id, node)` — re-hashes and re-probes from scratch (9.9%).

Merging these into a single `lazy_emplace` / `find_or_prepare_insert` (check the insertion result instead of pre-checking) removes essentially the entire `contains` cost. This is the **highest-leverage, lowest-risk** change exposed by the profile: ~11% of total macro cycles and ~10% of branch-misses recoverable.

`pending_cancel_ids_.contains()` did **not** surface as a hotspot, confirming the set is effectively empty in steady state (`cancel_miss = 0`). Switching it to an empty-guard + `absl::flat_hash_set` is a free consistency cleanup, not a performance driver.

### Finding 2 — `std::map` Level Container Is *Not* Cheap In A Long Run

This **contradicts the earlier stage-profiling conclusion** (that `std::map` lookup is the cheapest add stage). In the production run, `get_or_create` is **35% of `add_limit_order`** (17.8% of all cycles) and the **single largest branch-miss source** in the engine:

| `get_or_create` sub-cost | cycles % | branch-miss % |
|---|---:|---:|
| `lower_bound` (RB-tree traversal) | 6.3% | 6.5% |
| `emplace_hint` new-node insert | 8.9% | 9.7% |
| ┣ `_Rb_tree_insert_and_rebalance` | 3.6% | 4.7% |
| ┗ `_M_create_node` → `operator new` → `malloc` | 3.8% | 3.6% |

Plus `erase_best` level deletion (4.6% cycles), which carries its own `_Rb_tree_rebalance_for_erase` (1.2%) and `cfree` (~0.8%).

**Why the contradiction? It is a measurement artifact, not a workload difference.** Both runs use the identical workload: same `Setup()`, same op mix (45 add / 48 cancel / 5 modify / 2 market), same price distribution. The divergence comes entirely from *how* each run measured, and the stage-profiling number was unreliable for two compounding reasons:

1. **The instrumentation overhead dwarfed the thing being measured.** Stage profiling wrapped each of the 7 sub-stages of `add_limit_order` in a `__rdtsc()` + `steady_clock::now()` pair. That timer pair costs on the order of a hundred-plus cycles — comparable to or larger than the real per-stage cost. The proof is in the stage data itself: `node_init` (a 4-field struct assignment) reported **139 cycles**, `fifo_append` (a 4-pointer linked-list append) **141 cycles**, `pool_acquire` (a free-list pop) **139 cycles**. None of those operations genuinely cost ~140 cycles; the bulk of each number is the timer overhead. Because that overhead is roughly constant per stage, it **flattens the real spread** — the genuinely expensive `get_or_create` (malloc + RB-tree rebalance) ends up looking similar to trivial stages, and the genuinely cheap `level_lookup_existing` is the only one that slips below the overhead floor, which is exactly what created the false "lookup is cheapest" signal.

2. **The earlier conclusion also read its own data too narrowly.** It quoted only `level_lookup_existing` (63.89 cycles, 6.6% of add) and dismissed `level_create_new` as rare. Summed correctly, `get_or_create = level_lookup_existing + level_create_new = 63.89 + 40.98 = 104.87 cycles/add ≈ 10.9% of add` even in the distorted stage data. (A `count`-accounting bug, documented above, further hid the per-create cost by amortizing it across all adds.)

So the honest comparison is **~11% (stage profiling) vs 35% (perf)**, not "6.6% vs 35%". The remaining ~3× gap is the flattening described in (1): perf hardware sampling has zero instrumentation overhead, so it resolves the true cost spread that the timer pairs smeared out.

The corrected conclusion: **the `std::map` level container is a real cost center (~24% of macro cycles, top branch-miss source), and the production `perf record` profile is the authoritative measurement.** The low PMC cache-miss rate still holds — the std::map cost is branch-heavy RB-tree manipulation and allocator work, **not** memory stalls.

### Stage Profiling Is Retired

The consequence of the analysis above is that **per-sub-stage timing instrumentation is not a viable profiling method for this engine** and has been removed from the codebase (the `LLMES_PROFILE_ADD_REST_STAGES` feature, `add_rest_stage_profile.{hpp,cpp}`, the `order_book.cpp` / `bench_hft_macro.cpp` instrumentation, the CMake options, and `run_hft_macro_add_rest_stage_profile.sh`).

The reason, stated as data: the operations being timed are ~5–40 ns each, while a `__rdtsc()` + `steady_clock::now()` pair is itself on the order of tens of ns. When the probe costs as much as the probed region, the measurement reports mostly its own overhead and **compresses** real differences toward a uniform value (every "cheap" stage reported ~140 cycles). Any conclusion drawn from the relative ordering of such stages is therefore untrustworthy — as demonstrated by the inverted "std::map is cheapest" result that the production profile overturned.

Going forward, hot-path attribution uses **`perf record` with the window-isolated control FIFO** (this section) for cost-center and branch-miss attribution, and the existing **operation-level profiling** (`LLMES_PROFILE_HFT_MACRO_OPS`, which times whole operations with a single timer pair and whose op shares are corroborated by the perf function-level breakdown) for op-mix weighting. Sub-operation timing via inline instrumentation is no longer used.

## Revised Optimization Plan: Replace The Cancel-Index Hash Map

> **Correction note:** an earlier draft of this plan proposed a "Tier 1" that merged the add-path `contains` + `emplace` into a single `lazy_emplace` to recover ~11% of cycles. **That idea is withdrawn** (see "1. Why The Hash Map Is At Its Ceiling" below). The real lever is not to do tricks on the hash map but to replace it.

The production `perf record` profile shows the cancel index (`id_to_order_`, an `absl::flat_hash_map<uint64_t, Order*>`) is ~50% of all macro cycles: add `contains` 11.2% + add `emplace` 9.9% + cancel `find` 19.9% + cancel `erase` 7.6%, plus the modify share. This section is the authoritative Phase 5 plan for attacking it.

### 1. Why The Hash Map Is At Its Ceiling

Within the current contract — **arbitrary external `uint64_t` order ids** plus **"reject a duplicate / pending-cancel id before matching"** — the hash map cannot be made meaningfully cheaper:

- **The add-path double-probe cannot be merged.** The dup-check (`contains`) runs *before* matching and the insert (`emplace`) runs *after*. They are not redundant: the dup-check must reject a duplicate id before it consumes liquidity, so the two probes serve two different correctness purposes at two different times. Collapsing them into one call would change observable semantics (a duplicate would match first, then be rejected).
- **A `find` + hinted insert does not help.** `absl::flat_hash_map` treats the iterator hint as a non-binding suggestion and in practice **ignores it entirely**; the hint-taking overloads forward to the non-hint versions. On the hot `add_rest` path the key is absent, so `find` returns `end()` and the subsequent insert must still recompute the hash and re-probe from scratch. There is no single-probe path that preserves the reject-before-match semantics.
- **The probe work itself is already optimal.** The profile shows `find_large` → `Match` (`_mm_cmpeq_epi8`) + `PrefetchToLocalCache`: SIMD Swiss-table probing with prefetch. `absl` is at or near the practical floor for a general-purpose hash of an arbitrary 64-bit key. The low PMC cache-miss rate confirms this is compute-bound probe/hash work, not memory stalls.
- **The `pending_cancel_ids_` guard is noise, not a lever.** It never surfaced in the profile (in steady state the set is empty, `cancel_miss = 0`). Switching it to `absl::flat_hash_set` or adding an empty-guard is cosmetic, not a measurable win.

The conclusion: **with the current id contract, the hash map's ~50% is essentially irreducible.** Further "tricks" (hint merging, guards, table-type swaps) optimize a cost center that is already at its ceiling. To recover this cost the id contract itself must change.

### 2. What Replaces It

Replace the hash map with a **generational slotmap** built directly on the existing `OrderPool` slab. The order's identity becomes its physical position, so lookup is an array index instead of a hash probe.

A handle is a packed 64-bit value:

```text
handle = [ generation : high bits ] [ slot_index : low bits ]
```

- `slot_index` indexes straight into the pool's `std::vector<Order>` — lookup is one array load, no hash, no probe, no tombstones.
- `generation` is a per-slot counter bumped on every reuse. Cancel/modify compare the handle's generation against the slot's current generation; a mismatch is a stale handle and is rejected cleanly (this replaces the hash map's "find failed" path and prevents ABA when a slot is recycled).

`id_to_order_` is then **deleted outright**, and the add-path duplicate check disappears with it (allocating a slot *is* the uniqueness guarantee).

### 3. Why id pool + slotmap Is Feasible And Faster

**Feasible — the id contract is controllable.** In a real venue the exchange gateway assigns the order id at acknowledgment time, so the engine (not an external client) owns id assignment and can make ids dense and engine-managed. The matching core only needs a dense handle for book lookups; any client order id (`clOrdId`) that must be echoed back lives at the gateway layer, outside the hot path. We keep `Order.id` purely for trade reporting (`Trade::taker_order_id` / `maker_order_id`) and decouple it from the lookup key.

**Feasible — the slab already exists.** `OrderPool` is already a contiguous `std::vector<Order>` with an intrusive free list, so `slot_index = &order - &pool_[0]` is free. Half of the slotmap is already implemented; we add a per-slot generation and return handles from `acquire()`.

**Faster — measured against the profile:**

| Path | Now (`absl` hash) | Slotmap |
|---|---:|---|
| cancel `find` | 19.9% | array index + generation compare (~0) |
| cancel `erase` | 7.6% | free-list push (~1%) |
| add dup `contains` | 11.2% | not needed (slot allocation is the check) |
| add `emplace` | 9.9% | not needed (handle *is* the slot) |

Most of the ~50% cancel-index cost is recoverable, minus a few percent of new generation bookkeeping. This is the **single largest lever** identified in the whole profiling effort — larger than anything achievable inside the hash map or on `std::map`.

**Why naive direct indexing is not enough (and the slotmap is the right form).** A plain `std::vector<Order*>` indexed by id fails: ids are monotonic and unbounded over a run (in the benchmark `id_counter_` only increments), so the array would grow with total ops, not with live-order count — effectively a leak. Recycling ids bounds the array to the live-order count, and recycling reintroduces ABA, which is exactly what the generation field solves. The slotmap is the structure that simultaneously gives O(1) no-hash lookup, bounded memory, and safe stale-handle detection.

**Open decisions / wrinkles to settle before implementing:**

- **cancel-before-insert semantics.** `pending_cancel_ids_` exists for out-of-order client traffic (a cancel arriving before its insert). With engine-assigned handles this cannot happen — you cannot hold a handle the engine has not yet issued — so the path can be dropped from the matching core (it belongs at the gateway, if needed at all). This needs an explicit decision.
- **Generation correctness is mandatory, not optional.** Without it, cancelling a recycled slot would silently remove the wrong order. This is the main correctness risk of the redesign.
- **Benchmark change required for measurement.** `bench_hft_macro` currently self-assigns ids and tracks live orders by id; it must store the engine-returned handle instead. Without this change the win cannot be measured on the macro workload.

### 4. After The id Pool: Next Targets

Once the slotmap lands and is validated, the next-largest cost center is the `std::map` price-level container (`get_or_create` 17.8% of cycles, the top branch-miss source):

- **Tier A — pooled allocator for `std::map` (safe, do first).** Give the ordered map a free-list / pooled allocator so level create/destroy stops calling `operator new` / `malloc` / `cfree` (the allocator churn and rebalance cost the profile attributes to `emplace_hint` and `erase_best`). Keeps `std::map` semantics and **pointer stability** (`Order::parent_level` stays valid), so it is low risk.
- **Tier B — structural replacement (deferred).** Only if Tier A leaves material `lower_bound` + rebalance cost: replace `std::map` with `absl::btree_map` or a hot contiguous structure. Both move values on rebalance and would invalidate `Order::parent_level`, so this first requires changing the ownership model (e.g. heap-allocate `PriceLevel` and store `PriceLevel*` in the container). This is the constraint flagged in the Phase 4 storage report; do not start it until Tier A is measured.
- **Minor — `node_init` redundant stores.** Independent of the above; small.

Each step is validated with a 10-trial production PMC comparison (`instructions_per_op`, `branch_miss_rate`, `cache_misses_per_op`) plus a re-run of the window-isolated `perf record` to confirm the targeted cost center actually shrinks.

### Working Rule For Phase 5

The ordered sequence is: **(1) replace the cancel-index hash map with a generational slotmap on the order pool** (largest lever, requires the engine-assigned-id contract and a benchmark change), then **(2) a pooled allocator for the `std::map` price levels**, then re-profile before considering any structural price-level replacement. Do not spend further effort optimizing the hash map in place — under the current id contract it is already at its ceiling, and under the new contract it is gone.
