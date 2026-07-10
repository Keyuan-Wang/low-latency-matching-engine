# Benchmark scripts

Scripts are grouped by role. All paths are relative to the repo root.

```
benchmark/scripts/
  lib/          Shared shell helpers
  local/        Run on the current machine (build + benchmark + profile)
  remote/       SSH wrappers for cloud / dedicated bench hosts
  analysis/     Post-process CSVs and generate plots
```

## `lib/`

| File | Purpose |
|---|---|
| `bench_linux_isolation.sh` | CPU selection, IRQ/workqueue pinning, governor, numactl/chrt prefix |

## `local/` — local execution

| Script | Purpose |
|---|---|
| `benchmarks.sh` | Macro matrix: latency + PMC trials → raw CSVs |
| `hft_macro_scenarios.sh` | Per-scenario call attribution CSV |
| `hft_macro_perf_record.sh` | Window-isolated `perf record` + report/annotate |

Examples:

```bash
bash benchmark/scripts/local/benchmarks.sh
ENABLE_LTO=1 EVENTS=cycles:u FREQ=2000 USE_CHRT_FIFO=0 \
  bash benchmark/scripts/local/hft_macro_perf_record.sh
```

## `remote/` — cloud runners

Each script clones/pulls the repo on the server, runs the matching `local/` driver (and analysis steps), then downloads artifacts to `server_results/`.

| Script | Purpose |
|---|---|
| `compare.sh` | N-way branch/commit comparison (use `VERSIONS="master:baseline"` for a single run) |
| `hft_macro_scenarios_tuned.sh` | Per-scenario attribution with Linux isolation |
| `hft_macro_perf_record.sh` | Remote perf record (optional `SYNC_LOCAL_SCRIPTS=1`) |
| `ring_size_sweep.sh` | RingSize parameter sweep |
| `setup_nohz_full.sh` | One-time boot `nohz_full` + CPU isolation |

Examples:

```bash
# Single-branch benchmark (replaces the old bench.sh)
SERVER_IP=1.2.3.4 REPO_URL=https://github.com/you/llmes.git \
  VERSIONS="master:baseline" TRIALS=10 \
  bash benchmark/scripts/remote/compare.sh

# Two-branch A/B comparison
SERVER_IP=1.2.3.4 REPO_URL=https://github.com/you/llmes.git \
  VERSIONS="master:new,phase9:old" \
  bash benchmark/scripts/remote/compare.sh

SERVER_IP=1.2.3.4 REPO_URL=... ENABLE_LTO=1 USE_CHRT_FIFO=0 \
  bash benchmark/scripts/remote/hft_macro_perf_record.sh
```

## `analysis/` — post-processing

| Script | Purpose |
|---|---|
| `merge_benchmark_metrics.py` | Merge latency + PMC raw trials → aggregated CSV |
| `plot_benchmark.py` | Parametric plots from merged aggregate |
| `plot_version_comparison.py` | Version/branch comparison line, bar, heatmap |
| `plot_hft_macro_scenarios.py` | Per-scenario latency distribution plots |
| `analyze_hft_macro_attribution.py` | Summarize scenario attribution CSVs |

Configure via env vars (`AGG_CSV`, `PLOT_OUT_DIR`, `OUT_PREFIX`, …). See each file header for details.
