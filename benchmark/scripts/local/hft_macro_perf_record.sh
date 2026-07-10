#!/usr/bin/env bash
#
# hft_macro_perf_record.sh
#
# Instruction-level profiling of the hft_macro engine hot path via
# `perf record` + `perf annotate`, with the recording window restricted to
# the measured RunOp batch ONLY.
#
# Why this is not a plain `perf record ./bench_hft_macro`:
#   The benchmark's Setup() rebuilds the book and replays a 500k-event warmup
#   plus full batch pre-generation on every measured iteration.  A naive
#   recording would be dominated by that scaffolding (generate_pending_one,
#   build_book_from_tracking, RNG, tracking maps) and `perf report` would bury
#   the engine.  Instead we use perf's control FIFO: the runner enables
#   sampling only around the timed RunOp batch (see PerfRecordControl in
#   benchmark_runner.cpp), mirroring the PMC enable/disable window.
#
# The profiling binary is built as Release + `-g` with NO LLMES_PROFILE_*
# macros, so the annotated `add_limit_order` is the exact production code path
# measured by the campaign -- not an instrumented variant.
#
# Requirements: Linux perf, and typically `perf_event_paranoid <= 2`
# (sudo sysctl kernel.perf_event_paranoid=1).  Build with -g so annotate can
# map instructions back to source.
#
set -euo pipefail

SCRIPTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROOT="$(cd "$SCRIPTS_DIR/../.." && pwd)"

# --- Config (override via env) ---
ENABLE_LTO="${ENABLE_LTO:-0}"
BUILD_DIR="${BUILD_DIR:-}"
if [[ -z "$BUILD_DIR" ]]; then
	if [[ "$ENABLE_LTO" == "1" ]]; then
		BUILD_DIR="$ROOT/build-perf-lto"
	else
		BUILD_DIR="$ROOT/build-perf"
	fi
fi
BUILD_TYPE="${BUILD_TYPE:-Release}"
OUT_DIR="${OUT_DIR:-$ROOT/benchmark/results/hft_macro_perf_record_$(date +%Y%m%d_%H%M%S)}"

# Workload: keep aligned with the macro campaign, but use a large batch and
# enough iters so the *enabled* (sampled) window runs for several seconds.
# Each measured iter re-runs the heavy (unsampled) Setup, so prefer a big
# batch over many iters to keep wall time reasonable.
ORDERS="${ORDERS:-100000}"
LEVELS="${LEVELS:-100}"
BATCH_SIZE="${BATCH_SIZE:-1000000}"
WARMUP_ITERS="${WARMUP_ITERS:-1}"
ITERS="${ITERS:-40}"
SEED="${SEED:-42}"

# perf sampling
EVENTS="${EVENTS:-cycles,branch-misses}"
FREQ="${FREQ:-8000}"
CALL_GRAPH="${CALL_GRAPH:-dwarf}"
# Symbols to annotate (demangled substrings). Space-separated.
ANNOTATE_SYMBOLS="${ANNOTATE_SYMBOLS:-add_limit_order cancel_order modify_order add_market_order}"

VERSION_TAG="${VERSION_TAG:-perf_record}"
COMMIT_SHA="${COMMIT_SHA:-$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)}"

# Linux system-level benchmark isolation (matches remote/hft_macro_scenarios_tuned.sh).
ENABLE_LINUX_ISOLATION="${ENABLE_LINUX_ISOLATION:-1}"
BENCH_CPU="${BENCH_CPU:-auto}"
NUMA_NODE="${NUMA_NODE:-auto}"
CPU_PROBE_SECONDS="${CPU_PROBE_SECONDS:-2}"
USE_NUMACTL="${USE_NUMACTL:-1}"
SET_PERFORMANCE_GOVERNOR="${SET_PERFORMANCE_GOVERNOR:-1}"
RESTORE_GOVERNOR="${RESTORE_GOVERNOR:-0}"
REDUCE_BACKGROUND_NOISE="${REDUCE_BACKGROUND_NOISE:-1}"
STOP_NOISY_TIMERS="${STOP_NOISY_TIMERS:-1}"
AVOID_IRQ_ON_BENCH_CPU="${AVOID_IRQ_ON_BENCH_CPU:-1}"
AVOID_IRQ_ON_SMT_SIBLINGS="${AVOID_IRQ_ON_SMT_SIBLINGS:-1}"
RESTORE_IRQ_AFFINITY="${RESTORE_IRQ_AFFINITY:-0}"
AGGRESSIVE_ISOLATION="${AGGRESSIVE_ISOLATION:-1}"
PIN_WORKQUEUES_AWAY="${PIN_WORKQUEUES_AWAY:-1}"
STOP_IRQBALANCE="${STOP_IRQBALANCE:-1}"
DISABLE_KERNEL_WATCHDOGS="${DISABLE_KERNEL_WATCHDOGS:-1}"
LOCK_CPU_DMA_LATENCY="${LOCK_CPU_DMA_LATENCY:-1}"
USE_CHRT_FIFO="${USE_CHRT_FIFO:-1}"
REALTIME_PRIORITY="${REALTIME_PRIORITY:-95}"

BIN="$BUILD_DIR/benchmark/bench_hft_macro"
PERF_DATA="$OUT_DIR/perf.data"
ISOLATION_LIB="$SCRIPTS_DIR/lib/bench_linux_isolation.sh"

# --- Preflight ---
if ! command -v perf >/dev/null 2>&1; then
	echo "ERROR: 'perf' not found on PATH. Install linux-tools / perf." >&2
	exit 1
fi

paranoid_file=/proc/sys/kernel/perf_event_paranoid
if [[ -r "$paranoid_file" ]]; then
	paranoid="$(cat "$paranoid_file")"
	if (( paranoid > 2 )); then
		echo "WARNING: kernel.perf_event_paranoid=$paranoid may block sampling." >&2
		echo "         Try: sudo sysctl kernel.perf_event_paranoid=1" >&2
	fi
fi

mkdir -p "$OUT_DIR"

RUN_PREFIX=()
if [[ "$ENABLE_LINUX_ISOLATION" == "1" ]]; then
	if [[ ! -f "$ISOLATION_LIB" ]]; then
		echo "ERROR: isolation library not found: $ISOLATION_LIB" >&2
		exit 1
	fi
	# shellcheck source=/dev/null
	source "$ISOLATION_LIB"
	bench_linux_isolation_begin "$OUT_DIR"
	RUN_PREFIX=("${BENCH_LINUX_RUN_PREFIX[@]}")
fi

isolation_cleanup() {
	if [[ "$ENABLE_LINUX_ISOLATION" == "1" ]] && declare -F bench_linux_isolation_end >/dev/null; then
		bench_linux_isolation_end || true
	fi
}

# --- Build a dedicated profiling binary: Release + -g, NO profiling macros ---
# A separate build dir avoids clobbering the production Release build and
# guarantees debug line info without changing -O3 codegen.
echo "===== build (profiling binary) ====="
CXX_FLAGS_RELEASE="-O3 -DNDEBUG -g"
LINK_FLAGS_RELEASE=""
if [[ "$ENABLE_LTO" == "1" ]]; then
	CXX_FLAGS_RELEASE+=" -march=native -flto"
	LINK_FLAGS_RELEASE="-flto"
	echo "  LTO enabled: $CXX_FLAGS_RELEASE"
fi
cmake -S "$ROOT" -B "$BUILD_DIR" \
	-DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
	-DCMAKE_CXX_FLAGS_RELEASE="$CXX_FLAGS_RELEASE" \
	-DCMAKE_EXE_LINKER_FLAGS_RELEASE="$LINK_FLAGS_RELEASE" \
	-DLLMES_BUILD_BENCHMARKS=ON \
	-DLLMES_BUILD_TESTS=OFF
cmake --build "$BUILD_DIR" --target bench_hft_macro -j"$(nproc)"

if [[ ! -x "$BIN" ]]; then
	echo "ERROR: benchmark binary not found: $BIN" >&2
	exit 1
fi

# --- Set up the perf control FIFOs ---
FIFO_DIR="$(mktemp -d)"
CTL_FIFO="$FIFO_DIR/perf_ctl"
ACK_FIFO="$FIFO_DIR/perf_ack"
mkfifo "$CTL_FIFO" "$ACK_FIFO"

cleanup() {
	isolation_cleanup
	rm -rf "$FIFO_DIR"
}
trap cleanup EXIT

echo "===== perf record (window-isolated) ====="
echo "  bin          : $BIN"
echo "  events       : $EVENTS  (freq=$FREQ)"
echo "  orders/levels: $ORDERS / $LEVELS"
echo "  batch/iters  : $BATCH_SIZE / $ITERS  (warmup=$WARMUP_ITERS)"
echo "  out dir      : $OUT_DIR"
echo "  perf.data    : $PERF_DATA"
if [[ "$ENABLE_LINUX_ISOLATION" == "1" ]]; then
	echo "  isolation    : enabled (bench_cpu=${BENCH_CPU_SELECTED:-pending})"
fi

# `-D -1` starts with events disabled; the runner sends 'enable' via the
# control FIFO only around the RunOp batch. Env vars hand the FIFO paths to
# the child (inherited through perf).
LLMES_PERF_CTL_FIFO="$CTL_FIFO" \
LLMES_PERF_ACK_FIFO="$ACK_FIFO" \
"${RUN_PREFIX[@]}" perf record \
	-o "$PERF_DATA" \
	--control=fifo:"$CTL_FIFO","$ACK_FIFO" \
	-D -1 \
	-F "$FREQ" \
	-e "$EVENTS" \
	--call-graph "$CALL_GRAPH" \
	-- "$BIN" \
		--metric latency \
		--orders "$ORDERS" \
		--levels "$LEVELS" \
		--batch-size "$BATCH_SIZE" \
		--warmup-iters "$WARMUP_ITERS" \
		--iters "$ITERS" \
		--seed "$SEED" \
		--version-tag "$VERSION_TAG" \
		--commit-sha "$COMMIT_SHA" \
	2>&1 | tee "$OUT_DIR/run.log"

# --- Post-process: function-level report + instruction-level annotate ---
echo "===== perf report (enabled-window symbols) ====="
perf report -i "$PERF_DATA" --stdio --percent-limit 0.5 \
	> "$OUT_DIR/report.txt" 2>/dev/null || true
perf report -i "$PERF_DATA" --stdio --percent-limit 0.5 | head -60 || true

for sym in $ANNOTATE_SYMBOLS; do
	out="$OUT_DIR/annotate_${sym}.txt"
	# -l interleaves source lines; --stdio2 gives the richer asm+source view.
	if perf annotate -i "$PERF_DATA" --stdio2 --symbol="$sym" \
		> "$out" 2>/dev/null && [[ -s "$out" ]]; then
		echo "  annotated: $sym -> $out"
	else
		# Fall back to classic --stdio if --stdio2 produced nothing.
		perf annotate -i "$PERF_DATA" --stdio -l --symbol="$sym" \
			> "$out" 2>/dev/null || true
		[[ -s "$out" ]] && echo "  annotated: $sym -> $out" \
			|| echo "  (no samples for symbol: $sym)"
	fi
done

# --- Run metadata ---
{
	echo "scenario      : hft_macro"
	echo "version_tag   : $VERSION_TAG"
	echo "commit_sha    : $COMMIT_SHA"
	echo "build_type    : $BUILD_TYPE (+ -g, no LLMES_PROFILE_*)"
	echo "enable_lto    : $ENABLE_LTO"
	echo "events        : $EVENTS"
	echo "freq          : $FREQ"
	echo "call_graph    : $CALL_GRAPH"
	echo "orders        : $ORDERS"
	echo "levels        : $LEVELS"
	echo "batch_size    : $BATCH_SIZE"
	echo "warmup_iters  : $WARMUP_ITERS"
	echo "iters         : $ITERS"
	echo "seed          : $SEED"
	echo "window        : RunOp batch only (perf --control=fifo, -D -1)"
	echo "linux_isolation: $ENABLE_LINUX_ISOLATION"
} > "$OUT_DIR/meta.txt"
if [[ "$ENABLE_LINUX_ISOLATION" == "1" ]] && declare -F bench_linux_isolation_write_env >/dev/null; then
	bench_linux_isolation_write_env "$OUT_DIR/meta.txt"
fi

echo "done:"
echo "  report   : $OUT_DIR/report.txt"
echo "  annotate : $OUT_DIR/annotate_<symbol>.txt"
echo "  perf.data: $PERF_DATA"
echo "  meta     : $OUT_DIR/meta.txt"
