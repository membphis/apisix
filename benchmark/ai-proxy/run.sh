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

# --- Step: build bench binary -----------------------------------------------
echo "==> building bench binary"
(cd "$BENCH_DIR/bench" && go build -o "$ROOT/$BENCH_BIN" .)

# --- Step: record environment -----------------------------------------------
{
  echo "date: $(date -Is)"
  echo "git_sha: $(git rev-parse HEAD)"
  echo "kernel: $(uname -r)"
  echo "cpu_model: $(awk -F: '/model name/{print $2; exit}' /proc/cpuinfo | sed 's/^ //')"
  echo "go: $(go version)"
  echo "concurrency: $CONCURRENCY"
  echo "warmup_s: $WARMUP"
  echo "duration_s: $DURATION"
  echo "apisix_core: $APISIX_CORE"
  echo "server_cores: $SERVER_CORES"
  echo "client_cores: $CLIENT_CORES"
} > "$ENV_FILE"

# --- Step: start fake OpenAI upstream ---------------------------------------
echo "==> starting bench server on cores $SERVER_CORES"
taskset -c "$SERVER_CORES" "$BENCH_BIN" server --listen :1981 --gomaxprocs 2 \
  > "$SERVER_LOG" 2>&1 &
SERVER_PID=$!
# Wait until the server prints its startup banner (means the listener is up).
for _ in $(seq 1 30); do
  if grep -q '^bench-server: pid=' "$SERVER_LOG"; then break; fi
  sleep 0.1
done
if ! grep -q '^bench-server: pid=' "$SERVER_LOG"; then
  echo "bench server failed to start; see $SERVER_LOG" >&2
  exit 1
fi
echo "    bench server pid=$SERVER_PID"

# --- Step: write APISIX config ----------------------------------------------
echo "==> installing APISIX config"
cp "$BENCH_DIR/conf/config.yaml.tpl" conf/config.yaml

# --- Step: start APISIX ------------------------------------------------------
echo "==> starting APISIX"
make init
make run

# --- Step: locate the worker and pin it to APISIX_CORE ----------------------
echo "==> waiting for APISIX worker to come up"
for _ in $(seq 1 30); do
  WORKER_PID="$(pgrep -f 'nginx: worker process' | head -1 || true)"
  [ -n "$WORKER_PID" ] && break
  sleep 1
done
if [ -z "$WORKER_PID" ]; then
  echo "APISIX worker did not start within 30s" >&2
  exit 1
fi
echo "    APISIX worker pid=$WORKER_PID"
sudo taskset -cp "$APISIX_CORE" "$WORKER_PID"
taskset -cp "$WORKER_PID"

# --- Step: register the ai-proxy route --------------------------------------
echo "==> registering ai-proxy route"
ADMIN_KEY="edd1c9f034335f136f87ad84b625c8f1"
curl -sS -o /dev/null -w "    admin route response: %{http_code}\n" \
  -H "X-API-KEY: $ADMIN_KEY" -X PUT \
  http://127.0.0.1:9180/apisix/admin/routes/1 \
  -d '{
    "uri": "/v1/chat/completions",
    "plugins": {
      "ai-proxy": {
        "provider": "openai",
        "auth": { "header": { "Authorization": "Bearer sk-bench" } },
        "override": { "endpoint": "http://127.0.0.1:1981/v1/chat/completions" }
      }
    }
  }'

# Wait a beat for the route to be picked up.
sleep 1

# --- Step: run the client concurrency sweep ---------------------------------
echo "==> starting client sweep (concurrency=$CONCURRENCY)"
taskset -c "$CLIENT_CORES" "$BENCH_BIN" client \
  --apisix-url http://127.0.0.1:9080/v1/chat/completions \
  --apisix-pid "$WORKER_PID" \
  --server-pid "$SERVER_PID" \
  --concurrency "$CONCURRENCY" \
  --warmup "${WARMUP}s" \
  --duration "${DURATION}s" \
  --cool 5s \
  --gomaxprocs 5 \
  --out "$CSV"

# --- Step: sanity check the CSV ---------------------------------------------
echo "==> sanity checks"
python3 - "$CSV" <<'PY' || { echo "    sanity check failed"; exit 1; }
import csv, sys
rows = list(csv.DictReader(open(sys.argv[1])))
if not rows:
    print("no rows in CSV", file=sys.stderr); sys.exit(1)

# 1. peak row apisix_cpu_pct >= 90
peak = max(rows, key=lambda r: float(r["total_t_per_s"]))
cpu = float(peak["apisix_cpu_pct"])
print(f"peak: concurrency={peak['concurrency']} t/s={peak['total_t_per_s']} cpu={cpu}")
if cpu < 90:
    print(f"    WARN: APISIX CPU at peak is {cpu}%, not saturated", file=sys.stderr)

# 2. server/client cpu < 60 across rows
for r in rows:
    s, c = float(r["server_cpu_pct"]), float(r["client_cpu_pct"])
    if s >= 60 or c >= 60:
        print(f"    WARN: load-side near saturation: server={s}% client={c}%", file=sys.stderr)

# 3. throughput non-strictly monotonic up to peak
tps = [float(r["total_t_per_s"]) for r in rows]
peak_idx = tps.index(max(tps))
prev = 0.0
for i, v in enumerate(tps[:peak_idx+1]):
    if v + 1e-6 < prev:
        print(f"    WARN: t/s dropped before peak at row {i}", file=sys.stderr)
        break
    prev = v
print("    sanity checks complete")
PY

echo "==> done. CSV: $CSV"
