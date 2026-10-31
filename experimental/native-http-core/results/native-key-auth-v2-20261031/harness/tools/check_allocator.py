#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build and check the independent diagnostic allocator counter lifecycle."""
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
INCLUDE = '/usr/local/openresty/luajit/include/luajit-2.1'
LIB = '/usr/local/openresty/luajit/lib'
SAMPLER = Path('/tmp/apisix-allocator-sampler.so')
TEST = Path('/tmp/apisix-test-allocator')

commands = [
    ['gcc', '-shared', '-fPIC', '-O2', '-Wall', '-Wextra', '-Werror', '-I'+INCLUDE,
     str(ROOT/'tools/allocator_sampler.c'), '-o', str(SAMPLER)],
    ['gcc', '-O2', '-Wall', '-Wextra', '-Werror', '-I'+INCLUDE,
     str(ROOT/'tests/test_allocator.c'), '-L'+LIB, '-lluajit-5.1',
     '-Wl,-rpath,'+LIB, '-o', str(TEST)],
]
for command in commands:
    print(json.dumps(command), flush=True)
    subprocess.run(command, check=True)
subprocess.run([str(TEST), str(SAMPLER)], check=True)
print(json.dumps({'sha256': {str(path): hashlib.sha256(path.read_bytes()).hexdigest()
                           for path in [SAMPLER, TEST, ROOT/'tools/allocator_sampler.c',
                                        ROOT/'tests/test_allocator.c']}}))
