#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Container-side native rebuild using the pinned, already unpacked toolchain."""
from pathlib import Path
import os
import shutil
import subprocess

SOURCE = Path("/src/experimental/native-http-core/module")
DEST = Path("/opt/apisix-core-module")
NGINX = Path("/tmp/openresty-1.27.1.2/bundle/nginx-1.27.1")
sanitizer = os.environ.get("APISIX_CORE_SANITIZER", "")
function_profile = os.environ.get("APISIX_CORE_FUNCTION_PROFILE", "") == "1"
assert sanitizer in ("", "undefined", "address,undefined"), sanitizer
assert not (sanitizer and function_profile), "Use separate diagnostic builds"
cc_flags = "-O2"
link_flags = []
if sanitizer:
    cc_flags = "-O1 -g -fno-omit-frame-pointer -fsanitize=" + sanitizer
    link_flags = ["--with-ld-opt=-fsanitize=" + sanitizer]
for path in SOURCE.iterdir():
    if path.is_file():
        shutil.copyfile(path, DEST / path.name)
if function_profile:
    cc_flags += " -finstrument-functions"
    shutil.copyfile(SOURCE.parent / "tools/function_sampler.c", DEST / "function_sampler.c")
    config = DEST / "config"
    config.write_text(config.read_text().replace('ngx_module_srcs="',
                                               'ngx_module_srcs="$ngx_addon_dir/function_sampler.c '))
with Path("/tmp/apisix-core-compile.log").open("w") as output:
    try:
        subprocess.run(["./configure", "--with-compat", "--with-http_ssl_module", "--with-stream",
                        "--with-threads", "--with-cc-opt=" + cc_flags + " -I../ngx_lua-0.10.28/src/api -I/usr/local/openresty/luajit/include/luajit-2.1",
                        "--add-dynamic-module=/opt/apisix-core-module"] + link_flags, cwd=NGINX, stdout=output,
                       stderr=subprocess.STDOUT, check=True)
        subprocess.run(["make", "-j2", "modules"], cwd=NGINX, stdout=output,
                       stderr=subprocess.STDOUT, check=True)
    except subprocess.CalledProcessError:
        print(Path(output.name).read_text()[-12000:])
        raise
binary = DEST / "lib/ngx_http_apisix_core_module.so"
temporary = binary.with_suffix(".so.new")
shutil.copyfile(NGINX / "objs/ngx_http_apisix_core_module.so", temporary)
os.replace(temporary, binary)  # existing workers may still map the old inode
print("Native C module rebuilt", "function-profile" if function_profile else sanitizer or "optimized")
