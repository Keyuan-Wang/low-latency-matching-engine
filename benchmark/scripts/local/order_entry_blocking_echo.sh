#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
ITERATIONS="${ITERATIONS:-100000}"
PORT="${PORT:-9000}"

SERVER_TARGET="order_entry_echo_bench_server"
CLIENT_TARGET="order_entry_echo_bench_client"

OUT_DIR="$ROOT/benchmark/results/order_entry_blocking_echo_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$OUT_DIR"

echo "[build] $SERVER_TARGET $CLIENT_TARGET"
cmake --build "$BUILD_DIR" --target "$SERVER_TARGET" "$CLIENT_TARGET"

SERVER_BIN="$(find "$BUILD_DIR" -type f -name "$SERVER_TARGET" | head -n 1)"
CLIENT_BIN="$(find "$BUILD_DIR" -type f -name "$CLIENT_TARGET" | head -n 1)"

if [[ -z "$SERVER_BIN" || -z "$CLIENT_BIN" ]]; then
  echo "failed to find benchmark binaries" >&2
  exit 1
fi

cleanup() {
  if [[ -n "${SERVER_PID:-}" ]]; then
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT

echo "[server] $SERVER_BIN --port $PORT"
"$SERVER_BIN" --port "$PORT" > "$OUT_DIR/server.log" 2>&1 &
SERVER_PID=$!

sleep 0.2

echo "[client] $CLIENT_BIN --iterations $ITERATIONS --port $PORT"
"$CLIENT_BIN" \
  --host 127.0.0.1 \
  --port "$PORT" \
  --iterations "$ITERATIONS" \
  > "$OUT_DIR/client.log" 2>&1

wait "$SERVER_PID" 2>/dev/null || true
unset SERVER_PID

echo "[done] results: $OUT_DIR"
echo
cat "$OUT_DIR/client.log"
