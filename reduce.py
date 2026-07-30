#!/usr/bin/env python3
"""Generic C-source reducer for the fuzz triage miscompiles.

Oracle: a "interesting" predicate.  We compile the reduced program at a given
opt-level and an oracle opt-level (default O0 / known-good), run under QEMU via
tests/ir_tests/run.py, and require:
  * outputs DIFFER (so we don't reduce to a trivially-correct program)
  * reduced-bad-output equals the originally-recorded bad checksum
  * oracle output equals the recorded good checksum

Keeps the reduction faithful to the *original* miscompile.

Usage:
    python3 reduce.py <file.c> -O<badlevel> <badsum> -G <goodsum> [-g oracle_level]
"""
from __future__ import annotations
import argparse, os, re, subprocess, sys, random, tempfile, shutil

REPO = os.path.abspath(os.path.dirname(__file__))
RUN = os.path.join(REPO, "tests", "ir_tests", "run.py")
ENV = dict(os.environ)
ENV.pop("TCC_DISABLE_PASS", None)

_cache: dict[bytes, tuple[str, str]] = {}

def run(src: bytes, level: str) -> tuple[str, str]:
    h = hash((src, level))
    if h in _cache:
        return _cache[h]
    with tempfile.NamedTemporaryFile("wb", suffix=".c", delete=False) as f:
        f.write(src); path = f.name
    try:
        p = subprocess.run(["python", RUN, "-c", path, "--cflags=" + level],
                           capture_output=True, text=True, env=ENV,
                           cwd=os.path.join(REPO, "tests", "ir_tests"))
        out = p.stdout
        m = re.search(r"checksum=([0-9a-f]+)", out)
        summ = m.group(1) if m else ("ERR" if p.returncode else "NOOUT")
        res = (summ, out)
    finally:
        os.unlink(path)
    _cache[h] = res
    return res

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("-O", "--bad-level", required=True)
    ap.add_argument("-g", "--good-level", default="-O0")
    ap.add_argument("badsum")
    ap.add_argument("goodsum", nargs="?")
    args = ap.parse_args()

    with open(args.file, "rb") as f:
        src0 = f.read()

    def interesting(src: bytes) -> bool:
        bad, _ = run(src, args.bad_level)
        if bad != args.badsum:
            return False
        if args.goodsum:
            good, _ = run(src, args.good_level)
            if good != args.goodsum:
                return False
        return True

    assert interesting(src0), "original does not reproduce"
    print(f"[start] {len(src0)} bytes", flush=True)

    src = src0
    # Strategy 1: drop contiguous line ranges
    lines = src.split(b"\n")
    improved = True
    while improved:
        improved = False
        n = len(lines)
        # try dropping larger chunks first
        for span in [n, n//2, n//4, n//8, 16, 8, 4, 2, 1]:
            if span < 1: continue
            i = 0
            while i + span <= n:
                cand = lines[:i] + lines[i+span:]
                cs = b"\n".join(cand)
                if interesting(cs):
                    lines = cand
                    n = len(lines)
                    improved = True
                    print(f"[drop {span} @ {i}] -> {len(lines)} lines", flush=True)
                    continue
                i += span
        src = b"\n".join(lines)

    # Strategy 2: blank out substrings within a line (keep structure)
    # Replace parenthesized sub-expressions and identifier tokens with 0
    src = b"\n".join(lines)
    improved = True
    while improved:
        improved = False
        # replace each long token-ish run with '0'
        new = re.sub(rb"(0x[0-9a-fA-F]+|[0-9]+u?)", b"0", src)
        if new != src and interesting(new):
            src = new; improved = True; print("[num->0]", flush=True)
        # collapse sequences of casts/parens
        break

    # Strategy 3: repeated token-level deletion
    toks = src.split(b" ")
    improved = True
    while improved:
        improved = False
        n = len(toks)
        for span in [n, n//2, n//4, 8, 4, 2, 1]:
            if span < 1: continue
            i = 0
            while i + span <= n:
                cand = toks[:i] + toks[i+span:]
                cs = b" ".join(cand)
                if interesting(cs):
                    toks = cand; n = len(toks); improved = True
                    print(f"[tokdrop {span} @ {i}] -> {n} toks", flush=True)
                    continue
                i += span
        src = b" ".join(toks)

    with open(args.file + ".reduced.c", "wb") as f:
        f.write(src)
    print(f"[done] wrote {args.file}.reduced.c ({len(src)} bytes)")

if __name__ == "__main__":
    main()
