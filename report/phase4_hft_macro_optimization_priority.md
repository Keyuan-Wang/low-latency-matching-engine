# Phase 4 HFT Macro Profiling Findings

## What Was Measured

This report summarizes the repaired `hft_macro` operation-level profiling run
on `phase4-finale`.

The important context is that the earlier high `cancel_miss` rate was caused by
benchmark accounting drift. The measured batch was generated from a predictive
tracking map that could diverge from the actual book after market orders,
crossing limit orders, and modifies. After repairing measured-batch generation,
cancel and modify targets are selected from live book state.

Cloud profiling run:

```text
branch:       phase4-finale
commit:       efc1b67
script:       benchmark/scripts/run_hft_macro_op_profile.sh
trials:       1
orders:       100000
levels:       100
batch_size:   100000
warmup_iters: 1
iters:        1
metric:       latency
```

Artifacts:

```text
server_results/macro_op_profile_cloud_phase4_finale_t1/
```

Overall runner result:

| Metric | Value |
|---|---:|
| `avg_ns` | 110.793 |
| `ops_s` | 9.026e+06 |
| `ok` | 199977 |

## Result

Operation profile:

| Operation | Count | Share | Mean ns | p50 ns | p95 ns | p99 ns | Weighted time share |
|---|---:|---:|---:|---:|---:|---:|---:|
| `add_rest` | 47867 | 47.867% | 69.848 | 60.0 | 110.0 | 160.0 | 52.977% |
| `add_cross` | 469 | 0.469% | 76.311 | 70.0 | 140.0 | 190.0 | 0.567% |
| `cancel_hit` | 46153 | 46.153% | 48.459 | 50.0 | 60.0 | 70.0 | 35.438% |
| `cancel_miss` | 0 | 0.000% | 0.000 | 0.0 | 0.0 | 0.0 | 0.000% |
| `modify_hit` | 3919 | 3.919% | 131.357 | 110.0 | 240.0 | 310.0 | 8.157% |
| `modify_miss` | 0 | 0.000% | 0.000 | 0.0 | 0.0 | 0.0 | 0.000% |
| `market` | 1592 | 1.592% | 113.417 | 90.0 | 260.0 | 431.8 | 2.861% |

Market-order intensity:

| Metric | Value |
|---|---:|
| `market_levels_mean` | 0.760 |
| `market_levels_p95` | 1 |
| `market_levels_p99` | 1 |
| `market_filled_qty_mean` | 0.996 |
| `market_filled_qty_p95` | 3 |
| `market_filled_qty_p99` | 5 |

The corrected profile is internally coherent:

- `cancel_miss = 0`, so cancel target generation now tracks live orders.
- `modify_miss = 0`, so modify targets also come from live state.
- `add_rest + add_cross = 48.336%`, close to the intended add-heavy flow.
- `cancel_hit = 46.153%`, close to the intended cancel-heavy flow.
- `market = 1.592%`, close to the intended low-frequency market flow.

## Analysis

### `add_rest` Is The Dominant Cost Center

`add_rest` is not the slowest individual operation, but it contributes the most
total measured time:

```text
weighted_time(add_rest) = 47.867% * 69.848 ns = 52.977% total measured time
```

This means macro performance is currently shaped more by high-frequency resting
adds than by rare expensive operations. A small improvement in `add_rest` has a
large aggregate effect because nearly half of all measured events go through
that path.

For example, a 10 ns improvement in `add_rest` would translate to roughly:

```text
0.47867 * 10 ns = 4.79 ns per macro event
```

The same 10 ns improvement in `market` would translate to only:

```text
0.01592 * 10 ns = 0.16 ns per macro event
```

This is the main performance reason `add_rest` has the highest optimization
leverage in the current profile.

There is also a trading interpretation. In a market-making engine, `add_rest`
is the quote-placement path. It affects FIFO queue position, quote refresh
speed, and the time needed to restore resting liquidity after book changes. In
that sense, `add_rest` is more directly tied to queue priority and alpha
capture than low-frequency market-order processing.

### `cancel_hit` Is Important But Already Tight

`cancel_hit` is the second-largest weighted contributor:

```text
weighted_time(cancel_hit) = 35.438%
```

Its absolute latency is low:

| Metric | Value |
|---|---:|
| mean | 48.459 ns |
| p50 | 50.0 ns |
| p95 | 60.0 ns |
| p99 | 70.0 ns |

This shows that the O(1) cancel path is functioning well. It is still important
because cancel speed is a risk-control property, but this profile does not show
an obvious cancel-path latency problem. The tail is also tight: p99 is only 70
ns.

The result suggests that the remaining macro headroom is more likely in the
add path than in the cancel path.

### `modify_hit` Is Expensive But Low Frequency

`modify_hit` has the highest mean latency among the common non-market paths:

```text
mean_ns(modify_hit) = 131.357 ns
```

However, its event share is only 3.919%, so it contributes 8.157% of total
weighted time. That is material, but it is not the primary macro bottleneck.

The cost is expected because modify is effectively cancel plus add. It pays for
order lookup/removal and then a fresh limit-order insertion. The profile is
consistent with `add_rest` being expensive: modify inherits that insertion
cost whenever the replacement order rests.

### `market` Is Not The Current Bottleneck

`market` has a higher mean latency than `cancel_hit`, but it is rare:

| Metric | Value |
|---|---:|
| event share | 1.592% |
| weighted time share | 2.861% |
| mean latency | 113.417 ns |
| p99 latency | 431.8 ns |

The market intensity data also shows that this workload is not stressing deep
sweeps:

| Metric | Value |
|---|---:|
| market levels mean | 0.760 |
| market levels p95 | 1 |
| market levels p99 | 1 |
| filled quantity mean | 0.996 |
| filled quantity p99 | 5 |

Most market operations touch at most one level and fill very little quantity.
Therefore, market-sweep optimization would have little impact on the current
macro result.

### The ChunkPool Hypothesis Is Weaker After This Result

The ChunkPool design was motivated by improving locality for orders within the
same price level. If same-level order locality were the dominant macro issue,
the profile should show pressure in paths that traverse or remove resting
orders:

- `cancel_hit`
- `market`
- the cancel portion of `modify_hit`

The repaired profile instead shows:

- `cancel_hit` is already fast and tight-tailed.
- `market` barely touches book depth.
- `add_rest` dominates weighted time.

This weakens the original cache-locality argument for ChunkPool as the primary
macro optimization. The dominant measured cost is now the high-frequency
resting-add path, which includes fixed insertion work and bookkeeping:

- duplicate and pending-cancel checks
- crossing check against the opposite best
- price-level lookup or creation
- order allocation
- FIFO append
- `id_to_order_` insertion

The profile does not prove which of those sub-costs dominates, but it does show
that the macro bottleneck is better explained by high-frequency add-path fixed
cost than by deep market sweeps or cancel traversal.

### Business Interpretation

From an HFT engineering perspective, the highest-profit-sensitive path in this
profile is `add_rest`.

The reason is not only that it has the largest weighted time share. Resting add
latency is closely tied to quote placement:

- faster quote insertion can improve same-price FIFO position
- faster quote refresh can restore liquidity after cancels or book movement
- lower add latency reduces the delay between strategy decision and visible
  book placement

`cancel_hit` remains strategically critical because it controls stale quote
risk, but the measured cancel path is already strong. `market` is too rare and
too shallow in this workload to be a major profit lever.

The practical interpretation of this result is:

```text
The current macro workload is primarily an add-resting-order workload with a
very fast cancel path, not a deep-sweep or cancel-miss workload.
```

