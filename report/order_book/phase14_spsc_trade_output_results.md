# Phase 14: Async SPSC Trade Output Results

## Context

This phase tested whether trade/result output should stay on the matching thread or be moved through an SPSC queue to a consumer thread.

The benchmark compares two complete output paths:

- `hft_macro_overall_vector`: matching thread writes trades into a vector, then drains and attaches them to the result buffer.
- `hft_macro_spsc_async`: matching thread publishes trade events into an SPSC queue; a consumer thread pops events and writes them into the result buffer.

The old mainline result is included only as historical context:

- `hft_macro`: legacy `AddResult` with embedded `std::vector<Trade>`.

It is not a pure book-only / `NullTradeSink` measurement.

Source data:

- `server_results/matching_core_campaign_20260622_204849/results/matching_core_campaign_merged_agg.csv`
- Cloud environment: tuned Linux setup with `nohz_full=2,3`, `isolcpus`, `rcu_nocbs`, `irqaffinity=0,1`, `nowatchdog`, `chrt -f 95`, `numactl --physcpubind=2 --membind=0`
- LTO enabled
- 10 trials, 100,000 ops per trial

## Results

| Scenario | avg ns/op | cycles/op | instr/op | branches/op | branch misses/op | cache misses/op | ops/s |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| legacy embedded-vector `AddResult` | 14.86 | 54.81 | 94.83 | 17.08 | 1.234 | 0.020 | 67.28M |
| vector output | 23.33 | 86.56 | 204.36 | 26.77 | 1.220 | 0.040 | 42.87M |
| async SPSC output | 21.10 | 77.44 | 171.95 | 21.48 | 1.213 | 0.034 | 47.40M |

Compared with vector output, async SPSC reduces:

- average latency by **9.55%**
- cycles/op by **10.54%**
- instructions/op by **15.86%**
- branches/op by **19.78%**
- cache misses/op by **14.95%**

Throughput improves from **42.87M ops/s** to **47.40M ops/s**, a **10.57%** increase.

## Interpretation

The SPSC path wins because it moves part of result materialization away from the matching thread. The producer no longer needs to synchronously drain a vector and attach trades after every operation. It only publishes compact trade events into the queue.

The clean comparison here is between the two new output paths:

- vector output: **23.33 ns/op**
- async SPSC output: **21.10 ns/op**

The legacy `hft_macro` row is not a book-only baseline, so this report does not claim an output overhead relative to pure matching. It only claims that the async SPSC publication path is faster than the synchronous vector publication path under the new measurement definition.

The PMC data agrees with the latency result. SPSC does not merely hide wall-clock time; it lowers producer-side work:

- fewer instructions
- fewer branches
- fewer cache misses
- fewer cycles per op

## Conclusion

Async SPSC is a meaningful improvement for trade output. It does not make output free, but it cuts a large part of the cost added by synchronous vector materialization.

For this project, the clean design is:

- use a separate `NullTradeSink` benchmark when we want pure book latency
- use vector output as the simple synchronous baseline
- use async SPSC as the production-like output path

The current result supports keeping SPSC as the preferred trade publication mechanism.
