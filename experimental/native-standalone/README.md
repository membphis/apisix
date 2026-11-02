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

# Native standalone data-plane demo

**Status: partial routing experiment, not a complete APISIX core rewrite.**
The complete standalone HTTP data-plane rewrite requested by the user remains
unimplemented. This directory preserves the earlier reduced experiment and its
measurements. See [COVERAGE.md](COVERAGE.md) for the source-to-native migration gap.

This experiment implements prefix routing, a request-scoped route pointer,
plugin bitmask identifiers and a static upstream name in
`ngx_http_apisix_native_module`. NGINX owns
proxying and connection reuse. A small ngx_lua bridge runs the existing
`key-auth`, `limit-count` and `prometheus` plugins without changing their source.
The addon has a distinct name from the APISIX runtime's existing helper module.

`example.json` is the single startup snapshot. Prometheus is enabled globally,
including unmatched requests. Authentication and local rate limiting are enabled
only on their route prefixes:

| Prefix | Route plugins | Global plugin |
| --- | --- | --- |
| `/plain/*` | none | Prometheus |
| `/auth/*` | key-auth | Prometheus |
| `/limit/*` | limit-count | Prometheus |
| `/combined/*` | key-auth, limit-count | Prometheus |
| no matching prefix | 404 | Prometheus |

The demo consumer uses the public example key `demo-key`; it is not a deployment
credential. The default quota is five requests per minute. Benchmarks use a high
quota so they measure successful proxying rather than rejection throughput.

From the repository root, with Docker running:

```sh
bash experimental/native-standalone/run.sh build
bash experimental/native-standalone/run.sh start native
curl http://127.0.0.1:9080/plain/hello
curl -H 'apikey: demo-key' http://127.0.0.1:9080/combined/hello
curl http://127.0.0.1:9091/apisix/prometheus/metrics
bash experimental/native-standalone/run.sh test
bash experimental/native-standalone/run.sh bench
bash experimental/native-standalone/run.sh stop
```

The build uses a read-only CA secret, including for the session proxy. Set
`NATIVE_DEMO_CA` to a combined trust bundle when the system bundle is elsewhere.
The CA is not copied into an image layer. The runtime image is pinned by digest;
compiler packages use the corresponding dated Debian snapshot. TLS and package
signature verification remain enabled.

`start lua` runs the original APISIX data plane using the same current checkout,
runtime, standalone resources and plugins. `start native` replaces request hooks
and the Lua balancer in the generated NGINX configuration; all shared settings
come from the same APISIX template. Both use startup-frozen resource files.
Changing the business configuration requires restarting the gateway. Stop the
container before rebuilding the image. Ports 9080 and 9091 bind to host loopback.

The native request path does not call APISIX's old Lua router, request plugin
filter or balancer. Full APISIX initialization is reused as the cold compatibility
environment for consumers, plugin schemas, exporter timers and shared dictionaries.
Lua constructs the phase callback arrays, invokes the old Lua plugin runner and
owns much of the request context through `core.ctx`. Route lookup uses the index
and mask produced by C; it does not match
the request again. The C context belongs to the NGINX request pool and refers to
immutable configuration memory; the Lua view is released after the global log
phase. Existing Lua callback invocation and plugin helpers remain in the cost.

The implementation is deliberately bounded to 64 prefix routes, one loopback
upstream and HTTP/1.1. It does not implement reload/watch, etcd/Admin API,
Service/Consumer configuration merging, Redis quotas, DNS, health checks, retries,
TLS or other protocols. Unsupported business fields are rejected before stopping
an existing gateway. Prefixes end with `/`, so `/authentic/` does not match
`/auth/*`; overlapping routes select the longest prefix.

`test` checks both implementations over real HTTP, including global 404/401/429
metrics, quota isolation, two-worker shared quotas and native failure paths.
`bench` alternates implementations for each prefix: five seconds of warmup and
one 60-second sample each. It records worker CPU per request, RSS, errors, QPS,
latencies, environment and raw wrk output under the ignored `results/` directory.
Scrapes run once per second on both paths. On a machine with at least three
available CPUs, gateway, upstream and load generator use separate CPUs.

wrk uses a closed loop. Reported p99 values describe each implementation at its
own achieved throughput; they do not establish an improvement at equal offered
load. Eight one-minute samples are an exploratory comparison, not evidence of
long-term production stability. See `RESULTS.md` for actual verification evidence.
