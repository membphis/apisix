#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify raw hybrid samples and report paired QPS/latency without CI claims."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import statistics

VARIANTS=["lua-both","c-adapter-both","c-hybrid","c-no-metrics"]

def analyze(root):
    final=root/"final";result=[];resources=None;common=None
    module_sha=hashlib.sha256((final/"module.so").read_bytes()).hexdigest()
    for i in (1,2):
        for variant in VARIANTS:
            p=final/f"round-{i}-{variant}";rs=json.loads((p/"results.json").read_text());assert len(rs)==1
            r=rs[0];stem=r["mode"]+"-auth";raw=(p/(stem+".txt")).read_text()
            assert r["duration_seconds"]==60 and r["error_rate"]==0
            assert r["requests"]==int(re.search(r"(\d+) requests in",raw)[1])
            assert r["qps"]==float(re.search(r"Requests/sec:\s+([\d.]+)",raw)[1])
            assert not re.search(r"Socket errors:|Non-2xx or 3xx",raw)
            assert not (p/(stem+"-error.log")).read_bytes()
            assert set(r["latencies_us"])=={"mean","p50","p95","p99","p999"}
            for k,v in re.findall(r"\b(mean|p50|p95|p99|p999)=([\d.]+)",raw):assert r["latencies_us"][k]==float(v)
            env=json.loads((p/"environment.json").read_text());build=json.loads((p/"build.json").read_text())
            assert build["module_sha256"]==env["hashes"]["/opt/apisix-core-module/lib/ngx_http_apisix_core_module.so"]==module_sha
            assert not any(build[k] for k in ["sanitizer","function_profile","route_profile"])
            assert build["source_hashes"]=={Path(k).name:v for k,v in env["native_sources_sha256"].items()}
            cfg=json.loads((p/(stem+"-config.yaml")).read_text());data=json.loads((p/(stem+"-resources.json")).read_text())
            normalized={k:v for k,v in env.items() if k not in ["suite","variant","native_directives","scrape"]}
            if common is None:common=normalized
            assert common==normalized
            assert "ai" not in cfg["plugins"]
            if variant!="c-no-metrics":
                assert cfg["plugins"]==["key-auth","prometheus"] and len(data["global_rules"])==1
                if resources is None:resources=data
                assert data==resources
                m=r["metric_validation"];assert m["metric_errors"] and all(x==0 for x in m["metric_errors"])
                assert m["request_histogram_count"]==sum(m["status_counts"].values())
                assert 0<=m["server_request_excess"]<=m["cutoff_allowance"]==128
            else:assert cfg["plugins"]==["key-auth"] and not data["global_rules"]
            ngx=(p/(stem+"-nginx.conf")).read_text()
            if variant in ["c-hybrid","c-no-metrics"]:
                assert not re.search(r"\b(?:access|header_filter|body_filter|balancer)_by_lua",ngx)
                assert ngx.count('require("apisix.native.runtime").native_log()')==(variant=="c-hybrid")
            if variant=="lua-both":assert "load_module" not in ngx
            result.append({**r,"round":i,"variant":variant})
    means={v:{"qps":statistics.mean(r["qps"] for r in result if r["variant"]==v),
              "cpu_us":statistics.mean(r["worker_cpu_us_per_request"] for r in result if r["variant"]==v),
              "peak_rss_mib":statistics.mean(r["rss_peak_bytes"] for r in result if r["variant"]==v)/1048576,
              "latencies_us":{k:statistics.mean(r["latencies_us"][k] for r in result if r["variant"]==v) for k in ["mean","p50","p95","p99","p999"]}} for v in VARIANTS}
    comparisons=[]
    for candidate,control in [("c-adapter-both","lua-both"),("c-hybrid","lua-both"),("c-hybrid","c-adapter-both"),("c-hybrid","c-no-metrics")]:
        per=[]
        for i in (1,2):
            c=next(r for r in result if r["round"]==i and r["variant"]==candidate);b=next(r for r in result if r["round"]==i and r["variant"]==control);per.append((c["qps"]/b["qps"]-1)*100)
        comparisons.append({"candidate":candidate,"control":control,"qps_delta_percent":(means[candidate]["qps"]/means[control]["qps"]-1)*100,"per_round_qps_delta_percent":per,
                            "latency_delta_percent":{k:(means[candidate]["latencies_us"][k]/means[control]["latencies_us"][k]-1)*100 for k in ["mean","p95","p99","p999"]}})
    return {"sample_count":len(result),"module_sha256":module_sha,"means":means,"comparisons":comparisons,"rows":result,
            "raw_verification_passed":True,"with_metrics_resources_identical":True,
            "throttled_samples":[{"round":r["round"],"variant":r["variant"],"host":r["host_cgroup_cpu_delta"]} for r in result if r["host_cgroup_cpu_delta"].get("nr_throttled",0)],
            "limits":"Two reversed-order 60s rounds; no statistical-significance claim. Percentiles averaged per run, not pooled. Closed-loop each at its achieved QPS. Common pinned runtime, not stock stable release image. No-metrics control is cost reference only."}

if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument("root",type=Path);args=parser.parse_args();r=analyze(args.root)
    (args.root/"paired-summary.json").write_text(json.dumps(r,indent=2)+"\n")
    print(json.dumps({k:v for k,v in r.items() if k!="rows"},indent=2))
