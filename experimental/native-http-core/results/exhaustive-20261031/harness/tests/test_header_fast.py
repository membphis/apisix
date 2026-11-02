#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Header SDK cache/dirty-bit semantics with each experiment independently."""
import os
import subprocess
import time

import test_demo as demo


OBSERVER = r'''
        local plugin = require("apisix.plugins.prometheus")
        plugin.rewrite = function(conf, ctx)
            if ngx.var.uri ~= "/plain/header-fast" then return end
            local request = require("apisix.core.request")
            local h = request.headers(ctx)
            assert(request.headers(ctx) == h)
            assert(request.header(ctx, "X-Initial") == "initial")

            request.set_header(ctx, "Probe", "first")
            assert(request.headers(ctx) == h)
            assert(request.header(ctx, "probe") == "first")
            assert(ctx.var.http_probe == "first")
            request.set_header(ctx, "Probe", "second")
            assert(ctx.var.http_probe == "second")
            request.add_header(ctx, "Probe", "third")
            assert(request.header(ctx, "probe") == "second")
            assert(request.headers(ctx)["probe"][2] == "third")

            -- Explicitly retain the known baseline difference: original Lua
            -- clears http_x-cache-test while its reader caches http_x_cache_test.
            -- Native normalized this before these experiments; do not credit it
            -- as a new optimization or pretend the Lua baseline is already fixed.
            request.set_header(ctx, "X-Cache-Test", "before")
            assert(ctx.var.http_x_cache_test == "before")
            request.set_header(ctx, "X-Cache-Test", "after")
            assert(ctx.var.http_x_cache_test == (__NATIVE__ and "after" or "before"))
            ngx.log(ngx.WARN, "HEADER_HYPHEN_BASELINE_", ctx.var.http_x_cache_test)

            -- An external write must invalidate the cached headers, including
            -- when a subsequent SDK write occurs before the next read.
            ngx.req.set_header("X-External", "outside")
            request.set_header(ctx, "Probe", "after-external")
            local fresh = request.headers(ctx)
            assert(fresh ~= h)
            assert(fresh["x-external"] == "outside")
            assert(request.header(ctx, "probe") == "after-external")
            request.set_header(ctx, "Probe", nil)
            assert(request.header(ctx, "probe") == nil)

            request.set_header(ctx, "X-Multi", {"one", "two"})
            assert(request.header(ctx, "x-multi") == "one")
            request.add_header(ctx, "X-Multi", "three")
            assert(request.headers(ctx)["x-multi"][3] == "three")
            local long_name = "X-" .. string.rep("Long", 60)
            request.set_header(ctx, long_name, "long-value")
            collectgarbage("collect")
            assert(request.header(ctx, long_name) == "long-value")
            request.set_header(ctx, long_name, nil)
            assert(request.header(ctx, long_name) == nil)

            -- Use the original Lua oracle, including its and/or fallback when
            -- the first array element is nil/false. Old native rawgeti differed.
            local headers = request.headers(ctx)
            headers["x-false"] = false
            assert(request.header(ctx, "x-false") == false)
            assert(request.headers(false) == headers and request.headers(nil) == headers)
            assert(request.header(false, "x-false") == false)
            assert(request.header(nil, "x-false") == false)
            local empty, false_first, nil_meta = {}, {false}, setmetatable({}, {
                __index = function() return nil end
            })
            headers["x-array-empty"] = empty
            headers["x-array-false"] = false_first
            headers["x-array-nil-meta"] = nil_meta
            assert(request.header(ctx, "x-array-empty") == empty)
            assert(request.header(ctx, "x-array-false") == false_first)
            assert(request.header(ctx, "x-array-nil-meta") == nil_meta)
            local array_reads = 0
            headers["x-array-meta"] = setmetatable({}, {__index = function(_, key)
                assert(key == 1)
                array_reads = array_reads + 1
                return "from-meta"
            end})
            assert(request.header(ctx, "x-array-meta") == "from-meta")
            assert(array_reads == 1, "array metatable getter invoked twice")
            local ctx_reads, header_reads = 0, 0
            local custom_headers = setmetatable({}, {__index = function(_, name)
                header_reads = header_reads + 1
                assert(name == "custom")
                return "value"
            end})
            local custom = setmetatable({}, {__index = function(_, name)
                if name == "headers" then ctx_reads = ctx_reads + 1; return custom_headers end
            end})
            assert(request.header(custom, "custom") == "value")
            assert(ctx_reads == 1 and header_reads == 1, "cache probe invoked a getter twice")
            local ok = pcall(request.header, 42, "header")
            assert(not ok, "invalid context was accepted")
            ngx.log(ngx.WARN, "HEADER_FAST_OBSERVER_OK")
        end
'''


def check(mode, flags):
    os.environ["APISIX_CORE_DIRECTIVES"] = flags
    demo.prepare(mode)
    demo.stop()
    path = demo.ROOT / "conf/nginx.conf"
    text = path.read_text()
    marker = ('require("apisix.native.engine").init_worker()' if mode == "native"
              else "apisix.http_init_worker()")
    assert text.count(marker) == 1
    path.write_text(text.replace(marker, OBSERVER.replace("__NATIVE__", "true" if mode == "native" else "false") + marker))
    subprocess.run([demo.NGINX, "-p", str(demo.ROOT), "-c", "conf/nginx.conf"],
                   env=demo.NGINX_ENV, check=True)
    demo.ready()
    for _ in range(3):
        assert demo.request("/plain/header-fast", {"X-Initial": "initial"})[0] == 200
    log = demo.ROOT / "logs/error.log"
    for _ in range(100):
        text = log.read_text()
        if text.count("HEADER_FAST_OBSERVER_OK") == 3:
            break
        time.sleep(0.01)
    assert text.count("HEADER_FAST_OBSERVER_OK") == 3, text
    assert "[error]" not in text and "[alert]" not in text, text
    print("PASS header SDK", mode, flags or "all off", flush=True)
    demo.stop()


if __name__ == "__main__":
    previous = os.environ.get("APISIX_CORE_DIRECTIVES", "")
    flags = ["apisix_core_request_header_flags on;", "apisix_core_header_scratch on;",
             "apisix_core_sdk_cache_replay on;"]
    try:
        check("lua", "")
        for experiment in ["", *flags, " ".join(flags)]:
            check("native", experiment)
    finally:
        demo.stop()
        os.environ["APISIX_CORE_DIRECTIVES"] = previous
