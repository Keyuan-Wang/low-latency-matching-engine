# Phase 7 Benchmark Results

## Environment

All runs on the same Hetzner CCX23 instance (AMD EPYC Milan, 4 vCPU, 32 GB RAM, KVM). Compiler: GCC 15.2.0, `-O3`. Scenario: `hft_macro` with `orders=100000`, `levels=100`, `batch_size=100000`, `warmup_iters=1`, `iters=1`, `seed=42`.

## Experiment 1: Phase 7 vs Phase 6b (std::map + PMR rolled back)

10 trials. master @ `bc70159` (ring + cold) vs phase6b @ `4faa7f1` (naive `std::map` SideBook).

| Metric | master | phase6b | Change |
|---|---:|---:|---:|
| avg ns/op | **24.19** | 31.85 | **−24.1%** |
| ops/s | **41.3M** | 31.4M | **+31.7%** |
| instructions/op | **177.3** | 218.1 | **−18.7%** |
| branches/op | **36.1** | 47.9 | **−24.7%** |
| branch misses/op | **1.47** | 2.16 | **−32.1%** |
| CPI | 0.495 | 0.529 | −6.4% |
| cache misses/op | 0.041 | 0.032 | +29% |

95% CIs do not overlap: master [23.89, 24.49] vs phase6b [31.71, 31.99].

Artifacts: `server_results/compare_master_vs_phase6b_20260605_181141/`

## Experiment 2: Phase 7 vs Phase 6a (std::map, no PMR, handle-based)

10 trials. master @ `00e6470` vs phase6a @ `d778e4f`.

| Metric | master | phase6a | Change |
|---|---:|---:|---:|
| avg ns/op | **24.26** | 30.30 | **−19.9%** |
| ops/s | **41.2M** | 33.0M | **+24.9%** |
| instructions/op | **177.3** | 183.6 | **−3.4%** |
| branches/op | **36.1** | 41.4 | **−12.8%** |
| branch misses/op | **1.48** | 2.17 | **−32.2%** |
| CPI | **0.508** | 0.598 | **−15.1%** |
| cache misses/op | 0.038 | 0.035 | +7.5% |

95% CIs do not overlap: master [23.86, 24.66] vs phase6a [29.99, 30.61].

Artifacts: `server_results/compare_master_vs_phase6a_20260605_182321/`

### Why two baselines

Phase 6b = Phase 6a + PMR node pool (rejected). Phase 6a is the cleaner comparison because it isolates the ring vs `std::map` delta without the PMR noise. The two experiments serve as mutual cross-validation: master's latency is consistent across both (24.19 vs 24.26 ns/op).

### Where the speedup comes from

Phase 6a already removed the hash-map cancel index (Phase 6a handle migration), so the remaining `std::map` cost was the dominant bottleneck. Comparing master against phase6a shows:

- **Instructions dropped 3.4%** (177.3 vs 183.6). The gap is modest because phase6a was already lean — ring eliminates `lower_bound` + `try_emplace` + RB-tree rebalance, replacing them with `rank` + `idx_of` + one array load.
- **CPI dropped 15.1%** (0.508 vs 0.598). This is the dominant contributor. The ring's array index hits L1 directly, while `std::map`'s pointer-chasing tree traversal causes branch mispredictions and cache stalls.
- **Branch misses dropped 32.2%** (1.48 vs 2.17). This is the direct signature of the RB-tree comparison chain disappearing from the hot path.
- **Cache misses rose 7.5%** (0.038 vs 0.035). A minor regression — the ring + cold map is two data structures where `std::map` was one. But the instruction and CPI savings far outweigh this cost.

Comparing against phase6b amplifies the gap (−24.1% latency, −18.7% instructions) because phase6b's PMR layer added extra instruction overhead on top of phase6a.

## Experiment 3: RingSize Sweep (8, 16, 32, 64)

30 trials per configuration. Single commit `fc971b9`, same machine.

| RingSize | avg ns/op | 95% CI | ops/s | instr/op | branch miss/op | cache miss/op |
|---:|---:|---|---:|---:|---:|---:|
| **16** | **24.091** | [23.998, 24.184] | 41.5M | 177.5 | 1.467 | 0.040 |
| 32 | 24.100 | [24.015, 24.186] | 41.5M | 177.4 | 1.463 | 0.039 |
| 64 | 24.245 | [24.145, 24.344] | 41.2M | 179.4 | 1.465 | 0.040 |
| 8 | 25.057 | [24.967, 25.148] | 39.9M | 179.6 | 1.556 | 0.041 |

Statistical significance (Welch t-test, n=30):

| Comparison | Δ mean | p-value | Cohen's d | Bonferroni |
|---|---:|---:|---:|---|
| 16 vs 32 | 0.009 ns (0.04%) | 0.882 | 0.04 | no |
| 16 vs 64 | 0.15 ns (0.63%) | 0.025 | 0.59 | no |
| 16 vs 8 | 0.97 ns (3.86%) | <0.001 | 3.93 | **yes** |

Artifacts: `server_results/master_ring_size_sweep_trials30_20260605_185129/`

### Finding 1: RingSize=8 is too small

~4% slower than 16/32, p<0.001, survives Bonferroni correction (6 pairwise tests, threshold p<0.0083). The HFT macro workload places ~90% of operations within ±5 ticks of best. An 8-slot window cannot cover this range — near-best operations spill into the cold `std::map` path. This shows up as +2 instructions/op (179.6 vs 177.5) and +6% branch misses (1.556 vs 1.467).

### Finding 2: RingSize=16 and 32 are statistically indistinguishable

p=0.882, Cohen's d=0.04 (negligible effect). 95% CIs overlap almost entirely. Under the current workload, 16 slots already capture virtually all near-best operations. Widening to 32 adds no measurable benefit.

### Finding 3: RingSize=64 shows a weak regression trend

0.6% slower than 16, p=0.025 at α=5% but does not survive Bonferroni correction — treat as suggestive, not conclusive. The likely cause is +2 instructions/op (179.4 vs 177.5), possibly from different compiler codegen for the larger array (loop unrolling thresholds, `flush_all_to_cold` iterating 64 slots). Cache misses are identical (0.040), so it is not a footprint issue.

### RingSize conclusion

RingSize=16 is optimal for the current workload. It matches 32 in performance while using half the footprint (384B vs 768B ring array). The `uint_from_size<16>` trait maps `MaskType` to `uint16_t`, enabling `std::rotr` to perform a natural 16-bit rotation with no masking overhead.

## Cross-Phase Summary

Headline `hft_macro` latency across all measured phases, from the unified campaign and Phase 7 results:

| Phase | avg ns/op | ops/s | Key change |
|---|---:|---:|---|
| Phase 1 | 2170 | 0.47M | `std::list`, O(N) cancel |
| Phase 2a | 2137 | 0.47M | pool-backed intrusive list |
| Phase 2b | 48.3 | 20.7M | `unordered_map` O(1) cancel index |
| Phase 2e | 39.8 | 25.2M | `absl::flat_hash_map` (Swiss Table) |
| Phase 4a | 39.3 | 25.5M | `SideBook` abstraction |
| Phase 5 | 34.4 | 29.1M | production profiling baseline |
| Phase 6a | 30.3 | 33.0M | handle-based cancel, hash map removed |
| **Phase 7** | **24.2** | **41.3M** | **hot ring buffer + cold std::map** |

Phase 1 → Phase 7 cumulative: **2170 → 24.2 ns/op, 90× throughput improvement**.

Phase 6a → Phase 7: **30.3 → 24.2 ns/op, 20% latency reduction, 25% throughput gain**.
