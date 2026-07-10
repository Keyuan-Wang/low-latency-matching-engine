# HFT Macro Operation-Scoped PMC Profiling Design

## Purpose

The goal is to profile `hft_macro` at the operation level, not at the whole
process level.

Whole-process `perf stat` is intentionally avoided because it would include
benchmark setup, warmup, measured-batch generation, logging, CSV output, and
runner overhead. Those costs are useful for system-level benchmarking, but they
do not answer which matching-engine operation is expensive.

This design measures only the replayed `OrderBook` operation inside
`BenchHftMacro::RunOp()`:

```text
reset/enable PMC group
execute one pre-generated book operation
disable/read PMC group
attribute counters to that operation bucket
```

The operation buckets are:

```text
add_rest
add_cross
cancel_hit
cancel_miss
modify_hit
modify_miss
market
```

## Counter Groups

The cloud benchmark machine is fixed, so the counter groups are hard-coded
instead of dynamically probed.

The script runs one group at a time. The default set is:

| Group | Events |
|---|---|
| `core` | `cycles`, `instructions`, `branches`, `branch_misses` |
| `cache` | `cache_references`, `cache_misses` |
| `l1d` | `l1d_loads`, `l1d_load_misses` |
| `l2` | `l2_dc_accesses_from_l1_misses`, `l2_dc_hits_from_l1_misses`, `l2_dc_misses_from_l1_misses` |
| `dtlb` | `dtlb_loads`, `dtlb_load_misses` |

The implementation uses Linux `perf_event_open` through the existing
`PerfGroup` wrapper. It does not call `perf stat`.

Running one small group at a time keeps the measurement synchronized and avoids
mixing unrelated counters into a large multiplexed group.

The `l2` group uses AMD Milan raw core-PMU events verified on the target cloud
machine:

```text
l2_cache_accesses_from_dc_misses: event=0x60, umask=0xe8
l2_cache_hits_from_dc_misses:     event=0x64, umask=0xf0
l2_cache_misses_from_dc_misses:   event=0x64, umask=0x08
```

The code also contains optional `l2_all`, `l2_fill`, and `llc` groups. `llc` is
based on Linux's generic `PERF_COUNT_HW_CACHE_LL` encoding, but it is not part
of the default script because the target cloud machine rejected it through
`perf_event_open` with `ENOENT`. Perf exposes L3-related metrics such as
`l3_read_miss_latency`, but those are metric expressions rather than directly
openable operation-scoped events on this machine. Therefore L3 is not included
in the default operation-scoped PMC campaign.

Optional groups can still be requested explicitly:

```text
PMC_GROUPS="l2_all l2_fill"
```

## Output

The operation PMC CSV is long-form:

```text
scenario,version_tag,commit_sha,trial_id,seed,orders,levels,batch_size,
warmup_iters,iters,pmc_group,op_type,count,share,event_name,event_total,
event_per_op,weighted_event_per_macro_op
```

This schema is designed for direct operation-level analysis:

- `event_per_op` shows the average hardware cost of one operation of that type.
- `weighted_event_per_macro_op` combines operation cost with operation share.
- `count` and `share` allow the PMC result to be interpreted together with the
  macro workload mix.

For example, `add_rest` can have lower event cost than `modify_hit` but still
dominate the macro benchmark if its operation share is much larger.

## Automation

The automation script is:

```text
benchmark/scripts/run_hft_macro_op_pmc_groups.sh
```

Default behavior:

```text
PMC_GROUPS="core cache l1d l2 dtlb"
TRIALS=10
ORDERS=100000
LEVELS=100
BATCH_SIZE=100000
WARMUP_ITERS=1
ITERS=1
```

The script builds `bench_hft_macro` with:

```text
LLMES_PROFILE_HFT_MACRO_OPS=ON
LLMES_PROFILE_HFT_MACRO_OP_PMCS=ON
```

Then it runs `hft_macro` once per counter group and stores:

```text
benchmark/results/hft_macro_op_pmc_raw.csv
benchmark/results/hft_macro_op_pmc_logs/
```

Optional per-operation latency CSV output can be enabled with:

```text
WRITE_OP_LATENCY=1
```

Latency recorded during PMC profiling should be treated as profiling-mode data,
because enabling and disabling hardware counters around each operation changes
the benchmark execution environment. The primary purpose of this mode is PMC
attribution by operation type.
