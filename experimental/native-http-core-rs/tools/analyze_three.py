#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Summarize bench_three.py results.json: per-round values, means and ratios.

Prints Markdown tables and writes summary.json next to the (first) results.json,
or to `--summary`. Several results files with the same rates are merged, e.g.
a batch plus its gray-zone extra pair; `--variants` keeps only some variants.
Gate G1: Rust v2 within 5% of C v2 on mean throughput and on fixed-rate p99
(or within the round-to-round spread); gray zone 3%-8% triggers an extra pair.
"""
import argparse
import json
from pathlib import Path
import statistics

NAMES = {"lua-core": "Lua", "native-c-lua-key-auth": "C core + Lua key-auth",
         "native-rust-lua-key-auth": "Rust core + Lua key-auth",
         "native-c-pipeline": "C v2", "native-rust-pipeline": "Rust v2"}


def mean(values):
    return statistics.fmean(values) if values else float("nan")


def pct(new, old):
    return (new / old - 1) * 100


def main(paths, keep=None, summary_path=None):
    batches = [json.loads(Path(path).read_text()) for path in paths]
    data = {"rates": batches[0]["rates"], "samples": []}
    for batch in batches:
        assert batch["rates"] == data["rates"], "merged batches must use the same rates"
        data["samples"].extend(s for s in batch["samples"] if not keep or s["variant_name"] in keep)
    samples = data["samples"]
    keys = [(s["variant_name"], s["round"]) for s in samples]
    assert len(keys) == len(set(keys)), "duplicate variant/round in the merged samples"
    variants = [v for v in NAMES if any(s["variant_name"] == v for s in samples)]
    rounds = sorted({s["round"] for s in samples})
    by = {(s["variant_name"], s["round"]): s for s in samples}

    def series(variant, getter):
        return [getter(by[(variant, r)]) for r in rounds if (variant, r) in by]

    def fixed(sample, label):
        return next((f for f in sample["fixed_rate"] if f["label"] == label), None)

    metrics = {
        "qps": lambda s: s["qps"],
        "worker_cpu_us_per_request": lambda s: s["worker_cpu_us_per_request"],
        "rss_peak_mib": lambda s: s["rss_peak_bytes"] / 2**20,
        "closed_p99_us": lambda s: s["latencies_us"]["p99"],
        "low_p50_us": lambda s: fixed(s, "fixed-low")["latency_us"]["p50"],
        "low_p99_us": lambda s: fixed(s, "fixed-low")["latency_us"]["p99"],
        "low_p999_us": lambda s: fixed(s, "fixed-low")["latency_us"]["p999"],
        "low_cpu_us_per_request": lambda s: fixed(s, "fixed-low")["worker_cpu_us_per_request"],
        "low_cpu_utilization": lambda s: fixed(s, "fixed-low")["worker_cpu_utilization"],
    }
    high = {
        "high_p50_us": lambda s: fixed(s, "fixed-high")["latency_us"]["p50"],
        "high_p99_us": lambda s: fixed(s, "fixed-high")["latency_us"]["p99"],
        "high_p999_us": lambda s: fixed(s, "fixed-high")["latency_us"]["p999"],
        "high_cpu_us_per_request": lambda s: fixed(s, "fixed-high")["worker_cpu_us_per_request"],
        "high_cpu_utilization": lambda s: fixed(s, "fixed-high")["worker_cpu_utilization"],
    }
    summary = {"rates": data["rates"], "rounds": rounds, "sources": [str(Path(p).parent.name) for p in paths],
               "variants": {}}
    for variant in variants:
        entry = {}
        for name, getter in metrics.items():
            values = series(variant, getter)
            entry[name] = {"rounds": values, "mean": mean(values)}
        if any(fixed(by[(variant, r)], "fixed-high") for r in rounds if (variant, r) in by):
            for name, getter in high.items():
                values = series(variant, getter)
                entry[name] = {"rounds": values, "mean": mean(values)}
        summary["variants"][variant] = entry

    v = summary["variants"]
    comparisons = {}
    if "native-rust-pipeline" in v and "native-c-pipeline" in v:
        rust, c = v["native-rust-pipeline"], v["native-c-pipeline"]
        comparisons["rust_vs_c"] = {name: pct(rust[name]["mean"], c[name]["mean"]) for name in rust if name in c}
    if "native-rust-lua-key-auth" in v and "native-c-lua-key-auth" in v:
        rust, c = v["native-rust-lua-key-auth"], v["native-c-lua-key-auth"]
        comparisons["rust_step1_vs_c_step1"] = {name: pct(rust[name]["mean"], c[name]["mean"]) for name in rust if name in c}
    for native in [name for name in NAMES if name != "lua-core"]:
        if native in v and "lua-core" in v:
            comparisons[f"{native}_vs_lua"] = {name: pct(v[native][name]["mean"], v["lua-core"][name]["mean"])
                                               for name in metrics}
    summary["comparisons"] = comparisons
    output = Path(summary_path) if summary_path else Path(paths[0]).with_name("summary.json")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(summary, indent=2) + "\n")

    print(f"R_low = {data['rates'][0]} rps, R_high = {data['rates'][1]} rps, rounds = {rounds}\n")
    print("| Metric | " + " | ".join(NAMES[x] for x in variants) + " |")
    print("|---|" + "---:|" * len(variants))
    for name in list(metrics) + list(high):
        cells = []
        for variant in variants:
            item = v[variant].get(name)
            if not item:
                cells.append("—")
                continue
            rounds_text = " / ".join(f"{x:.2f}" if x < 100 else f"{x:.0f}" for x in item["rounds"])
            cells.append(f"{item['mean']:.2f} ({rounds_text})" if item["mean"] < 100 else f"{item['mean']:.0f} ({rounds_text})")
        print(f"| {name} | " + " | ".join(cells) + " |")
    print()
    for key, values in comparisons.items():
        print(f"{key}: " + ", ".join(f"{k} {x:+.2f}%" for k, x in values.items()))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("results", nargs="+", help="results.json files of one batch and its extra pairs")
    parser.add_argument("--variants", nargs="+", choices=list(NAMES), help="keep only these variants")
    parser.add_argument("--summary", help="summary.json path (default: next to the first results file)")
    args = parser.parse_args()
    main(args.results, args.variants, args.summary)
