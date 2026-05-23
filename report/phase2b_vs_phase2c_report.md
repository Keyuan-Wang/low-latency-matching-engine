# Phase2b vs Phase2c: Open-Addressing Flat Hash Table

## What Changed

Phase2c replaces `std::unordered_map<uint64_t, Order*>` (used for `id_to_order_` in Phase2b) with a custom open-addressing flat hash table that uses:

- **Single contiguous `std::vector`** for all slots → no per-entry heap allocation, no pointer chasing
- **Linear probing** with power-of-2 masking → probe chain fits in 1–2 cache lines
- **Tombstone-based deletion** → O(1) amortised erase
- **Load-factor-triggered rehash** at 60% (size + tombstones > capacity × 0.6) → purges accumulated tombstones

The motivation is the same as Phase2a's pool allocator: replace pointer-chasing data structures with contiguous memory to improve cache locality.

## Benchmark Results (orders=100,000, levels=100)

| Scenario | Metric | phase2b | phase2c | Change |
|---|---|---|---|---|
| **lmt_rest** | Throughput | 11.2M ops/s | 17.0M ops/s | **+52.1%** |
| | p99 latency | 158 ns | 77 ns | −51.5% |
| | Instructions/op | 842 | 552 | −34.4% |
| **lmt_cross_shallow** | Throughput | 10.2K ops/s | 15.4K ops/s | **+50.2%** |
| | p99 latency | 112 μs | 78 μs | −30.3% |
| | Instructions/op | 1.60M | 0.95M | −40.5% |
| **lmt_rest (levels=1000)** | Throughput | 11.1M op/s | 16.6M op/s | **+49.0%** |
| **mkt_sweep_deep** | Throughput | 134.5K ops/s | 184.0K ops/s | **+36.8%** |
| | Instructions/op | 108.6K | 64.7K | −40.4% |
| **lmt_cross_deep** | Throughput | 262.3K ops/s | 226.6K ops/s | **−13.6%** |
| | Instructions/op | 54.1K | 32.3K | −40.4% |
| | CPI | 0.29 | 0.59 | +104.3% |
| **cxl_hit** | Throughput | 5.48M ops/s | 3.42M ops/s | **−37.7%** |
| | CPI | 2.46 | 5.13 | +108.6% |
| **cxl_miss** | Throughput | 15.7M ops/s | 15.3M ops/s | −2.9% |
| **dup_reject** | Throughput | 56.3M ops/s | 49.4M ops/s | −12.2% |
| | CPI | 0.64 | 0.89 | +38.4% |
| **overall** | Throughput | 5.39M ops/s | 5.24M ops/s | −2.9% |
| | Instructions/op | 869 | 665 | −23.4% |
| | CPI | 0.77 | 1.02 | +33.3% |

## Analysis

### Scenarios That Improved (lmt_rest, lmt_cross_shallow, mkt_sweep_deep)

The flat hash table delivers instruction reductions of 34–41% in these scenarios, translating directly to throughput improvements of 37–52%. The mechanism:

1. `std::unordered_map::emplace` triggers bucket traversal, hash recomputation, and node allocation. The flat hash table reduces this to a single contiguous slot write.
2. The slot array's linear layout eliminates the cache miss from chasing `unordered_map` node pointers.

These scenarios do light find() work relative to the matching loop, so the hash table savings are multiplied by the volume of maker-insert and maker-erase calls.

### Scenarios That Regressed (cxl_hit, dup_reject, cxl_miss, overall)

The regression is concentrated in two families:

**Dup_reject** (p99 +63.7%, CPI +38.4%) — This is a micro-benchmark where the sole operation is `find()` returning a hit. With `std::unordered_map`, this is a single bucket probe (O(1) average). With open addressing, even a successful find probes 1–3 slots on average. The flat table's lower instructions/op (irrelevant here since no erase/insert happens) doesn't compensate for the longer probe path.

**cxl_hit** (throughput −37.7%) — Each cancel erases an entry, which places a **tombstone** in the slot. Subsequent find() calls must probe past tombstones until they reach an EMPTY slot or find their key. With many cancels accumulating in the table, the average probe chain length increases, eroding the performance of subsequent lookups.

**cxl_miss** (−2.9%) and **overall** (−2.9%) — Tomblestone accumulation in the mixed workload. The overall scenario's instruction count drops 23.4% (from 869 to 665), which confirms the open addressing scheme is doing less per-operation work. However, the CPI rises 33.3% (from 0.77 to 1.02), negating the instruction savings. The CPI increase is caused by the irregular memory access pattern from probing past tombstones — the probe chain jumps across the slot array rather than accessing sequential cache lines.

## The Tombstone Problem

The flat hash table's Achilles' heel is its deletion strategy:

```
Insert A → [A] [ ] [ ] [ ] [ ]    probe: 1 step, insert at slot 0
Insert B → [A] [B] [ ] [ ] [ ]    probe: 1 step, insert at slot 1
Erase  A → [T] [B] [ ] [ ] [ ]    tombstone at slot 0
Find   B → probe slot 0: TOMBSTONE → probe slot 1: OCCUPIED+B → found (2 steps)
Find   C → probe slot 0: TOMBSTONE → probe slot 1: OCCUPIED → slot 2: EMPTY → not found (3 steps)
```

Every erase creates a tombstone that the next find() must skip. In a cancel-heavy workload (35% cancel + 30% modify = 65% write operations that touch the hash table), tombstones proliferate. Rehash purges them, but with a 60% load factor trigger, the rehash happens infrequently.

### Why Not Just Rehash More Aggressively?

Frequent rehash is O(capacity) — copying every live entry. In a benchmark running 100,000 operations per trial, a rehash every few thousand operations would add significant overhead.

## Optimization Roadmap

### Phase 2d: Robin Hood Hashing + Backward Shift Deletion

Replace the current linear-probe-with-tombstones with the algorithm recommended by **"Optimizing Open Addressing"** (thenumbat), which opens with the claim:

> *"Your default hash table should be open-addressed, using Robin Hood linear probing with backward-shift deletion."*
>
> — [Optimizing Open Addressing](https://thenumb.at/Hashtables/)

Two complementary changes:

**Robin Hood insertion** — When inserting, track each entry's probe distance (how far it is from its ideal slot). If the currently-inserting key has probed farther than the entry in the slot, swap them. This bounds the maximum probe distance — "rich" (close-to-ideal) entries yield to "poor" (far-from-ideal) ones.

**Backward shift deletion** — When erasing, instead of placing a tombstone, shift subsequent displaced entries backward to fill the gap:

```
Before (tombstone):  [A] [T] [C] [D] [E] [EMPTY]
                     find(X) probes 5 slots to reach EMPTY

After (shift):       [A] [C] [D] [E] [EMPTY] [EMPTY]
                     find(X) probes 4 slots to reach EMPTY
```

The two operations work together: Robin Hood keeps probe distances balanced and short; backward shift deletion eliminates tombstones without disturbing the probe-distance distribution. Without shift deletion, Robin Hood's probe bounds would still be eroded by tombstone accumulation.

**Trade-off**: erase becomes O(probe-chain-length) instead of O(1), but at α < 0.6 the average chain is short (~2–3 slots). Insert gains a swap check per probe which adds ~2 instructions per probe step.

**Expected impact**: Recover the cxl_hit and cxl_miss regression where tombstone chains degraded lookup; overall throughput should stabilise at or slightly above Phase 2b.

### Phase 2e: Swiss Table with absl::flat_hash_map

Replace the custom Robin Hood hash table with Google Abseil's production-grade `absl::flat_hash_map`:

- Abseil's implementation uses the Swiss Table algorithm (16-way SIMD group lookup)
- Fully validated, SIMD-optimized, and memory-managed by Google's infrastructure team
- Drop-in replacement for `std::unordered_map` with a compatible API

**No need to hand-roll Swiss Table SIMD** — the value of this phase is not in the implementation but in the **benchmark comparison**: how does a hand-tuned Robin Hood table (Phase 2d) compare against a production SIMD-accelerated table (Phase 2e) in the matching-engine workload?

This closes the optimization loop: from `std::unordered_map` (Phase 2b) → custom open-addressing (Phase 2c) → custom Robin Hood + shift-delete (Phase 2d) → production flat_hash_map (Phase 2e), measuring the gap between a well-implemented academic algorithm and an engineered library.

### Summary

| Phase | Change | Target | Status |
|---|---|---|---|
| 2b | `std::unordered_map` + O(1) cancel | baseline | Done |
| 2c | Open-addressing flat hash table | cache locality | Done |
| 2d | Robin Hood + backward shift deletion | balanced probes, no tombstone | Planned |
| 2e | absl::flat_hash_map | production SIMD hash table | Planned |
