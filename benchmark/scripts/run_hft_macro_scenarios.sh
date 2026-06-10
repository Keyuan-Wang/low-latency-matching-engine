#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT_DIR="${OUT_DIR:-$ROOT/benchmark/results}"
mkdir -p "$OUT_DIR"

DRY_RUN="${DRY_RUN:-0}"
if [[ "${1:-}" == "--dry-run" ]]; then
	DRY_RUN=1
fi

BUILD_TYPE="${BUILD_TYPE:-Release}"
TRIALS="${TRIALS:-10}"
ITERS="${ITERS:-1}"
WARMUP_ITERS="${WARMUP_ITERS:-1}"
ORDERS="${ORDERS:-100000}"
LEVELS="${LEVELS:-100}"
BATCH_SIZE="${BATCH_SIZE:-100000}"
SEED="${SEED:-42}"
FOCUS="${FOCUS:-all}"
VERSION_TAG="${VERSION_TAG:-baseline}"
COMMIT_SHA="${COMMIT_SHA:-unknown}"
OUT_CSV="${OUT_CSV:-$OUT_DIR/hft_macro_scenario_cycles.csv}"

if (( DRY_RUN == 0 )); then
	cmake -S "$ROOT" -B "$ROOT/build" \
		-DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
		-DLLMES_BUILD_BENCHMARKS=ON
	cmake --build "$ROOT/build" -j
	: > "$OUT_CSV"
fi

BIN="$ROOT/build/benchmark/bench_hft_macro_scenarios"

echo "===== HFT macro per-scenario cycles ====="
echo "  trials      : $TRIALS"
echo "  focus       : $FOCUS"
echo "  batch_size  : $BATCH_SIZE"
echo "  iters       : $ITERS"
echo "  warmup_iters: $WARMUP_ITERS"
echo "  out         : $OUT_CSV"
echo ""

for trial in $(seq 1 "$TRIALS"); do
	printf "[%3d/%3d] focus=%-12s batch=%-8s " \
		"$trial" "$TRIALS" "$FOCUS" "$BATCH_SIZE"

	if (( DRY_RUN == 1 )); then
		echo "(dry-run)"
		continue
	fi

	if "$BIN" \
		--trial-id "$trial" \
		--orders "$ORDERS" \
		--levels "$LEVELS" \
		--batch-size "$BATCH_SIZE" \
		--warmup-iters "$WARMUP_ITERS" \
		--iters "$ITERS" \
		--seed "$SEED" \
		--focus "$FOCUS" \
		--version-tag "$VERSION_TAG" \
		--commit-sha "$COMMIT_SHA" \
		--out "$OUT_CSV" > /dev/null; then
		echo "ok"
	else
		echo "FAIL"
		exit 1
	fi
done

if (( DRY_RUN == 1 )); then
	echo ""
	echo "Dry-run complete. $TRIALS commands would be executed."
else
	echo ""
	echo "scenario cycles saved: $OUT_CSV"
fi
