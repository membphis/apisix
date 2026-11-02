<!--
Licensed to the Apache Software Foundation (ASF) under one or more
contributor license agreements. See the NOTICE file distributed with
this work for additional information regarding copyright ownership.
The ASF licenses this file to You under the Apache License, Version 2.0
(the "License"); you may not use this file except in compliance with
the License. You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# Earlier full HTTP core coverage reference

The user subsequently narrowed delivery to mechanisms used by the key-auth,
local limit-count and global Prometheus demo. Unused health, slow-start,
discovery and other APIs need extension points/documentation, not full ports.
The full-compatibility requirements below are retained as an earlier architectural
reference. Current acceptance is in
[the new scenario map](../native-http-core/SCENARIOS.md).


Source baseline: `b31e828c6f6b588ae954bf69c8432c08fb73bb7e`.

The user clarified that standalone limits deployment/configuration and demo
scenarios, not the completeness of the HTTP core rewrite. The scope is the
complete standalone HTTP data plane; TLS and stream protocols are outside this
iteration. Lua plugin business code remains unchanged. A thin coroutine/runtime
adapter may remain; APISIX core business decisions must be implemented in C.

The user's compatibility target is **all existing plugins**, with no plugin
source or call-site changes. key-auth, limit-count and Prometheus are the first
validation workloads, not a whitelist for native behavior. The API surface below
plugins is part of the rewrite, including APISIX SDK utilities and background
mechanisms. Reusing their original Lua business implementations is not native
completion.

The literal scan in [CORE_API_INVENTORY.json](CORE_API_INVENTORY.json) examines
224 plugin Lua files, including helpers. It finds 43 directly imported non-plugin
APISIX modules, 84 modules in the literal dependency closure, and 137 distinct
`core.*` member access paths. It misses dynamic imports, aliases and computed
members and is a lower-bound inventory, not a compatibility proof. HTTP plugins
also import metadata APIs from SSL/stream modules; keeping those imports usable
does not add TLS/stream request processing to this iteration.

Native-backed SDK facades must preserve Lua-visible types, table identity and
mutations, function arguments/returns, nil/null and array metatables, ctx/variable
semantics, metadata/resource keys, errors, coroutine yield/resumption and cleanup.
Moving core logic to C does not authorize changing the plugin ABI to userdata or
rewriting plugins around a new C-only API. Native decisions may continue to use
Lua tables as the compatibility data representation.

The existing module has 267 total C lines (230 excluding comments/blanks).
It implements a bounded linear prefix matcher, a request pointer, variables
carrying an index/bitmask and a static upstream name. It does not implement
the following source responsibilities completely.

| Source responsibility | Existing experiment | Required native behavior |
| --- | --- | --- |
| `init.lua:http_init`, `http_init_worker`, `http_exit_worker` | Reuses original Lua initialization | Native configuration/resource initialization, plugin lifecycle orchestration, worker start/exit and cleanup; no call into the old Lua core initializer |
| `init.lua:http_access_phase` and helpers | A replacement Lua access function | Native request context creation, URI/trusted-header processing, route selection, configuration resolution, phase ordering, rejection and proxy preparation |
| `init.lua:common_phase` | Reuses `plugin.run_plugin`; global Prometheus hardcoded | Generic native global/route phase scheduling and callback results across all HTTP phases |
| `plugin.lua:filter`, priorities and metadata | Two plugin bits with Lua-built callback arrays | Native plugin descriptors, priorities, enable/disable, metadata filters, phase plans, short-circuit/errors and prefer-route semantics |
| `plugin.lua` / `plugin_config.lua` configuration merge | Service, PluginConfig and consumer overrides excluded | Native Route/Service/PluginConfig/Consumer/ConsumerGroup inheritance, precedence, version/cache invalidation and consumer rewrite behavior |
| `http/route.lua`, `http/router/*.lua`, `router.lua` | Only longest literal prefix, max 64 | Native route indexes and original matching semantics: URI exact/prefix/parameters, host constraints, methods, remote addresses, expressions, priority, captures and misses |
| `core/ctx.lua`, request/response helpers | Calls Lua `core.ctx`, request/response APIs | Native authoritative request state and native-backed compatible APIs for Lua plugins; lifecycle and cleanup on every exit path |
| `consumer.lua`, `consumer_group.lua`, global rules | Original Lua consumer/global state | Native resource lookup/indexes, identity binding, configuration inheritance, global policy semantics and metadata |
| `upstream.lua`, `init.lua:handle_upstream` | One shared static upstream, no equivalent preparation | Native inline/referenced upstream resolution, route overrides, host/URI/headers, timeout/retry policy, before-proxy dispatch and response provenance |
| `balancer.lua`, `balancer/*.lua`, health and slow start | Delegates a single node to NGINX | Native equivalent peer selection, retry accounting, weights/priorities, health/failure behavior and cleanup for supported HTTP upstream features |
| `init.lua:http_header_filter_phase` | Replaced with an empty Lua hook | Native core header processing and full global/route header-filter dispatch |
| `init.lua:http_body_filter_phase`, delayed filtering | Replaced with an empty Lua hook | Native buffer/end-of-stream phase state and plugin filter dispatch; no dropped body-filter behavior |
| `init.lua:http_log_phase` | Replacement Lua log function | Native exactly-once finalization, global/route log dispatch, timing/provenance, health feedback, picker callbacks and resource release |
| HTTP subrequests/internal redirects/early exits | Only a small fixed happy-path context | Native context preservation and correct later-phase execution for redirects, rejection, timeout, upstream errors and client aborts |

These entries are **required and incomplete**, not accepted exclusions or
placeholder C entry points. A C wrapper that calls the corresponding old Lua
implementation does not satisfy a row. Adding empty phase hooks or more source
lines also does not satisfy a row.

The source files `init.lua`, `plugin.lua`, `router.lua`, `http/route.lua`, all
`http/router/*.lua`, `balancer.lua`, all `balancer/*.lua`, `upstream.lua`,
`consumer.lua` and `core/ctx.lua` contain 7,441 lines in total, including comments
and non-HTTP branches. This is an inventory scale reference, not a C LOC target
or an assertion that these files are the complete dependency closure.

## Completion evidence

- Each relevant source responsibility maps to an implemented C symbol and real
  tests, with intentional NGINX reuse and protocol exclusions identified.
- Native startup and requests run with the original HTTP core initialization,
  routing, scheduling, merging, balancing and finalization implementations
  unavailable, not merely with four selected entry points poisoned.
- Plugins are selected by native descriptors/plans, not hardcoded checks for
  key-auth, limit-count and Prometheus. Probe plugins exercise rewrite, access,
  before-proxy, header-filter, multi-buffer body-filter and log ordering,
  error/early exit and coroutine behavior.
- Route matching and configuration inheritance have differential tests against
  the inspected APISIX revision, covering the behaviors above rather than only
  the four demo prefixes.
- Existing key-auth and limit-count remain route-specific; existing Prometheus
  remains global. Plugin business source is unchanged.
- Lifecycle/fault tests and native memory checks pass. Every pressure sample
  remains 60 seconds, per the user's instruction.
- Old experiment measurements remain labeled as partial-experiment evidence;
  new full-core results require a new run of the completed implementation.

Current completion status: **not complete**. No full-core performance or
stability claim is supported by the earlier experiment.
