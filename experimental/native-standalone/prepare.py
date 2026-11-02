#!/usr/bin/env python3
# Licensed to the Apache Software Foundation (ASF) under one or more
# contributor license agreements. See the NOTICE file distributed with
# this work for additional information regarding copyright ownership.
# The ASF licenses this file to You under the Apache License, Version 2.0
# (the "License"); you may not use this file except in compliance with
# the License. You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Freeze one standalone snapshot, then select the Lua or native data plane."""

import argparse
import copy
import json
from pathlib import Path
import re
import shutil
import subprocess
import time

ROOT = Path("/usr/local/apisix")
NGINX = "/usr/local/openresty/nginx/sbin/nginx"
MODULE = "/opt/native-module/lib/ngx_http_apisix_native_module.so"


def validate(snapshot):
    def keys(value, allowed):
        if not isinstance(value, dict) or set(value) - set(allowed):
            raise ValueError(f"unsupported configuration keys; expected {allowed}")

    keys(snapshot, ["upstream", "routes", "consumers", "global_rules"])
    if not re.fullmatch(r"127\.0\.0\.1:[1-9][0-9]{0,4}", snapshot["upstream"]):
        raise ValueError("demo upstream must be a loopback IPv4 address and port")
    if int(snapshot["upstream"].split(":")[1]) > 65535:
        raise ValueError("upstream port out of range")
    globals_ = snapshot["global_rules"]
    if globals_ != [{"id": "metrics", "plugins": {"prometheus": {}}}]:
        raise ValueError("exactly one global Prometheus rule is required")
    ids, prefixes = set(), set()
    if not 1 <= len(snapshot["routes"]) <= 64:
        raise ValueError("expected 1..64 routes")
    for route in snapshot["routes"]:
        keys(route, ["id", "uri", "plugins"])
        rid, uri = route["id"], route["uri"]
        if not re.fullmatch(r"[a-zA-Z0-9_-]{1,128}", rid):
            raise ValueError("invalid route id")
        if len(uri) > 1025 or not re.fullmatch(r"/(?:[A-Za-z0-9._~-]+/)*\*", uri):
            raise ValueError("only /prefix/* routes are supported")
        if rid in ids or uri in prefixes:
            raise ValueError("duplicate route id or prefix")
        ids.add(rid)
        prefixes.add(uri)
        keys(route["plugins"], ["key-auth", "limit-count"])
        if "key-auth" in route["plugins"]:
            conf = route["plugins"]["key-auth"]
            keys(conf, ["header", "query", "realm", "hide_credentials"])
            if "hide_credentials" in conf and not isinstance(conf["hide_credentials"], bool):
                raise ValueError("hide_credentials must be boolean")
        if "limit-count" in route["plugins"]:
            conf = route["plugins"]["limit-count"]
            keys(conf, ["count", "time_window", "key", "key_type", "policy",
                        "rejected_code", "rejected_msg", "show_limit_quota_header"])
            if conf.get("policy", "local") != "local":
                raise ValueError("only local limit-count is supported")
            for key in ["count", "time_window"]:
                if type(conf.get(key)) is not int or conf[key] <= 0:
                    raise ValueError(f"{key} must be a positive integer")
            if conf.get("key_type", "var") not in ["var", "constant"]:
                raise ValueError("only var/constant limit-count keys are supported")
            if (conf.get("key_type", "var") == "var"
                    and conf.get("key", "remote_addr") not in ["remote_addr", "consumer_name"]):
                raise ValueError("unsupported limit-count variable")
    for consumer in snapshot["consumers"]:
        keys(consumer, ["username", "plugins"])
        keys(consumer["plugins"], ["key-auth"])
        keys(consumer["plugins"]["key-auth"], ["key"])
    return snapshot


def stop(prefix=ROOT):
    pid_file = prefix / "logs/nginx.pid"
    if not pid_file.exists():
        return
    pid = pid_file.read_text().strip()
    subprocess.run([NGINX, "-p", str(prefix), "-c", "conf/nginx.conf", "-s", "quit"],
                   check=True, stdout=subprocess.DEVNULL)
    for _ in range(100):
        if not pid_file.exists() or not Path(f"/proc/{pid}").exists():
            return
        time.sleep(0.05)
    raise RuntimeError("gateway did not stop gracefully")


def upstream(cpu=None):
    prefix = Path("/tmp/native-demo-upstream")
    (prefix / "logs").mkdir(parents=True, exist_ok=True)
    (prefix / "conf").mkdir(exist_ok=True)
    if (prefix / "logs/nginx.pid").exists():
        return
    affinity = f"worker_cpu_affinity {1 << cpu:b};\n" if cpu is not None else ""
    (prefix / "conf/nginx.conf").write_text(affinity + '''
worker_processes 1;
error_log logs/error.log warn;
pid logs/nginx.pid;
events { worker_connections 4096; }
http {
    access_log off;
    lua_shared_dict hits 1m;
    server {
        listen 127.0.0.1:1980;
        location = /__hits {
            content_by_lua_block { ngx.say(ngx.shared.hits:get("count") or 0) }
        }
        location / {
            content_by_lua_block {
                ngx.shared.hits:incr("count", 1, 0)
                if ngx.var.arg_sleep then ngx.sleep(tonumber(ngx.var.arg_sleep)) end
                local code = tonumber(ngx.var.arg_code) or 200
                ngx.status = code
                if ngx.req.get_headers()["X-Demo-Echo"] then
                    ngx.req.read_body()
                    ngx.header.content_type = "application/json"
                    ngx.say(require("cjson").encode({
                        uri = ngx.var.request_uri, method = ngx.req.get_method(),
                        body = ngx.req.get_body_data() or "",
                        headers = ngx.req.get_headers()
                    }))
                else ngx.say("ok") end
            }
        }
    }
}
''')
    subprocess.run([NGINX, "-p", str(prefix), "-c", "conf/nginx.conf"], check=True)


def prepare(mode, workers=1, benchmark=False, fixture=None, test_window=None,
            upstream_address=None, gateway_cpu=None, upstream_cpu=None):
    snapshot = json.loads(Path(fixture or "/demo/example.json").read_text())
    if upstream_address:
        snapshot["upstream"] = upstream_address
    if benchmark or test_window:
        for route in snapshot["routes"]:
            conf = route["plugins"].get("limit-count")
            if conf:
                conf["count"] = 1000000000 if benchmark else 5
                conf["time_window"] = test_window or 600
    validate(snapshot)
    stop()
    error_log = ROOT / "logs/error.log"
    if error_log.is_symlink():
        error_log.unlink()
        error_log.touch()
    source = ROOT / "apisix"
    if source.is_symlink():
        source.unlink()
    elif source.exists():
        shutil.rmtree(source)
    # Worker users must be able to read a checkout created with a restrictive umask.
    shutil.copytree("/src/apisix", source)
    for path in [source, *source.rglob("*")]:
        path.chmod(0o755 if path.is_dir() else 0o644)
    shutil.copyfile("/demo/native_demo.lua", ROOT / "native_demo.lua")
    (ROOT / "native_demo.lua").chmod(0o644)
    config = {
        "apisix": {"node_listen": [9080], "enable_ipv6": False,
                   "enable_control": False, "enable_http2": False,
                   "extra_lua_path": "/usr/local/apisix/?.lua",
                   "ssl": {"enable": False}, "enable_admin": False},
        "deployment": {"role": "data_plane", "role_data_plane": {"config_provider": "json"}},
        "plugins": ["prometheus", "key-auth", "limit-count"],
        "plugin_attr": {"prometheus": {"enable_export_server": True,
                         "export_addr": {"ip": "0.0.0.0", "port": 9091},
                         "refresh_interval": 1}},
        "nginx_config": {"worker_processes": workers, "error_log_level": "warn",
                         "http_server_location_configuration_snippet":
                             "proxy_connect_timeout 100ms; proxy_read_timeout 100ms; "
                             "proxy_send_timeout 100ms; proxy_next_upstream off;",
                         "http": {"enable_access_log": False}}
    }
    if gateway_cpu is not None:
        config["nginx_config"]["main_configuration_snippet"] = (
            f"worker_cpu_affinity {1 << gateway_cpu:b};")
    (ROOT / "conf/config.yaml").write_text(json.dumps(config))
    resources = copy.deepcopy(snapshot)
    endpoint = resources.pop("upstream")
    for route in resources["routes"]:
        route["upstream"] = {"type": "roundrobin", "nodes": {endpoint: 1},
                             "retries": 0,
                             "timeout": {"connect": 0.1, "send": 0.1, "read": 0.1}}
    (ROOT / "conf/apisix.json").write_text(json.dumps(resources))
    subprocess.run(["bash", "/src/bin/apisix", "init"], cwd=ROOT, check=True)
    path = ROOT / "conf/nginx.conf"
    text = path.read_text()
    if mode == "native":
        text = f"load_module {MODULE};\n" + text
        directives = ["apisix_native on;", "apisix_native_backend apisix_backend;"]
        for route in snapshot["routes"]:
            plan = int("key-auth" in route["plugins"]) + 2 * int("limit-count" in route["plugins"])
            directives.append(f'apisix_native_route {route["id"]} {route["uri"][:-1]} {plan};')
        # The marker is in the business server, not the exporter or named locations.
        marker = "apisix.http_access_phase()"
        if text.count(marker) != 1:
            raise RuntimeError("unexpected APISIX template: business access hook must be unique")
        location = text.rfind("location / {", 0, text.index(marker))
        if location < 0:
            raise RuntimeError("unexpected APISIX template: missing business location")
        text = text[:location] + "\n".join(directives) + "\n" + text[location:]
        text, count = re.subn(r"balancer_by_lua_block\s*\{\s*apisix.http_balancer_phase\(\)\s*\}", "", text)
        if count != 1:
            raise RuntimeError("unexpected APISIX template: exactly one upstream balancer required")
        text = text.replace("server 0.0.0.1;", f"server {endpoint};")
        text = text.replace("apisix.http_init_worker()", "apisix.http_init_worker()\nrequire('native_demo').init_worker()")
        text = text.replace("apisix.http_access_phase()", "require('native_demo').access()")
        text = text.replace("apisix.http_log_phase()", "require('native_demo').log()")
        for phase in ["header_filter", "body_filter"]:
            text = text.replace(f"apisix.http_{phase}_phase()", "-- no supported plugin in this phase")
        text = text.replace("$upstream_scheme://apisix_backend$upstream_uri",
                            "$upstream_scheme://$apisix_native_upstream$upstream_uri")
    path.write_text(text)
    subprocess.run([NGINX, "-p", str(ROOT), "-c", "conf/nginx.conf", "-t"], check=True)
    upstream(upstream_cpu)
    subprocess.run([NGINX, "-p", str(ROOT), "-c", "conf/nginx.conf"], check=True)
    print(f"Started {mode}: gateway :9080, global Prometheus :9091, {workers} worker(s)")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=["lua", "native", "stop"])
    parser.add_argument("--workers", type=int, choices=[1, 2, 4], default=1)
    parser.add_argument("--benchmark", action="store_true")
    parser.add_argument("--fixture")
    args = parser.parse_args()
    if args.mode == "stop":
        stop()
    else:
        prepare(args.mode, args.workers, args.benchmark, args.fixture)
