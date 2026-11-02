#!/usr/bin/env python3
# Licensed to the Apache Software Foundation (ASF) under one or more
# contributor license agreements. See the NOTICE file distributed with
# this work for additional information regarding copyright ownership.
# The ASF licenses this file to You under the Apache License, Version 2.0
# (the "License"); you may not use this file except in compliance with
# the License. You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Paired 60-second samples: isolated route plugins or the historical global suite."""
import argparse
import json
import hashlib
import os
from pathlib import Path
import platform
import re
import subprocess
import time

import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tests"))
from demo import prepare, stop, ROOT, HERE, SOURCE, UPSTREAM, MODULE
from test_demo import ready, metrics

OUT = Path("/tmp/apisix-http-core-results")
SINGLE_PLUGINS = {"metrics": "prometheus", "auth": "key-auth", "limit": "limit-count"}


def cgroup_cpu(path=Path("/sys/fs/cgroup/cpu.stat")):
    if not path.exists():
        return {}
    return {key: int(value) for key, value in
            (line.split() for line in path.read_text().splitlines())}


def source_hashes():
    paths = [SOURCE / "apisix/init.lua", SOURCE / "apisix-master-0.rockspec"]
    # Include helpers, not just top-level plugin files: release and branch differ.
    for plugin in ("key-auth", "limit-count", "prometheus"):
        paths.append(SOURCE / f"apisix/plugins/{plugin}.lua")
        directory = SOURCE / "apisix/plugins" / plugin
        if directory.is_dir():
            paths.extend(sorted(directory.rglob("*.lua")))
    return {str(path.relative_to(SOURCE)): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in paths}



def workers(master=None):
    if master is None:
        master = int((ROOT / "logs/nginx.pid").read_text())
    output = subprocess.check_output(
        ["ps", "--ppid", str(master), "-o", "pid=,args="], text=True)
    return sorted(int(line.split()[0]) for line in output.splitlines()
                  if "nginx: worker process" in line)


def usage(pids):
    user_cpu, system_cpu, rss = 0, 0, 0
    for pid in pids:
        fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
        user_cpu += int(fields[11]) / os.sysconf("SC_CLK_TCK")
        system_cpu += int(fields[12]) / os.sysconf("SC_CLK_TCK")
        rss += int(Path(f"/proc/{pid}/statm").read_text().split()[1]) * os.sysconf("SC_PAGE_SIZE")
    return {"cpu_seconds": user_cpu + system_cpu, "user_cpu_seconds": user_cpu,
            "system_cpu_seconds": system_cpu, "rss_bytes": rss}


def sample(mode, prefix, cpu, suite="legacy", variant="default"):
    prepare(mode, benchmark=True,
            gateway_cpu=cpu[0] if len(cpu) >= 3 else None,
            upstream_cpu=cpu[1] if len(cpu) >= 3 else None,
            single_plugin=SINGLE_PLUGINS[prefix] if suite == "single" else None)
    ready("/missing", expected=404) if suite == "single" else ready()
    for source, suffix in (("nginx.conf", "nginx.conf"), ("apisix.json", "resources.json"),
                           ("config.yaml", "config.yaml")):
        (OUT / f"{mode}-{prefix}-{suffix}").write_text((ROOT / "conf" / source).read_text())
    pids = workers()
    if len(cpu) >= 3:
        for pid in pids:
            assert os.sched_getaffinity(pid) == {cpu[0]}, "gateway affinity was not applied"
        upstream_master = int((UPSTREAM / "logs/nginx.pid").read_text())
        for pid in workers(upstream_master):
            assert os.sched_getaffinity(pid) == {cpu[1]}, "upstream affinity was not applied"
    def pin_driver():
        if len(cpu) >= 3:
            os.sched_setaffinity(0, set(cpu[2:]))
    command = ["wrk", "-t2", "-c32", "--latency", "-s", str(HERE / "tools/bench.lua"), "-H", "apikey: demo-key",
               f"http://127.0.0.1:9080/{prefix}/benchmark"]
    subprocess.run(command[:1] + ["-d5s"] + command[1:], check=True,
                   stdout=subprocess.DEVNULL, preexec_fn=pin_driver)
    log = ROOT / "logs/error.log"
    log_offset = log.stat().st_size
    start = usage(pids)
    start_wall = time.monotonic()
    cgroup_start = cgroup_cpu()
    host_cpu_start = cgroup_cpu(Path("/host-cpu.stat"))
    process = subprocess.Popen(command[:1] + ["-d60s"] + command[1:],
                               text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               preexec_fn=pin_driver)
    observations = []
    while process.poll() is None:
        if workers() != pids:
            process.kill()
            raise RuntimeError("gateway worker restarted during sample")
        observations.append(usage(pids))
        if suite == "legacy" or prefix == "metrics":
            metrics()  # single-plugin auth/limit do not enable Prometheus
        time.sleep(1)
    stdout, stderr = process.communicate()
    (OUT / f"{mode}-{prefix}.txt").write_text(stdout + stderr)
    if process.returncode:
        raise RuntimeError(f"wrk failed: {stdout} {stderr}")
    final = usage(pids)
    cgroup_end = cgroup_cpu()
    host_cpu_end = cgroup_cpu(Path("/host-cpu.stat"))
    errors = log.read_bytes()[log_offset:].decode(errors="replace")
    (OUT / f"{mode}-{prefix}-error.log").write_text(errors)
    if "[error]" in errors or "[alert]" in errors:
        raise RuntimeError(f"unexpected gateway error during sample: {errors[:1000]}")
    if re.search(r"Non-2xx or 3xx responses|Socket errors:", stdout):
        raise RuntimeError(f"failed requests in throughput sample: {stdout}")
    count = int(re.search(r"(\d+) requests in", stdout)[1])
    qps = float(re.search(r"Requests/sec:\s+([\d.]+)", stdout)[1])
    result = {"mode": mode, "prefix": prefix, "duration_seconds": 60,
              "suite": suite, "variant": variant,
              "plugin": SINGLE_PLUGINS.get(prefix) if suite == "single" else None,
              "cgroup_cpu_delta": {key: value - cgroup_start[key]
                                   for key, value in cgroup_end.items() if key in cgroup_start},
              "host_cgroup_cpu_delta": {key: value - host_cpu_start[key]
                                        for key, value in host_cpu_end.items() if key in host_cpu_start},
              "wall_seconds": time.monotonic() - start_wall,
              "requests": count, "qps": qps, "workers": pids,
              "worker_cpu_seconds": final["cpu_seconds"] - start["cpu_seconds"],
              "worker_user_cpu_seconds": final["user_cpu_seconds"] - start["user_cpu_seconds"],
              "worker_system_cpu_seconds": final["system_cpu_seconds"] - start["system_cpu_seconds"],
              "worker_cpu_us_per_request": (final["cpu_seconds"] - start["cpu_seconds"]) * 1e6 / count,
              "rss_start_bytes": start["rss_bytes"], "rss_end_bytes": final["rss_bytes"],
              "rss_peak_bytes": max([start["rss_bytes"], final["rss_bytes"]] + [x["rss_bytes"] for x in observations]),
              "latency_method": "wrk closed-loop; each implementation at its own achieved throughput",
              "latencies_us": {key: float(value) for key, value in
                               re.findall(r"\b(p50|p95|p99)=([\d.]+)", stdout)},
              "error_rate": 0}
    print(json.dumps(result), flush=True)
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", choices=["single", "legacy"], default="single")
    parser.add_argument("--prefixes", nargs="+")
    parser.add_argument("--modes", nargs="+", choices=["lua", "native"], default=["lua", "native"])
    parser.add_argument("--variant", default="default")
    parser.add_argument("--out", type=Path, required=True,
                        help="new evidence directory; existing results are never overwritten")
    args = parser.parse_args()
    OUT = args.out
    OUT.mkdir(parents=True, exist_ok=False)
    prefixes = args.prefixes or (["metrics", "auth", "limit"] if args.suite == "single"
                                else ["plain", "auth", "limit", "combined"])
    allowed = SINGLE_PLUGINS if args.suite == "single" else ["plain", "auth", "limit", "combined"]
    assert all(prefix in allowed for prefix in prefixes), "prefix does not belong to selected suite"
    symbols = subprocess.check_output(["nm", "-D", MODULE], text=True)
    assert not any(marker in symbols for marker in ("__asan", "__ubsan", "ac_function_profile_start", "ac_route_profile_start")), \
        "performance requires the optimized module"
    cpu = sorted(os.sched_getaffinity(0))
    environment = {"kernel": platform.platform(), "available_cpus": cpu,
                   "runtime": subprocess.check_output(["openresty", "-V"], stderr=subprocess.STDOUT, text=True),
                   "rocks": subprocess.check_output(["luarocks", "--tree=/usr/local/apisix/deps", "list", "--porcelain"], text=True),
                   "source_root": str(SOURCE),
                   "source_revision_declared": os.environ.get("APISIX_CORE_SOURCE_REVISION", "branch-worktree"),
                   "source_hashes": source_hashes(),
                   "suite": args.suite, "variant": args.variant,
                   "native_directives": os.environ.get("APISIX_CORE_DIRECTIVES", ""),
                   "disabled_default_plugin": "ai",
                   "scrape": "1Hz only in prometheus scenario" if args.suite == "single" else "1Hz all scenarios",
                   "module_kind": "ngx_http_apisix_core_module (new native-http-core)",
                   "sample_seconds": 60,
                   "hashes": {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in [
                       SOURCE / "apisix/init.lua", SOURCE / "apisix-master-0.rockspec",
                       HERE / "runtime.lua", Path(MODULE),
                       SOURCE / "apisix/plugins/key-auth.lua", SOURCE / "apisix/plugins/limit-count.lua",
                       SOURCE / "apisix/plugins/prometheus.lua"]}}
    for name in ["cpu.max", "memory.max"]:
        path = Path("/sys/fs/cgroup") / name
        environment[name] = path.read_text().strip() if path.exists() else "unavailable"
    host_quota = Path("/host-cpu.max")
    environment["host_cpu.max"] = host_quota.read_text().strip() if host_quota.exists() else "unavailable"
    environment["native_sources_sha256"] = {
        str(path.relative_to(HERE)): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in sorted((HERE / "module").iterdir()) if path.is_file()}
    (OUT / "environment.json").write_text(json.dumps(environment, indent=2))
    build = Path(MODULE).parent / "build.json"
    if build.exists():
        metadata = json.loads(build.read_text())
        assert metadata["module_sha256"] == hashlib.sha256(Path(MODULE).read_bytes()).hexdigest()
        if "source_hashes" in metadata:
            assert metadata["source_hashes"] == {
                str(Path(name).name): digest
                for name, digest in environment["native_sources_sha256"].items()
            }, "module source changed since the recorded build"
        (OUT / "build.json").write_text(build.read_text())
    stop(UPSTREAM)
    results = []
    for index, prefix in enumerate(prefixes):
        modes = args.modes if index % 2 == 0 else list(reversed(args.modes))
        for mode in modes:
            results.append(sample(mode, prefix, cpu, args.suite, args.variant))
            (OUT / "results.json").write_text(json.dumps(results, indent=2))
    prepare("native")
    ready()
