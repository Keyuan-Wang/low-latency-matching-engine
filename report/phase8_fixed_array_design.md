# Phase 8 Rationale: Moving from Hot Ring + Cold Map to Fixed Array Storage

## Summary

Phase 7 proved the central performance idea: when price-level lookup can be reduced to direct offset computation, it is much cheaper than ordered-map traversal. The hot ring buffer path replaced the old `std::map` hot lookup with a compact, indexable window and delivered a large macro-latency improvement.

The next bottleneck is no longer the cost of finding a hot price. The current perf data shows that the remaining cost is increasingly concentrated in maintaining the boundary between two different price-level storage models: the hot ring and the cold ordered map.

This report argues that Phase 8 should move toward a unified fixed-array price-level storage model. The goal is to preserve the direct-index property that made the hot ring successful while removing the re-anchor and hot/cold migration machinery that now consumes a comparable share of cycles.

## Evidence

The relevant profile is:

```text
server_results/hft_macro_perf_record_master_20260606_161810/
```

Run configuration from `SUMMARY.md`:

- commit: `e8c4f29`
- workload: `hft_macro`
- `orders=100000`, `levels=100`
- `batch_size=1000000`, `iters=40`
- build: Release + `-g`, no `LLMES_PROFILE_*`
- profiling window: RunOp batch only, via `perf --control=fifo`
- baseline: `153.81 instructions/op`, `77.93 cycles/op`, `1.488 branch misses/op`, `0.0334 cache misses/op`

The low cache-miss rate is important. The current workload is not primarily waiting on memory. The remaining performance pressure is instruction count, branch behavior, and control-flow complexity.

## Hot Ring Result

The hot ring path is already effective. In the `add_limit_order` breakdown:

| Hot spot | cycles % | instr/op | branch misses/op |
|---|---:|---:|---:|
| hot ring index hit | ~5.5-6.2% | 9.51 | 0.100 |

The exact summary table reports the hot ring index-hit remainder at `6.18%` cycles. Depending on aggregation, this is roughly the 5.5-6% range of total RunOp cycles.

This is a good result: a large fraction of former ordered-map lookup work has collapsed into simple offset arithmetic and slot access. The problem is that this fast path now sits next to maintenance paths whose cost is nearly as large.

## Boundary-Maintenance Cost

The same perf report shows:

| Hot spot | cycles % | instr/op | branch-miss % | branch misses/op |
|---|---:|---:|---:|---:|
| `erase_best` | 4.37% | 6.72 | 4.36% | 0.065 |
| `reanchor_to` | 4.65% | 7.15 | 5.76% | 0.086 |

Together, these two paths account for about:

- `9.02%` of total RunOp cycles
- `13.87 instr/op`
- `10.12%` of sampled branch misses

That is larger than the hot ring fast path itself. This is the key signal.

The hot ring lookup is no longer the main question. The current cost comes from keeping the ring coherent while the best price moves and while prices cross between the hot window and the cold map. Re-anchor, erase-best advancement, and cold/hot boundary management encode the fact that the side book is split across two data structures with different ordering and ownership rules.

## Why Fixed Array Storage Is the Natural Next Step

The hot ring buffer already demonstrates that the price axis wants to be addressed by arithmetic, not by tree traversal. A fixed array generalizes that idea:

- one uniform price-level address space
- direct price-to-offset calculation
- no hot/cold split
- no re-anchor operation
- no promotion or eviction between storage tiers
- less branch-heavy state management around best-price movement

The intended principle is simple: use a direct-addressed price-level array for the configured price band, and use compact bit manipulation to track which price levels are currently active and to locate the next best price. This preserves the successful part of Phase 7, while removing the dual-structure coupling that the perf report now exposes.

This report does not depend on a specific implementation layout. The important design direction is unification: one price-indexed storage model instead of a small hot ring plus a cold ordered map.

## Expected Performance Impact

The target is instruction count and branch misses.

The fixed-array direction should directly attack:

- the `reanchor_to` cost (`4.65%` cycles, `7.15 instr/op`)
- a meaningful part of `erase_best` best-advancement cost (`4.37%` cycles, `6.72 instr/op`)
- cold-map fallback and hot/cold migration logic
- control-flow complexity created by maintaining two storage invariants

The expected gain is not from improving cache-miss behavior. Cache misses are already extremely low at `0.0334/op`. The expected gain is from deleting work: fewer instructions, fewer conditional paths, and less side-book state coordination.

## Recommendation

Phase 8 should move from `hot ring + cold map` to a unified fixed-array side-book model for the HFT benchmark price band.

The decision is supported by the Phase 7 perf data:

- hot ring indexing has validated direct price-offset access
- the hot path itself is now only around 5.5-6% of RunOp cycles
- `erase_best` and `reanchor_to` together cost about 9% of RunOp cycles
- branch-miss share is also concentrated in these maintenance paths
- cache misses are already too low to be the main optimization lever

The next major improvement should therefore come from simplifying the data structure, not from further polishing the current hot/cold boundary.
