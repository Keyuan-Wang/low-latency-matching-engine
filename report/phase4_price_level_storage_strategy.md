# Phase 4: Price-Level Storage Strategy

## Overview

Phase 4 changes the matching engine's **price-level storage**. The current
high-level goal is to move beyond the baseline:

```cpp
std::map<price, PriceLevel>
```

without losing correctness under the Phase 3 HFT benchmark suite.

The main design constraint is that `hft_macro` does not have a fixed price
range. Micro benchmarks use predictable prices around `1000`, but the macro
benchmark generates prices relative to a drifting best ask and uses modify
events that can move resting orders by `+/- 1..3` ticks. Therefore a pure
fixed-size `std::vector<PriceLevel>` is not a correct final design unless the
benchmark itself clamps prices, which would change the workload.

This phase should proceed as an incremental performance-engineering campaign:
one structural change per version, one benchmark pass per version, and a clear
decision gate before adding the next layer.

---

## Current State

The matching engine already has the important Phase 2/3 primitives:

| Component | Current role |
|---|---|
| `OrderPool` | Pre-allocated order node storage |
| Intrusive per-level queue | FIFO order priority without per-node list allocation |
| `absl::flat_hash_map<id, Order*>` | O(1) average cancel/modify index |
| `std::map<price, level>` | Ordered price-level storage and best-price discovery |
| HFT benchmark suite | Micro and macro workloads with realistic spatial locality |

The next bottleneck is the price-level container:

- `std::map` gives correct ordering and O(1) `begin()` best access.
- It pays pointer-chasing and red-black-tree maintenance costs.
- Most HFT operations hit prices near the inside quote, so a contiguous hot
  structure should be faster if correctness for drifting prices is preserved.

---

## Price-Range Findings From HFT Benchmarks

### Micro Benchmarks

All HFT micro benchmarks prefill a sell-side book through:

```cpp
PrefillHftBook(book, orders, levels, 1000, ...)
```

This creates ask levels:

```text
[1000, 1000 + levels - 1]
```

With the default benchmark matrix:

| `levels` | Prefilled ask range |
|---:|---|
| 10 | `[1000, 1009]` |
| 100 | `[1000, 1099]` |
| 1000 | `[1000, 1999]` |

Additional micro benchmark prices:

| Scenario | Runtime prices | Best drift |
|---|---|---|
| `hft_add_near` | buy `998..999` | best ask does not drift |
| `hft_add_far` | buy `950..990` | best ask does not drift |
| `hft_modify_near` | buy `998..999` | best ask can move if old best is emptied |
| `hft_cancel_hot` | existing ask hot zone | best ask can move if old best is emptied |
| `hft_cancel_cold` | existing ask cold zone | best ask usually unchanged |
| `hft_market_small` | consumes ask levels | best ask moves up |
| `hft_market_large` | consumes ask levels | best ask moves up more |
| `hft_cxl_miss` | no price change | no best drift |

Micro benchmarks can be handled by a fixed price window. They are not enough to
justify correctness for the macro benchmark.

### Macro Benchmark

`hft_macro` has no hard upper bound:

```cpp
ref = best_ask;
buy_price  = max(1, ref - offset - 1);  // offset in [1, 100]
sell_price = ref + offset;              // offset in [1, 100]
```

Modify events also move old prices without a clamp:

```cpp
new_price = old_price +/- delta;         // delta in [1, 3]
```

The macro benchmark therefore requires price-level storage that can represent
arbitrary prices. Any final design must retain a cold path for prices outside
the hot region.

---

## Design Principle

Do not jump directly from `std::map` to a complex hybrid structure.

The correct Phase 4 workflow is:

1. Establish a clean `std::map` baseline.
2. Refactor the price-book interface without changing behavior.
3. Replace the ordered container with a cache-friendlier ordered container.
4. Add telemetry to quantify actual price locality and drift.
5. Add a hot contiguous structure only after the baseline data justifies it.
6. Benchmark every version with the same matrix and record the result.

This avoids a common performance-engineering failure mode: implementing several
optimizations at once and then being unable to explain the result.

---

## Version Plan

### V0: Restore `std::map` Baseline

**Goal**: return to a buildable, testable reference implementation.

Structure:

```cpp
using AskBook = std::map<std::int64_t, PriceLevel, std::less<>>;
using BidBook = std::map<std::int64_t, PriceLevel, std::greater<>>;
```

Acceptance criteria:

- `cmake` configures successfully.
- `matching_core_tests` pass.
- `benchmark_smoke_test` passes.
- HFT canary benchmarks run with `VERSION_TAG=v0_map_baseline`.

This version is the reference for all later changes.

### V1: Introduce `SideBook`, Still Backed by `std::map`

**Goal**: isolate price-level storage behind a small API while preserving the
same behavior and the same container.

Sketch:

```cpp
template <bool IsAsk>
class SideBook {
public:
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::int64_t best_price() const;
    [[nodiscard]] PriceLevel* best_level();

    PriceLevel& get_or_create(std::int64_t price);
    PriceLevel* find(std::int64_t price);
    void erase_if_empty(std::int64_t price);

private:
    using Comparator = std::conditional_t<IsAsk, std::less<std::int64_t>,
                                          std::greater<std::int64_t>>;
    std::map<std::int64_t, PriceLevel, Comparator> levels_;
};
```

Expected benchmark result:

- Throughput should be close to V0.
- Any material regression means the abstraction introduced overhead or changed
  behavior and must be investigated before moving on.

Decision gate:

- Continue only if V1 is functionally identical and benchmark-neutral.

### V2: Replace `std::map` With `absl::btree_map`

**Goal**: test a low-risk ordered-container replacement.

`absl::btree_map` preserves the key property needed by matching:

```cpp
best price == levels_.begin()->first
```

but stores keys and values in cache-friendlier B-tree nodes than `std::map`.

Expected wins:

- Lower pointer-chasing cost.
- Better cache behavior for market sweeps and best-level churn.
- Lower memory overhead per price level.

Risks:

- Moving `PriceLevel` values must be safe. Intrusive lists are movable, but
  order nodes must not rely on stable addresses of the container's value.
- If `Order` stores `parent_level*`, verify that no container operation moves
  a live `PriceLevel` after orders have been inserted. The safer long-term
  direction is to remove `parent_level*` and locate the level by
  `(side, price)` during cancel.

Decision gate:

- If V2 improves or is neutral, keep it as the ordered cold-path baseline.
- If V2 regresses, keep V1 and move to telemetry before adding hot structures.

### V3: Add Price-Locality Telemetry

**Goal**: measure the actual distribution needed to size the hot structure.

This version should not change matching behavior. It should report:

| Metric | Purpose |
|---|---|
| min/max order price seen | Validate total price drift |
| min/max best ask and best bid | Measure inside-price drift |
| distance from best histogram | Choose hot window size |
| hot-zone hit rate for N in {32,64,128,256,512,1024} | Estimate value of hot ring |
| cold promotion/eviction count | Estimate churn |

Suggested debug-only counters:

```cpp
struct PriceTelemetry {
    std::int64_t min_price = std::numeric_limits<std::int64_t>::max();
    std::int64_t max_price = std::numeric_limits<std::int64_t>::min();
    std::int64_t min_best_ask = std::numeric_limits<std::int64_t>::max();
    std::int64_t max_best_ask = std::numeric_limits<std::int64_t>::min();
    std::array<std::uint64_t, 1025> distance_hist{};
};
```

Decision gate:

- Choose `HotSize` from measured hit rate, not intuition.
- Do not introduce a hot ring unless the data shows a strong near-best
  concentration under `hft_macro`.

### V4: Hot Ring + Ordered Cold Map

**Goal**: first real price-level data-structure change.

Keep correctness simple:

- Hot path: fixed-size ring or vector window near the current best.
- Cold path: ordered map (`std::map` or `absl::btree_map`) for all other prices.

Recommended invariant:

```text
ask hot window: [best_ask, best_ask + HotSize)
bid hot window: (best_bid - HotSize, best_bid]
```

This is single-sided in the depth direction. A symmetric `best +/- N` window is
less precise for an order book because asks only need increasing depth from the
best ask and bids only need decreasing depth from the best bid.

Important correctness rule:

```text
Any price must be representable either in hot or cold.
```

No order should be rejected or clamped merely because it falls outside the hot
window.

Implementation note:

- On best movement, run `recenter_hot_window(new_best)`.
- Evict non-empty hot slots outside the new window to cold.
- Promote cold levels that now fall inside the new hot window.
- Keep cold ordered so the next best can be found without scanning.

Decision gate:

- V4 must pass all correctness tests before any bitmap or cold-hash work.
- The first performance target is lower average latency in `hft_add_near`,
  `hft_cancel_hot`, and `hft_macro` without p99 spikes in market sweeps.

### V5: Add Hot Bitmap

**Goal**: optimize best-level discovery inside the hot ring.

Instead of linearly scanning hot slots after a best level is emptied, maintain
occupancy bits:

```cpp
std::array<std::uint64_t, HotSize / 64> occupied_bits_;
```

Expected wins:

- Faster market sweeps.
- Lower p95/p99 when repeated cancels or market orders empty best levels.

Decision gate:

- Keep this version only if it improves market and macro tail latency.
- If average improves but p99 regresses, inspect recentering and bit operations.

### V6: Cold Container Experiments

**Goal**: optimize the cold path only after the hot structure is correct.

Candidates:

| Design | Exact price lookup | Cold best lookup | Complexity |
|---|---:|---:|---|
| `std::map` | O(log C) | O(1) via `begin()` | low |
| `absl::btree_map` | O(log C) | O(1) via `begin()` | low-medium |
| `absl::flat_hash_map + btree_set` | O(1) avg | O(1) via set begin | medium |
| `absl::flat_hash_map + heap` | O(1) avg | amortized O(log C) | medium-high |

Do not start here. Cold-path container experiments are only useful if telemetry
and V4/V5 benchmark data show cold activity is material.

---

## Benchmark Methodology

Every version must use the same benchmark matrix.

### Build and Test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DLLMES_BUILD_BENCHMARKS=ON
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

### HFT Micro Canary

```bash
SCENARIOS=hft_add_near,hft_add_far,hft_cancel_hot,hft_cancel_cold,hft_modify_near,hft_market_small,hft_market_large \
METRICS=latency TRIALS=3 ITERS=1000 WARMUP_ITERS=100 VERSION_TAG=vX \
bash benchmark/scripts/run_benchmarks.sh
```

### HFT Macro Canary

```bash
SCENARIOS=hft_macro METRICS=latency ORDERS=100000 LEVELS=100 \
BATCH_SIZES=100000 TRIALS=3 ITERS=1 WARMUP_ITERS=0 VERSION_TAG=vX \
bash benchmark/scripts/run_benchmarks.sh
```

### Merge Results

```bash
python3 benchmark/scripts/merge_benchmark_metrics.py
```

### Metrics to Track

| Metric | Why it matters |
|---|---|
| `ops_s` | headline throughput |
| `avg_ns` | steady-state average cost |
| `p95_ns`, `p99_ns` | tail latency and recentering spikes |
| `cache_misses_per_op` | memory behavior |
| `instructions_per_op` | algorithmic overhead |
| `cpi` | stall behavior |

PMC metrics should be collected on the remote benchmark box when available.
Local latency-only runs are acceptable for fast iteration, but not for final
phase conclusions.

---

## Correctness Requirements

The following invariants must hold for every Phase 4 version:

1. Price-time priority is preserved at each price level.
2. `cancel_order(id)` removes exactly one resting order if the id exists.
3. `modify_order(id, ...)` behaves as cancel plus add with the existing
   project semantics.
4. Market orders never rest remainder on the book.
5. Pending-cancel and duplicate-id behavior stays unchanged.
6. Arbitrary integer prices are representable.
7. The macro benchmark must not require clamping or rejecting valid prices.

Recommended long-term cleanup:

```text
Remove Order::parent_level.
```

The safer cancel path is:

```text
id_to_order_[id] -> Order*
Order has side and price
SideBook::find(price) -> PriceLevel*
PriceLevel::erase(order)
```

This avoids pointer-stability issues when price levels move between hot and
cold storage.

---

## What Not To Do Yet

Avoid these changes in the first implementation pass:

- Do not replace cold storage with only `absl::flat_hash_map`.
- Do not clamp `hft_macro` prices to fit a fixed vector.
- Do not add hot ring, bitmap, and cold hash indexing in one version.
- Do not judge a design only on micro benchmarks; `hft_macro` is the final
  workload.
- Do not keep `parent_level*` if price levels can move between containers.
- Do not assume a storage-locality change will improve macro throughput without
  first profiling the actual operation mix. The ChunkPool experiment (see below)
  is the canonical example: a plausible hypothesis, a benchmark campaign, a bug
  that invalidated the early result, and a corrected measurement that showed the
  simpler design was faster.

---

## Experiment: ChunkPool (April–June 2026)

### Hypothesis

The ChunkPool design was motivated by a cache locality hypothesis:

- A single large order pool scatters orders from the same hot price level across
  separate memory pages.
- Grouping orders into smaller per-level chunks should keep same-price-level
  orders contiguous in cache, reducing cache misses on cancellation and market
  sweeps.
- Different `kChunkSize` values might expose a better locality/overhead
  trade-off.

### Architecture

The ChunkPool design (branch `phase4a`, later `phase4-finale`) replaces the
single `OrderPool` with:

```
OrderPool (master)                  ChunkPool (phase4-finale)
┌─────────────────────┐            ┌──────┐ ┌──────┐ ┌──────┐
│ orders_[0..N]       │            │Chunk1│ │Chunk2│ │Chunk3│ ...
│ (single allocation) │            │256   │ │256   │ │256   │
│                     │            │orders│ │orders│ │orders│
│ PriceLevel per      │            └──┬───┘ └──┬───┘ └──┬───┘
│   price → fifo list │              │        │        │
└─────────────────────┘              ▼        ▼        ▼
                              PriceLevel A  PriceLevel A
                                (hot!)      (hot, more orders)
```

Key structural changes:

- `ChunkPool`: owns a fixed array of `Chunk[]`. Each chunk holds `kChunkSize`
  contiguous `Slot{Order + next_free}` elements. Provides `acquire_empty_chunk()`
  and `release_empty_chunk()`.
- `Chunk`: per-chunk free-slot stack (`allocate_order()` / `release_order()`).
  Maintains `link_available()` / `unlink_available()` for per-PriceLevel
  available-chunk list.
- `PriceLevel`: instead of a single fist pointer, manages a local linked list of
  chunks (`available_head_`). `allocate()` pops from the head chunk; `remove()`
  counts freed slots and returns empty chunks to `ChunkPool`.
- `chunk_from_order()`: O(1) owner-chunk lookup via byte-offset arithmetic on
  `Order*`, used in both `PriceLevel::remove()` and `ChunkPool::release_empty_chunk()`.

The `ChunkPool` constructor with `max_active_levels` accounts for fragmentation:
each active price level gets one chunk before the remaining capacity is divided
into full-chunk units.

```cpp
// chunk_count_for (two-arg overload)
return active_levels + (order_capacity - active_levels) / kChunkSize;
```

A `PriceLevel` only owns chunks while they contain live orders. Full chunks are
unlinked from `available_head_` (but retained by the level). Empty chunks are
returned to `ChunkPool`'s global free list. This means a hot price level with
many orders keeps many chunks allocated, while cold levels with few orders hold
only a single partially-filled chunk.

### Initial Benchmark Campaign (Bug-Affected)

A 10-trial, 8-micro + 1-macro campaign was run comparing `master` (with
ChunkPool at kChunkSize=16/32/64/128/256) against `phase4a` (the `PriceLevel`
wrapped behind `SideBook`, no ChunkPool).

Artifacts: `benchmark/results/campaign_20260601_1319/`

The initial per-scenario summary showed phase4a as the apparent winner:

| Scenario | Best chunk result | phase4a |
|---|---:|---:|
| `hft_add_near` | 21,438,083 | 23,863,634 |
| `hft_cancel_hot` | 10,706,453 | 13,167,589 |
| `hft_macro` | 16,075,480 | 15,958,480 |

### Bug Discovery: Cancel-Miss Accounting Drift

Per-operation profiling (`LLMES_PROFILE_HFT_MACRO_OPS`) exposed the problem. The
`hft_macro` benchmark's Setup() was generating `PendingOp` batches from a
**predictive tracking map** that could drift from actual book state:

```text
Observed operation share (bug-affected, 1 trial):
  add_rest:     48.30%
  cancel_miss:  43.69%   ← should be near 0%
  cancel_hit:    1.32%   ← should be ~46%
  modify_hit:    0.09%
  market:        1.85%
```

The root cause: market-order maker fills, crossing limit-order fills, and
modify outcomes were not reflected in the tracking map during batch
pre-generation. The map thought orders existed that had already been consumed.
This produced a massive false `cancel_miss` rate that distorted the benchmark's
operation mix away from the intended 45/48/5/2 distribution, making the
measured workload unrepresentative of real HFT flow.

### Fix: Planning-Book Replay Model

The fix (commit `7039990`, from branch `phase4-finale`) replaced the predictive
batch generation with a **dual-book planning-replay model**:

```text
Setup():
  1. Warmup: populate book_ and tracking map together
  2. build_book_from_tracking() → book_ and planning_book_ (identical copies)
  3. For each PendingOp:
     a. Execute on planning_book_ first (untimed)
     b. Update tracking from real AddResult / Trade outputs
     c. Validate cancel/modify targets against live book state
  4. planning_book_ destructed (no longer needed)
  
RunOp():
  Execute the same PendingOp on book_ (timed, no RNG, no map lookups)
```

The planning book captures all side effects (market sweeps, crossing fills,
modify rest/cross outcomes, cluster-cancel success/failure) so the tracking
map stays in sync with what `book_` will see during timed execution.

### Corrected Benchmark Result

A focused campaign comparing the repaired `master` (OrderPool) against
`phase4-finale` (ChunkPool, kChunkSize=16/32/64/128/256), hft_macro only,
1 trial each:

**Artifacts:** `benchmark/results/macro_master_vs_phase4_finale_chunkSweep_20260601/`

| Version | Architecture | ops/s | avg_ns | vs master |
|---|---|---|---|---|
| **master** | OrderPool | **28.1M** | 35.6 | — |
| phase4-finale chunk16 | ChunkPool | 24.3M | 41.2 | −13.6% |
| phase4-finale chunk32 | ChunkPool | 24.6M | 40.7 | −12.5% |
| phase4-finale chunk64 | ChunkPool | 24.4M | 40.9 | −13.0% |
| phase4-finale chunk128 | ChunkPool | 24.7M | 40.5 | −12.1% |
| phase4-finale chunk256 | ChunkPool | 24.7M | 40.5 | −12.0% |

**Conclusions:**

1. **OrderPool outperforms ChunkPool by 12–14%** on the repaired hft_macro
   benchmark. The version we previously thought was regressing (master/OrderPool)
   is actually the faster design.

2. **kChunkSize is irrelevant for macro throughput.** The five chunk-size
   variants cluster within 1.8% of each other (24.3M–24.7M ops/s), close to
   measurement noise at 1 trial. The chunk management overhead dominates any
   locality benefit.

3. **The cache-locality hypothesis is falsified for this workload.** The repaired
   profiling shows that `add_rest` (52.98% of weighted macro time) and
   `cancel_hit` (35.44%) dominate the measured cost. Neither operation benefits
   from per-level order clustering:
   - `add_rest` pays chunk allocation/linking on every insertion.
   - `cancel_hit` already has O(1) hash-table lookup + intrusive unlink; chunk
     slot recovery (`chunk_from_order`, `link_available`) adds extra operations.

4. **The earlier campaign (campaign_20260601_1319) was invalidated by the
   cancel-miss bug.** The ~43% false miss rate in the batch stream made the
   measured workload unrepresentative of the intended HFT mix. The corrected
   benchmark is the authoritative reference.

### Why ChunkPool Regresses

Per-operation profiling of the repaired `phase4-finale` macro workload shows the
dominant cost center is `add_rest`, contributing 52.98% of total measured time.
The `add_rest` path in ChunkPool executes:

1. `SideBook::get_or_create(price)` — `std::map` node lookup/creation
2. `PriceLevel::allocate()` — chunk list walk + possibly `acquire_empty_chunk()`
3. Aggregate assignment to Order fields
4. `PriceLevel::push_back()` — intrusive list append
5. `id_to_order_.emplace()` — Swiss-table insert

In the OrderPool design, step 2 is a simple pop from a single pre-allocated
array with no chunk linkage overhead. Steps 1, 3, 4, 5 are identical.

The `cancel_hit` path (35.44% weighted time) in ChunkPool executes:

1. `id_to_order_.find()` — Swiss-table lookup
2. `PriceLevel::remove()` — intrusive unlink
3. `ChunkPool::chunk_from_order()` — byte-offset arithmetic
4. `Chunk::release_order()` — free-slot stack push
5. Conditionally: `link_available()` (if chunk was full) or `unlink_available()`
   + `release_empty_chunk()` (if chunk is now empty)

In OrderPool, steps 3–5 are replaced by a direct pool return. The extra pointer
operations and conditional branches add measurable overhead on the
second-most-frequent operation.

The structural cost is that ChunkPool trades **simpler allocation for better
locality that the workload does not need.** The macro benchmark's operation mix
(high add, high cancel, low market sweep) means order lifetimes are short and
same-level traversal is rare — the scenario where chunk locality would pay off
simply does not occur.

---

## Current Baseline: OrderPool (master)

Based on the corrected benchmark results, the current `master` branch is the
correct Phase 4 starting point:

| Component | Implementation |
|---|---|
| Order storage | `OrderPool` — single contiguous allocation, O(1) acquire/release |
| Per-level queue | Intrusive doubly-linked list (no separate node allocation) |
| Cancel/modify index | `absl::flat_hash_map<uint64_t, Order*>` |
| Price-level storage | `std::map<int64_t, PriceLevel>` (wrapped in `SideBook`) |
| `hft_macro` ops/s | **28.1M** (1 trial, orders=100000, levels=100) |

The main structural simplification in this version is that `OrderPool` uses a
single pre-allocated array with a free-list stack. Each `PriceLevel` gets a
simple intrusive FIFO queue. The pool does not need per-level chunk tracking,
`chunk_from_order()` byte-offset reverse-lookups, or empty-chunk return logic.

This is the reference for all future Phase 4 changes.

### What the Data Tells Us About Optimization Priority

The repaired per-operation profiling (`report/phase4_hft_macro_optimization_priority.md`)
establishes the optimization leverage for each operation type:

| Operation | Weighted time share | Mean latency | Optimization priority |
|---|---|---|---|
| `add_rest` | 52.98% | 69.85 ns | **Highest** — high frequency, quote-placement path |
| `cancel_hit` | 35.44% | 48.46 ns | Medium — already fast and tight-tailed (p99=70ns) |
| `modify_hit` | 8.16% | 131.36 ns | Medium-low — expensive but low frequency (3.9%) |
| `market` | 2.86% | 113.42 ns | Low — rare (1.6%), shallow sweeps (p99 levels=1) |

The `add_rest` dominance means Phase 4 should prioritize the insertion path
over cache-locality work for cancellation or market sweeps. The fixed costs in
`add_rest` include:

1. `pending_cancel_ids_.contains()` — hash-set lookup
2. `id_to_order_.contains()` — Swiss-table lookup (duplicate check)
3. crossing check against opposite-side best level
4. `SideBook::get_or_create(price)` — `std::map` tree lookup/insertion
5. `PriceLevel::allocate()` — pool free-list pop
6. `PriceLevel::push_back()` — intrusive list append
7. `id_to_order_.emplace()` — Swiss-table insert

Neither the profiling data nor the ChunkPool experiment point toward
same-level order locality as the binding constraint. The next optimization
candidates are in the **price-level lookup** (step 4) and the **duplicate/pending
check sequence** (steps 1–2), not in order storage layout.

---

## Recommended Next Step

With the ChunkPool experiment recorded and the OrderPool baseline established,
the next Phase 4 work should:

1. **Run the price-locality telemetry described in V3.** The profiling shows
   `add_rest` dominates weighted time, and `add_rest` always involves a
   `get_or_create(price)` call. Quantify how often that call hits an existing
   level (cheap) versus creates a new `std::map` node (expensive).

2. **Profile the `add_rest` sub-path costs.** Distinguish between:
   - duplicate + pending-cancel checks (hash-table lookups)
   - crossing check (reads best opposite level, usually no match)
   - price-level lookup or creation (the `std::map` cost)
   - order allocation and queue append
   - `id_to_order_` insertion

3. **Consider moving the hash-table checks after the crossing check.** If
   duplicate and pending-cancel checks are cheap per-operation but run on every
   `add_rest`, reordering to a single combined check path may save redundant
   work.

4. **Defer hot-ring work until telemetry confirms near-best price concentration
   is high enough to justify the complexity.** The ChunkPool experiment is a
   cautionary example: a cache-locality intuition that did not survive
   benchmark contact. The same discipline should apply to the hot ring.

5. **Do not revisit ChunkPool.** The corrected macro benchmark shows it is
   structurally slower than the simpler OrderPool design, and no kChunkSize
   value changes this conclusion. The experiment is recorded for reference but
   the design is not the active baseline.

The following versions from the original plan remain relevant:

```text
V2: Replace std::map with absl::btree_map (low-risk ordered container swap)
V3: Add price-locality telemetry (prerequisite for hot-structure sizing)
V4: Hot ring + ordered cold map (only after V3 data justifies it)
```

V0 and V1 are already implemented — the current master is the `std::map` + `SideBook`
baseline.
