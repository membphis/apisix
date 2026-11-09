#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Same-batch functional parity: stable Lua, C step 1 (native-Lua), Rust step 1,
C v2 and Rust v2.

Reuses the unchanged v2 semantic suite (`tests/test_native_key_auth.py`):
47 HTTP cases, 128 concurrent authentications, keepalive reuse, rejected
GET/HEAD/POST never reaching the upstream, the retry-off policy, and the
generation lifecycle (pinning, reload, key rotation, rejected unsupported
reload and recovery). The comparison rule is the suite's own: every variant
must equal stable Lua, except the documented dot-segment URI case, which is
compared with the native-Lua control.

Step 1 is "only the core changes, key-auth stays Lua": C native-lua (the V1
Lua ABI design, also the suite's control) and the Rust step-1 core run the
unchanged Lua key-auth through the same `runtime.lua` transport.

In addition to the suite (which compares status, WWW-Authenticate and the
upstream echo), the response headers the native header filter owns
(`Server`, `X-APISIX-Upstream-Status`) are compared with stable Lua.
"""
import os

# The release checkout (pinned 3.19.0), as in tools/key_auth_experiment.py.
os.environ.setdefault("APISIX_CORE_SOURCE", "/stable")
os.environ.setdefault("APISIX_CORE_SOURCE_REVISION", "473043623c161966f3f6ef4e8f12d2d8667a12d2")

import argparse  # noqa: E402
import http.client  # noqa: E402
import json  # noqa: E402
from pathlib import Path  # noqa: E402
import re  # noqa: E402
import sys  # noqa: E402

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import rust_demo  # noqa: E402
import test_native_key_auth as suite  # noqa: E402

rust_demo.install()
suite.VARIANTS["native-rust-lua-key-auth"] = ("rust-lua", "")
suite.VARIANTS["native-rust-pipeline"] = ("rust", rust_demo.C_PIPELINE)
ORDER = ["lua-core", "native-lua", "native-rust-lua-key-auth", "native-c-pipeline", "native-rust-pipeline"]
LIFECYCLE = ("native-rust-lua-key-auth", "native-c-pipeline", "native-rust-pipeline")
HEADER_CASES = [
    ("/auth/item", [("apikey", "demo-key")]),
    ("/auth/item", []),
    ("/outside-route", [("apikey", "demo-key")]),
    ("/no-retry/item", [("apikey", "demo-key")]),
    ("/timeout/item?sleep=0.15", [("apikey", "demo-key")]),
    ("/apisix/nginx_status", []),
]


def checked_stop(prefix=suite.ROOT):
    """While a Rust gateway is up, prove it is the Rust module: the v2
    pipeline without Lua hooks, step 1 with exactly the C variant's entries."""
    conf = suite.ROOT / "conf/nginx.conf"
    if prefix == suite.ROOT and (suite.ROOT / "logs/nginx.pid").exists() and rust_demo.RUST_MODULE in conf.read_text():
        text = conf.read_text()
        assert "ngx_http_apisix_core_module" not in text and "apisix_core_" not in text
        hooks = re.findall(r"\b(access|header_filter|body_filter|log|balancer)_by_lua", text)
        if "apisix_rs_lua_plugins on;" in text:
            # The C native-lua template's hooks: no Lua header/body filters.
            assert set(hooks) == {"access", "balancer", "log"}, hooks
        else:
            assert not hooks, hooks
    rust_demo.demo.stop(prefix)


def response_headers(variant):
    """Server / X-APISIX-Upstream-Status for success, rejection, no route,
    upstream failure and timeout, plus a location outside the core."""
    mode, directives = suite.VARIANTS[variant]
    os.environ["APISIX_CORE_DIRECTIVES"] = directives
    fixture = Path("/tmp/native-key-auth-fixture.json")
    fixture.write_text(json.dumps(suite.fixture()))
    suite.prepare(mode, fixture=fixture, plugin_allowlist=["key-auth"], proxy_retry=True)
    suite.ready("/missing", expected=404)
    answers = []
    for path, headers in HEADER_CASES:
        connection = http.client.HTTPConnection("127.0.0.1", 9080, timeout=5)
        connection.request("GET", path, headers=dict(headers))
        response = connection.getresponse()
        response.read()
        answers.append([path, response.status, response.getheader("Server"),
                        response.getheader("X-APISIX-Upstream-Status")])
        connection.close()
    suite.stop()
    return answers


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--variants", nargs="+", choices=ORDER, default=ORDER)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    variants = [v for v in ORDER if v in args.variants]
    suite.stop = checked_stop
    result = {}
    try:
        for variant in variants:
            answer = suite.run(variant)
            if result:
                baseline = result.get("lua-core") or next(iter(result.values()))
                for index, value in enumerate(answer):
                    case = suite.cases()[index]
                    expected = baseline[index]
                    if case[0] == "/auth/a/../item" and "native-lua" in result:
                        expected = result["native-lua"][index]
                    if case[0] == "/auth/a/../item" and variant == "native-lua":
                        continue
                    assert value == expected, (variant, index, case, value, expected)
            result[variant] = answer
            print("PASS", variant, len(answer),
                  "semantic cases; 128 concurrent auth; keepalive; rejected requests do not proxy", flush=True)
        headers = {}
        for variant in variants:
            headers[variant] = response_headers(variant)
            baseline = headers.get("lua-core") or headers[variants[0]]
            assert headers[variant] == baseline, (variant, headers[variant], baseline)
            print("PASS", variant, "response headers", headers[variant], flush=True)
        for variant in variants:
            if variant in LIFECYCLE:
                suite.lifecycle(variant)
        if args.out:
            output = {"cases": result, "response_headers": headers}
            args.out.write_text(json.dumps(output, default=lambda x: x.decode(), indent=2) + "\n")
    finally:
        rust_demo.demo.stop()


if __name__ == "__main__":
    main()
