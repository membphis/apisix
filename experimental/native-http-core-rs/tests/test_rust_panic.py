#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""What a Rust panic does in each mode (test-only `APISIX_RS_FEATURES=panic-test`
build, where `GET /__apisix_rs_panic` panics inside the core).

- `--mode rust` (v2 pipeline): the panic happens in the NGINX access handler
  and is contained by `catch_unwind` (`guard::contain`). Expected: HTTP 500,
  the same worker PID before and after, the panic logged, and ordinary
  requests on the same worker still served.
- `--mode rust-lua` (step 1): the panic happens in `engine.start_phase`, a
  Lua-facing `C-unwind` function called by `runtime.lua`. `catch_unwind`
  cannot be used there (Lua errors unwind through it). The panic leaves
  Rust, LuaJIT catches it at the coroutine boundary, and Rust aborts the
  process because a foreign runtime discarded a Rust panic ("Rust panics must
  be rethrown"). This mode characterizes that outcome: the connection closes
  without a response, the worker exits on SIGABRT, and the master respawns a
  worker that serves requests again. It is the same blast radius as a crash
  in the C adapter.
"""
import os

os.environ.setdefault("APISIX_CORE_SOURCE", "/stable")

import argparse  # noqa: E402
import http.client  # noqa: E402
import json  # noqa: E402
from pathlib import Path  # noqa: E402
import sys  # noqa: E402
import time  # noqa: E402

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import rust_demo  # noqa: E402
from benchmark import workers  # noqa: E402  (unchanged research helper)

BUILD = Path("/opt/apisix-rs-module/lib/build.json")


def call(path, headers=None):
    connection = http.client.HTTPConnection("127.0.0.1", 9080, timeout=5)
    connection.request("GET", path, headers=headers or {})
    response = connection.getresponse()
    status, body = response.status, response.read()
    connection.close()
    return status, body


def respawned(seen):
    """Waits for a worker PID not in `seen` that serves requests."""
    deadline = time.time() + 15
    while time.time() < deadline:
        current = workers()
        if current and not set(current) & seen:
            try:
                if call("/auth/item", {"apikey": "demo-key"})[0] == 200:
                    return current
            except OSError:
                pass
        time.sleep(0.1)
    raise AssertionError("no respawned worker served the request")


def characterize_lua_adapter():
    """Step 1: a panic inside the Lua adapter aborts the worker."""
    # A served request guarantees the worker is up before its PID is read.
    assert call("/auth/item", {"apikey": "demo-key"})[0] == 200
    seen = set(workers())
    assert seen, "no gateway worker found"
    history = [sorted(seen)]
    for _ in range(3):
        try:
            status, _ = call("/__apisix_rs_panic", {"apikey": "demo-key"})
        except (http.client.RemoteDisconnected, ConnectionResetError):
            status = None  # the worker died while handling the request
        assert status is None, ("expected the connection to close", status)
        current = respawned(seen)
        seen |= set(current)
        history.append(sorted(current))
        assert call("/auth/item")[0] == 401
    log = (rust_demo.demo.ROOT / "logs/error.log").read_text()
    assert log.count("apisix-rs: panic at module/src/step1.rs") == 3, log[-2000:]
    assert log.count("Rust panics must be rethrown") == 3, log[-2000:]
    assert log.count("exited on signal 6") == 3, log[-2000:]
    print("CHARACTERIZED step-1 panic: 3 panics -> connection closed, worker aborted (SIGABRT)",
          "and respawned; worker PIDs", history, flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=["rust", "rust-lua"], default="rust")
    mode = parser.parse_args().mode
    build = json.loads(BUILD.read_text())
    assert build["features"] == ["panic-test"], "build the panic-test module first"
    rust_demo.install()
    rust_demo.prepare(mode, single_plugin="key-auth")
    try:
        if mode == "rust-lua":
            return characterize_lua_adapter()
        before = workers()
        assert call("/auth/item", {"apikey": "demo-key"})[0] == 200
        for _ in range(20):
            status, _ = call("/__apisix_rs_panic", {"apikey": "demo-key"})
            assert status == 500, status
            assert call("/auth/item", {"apikey": "demo-key"})[0] == 200
            assert call("/auth/item")[0] == 401
        after = workers()
        assert before == after, ("worker restarted", before, after)
        log = (rust_demo.demo.ROOT / "logs/error.log").read_text()
        assert log.count("contained panic in access handler: intentional panic from the panic-test build") == 20, log[-2000:]
        assert "apisix-rs: panic at module/src/pipeline.rs" in log
        assert "exited on signal" not in log, log[-2000:]
        print("PASS panic containment rust : 20 panics -> 500, worker PID unchanged", before, flush=True)
    finally:
        rust_demo.demo.stop()


if __name__ == "__main__":
    main()
