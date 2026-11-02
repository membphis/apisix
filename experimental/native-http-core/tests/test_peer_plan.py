#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Peer-plan off/on checks with real proxy, retry, DNS and connection reuse."""
from concurrent.futures import ThreadPoolExecutor
import copy
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import itertools
import json
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import threading
import time

import test_demo as demo


CONNECTIONS = itertools.count(1)


class Echo(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def setup(self):
        super().setup()
        self.connection_id = next(CONNECTIONS)

    def do_GET(self):
        local = self.connection.getsockname()
        body = json.dumps({"connection": self.connection_id, "ip": local[0],
                           "port": local[1], "uri": self.path}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_):
        pass


class IPv6Server(ThreadingHTTPServer):
    address_family = socket.AF_INET6


def backend(ip, server_type=ThreadingHTTPServer):
    server = server_type((ip, 0), Echo)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


class DNS:
    def __init__(self):
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.socket.bind(("127.0.0.1", 0))
        self.port = self.socket.getsockname()[1]
        self.answers = 0
        threading.Thread(target=self.run, daemon=True).start()

    def run(self):
        while True:
            try:
                query, remote = self.socket.recvfrom(4096)
                offset, labels = 12, []
                while query[offset]:
                    length = query[offset]
                    labels.append(query[offset + 1:offset + 1 + length].decode())
                    offset += length + 1
                question_end = offset + 5
                kind = struct.unpack("!H", query[offset + 1:offset + 3])[0]
                known = ".".join(labels) == "backend.peer-plan.test"
                answer = known and kind == 1
                response = query[:2] + struct.pack("!HHHHH", 0x8180 if known else 0x8183,
                                                 1, int(answer), 0, 0)
                response += query[12:question_end]
                if answer:
                    response += b"\xc0\x0c" + struct.pack("!HHIH", 1, 1, 1, 4)
                    response += socket.inet_aton("127.0.0.1")
                    self.answers += 1
                self.socket.sendto(response, remote)
            except OSError:
                return

    def close(self):
        self.socket.close()


OBSERVER = r'''
        local plugin = require("apisix.plugins.prometheus")
        plugin.access = function(conf, ctx)
            if ngx.var.uri:find("/peer/dynamic", 1, true) == 1 then
                require("apisix.upstream").set(ctx, "peer-plan-dynamic", 1, {
                    type = "roundrobin", nodes = {["127.0.0.1:__PORT_B__"] = 1},
                    scheme = "http", retries = 0,
                    timeout = {connect = 0.3, send = 1, read = 1}
                })
            end
        end
        plugin.before_proxy = function(conf, ctx)
            local path = ngx.var.uri
            ctx.peer_plan_calls = (ctx.peer_plan_calls or 0) + 1
            assert(ngx.get_phase() == (ctx.peer_plan_calls == 1 and "access" or "balancer"))
            assert(ctx.balancer_port and ctx.balancer_ip)
            if path:find("/peer/ipv6", 1, true) == 1 then
                assert(ctx.balancer_ip == "::1", tostring(ctx.balancer_ip))
            else
                assert(ctx.balancer_ip == "127.0.0.1", tostring(ctx.balancer_ip))
            end
            if path:find("/peer/scheme", 1, true) == 1 then
                -- Configured https descriptor must fall back after this write.
                ctx.var.upstream_scheme = "http"
            elseif path:find("/peer/restore", 1, true) == 1 then
                -- Equal contents in a new request-pool buffer may reuse a plan.
                ctx.var.upstream_scheme = "https"
                ctx.var.upstream_scheme = "http"
            end
            if path:find("/peer/reload", 1, true) == 1
                and ngx.var.http_x_peer_pause == "1" then
                _G.peer_plan_waiting = true
                ngx.sleep(0.3)
                _G.peer_plan_waiting = false
            end
        end
        plugin.log = function(conf, ctx)
            local expected = ngx.var.uri:find("/peer/retry", 1, true) == 1 and 2 or 1
            assert(ctx.peer_plan_calls == expected,
                   "unexpected before_proxy count: " .. tostring(ctx.peer_plan_calls))
            _G.peer_plan_logged = (_G.peer_plan_logged or 0) + 1
        end
'''


def available_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def route(name, port, host="127.0.0.1", scheme="http"):
    endpoint = f"[{host}]:{port}" if ":" in host else f"{host}:{port}"
    return {"id": name, "uri": f"/peer/{name}/*", "plugins": {"prometheus": {}},
            "upstream": {"type": "roundrobin", "nodes": {endpoint: 1},
                         "scheme": scheme, "retries": 0,
                         "timeout": {"connect": 0.3, "send": 1, "read": 1},
                         "keepalive_pool": {"size": 16, "idle_timeout": 30, "requests": 100}}}


def sample(flags, first, second, ipv6, dns):
    os.environ["APISIX_CORE_DIRECTIVES"] = flags
    demo.prepare("native", benchmark=True)
    demo.stop()
    port_a, port_b = first.server_port, second.server_port
    control_port = available_port()
    # Hold an unlistening socket so the intended first peer deterministically
    # refuses connections without another process racing to bind its port.
    dead = socket.socket()
    dead.bind(("127.0.0.1", 0))
    dead_port = dead.getsockname()[1]
    config_path = demo.ROOT / "conf/native.json"
    config = json.loads(config_path.read_text())
    config["global_rules"] = []
    config["consumers"] = []
    config["local_conf"]["dns_resolver"] = [["127.0.0.1", dns.port]]
    config["routes"] = [route("static", port_a), route("second", port_b),
                        route("scheme", port_a, scheme="https"), route("restore", port_a),
                        route("dynamic", port_a), route("dns", port_a, "backend.peer-plan.test"),
                        route("reload", port_a), route("retry", port_a)]
    retry = config["routes"][-1]["upstream"]
    retry["nodes"] = [{"host": "127.0.0.1", "port": dead_port, "weight": 1, "priority": 10},
                      {"host": "127.0.0.1", "port": port_a, "weight": 1, "priority": 0}]
    retry["retries"] = 1
    if ipv6:
        config["routes"].append(route("ipv6", ipv6.server_port, "::1"))
    config_path.write_text(json.dumps(config))
    nginx_path = demo.ROOT / "conf/nginx.conf"
    text = nginx_path.read_text()
    marker = 'require("apisix.native.engine").init_worker()'
    assert text.count(marker) == 1
    text = text.replace(marker, OBSERVER.replace("__PORT_B__", str(port_b)) + marker)
    assert "proxy_next_upstream off;" in text
    text = text.replace("proxy_next_upstream off;", "proxy_next_upstream error timeout;")
    control = '''
    server {
        listen 127.0.0.1:__PORT__;
        location / {
            content_by_lua_block {
                local config = require("apisix.native.config")
                local ok, err
                if ngx.var.arg_reload then ok, err = config.reload() end
                ngx.print(require("cjson.safe").encode({ok = ok, error = err,
                    stats = config.stats(), waiting = _G.peer_plan_waiting or false,
                    logged = _G.peer_plan_logged or 0}))
            }
        }
    }
'''.replace("__PORT__", str(control_port))
    close = text.rfind("}")
    assert close >= 0 and not text[close + 1:].strip()
    nginx_path.write_text(text[:close] + control + text[close:])
    subprocess.run([demo.NGINX, "-p", str(demo.ROOT), "-c", "conf/nginx.conf"],
                   env=demo.NGINX_ENV, check=True)
    requests = 0

    def echo(name, headers=None):
        nonlocal requests
        status, _, body = demo.request(f"/peer/{name}/value", headers=headers)
        requests += 1
        assert status == 200, (flags, name, status, body)
        return json.loads(body)

    def control_get(query=""):
        status, _, body = demo.request("/" + query, port=control_port)
        assert status == 200, body
        return json.loads(body)

    try:
        demo.ready(path="/peer/static/ready")
        # ready() submits one successful proxied request, included in log count.
        requests += 1
        initial = echo("static")
        for name in ("static", "scheme", "restore", "static"):
            value = echo(name)
            assert value["port"] == port_a and value["connection"] == initial["connection"], value
        alternate = echo("second")
        assert alternate["port"] == port_b and alternate["connection"] != initial["connection"], alternate
        dynamic = echo("dynamic")
        assert dynamic["port"] == port_b, dynamic
        before = dns.answers
        resolved = echo("dns")
        assert resolved["port"] == port_a and dns.answers > before, (resolved, dns.answers, before)
        retried = echo("retry")
        assert retried["port"] == port_a, retried
        if ipv6:
            value = echo("ipv6")
            assert value["ip"] == "::1" and value["port"] == ipv6.server_port, value
            assert echo("ipv6")["connection"] == value["connection"]
        with ThreadPoolExecutor(max_workers=1) as pool:
            pending = pool.submit(echo, "reload", {"X-Peer-Pause": "1"})
            for _ in range(100):
                if control_get()["waiting"]:
                    break
                time.sleep(0.002)
            else:
                raise AssertionError("old-generation request did not suspend")
            updated = copy.deepcopy(config)
            item = next(item for item in updated["routes"] if item["id"] == "reload")
            item["upstream"]["nodes"] = {f"127.0.0.1:{port_b}": 1}
            replacement = config_path.with_suffix(".next")
            replacement.write_text(json.dumps(updated))
            replacement.replace(config_path)
            result = control_get("?reload=1")
            assert result["ok"] and result["stats"]["generations"] == 2 and not pending.done(), result
            assert echo("reload")["port"] == port_b
            assert pending.result()["port"] == port_a
        for _ in range(100):
            final = control_get()
            if final["stats"]["readers"] == 0 and final["logged"] == requests:
                break
            time.sleep(0.01)
        assert final["stats"]["generations"] == 1 and final["stats"]["readers"] == 0, final
        assert final["logged"] == requests, (final, requests)
        logs = (demo.ROOT / "logs/error.log").read_text()
        errors = [line for line in logs.splitlines() if "[error]" in line or "[alert]" in line]
        # A refused first peer is the intended retry stimulus; no Lua assertion,
        # plan failure, second-peer failure or other runtime error is permitted.
        assert errors and all("connect() failed (111: Connection refused)" in line
                              and f"127.0.0.1:{dead_port}" in line for line in errors), errors
        assert "runtime error:" not in logs and "AddressSanitizer" not in logs, logs
        print("PASS peer plan:", flags or "off", "static IP, scheme fallback/content equality, "
              "dynamic upstream fallback, retry, DNS, keepalive pool identity, pinned reload",
              "IPv6=PASS" if ipv6 else "IPv6=SKIP")
    finally:
        demo.stop()
        dead.close()


def main():
    previous = os.environ.get("APISIX_CORE_DIRECTIVES")
    first = backend("127.0.0.1")
    second = backend("127.0.0.1")
    dns = DNS()
    ipv6 = None
    try:
        try:
            ipv6 = backend("::1", IPv6Server)
        except OSError as error:
            print("SKIP IPv6: local ::1 listener unavailable:", error)
        sample("", first, second, ipv6, dns)
        sample("apisix_core_balancer_peer_plan on;", first, second, ipv6, dns)
    finally:
        demo.stop()
        dns.close()
        for server in (first, second, ipv6):
            if server:
                server.shutdown()
                server.server_close()
        if previous is None:
            os.environ.pop("APISIX_CORE_DIRECTIVES", None)
        else:
            os.environ["APISIX_CORE_DIRECTIVES"] = previous


if __name__ == "__main__":
    main()
