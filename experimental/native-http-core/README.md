# Native standalone HTTP core demo

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
