# hft_macro TLB / Page-Fault Window-Isolated `perf stat`

**Date:** 2026-07-08  
**Host:** `178.104.95.188` — Hetzner, AMD EPYC-Milan (Zen 3), 4 vCPU, KVM, Ubuntu 26.04, `perf 7.0.6`  
**Scenario:** `hft_macro` (book mode)  
**Artifacts:** `server_results/hft_macro/perf_stat/hft_macro_perf_stat_20260708_174600/`  
**Driver:** `benchmark/scripts/local/hft_macro_perf_stat.sh`

---

## 1. What We Measured

Goal: quantify **DTLB / ITLB / page-fault** behaviour on the engine hot path, excluding benchmark scaffolding (`Setup()`, book rebuild, warmup replay).

Method:

- Wrap `bench_hft_macro` with `perf stat --control=fifo:<ctl>,<ack> -D -1`
- Runner toggles counting only around the measured `RunOp` batch (`PerfWindowControl` in `benchmark_runner.cpp`)
- Linux isolation: CPU 2, `numactl --physcpubind=2 --membind=0`, no `chrt`

Workload (aligned with prior window-isolated `perf record` runs):

| Parameter | Value |
|---|---|
| orders / levels | 100,000 / 100 |
| batch_size × iters | 1M × 40 |
| warmup_iters | 1 |
| measured_ops | 40M |
| wall latency | **16.4 ns/op** |

---

## 2. Corrected Results

After fixing the miss-rate denominator (see §3), the RunOp window looks healthy:

| Metric | Value | Interpretation |
|---|---:|---|
| **DTLB miss rate** | **0.019%** | `dTLB-load-misses / L1-dcache-loads` |
| DTLB MPKI | 0.113 | misses per 1K instructions |
| DTLB misses / op | 0.0139 | stable across runs |
| iTLB MPKI | 0.0008 | negligible |
| page-faults / op | ~0 | 3 total over 40M ops |
| major-faults | 0 | no disk-backed faults in window |
| instructions / op | 122.8 | consistent with macro PMC |

Cross-check with AMD native counters (20M ops, same workload):

| Counter | Count | Derived rate |
|---|---:|---:|
| `ls_l1_d_tlb_miss.all` | 306,557 | 0.0153 / op |
| `ls_dc_accesses` | 1,464,297,772 | — |
| **miss / dc_access** | — | **0.021%** |

Native and corrected generic formulas agree within noise. **The engine is not TLB-bound.**

Page faults are effectively absent in the measured window — expected for a steady-state hot loop whose working set (100K orders, 100 levels) fits comfortably in TLB/cache after warmup. Setup-time faults are correctly excluded by the FIFO window.

---

## 3. AMD Generic Counter Pitfall

### 3.1 The False Alarm

First run used the naive generic formula:

```
dTLB miss rate = dTLB-load-misses / dTLB-loads  →  90.7%
```

This is **wrong on AMD Zen**. It is a measurement artefact, not a code defect.

### 3.2 Root Cause

Linux maps generic cache events via `PERF_TYPE_HW_CACHE`:

```
dTLB-loads       → config 0x00003  (DTLB | READ | ACCESS)
dTLB-load-misses → config 0x10003  (DTLB | READ | MISS)
```

On Intel, both map to distinct hardware counters. On **AMD Zen 3**, there is no PMU counter for "total DTLB accesses" — only miss/reload counters exist (`ls_l1_d_tlb_miss.all`, etc.). The perf JSON therefore maps the generic **ACCESS** event to the same miss/reload counter as the miss event.

Evidence from the same 20M-op run:

| Event | Count | What it actually counts |
|---|---:|---|
| `dTLB-loads` (generic) | 306,594 | ≈ L1 DTLB miss/reload |
| `dTLB-load-misses` (generic) | 555,027* | L1 DTLB miss/reload |
| `ls_l1_d_tlb_miss.all` (native) | 306,557 | L1 DTLB miss/reload |
| `ls_dc_accesses` (native) | 1,464,297,772 | all data cache accesses |

\*First 40M-op run; absolute miss count is stable (~0.014/op), only the bogus ratio was wrong.

`dTLB-loads` ≈ `ls_l1_d_tlb_miss.all`, **not** `ls_dc_accesses`. Dividing two miss-level quantities yields ~1.0.

Even `perf stat -- true` reports ~1,476 `dTLB-loads` — confirming the generic ACCESS name does not mean "all loads" on this platform.

### 3.3 What Still Works

| Generic event | AMD behaviour | Usable as |
|---|---|---|
| `dTLB-load-misses` | ✅ maps to real miss counter | **numerator** |
| `dTLB-loads` | ❌ counts misses, not accesses | do **not** use as denominator |
| `L1-dcache-loads` | ✅ maps to `ls_dc_accesses` | **denominator** for miss rate |
| `instructions` | ✅ always reliable | **denominator** for MPKI |
| `page-faults` / `minor-faults` | ✅ software events | direct count |
| `dTLB-stores` | ❌ `<not supported>` on this VM | skip |

---

## 4. Recommended Formulas

Portable (Intel + AMD):

```
DTLB miss rate  = dTLB-load-misses / L1-dcache-loads
DTLB MPKI       = dTLB-load-misses / instructions × 1000
iTLB MPKI       = iTLB-load-misses / instructions × 1000
page-fault rate = page-faults / measured_ops
```

**Never use** `dTLB-load-misses / dTLB-loads` on AMD — the denominator is broken.

Default events in `hft_macro_perf_stat.sh` were updated accordingly:

```
L1-dcache-loads,dTLB-load-misses,iTLB-load-misses,page-faults,minor-faults,major-faults,instructions,cycles
```

Summary output now includes a `derived_metric` section with the corrected rates.

---

## 5. Conclusion

1. **Code is fine.** DTLB miss rate is ~0.02%, page faults are ~0 in the RunOp window. No TLB or paging optimisation is indicated for the current `hft_macro` workload at 100K×100 depth.
2. **The 90.7% figure was a measurement bug** caused by AMD's incomplete mapping of the generic `dTLB-loads` event — not by pathological memory access in the matching engine.
3. **For AMD profiling**, prefer `L1-dcache-loads` or `instructions` as the denominator; validate suspicious ratios against native events (`ls_l1_d_tlb_miss.all`, `ls_dc_accesses`) when in doubt.

---

## 6. Reproduce

```bash
USE_CHRT_FIFO=0 ENABLE_LINUX_ISOLATION=1 \
  bash benchmark/scripts/local/hft_macro_perf_stat.sh
```

Remote one-liner (sync repo first):

```bash
ORDERS=100000 LEVELS=100 BATCH_SIZE=1000000 ITERS=40 USE_CHRT_FIFO=0 \
  bash benchmark/scripts/local/hft_macro_perf_stat.sh
```
