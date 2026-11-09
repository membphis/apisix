#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Inside the pinned image: same-batch performance of stable Lua, the step-1
paths (C core / Rust core + unchanged Lua key-auth) and the v2 paths (C v2 /
Rust v2, native key-auth).

Each variant runs, in one gateway instance:
1. the unchanged research throughput sample (`benchmark.sample`): route-only
   key-auth on `/auth/*`, 5 s warmup + 60 s `wrk -t2 -c32`, gateway worker on
   CPU0, upstream on CPU1, driver on the remaining CPUs, worker CPU/RSS and
   error-log checks;
2. a 60 s fixed-rate sample at R_low = 80% of the first Lua throughput
   (all variants), with coordinated-omission-corrected latency
   (`tools/fixed_rate.c`) and worker CPU per request at equal load;
3. for the fully native v2 paths only, a 60 s fixed-rate sample at R_high =
   80% of the first C v2 throughput (Lua and step 1 cannot sustain it).

Rounds alternate forward/reverse order. Evidence goes to a new directory;
nothing is overwritten.
"""
import os

os.environ.setdefault("APISIX_CORE_SOURCE", "/stable")
os.environ.setdefault("APISIX_CORE_SOURCE_REVISION", "473043623c161966f3f6ef4e8f12d2d8667a12d2")

import argparse  # noqa: E402
from datetime import datetime, timezone  # noqa: E402
import hashlib  # noqa: E402
import json  # noqa: E402
from pathlib import Path  # noqa: E402
import platform  # noqa: E402
import re  # noqa: E402
import subprocess  # noqa: E402
import sys  # noqa: E402
import time  # noqa: E402

HERE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HERE / "tools"))
import rust_demo  # noqa: E402
import benchmark  # noqa: E402  (unchanged research harness)

rust_demo.install()
VARIANTS = {
    "lua-core": ("lua", ""),
    # Step 1 in C: the existing C core runs the unchanged Lua key-auth through
    # its Lua ABI/coroutine adapter (PR #1 "native-lua", same six switches).
    "native-c-lua-key-auth": ("native", rust_demo.C_BASE),
    # Step 1 in Rust: the Rust core runs the unchanged Lua key-auth through the
    # same engine protocol and runtime.lua, prepared from the C step-1 config.
    "native-rust-lua-key-auth": ("rust-lua", ""),
    "native-c-pipeline": ("native", rust_demo.C_PIPELINE),
    "native-rust-pipeline": ("rust", rust_demo.C_PIPELINE),
}
DEFAULT_ORDER = ["lua-core", "native-c-lua-key-auth", "native-rust-lua-key-auth",
                 "native-c-pipeline", "native-rust-pipeline"]
# Only fully native paths sustain R_high (80% of C v2 throughput).
HIGH_RATE_VARIANTS = {"native-c-pipeline", "native-rust-pipeline"}
GENERATOR = "/opt/fixed_rate"
C_BUILD = Path("/opt/apisix-core-module/lib/build.json")
RUST_BUILD = Path("/opt/apisix-rs-module/lib/build.json")


def log(message):
    print(datetime.now(timezone.utc).strftime("%H:%M:%S"), message, flush=True)


def fixed_rate(label, rate, seconds, out, cpu):
    pids = benchmark.workers()
    error_log = benchmark.ROOT / "logs/error.log"
    offset = error_log.stat().st_size
    start = benchmark.usage(pids)
    started = time.monotonic()
    result = subprocess.run(
        [GENERATOR, "127.0.0.1", "9080", "/auth/benchmark", str(rate), str(seconds), "32", "2", "apikey: demo-key"],
        text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        preexec_fn=lambda: os.sched_setaffinity(0, set(cpu[2:])))
    wall = time.monotonic() - started
    end = benchmark.usage(pids)
    (out / f"{label}.txt").write_text(result.stdout)
    errors = error_log.read_bytes()[offset:].decode(errors="replace")
    (out / f"{label}-error.log").write_text(errors)
    if result.returncode or "[error]" in errors or "[alert]" in errors:
        raise RuntimeError(f"fixed-rate sample failed: {result.stdout} {errors[:1000]}")
    if benchmark.workers() != pids:
        raise RuntimeError("gateway worker restarted during fixed-rate sample")
    values = {k: float(v) for k, v in re.findall(r"(\w+)=([\d.]+)", result.stdout)}
    return {
        "label": label, "rate": rate, "duration_seconds": seconds, "wall_seconds": wall,
        "achieved_rps": values["achieved"], "requests": int(values["requests"]),
        "late_sends": int(values["late_sends"]), "reconnects": int(values["reconnects"]),
        "latency_us": {k: values[k + "_us"] for k in ("mean", "p50", "p90", "p99", "p999", "max")},
        "worker_cpu_seconds": end["cpu_seconds"] - start["cpu_seconds"],
        "worker_cpu_us_per_request": (end["cpu_seconds"] - start["cpu_seconds"]) * 1e6 / values["requests"],
        "worker_cpu_utilization": (end["cpu_seconds"] - start["cpu_seconds"]) / wall,
        "rss_end_bytes": end["rss_bytes"],
        "latency_method": "open-loop fixed rate, latency from intended send time (coordinated-omission corrected)",
    }


def environment(out):
    def sha(path):
        return hashlib.sha256(Path(path).read_bytes()).hexdigest()
    rust_build = json.loads(RUST_BUILD.read_text())
    assert rust_build["features"] == [], "measure only the release Rust build"
    symbols = subprocess.check_output(["nm", "-D", benchmark.MODULE], text=True)
    assert "__asan" not in symbols and "__ubsan" not in symbols, "measure only the optimized C build"
    info = {
        "utc": datetime.now(timezone.utc).isoformat(),
        "kernel": platform.platform(),
        "available_cpus": sorted(os.sched_getaffinity(0)),
        "cpu_model": next((l.split(":", 1)[1].strip() for l in Path("/proc/cpuinfo").read_text().splitlines()
                           if l.startswith("model name")), "unknown"),
        "runtime": subprocess.check_output(["openresty", "-V"], stderr=subprocess.STDOUT, text=True),
        "source_root": str(benchmark.SOURCE),
        "source_revision_declared": os.environ["APISIX_CORE_SOURCE_REVISION"],
        "source_hashes": benchmark.source_hashes(),
        "c_build": json.loads(C_BUILD.read_text()),
        "rust_build": rust_build,
        "c_module_sha256": sha(benchmark.MODULE),
        "rust_module_sha256": sha(rust_demo.RUST_MODULE),
        "fixed_rate_source_sha256": sha(HERE / "tools/fixed_rate.c"),
        "fixed_rate_binary_sha256": sha(GENERATOR),
        "wrk": subprocess.run(["wrk", "--version"], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT).stdout.strip(),
        "host_cpu_stat_source": "/sys/fs/cgroup/cpu/cpu.stat (cgroup v1) mounted at /host-cpu.stat",
        "notes": "4 vCPU cloud VM: gateway worker CPU0, upstream CPU1, drivers CPU2-3 (the v2 report used CPU2-4).",
    }
    (out / "environment.json").write_text(json.dumps(info, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--rounds", type=int, default=2)
    parser.add_argument("--first-round", type=int, default=1)
    parser.add_argument("--fixed-seconds", type=int, default=60)
    parser.add_argument("--rates", help="reuse R_low,R_high from an earlier run instead of deriving them")
    parser.add_argument("--variants", nargs="+", choices=list(VARIANTS), default=DEFAULT_ORDER,
                        help="variants in forward order; rounds alternate forward/reverse")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    environment(args.out)
    cpu = sorted(os.sched_getaffinity(0))
    assert len(cpu) >= 3, cpu
    rates = [int(x) for x in args.rates.split(",")] if args.rates else None
    results = []
    benchmark.stop(benchmark.UPSTREAM)
    try:
        for round_id in range(args.first_round, args.first_round + args.rounds):
            order = args.variants if round_id % 2 == 1 else list(reversed(args.variants))
            for variant in order:
                mode, directives = VARIANTS[variant]
                os.environ["APISIX_CORE_DIRECTIVES"] = directives
                label = f"round-{round_id}-{variant}"
                out = args.out / label
                out.mkdir()
                benchmark.OUT = out
                log(f"{label}: throughput")
                sample = benchmark.sample(mode, "auth", cpu, "single", label)
                sample["variant_name"] = variant
                sample["round"] = round_id
                if rates is None:
                    if variant == "lua-core":
                        lua_rate = int(sample["qps"] * 0.8 // 100 * 100)
                        rates = [lua_rate, 0]
                    else:
                        raise RuntimeError("the first round must start with lua-core to derive R_low")
                if rates[1] == 0 and variant == "native-c-pipeline":
                    rates[1] = int(sample["qps"] * 0.8 // 100 * 100)
                fixed = []
                log(f"{label}: fixed rate {rates[0]}")
                fixed.append(fixed_rate("fixed-low", rates[0], args.fixed_seconds, out, cpu))
                if variant in HIGH_RATE_VARIANTS:
                    if rates[1] == 0:
                        raise RuntimeError("C must run before Rust in the first round to derive R_high")
                    log(f"{label}: fixed rate {rates[1]}")
                    fixed.append(fixed_rate("fixed-high", rates[1], args.fixed_seconds, out, cpu))
                sample["fixed_rate"] = fixed
                results.append(sample)
                (args.out / "results.json").write_text(json.dumps({"rates": rates, "samples": results}, indent=2) + "\n")
                log(f"{label}: qps={sample['qps']:.0f} " + " ".join(
                    f"{f['label']}:p99={f['latency_us']['p99']:.0f}us,cpu={f['worker_cpu_us_per_request']:.2f}us/req"
                    for f in fixed))
    finally:
        benchmark.stop()
        benchmark.stop(benchmark.UPSTREAM)


if __name__ == "__main__":
    main()
