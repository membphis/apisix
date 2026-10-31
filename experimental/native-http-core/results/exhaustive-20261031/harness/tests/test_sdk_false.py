#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Differential false/nil SDK checks; set APISIX_CORE_SOURCE=/stable for 3.19.0.

Run only after applying the staged SDK fix and rebuilding the native module.
These assertions use the stable Lua behavior, including legitimate false cache
values; they deliberately do not apply one truthiness rule to every cache.
"""
import os
import subprocess
import time

import test_demo as demo


OBSERVER = r'''
        local plugin = require("apisix.plugins.prometheus")
        plugin.rewrite = function(conf, ctx)
            if not ngx.var.uri:find("^/plain/sdk%-false/") then return end
            local request = require("apisix.core.request")
            local kind = ngx.var.uri:match("/([^/]+)$")

            if kind == "headers" then
                ctx.headers = false
                local headers = request.headers(ctx)
                assert(type(headers) == "table" and headers["x-sdk-test"] == "value")
                assert(request.headers(ctx) == headers)
                assert(request.headers(false) == headers)
                ctx.headers = false
                assert(request.header(ctx, "X-SDK-Test") == "value")
                headers = request.headers(ctx)
                headers["x-false-value"] = false
                assert(request.header(ctx, "x-false-value") == false)
                assert(request.header(false, "x-false-value") == false)

                ctx.req_uri_args = false
                local args = request.get_uri_args(ctx)
                assert(type(args) == "table" and args.a == "one")
                assert(request.get_uri_args(ctx) == args)
                ctx.req_uri_args = false
                args = request.get_uri_args(false)
                assert(args.a == "one" and request.get_uri_args(nil) == args)
                request.set_uri_args(false, "a=two")
                assert(ctx.req_uri_args == nil)
                assert(request.get_uri_args(ctx).a == "two")

            elseif kind == "getters" then
                local fake = {var = {
                    realip_remote_addr = false, remote_addr = "fallback",
                    remote_port = false, server_port = false,
                    scheme = false, host = false,
                }}
                assert(request.get_ip(fake) == "fallback")
                assert(request.get_remote_client_ip(fake) == "fallback")
                fake.var.remote_addr = false
                assert(request.get_ip(fake) == "")
                assert(request.get_remote_client_ip(fake) == "")
                assert(request.get_scheme(fake) == "" and request.get_host(fake) == "")
                assert(request.get_remote_client_port(fake) == nil)
                assert(request.get_port(fake) == nil)
                -- Zero and the empty string are truthy in Lua.
                fake.var.realip_remote_addr = ""
                fake.var.remote_addr = 0
                fake.var.host = 0
                fake.var.scheme = ""
                fake.var.remote_port = 0
                fake.var.server_port = "0"
                assert(request.get_ip(fake) == "")
                assert(request.get_remote_client_ip(fake) == 0)
                assert(request.get_host(fake) == 0 and request.get_scheme(fake) == "")
                assert(request.get_remote_client_port(fake) == 0 and request.get_port(fake) == 0)
                for _, name in ipairs({"get_ip", "get_remote_client_ip",
                    "get_remote_client_port", "get_host", "get_scheme", "get_port"}) do
                    assert(request[name](false) == request[name](ctx), name)
                    assert(request[name](nil) == request[name](ctx), name)
                end

            elseif kind == "var-cache" then
                local core_ctx = require("apisix.core.ctx")
                local pool = require("tablepool")
                local original_fetch = pool.fetch
                local supplied = {_cache = false}
                local fake = {}
                pool.fetch = function(name, ...)
                    if name == "ctx_var" then return supplied end
                    return original_fetch(name, ...)
                end
                local ok, err = pcall(core_ctx.set_vars_meta, fake)
                pool.fetch = original_fetch
                assert(ok, err)
                assert(fake.var == supplied and type(rawget(supplied, "_cache")) == "table")
                supplied._cache.sdk_false_cached = false
                assert(supplied.sdk_false_cached == false, "false variable cache value lost")
                core_ctx.release_vars(fake)
                assert(fake.var == nil)

            elseif kind == "form" then
                ctx.req_post_args = false
                local args = request.get_post_args(ctx)
                assert(type(args) == "table" and args.a == "one" and args.b == "two")
                assert(request.get_post_args(ctx) == args)
                ctx.req_post_args = false
                args = request.get_post_args(false)
                assert(args.a == "one" and request.get_post_args(nil) == args)
                assert(request.get_body(false, ctx) == "a=one&b=two")
                assert(request.get_body(false, false) == "a=one&b=two")
                ctx._request_body_table = nil
                local body, err = request.get_request_body_table(false, false, false)
                assert(body and body.a == "one", err)
                assert(request.get_request_body_table(ctx, false, false) == body)

            elseif kind == "json" then
                assert(request.get_body(false, ctx) == '{"a":"one"}')
                assert(request.get_body(false, false) == '{"a":"one"}')
                ctx._request_body_table = nil
                local body, err = request.get_request_body_table(false, false, false)
                assert(body and body.a == "one", err)
                assert(request.get_request_body_table(ctx, false, false) == body)
                assert(request.get_json_request_body_table(false) == body)
                ctx._request_body_table = nil
                body, err = request.get_json_request_body_table(false)
                assert(body and body.a == "one", err)
                -- The parsed-body cache explicitly uses ~= nil in stable Lua.
                -- False content_type must not request a type-mismatch check.
                ctx._request_body_table = false
                ctx._request_body_type = "application/json"
                local cached, cached_err = request.get_request_body_table(ctx, false, false)
                assert(cached == false and cached_err == nil)
                assert(ctx._request_body_table == false)
                local json_cached, json_err = request.get_json_request_body_table(false)
                assert(json_cached == nil and type(json_err) == "table")
                assert(json_err.message == nil and next(json_err) == nil)
                assert(ctx._request_body_table == false)
                assert(select("#", request.get_json_request_body_table(false)) == 2)
                cached, cached_err = request.get_request_body_table(ctx, "text/plain", false)
                assert(cached == nil and cached_err:find("request body type mismatch", 1, true))
                ctx._request_body_table = body

            elseif kind == "negative-limit" then
                local payload = '{"a":"one"}'
                for _, max_size in ipairs({-3, -1, 0, #payload - 1}) do
                    local body, err = request.get_body(max_size, ctx)
                    assert(body == nil and type(err) == "string")
                    assert(err == "request size " .. #payload ..
                        " is greater than the maximum size " .. max_size .. " allowed", err)
                    -- Force the post-read size check by omitting the optional
                    -- Content-Length getter from this supplied context.
                    body, err = request.get_body(max_size, {var = {}})
                    assert(body == nil and type(err) == "string")
                    assert(err == "request size " .. #payload ..
                        " is greater than the maximum size " .. max_size .. " allowed", err)
                end
                assert(request.get_body(#payload, ctx) == payload)
                assert(request.get_body(false, ctx) == payload)
                assert(request.get_body(nil, ctx) == payload)

            else
                error("unknown SDK false test: " .. kind)
            end
            ngx.log(ngx.WARN, "SDK_FALSE_OBSERVER_OK_", kind)
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
    path.write_text(text.replace(marker, OBSERVER + marker))
    subprocess.run([demo.NGINX, "-p", str(demo.ROOT), "-c", "conf/nginx.conf"],
                   env=demo.NGINX_ENV, check=True)
    demo.ready()
    cases = [
        ("headers?a=one", {"X-SDK-Test": "value"}, "GET", None),
        ("getters", {}, "GET", None),
        ("var-cache", {}, "GET", None),
        ("form", {"Content-Type": "application/x-www-form-urlencoded"}, "POST", "a=one&b=two"),
        ("json", {"Content-Type": "application/json"}, "POST", '{"a":"one"}'),
        ("negative-limit", {"Content-Type": "application/json"}, "POST", '{"a":"one"}'),
    ]
    for _ in range(2):
        for suffix, headers, method, body in cases:
            result = demo.request("/plain/sdk-false/" + suffix, headers, method, body)
            assert result[0] == 200, (mode, suffix, result)
    log = demo.ROOT / "logs/error.log"
    for _ in range(100):
        text = log.read_text()
        if text.count("SDK_FALSE_OBSERVER_OK_") == 2 * len(cases):
            break
        time.sleep(0.01)
    assert text.count("SDK_FALSE_OBSERVER_OK_") == 2 * len(cases), text
    for suffix, _, _, _ in cases:
        assert text.count("SDK_FALSE_OBSERVER_OK_" + suffix.split("?")[0]) == 2, text
    assert "[error]" not in text and "[alert]" not in text, text
    print("PASS false/nil SDK", mode, flags or "all off", flush=True)
    demo.stop()


if __name__ == "__main__":
    previous = os.environ.get("APISIX_CORE_DIRECTIVES", "")
    try:
        check("lua", "")
        check("native", "")
        check("native", "apisix_core_request_header_flags on; "
              "apisix_core_header_scratch on; apisix_core_sdk_cache_replay on;")
    finally:
        demo.stop()
        os.environ["APISIX_CORE_DIRECTIVES"] = previous
