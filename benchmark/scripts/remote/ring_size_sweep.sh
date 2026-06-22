#!/usr/bin/env bash
set -euo pipefail

# Sweep RingSize on a single branch (default: master) and compare hft_macro results.
#
# Usage:
#   SERVER_IP=1.2.3.4 REPO_URL=https://github.com/you/llmes.git \
#     bash benchmark/scripts/remote/ring_size_sweep.sh

SERVER_IP="${SERVER_IP:-}"
SSH_USER="${SSH_USER:-root}"
SSH_KEY="${SSH_KEY:-$HOME/.ssh/id_ed25519_hetzner}"
SSH_PORT="${SSH_PORT:-22}"

REPO_URL="${REPO_URL:-}"
BRANCH="${BRANCH:-master}"
RING_SIZES="${RING_SIZES:-8,16,32,64}"

REMOTE_ROOT="${REMOTE_ROOT:-/root/llmes-bench}"
REMOTE_REPO_DIR="${REMOTE_REPO_DIR:-$REMOTE_ROOT/repo}"
REMOTE_ARTIFACTS_DIR="${REMOTE_ARTIFACTS_DIR:-$REMOTE_ROOT/artifacts}"
REMOTE_TARBALL="${REMOTE_TARBALL:-$REMOTE_ROOT/bench_ring_size_artifacts.tgz}"

LOCAL_OUT_DIR="${LOCAL_OUT_DIR:-./server_results}"

SCENARIOS="${SCENARIOS:-hft_macro}"
METRICS="${METRICS:-latency,pmc}"
ORDERS="${ORDERS:-100000}"
LEVELS="${LEVELS:-100}"
BATCH_SIZES="${BATCH_SIZES:-100000}"
TRIALS="${TRIALS:-10}"
ITERS="${ITERS:-1}"
WARMUP_ITERS="${WARMUP_ITERS:-1}"
SEED="${SEED:-42}"

PLOT_METRICS="${PLOT_METRICS:-p99_ns,ops_s,cpi,cache_misses_per_op}"
PLOT_LEVEL="${PLOT_LEVEL:-100}"
FIXED_ORDERS="${FIXED_ORDERS:-}"

INSTALL_DEPS="${INSTALL_DEPS:-1}"
COMPARE_PREFIX="${COMPARE_PREFIX:-ring_size}"

RING_BUFFER_H="${RING_BUFFER_H:-core/matching_core/include/matching_core/ring_buffer.hpp}"

if [[ -z "$SERVER_IP" || -z "$REPO_URL" ]]; then
  echo "ERROR: SERVER_IP and REPO_URL are required"
  exit 1
fi

IFS=',' read -r -a SIZES <<< "$RING_SIZES"
N=${#SIZES[@]}

echo "===== RingSize Sweep Pipeline ====="
echo "  Branch    : $BRANCH"
echo "  Ring sizes: ${SIZES[*]}"
echo "  Server    : ${SSH_USER}@${SERVER_IP}"
echo ""

SSH_KEY="${SSH_KEY/#\~/$HOME}"
SSH_OPTS=(-i "$SSH_KEY" -p "$SSH_PORT" -o StrictHostKeyChecking=accept-new)

mkdir -p "$LOCAL_OUT_DIR"
LOCAL_OUT_DIR="$(cd "$LOCAL_OUT_DIR" && pwd)"
LOCAL_TARBALL="$LOCAL_OUT_DIR/bench_ring_size.tgz"

ssh "${SSH_OPTS[@]}" "${SSH_USER}@${SERVER_IP}" \
  "SSH_USER='$SSH_USER' SERVER_IP='$SERVER_IP' REPO_URL='$REPO_URL' BRANCH='$BRANCH' \
   RING_SIZES='$RING_SIZES' REMOTE_ROOT='$REMOTE_ROOT' REMOTE_REPO_DIR='$REMOTE_REPO_DIR' \
   REMOTE_ARTIFACTS_DIR='$REMOTE_ARTIFACTS_DIR' REMOTE_TARBALL='$REMOTE_TARBALL' \
   SCENARIOS='$SCENARIOS' METRICS='$METRICS' ORDERS='$ORDERS' LEVELS='$LEVELS' \
   BATCH_SIZES='$BATCH_SIZES' TRIALS='$TRIALS' ITERS='$ITERS' WARMUP_ITERS='$WARMUP_ITERS' \
   SEED='$SEED' PLOT_METRICS='$PLOT_METRICS' PLOT_LEVEL='$PLOT_LEVEL' FIXED_ORDERS='$FIXED_ORDERS' \
   INSTALL_DEPS='$INSTALL_DEPS' COMPARE_PREFIX='$COMPARE_PREFIX' RING_BUFFER_H='$RING_BUFFER_H' bash -s" <<'ENDSSH'
set -euo pipefail

echo "===== Remote RingSize sweep ====="
mkdir -p "$REMOTE_ROOT"

if [[ "$INSTALL_DEPS" == "1" ]]; then
  echo "--- Installing system packages ---"
  export DEBIAN_FRONTEND=noninteractive
  apt-get update
  apt-get install -y --no-install-recommends \
    git ca-certificates build-essential cmake python3 python3-venv python3-pip
fi

echo "--- Repository ---"
if [[ ! -d "$REMOTE_REPO_DIR/.git" ]]; then
  git clone "$REPO_URL" "$REMOTE_REPO_DIR"
fi

cd "$REMOTE_REPO_DIR"
git fetch --all --prune --tags
git checkout "origin/$BRANCH" 2>/dev/null || git checkout "$BRANCH"
BASE_SHA="$(git rev-parse --short HEAD)"
echo "  base commit = $BASE_SHA"

echo "--- Python dependencies ---"
python3 -m venv .venv
source .venv/bin/activate
python -m pip install --upgrade pip -q
python -m pip install -r requirements.txt -q

mkdir -p "$REMOTE_ARTIFACTS_DIR"
RES_DIR="$REMOTE_REPO_DIR/benchmark/results"

IFS=',' read -r -a SIZES <<< "$RING_SIZES"
N=${#SIZES[@]}

LAT_FILES=()
PMC_FILES=()
TAGS=()

for ((idx=0; idx<N; idx++)); do
  size="${SIZES[$idx]}"
  tag="ring${size}"
  prefix="v$((idx+1))"
  TAGS+=("$tag")

  echo ""
  echo "========== [$((idx+1))/$N]  $BRANCH  RingSize=$size  (tag: $tag) =========="

  git checkout -- "$RING_BUFFER_H"
  sed -i "s/static constexpr std::size_t   RingSize = [0-9]\+;/static constexpr std::size_t   RingSize = ${size};/" \
    "$RING_BUFFER_H"
  if [[ "$size" == "64" ]]; then
    sed -i 's/static constexpr std::uint64_t kValid   = (1ull << RingSize) - 1;/static constexpr std::uint64_t kValid = ~std::uint64_t{0};/' \
      "$RING_BUFFER_H"
    sed -i 's/(RingSize >= 64) ? ~std::uint64_t{0} : ((std::uint64_t{1} << RingSize) - 1);/~std::uint64_t{0};/' \
      "$RING_BUFFER_H"
  fi
  grep 'RingSize =' "$RING_BUFFER_H" | head -1 | tee "$REMOTE_ARTIFACTS_DIR/ring_size_${tag}.txt"

  rm -rf build
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DLLMES_BUILD_BENCHMARKS=ON
  cmake --build build -j"$(nproc)"

  echo "--- ctest ($tag) ---"
  ctest --test-dir build --output-on-failure | tee "$REMOTE_ARTIFACTS_DIR/ctest_${prefix}.log"

  echo "--- Benchmark ($tag) ---"
  SCENARIOS="$SCENARIOS" METRICS="$METRICS" ORDERS="$ORDERS" LEVELS="$LEVELS" \
  BATCH_SIZES="$BATCH_SIZES" TRIALS="$TRIALS" ITERS="$ITERS" WARMUP_ITERS="$WARMUP_ITERS" \
  SEED="$SEED" VERSION_TAG="$tag" COMMIT_SHA="$BASE_SHA" OUT_PREFIX="$prefix" \
    bash benchmark/scripts/local/benchmarks.sh | tee "$REMOTE_ARTIFACTS_DIR/run_${prefix}.log"

  LAT_FILES+=("$RES_DIR/${prefix}_latency_raw_trials.csv")
  PMC_FILES+=("$RES_DIR/${prefix}_pmc_raw_trials.csv")
done

git checkout -- "$RING_BUFFER_H"

echo ""
echo "--- Merging $N ring sizes ---"

COMBINED_LAT="$RES_DIR/${COMPARE_PREFIX}_latency_raw_trials.csv"
COMBINED_PMC="$RES_DIR/${COMPARE_PREFIX}_pmc_raw_trials.csv"

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

LAT_CSV="$COMBINED_LAT" PMC_CSV="$COMBINED_PMC" OUT_PREFIX="$COMPARE_PREFIX" \
  python benchmark/scripts/analysis/merge_benchmark_metrics.py | tee "$REMOTE_ARTIFACTS_DIR/merge_compare.log"

echo "--- Version comparison plots ---"
OUT_PREFIX="$COMPARE_PREFIX" PLOT_METRICS="$PLOT_METRICS" PLOT_LEVEL="$PLOT_LEVEL" \
  FIXED_ORDERS="$FIXED_ORDERS" \
  python benchmark/scripts/analysis/plot_version_comparison.py | tee "$REMOTE_ARTIFACTS_DIR/plot_compare.log"

{
  echo "timestamp=$(date -Iseconds)"
  echo "branch=$BRANCH"
  echo "base_commit=$BASE_SHA"
  echo "ring_sizes=$RING_SIZES"
  for ((idx=0; idx<N; idx++)); do
    echo "version_$((idx+1))_tag=${TAGS[$idx]}"
    echo "version_$((idx+1))_ring_size=${SIZES[$idx]}"
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
  uname -a
  echo
  lscpu
} > "$REMOTE_ARTIFACTS_DIR/env.txt"

rm -f "$REMOTE_TARBALL"
tar -czf "$REMOTE_TARBALL" \
  -C "$REMOTE_ROOT" \
  "$(basename "$REMOTE_ARTIFACTS_DIR")" \
  "repo/benchmark/results"

echo ""
echo "Remote artifacts ready: $REMOTE_TARBALL"
ENDSSH

echo ""
echo "[Download] Fetching artifacts ..."
scp "${SSH_OPTS[@]}" "${SSH_USER}@${SERVER_IP}:${REMOTE_TARBALL}" "$LOCAL_TARBALL"

echo "[Extract] Unpacking ..."
tar -xzf "$LOCAL_TARBALL" -C "$LOCAL_OUT_DIR"

echo ""
echo "Done."
echo "  Tarball   : $LOCAL_TARBALL"
echo "  Extracted : $LOCAL_OUT_DIR"
echo "  CSV       : $LOCAL_OUT_DIR/repo/benchmark/results/${COMPARE_PREFIX}_merged_agg.csv"
