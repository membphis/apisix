#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Development CPU sampling; this does not produce acceptance throughput data."""
import argparse
from bisect import bisect_right
from collections import Counter
import os
from pathlib import Path
import subprocess

from demo import NGINX, NGINX_ENV, ROOT, UPSTREAM, HERE, MODULE, prepare, stop


def function_summary(output):
    symbols = {}
    text = subprocess.check_output(["nm", "-an", "--defined-only", MODULE], text=True)
    for row in text.splitlines():
        parts = row.split()
        if len(parts) >= 3 and parts[1] in ("t", "T"):
            symbols[int(parts[0], 16)] = parts[2]
    rows = []
    for row in Path(output).read_text().splitlines():
        address, count, inclusive, own = row.split()
        rows.append((symbols.get(int(address, 16), address), int(count), int(inclusive), int(own)))
    total = sum(row[3] for row in rows)
    print("Instrumented C subtrees only, including instrumentation tax; not throughput evidence")
    print("self%  count      inclusive_ms     self_ms   function")
    for name, count, inclusive, own in sorted(rows, key=lambda row: row[3], reverse=True)[:35]:
        print(f"{own / total * 100:5.1f} {count:9d} {inclusive / 1e6:15.1f} {own / 1e6:11.1f} {name}")


def symbolize(output):
    """Resolve sampled PCs by ELF mapping and nearest defined text symbol."""
    mappings, symbols = [], {}
    for line in Path(output + ".maps").read_text().splitlines():
        fields = line.split(maxsplit=5)
        if "x" not in fields[1]:
            continue
        start, end = (int(value, 16) for value in fields[0].split("-"))
        path = fields[5] if len(fields) > 5 else "[JIT/anonymous executable]"
        mappings.append((start, end, start - int(fields[2], 16), path))
        if path.startswith("/") and path not in symbols:
            result = subprocess.run(["nm", "-an", "--defined-only", path], text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
            if not result.stdout.strip():
                result = subprocess.run(["nm", "-Dan", "--defined-only", path], text=True,
                                        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
            entries = []
            for row in result.stdout.splitlines():
                parts = row.split()
                if len(parts) >= 3 and parts[1] in ("t", "T", "w", "W"):
                    entries.append((int(parts[0], 16), parts[2]))
            symbols[path] = sorted(entries)
    counts = Counter()
    for row in Path(output).read_text().splitlines():
        address = int(row, 16)
        label = "[unknown]"
        for start, end, base, path in mappings:
            if not start <= address < end:
                continue
            entries = symbols.get(path, [])
            index = bisect_right(entries, (address - base, chr(0x10ffff))) - 1
            label = path if path.startswith("[") else Path(path).name
            if index >= 0:
                label += ":" + entries[index][1]
            break
        counts[label] += 1
    total = sum(counts.values())
    assert total > 0, "No CPU samples collected"
    print(f"{total} process-CPU PC samples; nearest ELF symbols, no call stacks")
    for name, count in counts.most_common(35):
        print(f"{count / total * 100:6.2f}% {count:6d} {name}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=["lua", "native"])
    parser.add_argument("--prefix", choices=["plain", "combined"], default="plain")
    parser.add_argument("--sampler", choices=["jit", "pc", "functions"], default="jit")
    args = parser.parse_args()
    if args.sampler == "functions":
        assert args.mode == "native"
        context = os.environ.copy(); context["APISIX_CORE_FUNCTION_PROFILE"] = "1"
        subprocess.run(["python3", str(HERE / "tools/rebuild.py")], env=context, check=True)
    cpu = sorted(os.sched_getaffinity(0))
    stop(UPSTREAM)
    prepare(args.mode, benchmark=True, gateway_cpu=cpu[0], upstream_cpu=cpu[1])
    stop()
    output = f"/tmp/apisix-core-profile-{args.mode}-{args.prefix}-{args.sampler}.txt"
    # The worker writes the profile, so an earlier worker's file must be removed.
    Path(output).unlink(missing_ok=True)
    Path(output + ".maps").unlink(missing_ok=True)
    path = ROOT / "conf/nginx.conf"
    text = path.read_text()
    marker = ('require("apisix.native.engine").init_worker()' if args.mode == "native"
              else "apisix.http_init_worker()")
    assert text.count(marker) == 1
    if args.sampler == "jit":
        profile = '''
        if require("ngx.process").type() == "worker" then
            local profiler = require("jit.p")
            ngx.timer.at(2, function() profiler.start("vfi1", "__OUTPUT__") end)
            ngx.timer.at(12, function() profiler.stop() end)
        end
'''
    elif args.sampler == "pc":
        subprocess.run(["gcc", "-shared", "-fPIC", "-O2", "-o", "/tmp/apisix-cpu-sampler.so",
                        str(HERE / "tools/cpu_sampler.c")], check=True)
        profile = '''
        if require("ngx.process").type() == "worker" then
            local ffi = require("ffi")
            ffi.cdef[[int ac_profile_start(void); int ac_profile_stop(const char *);]]
            local sampler = ffi.load("/tmp/apisix-cpu-sampler.so")
            ngx.timer.at(2, function() assert(sampler.ac_profile_start() == 0) end)
            ngx.timer.at(12, function()
                assert(sampler.ac_profile_stop("__OUTPUT__") > 0)
                local maps = assert(io.open("/proc/self/maps", "r"))
                local out = assert(io.open("__OUTPUT__.maps", "w"))
                out:write(maps:read("*a")); out:close(); maps:close()
            end)
        end
'''
    else:
        profile = '''
        if require("ngx.process").type() == "worker" then
            local ffi = require("ffi")
            ffi.cdef[[int ac_function_profile_start(void); int ac_function_profile_stop(const char *);]]
            ngx.timer.at(2, function() assert(ffi.C.ac_function_profile_start() == 0) end)
            ngx.timer.at(12, function() assert(ffi.C.ac_function_profile_stop("__OUTPUT__") == 0) end)
        end
'''
    profile = profile.replace("__OUTPUT__", output)
    path.write_text(text.replace(marker, marker + profile))
    subprocess.run([NGINX, "-p", str(ROOT), "-c", "conf/nginx.conf"], env=NGINX_ENV, check=True)
    try:
        def pin():
            os.sched_setaffinity(0, set(cpu[2:]))
        subprocess.run(["wrk", "-t2", "-c32", "-d15s", "-H", "apikey: demo-key",
                        f"http://127.0.0.1:9080/{args.prefix}/profile"],
                       preexec_fn=pin, stdout=subprocess.DEVNULL, check=True)
        if args.sampler == "pc":
            symbolize(output)
        elif args.sampler == "functions":
            function_summary(output)
        else:
            print(Path(output).read_text())
    finally:
        stop()


if __name__ == "__main__":
    main()
