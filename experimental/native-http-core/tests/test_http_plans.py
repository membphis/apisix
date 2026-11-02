#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Differential HTTP preparation checks for batching, read plans and plain URIs."""
import json
import os
from pathlib import Path
import subprocess

import test_demo as demo

FLAGS = "apisix_core_batch_vars on; apisix_core_http_var_plan on; apisix_core_plain_uri on;"

OBSERVER = r'''
        local core_ctx = require("apisix.core.ctx")
        local plugin = require("apisix.plugins.prometheus")
        local old_rewrite = plugin.rewrite
        local function note(ctx, name)
            ctx.read_order = ctx.read_order or {}
            ctx.read_order[#ctx.read_order + 1] = name
        end
        plugin.rewrite = function(conf, ctx)
            if ngx.var.uri:find("/plain/http-plan", 1, true) == 1 then
                ngx.header["X-Http-Plan"] = require("cjson.safe").encode({
                    order = ctx.read_order,
                    proto = ctx.var.http_x_forwarded_proto,
                    host = ctx.var.http_x_forwarded_host,
                    port = ctx.var.http_x_forwarded_port,
                    original = ctx.var.original_x_forwarded_proto,
                    replacement = ctx.cache_replaced or false,
                    uri = ctx.var.uri, request_uri = ctx.var.request_uri,
                    real_request_uri = ctx.var.real_request_uri,
                    sentinel = ctx.var._cache.sentinel
                })
            end
            if old_rewrite then return old_rewrite(conf, ctx) end
        end
'''
GETTERS = r'''
        core_ctx.register_var("http_x_forwarded_proto", function(ctx)
            note(ctx, "proto")
            ctx.var._cache.sentinel = false
            if __META__ then
                local cache = ctx.var._cache
                setmetatable(cache, {__newindex = function(t, key, value)
                    rawset(t, key, value)
                    if key == "http_x_forwarded_proto" then
                        local replacement = {}
                        for k, v in pairs(t) do replacement[k] = v end
                        ctx.var._cache = replacement
                        ctx.cache_replaced = true
                    end
                end})
            end
            return "untrusted-proto"
        end, {no_cacheable = true})
        core_ctx.register_var("scheme", function(ctx)
            note(ctx, "scheme")
            -- Registration during a preceding getter must be visible to the
            -- following read; a precompiled getter-admission mask would fail.
            core_ctx.register_var("http_host", function(current)
                note(current, "host")
                return "batch.example:4567"
            end, {no_cacheable = true})
            return "https"
        end, {no_cacheable = true})
        core_ctx.register_var("server_port", function(ctx)
            note(ctx, "port")
            return "1234"
        end, {no_cacheable = true})
'''


def sample(flags, metacache):
    os.environ["APISIX_CORE_DIRECTIVES"] = flags
    demo.prepare("native")
    demo.stop()
    path = demo.ROOT / "conf/nginx.conf"
    text = path.read_text()
    marker = 'require("apisix.native.engine").init_worker()'
    assert text.count(marker) == 1
    text = text.replace(marker, OBSERVER + marker + GETTERS.replace("__META__", str(metacache).lower()))
    path.write_text(text)
    subprocess.run([demo.NGINX, "-p", str(demo.ROOT), "-c", "conf/nginx.conf"],
                   env=demo.NGINX_ENV, check=True)
    demo.ready()
    result = []
    for target in ("/plain/http-plan", "/plain/http-plan?x=1", "/plain/http-plan/a%20b",
                   "/plain/http-plan/a%252Fb", "/plain/http-plan//tail"):
        status, headers, body = demo.request(target, {"X-Demo-Echo": "1", "X-Forwarded-Proto": "evil"})
        assert status == 200, (target, status, body)
        observed = json.loads(headers["X-Http-Plan"])
        assert observed["proto"] == "https" and observed["host"] == "batch.example:4567", observed
        assert observed["port"] == "4567" and observed["original"] == "untrusted-proto", observed
        assert observed["replacement"] == metacache and observed["sentinel"] is False, observed
        assert observed["order"] == ["proto", "scheme", "host", "port"], observed
        echoed = json.loads(body)
        result.append((observed, echoed["uri"], echoed["headers"].get("x-forwarded-proto"),
                       echoed["headers"].get("x-forwarded-host"), echoed["headers"].get("x-forwarded-port")))
    errors = (demo.ROOT / "logs/error.log").read_text()
    assert "[error]" not in errors and "[alert]" not in errors, errors
    demo.stop()
    return result


def main():
    previous = os.environ.get("APISIX_CORE_DIRECTIVES")
    try:
        for metacache in (False, True):
            assert sample("", metacache) == sample(FLAGS, metacache)
        print("PASS HTTP plans off/on: getter timing and dynamic registration, cache replacement "
              "fallback, false values, forwarded headers, plain/query/escaped/double-escaped/slash URIs")
    finally:
        demo.stop()
        if previous is None:
            os.environ.pop("APISIX_CORE_DIRECTIVES", None)
        else:
            os.environ["APISIX_CORE_DIRECTIVES"] = previous


if __name__ == "__main__":
    main()
