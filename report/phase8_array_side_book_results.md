# Phase 8 Array Side Book Results

## Summary

Phase 8 investigated replacing the Phase 7 `hot ring + cold map` side book with a unified fixed-array price-level book backed by an occupancy bitmap tree.

The experiment split naturally into three stages:

| Stage | Commit | Description | Verdict |
|---|---|---|---|
| Phase 8a | `81ab90488de2f6e2ebf4d7ab917c69e4f5711e70` | First working array side book | Functionally valid, performance-neutral |
| Phase 8b | `71f1ee191fbe40ad67d69572ccbc01c825d98b99` | Removed read-path ghost clear; fixed `OccupancyTree` as `std::array`; unrolled tree operations | Best version |
| Phase 8c | current `master` | Eager empty-level retirement from cancel/modify paths | Rejected: more instructions, slower macro latency |

The final recommendation is to keep **Phase 8b** as the stopping point for this optimization line. It is materially faster than Phase 7c and also faster than the eager-clear Phase 8c attempt.

## Phase 8a: First Working Array Side Book

Artifact:

```text
server_results/compare_master_vs_phase7c_trials50_20260608_210512/
server_results/hft_macro_perf_record_master_20260608_201629/
```

Phase 8a replaced the hot ring/cold map structure with a direct-addressed array side book and an occupancy tree for active price levels.

In the 50-trial macro comparison:

| Metric | Phase 8a `81ab904` | Phase 7c `2004f35` |
|---|---:|---:|
| avg ns/op | 19.30 | 19.57 |
| cycles/op | 70.3 | 71.0 |
| instructions/op | 158.3 | 137.1 |
| branch misses/op | 1.351 | 1.497 |
| cache misses/op | 0.022 | 0.034 |
| CPI | 0.444 | 0.518 |

Phase 8a was slightly faster on mean latency, but the difference was not statistically significant. The important signal was mixed:

- array storage improved CPI, branch misses, and cache misses;
- instruction count regressed by about 21 instructions/op;
- total cycles stayed roughly flat.

The perf record explained the problem. The new array book had moved cost into `empty()` / `clear_ghost_best_level()`:

| Hot spot | cycles % |
|---|---:|
| `clear_ghost_best_level` via `empty()` | ~24% |
| `get_or_create` -> `OccupancyTree::set` | ~7.6% |
| `OccupancyTree::set` | ~5.5% |
| `OccupancyTree::clear` | ~5.1% |

Phase 8a therefore validated the data-structure direction but not the implementation shape. The array book made the CPU behavior cleaner, but repeated ghost cleanup on pure read paths and dynamic occupancy-tree machinery erased most of the benefit.

## Phase 8b: Fixed Tree and No Read-Path Ghost Clear

Artifact:

```text
server_results/compare_master_vs_phase7c_newvm_20260610_172132/
server_results/hft_macro_perf_record_master_20260610_162529/
```

Phase 8b made two key changes:

1. `empty()`, `best_price()`, and `best_level()` became pure read paths again.
2. `OccupancyTree` was fixed to the known 65536-price range using `std::array`, with set/clear/find logic unrolled across the three tree levels.

This is the first Phase 8 version that clearly beats Phase 7c.

| Metric | Phase 8b `71f1ee1` | Phase 7c `2004f35` | Change |
|---|---:|---:|---:|
| avg ns/op | 17.21 | 19.27 | -10.7% |
| cycles/op | 62.82 | 70.26 | -10.6% |
| instructions/op | 130.05 | 137.13 | -5.2% |
| branch misses/op | 1.229 | 1.496 | -17.8% |
| cache misses/op | 0.0202 | 0.0333 | -39.3% |
| CPI | 0.483 | 0.512 | -5.7% |

This result is qualitatively different from Phase 8a. Phase 8b no longer relies on lower CPI to compensate for higher instruction count. It actually runs fewer instructions than Phase 7c while also improving branch and cache behavior.

The perf record for Phase 8b shows the new remaining bottleneck:

| Hot spot | cycles % | cycles/op |
|---|---:|---:|
| `erase_best` all sites | 9.28 | 5.83 |
| `clear_ghost_best_level` | 7.72 | 4.85 |
| `OccupancyTree::clear` | 2.43 | 1.53 |
| `find_next_set` + `find_prev_set` | 3.58 | 2.24 |
| `get_or_create` | 5.57 | 3.50 |
| `OccupancyTree::set` | 3.42 | 2.15 |

This is acceptable. Phase 8b has a clear remaining hot spot, but the overall system is already faster than Phase 7c by about 11%. The remaining cost is concentrated in a path that is fundamentally tied to best-level depletion, not in every read probe.

## Phase 8c: Eager Empty-Level Retirement

Artifact:

```text
server_results/compare_master_vs_phase8b_20260610_183431/
```

Phase 8c attempted to eliminate lazy ghost cleanup by clearing an active price level as soon as the last order leaves it. The implementation added side/index metadata to `PriceLevel` and had cancel/modify paths notify the owning array side book when a level became empty.

The idea was sound: remove ghost levels earlier and reduce `erase_best -> clear_ghost_best_level` work. The measured result went the other way.

| Metric | Phase 8c `e7f489e` | Phase 8b `71f1ee1` | Change |
|---|---:|---:|---:|
| avg ns/op | 17.95 | 17.05 | +5.2% slower |
| cycles/op | 65.93 | 62.39 | +3.54 cycles/op |
| instructions/op | 139.55 | 130.05 | +9.50 instr/op |
| branch misses/op | 1.293 | 1.230 | worse |
| cache misses/op | 0.0208 | 0.0190 | worse |
| CPI | 0.472 | 0.480 | slightly better |

The lower CPI did not help enough. Phase 8c added about 9.5 instructions/op and lost about 5% latency. That extra mechanism cost more than the ghost-cleanup work it removed.

The conclusion is that eager retirement is too expensive for this workload. It adds work to cancel/modify and level-management paths that are common enough to matter, while Phase 8b's lazy cleanup is cheap enough when read-path cleanup has already been removed.

## Final Decision

Phase 8 should stop at **Phase 8b**:

```text
71f1ee191fbe40ad67d69572ccbc01c825d98b99
```

Phase 8b is the best tradeoff found so far:

- it beats Phase 7c by about 10-11% on `hft_macro`;
- it reduces instructions/op below Phase 7c;
- it keeps branch misses and cache misses lower than Phase 7c;
- it avoids the extra side/index maintenance cost introduced by Phase 8c;
- it leaves the matching core simple enough to reason about.

Phase 8c should be recorded as a negative result, not continued. The remaining optimization space is likely in small, profile-guided polishing rather than another side-book architecture change.

## Recommended Next Step

Freeze Phase 8b as the performance baseline and move future work to smaller, targeted efforts:

- correctness tests for occupancy-tree boundary cases and ghost cleanup;
- optional header inlining of tiny occupancy-tree functions;
- scenario-level profiling inside the macro workload;
- minor layout reductions such as shrinking `PriceLevel` metadata only if a later experiment reintroduces metadata.

No further structural side-book redesign is recommended until a new profile shows a larger bottleneck than the current Phase 8b residual `erase_best` / ghost-cleanup path.
