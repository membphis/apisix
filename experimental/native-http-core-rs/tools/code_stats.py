#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Line counts and unsafe share of the Rust port (host or container, no deps).

Counts physical lines, code lines (non-blank, non-comment), test lines (inside
`#[cfg(test)] mod`), and code lines inside `unsafe` blocks / `unsafe fn` bodies.
The brace tracking ignores braces inside string literals and comments, which is
enough for this code base; it is an approximation, not a Rust parser.
"""
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]


def strip(line):
    """Remove string/char literals and line comments for brace counting."""
    line = re.sub(r'b?"(?:\\.|[^"\\])*"', '""', line)
    line = re.sub(r"b?'(?:\\.|[^'\\])'", "''", line)
    return line.split("//", 1)[0]


def analyze(path):
    code = tests = unsafe_lines = total = 0
    depth = 0
    unsafe_stack = []  # depths at which an unsafe region was opened
    test_depth = None
    pending_unsafe = pending_test = False
    for raw in path.read_text().splitlines():
        total += 1
        line = raw.strip()
        if not line or line.startswith("//"):
            continue
        code += 1
        bare = strip(raw)
        if "#[cfg(test)]" in bare:
            pending_test = True
        in_test = test_depth is not None
        if in_test:
            tests += 1
        if unsafe_stack:
            unsafe_lines += 1
        if re.search(r"\bunsafe\b", bare):
            pending_unsafe = True
            if not unsafe_stack:
                unsafe_lines += 1  # the line opening the region
        for char in bare:
            if char == "{":
                if pending_test and test_depth is None:
                    test_depth = depth
                    pending_test = False
                if pending_unsafe:
                    unsafe_stack.append(depth)
                    pending_unsafe = False
                depth += 1
            elif char == "}":
                depth -= 1
                if unsafe_stack and unsafe_stack[-1] == depth:
                    unsafe_stack.pop()
                if test_depth is not None and depth == test_depth:
                    test_depth = None
        if ";" in bare and pending_unsafe and "{" not in bare:
            pending_unsafe = False  # e.g. `unsafe impl` declarations without bodies
    return {"lines": total, "code": code, "tests": tests, "non_test_code": code - tests,
            "unsafe_code": unsafe_lines}


# Step 1 (Rust core + unchanged Lua plugins): the Lua ABI adapter, i.e. the
# engine protocol, the SDK subset and the Lua C API declarations.
STEP1 = {"module/src/lua.rs", "module/src/sdk.rs", "module/src/step1.rs"}


def summarize(per_file):
    totals = {key: sum(v[key] for v in per_file.values()) for key in next(iter(per_file.values()))}
    totals["unsafe_share_of_non_test_code"] = round(totals["unsafe_code"] / max(1, totals["non_test_code"]), 3)
    return totals


def main():
    report = {}
    for crate in ("core", "module"):
        files = sorted((ROOT / crate / "src").glob("*.rs"))
        per_file = {str(p.relative_to(ROOT)): analyze(p) for p in files}
        report[crate] = {"files": per_file, "total": summarize(per_file)}
    module = report["module"]["files"]
    report["groups"] = {
        "step1_lua_adapter": summarize({k: v for k, v in module.items() if k in STEP1}),
        "nginx_glue_shared_and_v2": summarize({k: v for k, v in module.items() if k not in STEP1}),
    }
    json.dump(report, sys.stdout, indent=2)
    print()


if __name__ == "__main__":
    main()
