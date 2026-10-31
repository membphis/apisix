#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Host entry point for the isolated, localhost-only demo container."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
NAME = os.environ.get("APISIX_CORE_CONTAINER", "apisix-native-demo")
BUILDER = "apisix-http-core-dev"
ENV = os.environ.copy()
for key in ("DOCKER_HOST", "DOCKER_CONTEXT", "DOCKER_TLS", "DOCKER_TLS_VERIFY", "DOCKER_CERT_PATH"):
    ENV.pop(key, None)
DOCKER = ["docker", "--host=unix:///var/run/docker.sock"]


def docker(*args, **kwargs):
    return subprocess.run(DOCKER + list(args), env=ENV, check=True, **kwargs)


def ensure():
    found = subprocess.run(DOCKER + ["container", "inspect", NAME], env=ENV,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if found.returncode:
        docker("run", "-d", "--init", "--name", NAME, "--ulimit", "nofile=65536:65536",
               "--label", "org.apache.apisix.experimental.native-http-core=1",
               "-p", "127.0.0.1:9080:9080", "-p", "127.0.0.1:9091:9091",
               "-v", f"{ROOT}:/src:ro", "--entrypoint", "sleep", "apisix-native-demo:local", "infinity")
    else:
        running = docker("inspect", "--format", "{{.State.Running}}", NAME, capture_output=True, text=True)
        if running.stdout.strip() != "true":
            docker("start", NAME)


def install_module():
    # Reuse the current build without VFS image snapshots for each edit.
    with tempfile.TemporaryDirectory(prefix="apisix-core-module-") as directory:
        path = str(Path(directory) / "ngx_http_apisix_core_module.so")
        docker("cp", BUILDER + ":/opt/apisix-core-module/lib/ngx_http_apisix_core_module.so", path)
        symbols = subprocess.check_output(["nm", "-D", path], text=True)
        if any(marker in symbols for marker in ("__asan", "__ubsan", "ac_function_profile_start")):
            raise SystemExit("Run tools/dev.py without APISIX_CORE_SANITIZER before starting the demo.")
        docker("exec", NAME, "python3", "/src/experimental/native-http-core/tools/demo.py", "stop")
        docker("exec", NAME, "mkdir", "-p", "/opt/apisix-core-module/lib")
        docker("cp", path, NAME + ":/opt/apisix-core-module/lib/ngx_http_apisix_core_module.so")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=["start", "test", "bench", "stop"])
    parser.add_argument("mode", nargs="?", choices=["native", "lua"], default="native")
    parser.add_argument("--workers", type=int, choices=[1, 2, 4], default=1)
    args = parser.parse_args()
    ensure()
    if args.action != "stop":
        install_module()
    base = "/src/experimental/native-http-core/"
    if args.action == "start":
        docker("exec", NAME, "python3", base + "tools/demo.py", args.mode, "--workers", str(args.workers))
    elif args.action == "test":
        docker("exec", NAME, "python3", base + "tests/test_demo.py")
    elif args.action == "bench":
        docker("exec", NAME, "python3", base + "tools/benchmark.py")
        results = ROOT / "experimental/native-http-core/results"
        results.mkdir(exist_ok=True)
        docker("cp", NAME + ":/tmp/apisix-http-core-results/.", str(results))
    else:
        docker("exec", NAME, "python3", base + "tools/demo.py", "stop")


if __name__ == "__main__":
    main()
