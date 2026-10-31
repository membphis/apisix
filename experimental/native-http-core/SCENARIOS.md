# Demo scope and native ownership

The user's final scope requires the mechanisms exercised by these scenarios to
behave correctly. Unused capabilities may be recorded as extension points.
This replaces the earlier requirement to finish every APISIX core API.

| Scenario mechanism | Native implementation | Evidence |
| --- | --- | --- |
| Initialization and worker lifecycle | `module.c`, `plugins.c`, `resources.c` | Both startup paths use the same rendered APISIX template; native startup rejects original Lua core loading |
| Request context and original plugin API | `module.c`, `request.c`, `response.c`, `sdk.c` | Request/header/body forwarding; header/query credentials; real plugin errors and client disconnect |
| URI routing and misses | `routes.c`, `expression.c`, `http.c` | Exact/prefix routing, overlapping authenticated prefix, normalized path, unmatched 404; additional router profiles |
| Generic global and route scheduling | `plugins.c`, `merge.c`, `native_phases.c` | C phase plans select the unchanged plugins; global Prometheus observes success, 401, 429, 404, 503 and 504; direct C filters and generic callback bridge agree |
| Authentication and identity merge | `consumer.c`, `merge.c`, `cache.c` | Original key-auth, hidden credentials, consumer identity and independent consumer quota |
| Local count quota | `cache.c`, `request.c`, `utils.c` plus the original plugin and limit-traffic runtime | Five accepted requests, quota reset, and exactly 5/27 accepted/rejected with two workers |
| Static HTTP upstream and dynamic keepalive | `upstream.c` | Real backend requests, body/HEAD, same backend connection reused, upstream failure and timeout |
| Metrics and finalization | `plugins.c`, `instrumentation.c`, `response.c`, `tracing.c` | Exact status counter increments, route/consumer labels, latency/bandwidth output and zero metric-error counters |

`runtime.lua` only transports calls/results across legal Lua coroutine frames.
It does not select routes, merge resources, build plugin plans or select peers.
Original Lua plugin business files and their helpers are copied without edits.

NGINX handles parsing, HTTP transport, events and connection pools. ngx_lua,
LuaJIT and the installed third-party libraries remain runtime dependencies.
Schema headers contain generated data, not original executable Lua core logic.
The APISIX CLI is reused at preparation time to render a shared NGINX template
and export local defaults; its Lua core does not execute in the native gateway.
The C SDK calls the existing exported ngxvar, header and balancer primitives
directly. Those signatures are coupled to the pinned runtime. Thin Lua cache
replay only returns values published by C; lookup policy and invalidation remain
native. Route and identity metadata caches belong to their resource generation.
The fixture enables direct C header/body filters because it has no corresponding
Lua callbacks, scripts or tracing. Unsupported combinations fail closed. The
generic phase bridge is retained and checked separately. Fixed-window diagnostic
JSON is built in C; the full NGINX variable value remains observable and its Lua
view is materialized on demand. Other shapes retain the generic encoding path.
Final HTTP log variables are prepared in a C batch when tracing is disabled;
already cached values, false, and custom getter timing are checked against the
original gateway through actual Prometheus labels.

## Extension points outside this delivery

| Unused feature | Existing extension point / status |
| --- | --- |
| Active/passive health, slow start and service discovery | Native upstream preparation, peer-selection and outcome interfaces; complete policies are not implemented |
| Redis rate-limit policies, distributed coordination | Native cache/event interfaces exist; only the plugin's local policy is accepted here |
| Remote secret managers and environment-backed credentials | Resource/SDK registration points; these credential modes are not accepted by this fixture |
| TLS, stream, gRPC and WebSocket proxy switching | NGINX transport integration and phase interface; no protocol-completeness claim |
| All other plugin SDK APIs and external plugin/wasm integration | Native preload registry; unimplemented interfaces are not claimed compatible |
| General internal redirect/subrequest semantics | Native request context and phase entry points; only the documented direct HTTP scenarios are accepted |

Existing extra implementations for DNS, scripts, consistent hashing, resource
reload and other router profiles are retained and regression-tested. Their
presence does not expand the scenario's compatibility claim to all APISIX APIs.

## Runnable checks

`python3 experimental/native-http-core/tools/dev.py` from the worktree builds the native
module and runs six core profiles plus the original/native scenario comparison.
Each core profile includes Lua callback suspension, resource merges, scripts,
DNS, hash selection, transactional reload and generation retirement.

`APISIX_CORE_SANITIZER=address,undefined python3 experimental/native-http-core/tools/dev.py` runs the same
checks with the native module instrumented. The runtime DSOs are not instrumented
and leak detection is disabled; this verifies accesses and undefined behavior.

Performance uses `tools/run.py bench`: four route scenarios, two implementations,
one 60-second sample each, five-second warmup and global Prometheus in every
sample. It validates worker identity, affinity, request errors and gateway logs.
Closed-loop latency percentiles are measured at each mode's achieved throughput.
