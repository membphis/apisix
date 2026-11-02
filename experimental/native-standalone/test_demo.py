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

from prepare import NGINX, ROOT, prepare, stop, validate


def request(path, headers=None, method="GET", body=None, port=9080):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    try:
        conn.request(method, path, body=body, headers=headers or {})
        response = conn.getresponse()
        return response.status, dict(response.getheaders()), response.read()
    finally:
        conn.close()


def ready():
    for _ in range(100):
        try:
            if request("/plain/ready")[0] == 200:
                return
        except (OSError, http.client.HTTPException):
            pass
        time.sleep(0.1)
    raise AssertionError("gateway not ready; inspect /usr/local/apisix/logs/error.log")


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


def suite(mode):
    fixture = json.loads(Path("/demo/example.json").read_text())
    fixture["routes"].append({"id": "nested", "uri": "/plain/auth/*",
                              "plugins": {"key-auth": {}}})
    fixture_path = Path("/tmp/native-demo-fixture.json")
    fixture_path.write_text(json.dumps(fixture))
    prepare(mode, fixture=fixture_path, test_window=2)
    ready()
    time.sleep(3)
    before = counts(metrics())
    expected = Counter()
    observations = []

    def check(path, status=200, headers=None, method="GET", body=None):
        result = request(path, headers, method, body)
        assert result[0] == status, (mode, path, result)
        expected[status] += 1
        observations.append((path, status))
        return result

    echo = {"X-Demo-Echo": "1"}
    _, _, body = check("/plain/echo?q=a%20b", headers=echo)
    assert json.loads(body)["uri"] == "/plain/echo?q=a%20b"
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
    print(f"PASS {mode}: {sum(expected.values())} real requests, global metrics {dict(expected)}", flush=True)
    return observations


def multiple_workers(mode):
    prepare(mode, workers=2, test_window=5)
    ready()
    with ThreadPoolExecutor(max_workers=16) as pool:
        codes = list(pool.map(lambda _: request("/limit/concurrent")[0], range(32)))
    assert Counter(codes) == {200: 5, 429: 27}, (mode, Counter(codes))
    print(f"PASS {mode}: 2-worker shared quota, 5 accepted / 27 rejected", flush=True)


def faults():
    prepare("native", upstream_address="127.0.0.1:1981")
    for _ in range(50):
        try:
            if request("/plain/unavailable")[0] == 502:
                break
        except OSError:
            pass
        time.sleep(0.1)
    else:
        raise AssertionError("unavailable upstream did not return 502")
    prepare("native")
    ready()
    stop()
    config_path = ROOT / "conf/nginx.conf"
    text = config_path.read_text().replace(
        "require('native_demo').init_worker()",
        "require('native_demo').init_worker()\n"
        "require('apisix.plugins.key-auth').rewrite = function() error('injected test failure') end")
    config_path.write_text(text)
    subprocess.run([NGINX, "-p", str(ROOT), "-c", "conf/nginx.conf"], check=True)
    ready()
    sent = hits()
    assert request("/auth/fault", {"apikey": "demo-key"})[0] == 500
    assert hits() == sent
    assert request("/plain/still-alive")[0] == 200
    print("PASS native: unavailable upstream and injected Lua exception fail closed", flush=True)
    prepare("native")
    ready()
    sock = socket.create_connection(("127.0.0.1", 9080))
    sock.sendall(b"GET /plain/abort?sleep=0.3 HTTP/1.1\r\nHost: localhost\r\n\r\n")
    time.sleep(0.02)
    sock.close()
    time.sleep(0.15)
    assert request("/plain/after-abort")[0] == 200
    stop()
    text = config_path.read_text().replace(
        "require('native_demo').init_worker()",
        "require('native_demo').init_worker()\n"
        "local function forbidden() error('old Lua core called by native path') end\n"
        "apisix.http_access_phase = forbidden\n"
        "apisix.http_balancer_phase = forbidden\n"
        "require('apisix.router').router_http.match = forbidden\n"
        "require('apisix.plugin').filter = forbidden")
    config_path.write_text(text)
    subprocess.run([NGINX, "-p", str(ROOT), "-c", "conf/nginx.conf"], check=True)
    ready()
    for prefix in ["plain", "auth", "limit", "combined"]:
        assert request(f"/{prefix}/native-proof", {"apikey": "demo-key"})[0] == 200
    assert request("/unmatched/native-proof")[0] == 404
    print("PASS native: client abort and execution with old Lua router/filter/balancer poisoned", flush=True)


if __name__ == "__main__":
    a = suite("lua")
    b = suite("native")
    assert a == b, "different behavioral observations"
    multiple_workers("lua")
    multiple_workers("native")
    faults()
    bad = json.loads(Path("/demo/example.json").read_text())
    bad["routes"][0]["plugins"]["prometheus"] = {}
    try:
        validate(bad)
    except ValueError:
        pass
    else:
        raise AssertionError("route-level Prometheus silently accepted")
    prepare("native")
    ready()
    print("PASS all standalone native demo checks", flush=True)
