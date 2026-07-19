#!/usr/bin/env python3
"""Normalized-window clone detector for the optimizer tree.

Used to size the duplication census in docs/plans/opt_pass_dedup_and_perf.md and
to check that a dedup phase actually removed what it claimed.

Normalization (per line, in order): drop preprocessor lines, comments, and lines
that are only braces/blank; canonicalize string literals to "S", character
literals to 'C', and integer/float literals to N; collapse whitespace.
Identifiers are *kept*, so a match means the same code, not merely the same
shape -- that keeps the false-positive rate low enough to act on.

A group is reported when the same window of >= --window normalized lines occurs
in two or more distinct files.  Windows are maximal: a window contained in an
already-reported longer window at the same sites is suppressed.

Usage:
  scripts/find_clones.py source/opt --window 10
  scripts/find_clones.py source/opt --window 6 --same-file --top 40
"""

import argparse
import hashlib
import os
import re
import sys
from collections import defaultdict

STR = re.compile(r'"(\\.|[^"\\])*"')
CHR = re.compile(r"'(\\.|[^'\\])*'")
NUM = re.compile(r"\b0[xX][0-9a-fA-F]+\b|\b\d+(\.\d+)?([eE][-+]?\d+)?[uUlLfF]*\b")
WS = re.compile(r"\s+")
BRACE_ONLY = re.compile(r"^[{}();\s]*$")


def normalize(path):
    """Return [(normalized_line, original_lineno)] for one file."""
    out = []
    in_block_comment = False
    with open(path, errors="replace") as fh:
        for lineno, raw in enumerate(fh, 1):
            line = raw
            if in_block_comment:
                end = line.find("*/")
                if end < 0:
                    continue
                line = line[end + 2:]
                in_block_comment = False
            while True:
                start = line.find("/*")
                if start < 0:
                    break
                end = line.find("*/", start + 2)
                if end < 0:
                    line = line[:start]
                    in_block_comment = True
                    break
                line = line[:start] + " " + line[end + 2:]
            line = re.sub(r"//.*$", "", line)
            if line.lstrip().startswith("#"):
                continue
            if BRACE_ONLY.match(line):
                continue
            line = STR.sub('"S"', line)
            line = CHR.sub("'C'", line)
            line = NUM.sub("N", line)
            line = WS.sub(" ", line).strip()
            if line:
                out.append((line, lineno))
    return out


def collect(roots, exts):
    files = []
    for root in roots:
        if os.path.isfile(root):
            files.append(root)
            continue
        for dirpath, _dirs, names in os.walk(root):
            for n in sorted(names):
                if os.path.splitext(n)[1] in exts:
                    files.append(os.path.join(dirpath, n))
    return sorted(files)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("roots", nargs="+", help="files or directories to scan")
    ap.add_argument("--window", type=int, default=10, help="minimum normalized lines")
    ap.add_argument("--ext", default=".c,.h", help="comma-separated extensions")
    ap.add_argument("--same-file", action="store_true",
                    help="also report clones within a single file")
    ap.add_argument("--top", type=int, default=25, help="print the N largest groups")
    args = ap.parse_args()

    exts = set(args.ext.split(","))
    files = collect(args.roots, exts)
    norm = {f: normalize(f) for f in files}

    # window hash -> [(file, first_lineno)]
    sites = defaultdict(list)
    for path, lines in norm.items():
        for i in range(len(lines) - args.window + 1):
            body = "\n".join(l for l, _ in lines[i:i + args.window])
            h = hashlib.blake2b(body.encode(), digest_size=16).digest()
            sites[h].append((path, lines[i][1], i))

    groups = []
    for h, occ in sites.items():
        distinct = {p for p, _, _ in occ}
        if len(occ) < 2:
            continue
        if len(distinct) < 2 and not args.same_file:
            continue
        groups.append((h, occ))

    # Suppress windows fully contained in a longer clone at the same file set:
    # keep only the earliest window index per (file-set, run).
    kept = []
    seen_runs = set()
    for h, occ in sorted(groups, key=lambda g: -len(g[1])):
        key = tuple(sorted((p, i) for p, _, i in occ))
        prev = tuple(sorted((p, i - 1) for p, _, i in occ))
        if prev in seen_runs:
            seen_runs.add(key)
            continue
        seen_runs.add(key)
        kept.append((h, occ))

    total_removable = 0
    for _h, occ in kept:
        total_removable += args.window * (len(occ) - 1)

    print("files scanned: %d   window: %d   clone groups: %d   "
          "removable lines (lower bound): ~%d"
          % (len(files), args.window, len(kept), total_removable))
    print()
    for _h, occ in sorted(kept, key=lambda g: -len(g[1]))[: args.top]:
        print("group x%d:" % len(occ))
        for path, lineno, _i in sorted(occ):
            print("  %s:%d" % (path, lineno))
    return 0


if __name__ == "__main__":
    sys.exit(main())
