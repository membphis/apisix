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

# Partial routing experiment: measured results

**Scope correction:** these measurements are for the reduced prefix-routing
experiment. They do not validate a complete rewrite of the APISIX HTTP core.
The earlier description overstated the C implementation's ownership of phases,
plugin scheduling and context management. The complete rewrite remains unfinished;
see [COVERAGE.md](COVERAGE.md).

Date: 2026-10-30 (UTC). Source baseline: `b31e828c6f6b588ae954bf69c8432c08fb73bb7e`
from `membphis/apisix`. All implementation changes are under this experimental
directory; the original APISIX sources and target Lua plugin files are unchanged.

This is a reduced standalone prototype: one static
upstream, four prefix routes, global Prometheus, route-specific authentication
and local quotas. C owns a linear prefix matcher, a request pointer, a plugin
bitmask and a static upstream name. Lua still builds phase callback arrays and
calls `apisix.plugin.run_plugin`; core context/consumer
helpers and the cold APISIX initialization remain in use.

## Functional and memory checks

`bash experimental/native-standalone/run.sh test` passed on the optimized image.
The same suite passed on an image built with `NATIVE_SANITIZE=undefined`.
The instrumented module imports the UBSan out-of-bounds, pointer-overflow,
nonnull and type-mismatch abort handlers. No C sanitizer diagnostic was observed.
The deliberately injected Lua exception is an expected HTTP 500 test.

- Each implementation passed 32 real HTTP requests and exact global status
  increments: 19x200, 3x404, 6x401, 2x429, 1x503 and 1x504.
- Verified longest-prefix matching, prefix boundaries, normalized URI routing,
  path/query forwarding, POST body, HEAD, header/query authentication and hidden
  credentials. Authentication failures and quota rejections did not reach upstream.
- Verified quota reset, consumer isolation and exactly five accepted requests out
  of 32 concurrent requests with two gateway workers, on both implementations.
- Verified global metrics include unmatched 404 and early 401/429 responses,
  consumer/route labels and HTTP latency/bandwidth output.
- Verified native upstream failure, timeout, client disconnect and Lua failure.
  Native requests still worked when the original Lua access entry point, router
  match, plugin filter and balancer entry point were replaced by functions that
  throw errors. This only proves those specific entry points are bypassed. It
  does not prove that their full behavior was rewritten or that the other Lua
  core modules have been replaced.
- `python3 -m py_compile`, `bash -n` and `git diff --check` passed.

The full repository `make test` was attempted on the host and could not run:
OpenResty is absent there, so the `runtime` prerequisite failed (exit 2 from
make). The full APISIX Test::Nginx suite is **not** claimed to have passed.
The Docker demo suite above is the runnable local regression for this prototype.

## Measurement method

One single-worker gateway sample per implementation and route prefix, each
**60 seconds**, with a five-second warmup. Order: Lua then native for each prefix.
No extra 30-minute soak or automatic multi-round matrix was run.

Both paths use the same optimized runtime image, APISIX checkout, Lua plugins,
startup snapshot, logs, proxy settings and connection reuse. Prometheus is global
in every sample, and its exporter is scraped once per second. The benchmark
sets the quota to 1,000,000,000 per 600 seconds and sends a valid example key so
all throughput requests are successful upstream requests, not quota/auth rejections.
The interactive demo is restored to its five-request example quota afterwards.

- OpenResty 1.27.1.2 / APISIX runtime 1.3.2, from the digest-pinned image in the
  Dockerfile. Necessary current-source Lua dependencies are overlaid identically
  on both paths; the runtime limit-traffic library is the build-tools-pinned
  `api7/lua-resty-limit-traffic` tag `v1.2.0` (consumed-count API).
- New C module: GCC 10, `-O2`, without sanitizer instrumentation for measurements.
- Five visible CPUs: gateway worker on CPU 0, upstream worker on CPU 1, wrk on
  CPUs 2-4. Affinity was applied through NGINX config and verified before sampling.
- wrk: 2 threads, 32 connections, HTTP/1.1 keepalive, three-byte `ok` body.
- Each sample monitors worker identity and rejects wrk socket/HTTP errors or new
  NGINX error/alert logs. Gateway worker CPU consumption was approximately 60 CPU
  seconds per one-minute sample, consistent with a saturated gateway core.
- CPU/request measures gateway worker CPU, including NGINX and Lua; it excludes
  the upstream and load generator. RSS is the gateway worker RSS.

Kernel: `Linux-6.18.57-x86_64-with-glibc2.31`. Reported container `cpu.max`: `max 100000`; `memory.max`: `max`. These are shared-cloud observations, not a dedicated-hardware specification.

## Throughput and CPU cost

| Scenario | Lua QPS | Native QPS | QPS change | Lua CPU us/req | Native CPU us/req | CPU/req change |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Global Prometheus | 18,937.91 | 27,194.82 | +43.6% | 52.74 | 36.68 | -30.5% |
| Global Prometheus + key-auth | 16,711.02 | 25,103.76 | +50.2% | 59.80 | 39.75 | -33.5% |
| Global Prometheus + local limit-count | 14,364.95 | 19,493.23 | +35.7% | 69.56 | 51.27 | -26.3% |
| Global Prometheus + both route plugins | 13,565.48 | 18,965.56 | +39.8% | 73.63 | 52.63 | -28.5% |

All eight samples completed with zero observed request errors and no worker
replacement during measurement. In this run the combined scenario increased
throughput by about 39.8% and reduced gateway CPU/request by about 28.5%.
These are observations from one sample per case, not statistical confidence bounds.

## Latency and resident memory

wrk is a closed-loop load generator. These percentiles are at each implementation's
own achieved throughput; they do **not** establish a p99 improvement at equal
fixed offered load. No fixed-arrival-rate latency experiment was run.

| Prefix | Mode | p50 ms | p95 ms | p99 ms | RSS start MiB | RSS end MiB | RSS peak MiB |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| plain | lua | 1.544 | 2.678 | 3.533 | 32.66 | 33.29 | 33.31 |
| plain | native | 1.066 | 1.978 | 2.638 | 32.14 | 32.43 | 32.55 |
| auth | lua | 1.738 | 3.125 | 4.311 | 33.23 | 34.18 | 34.27 |
| auth | native | 1.136 | 2.238 | 3.106 | 31.59 | 32.20 | 32.24 |
| limit | lua | 1.996 | 3.977 | 5.239 | 33.64 | 34.38 | 34.48 |
| limit | native | 1.447 | 3.163 | 4.230 | 32.12 | 32.57 | 32.75 |
| combined | lua | 2.121 | 4.227 | 5.684 | 32.92 | 33.87 | 33.89 |
| combined | native | 1.474 | 3.303 | 4.495 | 33.04 | 33.77 | 33.77 |

RSS includes LuaJIT and caches; the small increases within a minute are not a
long-term leak measurement. No worker crash or C sanitizer failure was observed
in the executed checks. One-minute samples do not establish greater production
stability or cover long-term resource exhaustion.

## Interpretation and boundaries

The lower worker CPU/request is consistent with removing Lua routing/dispatch
preparation and Lua balancing from this workload. No CPU flame graph was collected,
so no function-level percentage is attributed to an individual change. The new
module uses a bounded linear scan over at most 64 prefixes; the experiment measured
four routes and makes no large-route-count claim.

The compatibility target is the documented three-plugin subset. It does not cover
Service/Consumer override merging, other plugins/protocols, hot updates, Redis,
DNS or health checks. The minimal bridge still enters ngx_lua and retains no-op
header/body filter hooks from the shared template. The baseline sets an APISIX
Server header through its Lua header filter; native retains the runtime Server
header. Functional comparisons cover business status/body/headers and metrics,
not byte-identical branding headers.

## Reproduction and evidence

From the repository root:

```sh
bash experimental/native-standalone/run.sh build
bash experimental/native-standalone/run.sh start native
bash experimental/native-standalone/run.sh test
bash experimental/native-standalone/run.sh bench
```

For an instrumented build, set `NATIVE_DEMO_SANITIZE=undefined` and use a distinct
`NATIVE_DEMO_IMAGE` / container name. The actual validation used a separate UBSan
container without publishing ports, executed `/demo/test_demo.py`, then removed
that container before measuring the optimized image.

Raw local artifacts (ignored by Git):

- [results/results.json](results/results.json): eight exact measurement records.
- [results/environment.json](results/environment.json): runtime build arguments,
  installed dependency versions, CPU/cgroup observations and source/module hashes.
- `results/{lua,native}-{plain,auth,limit,combined}.txt`: raw wrk output, including
  request totals and percentile output from `bench.lua`.

The native gateway remains available within the execution environment on loopback
port 9080; global metrics are on port 9091 at `/apisix/prometheus/metrics`.
