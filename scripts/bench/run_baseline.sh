#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT_DIR="$ROOT/bench/results"
mkdir -p "$OUT_DIR"

cmake -S "$ROOT" -B "$ROOT/build" -DCMAKE_BUILD_TYPE=Release -DLLMES_BUILD_BENCHMARKS=ON
cmake --build "$ROOT/build" -j

BIN="$ROOT/build/core/matching_core/matching_core_phase1_bench"
CSV="$OUT_DIR/phase1_baseline_raw_trials.csv"

echo "mode,trial_id,scenario,orders,levels,warmup_iters,iters,seed,avg_ns,p50_ns,p95_ns,p99_ns,ops_s,ok" > "$CSV"

SCENARIOS=("lmt_rest" "lmt_cross_deep" "mkt_sweep_deep" "cxl_miss" "dup_reject")
ORDERS=(1000 10000 100000)
LEVELS=(10 100 1000)
ITERS="${ITERS:-2000}"
WARMUP_ITERS="${WARMUP_ITERS:-200}"
SEED="${SEED:-42}"
TRIALS="${TRIALS:-5}"

for t in $(seq 1 "$TRIALS"); do
  for s in "${SCENARIOS[@]}"; do
    for o in "${ORDERS[@]}"; do
      for l in "${LEVELS[@]}"; do
        if (( l > o )); then
          continue
        fi
        "$BIN" \
          --mode latency \
          --trial-id "$t" \
          --scenario "$s" \
          --orders "$o" \
          --levels "$l" \
          --warmup-iters "$WARMUP_ITERS" \
          --iters "$ITERS" \
          --seed "$SEED" \
          --out "$CSV"
      done
    done
  done
done

echo "baseline raw trials saved: $CSV"