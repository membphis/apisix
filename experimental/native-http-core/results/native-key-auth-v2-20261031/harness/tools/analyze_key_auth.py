#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify and summarize the frozen final key-auth comparison, without CI claims."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import statistics

VARIANTS = ["lua-core", "native-lua", "native-c-adapter", "native-c-pipeline"]

def analyze(root):
    final = root / "final"
    rows, environments = [], []
    resources = None
    module_sha = hashlib.sha256((final / "module.so").read_bytes()).hexdigest()
    for round_id in (1, 2):
        for variant in VARIANTS:
            path = final / f"round-{round_id}-{variant}"
            entries = json.loads((path / "results.json").read_text())
            assert len(entries) == 1
            r = entries[0]
            assert r["duration_seconds"] == 60 and r["error_rate"] == 0
            assert r["prefix"] == "auth" and r["plugin"] == "key-auth"
            stem = r["mode"] + "-auth"
            text = (path / (stem + ".txt")).read_text()
            assert float(re.search(r"Requests/sec:\s+([\d.]+)", text)[1]) == r["qps"]
            assert int(re.search(r"(\d+) requests in", text)[1]) == r["requests"]
            assert not re.search(r"Socket errors:|Non-2xx or 3xx", text)
            assert not (path / (stem + "-error.log")).read_bytes()
            resource = json.loads((path / (stem + "-resources.json")).read_text())
            if resources is None: resources = resource
            assert resource == resources, path
            config = json.loads((path / (stem + "-config.yaml")).read_text())
            assert config["plugins"] == ["key-auth"] and not resource["global_rules"]
            ngx = (path / (stem + "-nginx.conf")).read_text()
            if variant == "native-c-pipeline":
                assert not re.search(r"\b(?:access|header_filter|body_filter|log|balancer)_by_lua", ngx)
                assert "apisix_core_native_balancer;" in ngx
            env = json.loads((path / "environment.json").read_text())
            build = json.loads((path / "build.json").read_text())
            assert env["hashes"]["/opt/apisix-core-module/lib/ngx_http_apisix_core_module.so"] == module_sha == build["module_sha256"]
            assert not build["sanitizer"] and not build["function_profile"] and not build["route_profile"]
            assert build["source_hashes"] == {Path(k).name: v for k, v in env["native_sources_sha256"].items()}
            environments.append({k: v for k, v in env.items() if k not in ("variant", "native_directives")})
            rows.append({**r, "round": round_id, "variant": variant})
    assert all(env == environments[0] for env in environments)
    means = {variant: {field: statistics.mean(r[field] for r in rows if r["variant"] == variant)
                       for field in ("qps", "worker_cpu_us_per_request", "rss_peak_bytes")}
             for variant in VARIANTS}
    pairs = [("native-lua", "lua-core"), ("native-c-adapter", "native-lua"),
             ("native-c-pipeline", "native-c-adapter"), ("native-c-pipeline", "native-lua"),
             ("native-c-pipeline", "lua-core")]
    comparisons = []
    for candidate, control in pairs:
        deltas = []
        for i in (1, 2):
            c = next(r for r in rows if r["variant"] == candidate and r["round"] == i)
            b = next(r for r in rows if r["variant"] == control and r["round"] == i)
            deltas.append((c["qps"] / b["qps"] - 1) * 100)
        comparisons.append({"candidate": candidate, "control": control,
                            "mean_qps_delta_percent": (means[candidate]["qps"] / means[control]["qps"] - 1) * 100,
                            "per_round_delta_percent": deltas})
    return {"sample_count": len(rows), "module_sha256": module_sha,
            "means": means, "comparisons": comparisons,
            "throttled_samples": [{"round": r["round"], "variant": r["variant"],
                                   "host_cpu_delta": r["host_cgroup_cpu_delta"]}
                                  for r in rows if r["host_cgroup_cpu_delta"].get("nr_throttled", 0)],
            "raw_verification_passed": True, "same_resources_and_runtime": True,
            "rows": rows,
            "limitations": "Two reversed-order rounds, no statistical-significance claim. Common pinned historical dependencies, not a stock release image. C-only mode has an explicit documented capability contract."}

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__); parser.add_argument("root", type=Path)
    args = parser.parse_args(); result = analyze(args.root)
    (args.root / "paired-summary.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({k: v for k, v in result.items() if k != "rows"}, indent=2))
