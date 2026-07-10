# Phase 1–6 Evolution Report (Engine + `hft_macro`)

This report summarizes **incremental engine optimizations** from Phase 1 through Phase 6 (`master`), grounded in a **unified cloud `hft_macro` campaign** (Jun 3, 2026, Hetzner CCX23) and prior documented experiments. It is written for a **progressively developed** codebase: later phases add benchmarks and profiling that earlier phases do not have.

---

## 1. Measurement policy

### 1.1 Primary metric

**`hft_macro`** is the headline metric for cross-phase engine decisions (Phase 3 onward). Configuration for the Jun 2026 sweep:

| Parameter | Value |
|---|---:|
| `orders` | 100,000 |
| `levels` | 100 |
| `batch_size` | 100,000 |
| `iters` / `warmup_iters` | 1 / 1 |
| `trials` | 10 |
| `seed` | 42 |
| Host | Hetzner CCX23 (see `artifacts/env.txt` in each result dir) |

### 1.2 Fair cross-phase comparison: `*-devalidated` branches

Branches named `*-devalidated` remove **in-core validation** (`pending_cancel_ids_`, duplicate-id checks before match, etc.) while **keeping** the Phase 2–5 **id-keyed** cancel path (`cancel_order(order_id)`, `id_to_order_`). They do **not** include Phase 6’s `OrderHandle` refactor.

Use **devalidated** numbers when ranking Phase 1–5 engine structure. Cite **`master` separately** as Phase 6 (handle + gateway-owned validation boundary).

Already completed before this sweep (excluded from the 8-branch job):

- `phase5-finale-devalidated` vs `master` → `server_results/compare_master_vs_phase5_20260603_143852/`

### 1.3 What is *not* comparable across all rows

| Issue | Phases affected | How to read results |
|---|---|---|
| **O(N) cancel** | 1, 2a | `hft_macro` is 48% cancel; scan-cancel engines are ~45× slower than 2b+. Throughput table still lists them for completeness, but **do not** rank them against 2b–5. |
| **In-core validation** | Non-devalidated historical runs | Older campaigns (e.g. May 2026) include validation overhead. Jun 2026 devalidated sweep is the apples-to-apples line for 2b–5. |
| **`OrderHandle` + bench migration** | 6 (`master`) | Timed path uses **precomputed handles** from `Setup()`; no hash lookup in `RunOp()`. ~17.6% faster than `phase5-devalidated` on the same host is a **design + harness** win, not a pure “drop validation” delta. |
| **`perf record` / op profiling** | 5–6 only (feature exists) | Do not infer engine hot spots for Phase 1–4 from perf artifacts that did not exist on those commits. |
| **ChunkPool** | `phase4-finale` branch only | Not on `master`. Macro is within noise of 4a but micro paths regressed (see §4.4). |

### 1.4 Aggregated numbers (Jun 3, 2026)

Full CSV: `server_results/hft_macro_cross_phase_summary_20260603.csv`

| Phase | Tag | avg ns/op (95% CI) | ops/s (95% CI) | instr/op | cache miss/op | CPI |
|:---:|---|---:|---:|---:|---:|---:|
| 1 | p1-deval | 2170 (1941–2399) | 0.47M | 10797 | 112 | 0.78 |
| 2a | p2a-deval | 2137 (1945–2328) | 0.47M | 10750 | 117 | 0.76 |
| 2b | p2b-deval | **48.27** (48.09–48.46) | **20.7M** | 390 | 0.10 | 0.45 |
| 2c | p2c-deval | 70.57 (69.2–71.9) | 14.2M | 243 | 1.16 | 1.07 |
| 2d | p2d-deval | 71.78 (71.0–72.6) | 13.9M | 243 | 1.07 | 1.07 |
| 2e | p2e-deval | **39.76** (38.6–40.9) | **25.2M** | 313 | 0.12 | 0.46 |
| 4a | p4a-deval | 39.34 (38.1–40.6) | 25.5M | 313 | 0.13 | 0.47 |
| 4-finale | p4fin-deval | 40.17 (39.7–40.6) | 24.9M | 335 | 0.04 | 0.43 |
| 5 | phase5-deval | 34.44 (33.4–35.5) | 29.1M | 284 | 0.03 | 0.43 |
| 6 | master | **29.29** (29.0–29.6) | **34.1M** | **184** | 0.04 | 0.58 |

**Speedups vs p2b-deval (first O(1) cancel baseline):**

- 2e: **1.21×** latency  
- 4a: **1.23×**  
- 4-finale (ChunkPool): **1.20×** (within CI overlap with 2e/4a)  
- 5 devalidated: **1.40×**  
- 6 master vs 5 deval: **1.18×**; vs 2b: **1.65×**

Raw artifacts:

- 8 devalidated branches: `server_results/devalidated_hft_macro_20260603_150410/`
- master vs phase5: `server_results/compare_master_vs_phase5_20260603_143852/`

---

## 2. Capability matrix (progressive features)

| Capability | P1 | P2a | P2b–e | P3 | P4a | P4-finale | P5 | P6 (`master`) |
|---|:---:|:---:|:---:|:---:|:---:|:---:|:---:|:---:|
| Correct `OrderBook` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| Pool + intrusive per-level list | | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ (OrderPool) |
| O(1) `id_to_order_` cancel | | | ✓ | ✓ | ✓ | ✓ | ✓ | ✗ (handle) |
| `absl::flat_hash_map` index | | | 2e+ | ✓ | ✓ | ✓ | ✓ | ✗ |
| HFT micro + **`hft_macro`** | partial | partial | ✓ | **primary** | ✓ | ✓ | ✓ | ✓ (handle-aware bench) |
| Legacy `overall` / seven scenarios | ✓ | ✓ | ✓ | retired as primary | | | | |
| `LLMES_PROFILE_HFT_MACRO_OPS` | | | | | | | ✓ | ✓ |
| Window-isolated **`perf record`** | | | | | | | ✓ | ✓ |
| `add_rest` stage timers | | | | | | | ✗ retired | ✗ |
| **`*-devalidated`** branches | ✓ | ✓ | ✓ | — | ✓ | ✓ | ✓ | N/A |
| **`OrderHandle`** / no `id_to_order_` hot path | | | | | | | | ✓ |

---

## 3. Phase-by-phase narrative

### Phase 1 — Correctness-first baseline

**Engine:** `std::map` price levels; `std::list<Order>` per level; cancel/modify by **book scan**.

**Optimization intent:** Establish correct FIFO semantics, not throughput.

**`hft_macro` (devalidated):** ~2170 ns/op (~0.47M ops/s). Dominated by O(N) cancel under a 48% cancel mix. Confirms the macro is the right stress test but this phase is a **different performance class** than 2b+.

**Legacy benchmarks** (Phase 1 report): cancel-hit scenarios were ~17K ops/s vs millions after Phase 2b.

---

### Phase 2a — Pool + intrusive list

**Engine:** `std::vector<Order>` pool; intrusive list per level; still **O(N) cancel**.

**Win:** Removes per-order heap allocation; better traversal locality on adds and matches.

**`hft_macro`:** ~2137 ns/op — marginally better than Phase 1 on macro, still scan-bound on cancel.

**Lesson:** Allocator elimination alone cannot fix cancel-heavy HFT flow.

---

### Phase 2b — O(1) cancel index (discontinuity)

**Engine:** `std::unordered_map<OrderId, Order*>` + intrusive unlink.

**Win:** Cancel/modify become hash lookup + list splice. Legacy `cxl_hit` jumped to millions of ops/s; mixed workloads flipped from thousands to millions of ops/s.

**Cost:** Hash insert/erase on every resting add and maker fill.

**`hft_macro` (devalidated):** **48.27 ns/op, 20.7M ops/s** — first macro point comparable across later phases. This is the **reference cliff** for all subsequent tables.

---

### Phase 2c–2d — Custom hash tables (negative result)

**Engine:** Open addressing with tombstones (2c); Robin Hood + backward-shift deletion (2d).

**Hypothesis:** Lower instruction count vs `std::unordered_map`.

**`hft_macro`:** **~71.8 ns/op (~14M ops/s)** — ~**47% slower** than 2b. Higher CPI and **~10× cache misses/op** vs 2b/2e.

**Lesson:** Under realistic cancel-heavy mixing, probe chains and tombstones hurt more than instruction count helps. Documented in depth in `report/phase2b_to_phase_2e_comparison.md` (May campaign; conclusion unchanged on devalidated re-run).

---

### Phase 2e — Production hash index

**Engine:** `absl::flat_hash_map` for `id_to_order_`.

**`hft_macro`:** **39.76 ns/op, 25.2M ops/s** — **1.21× vs 2b**, with Swiss-table metadata probes and stable growth.

**Lesson:** Use battle-tested flat hash for the cancel index; do not maintain custom tables for this workload.

---

### Phase 3 — HFT benchmark redesign (measurement, not engine)

**Change:** Introduced micro scenarios (`hft_cancel_hot`, etc.) and **`hft_macro`** (45% add / 48% cancel / 5% modify / 2% market), pre-generated batches, PMC + latency.

**Effect on history:** Re-ranked 2c/2d from “looks OK on old `overall` mix” to “clear macro regression.” No separate engine commit label—compare **2e vs 2b** on macro before/after Phase 3 methodology.

**Testing note:** Phase 1–2a branches required build fixes (`OrderBook` stub ctor, Abseil for macro) before cloud macro would compile; capability matrix §2.

---

### Phase 4a — SideBook abstraction

**Engine:** Wrap bid/ask storage in `SideBook`; retain `std::map` price levels, pool, `absl::flat_hash_map` index. Baseline commit documented as `ce7e7c2`.

**`hft_macro` (devalidated):** **39.34 ns/op** — essentially tied with **2e** (within CI). Structural refactor did not regress macro.

**Price-level strategy:** Fixed-size vectors rejected for correctness (drifting best price under macro). `absl::btree_map` deferred due to pointer-stability constraints (`report/phase4_price_level_storage_strategy.md`).

---

### Phase 4-finale — ChunkPool experiment (side branch)

**Engine:** Per-level chunk storage for orders (hypothesis: better locality than one global pool).

**`hft_macro` (devalidated):** **40.17 ns/op** vs 4a **39.34** — **~2% slower**, CI overlapping.

**Micro / campaign_20260601_1319:** Several scenarios regressed vs 4a; ChunkPool **not** merged to `master`.

**Testing:** Op-profiling and repaired planning-book `hft_macro` validated on this branch (`server_results/macro_op_profile_cloud_phase4_finale_t1/`).

---

### Phase 5 — Profiling, perf record, de-validation

**Engine (on `phase5-finale`):** Same id-keyed core as 4a; focus shifts to **where** time goes.

**Benchmark / tooling (progressive):**

1. **`LLMES_PROFILE_HFT_MACRO_OPS`** — op-class shares (add_rest, cancel_hit, …). Early runs had bogus `cancel_miss` until planning-book replay fix.
2. **`add_rest` stage profiling** — **retired** (probe cost ≫ measured regions).
3. **Window-isolated `perf record`** (`PerfRecordControl`, FIFO) — authoritative: ~50% cycles in cancel-index hash ops; ~18% in `std::map` `get_or_create` on adds (`report/phase5_macro_profiling_plan.md`).

**De-validation (`phase5-finale-devalidated`):** **34.44 ns/op** — **1.40× vs 2b**, **1.15× vs 2e** on macro with validation stripped.

**Do not claim** perf-record breakdown for Phase 1–4 commits that lack the tooling.

---

### Phase 6 (`master`) — OrderHandle + benchmark contract

**Engine:** Dense `OrderHandle`; drop hot-path `id_to_order_`; gateway owns id→handle; in-core validation removed from timed path (validation at boundary).

**Benchmark:** `bench_hft_macro` tracks **handles** from `AddResult`; cancel/modify targets resolved in **`Setup()`**, not in `RunOp()` (`report/phase6_benchmark_handle_migration.md`).

**`hft_macro` vs `phase5-devalidated`:** **29.29 vs 34.44 ns/op** (~**17.6%** faster); **183.7 vs 284.0 instr/op** — handle path removes hash probes from the measured window.

**Interpretation:** Phase 6 number is the **production-shaped matching-core boundary** (handle in, handle out). It is **not** directly comparable to devalidated Phase 2–5 without also migrating those branches to handles (explicit non-goal for `*-devalidated` branches).

---

## 4. Synthesis

### 4.1 What moved the macro needle

1. **O(1) cancel index (2b)** — largest step function (~45× vs 1/2a on macro).  
2. **Better hash table (2e)** — +21% vs 2b on devalidated macro.  
3. **Avoid 2c/2d** — large regression from memory behavior, not instruction count.  
4. **SideBook / 4a** — neutral vs 2e.  
5. **ChunkPool** — not adopted; macro flat, micro regressed.  
6. **De-validation (5)** — measurable but smaller than index/handle changes.  
7. **Handles (6)** — largest single gain after 2b on comparable host vs phase5-deval; eliminates index from hot path.

### 4.2 Recommended reading order

| Topic | Document |
|---|---|
| Phase 1 vs 2 (legacy suite) | `report/phase1_vs_phase2_report.md` |
| 2b–2e macro deep dive | `report/phase2b_to_phase_2e_comparison.md` |
| Macro design | `report/phase3_hft_benchmark_design.md` |
| Price levels / ChunkPool | `report/phase4_price_level_storage_strategy.md`, `PROJECT_HISTORY.md` |
| Perf + op profile | `report/phase5_macro_profiling_plan.md` |
| Handle migration | `report/phase6_engine_handle_refactor_plan.md`, `report/phase6_benchmark_handle_migration.md` |
| De-validation rollout | `report/phase6_devalidation_branch_rollout.md` |
| Full experiment log | `PROJECT_HISTORY.md` |

### 4.3 Reproducing the Jun 2026 sweep

```bash
# 8 devalidated branches (already run)
VERSIONS='phase1-finale-devalidated:p1-deval,...,phase4-finale-devalidated:p4fin-deval' \
  SCENARIOS=hft_macro ORDERS=100000 LEVELS=100 BATCH_SIZES=100000 \
  ITERS=1 WARMUP_ITERS=1 TRIALS=10 \
  SERVER_IP=178.105.250.133 REPO_URL=https://github.com/Keyuan-Wang/llmes.git \
  INSTALL_DEPS=0 LOCAL_OUT_DIR=./server_results/devalidated_hft_macro_20260603_150410 \
  bash benchmark/scripts/run_remote_compare.sh

# master vs phase5 (already run)
VERSIONS='master:master,phase5-finale-devalidated:phase5-deval' \
  ... # same macro params
```

---

## 5. Limitations

- **Single host / single day** for the unified table; use CI columns for uncertainty.  
- **Phase 1–2a macro** measures an engine that cannot serve cancel-heavy production load—listed for historical continuity only.  
- **Phase 6** includes benchmark scope change (handles precomputed in `Setup()`); see Phase 6 plan “Benchmark Scope.”  
- Older campaigns with **validation enabled** report higher ns/op; cite devalidated or `master`/`phase5-deval` pairs when comparing to this table.

---

*Generated from cloud artifacts pulled 2026-06-03. Pipeline logs: `server_results/devalidated_hft_macro_20260603_150410/artifacts/pipeline.log`.*
