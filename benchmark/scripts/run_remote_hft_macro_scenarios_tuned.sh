#!/usr/bin/env bash
set -euo pipefail

# Remote HFT macro per-scenario runner with Linux system-level measurement hygiene.
#
# This follows the same "local SSH launcher -> remote clone/build/run/package ->
# local download" structure as run_remote_compare.sh, but targets the diagnostic
# hft_macro_scenarios benchmark and applies the first round of system tuning:
#
#   1) CPU core binding via taskset / numactl
#   2) NUMA memory binding when numactl is available
#   3) CPU governor set to performance for the run
#   4) SMT sibling awareness and common background-noise reduction
#
# Usage:
#   SERVER_IP=1.2.3.4 REPO_URL=git@github.com:you/llmes.git \
#     bash benchmark/scripts/run_remote_hft_macro_scenarios_tuned.sh

# --- remote connection ---
SERVER_IP="${SERVER_IP:-}"
SSH_USER="${SSH_USER:-root}"
SSH_KEY="${SSH_KEY:-$HOME/.ssh/id_ed25519_hetzner}"
SSH_PORT="${SSH_PORT:-22}"

# --- repository ---
REPO_URL="${REPO_URL:-}"
BRANCH="${BRANCH:-master}"
COMMIT_SHA="${COMMIT_SHA:-}"

# --- remote paths ---
REMOTE_ROOT="${REMOTE_ROOT:-/root/llmes-bench}"
REMOTE_REPO_DIR="${REMOTE_REPO_DIR:-$REMOTE_ROOT/repo}"
REMOTE_ARTIFACTS_DIR="${REMOTE_ARTIFACTS_DIR:-$REMOTE_ROOT/artifacts}"
REMOTE_TARBALL="${REMOTE_TARBALL:-$REMOTE_ROOT/hft_macro_scenarios_tuned_artifacts.tgz}"

# --- local output ---
LOCAL_OUT_DIR="${LOCAL_OUT_DIR:-./server_results}"

# --- benchmark params forwarded to run_hft_macro_scenarios.sh ---
TRIALS="${TRIALS:-10}"
ITERS="${ITERS:-1}"
WARMUP_ITERS="${WARMUP_ITERS:-1}"
ORDERS="${ORDERS:-100000}"
LEVELS="${LEVELS:-100}"
BATCH_SIZE="${BATCH_SIZE:-100000}"
SEED="${SEED:-42}"
FOCUS="${FOCUS:-all}"
VERSION_TAG="${VERSION_TAG:-tuned}"

# --- Linux tuning knobs ---
BENCH_CPU="${BENCH_CPU:-auto}"
NUMA_NODE="${NUMA_NODE:-auto}"
USE_NUMACTL="${USE_NUMACTL:-1}"
SET_PERFORMANCE_GOVERNOR="${SET_PERFORMANCE_GOVERNOR:-1}"
RESTORE_GOVERNOR="${RESTORE_GOVERNOR:-1}"
REDUCE_BACKGROUND_NOISE="${REDUCE_BACKGROUND_NOISE:-1}"
STOP_NOISY_TIMERS="${STOP_NOISY_TIMERS:-1}"

# --- execution mode ---
BACKGROUND="${BACKGROUND:-0}"
INSTALL_DEPS="${INSTALL_DEPS:-1}"

if [[ -z "$SERVER_IP" ]]; then
	echo "ERROR: SERVER_IP is required"
	echo "Example:"
	echo "  SERVER_IP=1.2.3.4 REPO_URL=git@github.com:you/llmes.git bash benchmark/scripts/run_remote_hft_macro_scenarios_tuned.sh"
	exit 1
fi

if [[ -z "$REPO_URL" ]]; then
	echo "ERROR: REPO_URL is required"
	exit 1
fi

SSH_OPTS=(-i "$SSH_KEY" -p "$SSH_PORT" -o StrictHostKeyChecking=accept-new)

STAMP="$(date +%Y%m%d_%H%M%S)"
TAG="${COMMIT_SHA:-$BRANCH}"
TAG_SAFE="$(echo "$TAG" | tr '/:' '__')"
LOCAL_TARBALL="$LOCAL_OUT_DIR/hft_macro_scenarios_tuned_${TAG_SAFE}_${STAMP}.tgz"

echo "===== Remote tuned HFT macro per-scenario pipeline ====="
echo "  Server     : ${SSH_USER}@${SERVER_IP}"
echo "  Checkout   : ${COMMIT_SHA:-$BRANCH}"
echo "  Trials     : $TRIALS"
echo "  Batch size : $BATCH_SIZE"
echo "  Bench CPU  : $BENCH_CPU"
echo "  NUMA node  : $NUMA_NODE"
echo ""

ssh "${SSH_OPTS[@]}" "${SSH_USER}@${SERVER_IP}" \
	"SSH_USER='$SSH_USER' SERVER_IP='$SERVER_IP' REPO_URL='$REPO_URL' BRANCH='$BRANCH' COMMIT_SHA='$COMMIT_SHA' \
	REMOTE_ROOT='$REMOTE_ROOT' REMOTE_REPO_DIR='$REMOTE_REPO_DIR' \
	REMOTE_ARTIFACTS_DIR='$REMOTE_ARTIFACTS_DIR' REMOTE_TARBALL='$REMOTE_TARBALL' \
	TRIALS='$TRIALS' ITERS='$ITERS' WARMUP_ITERS='$WARMUP_ITERS' ORDERS='$ORDERS' LEVELS='$LEVELS' \
	BATCH_SIZE='$BATCH_SIZE' SEED='$SEED' FOCUS='$FOCUS' VERSION_TAG='$VERSION_TAG' \
	BENCH_CPU='$BENCH_CPU' NUMA_NODE='$NUMA_NODE' USE_NUMACTL='$USE_NUMACTL' \
	SET_PERFORMANCE_GOVERNOR='$SET_PERFORMANCE_GOVERNOR' RESTORE_GOVERNOR='$RESTORE_GOVERNOR' \
	REDUCE_BACKGROUND_NOISE='$REDUCE_BACKGROUND_NOISE' STOP_NOISY_TIMERS='$STOP_NOISY_TIMERS' \
	INSTALL_DEPS='$INSTALL_DEPS' BACKGROUND='$BACKGROUND' bash -s" <<'ENDSSH'
set -euo pipefail

echo "===== Remote tuned HFT macro per-scenario pipeline ====="
mkdir -p "$REMOTE_ROOT"

if [[ "$BACKGROUND" == "1" ]]; then
	mkdir -p "$REMOTE_ARTIFACTS_DIR"
	nohup bash -c "
set -euo pipefail
$(cat)
" > "$REMOTE_ARTIFACTS_DIR/pipeline.log" 2>&1 &
	PID=$!
	echo ""
	echo "Background pipeline launched."
	echo "  PID : $PID"
	echo "  Log : $REMOTE_ARTIFACTS_DIR/pipeline.log"
	echo ""
	echo "  Check progress:"
	echo "    ssh ${SSH_USER}@${SERVER_IP} tail -f $REMOTE_ARTIFACTS_DIR/pipeline.log"
	echo ""
	echo "  Download when done:"
	echo "    scp ${SSH_USER}@${SERVER_IP}:${REMOTE_TARBALL} ./"
	exit 0
fi

if [[ "$INSTALL_DEPS" == "1" ]]; then
	echo "--- Installing system packages ---"
	export DEBIAN_FRONTEND=noninteractive
	apt-get update
	apt-get install -y --no-install-recommends \
		git ca-certificates build-essential cmake python3 python3-venv python3-pip \
		numactl util-linux
	apt-get install -y --no-install-recommends linux-tools-common || true
fi

echo "--- Repository ---"
if [[ ! -d "$REMOTE_REPO_DIR/.git" ]]; then
	git clone "$REPO_URL" "$REMOTE_REPO_DIR"
fi

cd "$REMOTE_REPO_DIR"
git fetch --all --prune --tags

if [[ -n "$COMMIT_SHA" ]]; then
	git checkout --detach "$COMMIT_SHA"
	CHECKOUT_DESC="commit $(git rev-parse HEAD) (detached)"
else
	git checkout "$BRANCH"
	git pull --ff-only origin "$BRANCH"
	CHECKOUT_DESC="branch $BRANCH @ $(git rev-parse HEAD)"
fi

mkdir -p "$REMOTE_ARTIFACTS_DIR"

echo "--- Python dependencies ---"
python3 -m venv .venv
source .venv/bin/activate
python -m pip install --upgrade pip
python -m pip install -r requirements.txt

echo "--- cmake build ---"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DLLMES_BUILD_BENCHMARKS=ON
cmake --build build -j"$(nproc)"

echo "--- ctest ---"
ctest --test-dir build --output-on-failure | tee "$REMOTE_ARTIFACTS_DIR/ctest.log"

detect_cpu() {
	if [[ "$BENCH_CPU" != "auto" ]]; then
		echo "$BENCH_CPU"
		return
	fi
	# Prefer a non-zero logical CPU whose SMT sibling set does not include CPU0.
	# CPU0 often carries timer/housekeeping noise; sharing its physical core can
	# still perturb the benchmark even if the benchmark itself is bound elsewhere.
	local cpus=()
	mapfile -t cpus < <(lscpu -p=CPU 2>/dev/null | awk -F, '$1 !~ /^#/ {print $1}')
	local cpu
	for cpu in "${cpus[@]}"; do
		[[ "$cpu" == "0" ]] && continue
		local siblings=",$(detect_siblings "$cpu"),"
		if [[ "$siblings" != *",0,"* && "$siblings" != *",0-"* && "$siblings" != *"-0,"* ]]; then
			echo "$cpu"
			return
		fi
	done
	for cpu in "${cpus[@]}"; do
		if [[ "$cpu" != "0" ]]; then
			echo "$cpu"
			return
		fi
	done
	if [[ ${#cpus[@]} -gt 0 ]]; then
		echo "${cpus[0]}"
	else
		echo 0
	fi
}

detect_numa_node() {
	local cpu="$1"
	if [[ "$NUMA_NODE" != "auto" ]]; then
		echo "$NUMA_NODE"
		return
	fi
	local node
	node="$(lscpu -p=CPU,NODE 2>/dev/null | awk -F, -v c="$cpu" '$1 == c {print $2; exit}')"
	if [[ -z "$node" || "$node" == "-" ]]; then
		node=0
	fi
	echo "$node"
}

detect_siblings() {
	local cpu="$1"
	local f="/sys/devices/system/cpu/cpu${cpu}/topology/thread_siblings_list"
	if [[ -r "$f" ]]; then
		cat "$f"
	else
		echo "$cpu"
	fi
}

write_governor_snapshot() {
	local out="$1"
	: > "$out"
	for f in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
		[[ -e "$f" ]] || continue
		printf "%s=%s\n" "$f" "$(cat "$f")" >> "$out"
	done
}

set_performance_governor() {
	if [[ "$SET_PERFORMANCE_GOVERNOR" != "1" ]]; then
		return
	fi
	echo "--- CPU governor: performance ---"
	write_governor_snapshot "$REMOTE_ARTIFACTS_DIR/governors_before.txt"
	if compgen -G "/sys/devices/system/cpu/cpu*/cpufreq/scaling_governor" >/dev/null; then
		for f in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
			echo performance > "$f" 2>/dev/null || true
		done
	elif command -v cpupower >/dev/null 2>&1; then
		cpupower frequency-set -g performance || true
	fi
	write_governor_snapshot "$REMOTE_ARTIFACTS_DIR/governors_during.txt"
}

restore_governor() {
	if [[ "$RESTORE_GOVERNOR" != "1" ]]; then
		return
	fi
	local snapshot="$REMOTE_ARTIFACTS_DIR/governors_before.txt"
	[[ -f "$snapshot" ]] || return
	while IFS='=' read -r f gov; do
		[[ -w "$f" ]] || continue
		echo "$gov" > "$f" 2>/dev/null || true
	done < "$snapshot"
	write_governor_snapshot "$REMOTE_ARTIFACTS_DIR/governors_after.txt"
}

reduce_background_noise() {
	if [[ "$REDUCE_BACKGROUND_NOISE" != "1" ]]; then
		return
	fi
	echo "--- Background-noise reduction ---"
	{
		echo "===== before: uptime ====="
		uptime || true
		echo
		echo "===== before: top CPU processes ====="
		ps -eo pid,psr,pcpu,pmem,comm --sort=-pcpu | head -25 || true
		echo
		echo "===== before: timers ====="
		systemctl list-timers --all 2>/dev/null || true
	} > "$REMOTE_ARTIFACTS_DIR/noise_before.txt"

	if [[ "$STOP_NOISY_TIMERS" == "1" ]] && command -v systemctl >/dev/null 2>&1; then
		for unit in \
			apt-daily.timer apt-daily-upgrade.timer \
			apt-daily.service apt-daily-upgrade.service \
			man-db.timer man-db.service \
			plocate-updatedb.timer plocate-updatedb.service \
			updatedb.timer updatedb.service \
			fstrim.timer fstrim.service; do
			systemctl stop "$unit" 2>/dev/null || true
		done
	fi

	sync || true
	{
		echo "===== after: uptime ====="
		uptime || true
		echo
		echo "===== after: top CPU processes ====="
		ps -eo pid,psr,pcpu,pmem,comm --sort=-pcpu | head -25 || true
		echo
		echo "===== after: timers ====="
		systemctl list-timers --all 2>/dev/null || true
	} > "$REMOTE_ARTIFACTS_DIR/noise_after.txt"
}

BENCH_CPU_SELECTED="$(detect_cpu)"
NUMA_NODE_SELECTED="$(detect_numa_node "$BENCH_CPU_SELECTED")"
SMT_SIBLINGS="$(detect_siblings "$BENCH_CPU_SELECTED")"

trap restore_governor EXIT
set_performance_governor
reduce_background_noise

RESULTS_DIR="$REMOTE_REPO_DIR/benchmark/results/hft_macro_scenarios_tuned_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$RESULTS_DIR"
OUT_CSV="$RESULTS_DIR/hft_macro_scenario_cycles.csv"
OUT_PNG="$RESULTS_DIR/hft_macro_scenario_cycles_distributions.png"

echo "--- Tuned benchmark settings ---"
echo "  selected CPU : $BENCH_CPU_SELECTED"
echo "  SMT siblings : $SMT_SIBLINGS"
echo "  NUMA node    : $NUMA_NODE_SELECTED"
echo "  use numactl  : $USE_NUMACTL"
echo "  output       : $RESULTS_DIR"

RUN_CMD=(bash benchmark/scripts/run_hft_macro_scenarios.sh)
if [[ "$USE_NUMACTL" == "1" ]] && command -v numactl >/dev/null 2>&1; then
	RUN_PREFIX=(numactl --physcpubind="$BENCH_CPU_SELECTED" --membind="$NUMA_NODE_SELECTED")
else
	RUN_PREFIX=(taskset -c "$BENCH_CPU_SELECTED")
fi

echo "--- HFT macro per-scenario benchmark (tuned) ---"
env \
	TRIALS="$TRIALS" \
	ITERS="$ITERS" \
	WARMUP_ITERS="$WARMUP_ITERS" \
	ORDERS="$ORDERS" \
	LEVELS="$LEVELS" \
	BATCH_SIZE="$BATCH_SIZE" \
	SEED="$SEED" \
	FOCUS="$FOCUS" \
	VERSION_TAG="$VERSION_TAG" \
	COMMIT_SHA="$(git rev-parse --short HEAD)" \
	OUT_DIR="$RESULTS_DIR" \
	OUT_CSV="$OUT_CSV" \
	"${RUN_PREFIX[@]}" "${RUN_CMD[@]}" | tee "$REMOTE_ARTIFACTS_DIR/run_hft_macro_scenarios_tuned.log"

echo "--- Plot per-scenario distributions ---"
CSV="$OUT_CSV" OUT="$OUT_PNG" \
	python benchmark/scripts/plot_hft_macro_scenarios.py | tee "$REMOTE_ARTIFACTS_DIR/plot_hft_macro_scenarios.log"

{
	echo "timestamp=$(date -Iseconds)"
	echo "checkout=${CHECKOUT_DESC}"
	echo "commit=$(git rev-parse --short HEAD)"
	echo "trials=$TRIALS"
	echo "focus=$FOCUS"
	echo "seed=$SEED"
	echo "orders=$ORDERS"
	echo "levels=$LEVELS"
	echo "batch_size=$BATCH_SIZE"
	echo "iters=$ITERS"
	echo "warmup_iters=$WARMUP_ITERS"
	echo "version_tag=$VERSION_TAG"
	echo
	echo "===== tuning ====="
	echo "bench_cpu=$BENCH_CPU_SELECTED"
	echo "numa_node=$NUMA_NODE_SELECTED"
	echo "smt_siblings=$SMT_SIBLINGS"
	echo "use_numactl=$USE_NUMACTL"
	echo "set_performance_governor=$SET_PERFORMANCE_GOVERNOR"
	echo "restore_governor=$RESTORE_GOVERNOR"
	echo "reduce_background_noise=$REDUCE_BACKGROUND_NOISE"
	echo "stop_noisy_timers=$STOP_NOISY_TIMERS"
	echo "run_prefix=${RUN_PREFIX[*]}"
	echo
	echo "===== uname -a ====="
	uname -a
	echo
	echo "===== lscpu ====="
	lscpu
	echo
	echo "===== numactl --hardware ====="
	numactl --hardware 2>/dev/null || true
	echo
	echo "===== governor during ====="
	cat "$REMOTE_ARTIFACTS_DIR/governors_during.txt" 2>/dev/null || true
	echo
	echo "===== g++ --version ====="
	g++ --version || true
	echo
	echo "===== cmake --version ====="
	cmake --version || true
} > "$RESULTS_DIR/env.txt"
cp "$RESULTS_DIR/env.txt" "$REMOTE_ARTIFACTS_DIR/env.txt"
echo "ok" > "$RESULTS_DIR/STATUS"

rm -f "$REMOTE_TARBALL"
tar -czf "$REMOTE_TARBALL" \
	-C "$REMOTE_ROOT" \
	"$(basename "$REMOTE_ARTIFACTS_DIR")" \
	"repo/benchmark/results"

echo ""
echo "Remote artifacts ready: $REMOTE_TARBALL"
ENDSSH

if [[ "$BACKGROUND" == "1" ]]; then
	echo ""
	echo "Pipeline running in background on remote server."
	echo "When complete, download manually:"
	echo ""
	echo "  mkdir -p $LOCAL_OUT_DIR"
	echo "  scp ${SSH_USER}@${SERVER_IP}:${REMOTE_TARBALL} $LOCAL_OUT_DIR/"
	echo "  tar -xzf $LOCAL_OUT_DIR/$(basename "$REMOTE_TARBALL") -C $LOCAL_OUT_DIR"
else
	echo ""
	echo "[Download] Fetching artifacts from ${SSH_USER}@${SERVER_IP} ..."
	mkdir -p "$LOCAL_OUT_DIR"
	scp "${SSH_OPTS[@]}" "${SSH_USER}@${SERVER_IP}:${REMOTE_TARBALL}" "$LOCAL_TARBALL"

	echo "[Extract] Unpacking locally ..."
	tar -xzf "$LOCAL_TARBALL" -C "$LOCAL_OUT_DIR"

	echo ""
	echo "Done."
	echo "  Tarball   : $LOCAL_TARBALL"
	echo "  Extracted : $LOCAL_OUT_DIR"
	echo ""
	echo "Key outputs:"
	echo "  CSV   : $LOCAL_OUT_DIR/repo/benchmark/results/hft_macro_scenarios_tuned_*/hft_macro_scenario_cycles.csv"
	echo "  Plot  : $LOCAL_OUT_DIR/repo/benchmark/results/hft_macro_scenarios_tuned_*/hft_macro_scenario_cycles_distributions.png"
	echo "  Logs  : $LOCAL_OUT_DIR/artifacts/*.log"
fi
