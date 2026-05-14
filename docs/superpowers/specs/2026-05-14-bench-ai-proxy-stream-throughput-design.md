# Design: ai-proxy Single-CPU Streaming Throughput Benchmark

**Date**: 2026-05-14
**Status**: Approved
**Worktree branch**: `worktree-bench-ai-proxy-stream-throughput`

## Goal

Measure the maximum tokens-per-second that an APISIX worker pinned to a single
CPU core can pass through when running the `ai-proxy` plugin in OpenAI Chat
Completions streaming passthrough mode, with no other plugins enabled.

The result we report is a single number with stated conditions:

> "On a {CPU model}, with `worker_processes=1` pinned to one core,
> `ai-proxy` configured for OpenAI passthrough (no converter, no other
> plugins), the single-worker ceiling is **T tokens/s** at concurrency N
> and APISIX CPU P%."

Out of scope: comparison baselines (raw nginx, ai-proxy-multi, converters,
other providers), production capacity planning, latency SLO design.

## Why this benchmark

Prior analytical estimate (see worktree conversation) put the ceiling around
20,000 tokens/s/core based on per-event Lua cost of ~50 µs, dominated by
`cjson` decode in `parse_sse_event`. This benchmark validates that estimate
with a controlled experiment and produces a defensible number for capacity
planning.

## §1 Architecture & data flow

```
                taskset -c 3-7                    taskset -c 0                taskset -c 1-2
              ┌──────────────┐  HTTP/1.1   ┌────────────────────┐  HTTP/1.1  ┌──────────────────┐
              │  bench       │─ N 个 ───▶  │  APISIX worker     │─ N 个 ──▶  │  bench server    │
              │  client      │   长连接     │  worker_procs=1    │   长连接   │  (fake OpenAI)   │
              │  (goroutine) │◀── SSE ────│  ai-proxy:openai   │◀── SSE ─── │  GOMAXPROCS=2    │
              └──────────────┘              └────────────────────┘            └──────────────────┘
                     │                              │
                     └─── 每秒采样 ─────▶   pidstat APISIX worker CPU%
                     │
                     ▼
              bench-result.csv：concurrency, total_t_per_s, apisix_cpu_pct, p50/p99 inter-event
```

Core constraints:

- APISIX: `worker_processes=1` + `taskset -c 0`; no access log; no proxy-mirror,
  proxy-cache, prometheus, or other plugins beyond `ai-proxy`.
- `ai-proxy` route uses `openai` provider with `override.endpoint` pointed at
  the fake server. Auth header is a dummy `Bearer sk-bench`.
- bench server and bench client are the same Go binary (subcommands), isolated
  via `taskset` to cores that do not overlap with APISIX core 0.
- All traffic over loopback. No TLS upstream (eliminates a confounding
  variable; ai-proxy code path for SSE is identical for http vs https).
- bench server pre-encodes a single ~200-byte realistic OpenAI chunk and reuses
  it for every event, so the upstream side does no JSON work in the hot loop.

## §2 bench server (fake OpenAI upstream)

Responsibility: serve `POST /v1/chat/completions` and stream pre-encoded SSE
chunks as fast as `Write+Flush` allows, until the client closes. Cannot be the
bottleneck.

**Endpoint**:

```
POST /v1/chat/completions
Response headers:
  Content-Type: text/event-stream
  Cache-Control: no-cache
  X-Accel-Buffering: no
  Connection: keep-alive
Body: pre-encoded SSE chunks, infinite stream until ctx.Done() / client close
```

The handler reads and discards the request body (so the kernel doesn't stall
on unread data), then loops `Write(preEncoded) + Flush()` until the connection
is closed. No `[DONE]` is emitted — the long-lived connection model has the
client close to end a measurement window.

**Pre-encoded chunk** (realistic OpenAI shape, ~200 bytes):

```
data: {"id":"chatcmpl-bench","object":"chat.completion.chunk","created":1715000000,"model":"gpt-4o","choices":[{"index":0,"delta":{"content":" hello"},"finish_reason":null}]}

```

Encoded once at server startup; the hot loop only writes raw bytes.

**Command line**:

```
bench server
  --listen :1981
  --token-text " hello"      # default 6 bytes; affects chunk size
  --gomaxprocs 2
```

No `--rate`, no `--max-tokens`. This benchmark is the "ceiling" mode; rate
controls would add timer goroutine overhead and obscure real APISIX cost.

**Implementation notes**:

- `http.Server` + bare `http.HandlerFunc`; no middleware.
- Uses `w.(http.Flusher).Flush()` per chunk. No `bufio.Writer` wrapping.
- Listens on `:1981` (slot follows existing `benchmark/server` port range).
- Handles SIGINT/SIGTERM for graceful shutdown.
- No access log.
- Prints a startup banner to stdout containing its own PID
  (`bench-server: pid=12345 listen=:1981`) so the harness and client can
  capture it for `pidstat`.

## §3 bench client (load generator)

Responsibility: open N concurrent SSE long-lived connections to APISIX, count
received tokens, sample APISIX worker CPU, emit CSV. Cannot be the bottleneck.

**Behavior per concurrency level** (from `--concurrency 1,2,4,8,16,32,64`):

1. Spawn N goroutines; each opens one `POST /v1/chat/completions` with
   `stream:true`.
2. Discard first **warmup (5s)** of traffic — covers cold start and any JIT-like
   warmup behavior.
3. During the **duration (60s)** measurement window:
   - Each goroutine increments a local token counter on every `data:` SSE
     event line (no atomics; lock-free per-goroutine state).
   - Each goroutine records inter-event latency into a local HdrHistogram
     (range 1µs–1s, 3 significant figures).
4. In parallel with the window, spawn `pidstat -p $APISIX_PID 1 $duration`,
   parse `%CPU` column; discard first sample, average the rest.
5. Cancel all goroutines, close connections, wait for cleanup.
6. Merge per-goroutine histograms, sum token counts, append one row to CSV.
7. **Cool 5s** between concurrency levels (let TIME_WAIT drain).

**Request body** (sent once per connection):

```json
{
  "model": "gpt-4o",
  "messages": [{"role": "user", "content": "hi"}],
  "stream": true
}
```

We deliberately do NOT set `stream_options.include_usage`. The ai-proxy plugin
injects it automatically via `prepare_outgoing_request` (see
`apisix/plugins/ai-protocols/openai-chat.lua:48`) — and that injection is part
of the code path we're measuring.

**SSE parsing**: `bufio.Scanner` line-by-line. Count lines that start with
`data: `. Ignore blank lines and `data: [DONE]`. No JSON decode. The client
must not become the bottleneck.

**Dependencies**: `github.com/HdrHistogram/hdrhistogram-go` for inter-event
latency. No other third-party deps; everything else is stdlib.

**CPU sampling**: shell out to `pidstat -p $APISIX_PID 1 $duration -u`, parse
`%CPU` column. Discard the first sample (pidstat's first interval often
under-reports). Sample count must be ≥ 20 or the row is flagged invalid.

**APISIX worker PID** is passed in by the harness via `--apisix-pid`. The
harness queries `pgrep -f 'nginx: worker process'` once after start.

**Command line**:

```
bench client
  --apisix-url http://127.0.0.1:9080/v1/chat/completions
  --apisix-pid 12345
  --server-pid 23456                 # bench server PID (for CPU sampling)
  --concurrency 1,2,4,8,16,32,64
  --warmup 5s
  --duration 60s
  --cool 5s
  --gomaxprocs 5
  --out bench-result.csv
```

**CSV columns**:

```
concurrency,duration_s,total_tokens,total_t_per_s,apisix_cpu_pct,server_cpu_pct,client_cpu_pct,p50_inter_event_us,p99_inter_event_us
```

**Per-row sanity** (enforced by client):

- ≥ 1000 events received per concurrency level (else error: upstream or route
  broken).
- ≥ 20 CPU samples (else error: pidstat unreliable).

## §4 APISIX configuration

**`conf/config.yaml` overrides** (written by harness from a template):

```yaml
deployment:
  role: traditional
  role_traditional:
    config_provider: yaml

nginx_config:
  worker_processes: 1
  error_log_level: warn
  http:
    access_log: 'off'

plugins:
  - ai-proxy
```

Notes:

- `proxy-mirror`, `proxy-cache`, `prometheus` are excluded from the plugin
  list. Existing `benchmark/run.sh` uses `sed` to comment them in
  `config-default.yaml`; we instead write a minimal explicit list to avoid
  depending on the upstream default ordering.
- **Fallback**: if APISIX startup fails because some plugin required for
  routing is missing from the explicit list, switch to the sed-based
  exclusion approach (copy `config-default.yaml`, comment out only
  `proxy-mirror`, `proxy-cache`, `prometheus`). Document which path was
  used in `env.txt`.
- Access log disabled at the http block level.
- `options` (model_options) is intentionally omitted — the client already
  sets `model` in the request body, and adding `options` would just
  overwrite it with the same value through the `build_request` hot path
  (`apisix/plugins/ai-proxy/base.lua:202`).

**Route** (admin API):

```bash
curl http://127.0.0.1:9180/apisix/admin/routes/1 \
  -H "X-API-KEY: $admin_key" -X PUT -d '
{
  "uri": "/v1/chat/completions",
  "plugins": {
    "ai-proxy": {
      "provider": "openai",
      "auth": {
        "header": { "Authorization": "Bearer sk-bench" }
      },
      "override": {
        "endpoint": "http://127.0.0.1:1981/v1/chat/completions"
      }
    }
  }
}'
```

We deliberately leave the following unset to test the default code path:

- `keepalive_pool`, `keepalive_timeout` — ai-proxy defaults
- `max_response_bytes`, `max_stream_duration_ms` — enabling either adds a
  per-chunk size/time check in the hot loop; that path is a different
  measurement
- `ssl_verify` — http upstream, no TLS

**Pinning the worker** (after APISIX start):

```bash
# poll until worker is up (sleep 2s is brittle on slower machines)
for i in $(seq 1 30); do
  WORKER_PID=$(pgrep -f 'nginx: worker process' | head -1)
  [ -n "$WORKER_PID" ] && break
  sleep 1
done
[ -z "$WORKER_PID" ] && { echo "APISIX worker did not start in 30s" >&2; exit 1; }

sudo taskset -cp 0 "$WORKER_PID"
taskset -cp "$WORKER_PID"   # verify affinity mask is 1
```

With `worker_processes=1`, the single worker is unambiguous; no PID filtering
needed.

## §5 Harness script

Layout:

```
benchmark/ai-proxy/
├── run.sh                  # entry point
├── bench/                  # Go single-binary source
│   ├── go.mod
│   ├── main.go
│   ├── server.go
│   └── client.go
├── conf/
│   └── config.yaml.tpl
└── result/                 # outputs (csv, logs)
```

**Main flow** (in order, top-down):

1. Parse env vars: `DURATION`, `WARMUP`, `CONCURRENCY`, `APISIX_CORE`,
   `SERVER_CORES`, `CLIENT_CORES`. All have defaults from §3.
2. `go build` the bench binary into `result/`.
3. Plain `cp conf/config.yaml.tpl → conf/config.yaml` (no templating engine;
   the template has no variables).
4. Start fake server in background: `taskset -c $SERVER_CORES bench server …`.
   Capture `$SERVER_PID = $!`.
5. `make init && make run` to start APISIX.
6. Poll for the worker (up to 30s): `pgrep -f 'nginx: worker process'`. Bind
   it: `sudo taskset -cp 0 $WORKER_PID`.
7. Register the route via admin API (§4).
8. Run client: `taskset -c $CLIENT_CORES bench client --apisix-pid $WORKER_PID
   --server-pid $SERVER_PID …` (blocks for the full sweep duration).
9. Cleanup via trap: `make stop`, kill server, kill any leftover pidstat.

**sudo handling**: `sudo -v` at script start to cache credentials for the
`taskset -cp` calls during the run. Everything else runs as the invoking user.

**Cleanup**:

```bash
cleanup() {
  make stop 2>/dev/null || true
  kill "$SERVER_PID" 2>/dev/null || true
  sudo killall pidstat 2>/dev/null || true
}
trap cleanup INT TERM EXIT
```

**Not included** (by design):

- No CI integration; this is a one-shot experiment, run manually.
- No plotting in this design; CSV → external tooling.
- No retry on failure; the script exits non-zero with a clear stderr message.

## §6 Output & acceptance criteria

**Produced artifacts** (in `benchmark/ai-proxy/result/`):

```
bench-result.csv          # per-concurrency rows
server.log                # bench server stdout/stderr (expected near-empty)
apisix-error.log          # copy of logs/error.log after the run
env.txt                   # git sha, kernel, CPU model, go version, date
```

**Primary deliverable**: peak `total_t_per_s` from CSV, plus the concurrency
and `apisix_cpu_pct` at that row. Stated in the form given in the Goal section.

**Sanity checks** (run by `run.sh` after the client exits; failures emit a
warning to stderr and a non-zero exit code):

1. At the peak-throughput row, `apisix_cpu_pct ≥ 90`. If not, the bottleneck
   is elsewhere and the number is not an APISIX ceiling.
2. The throughput curve is non-strictly monotonic up to peak (allowed to
   plateau or slightly drop after).
3. Each concurrency row has ≥ 1000 events (already enforced by client).
4. bench server `%CPU` < 60 and bench client `%CPU` < 60. The client itself
   `pidstat`-samples its own PID and the bench-server PID (parsed from
   server stdout banner at startup) during each concurrency level's window,
   and writes the means to the CSV in extra columns
   `server_cpu_pct,client_cpu_pct`. Either > 60 → warn "load-side may be
   saturating."

**When sanity fails**:

| Symptom | Likely cause | Adjustment |
|---------|--------------|------------|
| APISIX CPU < 80% but t/s plateaued | client or server saturated | raise `--gomaxprocs`, widen `*_CORES` |
| t/s linear with concurrency to last point | sweep too narrow | extend list to 128, 256 |
| Single-row variance > 15% | noise / contention | verify `taskset`, idle machine, `worker_processes=1` |
| `too many open files` in apisix error.log | rlimit too low | `ulimit -n 65535` or set `worker_rlimit_nofile` |

**Repeat for statistical confidence** (optional): `REPEAT=3 run.sh` reruns each
concurrency level three times; user takes the median externally. Default
`REPEAT=1` — first goal is reproducible numbers, statistical rigor is a layer
above this.

**Explicitly out of scope** for this design (will not be implemented unless
asked):

- Plotting / visualization
- Baseline: bare nginx without ai-proxy
- ai-proxy-multi
- Anthropic→OpenAI converter path
- Other providers (anthropic, bedrock, vertex, etc.)
