# Rust ports of the native key-auth paths (ngx-rust)

This directory holds the Rust side of the APISIX v4 core-language study
(NGINX C vs Rust), on the official [ngx-rust](https://github.com/nginx/ngx-rust)
bindings (`ngx` 0.5.1, pinned). It ports two C paths of
[`../native-http-core`](../native-http-core) (research PR membphis/apisix#1):

- **Step 1, "only the core changes, key-auth stays Lua"**: the V1 Lua ABI
  design (C native-lua): the core runs the unchanged Lua key-auth plugin
  through `apisix.native.engine` and the unchanged `runtime.lua`;
- **v2, "C 请求闭环"** (`apisix_core_native_pipeline on`): key-auth native,
  no Lua on the request path.

The C implementation, its harness and its evidence are reused unchanged.

Method and results: [RESULTS_STEP1.md](RESULTS_STEP1.md) (step 1, and the
same-batch v2 numbers); raw data under [`results/`](results/).

## Step 1 scope (same mechanism as C native-lua)

- `apisix.native.engine` with the C protocol: `init`/`init_worker`/`shutdown`,
  `start_phase`/`step`/`render`, `balance_start`/`balance_finish`; the same
  three Lua entries (access, balancer, log) and `runtime.lua` trampoline.
- The core owns phase scheduling (rewrite, consumer-route merge, access,
  before_proxy, log), routing, upstream preparation and peer selection
  (through ngx_lua's balancer API), the api_ctx table pool, `ctx.var`,
  forwarding-header sanitization and the native header filter.
- The SDK subset key-auth uses, implemented with the Lua C API (`sdk.rs`):
  `apisix.core` (`request`, `response`, `log`, `schema`, `ctx`, `version`),
  `apisix.schema_def`, `apisix.consumer`, `apisix.utils.auth`, `apisix.tracer`.
  Members outside the subset raise an explicit error.
- The same closed-world configuration contract as v2 (compiled by
  `apisix-rs-core`); plugin configurations get their defaults from the Lua
  plugin's own `check_schema`, as in C.

## v2 scope (same contract as v2)

- Route-only literal key-auth: header/query key, anonymous consumer,
  `hide_credentials`, `X-Consumer-Username`/`X-Credential-Identifier`/`X-Consumer-Custom-ID`.
- Compressed URI prefix tree with the C precedence rules (host rank, exact,
  prefix length, priority, source length, configuration order), host/method/CIDR predicates.
- Upstream preparation and NGINX peer callbacks: smooth weighted round robin
  (plus least_conn/ewma), priority tiers, retries/timeouts, `proxy_next_upstream off`
  precedence, stock `keepalive` wrapping, log-phase feedback.
- Standalone `native.json`: generations pinned per request, a 1 s file watcher,
  whole-generation rejection of unsupported configuration, reload-only settings.
- No Lua on the request path; configuration is parsed and compiled in Rust (serde_json).

Deliberate differences from the C module, all compile-time rejections
("nothing silently omitted"): unknown fields, services, plugin configs,
consumer groups, regex/parameter route patterns and the parameter router are
rejected; the v3 Lua log bridge is not ported.

## Layout

| Path | Content |
|---|---|
| `core/` | `apisix-rs-core`: NGINX-independent logic, `#![forbid(unsafe_code)]`, unit tests |
| `module/` | `ngx_http_apisix_rs_module` cdylib: directives, variables, phases, peer callbacks, watcher, panic containment |
| `module/src/step1.rs`, `sdk.rs`, `lua.rs` | step 1: engine protocol and Lua-side generation, SDK subset, Lua C API declarations |
| `module/src/filters.rs` | native response header filter (`Server`, `X-APISIX-Upstream-Status`), both modes |
| `tools/build_module.py` | container build (offline, vendored crates) against the C module's configured NGINX tree |
| `tools/rust_demo.py` | runs the unchanged research harness with an extra `rust` mode |
| `tests/test_rust_parity.py` | same-batch parity: stable Lua, C step 1 (native-Lua), Rust step 1, C v2, Rust v2 (47 cases, response headers, lifecycle) |
| `tests/test_rust_panic.py` | panic containment (test-only build) |
| `tools/fixed_rate.c` | open-loop fixed-rate load generator with coordinated-omission correction |
| `tools/bench_three.py` | same-batch throughput and fixed-rate performance of the five paths |
| `tools/code_stats.py` | line counts and unsafe share |
| `docker/Dockerfile` | adds Rust 1.97.0 and libclang to `apisix-http-core:local` |

Directives, v2: `apisix_rs_config <native.json>;` (http), `apisix_rs_proxy on;`
(location), `apisix_rs_balancer;` (upstream, before `keepalive`).
Step 1: `apisix_rs_lua_plugins on;` (http) and `apisix_rs_proxy on;`
(location); the configuration path comes from `engine.init` in `init_by_lua`,
as with the C module.

## Reproduce

Images (from the repository root; `--network host` lets builds reach the
session proxy, and the proxy CA is passed as a build secret):

```sh
docker build --network host --secret id=proxy_ca,src=<ca-bundle> -t apisix-native-demo:local <native-standalone context>
docker build --network host --secret id=proxy_ca,src=<ca-bundle> -t apisix-http-core:local experimental/native-http-core
docker build --network host --secret id=proxy_ca,src=<ca-bundle> -t apisix-http-core-rs:local experimental/native-http-core-rs/docker
```

In this study's environment, GitHub archive downloads were blocked, so the
`native-standalone` context was a copy whose `lua-resty-limit-traffic` v1.2.0
step copies a git clone of the same tag (`094d4f0`) instead of downloading
the archive. The research files themselves are unchanged.

Vendor crates on the host (`cargo vendor --versioned-dirs <dir>`), then start
the container with the pinned 3.19.0 release at `../apisix-stable`:

```sh
docker run -d --init --name apisix-http-core-dev --ulimit nofile=<n>:<n> \
  -v "$PWD:/src:ro" -v "$PWD/../apisix-stable:/stable:ro" \
  -v /sys/fs/cgroup/cpu/cpu.stat:/host-cpu.stat:ro -v <vendor-dir>:/vendor:ro \
  --entrypoint sleep apisix-http-core-rs:local infinity
docker exec -e APISIX_CORE_OPTIMIZATION=O3 apisix-http-core-dev python3 /src/experimental/native-http-core/tools/rebuild.py
docker exec -e APISIX_RS_CLIPPY=1 apisix-http-core-dev python3 /src/experimental/native-http-core-rs/tools/build_module.py
docker exec apisix-http-core-dev python3 /src/experimental/native-http-core-rs/tests/test_rust_parity.py --out /tmp/parity.json
docker exec -e APISIX_RS_FEATURES=panic-test apisix-http-core-dev python3 /src/experimental/native-http-core-rs/tools/build_module.py
docker exec apisix-http-core-dev python3 /src/experimental/native-http-core-rs/tests/test_rust_panic.py --mode rust
docker exec apisix-http-core-dev python3 /src/experimental/native-http-core-rs/tests/test_rust_panic.py --mode rust-lua
docker exec apisix-http-core-dev python3 /src/experimental/native-http-core-rs/tools/build_module.py   # restore release
docker exec apisix-http-core-dev gcc -O2 -Wall -Wextra -pthread -o /opt/fixed_rate /src/experimental/native-http-core-rs/tools/fixed_rate.c
docker exec apisix-http-core-dev python3 /src/experimental/native-http-core-rs/tools/bench_three.py --out /tmp/bench-<ts> --rates 13300,29500
# gray zone (3%-8%) on step 1: one extra pair, then merge the rounds
docker exec apisix-http-core-dev python3 /src/experimental/native-http-core-rs/tools/bench_three.py --out /tmp/bench-extra-<ts> \
  --rounds 2 --first-round 3 --rates 13300,29500 --variants native-c-lua-key-auth native-rust-lua-key-auth
python3 tools/analyze_three.py <batch>/results.json <extra>/results.json \
  --variants native-c-lua-key-auth native-rust-lua-key-auth --summary <dir>/summary.json
```

Without `--rates`, R_low and R_high are derived from the first round (80% of
Lua and of C v2 throughput); this study reused the first batch's values.

`build_module.py` runs the core unit tests (and clippy with `-D warnings`
when `APISIX_RS_CLIPPY=1`) before building. The C module must be rebuilt
first: both modules use the same configured NGINX tree and module signature.

## Environment notes

- 4 vCPU cloud VM (cgroup v1): gateway worker CPU0, upstream CPU1, load
  drivers CPU2–3 (the v2 report used CPU2–4). Absolute numbers are not
  comparable with the v2 report; compare only within one batch.
- The fixed-rate sampler is the repository's own `tools/fixed_rate.c`
  (wrk2-style intended-time latency) because building third-party load tools
  was not permitted in this environment.
- The file-descriptor limit followed the host hard limit (20000).
