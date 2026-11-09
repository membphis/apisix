#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Container-side build of the Rust module against the configured NGINX tree.

Runs inside apisix-http-core-dev (image apisix-http-core-rs:local). The C
module's rebuild.py must have configured the same NGINX source first, so both
modules share one configure invocation, struct layout and module signature.
Crates come from the host-vendored directory mounted at /vendor; the build is
offline.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

SOURCE = Path("/src/experimental/native-http-core-rs")
WORK = Path("/opt/apisix-rs-build")
TARGET = Path("/opt/apisix-rs-target")
DEST = Path("/opt/apisix-rs-module/lib")
NGINX = Path("/tmp/openresty-1.27.1.2/bundle/nginx-1.27.1")
VENDOR = Path("/vendor")

features = [f for f in os.environ.get("APISIX_RS_FEATURES", "").split(",") if f]
assert set(features) <= {"panic-test"}, features
clippy = os.environ.get("APISIX_RS_CLIPPY", "") == "1"


def copy_tree():
    if WORK.exists():
        shutil.rmtree(WORK)
    shutil.copytree(SOURCE, WORK, ignore=shutil.ignore_patterns("target", "__pycache__"))


def cargo(subcommand, *args):
    env = {**os.environ, "NGINX_SOURCE_DIR": str(NGINX), "NGINX_BUILD_DIR": str(NGINX / "objs"),
           "CARGO_TARGET_DIR": str(TARGET)}
    # Cargo options precede the subcommand arguments (which may contain `--`).
    command = ["cargo", subcommand, "--offline", "--locked",
               "--config", 'source.crates-io.replace-with="vendored-sources"',
               "--config", f'source.vendored-sources.directory="{VENDOR}"', *args]
    log = Path("/tmp/apisix-rs-compile.log")
    with log.open("a") as output:
        output.write("$ " + " ".join(command) + "\n")
        output.flush()
        result = subprocess.run(command, cwd=WORK, env=env, stdout=output, stderr=subprocess.STDOUT)
    if result.returncode:
        print(log.read_text()[-12000:])
        sys.exit(result.returncode)


def main():
    assert (NGINX / "objs/ngx_auto_config.h").is_file(), "run the C rebuild.py first to configure NGINX"
    Path("/tmp/apisix-rs-compile.log").write_text("")
    copy_tree()
    feature_args = ["--features", ",".join(features)] if features else []
    if clippy:
        cargo("clippy", "--release", "-p", "ngx_http_apisix_rs_module", *feature_args, "--", "-D", "warnings")
        cargo("clippy", "--release", "-p", "apisix-rs-core", "--all-targets", "--", "-D", "warnings")
    cargo("test", "--release", "-p", "apisix-rs-core")
    cargo("build", "--release", "-p", "ngx_http_apisix_rs_module", *feature_args)
    DEST.mkdir(parents=True, exist_ok=True)
    built = TARGET / "release/libngx_http_apisix_rs_module.so"
    binary = DEST / "ngx_http_apisix_rs_module.so"
    temporary = binary.with_suffix(".so.new")
    shutil.copyfile(built, temporary)
    os.replace(temporary, binary)  # running workers keep the old mapped inode
    sources = sorted(p for p in SOURCE.rglob("*")
                     if p.is_file() and "target" not in p.parts and "__pycache__" not in p.parts)
    metadata = {
        "profile": "release (opt-level=3, codegen-units=1, lto=fat, panic=unwind)",
        "features": features,
        "rustc": subprocess.check_output(["rustc", "--version"], text=True).strip(),
        "cargo": subprocess.check_output(["cargo", "--version"], text=True).strip(),
        "nginx_build_dir": str(NGINX / "objs"),
        "source_hashes": {str(p.relative_to(SOURCE)): hashlib.sha256(p.read_bytes()).hexdigest()
                          for p in sources},
        "module_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
    }
    (DEST / "build.json").write_text(json.dumps(metadata, indent=2) + "\n")
    print("Rust module rebuilt", ",".join(features) or "release", metadata["module_sha256"][:16])


if __name__ == "__main__":
    main()
