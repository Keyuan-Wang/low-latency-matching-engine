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
| CPU | AMD Ryzen 7 7840HS |
| kernel | WSL2 Linux 6.6.87.2 |
| compiler | g++ 13.3.0 |

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

| Scenario | Mean | p50 | p95 | p99 | p99.5 | p99.9 |
|---|---:|---:|---:|---:|---:|---:|
| `add_rest_existing_level` | 64.32 | 76 | 114 | 152 | 190 | 418 |
| `add_rest_new_level` | 100.34 | 76 | 228 | 494 | 608 | 1254 |
| `cancel_order` | 38.60 | 38 | 38 | 76 | 114 | 380 |

Adjusted elapsed-time results:

| Scenario | Mean ns | p50 ns | p95 ns | p99 ns | p99.5 ns | p99.9 ns |
|---|---:|---:|---:|---:|---:|---:|
| `add_rest_existing_level` | 20.57 | 13 | 31 | 50 | 62 | 203 |
| `add_rest_new_level` | 31.98 | 20 | 73 | 144 | 193 | 414 |
| `cancel_order` | 13.78 | 11 | 22 | 33 | 40 | 196 |

The key observations are:

1. `cancel_order` is very cheap in the common case.

   Its cycle distribution is sharply concentrated: p50 and p95 are both 38 adjusted cycles, and p99 is only 76 adjusted cycles. This supports the decision not to over-optimize cancel unless perf shows a specific empty-best-level path dominating.

2. `add_rest_existing_level` is modestly more expensive than cancel but still controlled.

   The p50 is 76 cycles and p99 is 152 cycles. This is consistent with an append into an already active price level.

3. `add_rest_new_level` is the main tail-latency source among the measured single-operation paths.

   Its p50 is also 76 cycles, but the distribution has a much longer tail: p95 228 cycles, p99 494 cycles, and p99.9 1254 cycles. This is the path that creates or activates a new price level and interacts with occupancy state.

4. The split between existing-level add and new-level add was necessary.

   A single `add_rest` bucket would hide a large difference in tail behavior. The per-scenario benchmark shows that the expensive add tail is not a generic add problem; it is concentrated in new-level adds.

5. Extreme maximum values should not be over-interpreted.

   The CSV contains rare large outliers, but the plotted distribution intentionally focuses on the body through p99.5. Those extreme values are likely dominated by system noise, scheduling, virtualization, or interrupt effects rather than deterministic matching-engine work.

## Next Step: Linux System-Level Measurement Hygiene

The current per-scenario result is useful, but the benchmark environment still has avoidable OS-level noise. Phase 9 should continue by tightening the Linux execution environment before drawing conclusions from p99.9 or max latency.

The next system-level work should prioritize:

### CPU Core Binding

Run the benchmark on a fixed CPU to avoid scheduler migration:

```bash
taskset -c <cpu> benchmark/scripts/run_hft_macro_scenarios.sh
```

This preserves L1/L2 cache, branch predictor, and local CPU state more consistently across the run.

### NUMA Binding

On multi-NUMA systems, bind CPU and memory to the same node:

```bash
numactl --physcpubind=<cpu> --membind=<node> ...
```

The current WSL environment reports one NUMA node, but cloud or bare-metal benchmark machines may not.

### Performance Governor

Use the performance CPU governor to reduce frequency-scaling noise:

```bash
sudo cpupower frequency-set -g performance
```

This is especially important when comparing elapsed ns distributions.

### Avoid SMT Sibling Interference

If SMT is enabled, avoid running benchmark work on a logical CPU whose sibling is busy. The ideal setup is either:

- bind to a physical core and keep its sibling idle;
- or disable SMT for benchmark runs.

This reduces shared frontend, execution-port, and cache interference.

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

## Final Position

Phase 9 does not replace the macro benchmark. It adds a diagnostic lens.

The standard `hft_macro` benchmark remains the metric for release-level performance decisions. `perf record` remains the tool for instruction-level attribution. The per-scenario benchmark is useful because it preserves the real macro workload while exposing which single-operation classes generate latency tails.

The first result is actionable: `add_rest_new_level` is the expensive measured path, while `cancel_order` is already extremely compact in the common case. Before optimizing further based on per-scenario p99.9 data, the benchmark should be rerun under a more controlled Linux system configuration.
