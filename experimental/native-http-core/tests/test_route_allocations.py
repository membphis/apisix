#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Observe lazy route allocations, filter identity and candidate growth over HTTP."""
import json
import os
from pathlib import Path
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from demo import ROOT, NGINX, NGINX_ENV, prepare, stop
from test_demo import request, ready


FILTER = """function(vars, opts, ctx, match_opts)
    assert(opts == match_opts and vars == ctx.var)
    assert(opts.remote_addr == "route-remote-1", "remote snapshot timing")
    assert(ctx.route_remote_reads == 1, "remote getter count before filter")
    assert(vars.remote_addr == "route-remote-2", "uncached getter")
    ctx.route_filter_matched = opts.matched
    opts.matched.custom = "retained"
    ngx.sleep(0.001)
    return vars.arg_allow == "yes"
end"""


OBSERVER = """
        local core_ctx = require("apisix.core.ctx")
        core_ctx.register_var("remote_addr", function(ctx)
            ctx.route_remote_reads = (ctx.route_remote_reads or 0) + 1
            return "route-remote-" .. ctx.route_remote_reads
        end, {no_cacheable = true})
        core_ctx.register_var("route_order", function(ctx)
            assert(ctx.route_remote_reads == 1, "remote getter must precede expression")
            return "seen"
        end)
        local last_matched
        local plugin = require("apisix.plugins.prometheus")
        plugin.rewrite = function(conf, ctx)
            local matched = ctx.curr_req_matched
            assert(type(matched) == "table" and matched ~= last_matched, "matched identity reused")
            last_matched = matched
            assert(matched._method == ngx.req.get_method())
            if ctx.route_filter_matched then
                assert(matched == ctx.route_filter_matched and matched.custom == "retained")
                assert(ctx.route_remote_reads == 2)
            else
                assert(ctx.route_remote_reads == 1, "remote getter skipped or repeated")
            end
            ngx.header["X-Route-Probe"] = ctx.route_id
            ngx.header["X-Matched-Path"] = matched._path
            if matched.item then ngx.header["X-Matched-Item"] = matched.item end
        end
"""


def run(flags):
    os.environ["APISIX_CORE_DIRECTIVES"] = flags
    fixture = Path("/tmp/native-route-allocation-fixture.json")
    routes = [
        {"id": "lazy", "uri": "/lazy", "vars": [["route_order", "==", "seen"]]},
        {"id": "filter", "uri": "/filtered", "priority": 100, "filter_func": FILTER},
        {"id": "filter-fallback", "uri": "/filtered"},
        {"id": "param", "uri": "/param/:item"},
    ]
    for index in range(6):
        routes.append({"id": f"fan-{index}", "uri": "/fan/*", "priority": index,
                       "vars": [["arg_choose", "==", str(index)]]})
    for route in routes:
        route["plugins"] = {"prometheus": {}}
    fixture.write_text(json.dumps({"upstream": "127.0.0.1:1980", "routes": routes,
                                   "router": "radixtree_uri_with_parameter",
                                   "consumers": [], "global_rules": []}))
    prepare("native", fixture=fixture)
    stop()
    path = ROOT / "conf/nginx.conf"
    text = path.read_text()
    marker = 'require("apisix.native.engine").init_worker()'
    assert text.count(marker) == 1
    path.write_text(text.replace(marker, OBSERVER + marker))
    subprocess.run([NGINX, "-p", str(ROOT), "-c", "conf/nginx.conf"],
                   env=NGINX_ENV, check=True)
    ready("/missing", expected=404)
    for uri, identity in (("/lazy", "lazy"), ("/lazy", "lazy"),
                          ("/filtered?allow=yes", "filter"),
                          ("/filtered?allow=no", "filter-fallback"),
                          ("/param/hello", "param")):
        status, headers, body = request(uri)
        assert status == 200 and headers.get("X-Route-Probe") == identity, (status, headers, body)
        if identity == "param":
            assert headers["X-Matched-Item"] == "hello"
            assert headers["X-Matched-Path"] == "/param/:item"
    for index in range(6):
        status, headers, body = request(f"/fan/test?choose={index}")
        assert status == 200 and headers.get("X-Route-Probe") == f"fan-{index}", (status, headers, body)
    assert request("/fan/test?choose=absent")[0] == 404
    assert request("/missing")[0] == 404
    time.sleep(0.05)
    errors = (ROOT / "logs/error.log").read_text()
    assert "[error]" not in errors and "[alert]" not in errors, errors
    stop()
    print("PASS route allocations:", flags, flush=True)


if __name__ == "__main__":
    previous = os.environ.get("APISIX_CORE_DIRECTIVES")
    try:
        for flags in ("apisix_core_lazy_route_opts off; apisix_core_route_candidates off;",
                      "apisix_core_lazy_route_opts on; apisix_core_route_candidates on;"):
            run(flags)
    finally:
        stop()
        if previous is None:
            os.environ.pop("APISIX_CORE_DIRECTIVES", None)
        else:
            os.environ["APISIX_CORE_DIRECTIVES"] = previous
