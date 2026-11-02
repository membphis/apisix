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
"""One 60-second sample per path and implementation, always with global metrics."""
import json
import hashlib
import os
from pathlib import Path
import platform
import re
import subprocess
import time

from prepare import prepare, stop, ROOT
from test_demo import ready, metrics

OUT = Path("/tmp/native-demo-results")


def workers(master=None):
    if master is None:
        master = int((ROOT / "logs/nginx.pid").read_text())
    output = subprocess.check_output(
        ["ps", "--ppid", str(master), "-o", "pid=,args="], text=True)
    return sorted(int(line.split()[0]) for line in output.splitlines()
                  if "nginx: worker process" in line)


def usage(pids):
    cpu, rss = 0, 0
    for pid in pids:
        fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
        cpu += (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")
        rss += int(Path(f"/proc/{pid}/statm").read_text().split()[1]) * os.sysconf("SC_PAGE_SIZE")
    return {"cpu_seconds": cpu, "rss_bytes": rss}


def sample(mode, prefix, cpu):
    prepare(mode, benchmark=True,
            gateway_cpu=cpu[0] if len(cpu) >= 3 else None,
            upstream_cpu=cpu[1] if len(cpu) >= 3 else None)
    ready()
    pids = workers()
    if len(cpu) >= 3:
        for pid in pids:
            assert os.sched_getaffinity(pid) == {cpu[0]}, "gateway affinity was not applied"
        upstream_master = int(Path("/tmp/native-demo-upstream/logs/nginx.pid").read_text())
        for pid in workers(upstream_master):
            assert os.sched_getaffinity(pid) == {cpu[1]}, "upstream affinity was not applied"
    command = ["wrk", "-t2", "-c32", "--latency", "-s", "/demo/bench.lua", "-H", "apikey: demo-key",
               f"http://127.0.0.1:9080/{prefix}/benchmark"]
    subprocess.run(command[:1] + ["-d5s"] + command[1:], check=True,
                   stdout=subprocess.DEVNULL)
    log = ROOT / "logs/error.log"
    log_offset = log.stat().st_size
    start = usage(pids)
    start_wall = time.monotonic()
    def pin_driver():
        if len(cpu) >= 3:
            os.sched_setaffinity(0, set(cpu[2:]))
    process = subprocess.Popen(command[:1] + ["-d60s"] + command[1:],
                               text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               preexec_fn=pin_driver)
    observations = []
    while process.poll() is None:
        if workers() != pids:
            process.kill()
            raise RuntimeError("gateway worker restarted during sample")
        observations.append(usage(pids))
        metrics()  # same scrape cadence for both implementations
        time.sleep(1)
    stdout, stderr = process.communicate()
    (OUT / f"{mode}-{prefix}.txt").write_text(stdout + stderr)
    if process.returncode:
        raise RuntimeError(f"wrk failed: {stdout} {stderr}")
    final = usage(pids)
    errors = log.read_bytes()[log_offset:].decode(errors="replace")
    if "[error]" in errors or "[alert]" in errors:
        raise RuntimeError(f"unexpected gateway error during sample: {errors[:1000]}")
    if re.search(r"Non-2xx or 3xx responses|Socket errors:", stdout):
        raise RuntimeError(f"failed requests in throughput sample: {stdout}")
    count = int(re.search(r"(\d+) requests in", stdout)[1])
    qps = float(re.search(r"Requests/sec:\s+([\d.]+)", stdout)[1])
    result = {"mode": mode, "prefix": prefix, "duration_seconds": 60,
              "wall_seconds": time.monotonic() - start_wall,
              "requests": count, "qps": qps, "workers": pids,
              "worker_cpu_seconds": final["cpu_seconds"] - start["cpu_seconds"],
              "worker_cpu_us_per_request": (final["cpu_seconds"] - start["cpu_seconds"]) * 1e6 / count,
              "rss_start_bytes": start["rss_bytes"], "rss_end_bytes": final["rss_bytes"],
              "rss_peak_bytes": max(x["rss_bytes"] for x in observations),
              "latency_method": "wrk closed-loop; each implementation at its own achieved throughput",
              "latencies_us": {key: float(value) for key, value in
                               re.findall(r"\b(p50|p95|p99)=([\d.]+)", stdout)},
              "error_rate": 0}
    print(json.dumps(result), flush=True)
    return result


if __name__ == "__main__":
    OUT.mkdir(exist_ok=True)
    cpu = sorted(os.sched_getaffinity(0))
    environment = {"kernel": platform.platform(), "available_cpus": cpu,
                   "runtime": subprocess.check_output(["openresty", "-V"], stderr=subprocess.STDOUT, text=True),
                   "rocks": subprocess.check_output(["luarocks", "--tree=/usr/local/apisix/deps", "list", "--porcelain"], text=True),
                   "hashes": {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in [
                       Path("/src/apisix/init.lua"), Path("/src/apisix-master-0.rockspec"),
                       Path("/demo/native_demo.lua"), Path("/opt/native-module/lib/ngx_http_apisix_native_module.so")]}}
    for name in ["cpu.max", "memory.max"]:
        path = Path("/sys/fs/cgroup") / name
        environment[name] = path.read_text().strip() if path.exists() else "unavailable"
    (OUT / "environment.json").write_text(json.dumps(environment, indent=2))
    stop(Path("/tmp/native-demo-upstream"))
    results = []
    for prefix in ["plain", "auth", "limit", "combined"]:
        for mode in ["lua", "native"]:
            results.append(sample(mode, prefix, cpu))
            (OUT / "results.json").write_text(json.dumps(results, indent=2))
    prepare("native")
    ready()
