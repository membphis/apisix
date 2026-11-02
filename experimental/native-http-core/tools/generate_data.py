#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Extract immutable schema data; no original core is used by the gateway."""
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[3]
TARGET = ROOT / "experimental/native-http-core/module"
for source, mode, destination, variable in (
    ("apisix/schema_def.lua", "schema", "schema_data.h", "ac_schema_graph"),
    ("apisix/utils/redis-schema.lua", "redis", "redis_data.h", "ac_redis_graph"),
):
    output = subprocess.check_output([
        "docker", "--host=unix:///var/run/docker.sock", "exec", "apisix-http-core-dev",
        "/usr/local/openresty/luajit/bin/luajit", "/src/experimental/native-http-core/tools/schema_graph.lua",
        "/src/" + source, mode,
    ], text=True).strip()
    json.loads(output)
    text = ("/* SPDX-License-Identifier: Apache-2.0 */\n"
            f"/* Generated DATA from {source} at b31e828c6f6b588ae954bf69c8432c08fb73bb7e.\n"
            " * Shared table references are preserved. Source functions are excluded. */\n"
            f"static const char {variable}[] =\n")
    text += "".join("    " + json.dumps(output[i:i+160]) + "\n" for i in range(0, len(output), 160)) + ";\n"
    (TARGET / destination).write_text(text)
