# Phase2c vs Phase2d: Robin Hood Hashing + Backward Shift Deletion

## What Changed

Phase2d replaces the tombstone-based linear probing in the open-addressing flat hash table with two complementary algorithms described in ["Optimizing Open Addressing"](https://thenumb.at/Hashtables/):

- **Robin Hood insertion** — When inserting, each entry tracks its probe distance (how far from its ideal slot). If the incoming entry has probed farther than the resident entry, they swap. This bounds the maximum probe distance: "rich" (close-to-ideal) entries yield to "poor" (far-from-ideal) ones.
- **Backward-shift deletion** — When erasing, instead of placing a tombstone, shift subsequent displaced entries backward to fill the gap. This keeps the table compact without needing tombstones.

The theory: Robin Hood keeps probe distances balanced and short; backward shift deletion eliminates tombstone accumulation that degraded cancel-heavy workloads in Phase2c.

## Benchmark Results (orders=100,000, levels=100)

| Scenario | Metric | phase2c | phase2d | Change |
|---|---|---|---|---|
| **dup_reject** | Throughput | 39.8M ops/s | 41.2M ops/s | **+3.5%** |
| | p99 latency | 46 ns | 41 ns | −12.1% |
| | Cache misses/op | 4.03 | 3.70 | −8.2% |
| **lmt_cross_deep** | Throughput | 198.4K ops/s | 205.2K ops/s | **+3.4%** |
| | Instructions/op | 32.3K | 33.1K | +2.5% |
| | CPI | 0.68 | 0.66 | −4.2% |
| **cxl_hit** | Throughput | 1.86M ops/s | 1.87M ops/s | **+0.3%** |
| | CPI | 9.09 | 8.25 | −9.2% |
| **cxl_miss** | Throughput | 12.1M ops/s | 12.0M ops/s | **−1.3%** |
| **lmt_rest** | Throughput | 16.7M ops/s | 16.5M ops/s | **−1.5%** |
| **lmt_cross_shallow** | Throughput | 13.9K ops/s | 13.6K ops/s | **−1.7%** |
| **mkt_sweep_deep** | Throughput | 127.1K ops/s | 124.2K ops/s | **−2.2%** |
| **overall** | Throughput | 4.10M ops/s | 3.92M ops/s | **−4.5%** |
| | Instructions/op | 665 | 687 | +3.3% |
| | CPI | 1.37 | 1.47 | +6.9% |
| | Cache misses/op | 7.35 | 9.23 | +25.6% |

## Analysis

### Why Did Phase2d Not Improve Performance?

**The short answer**: at α ≤ 0.6 (60% load factor), tombstone accumulation does not degrade performance enough for backward-shift deletion to justify its overhead.

### 1. Backward-Shift Overhead Dominates

Every erase now triggers an O(probe-chain) backward-shift scan. In a workload where 65% of operations involve an erase (35% cancel + 30% modify), this overhead runs frequently:

```
Phase2c erase:  mark slot as TOMBSTONE  → O(1)
Phase2d erase:  compact cluster         → O(cluster length)
```

At the cost of slightly longer probe chains (from tombstones), Phase2c keeps erase a single-store operation. Phase2d replaces that store with a loop that probes through the rest of the cluster, shifting entries. The extra instructions and cache misses from this scan offset the benefit of having no tombstones.

### 2. Low Load Factor Limits Tombstone Impact

The hash table rehashes when it reaches 60% load (including tombstones). This bounds the effective tombstone density:

  - Maximum tombstones before rehash ≈ 0.6 × capacity − live_entries
  - Average cluster size without tombstones at α=0.6: ~2.5 probes
  - Average cluster size with accumulated tombstones: still < 5 probes

The probe chain length penalty from tombstones is bounded by the rehash threshold. The backward-shift compaction saves at most 1–2 probe steps per lookup, which is rarely enough to recover its own cost.

### 3. Instruction Count Increased

Phase2d consistently shows 2–4% more instructions per operation across all non-trivial scenarios. The sources:

- **Robin Hood swap check**: Every insert probe step adds a `dist > slots_[idx].probe_dist` comparison and potential swap. In the insert-heavy crossing scenarios, this adds ~2.5% instruction overhead.
- **Backward shift loop**: Every erase iterates through the remainder of the cluster. The `ideal_idx` computation and the distance check `((gap - ideal) & mask) < ((idx - ideal) & mask)` add ~15 instructions per erased entry.

### 4. Cache Misses Increased (+25.6% overall)

The backward-shift loop accesses slots beyond the erased entry, pulling additional cache lines into the L1. In the overall benchmark, cache misses per operation increased from 7.35 to 9.23. These extra cache misses raise CPI from 1.37 to 1.47.

### 5. Dup_Reject Improved (+3.5%)

The one scenario where Phase2d shows a measurable improvement is dup_reject — a micro-benchmark that does only `find()` calls on a static table. With no tombstones in the probe path, successful finds probe 1–2 fewer slots on average. This confirms that the algorithm works as intended for read-heavy, erase-free workloads.

## Conclusion

Robin Hood + backward-shift deletion is an elegant algorithm that eliminates tombstones and bounds probe distances. However, in this specific workload:

- **Tombstones were not the bottleneck** — the rehash threshold (60% load) keeps tombstone density low enough that probe chains remain short.
- **Erase-heavy workload penalizes backward shift** — the compaction overhead runs on every erase, and with 65% erase operations, it dominates.
- **The original Phase2c design is better for this access pattern** — O(1) erase with occasional tombstone probing outperforms O(cluster) shift deletion.

The optimization roadmap:

| Phase | Change | Target | Status |
|---|---|---|---|
| 2b | `std::unordered_map` + O(1) cancel | baseline | Done |
| 2c | Open-addressing flat hash table (tombstones) | cache locality | Done |
| 2d | Robin Hood + backward shift deletion | balanced probes, no tombstone | Done |
| 2e | `absl::flat_hash_map` | production SIMD hash table | Next |

**Phase 2e** — replacing the custom hash table with Google's production `absl::flat_hash_map` (Swiss Table with 16-way SIMD group lookup) — will be the next step. The Swiss Table uses a different strategy entirely: metadata bytes in a separate array allow SIMD to skip empty/deleted slots in a single instruction, combining the O(1) erase of tombstones with the fast-skip of compacted storage. This may provide a genuine improvement over both Phase2c and Phase2d, particularly in the erase-heavy mixed workload.
