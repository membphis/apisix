#!/usr/bin/env python3
"""Prepare a fresh, pinned source tree for the native response-stage experiment."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import urllib.request

HERE = Path(__file__).resolve().parent
PACKAGES = [
    ("https://openresty.org/download/openresty-1.27.1.2.tar.gz", "openresty-1.27.1.2.tar.gz",
     "74f076f7e364b2a99a6c5f9bb531c27610c78985abe956b442b192a2295f7548"),
    ("https://codeload.github.com/api7/apisix-nginx-module/tar.gz/refs/tags/1.19.2", "apisix-nginx-module-1.19.2.tar.gz",
     "f95d621d80766d55bcea1ef7612411e776991fe6232427c7156b65e5cc6ea6e6"),
    ("https://codeload.github.com/api7/ngx_multi_upstream_module/tar.gz/refs/tags/1.3.2", "ngx_multi_upstream_module-1.3.2.tar.gz",
     "be9b69e744d91c7313d764c01fc419844ffb7e5534964f42d5701f0417b8b8e7"),
    ("https://codeload.github.com/api7/mod_dubbo/tar.gz/refs/tags/1.0.2", "mod_dubbo-1.0.2.tar.gz",
     "1fb02fccb89a042ebdd2a0da26c050ea9a3807e75fdbf67e79da91eaea0e6953"),
    ("https://codeload.github.com/api7/lua-var-nginx-module/tar.gz/refs/tags/v0.5.3", "lua-var-nginx-module-v0.5.3.tar.gz",
     "66792871b21d9e81f1ec6f8416bc18878ab28db0fbaad41877a0569e6a5c1b20"),
    ("https://codeload.github.com/Kong/lua-resty-events/tar.gz/refs/tags/0.2.0", "lua-resty-events-0.2.0.tar.gz",
     "accc80ad81c6258bf277f40b4d2feb10099e4d315036a8c285889d3991f9e224"),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--patch", type=Path, help="Replay the exported core patch instead of the adapter")
    args = parser.parse_args()
    root = args.directory.resolve()
    if (root / "work").exists():
        raise SystemExit("Choose a fresh directory: refusing to overwrite an existing prototype tree")
    downloads = root / "downloads"
    downloads.mkdir(parents=True, exist_ok=True)
    for url, name, checksum in PACKAGES:
        path = downloads / name
        if not path.exists():
            with urllib.request.urlopen(url, timeout=60) as response:
                path.write_bytes(response.read())
        if hashlib.sha256(path.read_bytes()).hexdigest() != checksum:
            raise RuntimeError("SHA256 mismatch: " + name)
        with tarfile.open(path) as archive:
            archive.extractall(root, filter="data")
        print("verified " + name, flush=True)
    source = root / "work/openresty-1.27.1.2"
    source.parent.mkdir()
    shutil.copytree(root / "openresty-1.27.1.2", source)
    with (root / "baseline-patches.log").open("w") as log:
        subprocess.run(["bash", "patch.sh", str(source)],
                       cwd=root / "ngx_multi_upstream_module-1.3.2", stdout=log, stderr=log, check=True)
        subprocess.run(["bash", "patch.sh", str(source)],
                       cwd=root / "apisix-nginx-module-1.19.2/patch", stdout=log, stderr=log, check=True)
    shutil.copytree(source, root / "work/baseline")
    if args.patch:
        patch = args.patch.resolve()
        subprocess.run(["patch", "--dry-run", "-p1", "-i", str(patch)], cwd=source, check=True)
        subprocess.run(["patch", "-p1", "-i", str(patch)], cwd=source, check=True)
    else:
        subprocess.run([sys.executable, str(HERE / "adapt.py"), str(source)], check=True)
    subprocess.run([sys.executable, str(HERE / "prepare-deps.py")], check=True)
    shutil.copytree(HERE / "vendor", root / "runtime-deps")
    shutil.copytree(HERE, root / "demo", ignore=shutil.ignore_patterns("vendor", "__pycache__", "build"))
    shutil.copytree(HERE.parent.parent / "apisix", root / "current-apisix")
    manifest = {"source_archives": [{"url": u, "sha256": h} for u, _, h in PACKAGES],
                "native_patch": str(args.patch) if args.patch else "adapt.py",
                "scope": "technical prototype; not production implementation"}
    (root / "source-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print("Prepared native prototype: " + str(root))


if __name__ == "__main__":
    main()
