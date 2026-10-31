#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Three paired 60-second limit-count samples, with a strict plugin allowlist.

Run inside the existing demo container. Reuse the original sampler and traffic
verbatim; its unused apikey header does not enable authentication. Keep the
original four URI patterns, disable every unmeasured route plugin, remove
consumers, and enable only global Prometheus plus the measured limit-count.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import subprocess

import benchmark as bench
import demo
from test_demo import request, ready

ENABLED = ["prometheus", "limit-count"]


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def check_configuration(directory, benchmark):
    config = json.loads((demo.ROOT / "conf/config.yaml").read_text())
    resources = json.loads((demo.ROOT / "conf/apisix.json").read_text())
    native = json.loads((demo.ROOT / "conf/native.json").read_text())
    assert config["plugins"] == ENABLED
    assert native["plugins"] == ENABLED
    assert native["local_conf"]["plugins"] == ENABLED
    assert resources["global_rules"] == [{"id": "metrics", "plugins": {"prometheus": {}}}]
    assert resources["consumers"] == []
    assert [r["uri"] for r in resources["routes"]] == ["/plain/*", "/auth/*", "/limit/*", "/combined/*"]
    for route in resources["routes"]:
        assert set(route["plugins"]) == ({"limit-count"} if route["id"] == "limit" else set())
    route = next(r for r in resources["routes"] if r["id"] == "limit")
    limit = route["plugins"]["limit-count"]
    assert limit == {"count": 1_000_000_000 if benchmark else 5,
                     "time_window": 600 if benchmark else 60,
                     "key_type": "constant", "key": "demo", "policy": "local", "rejected_code": 429}
    for name in ["config.yaml", "apisix.json", "native.json", "nginx.conf"]:
        (directory / name).write_bytes((demo.ROOT / "conf" / name).read_bytes())
    (directory / "configuration-check.json").write_text(json.dumps({
        "enabled_plugins": ENABLED, "global_plugins": ["prometheus"],
        "measured_route_plugins": ["limit-count"], "key_auth_enabled": False,
        "ai_plugins_enabled": [], "consumers": 0, "limit": limit,
        "original_uri_patterns_retained": True,
        "unused_original_apikey_request_header_retained": True}, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--source-commit", required=True)
    args = parser.parse_args()
    out = args.output
    assert re.fullmatch(r"[0-9a-f]{40}", args.source_commit)
    out.mkdir(parents=True, exist_ok=False)
    reference = json.loads((demo.HERE / "results/environment.json").read_text())
    expected = reference["hashes"][demo.MODULE]
    assert digest(demo.MODULE) == expected, "module differs from the earlier 11.1% measurement"
    for relative, expected_source in reference["native_sources_sha256"].items():
        assert digest(demo.HERE / relative) == expected_source, relative
    symbols = subprocess.check_output(["nm", "-D", demo.MODULE], text=True)
    assert not any(s in symbols for s in ["__asan", "__ubsan", "ac_function_profile_start"])
    fixture = json.loads((demo.HERE / "example.json").read_text())
    fixture["consumers"] = []
    for route in fixture["routes"]:
        if route["id"] != "limit":
            route["plugins"] = {}
    fixture_path = out / "fixture.json"
    fixture_path.write_text(json.dumps(fixture, indent=2) + "\n")
    cpu = sorted(os.sched_getaffinity(0))
    assert cpu == reference["available_cpus"], "CPU affinity set differs from reference"
    assert len(cpu) >= 5
    environment = {
        "started_utc": datetime.now(timezone.utc).isoformat(),
        "source_commit": args.source_commit,
        "kernel": platform.platform(), "available_cpus": cpu,
        "cpu_model": [s for s in Path("/proc/cpuinfo").read_text().splitlines() if s.startswith("model name")][:1],
        "runtime": subprocess.check_output(["openresty", "-V"], stderr=subprocess.STDOUT, text=True),
        "enabled_plugins": ENABLED, "module_sha256": digest(demo.MODULE),
        "reference_module_sha256": expected,
        "sampler_sha256": digest(Path(__file__).with_name("benchmark.py")),
        "recheck_sha256": digest(__file__), "demo_sha256": digest(Path(__file__).with_name("demo.py")),
        "lua_bridge_sha256": digest(demo.HERE / "runtime.lua"),
        "plugin_sha256": {name: digest(demo.SOURCE / f"apisix/plugins/{name}.lua") for name in ENABLED},
        "sample_seconds": 60, "warmup_seconds": 5,
        "orders": [["lua", "native"], ["native", "lua"], ["lua", "native"]],
        "reference_limit_qps": {r["mode"]: r["qps"] for r in json.loads((demo.HERE / "results/results.json").read_text()) if r["prefix"] == "limit"}}
    (out / "environment.json").write_text(json.dumps(environment, indent=2) + "\n")
    current = None

    def prepare(mode, **kwargs):
        demo.prepare(mode, fixture=fixture_path, enabled_plugins=ENABLED, **kwargs)
        ready()
        check_configuration(current, benchmark=True)
        assert request("/auth/no-key-auth-configured")[0] == 200
        code, headers, body = request("/limit/preflight")
        headers = {k.lower(): v for k, v in headers.items()}
        assert code == 200 and body == b"ok\n", (code, body)
        assert headers["x-ratelimit-limit"] == "1000000000", headers
        assert int(headers["x-ratelimit-remaining"]) < 1_000_000_000
        print("PASS strict configuration:", mode, ENABLED, flush=True)

    bench.prepare = prepare
    rows = []
    demo.stop(demo.UPSTREAM)
    try:
        for mode in ["lua", "native"]:
            current = out / "preflight" / mode
            current.mkdir(parents=True)
            prepare(mode, benchmark=True, gateway_cpu=cpu[0], upstream_cpu=cpu[1])
        for round_index, order in enumerate(environment["orders"], 1):
            for mode in order:
                current = out / f"round-{round_index}" / mode
                current.mkdir(parents=True)
                bench.OUT = current
                print(f"START round={round_index}/3 mode={mode} duration=60s", flush=True)
                row = bench.sample(mode, "limit", cpu)
                row["round"] = round_index
                rows.append(row)
                (out / "results.json").write_text(json.dumps(rows, indent=2) + "\n")
                print(f"DONE round={round_index}/3 mode={mode} qps={row['qps']}", flush=True)
    finally:
        demo.prepare("native", fixture=fixture_path, enabled_plugins=ENABLED)
        ready()
        restored = out / "restored-native"
        restored.mkdir()
        check_configuration(restored, benchmark=False)
    assert len(rows) == 6
    assert digest(demo.MODULE) == environment["module_sha256"]
    print("PASS all three pairs; restored strict native demo, quota=5", flush=True)


if __name__ == "__main__":
    main()
