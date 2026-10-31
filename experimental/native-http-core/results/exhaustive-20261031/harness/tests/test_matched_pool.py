#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise opt-in matched-record pooling without changing filter-owned records."""
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import subprocess
import time

import test_demo as demo


FILTER = r'''function(vars, opts, ctx, match_opts)
    assert(opts == match_opts and vars == ctx.var)
    ctx.filter_matched = opts.matched
    opts.matched.filter_probe = vars.arg_token
    ngx.sleep(0.001)
    assert(ctx.filter_matched == opts.matched, "filter identity changed while yielding")
    return vars.arg_allow == "yes"
end'''


OBSERVER = r'''
        local pool_enabled = __ENABLED__
        local clear = require("table.clear")
        local seen = setmetatable({}, {__mode="k"})
        local active = setmetatable({}, {__mode="k"})
        local protected, detached
        package.loaded["table.clear"] = function(value)
            if rawget(value, "matched_pool_probe") then
                assert(not active[value], "live matched record returned to pool")
                ngx.log(ngx.WARN, "MATCHED_POOL_RELEASE")
            end
            return clear(value)
        end
        local plugin = require("apisix.plugins.prometheus")
        local original_log = plugin.log
        plugin.rewrite = function(conf, ctx)
            local matched = ctx.curr_req_matched
            local token = ngx.var.arg_token or ngx.var.request_uri
            assert(type(matched) == "table" and not active[matched], "matched in-flight alias")
            assert(not matched.matched_pool_probe, "matched retained previous request data")
            if protected then
                assert(protected ~= matched and getmetatable(protected)
                       and protected.sentinel, "metatable matched record recycled")
            end
            if detached then
                assert(detached ~= matched and detached.matched_pool_probe,
                       "replaced matched record recycled")
            end
            if ctx.filter_matched then
                assert(matched == ctx.filter_matched and matched.filter_probe == token,
                       "filter matched record identity or fields changed")
            else
                assert(matched.filter_probe == nil, "filter state leaked into plain record")
                if seen[matched] then
                    assert(pool_enabled, "matched reused with pool off")
                    ngx.log(ngx.WARN, "MATCHED_POOL_REUSE")
                end
                seen[matched] = true
            end
            assert(matched._method == ngx.req.get_method())
            if ctx.route_id == "plain" then
                assert(matched.item == nil and matched._host == nil,
                       "capture or host leaked into plain record")
            elseif ctx.route_id == "param" then
                assert(matched.item == ngx.var.arg_expect, "wrong parameter capture")
            end
            active[matched] = token
            matched.matched_pool_probe = token
            local hold = tonumber(ngx.var.arg_hold)
            if hold then ngx.sleep(hold) end
            assert(ctx.curr_req_matched == matched and active[matched] == token
                   and matched.matched_pool_probe == token, "matched changed while yielding")
            ngx.header["X-Matched-Pool-Route"] = ctx.route_id
            if ngx.var.arg_fail then error("MATCHED_POOL_EXPECTED_ERROR") end
        end
        plugin.log = function(conf, ctx)
            local matched = ctx.curr_req_matched
            if active[matched] then
                assert(matched.matched_pool_probe == active[matched])
                active[matched] = nil
            end
            if ngx.var.arg_meta then
                protected = matched
                setmetatable(matched, {__index={sentinel=true}})
            elseif ngx.var.arg_replace then
                detached = matched
                ctx.curr_req_matched = {replacement=true}
            end
            return original_log(conf, ctx)
        end
'''


def run(enabled, api_pool=False):
    os.environ["APISIX_CORE_DIRECTIVES"] = (
        "apisix_core_lazy_route_opts on; apisix_core_matched_pool "
        + ("on;" if enabled else "off;")
        + (" apisix_core_request_pool_native on;" if api_pool else ""))
    fixture = Path("/tmp/native-matched-pool-fixture.json")
    routes = [
        {"id": "plain", "uri": "/plain/*"},
        {"id": "param", "uri": "/param/:item"},
        {"id": "filter", "uri": "/filtered", "priority": 100, "filter_func": FILTER},
        {"id": "fallback", "uri": "/filtered"},
    ]
    for route in routes:
        route["plugins"] = {"prometheus": {}}
    fixture.write_text(json.dumps({"upstream": "127.0.0.1:1980", "routes": routes,
                                   "router": "radixtree_uri_with_parameter",
                                   "consumers": [], "global_rules": []}))
    demo.prepare("native", fixture=fixture)
    demo.stop()
    path = demo.ROOT / "conf/nginx.conf"
    text = path.read_text()
    marker = 'require("apisix.native.engine").init_worker()'
    assert text.count(marker) == 1
    observer = OBSERVER.replace("__ENABLED__", "true" if enabled else "false")
    path.write_text(text.replace(marker, observer + marker))
    subprocess.run([demo.NGINX, "-p", str(demo.ROOT), "-c", "conf/nginx.conf"],
                   env=demo.NGINX_ENV, check=True)
    demo.ready("/missing", expected=404)
    for index in range(12):
        assert demo.request(f"/param/value-{index}?expect=value-{index}&token=param-{index}")[0] == 200
        assert demo.request(f"/plain/clean?token=plain-{index}")[0] == 200
    with ThreadPoolExecutor(max_workers=16) as pool:
        replies = list(pool.map(lambda n: demo.request(
            f"/plain/concurrent?token=parallel-{n}&hold=0.005")[0], range(64)))
    assert replies == [200] * 64, replies
    for index in range(8):
        for allow, expected in (("yes", "filter"), ("no", "fallback")):
            response = demo.request(f"/filtered?allow={allow}&token=filter-{index}-{allow}&hold=0.002")
            assert response[0] == 200 and response[1]["X-Matched-Pool-Route"] == expected, response
    assert demo.request("/plain/error?token=error&fail=1")[0] == 500
    assert demo.request("/plain/recovery?token=recovery")[0] == 200
    assert demo.request("/plain/meta?token=meta&meta=1")[0] == 200
    assert demo.request("/plain/after-meta?token=after-meta")[0] == 200
    assert demo.request("/plain/replace?token=replace&replace=1")[0] == 200
    assert demo.request("/plain/after-replace?token=after-replace")[0] == 200
    time.sleep(0.05)
    text = (demo.ROOT / "logs/error.log").read_text()
    if enabled:
        assert text.count("MATCHED_POOL_REUSE") >= 20, text
        assert text.count("MATCHED_POOL_RELEASE") >= 80, text
    else:
        assert "MATCHED_POOL_REUSE" not in text and "MATCHED_POOL_RELEASE" not in text, text
    for line in text.splitlines():
        if "[error]" in line or "[alert]" in line:
            assert "MATCHED_POOL_EXPECTED_ERROR" in line, line
    demo.stop()
    print(f"PASS matched_pool={enabled} api_pool={api_pool}: clean captures, concurrent yields, filter identity, "
          "error recovery, metatable and replacement aliases", flush=True)


if __name__ == "__main__":
    previous = os.environ.get("APISIX_CORE_DIRECTIVES")
    try:
        run(False)
        run(True)
        run(True, api_pool=True)
    finally:
        demo.stop()
        if previous is None:
            os.environ.pop("APISIX_CORE_DIRECTIVES", None)
        else:
            os.environ["APISIX_CORE_DIRECTIVES"] = previous
