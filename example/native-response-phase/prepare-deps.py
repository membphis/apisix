#!/usr/bin/env python3
"""Fetch only the pure-Lua runtime updates needed by this checkout.

Uses inherited HTTPS proxy and system CA trust. No toolchains or LuaRocks needed.
Downloaded files are disposable build inputs, not part of the patch.
"""
import hashlib
import io
from pathlib import Path
import tarfile
import urllib.request


DEST = Path(__file__).resolve().parent / "vendor"
PACKAGES = [
    ("api7", "lua-resty-websocket", "0.2.0",
     "5b9de232e4966d2450b80b664e64e50d6ea59472ae1dd37b243117c9aa841faf"),
    ("api7", "lua-resty-http", "0.2.4",
     "42028cf16297b6bdb7c7f3f11a09f07908f1f38a1cc230cb904887ffb16c2847"),
    ("iresty", "lua-resty-redis-cluster", "1.3.3",
     "e4da1c34c04af82ffa649f6b1cf7d895fa01b60d40a1e9416982a2660a63dc56"),
]


def main():
    for owner, name, version, checksum in PACKAGES:
        url = f"https://codeload.github.com/{owner}/{name}/tar.gz/refs/tags/v{version}"
        with urllib.request.urlopen(url, timeout=60) as response:
            data = response.read()
        if hashlib.sha256(data).hexdigest() != checksum:
            raise RuntimeError(f"SHA256 mismatch: {name} {version}")
        with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as archive:
            prefix = f"{name}-{version}/lib/"
            for member in archive.getmembers():
                if member.isfile() and member.name in (
                    f"{name}-{version}/LICENSE", f"{name}-{version}/LICENSE-APACHE",
                    f"{name}-{version}/README.markdown",
                ):
                    # WebSocket's BSD notices are in README.markdown and its
                    # proxy module also ships LICENSE-APACHE. Retain both.
                    target = DEST / "licenses" / (name + "-" + Path(member.name).name)
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(archive.extractfile(member).read())
                if member.isfile() and member.name.startswith(prefix + "resty/"):
                    relative = Path(member.name[len(prefix):])
                    if ".." in relative.parts:
                        raise RuntimeError("unsafe archive path")
                    target = DEST / relative
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(archive.extractfile(member).read())
        print(f"verified and prepared {name} v{version}")


if __name__ == "__main__":
    main()
