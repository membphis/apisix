#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Pinned-container hybrid key-auth/Prometheus validation and paired evidence."""
from datetime import datetime, timezone
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

HERE=Path(__file__).resolve().parents[1];sys.path.insert(0,str(HERE/"tests"))
from test_hybrid_prometheus import VARIANTS
OUT=Path(os.environ.get("APISIX_HYBRID_OUT","/tmp/apisix-hybrid-prometheus-20261031/final"))
ENV={**os.environ,"APISIX_CORE_SOURCE":"/stable","APISIX_CORE_SOURCE_REVISION":"473043623c161966f3f6ef4e8f12d2d8667a12d2","APISIX_CORE_OPTIMIZATION":"O3"}

def run(label,script,*args,extra=None):
    OUT.mkdir(parents=True,exist_ok=True)
    with (OUT/(label+".log")).open("x") as log:
        print(label,flush=True)
        r=subprocess.run(["python3",str(HERE/script),*map(str,args)],env={**ENV,**(extra or {})},stdout=log,stderr=subprocess.STDOUT)
    with (OUT/"events.jsonl").open("a") as log:log.write(json.dumps({"utc":datetime.now(timezone.utc).isoformat(),"label":label,"returncode":r.returncode})+"\n")
    if r.returncode:
        print((OUT/(label+".log")).read_text()[-5000:],flush=True);raise RuntimeError(label+" failed; evidence retained")

def build(label,extra=None):
    run(label,"tools/rebuild.py",extra=extra)
    shutil.copyfile("/tmp/apisix-core-compile.log",OUT/(label+"-compile.log"))
    shutil.copyfile("/opt/apisix-core-module/lib/build.json",OUT/(label+"-build.json"))

def validate():
    build("asan-build",{"APISIX_CORE_SANITIZER":"address,undefined"})
    run("asan-hybrid","tests/test_hybrid_prometheus.py","--out",OUT/"asan-hybrid.json",extra={"APISIX_CORE_SANITIZER":"address,undefined"})
    run("asan-v2-regression","tests/test_native_key_auth.py","--out",OUT/"asan-v2-regression.json",extra={"APISIX_CORE_SANITIZER":"address,undefined"})
    for profile in ["default","tail","encoded","servlet","trusted","untrusted"]:
        run("asan-core-"+profile,"tests/test_core.py",extra={"APISIX_CORE_SANITIZER":"address,undefined","APISIX_CORE_PROFILE":profile})
    build("final-o3")
    run("final-hybrid","tests/test_hybrid_prometheus.py","--out",OUT/"final-hybrid.json")
    shutil.copyfile("/opt/apisix-core-module/lib/ngx_http_apisix_core_module.so",OUT/"module.so")
    shutil.copytree(HERE/"module",OUT/"module-source")
    shutil.copyfile(HERE/"runtime.lua",OUT/"runtime.lua")

def bench():
    plan={"variants":VARIANTS,"orders":[list(VARIANTS),list(reversed(VARIANTS))],"sample_seconds":60,"warmup_seconds":5,"scrape":"1 Hz for all three with-Prometheus paths; none for no-metrics control","tuning":"all frozen variants reported; no throughput-based selection"}
    with (OUT/"comparison-plan.json").open("x") as f:json.dump(plan,f,indent=2)
    for i,order in enumerate(plan["orders"],1):
        for variant in order:
            mode,flags,suite=VARIANTS[variant];label=f"round-{i}-{variant}"
            run(label,"tools/benchmark.py","--suite",suite,"--prefixes","auth","--modes",mode,"--variant",label,"--out",OUT/label,"--stop-after",extra={"APISIX_CORE_DIRECTIVES":flags})

def diagnostics():
    for sampler,variants in [("alloc",["c-adapter-both","c-hybrid","c-no-metrics"]),("functions",["c-adapter-both","c-hybrid"])]:
        for variant in variants:
            mode,flags,suite=VARIANTS[variant];suite="hybrid" if suite=="hybrid" else "isolated-single";label=sampler+"-"+variant
            run(label,"tools/profile.py",mode,"--suite",suite,"--prefix","auth","--sampler",sampler,extra={"APISIX_CORE_DIRECTIVES":flags})
            prefix=Path(f"/tmp/apisix-core-profile-{mode}-{suite}-auth-{sampler}.txt")
            for f in prefix.parent.glob(prefix.name+"*"):shutil.copyfile(f,OUT/(label+f.name[len(prefix.name):]+".raw"))
            shutil.copyfile("/opt/apisix-core-module/lib/build.json",OUT/(label+"-build.json"))
            if sampler=="functions" and not (OUT/"function-module.so").exists():shutil.copyfile("/opt/apisix-core-module/lib/ngx_http_apisix_core_module.so",OUT/"function-module.so")
            if sampler=="alloc" and not (OUT/"allocator-sampler.so").exists():shutil.copyfile("/tmp/apisix-allocator-sampler.so",OUT/"allocator-sampler.so")
    build("restore-o3")
    assert hashlib.sha256(Path("/opt/apisix-core-module/lib/ngx_http_apisix_core_module.so").read_bytes()).hexdigest()==hashlib.sha256((OUT/"module.so").read_bytes()).hexdigest()

if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument("action",choices=["validate","bench","diagnostics"]);args=parser.parse_args();globals()[args.action]()
