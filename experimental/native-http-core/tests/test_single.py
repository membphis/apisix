#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Route-only single-plugin semantics, with the same unchanged source tree in both modes."""
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import time
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from demo import ROOT, SOURCE, prepare, stop
from test_demo import request, ready, hits, metrics, counts


def begin(mode, plugin, workers=1):
    prepare(mode, workers=workers, single_plugin=plugin, test_window=3)
    ready("/missing", expected=404)
    config = json.loads((ROOT / "conf/config.yaml").read_text())
    resources = json.loads((ROOT / "conf/apisix.json").read_text())
    assert config["plugins"] == [plugin] and "ai" not in config["plugins"]
    assert not resources["global_rules"]
    assert len(resources["routes"]) == 1
    assert list(resources["routes"][0]["plugins"]) == [plugin]


def connection_pair(prefix, headers=None):
    headers = {"X-Demo-Echo": "1", **(headers or {})}
    replies = [request(f"/{prefix}/echo", headers=headers) for _ in range(2)]
    assert all(reply[0] == 200 for reply in replies), replies
    payloads = [json.loads(reply[2]) for reply in replies]
    assert payloads[0]["connection"] == payloads[1]["connection"], "keepalive changed"
    return payloads


def suite(mode):
    begin(mode, "prometheus")
    time.sleep(2)
    before = counts(metrics())
    connection_pair("metrics")
    assert request("/metrics/failure?code=503")[0] == 503
    assert request("/outside-route")[0] == 404
    expected = Counter({200: 2, 503: 1})
    for _ in range(50):
        text = metrics()
        delta = counts(text) - before
        # Counter and histogram families can become visible in different
        # exporter cache refreshes. Wait for the full condition being tested.
        if delta == expected and "apisix_http_latency" in text and "apisix_bandwidth" in text:
            break
        time.sleep(0.1)
    assert delta == expected, (mode, "route-only metrics", delta, expected)
    assert 'route="metrics"' in text
    assert "apisix_http_latency" in text and "apisix_bandwidth" in text, (mode, text)
    for line in text.splitlines():
        if "metric_errors_total" in line and not line.startswith("#"):
            assert float(line.rsplit(" ", 1)[1]) == 0, line

    begin(mode, "key-auth")
    for payload in connection_pair("auth", {"apikey": "demo-key"}):
        assert payload["headers"]["x-consumer-username"] == "demo-user"
        assert "apikey" not in payload["headers"]
    code, _, body = request("/auth/echo?apikey=demo-key&keep=yes", {"X-Demo-Echo": "1"})
    assert code == 200 and json.loads(body)["uri"] == "/auth/echo?keep=yes"
    sent = hits()
    assert request("/auth/rejected")[0] == 401
    assert request("/auth/rejected", {"apikey": "invalid"})[0] == 401
    assert hits() == sent
    assert request("/authentic/path", {"apikey": "demo-key"})[0] == 404

    begin(mode, "limit-count")
    connection_pair("limit")
    for remaining in (2, 1, 0):
        code, headers, _ = request("/limit/item")
        assert code == 200 and headers["X-RateLimit-Remaining"] == str(remaining)
    sent = hits()
    assert request("/limit/item")[0] == 429
    assert hits() == sent
    time.sleep(3.1)
    assert request("/limit/item")[0] == 200
    begin(mode, "limit-count", workers=2)
    with ThreadPoolExecutor(max_workers=16) as pool:
        outcomes = Counter(pool.map(lambda _: request("/limit/concurrent")[0], range(32)))
    assert outcomes == {200: 5, 429: 27}, (mode, outcomes)
    print(f"PASS {mode}: isolated prometheus, key-auth, local limit-count; ai disabled; "
          "metrics/auth/quota/2-worker shared quota/keepalive", flush=True)


if __name__ == "__main__":
    print("Single-plugin source:", SOURCE, flush=True)
    try:
        for mode in ("lua", "native"):
            suite(mode)
    finally:
        stop()
