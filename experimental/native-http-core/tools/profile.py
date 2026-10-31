#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Development CPU sampling; this does not produce acceptance throughput data."""
import argparse
from bisect import bisect_right
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import subprocess

from demo import NGINX, NGINX_ENV, ROOT, UPSTREAM, HERE, MODULE, SOURCE, prepare, stop


# These callbacks deliberately aggregate rather than write an event stream. Hook
# dispatch still changes execution cost: neither exit counts nor GC snapshots
# are a CPU cost attribution or an allocation-rate measurement.
TRACE_PROFILE = r'''
        if require("ngx.process").type() == "worker" then
            local util = require("jit.util")
            local vmdef = require("jit.vmdef")
            local cjson = require("cjson.safe")
            local traces, exits, aborts, heap = {}, {}, {}, {}
            local totals = {start = 0, stop = 0, abort = 0, exit = 0,
                            dropped_exit_keys = 0, dropped_abort_keys = 0}
            local exit_keys, abort_keys = 0, 0
            local active = false
            local function trace_event(what, tr, func, pc, err, info)
                if what == "start" and tr <= 512 then
                    local fi = util.funcinfo(func, pc)
                    traces[tostring(tr)] = fi and (fi.loc or fi.source) or "unknown"
                end
                if not active then return end
                if totals[what] ~= nil then totals[what] = totals[what] + 1 end
                if what == "abort" then
                    local reason = vmdef.traceerr[err] or tostring(err)
                    local fi = util.funcinfo(func, pc)
                    local key = (fi and (fi.loc or fi.source) or "unknown") .. ": " .. reason
                    if aborts[key] then aborts[key] = aborts[key] + 1
                    elseif abort_keys < 128 then
                        aborts[key] = 1; abort_keys = abort_keys + 1
                    else totals.dropped_abort_keys = totals.dropped_abort_keys + 1 end
                end
            end
            local function trace_exit(tr, ex)
                totals.exit = totals.exit + 1
                local key = tr * 65536 + ex
                if exits[key] then exits[key] = exits[key] + 1
                elseif exit_keys < 512 then
                    exits[key] = 1; exit_keys = exit_keys + 1
                else totals.dropped_exit_keys = totals.dropped_exit_keys + 1 end
            end
            jit.off(trace_event, true)
            jit.off(trace_exit, true)
            -- Capture source locations of traces compiled during warmup too.
            jit.attach(trace_event, "trace")
            ngx.timer.at(2, function()
                active = true
                jit.attach(trace_exit, "texit")
            end)
            for second = 2, 12 do
                ngx.timer.at(second, function()
                    heap[#heap + 1] = {second = second, lua_heap_kib = collectgarbage("count")}
                end)
            end
            ngx.timer.at(12, function()
                jit.attach(trace_event)
                jit.attach(trace_exit)
                local rows = {}
                for key, count in pairs(exits) do
                    rows[#rows + 1] = {trace = math.floor(key / 65536),
                                      exit = key % 65536, count = count}
                end
                table.sort(rows, function(a, b) return a.count > b.count end)
                local out = assert(io.open("__OUTPUT__", "w"))
                out:write(assert(cjson.encode({totals = totals, aborts = aborts,
                    exits = rows, trace_sources = traces, heap_snapshots = heap,
                    note = "10-second diagnostic hooks; overhead changes execution. " ..
                           "Exit counts are not CPU percentages; heap snapshots are not allocated bytes."})))
                out:close()
            end)
        end
'''


def function_summary(output, module=MODULE):
    symbols = {}
    text = subprocess.check_output(["nm", "-an", "--defined-only", module], text=True)
    for row in text.splitlines():
        parts = row.split()
        if len(parts) >= 3 and parts[1] in ("t", "T"):
            symbols[int(parts[0], 16)] = parts[2]
    rows = []
    for row in Path(output).read_text().splitlines():
        address, count, inclusive, own = row.split()
        rows.append((symbols.get(int(address, 16), address), int(count), int(inclusive), int(own)))
    total = sum(row[3] for row in rows)
    denominator = "ac_http_prepare" if any(row[0] == "ac_http_prepare" for row in rows) else "ac_http_prepare_native"
    requests = next((row[1] for row in rows if row[0] == denominator), 0)
    print("Instrumented C subtrees only, including instrumentation tax; not throughput evidence")
    print("Request denominator (" + denominator + "):", requests)
    Path(str(output) + ".functions.json").write_text(json.dumps({
        "requests": requests,
        "request_denominator": denominator,
        "functions": [{"name": name, "calls": count, "inclusive_ns": inclusive,
                       "self_ns": own, "calls_per_request": count / requests if requests else None}
                      for name, count, inclusive, own in rows]}, indent=2))
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
    parser.add_argument("--suite", choices=["historical", "isolated-single"], default="historical")
    parser.add_argument("--prefix", choices=["plain", "combined", "prometheus", "metrics", "auth", "limit"],
                        default="plain")
    parser.add_argument("--sampler", choices=["jit", "pc", "functions", "traces", "route", "alloc"], default="jit")
    args = parser.parse_args()
    single_plugin = None
    if args.suite == "isolated-single":
        plugins = {"prometheus": "prometheus", "metrics": "prometheus",
                   "auth": "key-auth", "limit": "limit-count"}
        if args.prefix not in plugins:
            parser.error("isolated-single requires --prefix prometheus, auth, or limit")
        single_plugin = plugins[args.prefix]
    target_prefix = "metrics" if args.prefix == "prometheus" else args.prefix
    if args.sampler in ("functions", "route"):
        assert args.mode == "native"
        context = os.environ.copy()
        context["APISIX_CORE_FUNCTION_PROFILE" if args.sampler == "functions"
                else "APISIX_CORE_ROUTE_PROFILE"] = "1"
        subprocess.run(["python3", str(HERE / "tools/rebuild.py")], env=context, check=True)
    cpu = sorted(os.sched_getaffinity(0))
    stop(UPSTREAM)
    prepare(args.mode, benchmark=True, gateway_cpu=cpu[0], upstream_cpu=cpu[1],
            single_plugin=single_plugin)
    stop()
    output = f"/tmp/apisix-core-profile-{args.mode}-{args.suite}-{args.prefix}-{args.sampler}.txt"
    # The worker writes the profile, so an earlier worker's file must be removed.
    Path(output).unlink(missing_ok=True)
    Path(output + ".maps").unlink(missing_ok=True)
    path = ROOT / "conf/nginx.conf"
    text = path.read_text()
    marker = ('require("apisix.native.engine").init_worker()' if args.mode == "native"
              else "apisix.http_init_worker()")
    assert text.count(marker) == 1
    if args.sampler == "alloc":
        subprocess.run(["gcc", "-shared", "-fPIC", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-I/usr/local/openresty/luajit/include/luajit-2.1",
                        "-o", "/tmp/apisix-allocator-sampler.so",
                        str(HERE / "tools/allocator_sampler.c")], check=True)
        profile = '''
        if require("ngx.process").type() == "worker" then
            local ffi = require("ffi")
            -- Public NGINX stub_status counter, confirmed exported by this
            -- pinned x86_64 runtime. Accepted requests, not completions.
            assert(ffi.sizeof("unsigned long") == 8)
            ffi.cdef[[extern volatile unsigned long *ngx_stat_requests;]]
            local sampler = assert(package.loadlib("/tmp/apisix-allocator-sampler.so",
                                                   "luaopen_allocator_sampler"))()
            local requests, started
            assert(ngx.timer.at(2, function()
                requests = tonumber(ffi.C.ngx_stat_requests[0])
                started = ngx.now()
                assert(sampler.start())
            end))
            assert(ngx.timer.at(12, function()
                local finished = tonumber(ffi.C.ngx_stat_requests[0])
                local result = assert(sampler.stop())
                result.accepted_requests = finished - requests
                result.window_wall_seconds = ngx.now() - started
                result.normalization = "NGINX accepted requests across this master; includes " ..
                    "background HTTP and in-flight boundary effects, not completed wrk requests"
                local out = assert(io.open("__OUTPUT__", "w"))
                out:write(assert(require("cjson.safe").encode(result)))
                out:close()
            end))
        end
'''
    elif args.sampler == "route":
        profile = '''
        if require("ngx.process").type() == "worker" then
            local ffi = require("ffi")
            ffi.cdef[[int ac_route_profile_start(void); int ac_route_profile_stop(const char *);]]
            ngx.timer.at(2, function() assert(ffi.C.ac_route_profile_start() == 0) end)
            ngx.timer.at(12, function() assert(ffi.C.ac_route_profile_stop("__OUTPUT__") == 0) end)
        end
'''
    elif args.sampler == "traces":
        profile = TRACE_PROFILE
    elif args.sampler == "jit":
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
    Path(output + ".nginx.conf").write_text(path.read_text())
    metadata = {"mode": args.mode, "suite": args.suite, "prefix": args.prefix,
                "sampler": args.sampler, "source": str(SOURCE), "diagnostic_seconds": 15,
                "metrics_scrape": False,
                "directives": os.environ.get("APISIX_CORE_DIRECTIVES", ""),
                "note": "Diagnostic run only; not acceptance throughput evidence",
                "sha256": {str(item): hashlib.sha256(item.read_bytes()).hexdigest()
                           for item in [Path(MODULE), HERE / "runtime.lua", SOURCE / "apisix/init.lua"]}}
    if args.sampler == "alloc":
        metadata["allocator_sampler_sha256"] = {
            str(item): hashlib.sha256(item.read_bytes()).hexdigest()
            for item in [HERE / "tools/allocator_sampler.c", Path("/tmp/apisix-allocator-sampler.so")]}
    Path(output + ".metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    subprocess.run([NGINX, "-p", str(ROOT), "-c", "conf/nginx.conf"], env=NGINX_ENV, check=True)
    try:
        def pin():
            os.sched_setaffinity(0, set(cpu[2:]))
        with Path(output + ".wrk.txt").open("w") as raw:
            subprocess.run(["wrk", "-t2", "-c32", "-d15s", "-H", "apikey: demo-key",
                            f"http://127.0.0.1:9080/{target_prefix}/profile"],
                           preexec_fn=pin, stdout=raw, check=True)
        print("Raw diagnostic evidence:", output)
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
