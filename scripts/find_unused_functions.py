#!/usr/bin/env python3
"""
Whole-program unused-function scan; see docs/find_unused_functions.md

Runs cppcheck's --enable=unusedFunction over the compiler sources with the
armv8m build config pinned (see Makefile DEF-armv8m).  TCC_LOG_ALL and
CONFIG_TCC_DEBUG keep debug-only call sites visible, so a helper reachable
only from a log statement is not reported as dead.

Paths are passed relative to the repo root because the report is filtered on
a leading "tests/" to drop findings inside the unit tests themselves.
"""

import argparse
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# Pin the armv8m build config (see Makefile DEF-armv8m).
DEFINES = [
    "-DTCC_TARGET_ARM", "-DTCC_ARM_VFP", "-DTCC_ARM_EABI", "-DTCC_ARM_HARDFLOAT",
    "-DTCC_TARGET_ARM_THUMB", "-DTCC_TARGET_ARM_ARCHV8M",
    "-DTCC_LOG_ALL=1", "-DCONFIG_TCC_DEBUG=1",
]
INCLUDES = ["-I.", "-Iir", "-Iir/opt", "-Iarch", "-Iarch/arm", "-Iarch/arm/thumb"]


def collect_sources(with_tests: bool) -> list[str]:
    """Repo-relative .c paths, in the same order the shell version used."""
    sources = sorted(p.name for p in REPO.glob("*.c"))
    for d in ("ir", "arch"):
        sources += sorted(str(p.relative_to(REPO)) for p in (REPO / d).rglob("*.c"))
    if with_tests:
        unit = REPO / "tests" / "unit"
        if unit.is_dir():
            sources += sorted(str(p.relative_to(REPO)) for p in unit.rglob("*.c"))
    return sources


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--no-tests", action="store_true",
                    help="don't count tests/unit sources as callers")
    args = ap.parse_args()

    sources = collect_sources(with_tests=not args.no_tests)
    if not sources:
        print("find_unused_functions: no sources found", file=sys.stderr)
        return 2

    cmd = ["cppcheck", "--enable=unusedFunction", "--quiet"] + DEFINES + INCLUDES + sources
    try:
        proc = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True)
    except FileNotFoundError:
        print("find_unused_functions: cppcheck not found in PATH", file=sys.stderr)
        return 2

    # cppcheck reports findings on stderr; the shell version merged both streams.
    merged = proc.stdout.splitlines() + proc.stderr.splitlines()
    hits = sorted({
        line for line in merged
        if "[unusedFunction]" in line and not line.startswith("tests/")
    })
    for line in hits:
        print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
