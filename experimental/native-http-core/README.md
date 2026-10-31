# Native standalone HTTP core demo

> Latest research: [16 runtime experiments and evidence](EXHAUSTIVE_20261031.md),
> with [the previous single-plugin report](EXPERIMENTS_20261031.md) preserved.
> The comparison uses unchanged stable APISIX 3.19.0 plugin sources, one route plugin
> at a time (Prometheus, key-auth or local limit-count), and excludes `ai` on both sides.
> All pressure samples remain 60 seconds. Module ASan/UBSan, extended pool lifecycle
> and stable-source single-plugin checks have completed. [Two final paired rounds](EXHAUSTIVE_20261031.md#最终两轮稳定lua配对)
> show mean-QPS gains of 7.80% for Prometheus, 3.97% for key-auth and 6.81% for local
> limit-count, below the 30%–40% target. Two rounds establish neither statistical
> significance nor an absolute limit. Combination/allocation/JIT diagnostics and the URI-policy check have completed;
> raw evidence, limitations and the retained negative results are in the report.

## Latest sweep reproduction

The current candidate set contains the first 13 runtime switches, three additions
(`request_pool_native`, `matched_pool`, `balancer_peer_plan`), and the O3,
O3-lto and O3-lto-local build variants. All new switches default to off. The latest
report records shared SDK/IPv6 correctness fixes separately from optimization
results, along with request-lifetime alias, SDK wrapper replacement and
stable-release URI-policy compatibility boundaries.

Run [tools/sweep.py](tools/sweep.py) **inside the prepared test container**, with
this checkout mounted at `/src` and the pinned stable checkout at `/stable`.
Use a new `APISIX_CORE_SWEEP_OUT` directory for each independent reproduction;
existing log labels, benchmark directories and final selection files are rejected
rather than overwritten. Record the selection rule before inspecting the new
screening results; the [archived rule](results/exhaustive-20261031/selection-rule.json)
and [selection implementation](tools/finalize_run.py) specify this run's thresholds.
A new machine or sample set can select a different configuration.

```sh
export APISIX_CORE_SWEEP_OUT="/tmp/apisix-exhaustive-repro-$(date -u +%Y%m%dT%H%M%SZ)"
python3 /src/experimental/native-http-core/tools/sweep.py verify
python3 /src/experimental/native-http-core/tools/sweep.py sanitize
python3 /src/experimental/native-http-core/tools/sweep.py screen
python3 /src/experimental/native-http-core/tools/sweep.py extras
python3 /src/experimental/native-http-core/tools/sweep.py compilers
python3 /src/experimental/native-http-core/tools/sweep.py linkage
python3 /src/experimental/native-http-core/tools/finalize_run.py
```

`screen` runs the first 13 switches with surrounding Native controls. `extras`
runs the three additional switches, keeping `lazy_route_opts` enabled on both
sides of the `matched_pool` comparison. `compilers` runs O3 and O3-lto, then
restores O2 and measures their shared control. `linkage` runs O3-lto-local
(`-O3 -flto -fno-semantic-interposition`), checks the six core configurations and
stable single-plugin contract, then restores O2 and measures its own control.

[finalize_run.py](tools/finalize_run.py) reads those results, saves
`selection-final.json` without overwriting an existing decision, rebuilds and
archives the selected module, and runs core, branch legacy-demo and stable
single-plugin checks. It then runs all three Lua/Native plugin pairs twice,
reversing mode order in the second round. Finally it invokes `diagnostics` for
surrounding controls and the selected combination, allocation/JIT/trace/function
profiles, restores the selected build, and runs the URI-policy difference check.
Those are workflow steps, not a claim that every step in the current run has
already finished; check the latest report for completed evidence.

For a manually chosen configuration instead of the automatic final stage, use
`sweep.py final --label <new-label> --directives '<exact directives>'` twice with
the same directives and distinct labels, adding `--reverse` to the second call.
Never run correctness checks or rebuilds concurrently with throughput samples.

This reruns the method on the current source. The historical first-13 screen
has its own archived source/runtime/module snapshot and must remain unchanged.
Copy new evidence out of the container before removing it. Validation evidence,
retained failures and completed measurement results are maintained in the
[latest report](EXHAUSTIVE_20261031.md#三项追加实验与兼容性修复).

## Previous follow-up and historical setup (preserved)

The 2026-10-31 follow-up uses **three isolated route plugins** (Prometheus,
key-auth, local limit-count), with `ai` excluded from both allowlists. Both
implementations load unchanged APISIX **3.19.0 release** sources, pinned in
[stable-baseline.json](stable-baseline.json), on the same historical runtime.
See [the new experiment report](EXPERIMENTS_20261031.md) and
[new evidence](results/single-20261031/). The older global-Prometheus results
below remain historical evidence, not results for this revised comparison.

The three experimental directives default to **off**:
`apisix_core_log_var_plan`, `apisix_core_lazy_rate_json`, and
`apisix_core_fused_balancer`. Set `APISIX_CORE_DIRECTIVES` to HTTP main directives
when running `dev.py` or `run.py bench`; emitted configurations record them.
The release checkout must be at `../apisix-stable`, at the pinned commit.
After building and verifying as below:

```sh
APISIX_CORE_DIRECTIVES='apisix_core_lazy_rate_json on;' \
  python3 experimental/native-http-core/tools/run.py bench --suite single
```

Each sample remains five seconds warmup plus 60 seconds pressure. The runner
alternates Lua/native order across scenarios and writes a new timestamped
directory, refusing to overwrite evidence. Only the Prometheus scenario scrapes
metrics. `tools/report.py` still reports the **historical** matrix; use the new
directory's `results.json` and `environment.json` for these samples.

This implements the core mechanisms needed by the standalone HTTP demo in an
NGINX C module while calling unchanged Lua plugins. The current agreed scope is
key-auth and local limit-count on prefix routes, with Prometheus globally.
Unused capabilities need documented extension points, not complete ports.
It does not claim compatibility with every APISIX feature or Lua plugin.

Research handoff on branch `apisix-next-v4-llm`:
[temporary context and optimization notes](HANDOFF_TEMP.md),
[next-model prompt](NEXT_MODEL_PROMPT.md), and
[design context](DESIGN_CONTEXT.md). The checked-in `results/` archives retain
positive and negative measurements, verification logs, profiles and available
historical source/binary snapshots. Rebuilding from source is the supported
way to run on another environment.

The user's subsequent [three-pair limit-count recheck](results/limit-recheck-20261031T0617Z/REPORT.md)
uses only `prometheus` and `limit-count` in both plugin enable lists, with no
key-auth or AI plugins. Paired throughput changes were +11.44%, +11.72% and
+13.32%; the ratio of mean QPS was +12.16%. The native binary was unchanged.
`tools/recheck_limit.py` reuses the original sampler and alternates run order;
`tools/report_limit_recheck.py` validates and summarizes its independent archive.
For this historical recheck, first use a separate checkout at
`a3770f02b3adc2c295e993b8234da3e49f26864c`; its source and helpers match the
recorded binary. The branch now also contains later candidate code. See the
[version boundary](results/limit-recheck-20261031T0617Z/PROVENANCE.md).
Run the recheck inside the matching demo container with a fresh output path:

```sh
docker exec apisix-native-demo python3 /src/experimental/native-http-core/tools/recheck_limit.py \
  --source-commit "$(git rev-parse HEAD)" --output /tmp/limit-recheck-new
docker cp apisix-native-demo:/tmp/limit-recheck-new experimental/native-http-core/results/limit-recheck-new
python3 experimental/native-http-core/tools/report_limit_recheck.py experimental/native-http-core/results/limit-recheck-new
```

It verifies that the module and native sources still match the original recorded
binary; a new implementation needs its own explicitly recorded reference.

The subsequent [limit-count-only comparison](results/limit-only-v6-20261031T0644Z/REPORT.md)
disables all global plugins, Prometheus export and scraping on that same frozen
v6 binary. Its three paired throughput changes are +14.28%, +13.94% and +10.28%;
the ratio of mean QPS is +12.81%, with zero observed errors. Exact preparation,
sampler and runtime snapshots plus reproduction steps are in the
[version record](results/limit-only-v6-20261031T0644Z/PROVENANCE.md).

The later [inner-core CPU / GC / latency diagnostic](results/core-cost-v6-20261031T0757Z/REPORT.md)
records those quantities separately at fixed offered load. It also exposes
asymmetric probe/JIT perturbation, so it **does not establish a clean core-only
speedup factor**. See [the measurement method](tools/core_cost/README.md).

The subsequent [subtraction / plugin-amplification experiment](results/subtraction-v6-20261031T0925Z/REPORT.md)
uses the same frozen v6 core without request timing probes: pure NGINX and
no-plugin APISIX baselines, original limit-count, and repeated rate-limit calls.
It reports worker CPU, normal-GC-inclusive differences, positive-k fits and
actual latency; addon/integration cost is distinct from function self time.
See [the method and reproduction steps](tools/subtraction/README.md).

The implementation is separate from the earlier small partial experiment.
Its measurements are not evidence for this implementation.

The [60-second comparisons](RESULTS.md) include the earlier performance
regressions and the current directly connected C API implementation. Scenario
checks and module ASan/UBSan checks pass. Throughput, CPU and memory benefits
are measured outcomes, not assumed consequences of changing language. These
checks do not demonstrate a long-term stability improvement.

## Current implementation

- A compressed native URI prefix tree, exact/prefix priority, host/method/CIDR
  constraints, parameter patterns, compiled variable expressions, and yielding
  route filters.
- Generic C plugin registry and phase plans for rewrite, access, before-proxy,
  header/body/delayed-body filters and log, including priorities, disable,
  prefer-route, metadata filters/pre-functions and rejection handling.
- Native resource objects and Route/Service/PluginConfig/Consumer/ConsumerGroup
  merges. Plugin configuration tables retain their Lua identities where the
  original implementation shares them.
- Direct NGINX C header/body filters for configurations with no Lua filter
  callbacks, route scripts or tracing. These restrictions are checked, and the
  generic callback path is retained. The demo enables this mode because its
  unchanged plugins have no header/body callbacks.
- Native-backed SDK modules under their original `require` names: request and
  variable handling, response operations, schema data/validation orchestration,
  table/string/JSON utilities, LRU/version/negative caching, DNS policy and
  selected resource/metric interfaces. Several SDK modules remain incomplete.
- Variable reads use the existing ngxvar/runtime C primitives directly for
  typed values and aggregated upstream timings, and NGINX's variable API for
  other names. Scalar headers, timeouts, peer setup and keepalive similarly
  call the exported C primitives without re-entering Lua/FFI wrappers.
- Native upstream preparation and selection, weighted round robin, priority
  tiers, retries, timeouts, keepalive integration and asynchronous domain
  resolution. Consistent-hash key selection, weight normalization and retry
  traversal are in C; the existing third-party `resty.chash` hash ring remains a
  primitive. Health/discovery and complete balancing equivalence remain open.
- Native request preparation: servlet normalization, route-only encoded-slash
  handling, tail-slash options, original/normalized URI views, trusted forwarded
  headers and upstream Host/path construction.
- C script loading and phase dispatch, including coroutine suspension in script
  factories and legal request phases. The configured user script remains Lua.
- Standalone JSON file watching and transactional compilation. New requests
  acquire the published generation; in-flight requests retain their old native
  pools and Lua references. Failed JSON/schema/filter candidates keep the old
  configuration. Automatic readers keep their object identity when updated;
  closing a reader stops its updates. Local settings and the enabled-plugin list
  require an NGINX reload. This currently uses the demo's combined JSON input;
  full original provider API and YAML compatibility are still incomplete.

Fixed-window rate-limit diagnostics are serialized directly in C for the
integer shapes used here. The complete JSON remains in the NGINX variable;
its Lua string view is created when read. Other shapes use the existing generic
C table/JSON path. Tests compare the decoded values, including escaping,
missing fields, booleans, precision boundaries and cache invalidation.

For log callbacks with tracing disabled, C batches reads of common final HTTP
and metric-label variables before invoking Lua. Existing cache entries and
registered getters take precedence. This keeps repeated plugin reads on the
compiled cache path, while lookup and admission policy remain native.

NGINX proxy handles HTTP transmission. ngx_lua/LuaJIT and existing third-party
libraries remain runtime dependencies. `runtime.lua` only transports calls and
results across Lua coroutine frames; APISIX scheduling and decisions are in C.
The thin coroutine bridge can replay values C has already published in the
variable cache so cached reads remain inside a compiled plugin trace. C owns
getter dispatch, cache admission and invalidation. Context metadata is compiled
per configuration generation and consumer/group combination.

The direct C adapters target the **pinned runtime's exported ABI**. Upgrading
OpenResty/APISIX runtime requires checking those signatures and rerunning the
regression; this is not an ABI compatibility claim for arbitrary OpenResty builds.
Generated schema headers contain data, preserve table sharing and exclude source
functions. They are not rewritten executable logic or a source-line metric.

## Start, compare and measure

After building the images below and running `tools/dev.py` from the repository:

```sh
python3 experimental/native-http-core/tools/run.py start native
curl http://127.0.0.1:9080/plain/demo
curl -H 'apikey: demo-key' http://127.0.0.1:9080/auth/demo
curl http://127.0.0.1:9091/apisix/prometheus/metrics
python3 experimental/native-http-core/tools/run.py test
python3 experimental/native-http-core/tools/run.py bench
python3 experimental/native-http-core/tools/report.py
```

The host runner uses the dedicated `apisix-native-demo` container and localhost
ports 9080/9091. It can reuse the earlier demo container, stopping its old gateway
before loading the new module. `start lua` selects the original source baseline.
`bench` restores the native five-request example at the end and copies raw data
to `results/`. [SCENARIOS.md](SCENARIOS.md) maps core ownership and boundaries.

## Run the current regression

Build the pinned toolchain/runtime image, then the new module image:

```sh
docker build --secret id=proxy_ca,src=/etc/ssl/certs/ca-certificates.crt \
  -t apisix-native-demo:local experimental/native-standalone
docker build --secret id=proxy_ca,src=/etc/ssl/certs/ca-certificates.crt \
  -t apisix-http-core:local experimental/native-http-core
python3 experimental/native-http-core/tools/dev.py
```

The development runner reuses one disposable container so Docker's VFS storage
driver does not create a full filesystem copy for every edit. It assumes a local
Docker socket at `/var/run/docker.sock` and does not publish ports.

Tests load unchanged `key-auth`, local `limit-count` and global `prometheus`,
plus generic probe plugins. A loader guard rejects original APISIX core Lua
modules. Checks exercise concurrent yielding callbacks, phase ordering, SDK
semantics, rejection/404 metrics, configuration inheritance, real HTTP proxying,
weighted peers, failover and controlled DNS. Six profiles cover default,
servlet, tail-slash, encoded-slash, trusted and untrusted configurations. Unchanged
`workflow` also exercises action registration and skipping a duplicate action.
Reload checks hold a yielding request across publication, reject invalid
candidates and reader filters, update/close readers, repeat generation retirement
and exercise the file watcher. Expression results are compared
with the original third-party evaluator through actual native routes.

Scenario checks compare the original gateway, direct C filters and the generic
native callback bridge. `tools.demo.prepare(..., native_filters=False)` retains
the Lua phase hooks for that control. Enabling `apisix_core_native_filters`
with a Lua filter callback fails closed; callbacks are never silently skipped.

For instrumented checks:

```sh
APISIX_CORE_SANITIZER=address,undefined python3 experimental/native-http-core/tools/dev.py
```

Only the new module is instrumented. ASan leak detection is disabled because the
NGINX/LuaJIT runtime and its other DSOs are not instrumented; this is access and
undefined-behavior evidence, not a full leak or production-stability proof.
The runner's default invocation restores an optimized build.

## Scenario acceptance and boundaries

Scenario acceptance includes an original-source HTTP behavior comparison and
performance measurements for the four routes. Each pressure sample runs for
**60 seconds**, with the same resources, runtime and configuration for Lua and C.

Not implemented for this demo: active/passive health checking, slow start,
service discovery, complete secret-manager/provider support, Redis rate-limit
policies, TLS/stream protocol handling, gRPC/WebSocket proxy switching, every
public plugin-framework/SDK API, and general internal-redirect/subrequest
compatibility. These are outside the user's revised scenario scope. Skeletons
are the upstream selection/feedback interfaces, native SDK module registration,
phase state machine and versioned resource provider; missing behavior is not
claimed to be implemented.

The old full-compatibility inventory in `../native-standalone/COVERAGE.md` remains
an architectural reference; its requirement to finish unused features was
superseded by the user's explicit scope amendment.

## Development profiling

Inside `apisix-http-core-dev`, run
`python3 /src/experimental/native-http-core/tools/profile.py native --prefix combined --sampler jit`.
`--sampler pc` collects Linux program-counter samples; symbol names are nearest
ELF symbols and can be unresolved or approximate for stripped libraries.
`--sampler functions` makes a separate GCC-instrumented native build and reports
function counts and C subtree timings, including instrumentation overhead.
These short profiling runs are not throughput acceptance samples.

After function profiling, run `tools/dev.py` from the worktree to restore and
verify the optimized module. The host runner and benchmark refuse instrumented
modules. Rebuilds atomically replace the module file so an existing worker's
mapped inode is preserved until it exits.
