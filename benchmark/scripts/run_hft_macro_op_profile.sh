#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT_DIR="${OUT_DIR:-$ROOT/benchmark/results}"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
mkdir -p "$OUT_DIR"

DRY_RUN="${DRY_RUN:-0}"
if [[ "${1:-}" == "--dry-run" ]]; then
	DRY_RUN=1
fi

# Benchmark config (defaults align with macro campaign settings)
METRIC="${METRIC:-latency}"                # latency | pmc
TRIALS="${TRIALS:-10}"
ORDERS="${ORDERS:-100000}"
LEVELS="${LEVELS:-100}"
BATCH_SIZE="${BATCH_SIZE:-100000}"
WARMUP_ITERS="${WARMUP_ITERS:-1}"
ITERS="${ITERS:-1}"
SEED="${SEED:-42}"
VERSION_TAG="${VERSION_TAG:-master}"
COMMIT_SHA="${COMMIT_SHA:-$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)}"
OUT_PREFIX="${OUT_PREFIX:-macro_op_profile}"

PROFILE_RAW_CSV="${PROFILE_RAW_CSV:-$OUT_DIR/${OUT_PREFIX}_raw_trials.csv}"
LAT_OR_PMC_CSV="${LAT_OR_PMC_CSV:-$OUT_DIR/${OUT_PREFIX}_${METRIC}_raw_trials.csv}"
LOG_DIR="${LOG_DIR:-$OUT_DIR/${OUT_PREFIX}_logs}"
mkdir -p "$LOG_DIR"

BIN="$BUILD_DIR/benchmark/bench_hft_macro"

if [[ "$METRIC" != "latency" && "$METRIC" != "pmc" ]]; then
	echo "ERROR: METRIC must be latency or pmc, got: $METRIC" >&2
	exit 2
fi

if (( DRY_RUN == 0 )); then
	cmake -S "$ROOT" -B "$BUILD_DIR" \
		-DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
		-DLLMES_BUILD_BENCHMARKS=ON \
		-DLLMES_PROFILE_HFT_MACRO_OPS=ON
	cmake --build "$BUILD_DIR" --target bench_hft_macro -j

	# Fresh outputs for this run
	: > "$PROFILE_RAW_CSV"
	: > "$LAT_OR_PMC_CSV"
fi

echo "===== hft_macro op profiling ====="
echo "  bin            : $BIN"
echo "  metric         : $METRIC"
echo "  trials         : $TRIALS"
echo "  orders/levels  : $ORDERS / $LEVELS"
echo "  batch_size     : $BATCH_SIZE"
echo "  warmup/iters   : $WARMUP_ITERS / $ITERS"
echo "  seed           : $SEED"
echo "  version_tag    : $VERSION_TAG"
echo "  commit_sha     : $COMMIT_SHA"
echo "  profile csv    : $PROFILE_RAW_CSV"
echo "  runner csv     : $LAT_OR_PMC_CSV"
echo "  logs           : $LOG_DIR"

for trial in $(seq 1 "$TRIALS"); do
	trial_seed=$((SEED + trial - 1))
	trial_log="$LOG_DIR/trial_${trial}.log"

	printf "[%2d/%2d] metric=%-7s seed=%-6d " "$trial" "$TRIALS" "$METRIC" "$trial_seed"
	if (( DRY_RUN == 1 )); then
		echo "(dry-run)"
		continue
	fi

	if LLMES_HFT_MACRO_OP_PROFILE_OUT="$PROFILE_RAW_CSV" \
		"$BIN" \
			--metric "$METRIC" \
			--trial-id "$trial" \
			--orders "$ORDERS" \
			--levels "$LEVELS" \
			--batch-size "$BATCH_SIZE" \
			--warmup-iters "$WARMUP_ITERS" \
			--iters "$ITERS" \
			--seed "$trial_seed" \
			--version-tag "$VERSION_TAG" \
			--commit-sha "$COMMIT_SHA" \
			--out "$LAT_OR_PMC_CSV" \
			>"$trial_log" 2>&1; then
		echo "ok"
	else
		echo "FAIL (see $trial_log)"
		exit 1
	fi
done

if (( DRY_RUN == 1 )); then
	echo "Dry-run complete."
	exit 0
fi

echo "done:"
echo "  macro op profile raw trials: $PROFILE_RAW_CSV"
echo "  runner raw trials          : $LAT_OR_PMC_CSV"
echo "  trial logs                 : $LOG_DIR"
