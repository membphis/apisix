#!/usr/bin/env bash
#
# ai-proxy single-CPU streaming throughput benchmark harness.
# See docs/superpowers/specs/2026-05-14-bench-ai-proxy-stream-throughput-design.md
#
set -euo pipefail

# Repo root (the harness is meant to be run from anywhere).
ROOT="$(git rev-parse --show-toplevel)"
cd "$ROOT"

DURATION="${DURATION:-60}"
WARMUP="${WARMUP:-5}"
CONCURRENCY="${CONCURRENCY:-1,2,4,8,16,32,64}"
APISIX_CORE="${APISIX_CORE:-0}"
SERVER_CORES="${SERVER_CORES:-1-2}"
CLIENT_CORES="${CLIENT_CORES:-3-7}"

BENCH_DIR="benchmark/ai-proxy"
RESULT_DIR="$BENCH_DIR/result"
mkdir -p "$RESULT_DIR"

BENCH_BIN="$RESULT_DIR/bench"
CSV="$RESULT_DIR/bench-result.csv"
SERVER_LOG="$RESULT_DIR/server.log"
ENV_FILE="$RESULT_DIR/env.txt"
ERR_COPY="$RESULT_DIR/apisix-error.log"

SERVER_PID=""
WORKER_PID=""

cleanup() {
  set +e
  if [ -n "${WORKER_PID:-}" ]; then
    : # nothing to do for the worker beyond `make stop`
  fi
  if [ -n "${SERVER_PID:-}" ]; then
    kill "$SERVER_PID" 2>/dev/null
  fi
  make stop 2>/dev/null
  sudo killall pidstat 2>/dev/null
  # Snapshot APISIX error log if present.
  if [ -f logs/error.log ]; then
    cp logs/error.log "$ERR_COPY" 2>/dev/null || true
  fi
}
trap cleanup INT TERM EXIT

# Cache sudo creds so `taskset -cp` doesn't prompt mid-run.
sudo -v
