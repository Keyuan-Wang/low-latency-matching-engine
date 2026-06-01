# Phase 5 Macro Profiling Plan

## Context

Phase 4 ended with a corrected `hft_macro` benchmark and operation-level
profiling. The important fix was removing measured-batch tracking drift: cancel
and modify targets now come from live book state, so the macro workload no
longer produces a large artificial `cancel_miss` population.

Phase 5 starts from the repaired `master` branch and focuses on profiling before
making more data-structure changes.

## Latest Aligned Run

Cloud run:

```text
branch:       master
commit:       5186b6c
scenario:     hft_macro
trials:       1
orders:       100000
levels:       100
batch_size:   100000
warmup_iters: 1
iters:        1
seed:         42
```

Artifacts:

```text
server_results/macro_op_profile_cloud_master_pmc_t1_20260601/
```

The run contains two aligned measurements:

- `latency`: wall-clock macro and per-operation latency.
- `pmc`: in-process perf counters around the measured benchmark window.

## Overall Result

| Mode | avg_ns | ops_s | cycles/op | instructions/op | CPI | branch miss rate | cache misses/op | ok |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `latency` | 103.934 | 9.62145e+06 | - | - | - | - | - | 199981 |
| `pmc` | - | - | 361.417 | 670.021 | 0.539411 | 0.0192954 | 0.07932 | 199989 |

The PMC result does not look memory-bound. `cache_misses_per_op = 0.07932`,
which is roughly:

```text
0.07932 / 670.021 * 1000 = 0.118 cache misses per 1000 instructions
```

This is very low for a workload that would be dominated by LLC or DRAM misses.
The current macro benchmark is more consistent with fixed instruction-path cost
than with cache-miss latency.

## Operation-Level Result

The latency run gives the clearest weighted-cost picture:

| Operation | Share | Mean ns | p50 ns | p95 ns | p99 ns | Weighted time share |
|---|---:|---:|---:|---:|---:|---:|
| `add_rest` | 47.849% | 65.023 | 60 | 100 | 150 | 53.592% |
| `cancel_hit` | 46.159% | 43.507 | 40 | 60 | 60 | 34.592% |
| `modify_hit` | 3.929% | 117.144 | 100 | 220 | 290 | 7.928% |
| `market` | 1.597% | 119.105 | 80 | 250 | 410.4 | 3.276% |
| `add_cross` | 0.466% | 76.094 | 70 | 130 | 173.5 | 0.611% |
| `cancel_miss` | 0.000% | 0.000 | 0 | 0 | 0 | 0.000% |
| `modify_miss` | 0.000% | 0.000 | 0 | 0 | 0 | 0.000% |

The workload is now internally coherent:

- `cancel_miss = 0`
- `modify_miss = 0`
- `add_rest + add_cross ~= 48.3%`
- `cancel_hit ~= 46.2%`
- `market ~= 1.6%`

## Interpretation

### `add_rest` Is The Main Macro Cost Center

`add_rest` contributes more than half of measured macro time:

```text
47.849% event share * 65.023 ns mean = 53.592% weighted time share
```

It is not the slowest individual operation, but it is the highest-leverage path
because it is hit on nearly half of all macro events.

In HFT terms, this is also the quote-placement path. Reducing `add_rest` latency
can improve quote refresh speed and same-price FIFO positioning. That makes it
more commercially relevant than optimizing rare market sweeps in the current
workload.

### `cancel_hit` Is Important But Already Tight

`cancel_hit` is the second-largest weighted contributor, but its latency profile
is already strong:

```text
mean = 43.507 ns
p50  = 40 ns
p95  = 60 ns
p99  = 60 ns
```

The current O(1) cancel path appears healthy. It should be protected during
future changes, but the data does not point to cancel as the first optimization
target.

### `modify_hit` Is Expensive But Low Share

`modify_hit` is more expensive than `add_rest` and `cancel_hit`, but its event
share is only about 3.9%. Its weighted contribution is meaningful but secondary.
This result is expected because modify is structurally close to cancel plus add.

### `market` Is Not The Current Bottleneck

Market orders are low-frequency and shallow in this workload:

```text
market share ~= 1.6%
market_levels_p99 = 2
market_filled_qty_p99 = 5
```

Optimizing deep market sweeps is unlikely to move this macro benchmark unless
the workload model changes.

### Cache Locality Is Not The Leading Hypothesis

The low generic cache-miss rate weakens the hypothesis that macro performance is
currently dominated by cache misses from poor order locality. This does not
mean memory layout is irrelevant, but it does mean that the next optimization
should not be based on cache-locality intuition alone.

The ChunkPool experiment was useful because it tested that intuition. The
current profiling result suggests Phase 5 should instead identify which fixed
steps inside `add_rest` consume cycles.

## Next Work

Phase 5 should add function-level profiling inside the `add_rest` path before
changing core data structures again.

The profiling should break `OrderBook::add_limit_order` into measurable stages:

| Stage | Question |
|---|---|
| duplicate / pending-cancel checks | How much does front-door validation cost? |
| crossing check | Is best-price access and branch structure material? |
| price-level lookup / creation | Is the map lookup the dominant add cost? |
| order allocation | Is pool allocation still visible in the add path? |
| FIFO append | Is intrusive list maintenance measurable? |
| `id_to_order_` insertion | Is order-id indexing the dominant add cost? |
| best-price update | Is best-price maintenance visible? |

The goal is to produce a stage-level table for `add_rest`:

```text
stage, count, mean_ns, mean_cycles, weighted_ns_per_add_rest
```

Once that table exists, optimization candidates can be ranked by measured cost
rather than by intuition. The likely decision points are:

- If price-level lookup dominates, revisit price-index structure.
- If `id_to_order_` insertion dominates, investigate order-id indexing.
- If allocation dominates, revisit pool implementation.
- If no single stage dominates, optimize instruction count and branch structure
  in the add path.

## Working Rule For Phase 5

Do not introduce another major storage redesign until the `add_rest`
function-level profile identifies a concrete cost center.

The next change should be profiling instrumentation, not a production
optimization.
