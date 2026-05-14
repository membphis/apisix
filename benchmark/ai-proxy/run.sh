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
  if [ -n "${SERVER_PID:-}" ]; then
    kill "$SERVER_PID" 2>/dev/null
  fi
  # Snapshot APISIX error log from inside the container before tearing it down.
  if [ -n "$(docker ps -q -f name=apisix-bench 2>/dev/null)" ]; then
    docker exec apisix-bench cat /usr/local/apisix/logs/error.log > "$ERR_COPY" 2>/dev/null || true
  fi
  docker rm -f apisix-bench 2>/dev/null
  sudo killall pidstat 2>/dev/null
}
trap cleanup INT TERM EXIT

# Validate sudo access so `taskset -cp` doesn't prompt mid-run.
# (Caller should run `sudo -v` in their shell first to prime the credential cache.)
sudo -n true || { echo "sudo access required (run 'sudo -v' first)" >&2; exit 1; }

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

# --- Step: start APISIX (apache/apisix:dev) in docker -----------------------
echo "==> starting APISIX in docker (standalone yaml mode)"
APISIX_CONTAINER="apisix-bench"
docker rm -f "$APISIX_CONTAINER" 2>/dev/null || true
# Copy config template to a writable working file under RESULT_DIR.
# data_plane mode doesn't write back to config.yaml, but this avoids
# any future surprises if the config mode changes.
APISIX_CONFIG_RW="$ROOT/$RESULT_DIR/config.yaml"
cp "$ROOT/$BENCH_DIR/conf/config.yaml.tpl" "$APISIX_CONFIG_RW"
# --network host: APISIX :9080 on host loopback, no NAT, zero network overhead
# --pid    host: container processes visible to host pgrep + taskset -cp
# config.yaml mounted writable (APISIX writes a UUID back on first start).
# apisix.yaml (routes) mounted read-only — APISIX never writes to it.
docker run -d \
  --name "$APISIX_CONTAINER" \
  --network host \
  --pid host \
  -v "$APISIX_CONFIG_RW:/usr/local/apisix/conf/config.yaml" \
  -v "$ROOT/$BENCH_DIR/conf/apisix.yaml.tpl:/usr/local/apisix/conf/apisix.yaml:ro" \
  apache/apisix:dev > /dev/null

# --- Step: wait for the worker, then pin to APISIX_CORE ---------------------
echo "==> waiting for APISIX worker"
for _ in $(seq 1 30); do
  WORKER_PID="$(pgrep -f 'nginx: worker process' | head -1 || true)"
  [ -n "$WORKER_PID" ] && break
  sleep 1
done
if [ -z "$WORKER_PID" ]; then
  echo "APISIX worker did not start within 30s; container logs:" >&2
  docker logs --tail 40 "$APISIX_CONTAINER" >&2 2>&1
  exit 1
fi
echo "    APISIX worker pid=$WORKER_PID"
sudo taskset -cp "$APISIX_CORE" "$WORKER_PID"
taskset -cp "$WORKER_PID"

# --- Step: probe the route to confirm it loaded ------------------------------
# Standalone mode: route comes from apisix.yaml, no admin API needed.
# Verify by a quick HEAD against the route. APISIX will return 405 from
# ai-proxy on HEAD (since the plugin only accepts POST/GET as configured),
# but a 405 still proves the route matched (vs. 404 if it didn't).
sleep 1
ROUTE_CHECK=$(curl -s -o /dev/null -w '%{http_code}' -X POST \
  http://127.0.0.1:9080/v1/chat/completions \
  -H 'Content-Type: application/json' \
  --max-time 2 \
  -d '{"model":"gpt-4o","messages":[{"role":"user","content":"x"}],"stream":false}' || true)
echo "    route probe HTTP status: $ROUTE_CHECK"
case "$ROUTE_CHECK" in
  2*|4*|5*) ;;  # 2xx = ok via proxy; 4xx/5xx = route reached APISIX (would be 404 otherwise)
  *)
    echo "    route not loading; container logs:" >&2
    docker logs --tail 40 "$APISIX_CONTAINER" >&2 2>&1
    exit 1 ;;
esac

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
