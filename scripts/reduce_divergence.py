#!/usr/bin/env python3
"""Delta-reduce a divergent C program to a smaller repro (Phase BH helper).

Given a ``.c`` file that produces different output under armv8m-tcc at two
different optimization levels (the "interestingness" property), greedily delete
top-level functions and individual statement lines while the divergence persists,
yielding a smaller program with the same bug.  Reuses the QEMU harness
(``tests/fuzz/fuzz_harness.py``) so the reduced program is still validated
end-to-end on the real target.

This is intentionally simple (line/function granularity, not a full C reducer
like creduce) -- enough to hand a much smaller repro to bug-fix work.

Usage:
    python scripts/reduce_divergence.py FILE.c --low -O0 --high -O1 -o reduced.c
    python scripts/reduce_divergence.py FILE.c --low -O0 --high -O2

The reduced program is only guaranteed to *reproduce the divergence*; it is not
re-checked for UB (the original was UB-free; deletions cannot introduce signed
overflow etc. given the generator's all-unsigned discipline, but treat the
reduced output as a starting point for manual minimization).
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from sources.fuzz_common import FUZZ_DIR, H


def diverges(source_text: str, low: str, high: str, work_dir: Path) -> bool:
    """True iff tcc at ``low`` and ``high`` produce different (stdout, exit) AND
    both builds/runs succeed (so we don't 'reduce' into a compile error)."""
    tmp = work_dir / "candidate.c"
    tmp.write_text(source_text)
    rl = H.run_with_tcc(tmp, low, work_dir)
    rh = H.run_with_tcc(tmp, high, work_dir)
    if not (rl.ok and rh.ok):
        return False
    return rl.signature != rh.signature


def _split_top_level(text: str) -> list[str]:
    """Return lines; we operate at line granularity but never remove the
    csmix/printf scaffolding that defines the observable output."""
    return text.splitlines(keepends=False)


def reduce_text(text: str, low: str, high: str, work_dir: Path) -> str:
    work_dir.mkdir(parents=True, exist_ok=True)
    assert diverges(text, low, high, work_dir), "input does not diverge"

    lines = _split_top_level(text)
    # Protect lines that are structurally required to keep a compilable program
    # that still prints something: includes, csmix, the main signature, the
    # printf/return, and brace-only lines (cheap structural safety).
    def protected(ln: str) -> bool:
        s = ln.strip()
        return (
            s.startswith("#include")
            or "csmix" in s
            or s.startswith("int main")
            or s.startswith("printf")
            or s.startswith("return")
            or s in ("{", "}")
            or s.startswith("struct S")
            or s.startswith("unsigned cs =")
        )

    changed = True
    while changed:
        changed = False
        i = 0
        while i < len(lines):
            if protected(lines[i]):
                i += 1
                continue
            trial = lines[:i] + lines[i + 1:]
            if diverges("\n".join(trial) + "\n", low, high, work_dir):
                lines = trial
                changed = True
                # don't advance i; the next line shifted into position i
            else:
                i += 1
    return "\n".join(lines) + "\n"


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file", help="divergent .c file to reduce")
    ap.add_argument("--low", default="-O0", help="reference O-level (default -O0)")
    ap.add_argument("--high", default="-O2", help="divergent O-level (default -O2)")
    ap.add_argument("-o", "--output", default=None, help="write reduced program here")
    ap.add_argument("--work-dir", default=None, help="scratch build dir")
    args = ap.parse_args(argv)

    usable, reason = H.qemu_available()
    if not usable:
        print(f"[reduce] QEMU/newlib not usable: {reason}", file=sys.stderr)
        return 2

    src = Path(args.file)
    text = src.read_text()
    work_dir = Path(args.work_dir) if args.work_dir else (FUZZ_DIR / "results" / "_reduce")
    work_dir.mkdir(parents=True, exist_ok=True)

    if not diverges(text, args.low, args.high, work_dir):
        print(f"[reduce] {src} does not diverge at {args.low} vs {args.high}; nothing to do",
              file=sys.stderr)
        return 1

    before = len(text.splitlines())
    reduced = reduce_text(text, args.low, args.high, work_dir)
    after = len(reduced.splitlines())
    out = Path(args.output) if args.output else src.with_name(src.stem + "_reduced.c")
    out.write_text(reduced)
    print(f"[reduce] {src.name}: {before} -> {after} lines "
          f"(still diverges {args.low} vs {args.high}) -> {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
