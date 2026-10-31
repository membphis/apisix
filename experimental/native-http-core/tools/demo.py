#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Inside the pinned image: prepare identical standalone Lua/native fixtures."""
import argparse
import copy
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time

HERE = Path(__file__).resolve().parents[1]
SOURCE = HERE.parents[1]
ROOT = Path("/tmp/apisix-http-core-demo")
UPSTREAM = Path("/tmp/apisix-http-core-upstream")
NGINX = "/usr/local/openresty/nginx/sbin/nginx"
LUAJIT = "/usr/local/openresty/luajit/bin/luajit"
MODULE = "/opt/apisix-core-module/lib/ngx_http_apisix_core_module.so"
NGINX_ENV = os.environ.copy()
if "address" in os.environ.get("APISIX_CORE_SANITIZER", ""):
    NGINX_ENV["LD_PRELOAD"] = subprocess.check_output(["gcc", "-print-file-name=libasan.so"], text=True).strip()
    NGINX_ENV["ASAN_OPTIONS"] = "detect_leaks=0:abort_on_error=1"
NGINX_ENV["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"


def stop(prefix=ROOT):
    pid_file = prefix / "logs/nginx.pid"
    if not pid_file.exists():
        return
    pid = pid_file.read_text().strip()
    subprocess.run([NGINX, "-p", str(prefix), "-c", "conf/nginx.conf", "-s", "quit"], env=NGINX_ENV, check=True)
    for _ in range(100):
        if not pid_file.exists() or not Path("/proc", pid).exists():
            return
        time.sleep(0.05)
    raise RuntimeError("NGINX did not stop gracefully: " + str(prefix))


def backend(cpu=None):
    if (UPSTREAM / "logs/nginx.pid").exists():
        return
    (UPSTREAM / "logs").mkdir(parents=True, exist_ok=True)
    (UPSTREAM / "conf").mkdir(exist_ok=True)
    affinity = f"worker_cpu_affinity {1 << cpu:b};\n" if cpu is not None else ""
    text = affinity + '''
worker_processes 1;
error_log logs/error.log warn;
pid logs/nginx.pid;
events { worker_connections 4096; }
http {
    access_log off;
    lua_shared_dict hits 1m;
    server {
        listen 127.0.0.1:1980;
        location = /__hits { content_by_lua_block { ngx.say(ngx.shared.hits:get("count") or 0) } }
        location / { content_by_lua_block {
            ngx.shared.hits:incr("count", 1, 0)
            if ngx.var.arg_sleep then ngx.sleep(tonumber(ngx.var.arg_sleep)) end
            ngx.status = tonumber(ngx.var.arg_code) or 200
            if ngx.var.http_x_demo_echo then
                ngx.req.read_body()
                ngx.header.content_type = "application/json"
                ngx.say(require("cjson.safe").encode({uri = ngx.var.request_uri,
                    method = ngx.req.get_method(), body = ngx.req.get_body_data() or "",
                    connection = ngx.var.connection, headers = ngx.req.get_headers()}))
            else ngx.say("ok") end
        } }
    }
}
'''
    (UPSTREAM / "conf/nginx.conf").write_text(text)
    subprocess.run([NGINX, "-p", str(UPSTREAM), "-c", "conf/nginx.conf"], env=NGINX_ENV, check=True)


def replace_init(text, directive, body):
    pattern = r"\b" + directive + r"\s*\{.*?^    \}"
    text, count = re.subn(pattern, lambda _: directive + " {\n" + body + "\n    }", text,
                          flags=re.MULTILINE | re.DOTALL)
    if count != 1:
        raise RuntimeError(f"expected one HTTP {directive}, found {count}")
    return text


def merged_settings():
    script = ROOT / "export-local-conf.lua"
    script.write_text('''
package.path = "./?.lua;./deps/share/lua/5.1/?.lua;./deps/share/lua/5.1/?/init.lua;" .. package.path
package.cpath = "./deps/lib64/lua/5.1/?.so;./deps/lib/lua/5.1/?.so;/usr/local/openresty/lualib/?.so;" .. package.cpath
local value, err = require("apisix.cli.file").read_yaml_conf("__ROOT__")
assert(value, err)
print(require("cjson.safe").encode(value))
'''.replace("__ROOT__", str(ROOT)))
    return json.loads(subprocess.check_output([LUAJIT, str(script)], cwd=ROOT, text=True))


def prepare(mode, workers=1, benchmark=False, fixture=None, test_window=None,
            upstream_address=None, gateway_cpu=None, upstream_cpu=None, native_filters=True):
    assert mode in ("lua", "native"), mode
    stop()
    # Retire only the earlier prototype processes in this dedicated demo container.
    stop(Path("/usr/local/apisix"))
    stop(Path("/tmp/native-demo-upstream"))
    snapshot = json.loads(Path(fixture or HERE / "example.json").read_text())
    if upstream_address:
        snapshot["upstream"] = upstream_address
    if benchmark or test_window:
        for route in snapshot["routes"]:
            limit = route.get("plugins", {}).get("limit-count")
            if limit:
                limit["count"] = 1_000_000_000 if benchmark else 5
                limit["time_window"] = test_window or 600
    shutil.rmtree(ROOT, ignore_errors=True)
    (ROOT / "logs").mkdir(parents=True)
    shutil.copytree(SOURCE / "apisix", ROOT / "apisix")
    shutil.copytree(SOURCE / "conf", ROOT / "conf")
    (ROOT / "deps").symlink_to("/usr/local/apisix/deps")
    (ROOT / "native_lua/apisix/native").mkdir(parents=True)
    shutil.copyfile(HERE / "runtime.lua", ROOT / "native_lua/apisix/native/runtime.lua")
    config = {
        "apisix": {"node_listen": [9080], "enable_ipv6": False, "enable_control": False,
                   "enable_http2": False, "enable_admin": False, "ssl": {"enable": False},
                   "extra_lua_path": str(ROOT / "native_lua/?.lua") + ";" + str(ROOT / "?.lua")},
        "deployment": {"role": "data_plane", "role_data_plane": {"config_provider": "json"}},
        "plugins": ["prometheus", "key-auth", "limit-count"],
        "plugin_attr": {"prometheus": {"enable_export_server": True,
                         "export_addr": {"ip": "0.0.0.0", "port": 9091}, "refresh_interval": 1}},
        "nginx_config": {"worker_processes": workers, "error_log_level": "warn",
            "http_server_location_configuration_snippet":
                "proxy_connect_timeout 100ms; proxy_read_timeout 100ms; proxy_send_timeout 100ms; proxy_next_upstream off;",
            "http": {"enable_access_log": False}}
    }
    if gateway_cpu is not None:
        config["nginx_config"]["main_configuration_snippet"] = f"worker_cpu_affinity {1 << gateway_cpu:b};"
    resources = copy.deepcopy(snapshot)
    endpoint = resources.pop("upstream")
    for route in resources["routes"]:
        route["upstream"] = {"type": "roundrobin", "nodes": {endpoint: 1}, "retries": 0,
                             "timeout": {"connect": 0.1, "read": 0.1, "send": 0.1}}
    (ROOT / "conf/config.yaml").write_text(json.dumps(config))
    (ROOT / "conf/apisix.json").write_text(json.dumps(resources))
    subprocess.run(["bash", str(SOURCE / "bin/apisix"), "init"], cwd=ROOT, check=True)
    local_conf = merged_settings()
    native_config = {**resources, "plugins": config["plugins"], "local_conf": local_conf}
    (ROOT / "conf/native.json").write_text(json.dumps(native_config))
    path = ROOT / "conf/nginx.conf"
    text = path.read_text()
    if mode == "native":
        text = f"load_module {MODULE};\n" + text
        text = replace_init(text, "init_by_lua_block", '''
        require "resty.core"
        table.insert(package.loaders, 2, function(name)
            if name == "apisix" or (name:match("^apisix%.") and not name:match("^apisix%.native%.")
               and not name:match("^apisix%.plugins%.")) then
                error("original APISIX core forbidden: " .. name)
            end
        end)
        require("apisix.native.engine").init("__CONFIG__")
'''.replace("__CONFIG__", str(ROOT / "conf/native.json")))
        worker_init = r"""
        require("apisix.native.engine").init_worker()
        assert(package.loaded.apisix == nil, "old APISIX initializer loaded")
        for name, value in pairs(package.loaded) do
            if type(name) == "string" and name:match("^apisix%.") and not name:match("^apisix%.plugins%.")
               and type(value) == "table" then
                for _, method in pairs(value) do
                    if type(method) == "function" then
                        local info = debug.getinfo(method, "S")
                        local path = info.source or ""
                        assert(not (path:find("__OLD_CORE__", 1, true) and not path:find("/apisix/plugins/", 1, true)),
                               "old Lua core function loaded: " .. name .. " " .. path)
                    end
                end
            end
        end
""".replace("__OLD_CORE__", str(ROOT / "apisix/"))
        text = replace_init(text, "init_worker_by_lua_block", worker_init)
        text = replace_init(text, "exit_worker_by_lua_block", '        require("apisix.native.engine").shutdown()')
        for phase in ("access", "header_filter", "body_filter", "log"):
            text = text.replace(f"apisix.http_{phase}_phase()", f'require("apisix.native.runtime").run("{phase}")')
        text = text.replace("apisix.http_balancer_phase()", 'require("apisix.native.runtime").balance()')
        if native_filters:
            text = text.replace("http {", "http {\n    apisix_core_native_filters on;", 1)
            for phase in ("header_filter", "body_filter"):
                pattern = r"\b" + phase + r"_by_lua_block\s*\{[^{}]*\}"
                text, removed = re.subn(pattern, "", text)
                if removed < 1:
                    raise RuntimeError("expected generated " + phase + " hook")
        marker = 'require("apisix.native.runtime").run("access")'
        location = text.rfind("location / {", 0, text.index(marker))
        if location < 0 or text.count(marker) != 1:
            raise RuntimeError("unexpected generated business access location")
        text = text[:location] + "apisix_core_proxy on;\n" + text[location:]
        # C captures originals and sanitizes these; avoid a duplicate template pass.
        text = re.sub(r"^\s*set \$original_(?:x_forwarded_\w+|forwarded)\s+[^;]*;\n", "", text, flags=re.MULTILINE)
        text = re.sub(r'^\s*more_set_input_headers "(?:X-Forwarded-(?:Proto|Host|Port)|Forwarded):[^\n]*\n',
                      "", text, flags=re.MULTILINE)
    path.write_text(text)
    for entry in [ROOT, *ROOT.rglob("*")]:
        if not entry.is_symlink():
            entry.chmod(0o755 if entry.is_dir() else 0o644)
    subprocess.run([NGINX, "-p", str(ROOT), "-c", "conf/nginx.conf", "-t"], env=NGINX_ENV, check=True)
    backend(upstream_cpu)
    subprocess.run([NGINX, "-p", str(ROOT), "-c", "conf/nginx.conf"], env=NGINX_ENV, check=True)
    print(f"Started {mode}: :9080, global Prometheus :9091, workers={workers}", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=["lua", "native", "stop"])
    parser.add_argument("--workers", type=int, choices=[1, 2, 4], default=1)
    parser.add_argument("--benchmark", action="store_true")
    args = parser.parse_args()
    if args.mode == "stop":
        stop()
    else:
        prepare(args.mode, workers=args.workers, benchmark=args.benchmark)
