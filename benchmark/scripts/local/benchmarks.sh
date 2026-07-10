#!/usr/bin/env bash
set -euo pipefail

SCRIPTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROOT="$(cd "$SCRIPTS_DIR/../.." && pwd)"
OUT_DIR="${OUT_DIR:-$ROOT/benchmark/results}"
mkdir -p "$OUT_DIR"

DRY_RUN="${DRY_RUN:-0}"

if [[ "${1:-}" == "--dry-run" ]]; then
	DRY_RUN=1
fi

if (( DRY_RUN == 0 )); then
	cmake -S "$ROOT" -B "$ROOT/build" -DCMAKE_BUILD_TYPE="${BUILD_TYPE:-Release}" -DLLMES_BUILD_BENCHMARKS=ON
	cmake --build "$ROOT/build" -j
fi

SCENARIOS_CSV="${SCENARIOS:-hft_macro}"
METRICS_CSV="${METRICS:-latency,pmc}"
ORDERS_CSV="${ORDERS:-100000}"
LEVELS_CSV="${LEVELS:-100}"
BATCH_SIZES_CSV="${BATCH_SIZES:-100000}"

TRIALS="${TRIALS:-5}"
ITERS="${ITERS:-1}"
WARMUP_ITERS="${WARMUP_ITERS:-1}"
SEED="${SEED:-42}"
VERSION_TAG="${VERSION_TAG:-baseline}"
COMMIT_SHA="${COMMIT_SHA:-unknown}"
OUT_PREFIX="${OUT_PREFIX:-benchmark}"

IFS=',' read -r -a SCENARIOS <<< "$SCENARIOS_CSV"
IFS=',' read -r -a METRICS <<< "$METRICS_CSV"
IFS=',' read -r -a ORDERS_ARR <<< "$ORDERS_CSV"
IFS=',' read -r -a LEVELS_ARR <<< "$LEVELS_CSV"
IFS=',' read -r -a BATCH_ARR <<< "$BATCH_SIZES_CSV"

LAT_CSV="$OUT_DIR/${OUT_PREFIX}_latency_raw_trials.csv"
PMC_CSV="$OUT_DIR/${OUT_PREFIX}_pmc_raw_trials.csv"
RUN_LATENCY=0
RUN_PMC=0
for metric in "${METRICS[@]}"; do
	if [[ "$metric" == "latency" ]]; then
		RUN_LATENCY=1
	elif [[ "$metric" == "pmc" ]]; then
		RUN_PMC=1
	fi
done

if (( DRY_RUN == 0 )); then
	if (( RUN_LATENCY == 1 )); then
		: > "$LAT_CSV"
	fi
	if (( RUN_PMC == 1 )); then
		: > "$PMC_CSV"
	fi
fi

bin_for_scenario() {
	case "$1" in
		hft_macro) echo "$ROOT/build/benchmark/bench_hft_macro" ;;
		*)
			echo "unknown scenario: $1" >&2
			return 1
			;;
	esac
}

# Count total runs for progress tracking
total_runs=0
for trial in $(seq 1 "$TRIALS"); do
	for scenario in "${SCENARIOS[@]}"; do
		for metric in "${METRICS[@]}"; do
			for orders in "${ORDERS_ARR[@]}"; do
				for levels in "${LEVELS_ARR[@]}"; do
					if (( levels > orders )); then continue; fi
					for batch_size in "${BATCH_ARR[@]}"; do
						(( total_runs++ )) || true
					done
				done
			done
		done
	done
done

current_run=0
for trial in $(seq 1 "$TRIALS"); do
	for scenario in "${SCENARIOS[@]}"; do
		BIN="$(bin_for_scenario "$scenario")"

		for metric in "${METRICS[@]}"; do
			for orders in "${ORDERS_ARR[@]}"; do
				for levels in "${LEVELS_ARR[@]}"; do
					if (( levels > orders )); then continue; fi
					for batch_size in "${BATCH_ARR[@]}"; do
						(( current_run++ )) || true
						if [[ "$metric" == "latency" ]]; then
							out_file="$LAT_CSV"
						elif [[ "$metric" == "pmc" ]]; then
							out_file="$PMC_CSV"
						else
							echo "unknown metric: $metric" >&2
							exit 2
						fi

						printf "[%3d/%3d] trial=%-2d scenario=%-15s metric=%-7s orders=%-6s levels=%-4s batch=%-3s " \
							"$current_run" "$total_runs" "$trial" "$scenario" "$metric" "$orders" "$levels" "$batch_size"

						if (( DRY_RUN == 1 )); then
							echo "(dry-run)"
						else
							if "$BIN" \
								--metric "$metric" \
								--trial-id "$trial" \
								--orders "$orders" \
								--levels "$levels" \
								--batch-size "$batch_size" \
								--warmup-iters "$WARMUP_ITERS" \
								--iters "$ITERS" \
								--seed "$SEED" \
								--version-tag "$VERSION_TAG" \
								--commit-sha "$COMMIT_SHA" \
								--out "$out_file" > /dev/null; then
								echo "ok"
							else
								echo "FAIL"
								exit 1
							fi
						fi
					done
				done
			done
		done
	done
done

if (( DRY_RUN == 1 )); then
	echo ""
	echo "Dry-run complete. $total_runs commands would be executed."
elif (( RUN_LATENCY == 1 )); then
	echo "latency raw trials saved: $LAT_CSV"
fi
if (( DRY_RUN == 0 )) && (( RUN_PMC == 1 )); then
	echo "pmc raw trials saved: $PMC_CSV"
fi
