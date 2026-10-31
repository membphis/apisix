#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""C key-auth plus unchanged Lua Prometheus: exact counters and wire accounting."""
import argparse
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor
import copy
import http.client
import json
import os
from pathlib import Path
import re
import socket
import sys
import time
from urllib.parse import parse_qsl

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from demo import prepare, stop, ROOT
from test_demo import ready, request, metrics, hits
from test_native_key_auth import BASE, fixture, cases, call, keepalive

VARIANTS = {
    "lua-both": ("lua", "", "hybrid"),
    "c-adapter-both": ("native", BASE + " apisix_core_native_key_auth on;", "hybrid"),
    "c-hybrid": ("native", BASE + " apisix_core_native_pipeline on; apisix_core_lua_log_bridge on;", "hybrid"),
    "c-no-metrics": ("native", BASE + " apisix_core_native_pipeline on;", "single"),
}

def rows(text, family, probe=None):
    result = []
    for line in text.splitlines():
        match = re.fullmatch(re.escape(family) + r"\{(.*)\} ([0-9.eE+-]+)", line)
        if not match: continue
        labels = {k: json.loads('"' + v + '"') for k, v in re.findall(r'(\w+)="((?:\\.|[^"\\])*)"', match[1])}
        if probe is None or labels.get("probe") == probe: result.append((labels, float(match[2])))
    return result

def wait_metrics(check):
    last = None
    for _ in range(100):
        try:
            last = metrics()
            if check(last): return last
        except (AssertionError, OSError, http.client.HTTPException): pass
        time.sleep(.1)
    path = Path("/tmp/hybrid-metrics-failed-" + str(time.time_ns()) + ".prom")
    path.write_text(last or "no metrics response")
    raise AssertionError(("metrics condition did not converge; raw output saved", str(path)))

def status_map(text, probe):
    return counter_sum(rows(text, "apisix_http_status", probe))

def counter_sum(values):
    out = Counter()
    for labels, value in values: out[int(labels["code"])] += value
    return out

def collect_log(probe):
    result = []
    for line in (ROOT / "logs/hybrid-access.log").read_text().splitlines():
        tag, skip, status, ingress, egress = line.split("|")
        if tag == probe and skip != "1": result.append((int(status), int(ingress), int(egress)))
    return result

def snapshot(mode, fixture_path, variant, workers=1):
    settings = {"metrics": {name: {"extra_labels": [{"probe": "$hybrid_probe"}]} for name in
                            ["http_status", "http_latency", "bandwidth"]}}
    getter = '''
        require("apisix.core.ctx").register_var("hybrid_probe", function(ctx)
            ctx.hybrid_probe_reads = (ctx.hybrid_probe_reads or 0) + 1
            assert(ctx.hybrid_probe_reads == 1, "custom getter must remain cached within one request")
            return ngx.var.http_x_demo_probe or ""
        end)
'''
    prepare(mode, fixture=fixture_path, plugin_allowlist=["key-auth", "prometheus"],
            with_prometheus=True, proxy_retry=True, prometheus_settings=settings,
            observe_access=True, workers=workers, init_worker_extra=getter)
    ready("/missing", expected=404)
    assert request("/apisix/nginx_status")[0] == 200
    if variant == "c-hybrid":
        text = (ROOT / "conf/nginx.conf").read_text()
        assert not re.search(r"\b(?:access|header_filter|body_filter|balancer)_by_lua", text)
        assert text.count('require("apisix.native.runtime").native_log()') == 1

def suite(variant):
    mode, directives, _ = VARIANTS[variant]; os.environ["APISIX_CORE_DIRECTIVES"] = directives
    f = fixture()
    f["routes"][0]["name"] = "auth-name"
    f["global_rules"] = [{"id": "metrics", "plugins": {"prometheus": {
        "_meta": {"filter": [["arg_skip", "~=", "1"]]}}}}]
    f["routes"].append({"id": "named", "name": "named-route", "uri": "/named/*",
                         "plugins": {"key-auth": {}, "prometheus": {"prefer_name": True}}})
    path = Path("/tmp/hybrid-prometheus-fixture.json"); path.write_text(json.dumps(f))
    snapshot(mode, path, variant)
    inputs = [x for x in cases() if x[0] != "/auth/a/../item"]
    inputs += [("/named/item", [("apikey", "demo-key")]),
               ("/named/item", []),
               ("/auth/item?skip=1", [("apikey", "demo-key")]),
               ("/auth/item?code=503", [("apikey", "demo-key")])]
    answers, expected = [], Counter()
    for x in inputs:
        args = list(x); args[1] = [*args[1], ("X-Demo-Probe", "semantics")]
        out = call(*args)
        if isinstance(out[2], dict) and "connection" in out[2]:
            out[2].pop("connection")
            uri = out[2]["uri"].split("?", 1)
            out[2]["uri"] = [uri[0], sorted(parse_qsl(uri[1], keep_blank_values=True)) if len(uri) == 2 else []]
        answers.append(out)
        if "skip=1" not in args[0]: expected[out[0]] += 1
    text = wait_metrics(lambda t: status_map(t, "semantics") == expected
                        and sum(v for l, v in rows(t, "apisix_http_latency_count", "semantics") if l["type"] == "request") == sum(expected.values())
                        and len(rows(t, "apisix_bandwidth", "semantics")) > 0)
    observed = rows(text, "apisix_http_status", "semantics")
    assert sum(v for l, v in observed if l["route"] == "named-route") == 2, observed
    assert not any(l["route"] == "named" for l, _ in observed), "prefer_route/name policy changed"
    assert any(l["code"] == "200" and l["consumer"] == "demo-user" and l["node"] == "127.0.0.1" and l["response_source"] == "upstream" for l, _ in observed)
    assert any(l["code"] == "401" and l["consumer"] == "" and l["node"] == "" and l["response_source"] == "apisix" for l, _ in observed)
    assert any(l["code"] == "404" and l["route"] == "" and l["matched_uri"] == "" for l, _ in observed)
    assert any(l["code"] == "504" and l["response_source"] == "nginx" for l, _ in observed)
    latency_counts = Counter()
    for l, v in rows(text, "apisix_http_latency_count", "semantics"): latency_counts[l["type"]] += v
    assert latency_counts["request"] == latency_counts["apisix"] == sum(expected.values())
    assert latency_counts["upstream"] == sum(v for code, v in expected.items() if code not in [401, 404]), latency_counts
    for kind in ["request", "upstream", "apisix"]:
        inf = sum(v for l, v in rows(text, "apisix_http_latency_bucket", "semantics") if l["type"] == kind and l["le"] == "+Inf")
        assert inf == latency_counts[kind]
    log = collect_log("semantics"); assert Counter(x[0] for x in log) == expected
    bandwidth = Counter()
    for l, v in rows(text, "apisix_bandwidth", "semantics"): bandwidth[l["type"]] += v
    assert bandwidth["ingress"] == sum(x[1] for x in log), (variant, "ingress", bandwidth, log)
    assert bandwidth["egress"] == sum(x[2] for x in log), (variant, "egress", bandwidth, log)
    assert all(v == 0 for l, v in rows(text, "apisix_nginx_metric_errors_total"))
    metric_errors = [line for line in text.splitlines() if "metric_errors_total" in line and not line.startswith("#")]
    assert all(float(line.rsplit(" ", 1)[1]) == 0 for line in metric_errors), metric_errors
    # The metric label set/counter values must agree independently of timing.
    status = sorted((tuple(sorted(l.items())), v) for l, v in observed)
    with ThreadPoolExecutor(max_workers=32) as pool:
        outs = list(pool.map(lambda i: call("/auth/item", [("apikey", "demo-key" if i % 2 else "second-key"), ("X-Demo-Probe", "concurrent")]), range(128)))
    assert all(x[0] == 200 for x in outs)
    t = wait_metrics(lambda t: status_map(t, "concurrent") == {200: 128})
    consumers = Counter()
    for l, v in rows(t, "apisix_http_status", "concurrent"): consumers[l["consumer"]] += v
    assert consumers == {"demo-user": 64, "second-user": 64}, consumers
    keepalive()
    before = hits(); assert call("/auth/reject", [("X-Demo-Probe", "reject")])[0] == 401; assert hits() == before
    # Abort while the upstream is pending, before the configured timeout.
    s = socket.create_connection(("127.0.0.1", 9080)); s.sendall(b"GET /auth/item?sleep=0.3 HTTP/1.1\r\nHost: localhost\r\napikey: demo-key\r\nX-Demo-Probe: abort\r\n\r\n")
    time.sleep(.02); s.close(); wait_metrics(lambda t: status_map(t, "abort") == {499: 1})
    stop()
    print("PASS", variant, len(inputs), "semantic cases, exact global/route status+labels+histograms, bandwidth equals access-log bytes, 128 concurrent auth, 499 abort, keepalive", flush=True)
    return {"answers": answers, "status_series": status, "expected_status": dict(expected), "histogram_counts": dict(latency_counts), "bandwidth": dict(bandwidth)}

def lifecycle(variant):
    mode, directives, _ = VARIANTS[variant]; os.environ["APISIX_CORE_DIRECTIVES"] = directives
    f = fixture(); f["routes"][0]["name"] = "auth-old"
    f["routes"][0]["upstream"] = {"type": "roundrobin", "nodes": {"127.0.0.1:1980": 1},
        "retries": 0, "timeout": {"connect": .1, "read": 3, "send": .1}}
    f["global_rules"] = [{"id": "metrics", "plugins": {"prometheus": {"prefer_name": True}}}]
    path = Path("/tmp/hybrid-prometheus-lifecycle.json");path.write_text(json.dumps(f));snapshot(mode,path,variant)
    resource = ROOT / "conf" / ("apisix.json" if mode == "lua" else "native.json")
    with ThreadPoolExecutor(max_workers=2) as pool:
        pending = pool.submit(call, "/auth/item?sleep=2", [("apikey","demo-key"),("X-Demo-Probe","old-generation")])
        time.sleep(.1)
        changed = json.loads(resource.read_text()); changed["routes"][0]["name"] = "auth-new"
        changed["consumers"][0]["username"] = "rotated-user";changed["consumers"][0].pop("id",None)
        changed["consumers"][0]["plugins"]["key-auth"]["key"] = "rotated-key"
        write_resource(resource, changed)
        for _ in range(50):
            if call("/auth/item",[("apikey","rotated-key"),("X-Demo-Probe","poll")])[0] == 200: break
            time.sleep(.1)
        else: raise AssertionError("consumer key rotation did not publish")
        assert pending.result()[0] == 200
    assert call("/auth/item",[("apikey","rotated-key"),("X-Demo-Probe","new-generation")])[0] == 200
    assert call("/auth/item",[("apikey","demo-key"),("X-Demo-Probe","old-key-rejected")])[0] == 401
    text=wait_metrics(lambda t:status_map(t,"old-generation")=={200:1} and status_map(t,"new-generation")=={200:1})
    assert all(l["route"]=="auth-old" and l["consumer"]=="demo-user" for l,v in rows(text,"apisix_http_status","old-generation"))
    assert all(l["route"]=="auth-new" and l["consumer"]=="rotated-user" for l,v in rows(text,"apisix_http_status","new-generation"))
    # Existing Lua Prometheus metadata cache must see a new resource version.
    changed["plugin_metadata"]=[{"id":"prometheus","disabled_labels":{"http_status":["consumer"],"http_latency":["consumer"]}}]
    write_resource(resource, changed);time.sleep(1.3)
    assert call("/auth/item",[("apikey","rotated-key"),("X-Demo-Probe","disabled-label")])[0]==200
    text=wait_metrics(lambda t:status_map(t,"disabled-label")=={200:1})
    assert all(l["consumer"]=="" for l,v in rows(text,"apisix_http_status","disabled-label"))
    assert all(l["consumer"]=="" for l,v in rows(text,"apisix_http_latency_count","disabled-label"))
    stop()
    # Each worker owns a VM and peer pool; metric counters remain shared.
    snapshot(mode,path,variant,workers=2)
    with ThreadPoolExecutor(max_workers=32) as pool:
        replies=list(pool.map(lambda _:call("/auth/item",[("apikey","demo-key"),("X-Demo-Probe","two-workers")]),range(128)))
    assert all(r[0]==200 for r in replies)
    wait_metrics(lambda t:status_map(t,"two-workers")=={200:128})
    stop();print("PASS",variant,"old/new generation labels, key rotation, metadata invalidation, custom getter/cache, two-worker shared metrics",flush=True)

def write_resource(path, value):
    # Stable config_yaml polls an integer-second mtime, while the C watcher
    # uses content CRC. Ensure the baseline actually observes rapid edits.
    stamp = max(int(time.time()), int(path.stat().st_mtime) + 2)
    path.write_text(json.dumps(value)); os.utime(path, (stamp, stamp))

if __name__ == "__main__":
    parser = argparse.ArgumentParser(); parser.add_argument("--variants", nargs="+", default=["lua-both", "c-adapter-both", "c-hybrid"], choices=list(VARIANTS)[:-1]); parser.add_argument("--out", type=Path); args = parser.parse_args()
    results = {}
    try:
        for variant in args.variants:
            result = suite(variant)
            if results:
                original = next(iter(results.values()))
                assert result["answers"] == original["answers"], (variant, "HTTP response mismatch")
                assert result["status_series"] == original["status_series"], (variant, "metric label/value mismatch", result["status_series"], original["status_series"])
            results[variant] = result
        for variant in args.variants: lifecycle(variant)
        if args.out: args.out.write_text(json.dumps(results,default=lambda b:b.decode(),indent=2)+"\n")
    finally: stop()
