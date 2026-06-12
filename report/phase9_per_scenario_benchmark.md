# Phase 9 Per-Scenario Macro Benchmark

## Summary

Phase 9 adds a diagnostic benchmark layer on top of the existing HFT macro workload.

The original `hft_macro` benchmark remains the primary performance metric. It measures the full mixed workload with low instrumentation overhead and is the right source for end-to-end latency and PMC results. The new per-scenario benchmark answers a different question: within the same realistic macro event stream, which single-operation scenarios are responsible for the observed latency distribution?

The first useful result is:

```text
server_results/hft_macro_scenarios_20260610_200842/
```

It separates the measured single-operation paths into:

- `add_rest_existing_level`
- `add_rest_new_level`
- `cancel_order`

The result confirms that `cancel_order` is very cheap and stable, while `add_rest_new_level` is the major source of add-rest tail latency.

The final stage of Phase 9 pushed the Linux-side tuning as far as is practical on the cloud VM: dynamic CPU selection, IRQ affinity control, SMT-sibling avoidance, aggressive workqueue/watchdog/realtime tuning, and finally boot-time `nohz_full` isolation. The system counters became much cleaner, but the per-scenario p99 latency did not materially improve. This closes Phase 9 with an important negative result: the remaining p99 behavior is no longer dominated by easily removable Linux-system noise.

## Measurement Environment

All Phase 9 benchmark results discussed in this report were collected on the remote Hetzner CCX23 benchmark server.

| Field | Value |
|---|---|
| provider / machine | Hetzner Cloud CCX23 |
| virtualization | KVM guest |
| CPU model | AMD EPYC-Milan Processor |
| logical CPUs | 4 |
| core topology | 2 physical cores, SMT=2 |
| benchmark CPU | CPU2 unless otherwise stated |
| SMT sibling set | CPU2 sibling set = `2-3` |
| NUMA topology | 1 NUMA node, node0 CPUs = `0-3` |
| memory | about 16 GB |
| kernel | Ubuntu Linux `7.0.0-15-generic` |
| compiler | g++ `15.2.0` |
| CMake | `4.2.3` |

The local development machine is not used as the measurement environment for the results interpreted here.

## Existing Benchmark Setup

### HFT Macro Benchmark

The standard macro benchmark is driven by:

```text
benchmark/scripts/run_benchmarks.sh
benchmark/src/hft/bench_hft_macro.cpp
benchmark/src/benchmark_runner.cpp
```

The default script settings are:

| Setting | Default |
|---|---:|
| scenarios | `hft_macro` |
| metrics | `latency,pmc` |
| orders | `100000` |
| levels | `100` |
| batch size | `100000` |
| trials | `5` |
| measured iterations | `1` |
| warmup iterations | `1` |
| seed | `42` |

The important measurement property is that the benchmark is **batch measured**.

For latency mode, each measured iteration does:

```text
Setup()                  untimed
t0 = steady_clock::now()
for batch_idx in batch_size:
    RunOp()
t1 = steady_clock::now()
Teardown()               untimed
ns/op = (t1 - t0) / batch_size
```

This means the normal macro benchmark does not insert a timing read after every operation. It records one timing window around a large batch and divides by the number of operations.

For PMC mode, the shape is similar:

```text
Setup()                  untimed
perf.ResetEnable()
for batch_idx in batch_size:
    RunOp()
perf.Disable()
perf.ReadValues()         outside the measured batch
Teardown()                untimed
counter/op = counter_delta / batch_size
```

This keeps the hot path close to production execution. The measurement overhead is amortized across the batch rather than paid by each operation.

### Perf Record Benchmark

Instruction-level profiling is driven by:

```text
benchmark/scripts/run_hft_macro_perf_record.sh
```

The default workload settings are:

| Setting | Default |
|---|---:|
| orders | `100000` |
| levels | `100` |
| batch size | `1000000` |
| warmup iterations | `1` |
| measured iterations | `40` |
| events | `cycles,branch-misses` |
| sampling frequency | `8000` |
| call graph | `dwarf` |

This script intentionally avoids a naive:

```text
perf record ./bench_hft_macro
```

because `Setup()` is heavy: it rebuilds book state, replays a 500k-event warmup, and pre-generates the measured batch. A naive recording would be dominated by benchmark scaffolding rather than matching-engine hot paths.

Instead, the runner uses perf's control FIFO. `perf record` starts disabled, and the benchmark process enables sampling only around the measured `RunOp()` batch:

```text
Setup()                         unsampled
perf record enable
batch_size x RunOp()            sampled
perf record disable
Teardown()                      unsampled
```

The program does not manually read hardware counters per operation in this mode. The kernel perf machinery samples asynchronously inside the enabled window.

## Why Per-Scenario Benchmarking Is Needed

The macro benchmark is the correct top-level decision metric, but it intentionally compresses the whole workload into aggregate numbers:

- ns/op
- cycles/op
- instructions/op
- branches/op
- branch misses/op
- cache misses/op
- function-level and instruction-level perf profiles

Those results explain whether a version is faster and which functions consume cycles. They do not directly answer which business scenario caused the latency distribution.

This became important after Phase 8. The array side book made the overall macro workload faster, but the remaining hot spots were not evenly distributed across operations. In particular:

- `cancel_order` is common, but its path can be very short unless it empties a best level;
- `add_rest_existing_level` should be mostly an append into an existing level;
- `add_rest_new_level` may need to activate a price level and update occupancy state;
- `modify_order` is semantically a cancel plus an add;
- crossing limit orders and market orders may perform many internal matches, so they are not comparable to a single small operation.

A single macro average cannot separate those paths. A micro benchmark can isolate them, but loses the real macro context: current book shape, hot levels, cancel clusters, realistic order lifetime, and the deterministic pre-generated event stream.

The per-scenario benchmark is the compromise:

- keep the real HFT macro workload;
- keep the pre-generated operation list;
- replay every operation so book state evolves realistically;
- only record per-call latency for selected single-operation scenarios.

The goal is not to replace the macro benchmark. The goal is to explain it.

## Per-Scenario Benchmark Design

The new diagnostic collector lives in:

```text
benchmark/src/hft/bench_hft_macro_scenarios.cpp
benchmark/scripts/run_hft_macro_scenarios.sh
benchmark/scripts/plot_hft_macro_scenarios.py
```

The default script settings are:

| Setting | Default |
|---|---:|
| trials | `10` |
| orders | `100000` |
| levels | `100` |
| batch size | `100000` |
| measured iterations | `1` |
| warmup iterations | `1` |
| focus | `all` |
| seed | `42` |

The collector currently measures three scenarios:

| Scenario | Meaning |
|---|---|
| `add_rest_existing_level` | a successful resting limit add where the price level already existed before the add |
| `add_rest_new_level` | a successful resting limit add where the price level did not exist before the add |
| `cancel_order` | a successful cancel of a resting order |

Other operations are replayed but not timed:

- crossing limit adds;
- market orders;
- modify orders;
- failed or unmeasured fallback events.

This is deliberate. Crossing adds and market orders can contain many internal matches, so they are not clean single-operation latency samples. Modify is also excluded because it is conceptually cancel plus add.

### Measurement Isolation

The initial implementation had an important trap: measuring all scenarios in a single replay polluted the latency path. If `add_rest_*` operations are instrumented in the same replay as `cancel_order`, the add-side measurement overhead changes the CPU state seen by later cancels.

The fixed design restores measurement isolation.

For `--focus all`, the benchmark replays the same deterministic workload once per measured scenario:

```text
replay 1: measure add_rest_existing_level only
replay 2: measure add_rest_new_level only
replay 3: measure cancel_order only
```

In each replay:

- focused operations are wrapped with `rdtsc/rdtscp` and `steady_clock`;
- non-focused operations are executed normally;
- book state still evolves through the full macro sequence;
- composition counts are recorded once, not once per replay.

This preserves the "complete per-call data" requirement while avoiding cross-scenario instrumentation pollution.

The CSV contains one row per measured scenario call, including:

- scenario name;
- trial id;
- operation index;
- scenario-local call index;
- side;
- price;
- quantity;
- raw cycles;
- overhead-adjusted cycles;
- raw elapsed ns;
- overhead-adjusted elapsed ns.

## Limitations

The per-scenario benchmark is useful, but it is not a clean production-speed measurement.

Every measured `RunOp()` is wrapped by:

```text
steady_clock::now()
lfence + rdtsc
RunOp()
rdtscp + lfence
steady_clock::now()
samples.push_back(...)
```

This changes the machine state around the operation. The effects include:

- pipeline serialization from `lfence` and `rdtscp`;
- front-end disruption from the measurement branch and additional instructions;
- possible branch predictor state changes;
- extra stores into the sample vector;
- cache-line traffic from sample recording;
- possible interaction with store buffers and load/store scheduling;
- `steady_clock` / vDSO path overhead;
- larger instruction footprint than the production hot loop.

This matters because the operations are extremely short. A `cancel_order` p50 can be around 10 ns in adjusted elapsed time, so even a small amount of instrumentation can affect the tail distribution.

The benchmark mitigates the worst problem by measuring one scenario per replay. That prevents add-side instrumentation from polluting cancel-side measurement. It does not eliminate the fact that the focused operation itself is measured with invasive per-op instrumentation.

Therefore the interpretation should be:

- use standard `hft_macro` latency/PMC for final performance decisions;
- use `perf record` for instruction-level hot-path attribution;
- use per-scenario benchmark for relative diagnosis inside the macro workload;
- avoid treating per-scenario absolute ns values as production latency.

## Preliminary Result

Artifact:

```text
server_results/hft_macro_scenarios_20260610_200842/
```

Environment summary:

| Field | Value |
|---|---|
| commit | `cfb81c9` |
| trials | `10` |
| focus | `all` |
| seed | `42` |
| orders | `100000` |
| levels | `100` |
| batch size | `100000` |
| server | Hetzner Cloud CCX23 |
| virtualization | KVM guest |
| CPU | AMD EPYC-Milan Processor |
| topology | 4 logical CPUs, 2 physical cores, SMT=2 |
| NUMA | 1 node, node0 CPUs = `0-3` |
| kernel | Ubuntu Linux `7.0.0-15-generic` |
| compiler | g++ `15.2.0` |

The output CSV contains 937,410 measured calls:

| Scenario | Calls | Share |
|---|---:|---:|
| `add_rest_existing_level` | 91,810 | 9.79% |
| `add_rest_new_level` | 384,300 | 41.00% |
| `cancel_order` | 461,300 | 49.21% |

Each trial has the same deterministic scenario composition:

| Scenario | Calls per trial |
|---|---:|
| `add_rest_existing_level` | 9,181 |
| `add_rest_new_level` | 38,430 |
| `cancel_order` | 46,130 |

Adjusted CPU-cycle results:

| Scenario | Mean | p50 | p95 | p99 | p99.5 | p999 |
|---|---:|---:|---:|---:|---:|---:|
| `add_rest_existing_level` | 64.32 | 76 | 114 | 152 | 190 | 418 |
| `add_rest_new_level` | 100.34 | 76 | 228 | 494 | 608 | 1254 |
| `cancel_order` | 38.60 | 38 | 38 | 76 | 114 | 380 |

Adjusted elapsed-time results:

| Scenario | Mean ns | p50 ns | p95 ns | p99 ns | p99.5 ns | p999 ns |
|---|---:|---:|---:|---:|---:|---:|
| `add_rest_existing_level` | 20.57 | 13 | 31 | 50 | 62 | 203 |
| `add_rest_new_level` | 31.98 | 20 | 73 | 144 | 193 | 414 |
| `cancel_order` | 13.78 | 11 | 22 | 33 | 40 | 196 |

Pooled latency distributions (10 trials, 100 bins, x-axis clipped at max(p99.5, p999); stats box and dotted vertical line mark p999):

![Untuned per-scenario distributions](phase9_untuned_20260610_200842_distributions.png)

The key observations are:

1. `cancel_order` is very cheap in the common case.

   Its cycle distribution is sharply concentrated: p50 and p95 are both 38 adjusted cycles, and p99 is only 76 adjusted cycles. This supports the decision not to over-optimize cancel unless perf shows a specific empty-best-level path dominating.

2. `add_rest_existing_level` is modestly more expensive than cancel but still controlled.

   The p50 is 76 cycles and p99 is 152 cycles. This is consistent with an append into an already active price level.

3. `add_rest_new_level` is the main tail-latency source among the measured single-operation paths.

   Its p50 is also 76 cycles, but the distribution has a much longer tail: p95 228 cycles, p99 494 cycles, and p999 1254 cycles. This is the path that creates or activates a new price level and interacts with occupancy state.

4. The split between existing-level add and new-level add was necessary.

   A single `add_rest` bucket would hide a large difference in tail behavior. The per-scenario benchmark shows that the expensive add tail is not a generic add problem; it is concentrated in new-level adds.

5. Extreme maximum values should not be over-interpreted.

   The CSV contains rare large outliers, but the plotted distribution extends the x-axis to max(p99.5, p999) and annotates p999 explicitly. Values beyond that tail are stacked in the last histogram bar.

## Linux System-Level Measurement Hygiene

The current per-scenario result is useful, but the benchmark environment still has avoidable OS-level noise. Phase 9 should continue by tightening the Linux execution environment before drawing conclusions from p99.9 or max latency.

The first tuned runner is:

```text
benchmark/scripts/run_remote_hft_macro_scenarios_tuned.sh
```

It follows the existing remote benchmark pipeline style:

1. SSH to the benchmark server.
2. Clone/fetch/checkout the repository.
3. Build and test.
4. Apply benchmark-oriented Linux hygiene.
5. Run `run_hft_macro_scenarios.sh`.
6. Generate the per-scenario distribution plot.
7. Package and download artifacts.

The tuning is intentionally conservative. It improves measurement hygiene but avoids irreversible system changes.

### CPU Core Binding

The tuned runner binds the benchmark to a fixed logical CPU:

```text
numactl --physcpubind=<cpu> --membind=<node>
```

This preserves L1/L2 cache, branch predictor, and local CPU state more consistently across the run.

The auto-selection logic was updated after the first tuned run. It now prefers a logical CPU whose SMT sibling set does **not** include CPU0. This avoids sharing a physical core with CPU0, which often carries timer and housekeeping noise.

On the 2026-06-11 cloud VM:

```text
CPU topology: 4 logical CPUs, 2 cores, SMT=2
CPU0 sibling set: 0-1
CPU2 sibling set: 2-3
```

The first tuned run selected CPU1, which still shared a physical core with CPU0:

```text
server_results/hft_macro_scenarios_tuned_20260611_224657/
bench_cpu=1
smt_siblings=0-1
```

After the auto-selection fix, later tuned runs selected CPU2:

```text
server_results/hft_macro_scenarios_tuned_20260611_232354/
server_results/hft_macro_scenarios_tuned_20260611_233122/
bench_cpu=2
smt_siblings=2-3
```

### NUMA Binding

On multi-NUMA systems, bind CPU and memory to the same node:

```bash
numactl --physcpubind=<cpu> --membind=<node> ...
```

The Hetzner CCX23 benchmark VM reports one NUMA node:

```text
available: 1 nodes (0)
node 0 cpus: 0 1 2 3
```

Even so, the tuned runner records the NUMA topology and uses `numactl` when available so that multi-node machines are handled correctly.

### Performance Governor

Use the performance CPU governor to reduce frequency-scaling noise:

```bash
sudo cpupower frequency-set -g performance
```

This is especially important when comparing elapsed ns distributions.

On the tested KVM VM, the cpufreq governor files were not exposed, so the governor snapshot in the result is empty. The script still records `governors_before`, `governors_during`, and `governors_after` when the platform exposes them.

### Avoid SMT Sibling Interference

If SMT is enabled, avoid running benchmark work on a logical CPU whose sibling is busy. The ideal setup is either:

- bind to a physical core and keep its sibling idle;
- or disable SMT for benchmark runs.

This reduces shared frontend, execution-port, and cache interference.

The tuned runner now records:

```text
bench_cpu=<cpu>
smt_siblings=<sibling-list>
run_prefix=<taskset-or-numactl command>
```

This makes every result self-describing.

### Reduce Background Noise

Keep unrelated work off the benchmark machine where possible:

- package managers;
- indexing services;
- logging bursts;
- cloud monitoring agents;
- other perf/tracing tools;
- cron jobs;
- unrelated compile jobs.

The goal is not mainly to improve p50. The main goal is to make p99 and p99.9 reflect matching-engine behavior rather than OS scheduling, interrupts, frequency transitions, or VM noise.

The tuned runner records before/after snapshots of:

```text
uptime
top CPU-consuming processes
systemd timers
```

It also stops common noisy timers and services when possible:

```text
apt-daily
apt-daily-upgrade
man-db
plocate/updatedb
fstrim
```

### Kernel Activity Snapshot

The next addition was explicit kernel-activity observation. The tuned runner now records before/after snapshots of:

```text
/proc/interrupts
/proc/softirqs
/proc/schedstat
```

It also writes:

```text
kernel_activity_delta.txt
```

This file reports per-CPU deltas and highlights the benchmark CPU. This is useful because binding the benchmark thread is not enough: kernel interrupts and softirqs can still run on the same CPU.

The 2026-06-11 run after CPU0-sibling avoidance showed:

```text
server_results/hft_macro_scenarios_tuned_20260611_232354/

bench_cpu=2
interrupts on CPU2 total = 15592
LOC local timer interrupts on CPU2 = 15429

softirqs on CPU2 total = 2266
RCU   on CPU2 = 1222
TIMER on CPU2 = 622
SCHED on CPU2 = 370
NET_RX on CPU2 = 52
```

This showed that CPU binding and background-noise reduction are not the whole story. Local timer interrupts and kernel housekeeping still run on the benchmark CPU.

### IRQ Affinity Snapshot

The tuned runner then gained IRQ affinity snapshots:

```text
irq_affinity_before.txt
irq_affinity_after.txt
```

Each row records:

```text
irq
smp_affinity_list
smp_affinity
effective_affinity_list
effective_affinity
label
```

`kernel_activity_delta.txt` now also prints affinity information for the top interrupt rows.

The result:

```text
server_results/hft_macro_scenarios_tuned_20260611_233122/
```

showed two concrete device IRQs hitting the benchmark CPU:

```text
IRQ 38 virtio5-request:
  bench_cpu_delta=105
  smp_affinity_list=2
  effective_affinity_list=2

IRQ 43 virtio1-input.0:
  bench_cpu_delta=44
  smp_affinity_list=0-3
  effective_affinity_list=2
```

This is a useful distinction:

- IRQ 38 is explicitly pinned to the benchmark CPU.
- IRQ 43 is allowed on all CPUs but effectively routed to the benchmark CPU.
- `LOC`, `RCU`, `TIMER`, and `SCHED` are not ordinary numbered device IRQs and require stronger kernel-level isolation if they need to be reduced further.

## Tuned Results So Far

The first tuned run:

```text
server_results/hft_macro_scenarios_tuned_20260611_224657/
```

used CPU1, which shared a physical core with CPU0. Even so, the improvement was immediate:

| Scenario | Untuned elapsed p99 | Tuned elapsed p99 |
|---|---:|---:|
| `add_rest_existing_level` | 50 ns | 21 ns |
| `add_rest_new_level` | 144 ns | 61 ns |
| `cancel_order` | 33 ns | 12 ns |

![First tuned run distributions (CPU1, shared core with CPU0)](phase9_tuned_20260611_224657_distributions.png)

The later 10-trial run with CPU2:

```text
server_results/hft_macro_scenarios_tuned_20260611_232354/
```

produced:

| Scenario | cycles p50 | cycles p95 | cycles p99 | cycles p999 | elapsed p50 | elapsed p95 | elapsed p99 | elapsed p999 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `add_rest_existing_level` | 22 | 44 | 44 | 66 | 11 ns | 21 ns | 22 ns | 50 ns |
| `add_rest_new_level` | 22 | 88 | 154 | 286 | 11 ns | 41 ns | 71 ns | 132 ns |
| `cancel_order` | 22 | 44 | 44 | 44 | 10 ns | 20 ns | 21 ns | 22 ns |

![CPU2 tuned run distributions (10 trials pooled)](phase9_tuned_20260611_232354_distributions.png)

The next 10-trial run with IRQ affinity observation:

```text
server_results/hft_macro_scenarios_tuned_20260611_233122/
```

was similar, with a slightly larger p999 tail:

| Scenario | cycles p50 | cycles p95 | cycles p99 | cycles p999 | elapsed p50 | elapsed p95 | elapsed p99 | elapsed p999 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `add_rest_existing_level` | 22 | 44 | 44 | 66 | 11 ns | 21 ns | 22 ns | 40.2 ns |
| `add_rest_new_level` | 44 | 88 | 154 | 308 | 11 ns | 41 ns | 71 ns | 150 ns |
| `cancel_order` | 22 | 44 | 44 | 44 | 10 ns | 21 ns | 21 ns | 31 ns |

![IRQ affinity observation run distributions (10 trials pooled)](phase9_tuned_20260611_233122_distributions.png)

The important conclusion is not that the exact p999 values are fixed. It is that the tuned setup dramatically reduces the system-noise component of the per-scenario distribution, while the kernel snapshots now make the remaining noise visible.

## Final System-Level Tuning Stage

The final part of Phase 9 tested whether the remaining per-scenario p99 tail was still mostly caused by Linux scheduling, interrupts, softirqs, or VM housekeeping.

Three progressively stronger configurations were tested on the same cloud VM and commit:

| Stage | Artifact | Description |
|---|---|---|
| dynamic CPU + IRQ move | `server_results/hft_macro/scenarios_tuned/hft_macro_scenarios_tuned_20260612_002441/` | automatic clean-CPU selection, NUMA binding, IRQ affinity movement away from the benchmark CPU |
| aggressive isolation | `server_results/hft_macro/scenarios_tuned/hft_macro_scenarios_tuned_20260612_005335/` | SMT-sibling IRQ avoidance, workqueue cpumask movement, `chrt -f 95`, watchdog off, RT throttling off, CPU DMA latency lock |
| boot-time `nohz_full` | `server_results/hft_macro/scenarios_tuned/hft_macro_scenarios_tuned_20260612_014047/` | `nohz_full`, `rcu_nocbs`, `isolcpus`, `irqaffinity`, and `kthread_cpus` applied through GRUB and reboot |

The `nohz_full` setup result is:

```text
server_results/nohz_full_setup_20260612_023844/
```

The post-reboot verification confirmed that the kernel command line contained:

```text
nohz_full=2,3
rcu_nocbs=2,3
isolcpus=nohz,domain,managed_irq,2,3
irqaffinity=0,1
kthread_cpus=0,1
nowatchdog
nmi_watchdog=0
```

and:

```text
/sys/devices/system/cpu/nohz_full=2-3
```

The benchmark itself continued to run on CPU2:

```text
chrt -f 95 numactl --physcpubind=2 --membind=0
```

### Kernel Activity Effect

The system-level tuning did reduce kernel activity on CPU2.

| Stage | LOC on CPU2 | IRQ38 on CPU2 | RCU on CPU2 | TIMER on CPU2 | SCHED on CPU2 | Softirq total on CPU2 |
|---|---:|---:|---:|---:|---:|---:|
| dynamic CPU + IRQ move | 15590 | 102 | 1239 | 635 | 380 | 2254 |
| aggressive isolation | 15319 | 106 | 1332 | 639 | 368 | 2339 |
| `nohz_full` | 4695 | 113 | 117 | 11 | 91 | 219 |

This proves that `nohz_full` and `rcu_nocbs` were not a no-op. They reduced benchmark-CPU softirqs by roughly an order of magnitude and also reduced local timer interrupt activity substantially.

However, not everything can be moved:

- `IRQ38 virtio5-request` remained pinned to CPU2.
- Attempts to move it failed with `Operation not permitted`.
- `LOC` was reduced but not eliminated, which is expected for a guest VM.
- Some host/KVM behavior remains outside the guest kernel's control.

Pooled per-scenario distributions for the three final tuning stages (10 trials each):

![Dynamic CPU selection + IRQ move](phase9_tuned_20260612_002441_distributions.png)

![Aggressive isolation (workqueue cpumask, chrt FIFO, watchdog off)](phase9_tuned_20260612_005335_distributions.png)

![Boot-time nohz_full isolation (CPU2–3 isolated, housekeeping on CPU0–1)](phase9_tuned_20260612_014047_distributions.png)

### Latency Effect

Despite the cleaner kernel counters, the p99 latency distribution did not materially improve.

Adjusted cycle results:

| Stage | Scenario | Mean | p50 | p95 | p99 | p999 |
|---|---|---:|---:|---:|---:|---:|
| dynamic CPU + IRQ move | `add_rest_existing_level` | 29.19 | 22 | 44 | 44 | 66 |
| dynamic CPU + IRQ move | `add_rest_new_level` | 37.91 | 22 | 88 | 154 | 286 |
| dynamic CPU + IRQ move | `cancel_order` | 18.98 | 22 | 44 | 44 | 44 |
| aggressive isolation | `add_rest_existing_level` | 33.74 | 22 | 44 | 44 | 66 |
| aggressive isolation | `add_rest_new_level` | 41.98 | 44 | 88 | 154 | 286 |
| aggressive isolation | `cancel_order` | 24.67 | 22 | 44 | 44 | 44 |
| `nohz_full` | `add_rest_existing_level` | 20.97 | 22 | 44 | 44 | 66 |
| `nohz_full` | `add_rest_new_level` | 31.74 | 22 | 88 | 154 | 286 |
| `nohz_full` | `cancel_order` | 13.03 | 0 | 44 | 44 | 44 |

The `0` cycle p50 for `nohz_full` `cancel_order` is an overhead-adjustment artifact caused by subtracting the measured timing overhead from an extremely short operation. It should not be interpreted as literal zero work; the p95/p99/p999 values are more meaningful for that path.

Adjusted elapsed-time results:

| Stage | Scenario | Mean ns | p50 ns | p95 ns | p99 ns | p999 ns |
|---|---|---:|---:|---:|---:|---:|
| dynamic CPU + IRQ move | `add_rest_existing_level` | 13.52 | 11 | 21 | 22 | 32 |
| dynamic CPU + IRQ move | `add_rest_new_level` | 17.42 | 11 | 41 | 71 | 137.61 |
| dynamic CPU + IRQ move | `cancel_order` | 8.38 | 10 | 21 | 21 | 22 |
| aggressive isolation | `add_rest_existing_level` | 16.31 | 11 | 21 | 22 | 40 |
| aggressive isolation | `add_rest_new_level` | 18.89 | 12 | 40 | 71 | 131 |
| aggressive isolation | `cancel_order` | 11.09 | 10 | 21 | 21 | 22 |
| `nohz_full` | `add_rest_existing_level` | 11.69 | 11 | 21 | 22 | 32 |
| `nohz_full` | `add_rest_new_level` | 17.90 | 11 | 41 | 80 | 141 |
| `nohz_full` | `cancel_order` | 8.87 | 10 | 21 | 21 | 31 |

The p99 cycle values are identical across all three final stages:

| Scenario | p99 cycles |
|---|---:|
| `add_rest_existing_level` | 44 |
| `add_rest_new_level` | 154 |
| `cancel_order` | 44 |

The elapsed p99 values are also effectively unchanged:

| Scenario | dynamic CPU + IRQ move | aggressive isolation | `nohz_full` |
|---|---:|---:|---:|
| `add_rest_existing_level` | 22 ns | 22 ns | 22 ns |
| `add_rest_new_level` | 71 ns | 71 ns | 80 ns |
| `cancel_order` | 21 ns | 21 ns | 21 ns |

This is the key final-stage result. The Linux-level tuning successfully removed a large amount of measured kernel activity, but the benchmark's p99 operation latency did not follow. Therefore the remaining p99 distribution is not primarily caused by ordinary movable IRQs, workqueues, watchdogs, RCU callbacks, or periodic scheduler ticks.

### Final Interpretation

The system-level work was still useful. It proved that:

1. The tuned per-scenario runner can select a clean CPU automatically.
2. Ordinary device IRQs can be moved away from the benchmark CPU.
3. SMT-sibling IRQ avoidance and workqueue pinning are observable in the artifacts.
4. Boot-time `nohz_full` reduces CPU2 `LOC` and softirq activity substantially.
5. These reductions do not translate into a meaningful p99 latency improvement for the current workload.

The remaining system-side noise is either:

- local timer / APIC / KVM activity that cannot be fully removed in the guest;
- pinned virtio queue IRQs such as IRQ38;
- residual VM host scheduling effects;
- or noise below the threshold that currently determines p99.

Further Linux tuning is unlikely to be a productive Phase 9 direction unless the environment changes to bare metal or a VM with controllable virtio queue affinity.

## Final Position

Phase 9 does not replace the macro benchmark. It adds a diagnostic lens.

The standard `hft_macro` benchmark remains the metric for release-level performance decisions. `perf record` remains the tool for instruction-level attribution. The per-scenario benchmark is useful because it preserves the real macro workload while exposing which single-operation classes generate latency tails.

The first per-scenario result was actionable: `add_rest_new_level` is the expensive measured path, while `cancel_order` is already extremely compact in the common case.

The system-level tuning work then established a clean measurement framework:

- benchmark CPU and NUMA binding;
- SMT-sibling awareness;
- dynamic clean-CPU selection;
- IRQ affinity snapshots and movement;
- kernel activity snapshots;
- aggressive optional isolation controls;
- boot-time `nohz_full` validation.

The final `nohz_full` experiment closes the system-tuning thread. It reduced observable kernel activity but did not reduce p99 latency. That makes the next optimization target clear: return to matching-engine internals, especially the `add_rest_new_level` path and the work needed to activate a previously empty price level.

For Phase 10, the strongest candidate is no longer Linux hygiene. It is instruction-count reduction inside the add-new-level path.
