#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Inside the pinned image: correctness, four-way 60s comparison and diagnostics."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
from datetime import datetime, timezone

HERE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HERE / "tests"))
from test_native_key_auth import VARIANTS

OUT = Path(os.environ.get("APISIX_KEY_AUTH_OUT", "/tmp/apisix-native-key-auth-v2-20261031"))
ENV = {**os.environ, "APISIX_CORE_SOURCE": "/stable",
       "APISIX_CORE_SOURCE_REVISION": "473043623c161966f3f6ef4e8f12d2d8667a12d2",
       "APISIX_CORE_OPTIMIZATION": "O3"}

def run(label, script, *args, extra=None):
    OUT.mkdir(exist_ok=True, parents=True)
    with (OUT / (label + ".log")).open("x") as log:
        print(label, flush=True)
        result = subprocess.run(["python3", str(HERE / script), *map(str, args)],
                                env={**ENV, **(extra or {})}, stdout=log, stderr=subprocess.STDOUT)
    with (OUT / "events.jsonl").open("a") as log:
        log.write(json.dumps({"utc": datetime.now(timezone.utc).isoformat(), "label": label,
                              "returncode": result.returncode}) + "\n")
    if result.returncode:
        print((OUT / (label + ".log")).read_text()[-6000:], flush=True)
        raise RuntimeError(label + " failed; evidence retained")

def build(label, extra=None):
    run(label, "tools/rebuild.py", extra=extra)
    for source, target in [("/tmp/apisix-core-compile.log", label + "-compile.log"),
                           ("/opt/apisix-core-module/lib/build.json", label + "-build.json")]:
        shutil.copyfile(source, OUT / target)

def validate(skip_core=False):
    build("asan-build", {"APISIX_CORE_SANITIZER": "address,undefined"})
    run("asan-four-way", "tests/test_native_key_auth.py", "--out", OUT / "asan-semantics.json",
        extra={"APISIX_CORE_SANITIZER": "address,undefined"})
    # Default-off path retains the original full core/plug-in contracts.
    for profile in (() if skip_core else ("default", "tail", "encoded", "servlet", "trusted", "untrusted")):
        run("asan-core-" + profile, "tests/test_core.py", extra={"APISIX_CORE_PROFILE": profile,
            "APISIX_CORE_SANITIZER": "address,undefined"})
    build("final-o3")
    run("final-four-way", "tests/test_native_key_auth.py", "--out", OUT / "final-semantics.json")
    shutil.copyfile("/opt/apisix-core-module/lib/ngx_http_apisix_core_module.so", OUT / "module.so")
    shutil.copytree(HERE / "module", OUT / "module-source")
    shutil.copyfile(HERE / "runtime.lua", OUT / "runtime.lua")

def bench():
    # A complete reversed order, not choosing an implementation after screening.
    orders = [list(VARIANTS), list(reversed(VARIANTS))]
    for round_id, order in enumerate(orders, 1):
        for variant in order:
            mode, directives = VARIANTS[variant]
            label = f"round-{round_id}-{variant}"
            run(label, "tools/benchmark.py", "--suite", "single", "--prefixes", "auth",
                "--modes", mode, "--variant", label, "--out", OUT / label, "--stop-after",
                extra={"APISIX_CORE_DIRECTIVES": directives})

def diagnostics():
    for sampler in ("alloc", "functions"):
        for variant in ("native-lua", "native-c-adapter", "native-c-pipeline"):
            mode, directives = VARIANTS[variant]
            label = sampler + "-" + variant
            run(label, "tools/profile.py", mode, "--suite", "isolated-single", "--prefix", "auth",
                "--sampler", sampler, extra={"APISIX_CORE_DIRECTIVES": directives})
            prefix = Path(f"/tmp/apisix-core-profile-{mode}-isolated-single-auth-{sampler}.txt")
            for file in prefix.parent.glob(prefix.name + "*"):
                shutil.copyfile(file, OUT / (label + file.name[len(prefix.name):] + ".raw"))
            shutil.copyfile("/opt/apisix-core-module/lib/build.json", OUT / (label + "-build.json"))
            if sampler == "functions" and not (OUT / "function-module.so").exists():
                shutil.copyfile("/opt/apisix-core-module/lib/ngx_http_apisix_core_module.so", OUT / "function-module.so")
            if sampler == "alloc" and not (OUT / "allocator-sampler.so").exists():
                shutil.copyfile("/tmp/apisix-allocator-sampler.so", OUT / "allocator-sampler.so")
    build("restore-o3")
    assert hashlib.sha256(Path("/opt/apisix-core-module/lib/ngx_http_apisix_core_module.so").read_bytes()).hexdigest() == hashlib.sha256((OUT / "module.so").read_bytes()).hexdigest()

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=["validate", "bench", "diagnostics"])
    parser.add_argument("--skip-core", action="store_true", help="reuse already-passed default-off core checks after a C-only path change")
    args = parser.parse_args()
    if args.action == "validate": validate(args.skip_core)
    else: globals()[args.action]()
