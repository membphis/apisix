#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Inside the pinned image: run the existing harness with the Rust module.

The C research harness (`experimental/native-http-core/tools/demo.py`) is
reused unchanged. A Rust gateway is prepared from exactly the configuration
the C pipeline variant generates (same APISIX template, `native.json`,
upstream block, keepalive and CPU affinity), then converted:

- `load_module` points at the Rust module;
- every `apisix_core_*` directive is replaced by its `apisix_rs_*` counterpart
  (`apisix_rs_config`, `apisix_rs_proxy`, `apisix_rs_balancer`);
- the init/init_worker/exit_worker Lua blocks no longer load the C module's
  `apisix.native.engine` (configuration loading is done by Rust).

No request-phase Lua hook exists in either pipeline variant.

The "rust-lua" mode is step 1 of the migration (Rust core + unchanged Lua
key-auth). It is prepared from exactly the C native-lua configuration (the
V1 Lua ABI design with the six selected switches) and converted the same
way: the Lua entries (`engine.init`/`init_worker`/`shutdown`, `runtime.run`
for access and log, `runtime.balance`) and `runtime.lua` stay unchanged;
only the module providing `apisix.native.engine` and the SDK changes.
"""
import os
from pathlib import Path
import re
import subprocess
import sys

HERE = Path(__file__).resolve().parents[1]
CORE = HERE.parent / "native-http-core"
sys.path.insert(0, str(CORE / "tools"))
sys.path.insert(0, str(CORE / "tests"))

import demo  # noqa: E402  (the unchanged research harness)

RUST_MODULE = "/opt/apisix-rs-module/lib/ngx_http_apisix_rs_module.so"
# The six selected C switches plus the C pipeline; the Rust conversion
# removes all of them, so the generated template is identical otherwise.
C_BASE = ("apisix_core_lazy_rate_json on; apisix_core_batch_vars on; "
          "apisix_core_lazy_route_opts on; apisix_core_request_header_flags on; "
          "apisix_core_header_scratch on; apisix_core_request_pool_native on;")
C_PIPELINE = C_BASE + " apisix_core_native_pipeline on;"
_original_prepare = demo.prepare


def convert(text):
    """C pipeline nginx.conf -> Rust pipeline nginx.conf (fails loudly)."""
    def replace_once(old, new, value):
        if value.count(old) != 1:
            raise RuntimeError(f"expected exactly one {old!r} in the generated configuration")
        return value.replace(old, new)

    text = replace_once(f"load_module {demo.MODULE};", f"load_module {RUST_MODULE};", text)
    text = replace_once("apisix_core_native_filters on;",
                        f"apisix_rs_config {demo.ROOT / 'conf/native.json'};", text)
    text, count = re.subn(r"^apisix_core_[^\n]*\n", "", text, flags=re.MULTILINE)
    if count != 1:
        raise RuntimeError("expected one line of apisix_core experiment directives")
    text = replace_once("apisix_core_proxy on;", "apisix_rs_proxy on;", text)
    text = replace_once("apisix_core_native_balancer;", "apisix_rs_balancer;", text)
    text = demo.replace_init(text, "init_by_lua_block", '        require "resty.core"')
    text = demo.replace_init(text, "init_worker_by_lua_block", "        -- configuration is compiled by the Rust module")
    text = demo.replace_init(text, "exit_worker_by_lua_block", "        -- nothing to release")
    if "apisix_core" in text or "apisix.native" in text:
        raise RuntimeError("C module references remain in the Rust configuration")
    if re.search(r"\b(?:access|rewrite|header_filter|body_filter|log|balancer)_by_lua", text):
        raise RuntimeError("request-phase Lua hook present in the Rust pipeline configuration")
    return text


def convert_lua(text):
    """C native-lua nginx.conf -> Rust step-1 nginx.conf (fails loudly)."""
    def replace_once(old, new, value):
        if value.count(old) != 1:
            raise RuntimeError(f"expected exactly one {old!r} in the generated configuration")
        return value.replace(old, new)

    text = replace_once(f"load_module {demo.MODULE};", f"load_module {RUST_MODULE};", text)
    text = replace_once("apisix_core_native_filters on;", "apisix_rs_lua_plugins on;", text)
    text, count = re.subn(r"^apisix_core_[^\n]*\n", "", text, flags=re.MULTILINE)
    if count != 1:
        raise RuntimeError("expected one line of apisix_core experiment directives")
    # The C directive sits at server level before the business location; the
    # Rust flag is location-level. Only that location runs the Lua engine.
    text = replace_once("apisix_core_proxy on;\nlocation / {", "location / {\n            apisix_rs_proxy on;", text)
    if "apisix_core" in text:
        raise RuntimeError("C module directives remain in the Rust step-1 configuration")
    for entry in ('require("apisix.native.engine").init(', 'require("apisix.native.engine").init_worker()',
                  'require("apisix.native.runtime").run("access")', 'require("apisix.native.runtime").balance()'):
        if text.count(entry) != 1:
            raise RuntimeError(f"expected the unchanged Lua entry {entry!r}")
    if 'require("apisix.native.runtime").run("log")' not in text:
        raise RuntimeError("expected the unchanged Lua log entry")
    return text


def prepare(mode, **kwargs):
    """demo.prepare with additional "rust" (v2 pipeline) and "rust-lua" (step 1) modes."""
    if mode == "rust-lua":
        saved = os.environ.get("APISIX_CORE_DIRECTIVES")
        os.environ["APISIX_CORE_DIRECTIVES"] = C_BASE
        try:
            _original_prepare("native", **kwargs)
        finally:
            if saved is None:
                os.environ.pop("APISIX_CORE_DIRECTIVES", None)
            else:
                os.environ["APISIX_CORE_DIRECTIVES"] = saved
        demo.stop()
        path = demo.ROOT / "conf/nginx.conf"
        path.write_text(convert_lua(path.read_text()))
        (demo.ROOT / "logs/error.log").write_text("")
        subprocess.run([demo.NGINX, "-p", str(demo.ROOT), "-c", "conf/nginx.conf", "-t"], env=demo.NGINX_ENV, check=True)
        subprocess.run([demo.NGINX, "-p", str(demo.ROOT), "-c", "conf/nginx.conf"], env=demo.NGINX_ENV, check=True)
        print("Restarted as rust-lua: :9080", flush=True)
        return None
    if mode != "rust":
        return _original_prepare(mode, **kwargs)
    saved = os.environ.get("APISIX_CORE_DIRECTIVES")
    os.environ["APISIX_CORE_DIRECTIVES"] = C_PIPELINE
    try:
        _original_prepare("native", **kwargs)
    finally:
        if saved is None:
            os.environ.pop("APISIX_CORE_DIRECTIVES", None)
        else:
            os.environ["APISIX_CORE_DIRECTIVES"] = saved
    demo.stop()
    path = demo.ROOT / "conf/nginx.conf"
    path.write_text(convert(path.read_text()))
    (demo.ROOT / "logs/error.log").write_text("")
    subprocess.run([demo.NGINX, "-p", str(demo.ROOT), "-c", "conf/nginx.conf", "-t"], env=demo.NGINX_ENV, check=True)
    subprocess.run([demo.NGINX, "-p", str(demo.ROOT), "-c", "conf/nginx.conf"], env=demo.NGINX_ENV, check=True)
    print("Restarted as rust: :9080", flush=True)


def install():
    """Route every harness module's `prepare` through the dispatcher."""
    demo.prepare = prepare
    for name in ("test_native_key_auth", "benchmark", "test_demo"):
        module = sys.modules.get(name)
        if module is not None and hasattr(module, "prepare"):
            module.prepare = prepare
