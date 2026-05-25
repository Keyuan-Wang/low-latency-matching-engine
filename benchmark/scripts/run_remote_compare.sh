#!/usr/bin/env bash
set -euo pipefail

# Remote N-way benchmark comparison runner.
#
# Checks out up to N versions of the code on the same server, runs the full
# benchmark matrix for each, merges all results, and generates comparison plots.
# All versions run on the same machine in the same session → environmentally clean.
#
# Usage:
#   SERVER_IP=1.2.3.4 REPO_URL=git@github.com:you/llmes.git \
#     bash benchmark/scripts/run_remote_compare.sh
#
# Versions are specified as a single comma-separated list of  commit:label  pairs:
#
#   VERSIONS="phase1-finale:v1.0,master:v2.0,abc123:v2.5"
#
# Each pair is  commit_or_tag_or_branch  :  human_readable_version_tag
#
# Defaults to phase2b..phase2e if VERSIONS is unset.

# --- remote connection ---
SERVER_IP="${SERVER_IP:-}"
SSH_USER="${SSH_USER:-root}"
SSH_KEY="${SSH_KEY:-$HOME/.ssh/id_ed25519_hetzner}"
SSH_PORT="${SSH_PORT:-22}"

# --- repository ---
REPO_URL="${REPO_URL:-}"

# --- versions: comma-separated  commit:label  pairs ---
# Each pair is  branch_or_commit:version_tag
VERSIONS="${VERSIONS:-phase2b:phase2b,phase2c:phase2c,phase2d:phase2d,phase2e:phase2e}"

# --- remote paths ---
REMOTE_ROOT="${REMOTE_ROOT:-/root/llmes-bench}"
REMOTE_REPO_DIR="${REMOTE_REPO_DIR:-$REMOTE_ROOT/repo}"
REMOTE_ARTIFACTS_DIR="${REMOTE_ARTIFACTS_DIR:-$REMOTE_ROOT/artifacts}"
REMOTE_TARBALL="${REMOTE_TARBALL:-$REMOTE_ROOT/bench_compare_artifacts.tgz}"

# --- local output ---
LOCAL_OUT_DIR="${LOCAL_OUT_DIR:-./server_results}"

# --- benchmark campaign params (applied to every version) ---
SCENARIOS="${SCENARIOS:-hft_add_near,hft_add_far,hft_cancel_hot,hft_cancel_cold,hft_modify_near,hft_cxl_miss,hft_market_small,hft_market_large}"
METRICS="${METRICS:-latency,pmc}"
ORDERS="${ORDERS:-100,500,1000,5000,10000,50000,100000}"
LEVELS="${LEVELS:-10,100,1000}"
BATCH_SIZES="${BATCH_SIZES:-64}"
TRIALS="${TRIALS:-10}"
ITERS="${ITERS:-1000}"
WARMUP_ITERS="${WARMUP_ITERS:-100}"
SEED="${SEED:-42}"

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
  echo "Usage:"
  echo "  SERVER_IP=1.2.3.4 REPO_URL=git@github.com:you/llmes.git bash benchmark/scripts/run_remote_compare.sh"
  exit 1
fi

if [[ -z "$REPO_URL" ]]; then
  echo "ERROR: REPO_URL is required"
  exit 1
fi

# Parse VERSIONS into two parallel arrays: commits[] and tags[]
IFS=',' read -r -a RAW <<< "$VERSIONS"
COMMITS=()
TAGS=()
for pair in "${RAW[@]}"; do
  pair="$(echo "$pair" | xargs)"                   # trim whitespace
  COMMITS+=("${pair%%:*}")                          # everything before first :
  TAGS+=("${pair##*:}")                             # everything after last :
done

N_VERSIONS=${#COMMITS[@]}

echo "===== N-Way Comparison Pipeline ====="
echo "  Versions : $N_VERSIONS"
for ((i=0; i<N_VERSIONS; i++)); do
  printf "    [%d] %-30s  tag: %s\n" "$((i+1))" "${COMMITS[$i]}" "${TAGS[$i]}"
done
echo "  Server   : ${SSH_USER}@${SERVER_IP}"
echo ""

# Serialise arrays into env vars for the SSH heredoc
COMMITS_STR=$(IFS=','; echo "${COMMITS[*]}")
TAGS_STR=$(IFS=','; echo "${TAGS[*]}")

SSH_OPTS=(-i "$SSH_KEY" -p "$SSH_PORT" -o StrictHostKeyChecking=accept-new)

STAMP="$(date +%Y%m%d_%H%M%S)"
LOCAL_TARBALL="$LOCAL_OUT_DIR/bench_compare_${STAMP}.tgz"

# ---------------------------------------------------------------------------
# Remote execution
# ---------------------------------------------------------------------------

ssh "${SSH_OPTS[@]}" "${SSH_USER}@${SERVER_IP}" \
  "SSH_USER='$SSH_USER' SERVER_IP='$SERVER_IP' REPO_URL='$REPO_URL' \
   COMMITS_STR='$COMMITS_STR' TAGS_STR='$TAGS_STR' \
   REMOTE_ROOT='$REMOTE_ROOT' REMOTE_REPO_DIR='$REMOTE_REPO_DIR' \
   REMOTE_ARTIFACTS_DIR='$REMOTE_ARTIFACTS_DIR' REMOTE_TARBALL='$REMOTE_TARBALL' \
   SCENARIOS='$SCENARIOS' METRICS='$METRICS' ORDERS='$ORDERS' LEVELS='$LEVELS' \
   BATCH_SIZES='$BATCH_SIZES' TRIALS='$TRIALS' ITERS='$ITERS' WARMUP_ITERS='$WARMUP_ITERS' \
   SEED='$SEED' \
   PLOT_METRICS='$PLOT_METRICS' PLOT_LEVEL='$PLOT_LEVEL' FIXED_ORDERS='$FIXED_ORDERS' \
   INSTALL_DEPS='$INSTALL_DEPS' BACKGROUND='$BACKGROUND' COMPARE_PREFIX='compare' bash -s" <<'ENDSSH'
set -euo pipefail

echo "===== Remote comparison pipeline ====="
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
    git ca-certificates build-essential cmake python3 python3-venv python3-pip libabsl-dev
fi

# Restore system absl from .bak if it was previously renamed (Debian compat fix)
if [[ -d "/usr/lib/x86_64-linux-gnu/cmake/absl.bak" && ! -d "/usr/lib/x86_64-linux-gnu/cmake/absl" ]]; then
  mv /usr/lib/x86_64-linux-gnu/cmake/absl.bak /usr/lib/x86_64-linux-gnu/cmake/absl
  echo "  restored system absl from .bak"
fi

# ---- 2. Clone / fetch repo ----
echo "--- Repository ---"
if [[ ! -d "$REMOTE_REPO_DIR/.git" ]]; then
  git clone "$REPO_URL" "$REMOTE_REPO_DIR"
fi

cd "$REMOTE_REPO_DIR"
git fetch --all --prune --tags

# ---- 3. Python venv ----
echo "--- Python dependencies ---"
python3 -m venv .venv
source .venv/bin/activate
python -m pip install --upgrade pip
python -m pip install -r requirements.txt

mkdir -p "$REMOTE_ARTIFACTS_DIR"

# ---- Parse version arrays ----
IFS=',' read -r -a COMMITS <<< "$COMMITS_STR"
IFS=',' read -r -a TAGS    <<< "$TAGS_STR"
N=${#COMMITS[@]}

RES_DIR="$REMOTE_REPO_DIR/benchmark/results"

# ---- 4. Run benchmark for each version ----
LAT_FILES=()
PMC_FILES=()
COMMIT_SHAS=()

for ((idx=0; idx<N; idx++)); do
  commit="${COMMITS[$idx]}"
  tag="${TAGS[$idx]}"
  prefix="v$((idx+1))"

  echo ""
  echo "========== [$((idx+1))/$N]  $commit  (tag: $tag) =========="

  git checkout --detach "$commit"
  sha="$(git rev-parse --short HEAD)"
  COMMIT_SHAS+=("$sha")
  echo "  commit = $sha"

  # Rebuild — previous version's build dir may be incompatible
  rm -rf build
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DLLMES_BUILD_BENCHMARKS=ON
  cmake --build build -j"$(nproc)"

  echo "--- ctest ($tag) ---"
  ctest --test-dir build --output-on-failure | tee "$REMOTE_ARTIFACTS_DIR/ctest_${prefix}.log"

  echo "--- Benchmark ($tag) ---"
  SCENARIOS="$SCENARIOS" METRICS="$METRICS" ORDERS="$ORDERS" LEVELS="$LEVELS" \
  BATCH_SIZES="$BATCH_SIZES" TRIALS="$TRIALS" ITERS="$ITERS" WARMUP_ITERS="$WARMUP_ITERS" \
  SEED="$SEED" VERSION_TAG="$tag" \
  COMMIT_SHA="$sha" OUT_PREFIX="$prefix" \
    bash benchmark/scripts/run_benchmarks.sh | tee "$REMOTE_ARTIFACTS_DIR/run_${prefix}.log"

  LAT_FILES+=("$RES_DIR/${prefix}_latency_raw_trials.csv")
  PMC_FILES+=("$RES_DIR/${prefix}_pmc_raw_trials.csv")

  # ---- HFT macro benchmark (separate params) ----
  echo "--- HFT macro benchmark ($tag) ---"
  OUT_PREFIX="macro_${prefix}" TRIALS="$TRIALS" SCENARIOS="hft_macro" \
    ORDERS=100000 LEVELS=100 BATCH_SIZES=100000 ITERS=1 WARMUP_ITERS=1 \
    VERSION_TAG="$tag" COMMIT_SHA="$sha" \
    bash benchmark/scripts/run_benchmarks.sh 2>&1 | tee "$REMOTE_ARTIFACTS_DIR/run_macro_${prefix}.log"
done

# ---- 5. Merge all versions ----
echo ""
echo "--- Merging $N versions ---"

COMBINED_LAT="$RES_DIR/${COMPARE_PREFIX}_latency_raw_trials.csv"
COMBINED_PMC="$RES_DIR/${COMPARE_PREFIX}_pmc_raw_trials.csv"

# Merge latency CSVs
HEADER_WRITTEN=0
for f in "${LAT_FILES[@]}"; do
  if [[ -f "$f" && -s "$f" ]]; then
    if (( HEADER_WRITTEN == 0 )); then
      head -1 "$f" > "$COMBINED_LAT"
      HEADER_WRITTEN=1
    fi
    tail -n +2 "$f" >> "$COMBINED_LAT"
  fi
done
if (( HEADER_WRITTEN )); then
  echo "  combined latency CSV: $(wc -l < "$COMBINED_LAT") rows"
else
  echo "  WARNING: no latency data found"
fi

# Merge PMC CSVs
HEADER_WRITTEN=0
for f in "${PMC_FILES[@]}"; do
  if [[ -f "$f" && -s "$f" ]]; then
    if (( HEADER_WRITTEN == 0 )); then
      head -1 "$f" > "$COMBINED_PMC"
      HEADER_WRITTEN=1
    fi
    tail -n +2 "$f" >> "$COMBINED_PMC"
  fi
done
if (( HEADER_WRITTEN )); then
  echo "  combined PMC CSV: $(wc -l < "$COMBINED_PMC") rows"
else
  echo "  WARNING: no PMC data found"
fi

# Run merge_benchmark_metrics on combined data
LAT_CSV="$COMBINED_LAT" PMC_CSV="$COMBINED_PMC" OUT_PREFIX="$COMPARE_PREFIX" \
  python benchmark/scripts/merge_benchmark_metrics.py | tee "$REMOTE_ARTIFACTS_DIR/merge_compare.log"

# ---- 6. Version-comparison plots ----
echo "--- Version comparison plots ---"
OUT_PREFIX="$COMPARE_PREFIX" PLOT_METRICS="$PLOT_METRICS" PLOT_LEVEL="$PLOT_LEVEL" \
FIXED_ORDERS="$FIXED_ORDERS" \
  python benchmark/scripts/plot_version_comparison.py | tee "$REMOTE_ARTIFACTS_DIR/plot_compare.log"

# ---- 7. Environment info ----
{
  echo "timestamp=$(date -Iseconds)"
  for ((idx=0; idx<N; idx++)); do
    echo "version_$((idx+1))_commit=${COMMIT_SHAS[$idx]}"
    echo "version_$((idx+1))_tag=${TAGS[$idx]}"
  done
  echo "trials=$TRIALS"
  echo "iters=$ITERS"
  echo "warmup_iters=$WARMUP_ITERS"
  echo "seed=$SEED"
  echo "scenarios=$SCENARIOS"
  echo "metrics=$METRICS"
  echo "orders=$ORDERS"
  echo "levels=$LEVELS"
  echo "batch_sizes=$BATCH_SIZES"
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

# ---- 8. Package artifacts ----
rm -f "$REMOTE_TARBALL"
tar -czf "$REMOTE_TARBALL" \
  -C "$REMOTE_ROOT" \
  "$(basename "$REMOTE_ARTIFACTS_DIR")" \
  "repo/benchmark/results"

echo ""
echo "Remote artifacts ready: $REMOTE_TARBALL"
ENDSSH

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
  echo "  $LOCAL_OUT_DIR/repo/benchmark/results/compare_merged_agg.csv"
  echo "  $LOCAL_OUT_DIR/repo/benchmark/results/plots/*.png"
else
  echo ""
  echo "[Download] Fetching artifacts from ${SSH_USER}@${SERVER_IP} ..."
  mkdir -p "$LOCAL_OUT_DIR"
  scp "${SSH_OPTS[@]}" "${SSH_USER}@${SERVER_IP}:${REMOTE_TARBALL}" "$LOCAL_TARBALL"

  echo "[Extract] Unpacking locally ..."
  tar -xzf "$LOCAL_TARBALL" -C "$LOCAL_OUT_DIR"

  echo ""
  echo "Done."
  echo "  Tarball    : $LOCAL_TARBALL"
  echo "  Extracted  : $LOCAL_OUT_DIR"
  echo ""
  echo "Key comparison outputs:"
  echo "  Aggregated CSV   : $LOCAL_OUT_DIR/repo/benchmark/results/compare_merged_agg.csv"
  echo "  Line charts      : $LOCAL_OUT_DIR/repo/benchmark/results/plots/*_vs_*.png"
  echo "  Bar charts       : $LOCAL_OUT_DIR/repo/benchmark/results/plots/*_bar_vs_*.png"
  echo "  Heatmaps         : $LOCAL_OUT_DIR/repo/benchmark/results/plots/*_pct_change_heatmap.png"
  echo "  Logs             : $LOCAL_OUT_DIR/artifacts/*.log"
fi
