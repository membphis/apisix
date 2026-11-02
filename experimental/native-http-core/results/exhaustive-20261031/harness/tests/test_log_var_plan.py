#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Focused observer checks for the opt-in compiled log-variable plan."""
import os
from pathlib import Path
import subprocess
import time

import test_demo as demo
from demo import MODULE


def main():
    previous = os.environ.get("APISIX_CORE_DIRECTIVES", "")
    # This test has its own isolated experiment configuration.
    os.environ["APISIX_CORE_DIRECTIVES"] = "apisix_core_log_var_plan on;"
    try:
        # Optional LLM labels need not be declared in minimal installations.
        minimal = Path("/tmp/apisix-log-plan-minimal.conf")
        minimal.write_text(f"load_module {MODULE};\nerror_log stderr;\n"
                           "pid /tmp/apisix-log-plan-minimal.pid;\nevents {}\n"
                           "http { apisix_core_log_var_plan on; }\n")
        subprocess.run([demo.NGINX, "-t", "-c", str(minimal)],
                       env=demo.NGINX_ENV, check=True)
        demo.log_variable_semantics("native")
        demo.prepare("native")
        demo.stop()
        path = demo.ROOT / "conf/nginx.conf"
        text = path.read_text()
        marker = 'require("apisix.native.engine").init_worker()'
        observer = '''
        local plugin = require("apisix.plugins.prometheus")
        local original_log = plugin.log
        plugin.rewrite = function(conf, ctx)
            -- Read the NGINX indexed slot before response bytes exist, without
            -- admitting it to ctx.var's deliberately persistent Lua cache.
            ctx.early_sent = tonumber(ngx.var.bytes_sent)
            ngx.var.request_type = "plan-type"
            ngx.var.request_llm_model = "plan-request-model"
            ngx.var.llm_model = "plan-model"
        end
        plugin.log = function(conf, ctx)
            if ngx.var.uri == "/plain/log-plan" then
                assert(ctx.var.bytes_sent == tonumber(ngx.var.bytes_sent), "stale bytes_sent slot")
                assert(ctx.var.bytes_sent > ctx.early_sent, "response bytes were not refreshed")
                assert(ctx.var.request_length == tonumber(ngx.var.request_length))
                assert(ctx.var.request_type == "plan-type")
                assert(ctx.var.request_llm_model == "plan-request-model")
                assert(ctx.var.llm_model == "plan-model")
                ngx.log(ngx.WARN, "LOG_VAR_PLAN_OBSERVER_OK")
            end
            return original_log(conf, ctx)
        end
'''
        assert text.count(marker) == 1
        path.write_text(text.replace(marker, observer + marker))
        subprocess.run([demo.NGINX, "-p", str(demo.ROOT), "-c", "conf/nginx.conf"],
                       env=demo.NGINX_ENV, check=True)
        demo.ready()
        for method, body in (("GET", None), ("HEAD", None), ("POST", "payload")):
            assert demo.request("/plain/log-plan", method=method, body=body)[0] == 200
        log = demo.ROOT / "logs/error.log"
        for _ in range(100):
            text = log.read_text()
            if text.count("LOG_VAR_PLAN_OBSERVER_OK") == 3:
                break
            time.sleep(0.01)
        assert text.count("LOG_VAR_PLAN_OBSERVER_OK") == 3, text
        assert "[error]" not in text and "[alert]" not in text, text
        # An operator's existing indexed use intentionally changes bytes_sent
        # caching. The plan must preserve that too, rather than forcing a refresh.
        demo.stop()
        indexed = path.read_text().replace(
            "http {", 'http {\nmap $bytes_sent $plan_unused { default ""; }', 1)
        indexed = indexed.replace(
            'ctx.var.bytes_sent > ctx.early_sent, "response bytes were not refreshed"',
            'ctx.var.bytes_sent == ctx.early_sent, "existing indexed cache was bypassed"')
        indexed = indexed.replace("LOG_VAR_PLAN_OBSERVER_OK", "LOG_VAR_PLAN_INDEXED_OK")
        path.write_text(indexed)
        subprocess.run([demo.NGINX, "-p", str(demo.ROOT), "-c", "conf/nginx.conf"],
                       env=demo.NGINX_ENV, check=True)
        demo.ready()
        for method, body in (("GET", None), ("HEAD", None), ("POST", "payload")):
            assert demo.request("/plain/log-plan", method=method, body=body)[0] == 200
        for _ in range(100):
            text = log.read_text()
            if text.count("LOG_VAR_PLAN_INDEXED_OK") == 3:
                break
            time.sleep(0.01)
        assert text.count("LOG_VAR_PLAN_INDEXED_OK") == 3, text
        assert "[error]" not in text and "[alert]" not in text, text
        print("PASS log variable plan: optional declarations, false/cache/getter timing, "
              "unindexed and already-indexed bytes_sent, labels")
    finally:
        demo.stop()
        if previous:
            os.environ["APISIX_CORE_DIRECTIVES"] = previous
        else:
            os.environ.pop("APISIX_CORE_DIRECTIVES", None)


if __name__ == "__main__":
    main()
