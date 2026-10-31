#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Differential native key-auth and C-only transport semantics, inside the image."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import copy
import http.client
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
from urllib.parse import parse_qsl

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from demo import prepare, stop, ROOT, NGINX, NGINX_ENV
from test_demo import ready, hits, request

BASE = ("apisix_core_lazy_rate_json on; apisix_core_batch_vars on; "
        "apisix_core_lazy_route_opts on; apisix_core_request_header_flags on; "
        "apisix_core_header_scratch on; apisix_core_request_pool_native on;")
VARIANTS = {
    "lua-core": ("lua", ""),
    "native-lua": ("native", BASE),
    "native-c-adapter": ("native", BASE + " apisix_core_native_key_auth on;"),
    "native-c-pipeline": ("native", BASE + " apisix_core_native_pipeline on;"),
}

def call(path, headers=(), method="GET", body=None):
    c = http.client.HTTPConnection("127.0.0.1", 9080, timeout=5)
    c.putrequest(method, path, skip_host=any(k.lower() == "host" for k, _ in headers))
    for k, v in [("X-Demo-Echo", "1"), *headers]:
        c.putheader(k, v)
    if body is not None: c.putheader("Content-Length", str(len(body)))
    c.endheaders(body)
    r = c.getresponse()
    status, hdr, data = r.status, {k.lower(): v for k, v in r.getheaders()}, r.read()
    c.close()
    if status == 200 and method != "HEAD":
        value = json.loads(data)
        value.pop("connection", None)
        uri = value["uri"].split("?", 1)
        # ngx.encode_args traverses a hash table; field ordering is unspecified.
        value["uri"] = [uri[0], sorted(parse_qsl(uri[1], keep_blank_values=True)) if len(uri) == 2 else []]
        value["headers"].pop("content-length", None) if method == "GET" else None
        return status, hdr.get("www-authenticate"), value
    if method == "HEAD": return status, hdr.get("www-authenticate"), b""
    try: data = json.loads(data)
    except ValueError: data = re.sub(rb"nginx/[0-9.]+", b"nginx", data)
    return status, hdr.get("www-authenticate"), data

def fixture():
    routes = []
    def route(name, conf=None, **extra):
        routes.append({"id": name, "uri": "/" + name + "/*", "plugins": {"key-auth": conf or {}}, **extra})
    route("auth", {"hide_credentials": True})
    route("keep", {"hide_credentials": False})
    route("custom", {"header": "X-Key", "query": "token", "realm": "native-test", "hide_credentials": True})
    route("anon", {"anonymous_consumer": "guest", "hide_credentials": True})
    route("anonkeep", {"anonymous_consumer": "guest"})
    route("bad-anon", {"anonymous_consumer": "missing", "hide_credentials": True})
    route("disabled", {"_meta": {"disable": True}})
    route("method", methods=["POST"])
    route("host", host="native.example")
    timeout = {"connect": .05, "read": .05, "send": .05}
    route("retry", upstream={"type": "roundrobin", "nodes": [
        {"host": "127.0.0.1", "port": 1999, "weight": 1, "priority": 1},
        {"host": "127.0.0.1", "port": 1980, "weight": 1}], "retries": 1, "timeout": timeout})
    route("no-retry", upstream={"type": "roundrobin", "nodes": {"127.0.0.1:1999": 1}, "retries": 0, "timeout": timeout})
    route("timeout", upstream={"type": "roundrobin", "nodes": {"127.0.0.1:1980": 1}, "retries": 0, "timeout": timeout})
    return {"upstream": "127.0.0.1:1980", "routes": routes, "global_rules": [], "consumers": [
        {"username": "demo-user", "labels": {"custom_id": "custom-user"}, "plugins": {"key-auth": {"key": "demo-key"}}},
        {"username": "second-user", "plugins": {"key-auth": {"key": "second-key"}}},
        {"username": "guest", "plugins": {}},
        {"username": "empty-key-user", "plugins": {"key-auth": {"key": ""}}},
        {"username": "escaped-user", "plugins": {"key-auth": {"key": "space +/&=%"}}},
    ]}

def cases():
    key = [("apikey", "demo-key")]
    return [
        ("/auth/item", key), ("/auth/item", [("APIKEY", "second-key")]),
        ("/auth/item", [("apikey", "demo-key"), ("apikey", "second-key")]),
        ("/auth/item", [("apikey", "wrong"), ("apikey", "demo-key")]),
        ("/auth/item", []), ("/auth/item", [("apikey", "DEMO-KEY")]),
        ("/auth/item?apikey=demo-key&keep=yes", []),
        ("/auth/item?apikey=second-key", key),
        ("/auth/item?apikey=demo-key", [("apikey", "wrong")]),
        ("/auth/item?apikey", []), ("/auth/item?apikey=", []),
        ("/auth/item?apikey=demo-key&apikey=demo-key", []),
        ("/auth/item?api%6bey=demo-key&v=a+b&v=c%26d&flag&empty=", []),
        ("/auth/item?apikey=space%20%2B%2F%26%3D%25", []),
        ("/auth/item?" + "&".join("x%d=1" % i for i in range(120)) + "&apikey=demo-key", []),
        ("/auth/item", key + [("X-Consumer-Username", "spoof"), ("X-Consumer-Custom-ID", "spoof"), ("X-Credential-Identifier", "spoof")]),
        ("/keep/item?apikey=demo-key", []), ("/keep/item", key),
        ("/custom/item", [("X-Key", "demo-key")]), ("/custom/item?token=second-key", []),
        ("/custom/item", []),
        ("/anon/item", []), ("/anon/item?apikey=wrong&keep=yes", [("apikey", "wrong")]),
        ("/anon/item?apikey=second-key", key), ("/anonkeep/item?apikey=wrong", [("apikey", "wrong")]),
        ("/bad-anon/item", []), ("/disabled/item", []),
        ("/outside-route", key), ("/authentic/item", key),
        ("/auth/a/../item", key), ("/auth/a%2Fb", key), ("/auth//item", key),
        ("/method/item", key), ("/method/item", key, "POST", b"key-auth-body"),
        ("/auth/item", key, "HEAD"), ("/auth/item", [], "HEAD"),
        ("/auth/item", [("apikey", "wrong")], "HEAD"), ("/outside-route", [], "HEAD"),
        ("/host/item", key), ("/host/item", key + [("Host", "native.example")]),
        ("/retry/item", key), ("/no-retry/item", key), ("/timeout/item?sleep=0.15", key),
        ("/timeout/item?sleep=0.08", key),
        ("/auth/item", key + [("Forwarded", "for=spoof"), ("X-Forwarded-Proto", "spoof"), ("X-Forwarded-Host", "spoof"), ("X-Forwarded-Port", "1")]),
        ("/auth/item", [("X-Pad-%d" % i, "1") for i in range(105)] + key),
        ("/auth/item?apikey=demo-key", [("X-Pad-%d" % i, "1") for i in range(105)] + [("apikey", "wrong")]),
    ]

def keepalive():
    conns = []
    for _ in range(3):
        c = http.client.HTTPConnection("127.0.0.1", 9080)
        c.request("GET", "/auth/echo", headers={"apikey": "demo-key", "X-Demo-Echo": "1"})
        r = c.getresponse(); assert r.status == 200
        conns.append(json.loads(r.read())["connection"]); c.close()
    assert len(set(conns)) == 1, conns

def run(variant):
    mode, directives = VARIANTS[variant]
    os.environ["APISIX_CORE_DIRECTIVES"] = directives
    f = Path("/tmp/native-key-auth-fixture.json"); f.write_text(json.dumps(fixture()))
    prepare(mode, fixture=f, plugin_allowlist=["key-auth"], proxy_retry=True)
    ready("/missing", expected=404)
    code, _, body = request("/apisix/nginx_status")
    assert code == 200 and b"Active connections:" in body, (variant, "nginx_status", code, body)
    answers = [call(*case) for case in cases()]
    keepalive()
    sent = hits(); assert call("/auth/item")[0] == 401; assert hits() == sent
    for method in ("GET", "HEAD", "POST"):
        sent = hits()
        assert call("/auth/item", method=method, body=b"reject-body" if method == "POST" else None)[0] == 401
        assert hits() == sent, (variant, "rejected request reached upstream", method)
    with ThreadPoolExecutor(max_workers=32) as pool:
        outputs = list(pool.map(lambda i: call("/auth/item", [("apikey", "demo-key" if i % 2 else "second-key")]), range(128)))
    assert all(x[0] == 200 for x in outputs)
    assert [x[2]["headers"]["x-consumer-username"] for x in outputs] == ["demo-user" if i % 2 else "second-user" for i in range(128)]
    if variant == "native-c-pipeline":
        text = (ROOT / "conf/nginx.conf").read_text()
        # All request phase hooks, including balancer, must be absent.
        assert not re.search(r"\b(?:access|header_filter|body_filter|log|balancer)_by_lua", text)
    # The route's retries=1 must not override the server's retry-off policy.
    stop()
    prepare(mode, fixture=f, plugin_allowlist=["key-auth"], proxy_retry=False)
    ready("/missing", expected=404)
    assert call("/retry/item", [("apikey", "demo-key")])[0] == 502
    stop(); return answers

def lifecycle(variant):
    mode, directives = VARIANTS[variant]
    os.environ["APISIX_CORE_DIRECTIVES"] = directives
    prepare(mode, single_plugin="key-auth")
    ready("/missing", expected=404)
    path = ROOT / "conf/native.json"
    original = json.loads(path.read_text())
    # Native requests pin old generations until their network I/O completes.
    with ThreadPoolExecutor(max_workers=4) as pool:
        pending = pool.submit(call, "/auth/item?sleep=2", [("apikey", "demo-key")])
        time.sleep(.1)
        updated = copy.deepcopy(original)
        updated["routes"][0]["upstream"]["timeout"]["read"] = 3
        # The in-flight request uses the original 100ms timeout. Test pinning
        # separately below after installing the long timeout.
        path.write_text(json.dumps(updated))
        assert pending.result()[0] == 504
    time.sleep(1.3)
    with ThreadPoolExecutor(max_workers=4) as pool:
        pending = pool.submit(call, "/auth/item?sleep=2", [("apikey", "demo-key")])
        time.sleep(.1)
        changed = copy.deepcopy(updated)
        changed["consumers"][0]["plugins"]["key-auth"]["key"] = "new-demo-key"
        path.write_text(json.dumps(changed))
        for _ in range(40):
            if call("/auth/item", [("apikey", "new-demo-key")])[0] == 200: break
            time.sleep(.1)
        else: raise AssertionError("new consumer key was not published")
        assert call("/auth/item", [("apikey", "demo-key")])[0] == 401
        reply = pending.result(); assert reply[0] == 200, (variant, reply)
    # Unsupported new business logic must reject the whole generation, keeping
    # the previous valid graph active. Nothing may be silently omitted.
    bad = copy.deepcopy(changed)
    bad["routes"][0]["filter_func"] = "function() return true end"
    path.write_text(json.dumps(bad)); time.sleep(1.3)
    assert call("/auth/item", [("apikey", "new-demo-key")])[0] == 200
    text = (ROOT / "logs/error.log").read_text()
    assert "standalone configuration rejected" in text and "Lua route predicates" in text
    path.write_text(json.dumps(changed)); time.sleep(1.2)
    # Missing key/alias errors should not corrupt subsequent requests.
    for _ in range(3):
        assert call("/auth/item")[0] == 401
        assert call("/auth/item", [("apikey", "new-demo-key")])[0] == 200
    stop()
    print("PASS", variant, "generation pinning/reload/key rotation/rejected unsupported reload/recovery", flush=True)

if __name__ == "__main__":
    parser = argparse.ArgumentParser(); parser.add_argument("--variants", nargs="+", choices=list(VARIANTS), default=list(VARIANTS))
    parser.add_argument("--out", type=Path); args = parser.parse_args()
    result = {}
    try:
        for variant in args.variants:
            answer = run(variant)
            # Branch URI normalization differs from stable Lua on dot segments,
            # already recorded before this task. Compare the C variants to the
            # native-Lua control for this one input; other inputs use stable.
            if result:
                baseline = result.get("lua-core") or next(iter(result.values()))
                for i, value in enumerate(answer):
                    expected = baseline[i]
                    if cases()[i][0] == "/auth/a/../item" and "native-lua" in result:
                        expected = result["native-lua"][i]
                    if cases()[i][0] == "/auth/a/../item" and variant == "native-lua": continue
                    assert value == expected, (variant, i, cases()[i], value, expected)
            result[variant] = answer
            print("PASS", variant, len(answer), "semantic cases; 128 concurrent auth; keepalive; rejected requests do not proxy", flush=True)
        for variant in args.variants:
            if variant in ("native-c-adapter", "native-c-pipeline"): lifecycle(variant)
        if args.out:
            args.out.write_text(json.dumps(result, default=lambda x: x.decode(), indent=2) + "\n")
    finally: stop()
