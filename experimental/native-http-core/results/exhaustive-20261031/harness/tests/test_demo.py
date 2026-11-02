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
"""Real HTTP conformance checks against both data planes and the unchanged plugins."""
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
import http.client
import json
from pathlib import Path
import re
import socket
import subprocess
import time

import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from demo import NGINX, NGINX_ENV, ROOT, HERE, prepare, stop


def request(path, headers=None, method="GET", body=None, port=9080):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    try:
        conn.request(method, path, body=body, headers=headers or {})
        response = conn.getresponse()
        return response.status, dict(response.getheaders()), response.read()
    finally:
        conn.close()


def ready(path="/plain/ready", expected=200, headers=None):
    for _ in range(100):
        try:
            if request(path, headers=headers)[0] == expected:
                return
        except (OSError, http.client.HTTPException):
            pass
        time.sleep(0.1)
    raise AssertionError("gateway not ready; inspect /tmp/apisix-http-core-demo/logs/error.log")


def hits():
    return int(request("/__hits", port=1980)[2])


def metrics():
    code, _, body = request("/apisix/prometheus/metrics", port=9091)
    assert code == 200, (code, body)
    return body.decode()


def counts(text):
    result = Counter()
    for line in text.splitlines():
        if line.startswith("apisix_http_status{"):
            code = re.search(r'\bcode="(\d+)"', line)
            if code:
                result[int(code[1])] += float(line.rsplit(" ", 1)[1])
    return result


def suite(mode, native_filters=True):
    fixture = json.loads((HERE / "example.json").read_text())
    fixture["routes"].append({"id": "nested", "uri": "/plain/auth/*",
                              "plugins": {"key-auth": {}}})
    fixture_path = Path("/tmp/apisix-http-core-fixture.json")
    fixture_path.write_text(json.dumps(fixture))
    prepare(mode, fixture=fixture_path, test_window=2, native_filters=native_filters)
    ready()
    if mode == "native" and native_filters:
        config = (ROOT / "conf/nginx.conf").read_text()
        assert "apisix_core_native_filters on;" in config
        assert "header_filter_by_lua_block" not in config and "body_filter_by_lua_block" not in config
    time.sleep(3)
    before = counts(metrics())
    expected = Counter()
    observations = []

    def check(path, status=200, headers=None, method="GET", body=None):
        result = request(path, headers, method, body)
        assert result[0] == status, (mode, path, result)
        assert result[1].get("Server", "").startswith("APISIX"), result[1]
        expected[status] += 1
        observations.append((path, status))
        return result

    echo = {"X-Demo-Echo": "1"}
    _, _, body = check("/plain/echo?q=a%20b", headers=echo)
    assert json.loads(body)["uri"] == "/plain/echo?q=a%20b"
    _, _, first = check("/plain/connection", headers=echo)
    _, _, second = check("/plain/connection", headers=echo)
    assert json.loads(first)["connection"] == json.loads(second)["connection"], "upstream keepalive was not reused"
    _, _, body = check("/plain/body", headers=echo, method="POST", body="payload")
    assert json.loads(body)["body"] == "payload"
    check("/plain/head", method="HEAD")
    _, _, body = check("/plain/a/../ok", headers=echo)
    assert json.loads(body)["uri"] == "/plain/ok"
    check("/plain", 404)
    check("/authentic/item", 404)
    check("/missing", 404)
    check("/plain/../auth/echo", 401)
    check("/plain/auth/item", 401)
    check("/plain/auth/item", headers={"apikey": "demo-key"})

    _, _, body = check("/auth/echo", headers={**echo, "apikey": "demo-key"})
    payload = json.loads(body)
    assert payload["headers"]["x-consumer-username"] == "demo-user"
    assert "apikey" not in payload["headers"]
    _, _, body = check("/auth/echo?apikey=demo-key&keep=yes", headers=echo)
    assert json.loads(body)["uri"] == "/auth/echo?keep=yes"
    sent = hits()
    check("/auth/item", 401)
    check("/auth/item", 401, {"apikey": "wrong"})
    check("/auth/item?apikey=demo-key", 401, {"apikey": "wrong"})
    assert hits() == sent, "failed authentication reached upstream"

    for remaining in range(4, -1, -1):
        _, headers, _ = check("/limit/item")
        assert headers["X-RateLimit-Remaining"] == str(remaining), headers
    sent = hits()
    check("/limit/item", 429)
    assert hits() == sent, "rejected limit-count reached upstream"
    time.sleep(2.1)
    check("/limit/item")

    check("/combined/item", 401, {"apikey": "wrong"})
    for _ in range(5):
        check("/combined/item", headers={"apikey": "demo-key"})
    check("/combined/item", 429, {"apikey": "demo-key"})
    check("/combined/item", headers={"apikey": "second-key"})
    check("/plain/error?code=503", 503)
    check("/plain/timeout?sleep=0.3", 504)

    for _ in range(60):
        text = metrics()
        delta = counts(text) - before
        if delta == expected:
            break
        time.sleep(0.1)
    assert delta == expected, (mode, "global metrics mismatch", delta, expected)
    assert re.search(r'code="404"[^\n]*route=""', text), "404 must have an empty route label"
    assert 'consumer="demo-user"' in text
    assert 'route="plain"' in text
    assert 'source="nginx"' in text or 'response_source="nginx"' in text
    assert "apisix_http_latency" in text and "apisix_bandwidth" in text
    for line in text.splitlines():
        if "metric_errors_total" in line and not line.startswith("#"):
            assert float(line.rsplit(" ", 1)[1]) == 0, line
    print(f"PASS {mode}: {sum(expected.values())} real requests, global metrics {dict(expected)}", flush=True)
    return observations


def multiple_workers(mode):
    prepare(mode, workers=2, test_window=5)
    ready()
    with ThreadPoolExecutor(max_workers=16) as pool:
        codes = list(pool.map(lambda _: request("/limit/concurrent")[0], range(32)))
    assert Counter(codes) == {200: 5, 429: 27}, (mode, Counter(codes))
    print(f"PASS {mode}: 2-worker shared quota, 5 accepted / 27 rejected", flush=True)



def native_faults():
    prepare("native", upstream_address="127.0.0.1:1981")
    for _ in range(100):
        try:
            if request("/plain/unavailable")[0] == 502:
                break
        except OSError:
            pass
        time.sleep(0.05)
    else:
        raise AssertionError("unavailable upstream did not return 502")
    prepare("native")
    ready()
    sock = socket.create_connection(("127.0.0.1", 9080))
    sock.sendall(b"GET /plain/abort?sleep=0.3 HTTP/1.1\r\nHost: localhost\r\n\r\n")
    time.sleep(0.02)
    sock.close()
    time.sleep(0.15)
    assert request("/plain/after-abort")[0] == 200
    stop()
    path = ROOT / "conf/nginx.conf"
    config = path.read_text()
    marker = 'require("apisix.native.engine").init_worker()'
    assert config.count(marker) == 1
    injected = 'require("apisix.plugins.key-auth").rewrite = function() error("injected Lua failure") end; '
    config = config.replace(marker, injected + marker)
    path.write_text(config)
    subprocess.run([NGINX, "-p", str(ROOT), "-c", "conf/nginx.conf"], env=NGINX_ENV, check=True)
    ready()
    before = hits()
    assert request("/auth/fault", {"apikey": "demo-key"})[0] == 500
    assert hits() == before
    assert request("/plain/still-alive")[0] == 200
    logs = (ROOT / "logs/error.log").read_text()
    assert "AddressSanitizer" not in logs and "UndefinedBehaviorSanitizer" not in logs, logs
    assert not re.search(r"\.[ch]:\d+(?::\d+)?: runtime error:", logs), logs
    print("PASS native: upstream failure, client abort, injected plugin error, original-core loader guard", flush=True)


def native_filter_guard():
    prepare("native")
    ready()
    before = hits()
    stop()
    path = ROOT / "conf/nginx.conf"
    text = path.read_text()
    marker = 'require("apisix.native.engine").init_worker()'
    injected = 'require("apisix.plugins.key-auth").header_filter = function() error("must not be skipped") end; '
    assert text.count(marker) == 1
    path.write_text(text.replace(marker, injected + marker))
    subprocess.run([NGINX, "-p", str(ROOT), "-c", "conf/nginx.conf"], env=NGINX_ENV, check=True)
    for _ in range(100):
        try:
            code = request("/plain/unsupported-filter")[0]
            assert code == 500, code
            break
        except OSError:
            time.sleep(0.05)
    else:
        raise AssertionError("guarded gateway did not respond")
    assert hits() == before, "unsupported native filter configuration reached upstream"
    logs = (ROOT / "logs/error.log").read_text()
    assert "native filter mode cannot dispatch Lua filter callbacks" in logs, logs
    print("PASS native: filter callbacks rejected instead of skipped", flush=True)


def log_variable_semantics(mode):
    prepare(mode)
    stop()
    path = ROOT / "conf/nginx.conf"
    text = path.read_text()
    marker = ('require("apisix.native.engine").init_worker()' if mode == "native"
              else "apisix.http_init_worker()")
    before = '''
        require("apisix.plugins.prometheus").rewrite = function(conf, ctx)
            ctx.var.status = 299
            ctx.var.llm_model = false
        end
'''
    after = '''
        require("apisix.core.ctx").register_var("request_llm_model", function(ctx)
            assert(ctx._plugin_name == "prometheus", "custom getter executed before its plugin")
            return "custom-model"
        end, {no_cacheable = true})
'''
    assert text.count(marker) == 1
    path.write_text(text.replace(marker, before + marker + after))
    subprocess.run([NGINX, "-p", str(ROOT), "-c", "conf/nginx.conf"], env=NGINX_ENV, check=True)
    ready()
    lines = []
    for _ in range(60):
        code, _, body = request("/apisix/prometheus/metrics", port=9091)
        # The exporter populates its response cache asynchronously. A ready
        # proxy does not imply its first collection timer has fired yet.
        if code == 500 and json.loads(body).get("message") == "Failed to retrieve metrics: no data available":
            time.sleep(0.1)
            continue
        assert code == 200, (code, body)
        lines = [line for line in body.decode().splitlines() if line.startswith("apisix_http_status{")]
        if any('code="299"' in line and 'request_llm_model="custom-model"' in line
               and 'llm_model="<non-scalar>"' in line for line in lines):
            break
        time.sleep(0.1)
    else:
        raise AssertionError((mode, "cached status/false or custom getter was overridden", lines))
    print(f"PASS {mode}: cached readonly status, false value and custom log getter preserved", flush=True)


if __name__ == "__main__":
    try:
        lua = suite("lua")
        native = suite("native")
        assert lua == native, "different HTTP behavior"
        bridge = suite("native", native_filters=False)
        assert native == bridge, "native C filters changed HTTP behavior"
        multiple_workers("lua")
        multiple_workers("native")
        log_variable_semantics("lua")
        log_variable_semantics("native")
        native_filter_guard()
        native_faults()
        print("PASS all scenario comparisons", flush=True)
    finally:
        stop()
