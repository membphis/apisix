#!/usr/bin/env python3
"""Export the native source delta, relative to the pinned API7-patched bundle."""
import argparse
import difflib
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    baseline = args.directory / "work/baseline"
    source = args.directory / "work/openresty-1.27.1.2"
    pieces = []
    changed = []
    for path in sorted(source.rglob("*")):
        relative = path.relative_to(source)
        if not path.is_file() or "objs" in relative.parts or path.name.endswith("_autoconf.h"):
            continue
        if path.suffix not in {".c", ".h", ".lua"} and str(relative) not in {
            "bundle/nginx-1.27.1/auto/modules", "bundle/ngx_lua-0.10.28/config"}:
            continue
        original = baseline / relative
        before = original.read_text() if original.exists() else ""
        after = path.read_text()
        if before == after:
            continue
        changed.append(str(relative))
        pieces.append(f"diff --git a/{relative} b/{relative}\n")
        if not original.exists():
            pieces.append("new file mode 100644\n")
        pieces.extend(difflib.unified_diff(before.splitlines(True), after.splitlines(True),
            fromfile=f"a/{relative}" if original.exists() else "/dev/null", tofile=f"b/{relative}"))
    args.output.write_text("".join(pieces))
    print(f"Exported {len(changed)} changed native source files: {args.output}")
    for name in changed:
        print(name)


if __name__ == "__main__":
    main()
