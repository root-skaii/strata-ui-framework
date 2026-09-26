#!/usr/bin/env python3
"""Counts non-blank, non-comment lines of C/C++/HLSL source under one or more paths.

Usage:
    python tools/loc.py [paths...] [--ext .cpp,.hpp,...]

With no paths, counts the whole repo (strata, sandbox, tests, overlay). Comments are stripped
with a small character-by-character scan that understands string and char literals, so a "//"
or "/*" inside a string is not mistaken for a comment; it does not otherwise parse the language.
"""

import argparse
import pathlib
import sys

DEFAULT_EXTS = {".cpp", ".hpp", ".h", ".c", ".cc", ".hlsl"}
DEFAULT_PATHS = ["strata", "sandbox", "tests", "overlay"]


def strip_comments(text: str) -> str:
    out = []
    i, n = 0, len(text)
    in_line = in_block = in_str = in_chr = False
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if in_line:
            if c == "\n":
                in_line = False
                out.append(c)
            i += 1
            continue
        if in_block:
            if c == "*" and nxt == "/":
                in_block = False
                i += 2
                continue
            if c == "\n":
                out.append(c)
            i += 1
            continue
        if in_str:
            out.append(c)
            if c == "\\" and i + 1 < n:
                out.append(nxt)
                i += 2
                continue
            if c == '"':
                in_str = False
            i += 1
            continue
        if in_chr:
            out.append(c)
            if c == "\\" and i + 1 < n:
                out.append(nxt)
                i += 2
                continue
            if c == "'":
                in_chr = False
            i += 1
            continue
        if c == "/" and nxt == "/":
            in_line = True
            i += 2
            continue
        if c == "/" and nxt == "*":
            in_block = True
            i += 2
            continue
        if c == '"':
            in_str = True
            out.append(c)
            i += 1
            continue
        if c == "'":
            in_chr = True
            out.append(c)
            i += 1
            continue
        out.append(c)
        i += 1
    return "".join(out)


def count_file(path: pathlib.Path) -> int:
    text = path.read_text(encoding="utf-8", errors="replace")
    stripped = strip_comments(text)
    return sum(1 for line in stripped.split("\n") if line.strip())


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="*", help="files or directories to scan (default: the whole repo)")
    ap.add_argument("--ext", default=",".join(sorted(DEFAULT_EXTS)), help="comma-separated extensions to include")
    ap.add_argument("--quiet", "-q", action="store_true", help="print only the grand total")
    args = ap.parse_args()

    exts = {e if e.startswith(".") else f".{e}" for e in args.ext.split(",") if e}
    repo_root = pathlib.Path(__file__).resolve().parent.parent
    paths = [pathlib.Path(p) for p in args.paths] if args.paths else [repo_root / p for p in DEFAULT_PATHS]

    files = []
    for p in paths:
        if p.is_file():
            files.append(p)
        elif p.is_dir():
            files.extend(sorted(f for f in p.rglob("*") if f.is_file() and f.suffix in exts))
        else:
            print(f"loc: no such file or directory: {p}", file=sys.stderr)
            return 1

    counts = [(count_file(f), f) for f in files]
    counts.sort(reverse=True)
    total = sum(c for c, _ in counts)

    if not args.quiet:
        for c, f in counts:
            print(f"{c:6d}  {f}")
        print()
    print(f"TOTAL: {total} non-blank, non-comment lines across {len(counts)} files")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
