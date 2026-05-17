#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT_DIR="$ROOT/bench/results"
mkdir -p "$OUT_DIR"

BIN="$ROOT/build/core/matching_core/matching_core_phase1_bench"
PMC_CSV="$OUT_DIR/phase1_pmc_raw_trials.csv"

echo "mode,trial_id,scenario,orders,levels,warmup_iters,iters,seed,cycles_per_op,instructions_per_op,branches_per_op,branch_misses_per_op,llc_load_misses_per_op,llc_store_misses_per_op,cache_misses_per_op,cpi,branch_miss_rate,llc_miss_per_op,ok" > "$PMC_CSV"

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
          --mode pmc \
          --trial-id "$t" \
          --scenario "$s" \
          --orders "$o" \
          --levels "$l" \
          --warmup-iters "$WARMUP_ITERS" \
          --iters "$ITERS" \
          --seed "$SEED" \
          --out "$PMC_CSV"
      done
    done
  done
done

echo "pmc raw trials saved: $PMC_CSV"