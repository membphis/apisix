#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise API-context reuse against Lua lifecycle, including concurrent yields."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import http.client
import os
from pathlib import Path
import socket
import subprocess
import time

import test_demo as demo


def check(mode, native_pool=False, extended=False):
    os.environ["APISIX_CORE_DIRECTIVES"] = ("apisix_core_request_pool on; " +
        ("apisix_core_request_pool_native on;" if native_pool else ""))
    demo.prepare(mode)
    demo.stop()
    config = demo.ROOT / "conf/nginx.conf"
    text = config.read_text()
    marker = ('require("apisix.native.engine").init_worker()' if mode == "native"
              else "apisix.http_init_worker()")
    observer = r'''
        local tablepool = require("tablepool")
        local fetch, release = tablepool.fetch, tablepool.release
        local seen = setmetatable({}, {__mode="k"})
        local active = setmetatable({}, {__mode="k"})
        local pool_name = "__POOL__"
        local native_pool = __NATIVE_POOL__
        local clear_pool
        local discarded = {}
        if native_pool then
            local clear = require("table.clear")
            clear_pool = function(value)
                if rawget(value, "pool_probe") then
                    assert(not active[value], "live/yielding api_ctx returned to pool")
                    if value.pool_probe.clear_error then
                        error("REQUEST_POOL_EXPECTED_CLEAR_ERROR")
                    end
                    ngx.log(ngx.WARN, "REQUEST_POOL_RELEASE")
                end
                return clear(value)
            end
            package.loaded["table.clear"] = clear_pool
            local core_ctx = require("apisix.core.ctx")
            local release_vars = core_ctx.release_vars
            core_ctx.release_vars = function(ctx)
                release_vars(ctx)
                if ctx.pool_probe and ctx.pool_probe.clear_missing then
                    package.loaded["table.clear"] = nil
                end
            end
        end
        tablepool.fetch = function(name, ...)
            local value = fetch(name, ...)
            if not native_pool and name == pool_name then
                assert(next(value) == nil, "pooled api_ctx retained request data")
            end
            return value
        end
        tablepool.release = function(name, value, ...)
            if not native_pool and name == pool_name then
                assert(not active[value], "live/yielding api_ctx returned to pool")
                ngx.log(ngx.WARN, "REQUEST_POOL_RELEASE")
            end
            return release(name, value, ...)
        end
        local protected_ctx
        local plugin = require("apisix.plugins.prometheus")
        local original_log = plugin.log
        plugin.rewrite = function(conf, ctx)
            if native_pool then
                package.loaded["table.clear"] = clear_pool
                for _, value in ipairs(discarded) do
                    assert(value ~= ctx and value.pool_probe,
                           "failed clear context was recycled")
                end
            end
            assert(not active[ctx] and ctx.pool_probe == nil, "request alias or stale marker")
            if protected_ctx then
                assert(protected_ctx ~= ctx and getmetatable(protected_ctx)
                       and protected_ctx.pool_sentinel, "metatable-bearing api_ctx was recycled")
            end
            local reused = seen[ctx]
            if reused then
                ngx.log(ngx.WARN, "REQUEST_POOL_REUSE")
            elseif ngx.var.arg_rotate then
                ngx.log(ngx.WARN, "REQUEST_POOL_ROTATION_NEW")
            end
            seen[ctx] = true
            local token = ngx.var.arg_token or ngx.var.request_uri
            active[ctx] = token
            ctx.pool_probe = {token=token, clear_error=ngx.var.arg_clear_error,
                              clear_missing=ngx.var.arg_clear_missing}
            if ngx.var.arg_gate then
                local shared = ngx.shared.pool_test
                local gate = ngx.var.arg_gate
                shared:incr("ready-" .. gate, 1, 0)
                if reused then shared:incr("reused-" .. gate, 1, 0) end
                local deadline = ngx.now() + 4
                while (shared:get("ready-" .. gate) or 0) < 220 do
                    assert(ngx.now() < deadline, "pool capacity gate timed out")
                    ngx.sleep(0.001)
                end
                ngx.header["X-Pool-Capacity-Reused"] = shared:get("reused-" .. gate) or 0
            end
            local hold = tonumber(ngx.var.arg_hold)
            if hold then ngx.sleep(hold) end
            assert(ctx == ngx.ctx.api_ctx and ctx.ngx_ctx == ngx.ctx)
            assert(active[ctx] == token and ctx.pool_probe.token == token,
                   "context changed while request yielded")
            if ngx.var.arg_fail then error("REQUEST_POOL_EXPECTED_ERROR") end
        end
        plugin.log = function(conf, ctx)
            local value = active[ctx]
            if value then
                assert(ctx.pool_probe and ctx.pool_probe.token == value)
                active[ctx] = nil
            end
            if native_pool and (ngx.var.arg_clear_error or ngx.var.arg_clear_missing) then
                discarded[#discarded + 1] = ctx
            end
            if ngx.var.arg_meta then
                protected_ctx = ctx
                setmetatable(ctx, {__index={pool_sentinel=true}})
            end
            return original_log(conf, ctx)
        end
'''.replace("__POOL__", "native_api_ctx" if mode == "native" else "api_ctx")\
       .replace("__NATIVE_POOL__", "true" if native_pool else "false")
    assert text.count(marker) == 1
    text = text.replace("http {", "http {\nlua_shared_dict pool_test 1m;", 1)
    config.write_text(text.replace(marker, observer + marker))
    subprocess.run([demo.NGINX, "-p", str(demo.ROOT), "-c", "conf/nginx.conf"],
                   env=demo.NGINX_ENV, check=True)
    demo.ready()
    with ThreadPoolExecutor(max_workers=16) as pool:
        replies = list(pool.map(lambda n: demo.request(
            f"/plain/pool?token=parallel-{n}&hold=0.005")[0], range(64)))
    assert replies == [200] * 64, replies
    for n in range(24):
        assert demo.request(f"/plain/pool?token=serial-{n}")[0] == 200
    assert demo.request("/plain/pool?token=expected-error&fail=1")[0] == 500
    assert demo.request("/plain/pool?token=after-error")[0] == 200
    if mode == "native":
        assert demo.request("/plain/pool?token=metatable&meta=1")[0] == 200
        assert demo.request("/plain/pool?token=after-metatable")[0] == 200
    if native_pool:
        assert demo.request("/plain/pool?token=clear-missing&clear_missing=1")[0] == 200
        assert demo.request("/plain/pool?token=after-clear-missing")[0] == 200
        assert demo.request("/plain/pool?token=clear-error&clear_error=1")[0] == 200
        assert demo.request("/plain/pool?token=after-clear-error")[0] == 200
    # The aborted coroutine owns its table until NGINX completes request cleanup.
    client = socket.create_connection(("127.0.0.1", 9080))
    client.sendall(b"GET /plain/pool?token=aborted&hold=0.05 HTTP/1.1\r\nHost: localhost\r\n\r\n")
    client.close()
    for n in range(8):
        assert demo.request(f"/plain/pool?token=after-abort-{n}&hold=0.002")[0] == 200
    time.sleep(0.1)
    if native_pool and extended:
        # Two waves hold >200 contexts alive. Exactly 200 from the first wave
        # may be borrowed in the second; excess releases must be discarded.
        for wave in (1, 2):
            with ThreadPoolExecutor(max_workers=220) as pool:
                replies = list(pool.map(lambda n: demo.request(
                    f"/plain/pool?token=capacity-{wave}-{n}&gate={wave}"), range(220)))
            assert [reply[0] for reply in replies] == [200] * 220, replies
            if wave == 2:
                assert all(reply[1]["X-Pool-Capacity-Reused"] == "200"
                           for reply in replies), replies
        # Cross the exact installed tablepool's 20,000-release retirement rule.
        connection = http.client.HTTPConnection("127.0.0.1", 9080, timeout=5)
        try:
            for n in range(20010):
                connection.request("GET", f"/plain/pool?token=rotation-{n}&rotate=1")
                response = connection.getresponse()
                response.read()
                assert response.status == 200, (n, response.status)
        finally:
            connection.close()
        time.sleep(0.1)
    text = (demo.ROOT / "logs/error.log").read_text()
    assert text.count("REQUEST_POOL_REUSE") >= 10, text
    assert text.count("REQUEST_POOL_RELEASE") >= 90, text
    if native_pool and extended:
        assert "REQUEST_POOL_ROTATION_NEW" in text, "pool did not retire after 20,000 releases"
    for failure in ("pooled api_ctx retained", "live/yielding api_ctx", "request alias or stale",
                    "context changed while", "native api_ctx pool release failed",
                    "pool capacity gate timed out", "metatable-bearing api_ctx was recycled",
                    "failed clear context was recycled"):
        assert failure not in text, text
    # Error sample is intentional. All other error/alert blocks must be absent.
    for line in text.splitlines():
        if "[error]" in line or "[alert]" in line:
            assert ("REQUEST_POOL_EXPECTED_ERROR" in line
                    or "REQUEST_POOL_EXPECTED_CLEAR_ERROR" in line
                    or "client prematurely closed" in line), line
    if native_pool:
        assert text.count("native registry api_ctx pool clear failed:") == 1, text
    print(f"PASS {mode} native_pool={native_pool} extended={extended}: "
          "bounded API-context reuse, 64 concurrent yields, clean fields, "
          "error/abort recovery and no live-table release", flush=True)
    demo.stop()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--extended", action="store_true",
                        help="also exercise native capacity200 and 20,000-release retirement")
    args = parser.parse_args()
    previous = os.environ.get("APISIX_CORE_DIRECTIVES", "")
    os.environ["APISIX_CORE_DIRECTIVES"] = "apisix_core_request_pool on;"
    try:
        check("lua")
        check("native")
        check("native", native_pool=True, extended=args.extended)
    finally:
        demo.stop()
        if previous:
            os.environ["APISIX_CORE_DIRECTIVES"] = previous
        else:
            os.environ.pop("APISIX_CORE_DIRECTIVES", None)


if __name__ == "__main__":
    main()
