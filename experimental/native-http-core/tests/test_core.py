#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run inside the pinned OpenResty image; no original APISIX core is loaded."""
import concurrent.futures
import copy
import http.client
import json
import os
import re
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import threading
import time
from expressions import cases as expression_cases

HERE = Path(__file__).resolve().parent.parent
PREFIX = Path("/tmp/apisix-http-core-test")
NGINX = "/usr/local/openresty/nginx/sbin/nginx"
PROFILE = os.environ.get("APISIX_CORE_PROFILE", "default")
NGINX_ENV = os.environ.copy()
if "address" in os.environ.get("APISIX_CORE_SANITIZER", ""):
    NGINX_ENV["LD_PRELOAD"] = subprocess.check_output(["gcc", "-print-file-name=libasan.so"], text=True).strip()
    # The runtime and its other DSOs are not instrumented; test native accesses.
    NGINX_ENV["ASAN_OPTIONS"] = "detect_leaks=0:abort_on_error=1"
NGINX_ENV["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"


def request(path, method="GET", headers=None, port=18880, body=None):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    conn.request(method, path, body=body, headers=headers or {})
    resp = conn.getresponse()
    result = resp.status, dict(resp.getheaders()), resp.read().decode()
    conn.close()
    return result


def main():
    dns = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    dns.bind(("127.0.0.1", 18885))
    def dns_loop():
        while True:
            try:
                query, remote = dns.recvfrom(4096)
            except OSError:
                return
            end = 12
            labels = []
            while query[end]:
                length = query[end]
                labels.append(query[end + 1:end + 1 + length].decode())
                end += length + 1
            end += 5
            name = ".".join(labels)
            answer = name == "backend.native.test"
            header = query[:2] + struct.pack("!HHHHH", 0x8180 if answer else 0x8183, 1, int(answer), 0, 0)
            response = header + query[12:end]
            if answer:
                response += b"\xc0\x0c" + struct.pack("!HHIH", 1, 1, 1, 4) + socket.inet_aton("127.0.0.1")
            time.sleep(0.002)
            dns.sendto(response, remote)
    threading.Thread(target=dns_loop, daemon=True).start()
    shutil.rmtree(PREFIX, ignore_errors=True)
    (PREFIX / "logs").mkdir(parents=True)
    (PREFIX / "lua/apisix/native").mkdir(parents=True)
    (PREFIX / "lua/apisix/plugins").mkdir(parents=True)
    shutil.copyfile(HERE / "runtime.lua", PREFIX / "lua/apisix/native/runtime.lua")
    shutil.copyfile(HERE.parent.parent / "apisix/plugins/key-auth.lua", PREFIX / "lua/apisix/plugins/key-auth.lua")
    shutil.copytree(HERE.parent.parent / "apisix/plugins/limit-count", PREFIX / "lua/apisix/plugins/limit-count")
    shutil.copyfile(HERE.parent.parent / "apisix/plugins/limit-count.lua", PREFIX / "lua/apisix/plugins/limit-count.lua")
    shutil.copyfile(HERE.parent.parent / "apisix/plugins/workflow.lua", PREFIX / "lua/apisix/plugins/workflow.lua")
    shutil.copytree(HERE.parent.parent / "apisix/plugins/prometheus", PREFIX / "lua/apisix/plugins/prometheus")
    shutil.copyfile(HERE.parent.parent / "apisix/plugins/prometheus.lua", PREFIX / "lua/apisix/plugins/prometheus.lua")
    shutil.copytree(HERE.parent.parent / "apisix/plugins/grpc-transcode", PREFIX / "lua/apisix/plugins/grpc-transcode")
    shutil.copyfile(HERE / "tests/probe.lua", PREFIX / "lua/probe.lua")
    shutil.copyfile(HERE / "tests/sdk.lua", PREFIX / "lua/sdk.lua")
    for path in PREFIX.rglob("*"):
        path.chmod(0o755 if path.is_dir() else 0o644)
    config = {
        "plugins": ["global", "route", "prefer", "tail", "key-auth", "limit-count", "prometheus", "workflow"],
        "local_conf": {"deployment": {"role": "data_plane", "config_provider": "json"},
                       "dns_resolver": [["127.0.0.1", 18885]],
                       "apisix": {"enable_admin": False, "proxy_mode": "http", "tracing": True},
                       "nginx_config": {"worker_processes": 1, "event": {"worker_connections": 1024}},
                       "plugin_attr": {"prometheus": {"export_uri": "/apisix/prometheus/metrics",
                           "enable_export_server": True, "metric_prefix": "apisix_", "refresh_interval": 1,
                           "remove_expired_keys_interval": 3600}}},
        "services": [{"id": "s", "plugins": {"route": {"tag": "service"}, "prefer": {"tag": "service-prefer"}}}],
        "plugin_configs": [{"id": "p", "plugins": {"tail": {"tag": "plugin-config"}, "route": {"tag": "plugin-config-route"}}}],
        "consumer_groups": [{"id": "cg", "plugins": {"prefer": {"tag": "consumer-group"}}}],
        "consumers": [{"username": "alice", "plugins": {"key-auth": {"key": "alice-secret"}}},
                      {"username": "bob", "group_id": "cg", "plugins": {
                          "key-auth": {"key": "bob-secret"}, "route": {"tag": "consumer-override"},
                          "tail": {"tag": "consumer-new"}}}],
        "global_rules": [{"id": "g", "plugins": {
            "global": {"tag": "global"}, "prefer": {"tag": "global-prefer"}, "prometheus": {}}}],
        "routes": [
            {"id": "exact", "uri": "/exact", "plugins": {
                "route": {"yield": True}, "prefer": {"tag": "route-prefer"}}},
            {"id": "wild", "uri": "/*", "priority": 100, "vars": [["uri", "~=", "/404"]], "plugins": {
                "tail": {"tag": "wild", "_meta": {"filter": [["arg_active", "==", "yes"]]}}}},
            {"id": "reject", "uri": "/reject", "plugins": {"route": {"reject": True}}},
            {"id": "custom", "uri": "/custom", "plugins": {"route": {"reject": True,
                "_meta": {"error_response": {"message": "custom response"}}}}},
            {"id": "error", "uri": "/error", "plugins": {"route": {"fail": True}}},
            {"id": "metadata", "uri": "/metadata", "plugins": {
                "route": {"_meta": {"disable": True}},
                "tail": {"tag": "meta", "_meta": {"priority": 2000,
                    "pre_function": "function(conf, ctx) ctx.pre = (ctx.pre or 0) + 1 end"}}}},
            {"id": "host", "uri": "/host", "host": "a.example", "methods": ["POST"],
                "remote_addr": "127.0.0.0/8", "plugins": {"route": {"tag": "host"}}},
            {"id": "sdk", "uri": "/sdk", "plugins": {"route": {
                "sdk": True, "json": True, "body": '{"answer":42}'}}},
            {"id": "filter", "uri": "/filtered", "priority": 200,
             "filter_func": "function(vars, opts, ctx, match_opts) ngx.sleep(0.001); assert(opts == match_opts and vars == ctx.var); return vars.arg_allow == 'yes' end"},
            {"id": "filter-default", "uri": "/filtered"},
            {"id": "merge", "uri": "/merge", "service_id": "s", "plugin_config_id": "p",
             "plugins": {"route": {"tag": "route-override"}}},
            {"id": "auth", "uri": "/auth", "plugins": {"key-auth": {}}},
            {"id": "auth-merge", "uri": "/authmerge", "plugins": {
                "key-auth": {"hide_credentials": True}, "route": {"tag": "original"}}},
            {"id": "limit", "uri": "/limit*", "plugins": {"limit-count": {
                "count": 5, "time_window": 60, "rejected_code": 429, "show_limit_quota_header": True}}},
            {"id": "combined", "uri": "/combined*", "plugins": {"key-auth": {}, "limit-count": {
                "count": 5, "time_window": 60, "rejected_code": 429, "show_limit_quota_header": True}}},
            {"id": "proxy", "uri": "/proxy*", "upstream": {
                "nodes": {"127.0.0.1:18881": 1}, "type": "roundrobin", "pass_host": "rewrite",
                "upstream_host": "service.internal", "timeout": {"connect": 0.3, "read": 0.3, "send": 0.3}},
             "plugins": {"key-auth": {"hide_credentials": True}}},
            {"id": "retry", "uri": "/retry*", "upstream": {"nodes": [
                {"host": "127.0.0.1", "port": 18889, "weight": 1, "priority": 10},
                {"host": "127.0.0.1", "port": 18881, "weight": 1, "priority": 0}], "retries": 1}},
            {"id": "weighted", "uri": "/weighted*", "upstream": {
                "nodes": {"127.0.0.1:18881": 1, "127.0.0.1:18883": 3}}},
            {"id": "unavailable", "uri": "/unavailable*", "upstream": {
                "nodes": {"127.0.0.1:18889": 1}, "retries": 0}},
            {"id": "dns", "uri": "/dns*", "upstream": {
                "nodes": {"backend.native.test:18881": 1}}},
        ],
    }
    options = config["local_conf"]["apisix"]
    if PROFILE == "servlet":
        options["normalize_uri_like_servlet"] = True
    elif PROFILE == "tail":
        options["delete_uri_tail_slash"] = True
    elif PROFILE == "encoded":
        options["match_uri_encoded_slash"] = True
        config["router"] = "radixtree_uri_with_parameter"
        config["routes"].append({"id": "encoded", "uri": "/wire/:item",
            "vars": [["uri", "==", "/wire/a%2Fb"]],
            "filter_func": "function(vars, opts, ctx) return vars.uri == '/wire/a%2Fb' end",
            "upstream": {"nodes": {"127.0.0.1:18881": 1}, "pass_host": "node"}, "plugins": {"route": {"wire": True}}})
    elif PROFILE == "trusted":
        options["trusted_addresses"] = ["127.0.0.0/8", "::1"]
    elif PROFILE == "untrusted":
        options["trusted_addresses"] = []
    else:
        assert PROFILE == "default", PROFILE
    config["routes"].append({"id": "chash", "uri": "/chash*", "upstream": {
        "type": "chash", "hash_on": "header", "key": "X-Tenant",
        "nodes": {"127.0.0.1:18881": 2, "127.0.0.1:18883": 6}}})
    chash_source = next(route for route in config["routes"] if route["id"] == "chash")
    for suffix, hash_on, key in (("var", "vars", "arg_tenant"), ("cookie", "cookie", "tenant"),
                                 ("consumer", "consumer", "unused"),
                                 ("combination", "vars_combinations", "$arg_tenant:$http_x_tenant"),
                                 ("dns", "header", "X-Tenant")):
        derived = copy.deepcopy(chash_source)
        derived["id"] = "chash-" + suffix
        derived["uri"] = "/chash-" + suffix + "*"
        derived["upstream"].update(hash_on=hash_on, key=key)
        if suffix == "consumer":
            derived["plugins"] = {"key-auth": {}}
        elif suffix == "dns":
            derived["upstream"]["nodes"] = {"backend.native.test:18881": 2, "127.0.0.1:18883": 6}
        config["routes"].append(derived)
    retry_hash = copy.deepcopy(chash_source)
    retry_hash.update(id="chash-retry", uri="/chash-retry*")
    retry_hash["upstream"].update(nodes={"127.0.0.1:18889": 1, "127.0.0.1:18881": 1}, retries=1)
    config["routes"].append(retry_hash)
    config["routes"].append({"id": "snapshot", "uri": "/snapshot",
                              "plugins": {"route": {"tag": "old", "hold": 0.4}}})
    config["routes"].append({"id": "script", "uri": "/script", "script": r"""
        ngx.sleep(0.001)
        return setmetatable({access = function(ctx)
            assert(ctx == ngx.ctx.api_ctx and ctx.script_obj)
            ngx.sleep(0.001)
            ctx.script_trace = {"access"}
            return 418, "ignored by original script framework"
        end}, {__index = function(_, phase)
            return function(ctx)
                ctx.script_trace[#ctx.script_trace + 1] = phase
                if phase == "header_filter" then ngx.header["X-Script"] = "native-dispatch" end
                if phase == "log" then
                    ngx.shared.probes:set("trace:script", require("apisix.core.json").encode(ctx.script_trace))
                end
                return 418, "ignored"
            end
        end})
    """})
    config["routes"].append({"id": "workflow", "uri": "/workflow*", "plugins": {
        "workflow": {"rules": [
            {"case": [["arg_block", "==", "yes"]], "actions": [["return", {"code": 403}]]},
            {"actions": [["limit-count", {"count": 2, "time_window": 60, "rejected_code": 429,
                                         "show_limit_quota_header": True}]]}]},
        "limit-count": {"count": 1, "time_window": 60, "rejected_code": 429,
                        "show_limit_quota_header": True}}})
    config["routes"].append({"id": "wire", "uri": "/wire*",
        "upstream": {"nodes": {"127.0.0.1:18881": 1}, "pass_host": "node"},
        "plugins": {"route": {"wire": True}}})
    expressions = expression_cases()
    for i, (item, _) in enumerate(expressions):
        config["routes"].append({"id": "expr-" + str(i), "uri": "/expr/" + str(i),
                                  "vars": item["rule"]})
    (PREFIX / "config.json").write_text(json.dumps(config))
    nginx_conf = '''
load_module /opt/apisix-core-module/lib/ngx_http_apisix_core_module.so;
worker_processes 1;
pid logs/nginx.pid;
error_log logs/error.log info;
events { worker_connections 1024; }
http {
    lua_package_path "__PREFIX__/lua/?.lua;/usr/local/apisix/deps/share/lua/5.1/?.lua;/usr/local/apisix/deps/share/lua/5.1/?/init.lua;;";
    lua_package_cpath "/usr/local/apisix/deps/lib64/lua/5.1/?.so;/usr/local/apisix/deps/lib/lua/5.1/?.so;;";
    variables_hash_bucket_size 128;
    lua_shared_dict probes 1m;
    lua_shared_dict lrucache-lock 1m;
    lua_shared_dict plugin-limit-count 1m;
    lua_shared_dict plugin-limit-count-reset-header 1m;
    lua_shared_dict prometheus-metrics 10m;
    lua_shared_dict prometheus-cache 2m;
    lua_shared_dict upstream-healthcheck 1m;
    upstream native_http_core {
        server 127.0.0.1:1;
        balancer_by_lua_block { require("apisix.native.runtime").balance() }
        keepalive 16;
    }
    init_by_lua_block {
        local probe = require("probe")
        for i, name in ipairs({"global", "route", "prefer", "tail"}) do
            package.preload["apisix.plugins." .. name] = function()
                return probe.new(name, 1000 - i, name == "prefer" and "prefer_route" or nil)
            end
        end
        table.insert(package.loaders, 2, function(name)
            if name:match("^apisix%.") and not name:match("^apisix%.native%.")
               and not name:match("^apisix%.plugins%.") then
                error("original APISIX core forbidden: " .. name)
            end
        end)
        require("apisix.native.engine").init("__PREFIX__/config.json")
        require("sdk")
    }
    init_worker_by_lua_block { require("apisix.native.engine").init_worker() }
    exit_worker_by_lua_block { require("apisix.native.engine").shutdown() }
    server {
        listen 127.0.0.1:18881;
        location /wire { content_by_lua_block {
            ngx.print(require("cjson.safe").encode({request_uri = ngx.var.request_uri,
                headers = ngx.req.get_headers(),
                view = require("cjson.safe").decode(ngx.var.http_x_core_view)}))
        } }
        location /proxy { content_by_lua_block {
            ngx.req.read_body()
            ngx.print(require("cjson.safe").encode({uri = ngx.var.uri, args = ngx.var.args,
                host = ngx.var.http_host, consumer = ngx.var.http_x_consumer_username,
                apikey = ngx.var.http_apikey, body = ngx.req.get_body_data()}))
        } }
        location / { return 200 'upstream'; }
    }
    server { listen 127.0.0.1:18883; location / { return 200 'second'; } }
    server {
        listen 127.0.0.1:18884;
        location / {
            apisix_core_proxy on;
            access_by_lua_block { require("apisix.native.runtime").run("access") }
            header_filter_by_lua_block { require("apisix.native.runtime").run("header_filter") }
            body_filter_by_lua_block { require("apisix.native.runtime").run("body_filter") }
            log_by_lua_block { require("apisix.native.runtime").run("log") }
            proxy_http_version 1.1;
            proxy_set_header Connection '';
            proxy_set_header Host $upstream_host;
            proxy_set_header X-Real-IP $remote_addr;
            proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
            proxy_next_upstream error timeout;
            proxy_pass $upstream_scheme://native_http_core$upstream_uri;
        }
    }
    server {
        listen 127.0.0.1:18882;
        location /chash-retry-reference { content_by_lua_block {
            local ring = require("resty.chash"):new({["127.0.0.1" .. string.char(0) .. "18889"] = 1,
                                                    ["127.0.0.1" .. string.char(0) .. "18881"] = 1})
            ngx.print(ring:find(ngx.var.arg_key):sub(-5))
        } }
        location /chash-reference { content_by_lua_block {
            local ring = package.loaded["native_chash_reference"]
            if not ring then
                ring = require("resty.chash"):new({["127.0.0.1\\0" .. "18881"] = 1,
                                                  ["127.0.0.1\\0" .. "18883"] = 3})
                package.loaded["native_chash_reference"] = ring
            end
            local server = ring:find(ngx.var.arg_key or "127.0.0.1")
            ngx.print(server:sub(-5) == "18881" and "upstream" or "second")
        } }
        location /config { content_by_lua_block {
            local config = require("apisix.native.config")
            local reader = package.loaded["native_reader_probe"]
            if not reader then
                reader = require("apisix.core.config").new("/routes", {automatic = true, filter = function(row)
                    if row.value.id == "snapshot" and row.value.plugins.route.tag == "reader-reject" then
                        error("reader rejected candidate")
                    end
                    row.native_filtered = true
                end})
                package.loaded["native_reader_probe"] = reader
            end
            if ngx.var.arg_close then reader:close() end
            local ok, err
            if ngx.var.arg_action == "reload" then ok, err = config.reload() end
            ngx.print(require("cjson.safe").encode({ok = ok, result = err, stats = config.stats(),
                ready = ngx.shared.probes:get("trace:snapshot-ready"), reader_version = reader.conf_version,
                reader_tag = reader:get("snapshot").value.plugins.route.tag,
                reader_filtered = reader:get("snapshot").native_filtered}))
        } }
        location /expr-reference { content_by_lua_block {
            ngx.req.read_body()
            local json = require("cjson.safe")
            local input = json.decode(ngx.req.get_body_data())
            local expression, err = require("resty.expr.v1").new(input.rule)
            assert(expression, err)
            ngx.print(json.encode({value = expression:eval(input.ctx)}))
        } }
        location /apisix/prometheus/metrics { content_by_lua_block {
            require("apisix.plugins.prometheus.exporter").export_metrics()
        } }
        location /trace { content_by_lua_block {
            ngx.print(ngx.shared.probes:get("trace:" .. ngx.var.arg_path) or "null")
        } }
        location /stats { content_by_lua_block {
            local out = {}
            for _, key in ipairs(ngx.shared.probes:get_keys()) do
                if not key:match("^trace:") then out[key] = ngx.shared.probes:get(key) end
            end
            ngx.print(require("cjson.safe").encode(out))
        } }
    }
    server {
        listen 127.0.0.1:18880;
        location / {
            access_by_lua_block { require("apisix.native.runtime").run("access") }
            header_filter_by_lua_block { require("apisix.native.runtime").run("header_filter") }
            body_filter_by_lua_block { require("apisix.native.runtime").run("body_filter") }
            log_by_lua_block { require("apisix.native.runtime").run("log") }
            content_by_lua_block {
                local engine = require("apisix.native.engine")
                local ctx = engine.context()
                ngx.print(engine.encode({id = ctx.route_id, trace = ctx.trace,
                                        pre = ctx.pre, matched = ctx.curr_req_matched, script_trace = ctx.script_trace}))
            }
        }
    }
}
'''.replace("__PREFIX__", str(PREFIX))
    (PREFIX / "nginx.conf").write_text(nginx_conf)
    subprocess.run([NGINX, "-p", str(PREFIX), "-c", "nginx.conf", "-t"], env=NGINX_ENV, check=True)
    subprocess.run([NGINX, "-p", str(PREFIX), "-c", "nginx.conf"], env=NGINX_ENV, check=True)
    checks = 0
    try:
        for _ in range(100):
            try:
                if request("/stats", port=18882)[0] == 200:
                    break
            except (OSError, http.client.HTTPException):
                pass
            time.sleep(0.02)
        incoming = {"Host": "client.example:4567", "X-Forwarded-Proto": "https",
                    "X-Forwarded-Host": "original.example", "X-Forwarded-Port": "9443",
                    "Forwarded": "for=client;proto=https", "X-Forwarded-For": "192.0.2.1"}
        paths = [("/wire/plain?q=a%20b", "/wire/plain?q=a%20b", "/wire/plain"),
                 ("/wire/a%2fb?q=x%26y", "/wire/a%2fb?q=x%26y", "/wire/a/b"),
                 ("/wire//a", "/wire//a", "/wire/a"),
                 ("/wire/a/../b", "/wire/b", "/wire/b"),
                 ("/wire/%2E%2E/wire/b", "/wire/b", "/wire/b"),
                 ("/wire/percent%252f", "/wire/percent%252f", "/wire/percent%2f"),
                 ("/wire/end/", "/wire/end/", "/wire/end/"),
                 ("/wire/path;x=1?q=two", "/wire/path;x=1?q=two", "/wire/path;x=1")]
        if PROFILE == "servlet":
            paths = [(path, forwarded.replace("/wire//a", "/wire/a").replace("a%2fb", "a/b"),
                      uri.split(";")[0]) for path, forwarded, uri in paths]
        for path, forwarded, uri in paths:
            status, _, body = request(path, headers=incoming, port=18884)
            assert status == 200, (PROFILE, path, status, body)
            wire = json.loads(body)
            view, headers = wire["view"], wire["headers"]
            assert wire["request_uri"] == forwarded, (PROFILE, path, wire)
            if PROFILE == "tail" and uri.endswith("/"):
                uri = uri[:-1]
            assert view["uri"] == uri and view["real_request_uri"] == path, (PROFILE, path, view)
            assert view["request_uri"] == uri + (("?" + path.split("?", 1)[1]) if "?" in path else ""), view
            assert view["request_line"] == "GET " + path + " HTTP/1.1", view
            assert headers["host"] == "127.0.0.1:18881", headers
            assert view["original_x_forwarded_for"] == "192.0.2.1", view
            assert view["original_x_forwarded_host"] == "original.example", view
            assert view["original_x_forwarded_proto"] == "https", view
            assert view["original_x_forwarded_port"] == "9443", view
            assert view["original_forwarded"] == "for=client;proto=https", view
            if PROFILE == "trusted":
                assert headers["x-forwarded-proto"] == "https", headers
                assert headers["x-forwarded-host"] == "original.example", headers
                assert headers["x-forwarded-port"] == "9443", headers
                assert headers["forwarded"] == incoming["Forwarded"], headers
            else:
                assert headers["x-forwarded-proto"] == "http", headers
                assert headers["x-forwarded-host"] == "client.example:4567", headers
                assert headers["x-forwarded-port"] == "4567" and "forwarded" not in headers, headers
            if PROFILE == "untrusted":
                assert headers["x-forwarded-for"] == "127.0.0.1" and "http_x_forwarded_for" not in view, (headers, view)
            else:
                assert headers["x-forwarded-for"] == "192.0.2.1, 127.0.0.1", headers
            if PROFILE == "encoded" and path.startswith("/wire/a%2fb"):
                assert view["matched"].get("item") == "a%2Fb", view
            checks += 1
        if PROFILE == "servlet":
            for path in ("/wire/.;x/a", "/wire/..;x/a", "/wire/;x/a",
                         "/wire/%252e;x/a", "/wire/%252E%252e;x/a"):
                status, _, body = request(path, port=18884)
                assert status == 400, (path, status, body)
                checks += 1
        status, headers, body = request("/exact")
        assert status == 200, (status, body)
        data = json.loads(body)
        expected = [f"{name}:{phase}" for phase in ("rewrite", "access", "before_proxy")
                    for name in ("global", "route", "route-prefer")]
        assert data["trace"] == expected, data
        assert data["id"] == "exact", data
        assert headers["X-Probe-prefer"] == "route-prefer", headers
        checks += 1
        status, headers, body = request("/404")
        assert status == 404 and json.loads(body)["error_msg"] == "404 Route Not Found", (status, body)
        assert headers["X-Probe-global"] == "global", headers
        checks += 1
        for allow, expected_id in (("yes", "filter"), ("no", "filter-default")):
            status, _, body = request("/filtered?allow=" + allow)
            assert status == 200 and json.loads(body)["id"] == expected_id, (status, body)
            checks += 1
        status, _, body = request("/merge")
        data = json.loads(body)
        assert "route-override:rewrite" in data["trace"] and "plugin-config:rewrite" in data["trace"], data
        assert "service-prefer:rewrite" in data["trace"] and not any(v.startswith("service:") for v in data["trace"]), data
        checks += 1
        for key, expected_status in ((None, 401), ("wrong", 401), ("alice-secret", 200)):
            status, _, body = request("/auth", headers={"apikey": key} if key else {})
            assert status == expected_status, (status, body)
            checks += 1
        status, headers, body = request("/script")
        assert status == 200 and json.loads(body)["script_trace"] == ["access", "before_proxy"], (status, body)
        assert headers["X-Script"] == "native-dispatch", headers
        script_trace = json.loads(request("/trace?path=script", port=18882)[2])
        assert script_trace[:3] == ["access", "before_proxy", "header_filter"] and script_trace[-1] == "log", script_trace
        body_phases = script_trace[3:-1]
        assert body_phases and len(body_phases) % 2 == 0, script_trace
        assert body_phases == ["body_filter", "delayed_body_filter"] * (len(body_phases) // 2), script_trace
        checks += 1
        status, _, body = request("/workflow/test?block=yes")
        assert status == 403 and json.loads(body)["error_msg"] == "rejected by workflow", (status, body)
        workflow = [request("/workflow/test") for _ in range(3)]
        assert [item[0] for item in workflow] == [200, 200, 429], workflow
        assert workflow[0][1]["X-RateLimit-Limit"] == "2", workflow[0]
        checks += 4
        status, _, body = request("/authmerge", headers={"apikey": "bob-secret"})
        data = json.loads(body)
        assert status == 200 and "original:rewrite" in data["trace"], (status, data)
        assert "consumer-override:rewrite" not in data["trace"] and "consumer-override:access" in data["trace"], data
        assert "consumer-new:rewrite" in data["trace"] and "consumer-group:rewrite" in data["trace"], data
        checks += 1
        for path, headers in (("/limit/test", {}), ("/combined/test", {"apikey": "alice-secret"})):
            if headers:
                assert request(path)[0] == 401
            responses = [request(path, headers=headers) for _ in range(8)]
            assert [item[0] for item in responses] == [200] * 5 + [429] * 3, responses
            assert responses[0][1]["X-RateLimit-Limit"] == "5", responses[0]
            assert responses[-1][1]["X-RateLimit-Remaining"] == "0", responses[-1]
            checks += len(responses)
        status, _, body = request("/proxy/test?a=x%20y", "POST", {"apikey": "alice-secret"},
                                  port=18884, body="proxy request")
        assert status == 200, (status, body)
        data = json.loads(body)
        assert data == {"uri": "/proxy/test", "args": "a=x%20y", "host": "service.internal",
                        "consumer": "alice", "body": "proxy request"}, data
        assert request("/proxy/test", port=18884)[0] == 401
        assert request("/retry/test", port=18884)[2] == "upstream"
        assert request("/unavailable/test", port=18884)[0] == 502
        counts = {"upstream": 0, "second": 0}
        for _ in range(40):
            status, _, body = request("/weighted/test", port=18884)
            assert status == 200, (status, body)
            counts[body] += 1
        assert counts == {"upstream": 10, "second": 30}, counts
        checks += 44
        destinations = set()
        for i in range(64):
            key = "tenant-" + str(i)
            expected_peer = request("/chash-reference?key=" + key, port=18882)[2]
            status, _, body = request("/chash/test", headers={"X-Tenant": key}, port=18884)
            assert status == 200 and body == expected_peer, (key, status, body, expected_peer)
            destinations.add(body)
            checks += 1
        assert destinations == {"upstream", "second"}, destinations
        for suffix, key, headers, query in (
                ("var", "variable", {}, "?tenant=variable"),
                ("cookie", "cookie-value", {"Cookie": "tenant=cookie-value"}, ""),
                ("consumer", "alice", {"apikey": "alice-secret"}, ""),
                ("combination", "combined:header", {"X-Tenant": "header"}, "?tenant=combined"),
                ("dns", "dns-key", {"X-Tenant": "dns-key"}, "")):
            expected_peer = request("/chash-reference?key=" + key, port=18882)[2]
            status, _, body = request("/chash-" + suffix + "/test" + query, headers=headers, port=18884)
            assert status == 200 and body == expected_peer, (suffix, status, body, expected_peer)
            checks += 1
        for i in range(100):
            key = "retry-key-" + str(i)
            if request("/chash-retry-reference?key=" + key, port=18882)[2] == "18889":
                break
        else:
            raise AssertionError("reference ring did not select the failing peer")
        assert request("/chash-retry/test", headers={"X-Tenant": key}, port=18884)[2] == "upstream"
        checks += 1
        assert request("/chash/test", port=18884)[2] == request("/chash-reference", port=18882)[2]
        checks += 1
        for _ in range(4):
            status, _, body = request("/dns/test", port=18884)
            assert status == 200 and body == "upstream", (status, body)
            checks += 1
        for i, (item, query) in enumerate(expressions):
            expected_match = json.loads(request("/expr-reference", "POST", port=18882,
                                          body=json.dumps(item))[2])["value"]
            status, _, body = request("/expr/" + str(i) + "?" + query)
            actual = json.loads(body)["id"] == "expr-" + str(i)
            assert actual == expected_match, (item, actual, expected_match, status, body)
            checks += 1
        time.sleep(0.02)
        trace = json.loads(request("/trace?path=/exact", port=18882)[2])
        for phase in ("header_filter", "body_filter", "delayed_body_filter", "log"):
            assert f"route:{phase}" in trace, trace
        checks += 1
        for path, message in (("/reject", "probe rejected"), ("/custom", "custom response")):
            status, _, body = request(path)
            assert status == 418 and json.loads(body)["message"] == message, (status, body)
            checks += 1
        assert request("/error")[0] == 500
        checks += 1
        data = json.loads(request("/metadata")[2])
        assert data["pre"] == 3 and not any(s.startswith("route:") for s in data["trace"]), data
        checks += 1
        for active in ("no", "yes"):
            data = json.loads(request("/anything?active=" + active)[2])
            assert ("wild:rewrite" in data["trace"]) == (active == "yes"), data
            checks += 1
        data = json.loads(request("/host", "POST", {"Host": "a.example"})[2])
        assert data["id"] == "host", data
        checks += 1
        assert json.loads(request("/host", "GET", {"Host": "a.example"})[2])["id"] == "wild"
        checks += 1
        status, _, body = request("/sdk?a=x%20y&a=z", "POST",
                                  {"X-Test": "original", "Content-Type": "application/json"},
                                  body='{"answer":42}')
        assert status == 200 and json.loads(body)["id"] == "sdk", (status, body)
        checks += 1
        with concurrent.futures.ThreadPoolExecutor(max_workers=16) as executor:
            results = list(executor.map(lambda _: request("/exact"), range(64)))
        assert all(status == 200 and json.loads(body)["trace"] == expected
                   for status, _, body in results), results[:1]
        checks += 64
        stats = json.loads(request("/stats", port=18882)[2])
        assert stats.pop("sdk-checks") == 32, stats
        assert stats["timer-fired"] == 2 and stats["timer-finished"] == 2, stats
        assert all(value == 2 for value in stats.values()), stats
        checks += 1
        log = (PREFIX / "logs/error.log").read_text()
        assert "probe failure" in log
        assert not any(marker in log for marker in ("signal 11", "alert]", "forbidden", "C-call boundary",
                       "AddressSanitizer", "UndefinedBehaviorSanitizer")), log
        assert not re.search(r"\.[ch]:\d+(?::\d+)?: runtime error:", log), log
        for _ in range(100):
            status, _, metrics = request("/apisix/prometheus/metrics", port=18882)
            if status == 200 and 'code="404"' in metrics and 'code="401"' in metrics and 'code="429"' in metrics:
                break
            time.sleep(0.05)
        assert status == 200 and 'code="404"' in metrics and 'code="401"' in metrics and 'code="429"' in metrics, (status, metrics)
        assert 'response_source="upstream"' in metrics and 'response_source="nginx"' in metrics, metrics
        checks += 1
        def reload_config(value, raw=False):
            replacement = PREFIX / "config.next.json"
            replacement.write_text(value if raw else json.dumps(value))
            replacement.replace(PREFIX / "config.json")
            return json.loads(request("/config?action=reload", port=18882)[2])
        start_version = json.loads(request("/config", port=18882)[2])["stats"]["version"]
        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as executor:
            pending = executor.submit(request, "/snapshot")
            for _ in range(100):
                if json.loads(request("/config", port=18882)[2]).get("ready"):
                    break
                time.sleep(0.005)
            else:
                raise AssertionError("snapshot request did not enter yielding callback")
            updated = copy.deepcopy(config)
            target = next(route for route in updated["routes"] if route["id"] == "snapshot")
            target["plugins"]["route"] = {"tag": "new"}
            result = reload_config(updated)
            assert result["ok"] and result["stats"]["version"] == start_version + 1, result
            assert result["stats"]["generations"] == 2 and not pending.done(), result
            assert result["reader_tag"] == "new" and result["reader_filtered"], result
            assert result["reader_version"] == result["stats"]["version"], result
            assert "new:rewrite" in json.loads(request("/snapshot")[2])["trace"]
            old_result = pending.result()
            assert old_result[0] == 200 and "old:rewrite" in json.loads(old_result[2])["trace"], old_result
        for _ in range(100):
            info = json.loads(request("/config", port=18882)[2])["stats"]
            if info["generations"] == 1 and info["readers"] == 0:
                break
            time.sleep(0.005)
        assert info["generations"] == 1 and info["readers"] == 0, info
        checks += 3
        result = reload_config("{", raw=True)
        assert not result["ok"] and result["stats"]["version"] == start_version + 1, result
        invalid = copy.deepcopy(updated)
        next(route for route in invalid["routes"] if route["id"] == "limit")["plugins"]["limit-count"]["count"] = 0
        for _ in range(3):
            result = reload_config(invalid)
            assert not result["ok"] and result["stats"]["generations"] == 1, result
            assert result["stats"]["version"] == start_version + 1, result
            assert "new:access" in json.loads(request("/snapshot")[2])["trace"]
            checks += 1
        rejected = copy.deepcopy(updated)
        next(route for route in rejected["routes"] if route["id"] == "snapshot")["plugins"]["route"]["tag"] = "reader-reject"
        result = reload_config(rejected)
        assert not result["ok"] and "reader rejected candidate" in result["result"], result
        assert result["reader_tag"] == "new" and result["reader_version"] == start_version + 1, result
        checks += 1
        for revision in range(12):
            target["plugins"]["route"]["tag"] = "revision-" + str(revision)
            result = reload_config(updated)
            assert result["ok"] and result["stats"]["generations"] == 1, result
            assert target["plugins"]["route"]["tag"] + ":access" in json.loads(request("/snapshot")[2])["trace"]
            checks += 1
        forbidden = copy.deepcopy(updated)
        forbidden["local_conf"]["apisix"]["tracing"] = False
        result = reload_config(forbidden)
        assert not result["ok"] and "NGINX reload" in result["result"], result
        checks += 1
        target["plugins"]["route"]["tag"] = "watched"
        replacement = PREFIX / "config.next.json"
        replacement.write_text(json.dumps(updated))
        replacement.replace(PREFIX / "config.json")
        for _ in range(100):
            if "watched:access" in json.loads(request("/snapshot")[2])["trace"]:
                break
            time.sleep(0.03)
        else:
            raise AssertionError("native file watcher did not publish new configuration")
        checks += 1
        closed = json.loads(request("/config?close=yes", port=18882)[2])
        target["plugins"]["route"]["tag"] = "closed-reader"
        result = reload_config(updated)
        assert result["ok"] and result["reader_tag"] == closed["reader_tag"], result
        assert "closed-reader:access" in json.loads(request("/snapshot")[2])["trace"]
        log = (PREFIX / "logs/error.log").read_text()
        assert not any(marker in log for marker in ("signal 11", "alert]", "forbidden", "C-call boundary",
                       "AddressSanitizer", "UndefinedBehaviorSanitizer")), log
        assert not re.search(r"\.[ch]:\d+(?::\d+)?: runtime error:", log), log
        checks += 1
        print(json.dumps({"profile": PROFILE, "checks": checks, "phase_dispatch": "passed", "lua_plugin_yield": "passed",
                          "original_core_guard": "enabled"}, indent=2))
    finally:
        subprocess.run([NGINX, "-p", str(PREFIX), "-c", "nginx.conf", "-s", "quit"], env=NGINX_ENV, check=True)
        dns.close()


if __name__ == "__main__":
    main()
