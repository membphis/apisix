#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Callback table-handle/phase-index checks, including mutations and legal yield."""
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import subprocess
import time

import test_demo as demo


def check(directives):
    os.environ["APISIX_CORE_DIRECTIVES"] = directives
    demo.prepare("native", benchmark=True)
    demo.stop()
    path = demo.ROOT / "conf/nginx.conf"
    text = path.read_text()
    marker = 'require("apisix.native.engine").init_worker()'
    observer = '''
        local plugin = require("apisix.plugins.key-auth")
        local original_rewrite = plugin.rewrite
        local function observe(phase, ctx)
            assert(ctx == ngx.ctx.api_ctx)
            assert(ctx._plugin_name == "key-auth", "wrong current plugin")
            assert(ctx.conf_type and ctx.conf_type ~= "poison", "metadata not restored")
            assert(ctx.conf_id and ctx.conf_id ~= "poison", "id not restored")
            ctx.phase_probe = ctx.phase_probe or {}
            ctx.phase_probe[#ctx.phase_probe + 1] = phase
        end
        local function poison(ctx)
            ctx.conf_type = "poison"
            ctx.conf_id = "poison"
            ctx._plugin_name = "poison"
        end
        plugin.rewrite = function(conf, ctx)
            observe("rewrite", ctx)
            ngx.sleep(0.002)
            assert(ctx == ngx.ctx.api_ctx and ctx._plugin_name == "key-auth")
            local code, body = original_rewrite(conf, ctx)
            poison(ctx)
            return code, body
        end
        plugin.access = function(conf, ctx)
            observe("access", ctx)
            ngx.sleep(0.002)
            assert(ctx == ngx.ctx.api_ctx and ctx._plugin_name == "key-auth")
            poison(ctx)
        end
        plugin.before_proxy = function(conf, ctx)
            observe("before_proxy", ctx)
            poison(ctx)
        end
        plugin.log = function(conf, ctx)
            observe("log", ctx)
            assert(table.concat(ctx.phase_probe, ",") == "rewrite,access,before_proxy,log")
            poison(ctx)
        end
'''
    assert text.count(marker) == 1
    text = text.replace(marker, observer + marker)
    marker = 'require("apisix.native.runtime").run("log")'
    trailer = '''
                if ngx.var.uri == "/auth/phase-context" then
                    local ctx = ngx.ctx.api_ctx
                    assert(ctx._plugin_name == nil)
                    assert(ctx.conf_type ~= "poison" and ctx.conf_id ~= "poison")
                    ngx.log(ngx.WARN, "PHASE_CONTEXT_OK")
                end
'''
    # The generated template has business and named error locations. Each log
    # entry needs the same observer guard; only the requested URI is checked.
    assert text.count(marker) >= 1
    path.write_text(text.replace(marker, marker + trailer))
    subprocess.run([demo.NGINX, "-p", str(demo.ROOT), "-c", "conf/nginx.conf"],
                   env=demo.NGINX_ENV, check=True)
    try:
        demo.ready()
        with ThreadPoolExecutor(max_workers=12) as pool:
            replies = list(pool.map(lambda _: demo.request("/auth/phase-context",
                                  headers={"apikey": "demo-key"})[0], range(24)))
        assert replies == [200] * 24, replies
        log = demo.ROOT / "logs/error.log"
        for _ in range(100):
            text = log.read_text()
            if text.count("PHASE_CONTEXT_OK") == 24:
                break
            time.sleep(0.01)
        assert text.count("PHASE_CONTEXT_OK") == 24, text
        assert "[error]" not in text and "[alert]" not in text, text
        print("PASS phase context/index: mutation restoration, plugin-name timing, concurrent yield:", directives)
    finally:
        demo.stop()


if __name__ == "__main__":
    previous = os.environ.get("APISIX_CORE_DIRECTIVES", "")
    try:
        check("")
        check("apisix_core_phase_context on; apisix_core_phase_index on;")
    finally:
        os.environ["APISIX_CORE_DIRECTIVES"] = previous
