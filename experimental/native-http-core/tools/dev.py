#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compile in one disposable container; avoid a VFS image snapshot per edit."""
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[3]
NAME = "apisix-http-core-dev"
SANITIZER = os.environ.get("APISIX_CORE_SANITIZER", "")
ENV = os.environ.copy()
for key in ("DOCKER_HOST", "DOCKER_CONTEXT", "DOCKER_TLS", "DOCKER_TLS_VERIFY", "DOCKER_CERT_PATH"):
    ENV.pop(key, None)
DOCKER = ["docker", "--host=unix:///var/run/docker.sock"]


def docker(*args, **kwargs):
    return subprocess.run(DOCKER + list(args), env=ENV, check=True, **kwargs)


def main():
    found = subprocess.run(DOCKER + ["container", "inspect", NAME], env=ENV,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if found.returncode:
        docker("run", "-d", "--init", "--name", NAME, "--ulimit", "nofile=65536:65536",
               "--label", "org.apache.apisix.experimental.native-http-core=1",
               "-v", f"{ROOT}:/src:ro", "--entrypoint", "sleep", "apisix-http-core:local", "infinity")
    docker("exec", "-e", "APISIX_CORE_SANITIZER=" + SANITIZER, NAME, "python3",
           "/src/experimental/native-http-core/tools/rebuild.py")
    for profile in ("default", "servlet", "tail", "encoded", "trusted", "untrusted"):
        docker("exec", "-e", "APISIX_CORE_PROFILE=" + profile,
               "-e", "APISIX_CORE_SANITIZER=" + SANITIZER, NAME, "python3",
               "/src/experimental/native-http-core/tests/test_core.py")
    docker("exec", "-e", "APISIX_CORE_SANITIZER=" + SANITIZER, NAME, "python3",
           "/src/experimental/native-http-core/tests/test_demo.py")


if __name__ == "__main__":
    main()
