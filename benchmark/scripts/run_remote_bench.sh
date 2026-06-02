#!/usr/bin/env bash
set -euo pipefail

# One-command remote benchmark runner:
# 1) SSH to remote server
# 2) Clone/pull repository (branch or exact commit)
# 3) Install OS + Python dependencies
# 4) cmake build + ctest
# 5) Run benchmark matrix (run_benchmarks.sh)
# 6) Merge latency & PMC metrics
# 7) Generate baseline and version-comparison plots
# 8) Package artifacts and download to local machine
#
# Usage:
#   SERVER_IP=1.2.3.4 REPO_URL=git@github.com:you/llmes.git \
#     bash benchmark/scripts/run_remote_bench.sh
#
# All benchmark campaign parameters are forwarded to run_benchmarks.sh
# and can be overridden via env vars (see below).

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
REMOTE_TARBALL="${REMOTE_TARBALL:-$REMOTE_ROOT/bench_artifacts.tgz}"

# --- local output ---
LOCAL_OUT_DIR="${LOCAL_OUT_DIR:-$(pwd)/server_results}"

# --- benchmark campaign params (forwarded to run_benchmarks.sh) ---
SCENARIOS="${SCENARIOS:-lmt_rest,lmt_cross_shallow,lmt_cross_deep,mkt_sweep_deep,cxl_hit}"
METRICS="${METRICS:-latency,pmc}"
ORDERS="${ORDERS:-100,500,1000,5000,10000,50000,100000}"
LEVELS="${LEVELS:-10,100,1000}"
BATCH_SIZES="${BATCH_SIZES:-64}"
TRIALS="${TRIALS:-5}"
ITERS="${ITERS:-1000}"
WARMUP_ITERS="${WARMUP_ITERS:-100}"
SEED="${SEED:-42}"
VERSION_TAG="${VERSION_TAG:-baseline}"
OUT_PREFIX="${OUT_PREFIX:-benchmark}"

# --- plot params ---
PLOT_METRICS="${PLOT_METRICS:-p99_ns,ops_s,cpi,cache_misses_per_op}"
PLOT_LEVEL="${PLOT_LEVEL:-100}"
FIXED_ORDERS="${FIXED_ORDERS:-}"

# --- execution mode ---
BACKGROUND="${BACKGROUND:-0}"
INSTALL_DEPS="${INSTALL_DEPS:-1}"

# ---------------------------------------------------------------------------
# Pre-flight checks
# ---------------------------------------------------------------------------

if [[ -z "$SERVER_IP" ]]; then
	echo "ERROR: SERVER_IP is required"
	echo "Example:"
	echo "  SERVER_IP=1.2.3.4 REPO_URL=git@github.com:you/llmes.git bash benchmark/scripts/run_remote_bench.sh"
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
LOCAL_TARBALL="$LOCAL_OUT_DIR/bench_artifacts_${TAG_SAFE}_${STAMP}.tgz"

# ---------------------------------------------------------------------------
# Remote execution
# ---------------------------------------------------------------------------

echo "[1/3] Running remote benchmark pipeline on ${SSH_USER}@${SERVER_IP}"

ssh "${SSH_OPTS[@]}" "${SSH_USER}@${SERVER_IP}" \
	"SSH_USER='$SSH_USER' SERVER_IP='$SERVER_IP' REPO_URL='$REPO_URL' BRANCH='$BRANCH' COMMIT_SHA='$COMMIT_SHA' \
	REMOTE_ROOT='$REMOTE_ROOT' REMOTE_REPO_DIR='$REMOTE_REPO_DIR' \
	REMOTE_ARTIFACTS_DIR='$REMOTE_ARTIFACTS_DIR' REMOTE_TARBALL='$REMOTE_TARBALL' \
	SCENARIOS='$SCENARIOS' METRICS='$METRICS' ORDERS='$ORDERS' LEVELS='$LEVELS' \
	BATCH_SIZES='$BATCH_SIZES' TRIALS='$TRIALS' ITERS='$ITERS' WARMUP_ITERS='$WARMUP_ITERS' \
	SEED='$SEED' VERSION_TAG='$VERSION_TAG' OUT_PREFIX='$OUT_PREFIX' \
	PLOT_METRICS='$PLOT_METRICS' PLOT_LEVEL='$PLOT_LEVEL' FIXED_ORDERS='$FIXED_ORDERS' \
	INSTALL_DEPS='$INSTALL_DEPS' BACKGROUND='$BACKGROUND' bash -s" <<'EOF'
set -euo pipefail

echo "===== Remote benchmark pipeline ====="
mkdir -p "$REMOTE_ROOT"

# --- background mode: fork pipeline into nohup, return control immediately ---
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

# --- foreground mode (default) ---

# ---- 1. System dependencies ----
if [[ "$INSTALL_DEPS" == "1" ]]; then
	echo "--- Installing system packages ---"
	export DEBIAN_FRONTEND=noninteractive
	apt-get update
	apt-get install -y --no-install-recommends \
	git ca-certificates build-essential cmake python3 python3-venv python3-pip
fi

# ---- 2. Clone / fetch repo ----
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

# ---- 3. Python venv ----
echo "--- Python dependencies ---"
python3 -m venv .venv
source .venv/bin/activate
python -m pip install --upgrade pip
python -m pip install -r requirements.txt

# ---- 4. Build ----
echo "--- cmake build ---"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DLLMES_BUILD_BENCHMARKS=ON
cmake --build build -j"$(nproc)"

mkdir -p "$REMOTE_ARTIFACTS_DIR"

# ---- 5. Tests ----
echo "--- ctest ---"
ctest --test-dir build --output-on-failure | tee "$REMOTE_ARTIFACTS_DIR/ctest.log"

# ---- 6. Benchmark matrix ----
echo "--- Benchmark campaign ---"
SCENARIOS="$SCENARIOS" METRICS="$METRICS" ORDERS="$ORDERS" LEVELS="$LEVELS" \
BATCH_SIZES="$BATCH_SIZES" TRIALS="$TRIALS" ITERS="$ITERS" WARMUP_ITERS="$WARMUP_ITERS" \
SEED="$SEED" VERSION_TAG="$VERSION_TAG" \
COMMIT_SHA="${COMMIT_SHA:-$(git rev-parse --short HEAD)}" OUT_PREFIX="$OUT_PREFIX" \
	bash benchmark/scripts/run_benchmarks.sh | tee "$REMOTE_ARTIFACTS_DIR/run_benchmarks.log"

# ---- 7. Merge metrics ----
echo "--- Merge metrics ---"
OUT_PREFIX="$OUT_PREFIX" \
	python benchmark/scripts/merge_benchmark_metrics.py | tee "$REMOTE_ARTIFACTS_DIR/merge_metrics.log"

# ---- 8. Baseline plot ----
echo "--- Baseline plots ---"
OUT_PREFIX="$OUT_PREFIX" PLOT_LEVEL="$PLOT_LEVEL" \
	python benchmark/scripts/plot_benchmark.py | tee "$REMOTE_ARTIFACTS_DIR/plot_baseline.log"

# ---- 9. Version-comparison plot ----
echo "--- Version comparison plots ---"
OUT_PREFIX="$OUT_PREFIX" PLOT_METRICS="$PLOT_METRICS" PLOT_LEVEL="$PLOT_LEVEL" \
FIXED_ORDERS="$FIXED_ORDERS" \
	python benchmark/scripts/plot_version_comparison.py | tee "$REMOTE_ARTIFACTS_DIR/plot_comparison.log"

# ---- 10. Environment info ----
{
	echo "timestamp=$(date -Iseconds)"
	echo "checkout=${CHECKOUT_DESC}"
	echo "trials=$TRIALS"
	echo "iters=$ITERS"
	echo "warmup_iters=$WARMUP_ITERS"
	echo "seed=$SEED"
	echo "scenarios=$SCENARIOS"
	echo "metrics=$METRICS"
	echo "orders=$ORDERS"
	echo "levels=$LEVELS"
	echo "batch_sizes=$BATCH_SIZES"
	echo "version_tag=$VERSION_TAG"
	echo "out_prefix=$OUT_PREFIX"
	echo "plot_metrics=$PLOT_METRICS"
	echo "plot_level=$PLOT_LEVEL"
	echo "fixed_orders=$FIXED_ORDERS"
	echo
	echo "===== uname -a ====="
	uname -a
	echo
	echo "===== lscpu ====="
	lscpu
	echo
	echo "===== g++ --version ====="
	g++ --version || true
	echo
	echo "===== cmake --version ====="
	cmake --version || true
} > "$REMOTE_ARTIFACTS_DIR/env.txt"

# ---- 11. Package artifacts ----
rm -f "$REMOTE_TARBALL"
tar -czf "$REMOTE_TARBALL" \
	-C "$REMOTE_ROOT" \
	"$(basename "$REMOTE_ARTIFACTS_DIR")" \
	"repo/benchmark/results"

echo "Remote artifacts ready: $REMOTE_TARBALL"
EOF

# ---------------------------------------------------------------------------
# Download
# ---------------------------------------------------------------------------

if [[ "$BACKGROUND" == "1" ]]; then
	echo ""
	echo "Pipeline running in background on remote server."
	echo "When complete, download manually:"
	echo ""
	echo "  mkdir -p $LOCAL_OUT_DIR"
	echo "  scp ${SSH_USER}@${SERVER_IP}:${REMOTE_TARBALL} $LOCAL_OUT_DIR/"
	echo "  tar -xzf $LOCAL_OUT_DIR/$(basename "$REMOTE_TARBALL") -C $LOCAL_OUT_DIR"
	echo ""
	echo "Key outputs after extraction:"
	echo "  - $LOCAL_OUT_DIR/repo/benchmark/results/*.csv"
	echo "  - $LOCAL_OUT_DIR/repo/benchmark/results/plots/*.png"
	echo "  - $LOCAL_OUT_DIR/artifacts/*.log"
else
	echo "[2/3] Downloading artifacts to local"
	mkdir -p "$LOCAL_OUT_DIR"
	scp "${SSH_OPTS[@]}" "${SSH_USER}@${SERVER_IP}:${REMOTE_TARBALL}" "$LOCAL_TARBALL"

	echo "[3/3] Extracting locally"
	tar -xzf "$LOCAL_TARBALL" -C "$LOCAL_OUT_DIR"

	echo "Done."
	echo "Tarball: $LOCAL_TARBALL"
	echo "Extracted into: $LOCAL_OUT_DIR"
	echo "Key outputs:"
	echo "  - $LOCAL_OUT_DIR/repo/benchmark/results/*.csv"
	echo "  - $LOCAL_OUT_DIR/repo/benchmark/results/plots/*.png"
	echo "  - $LOCAL_OUT_DIR/artifacts/*.log"
fi
