"""
Shared plumbing for the fuzz-driven runner scripts.

Importing this module puts ``tests/fuzz`` on ``sys.path`` and re-exports the
harness entry points, so a runner needs a single import instead of repeating
the bootstrap:

    from sources.fuzz_common import H, generate_program, parse_seed_spec
"""

from __future__ import annotations

import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
FUZZ_DIR = REPO_ROOT / "tests" / "fuzz"
if str(FUZZ_DIR) not in sys.path:
    sys.path.insert(0, str(FUZZ_DIR))

import fuzz_harness as H                                      # noqa: E402
from fuzz_harness import CompileConfig, compile_testcase, MACHINE  # noqa: E402
from gen_c import generate_program, PROFILES                  # noqa: E402

__all__ = [
    "REPO_ROOT",
    "FUZZ_DIR",
    "H",
    "CompileConfig",
    "compile_testcase",
    "MACHINE",
    "generate_program",
    "PROFILES",
    "parse_seed_spec",
]


def parse_seed_spec(args) -> list[int]:
    """Resolve --seed / --seeds RANGE / --count+--start into a seed list."""
    seeds: list[int] = []
    if args.seeds:
        for token in args.seeds.split(","):
            token = token.strip()
            if "-" in token:
                lo, hi = token.split("-", 1)
                seeds.extend(range(int(lo), int(hi) + 1))
            elif token:
                seeds.append(int(token))
    seeds.extend(args.seed or [])
    if args.count:
        seeds.extend(range(args.start, args.start + args.count))
    if not seeds and not args.file:
        seeds = list(range(0, 20))   # sensible default
    # De-dup, preserve order.
    seen = set()
    out = []
    for s in seeds:
        if s not in seen:
            seen.add(s)
            out.append(s)
    return out
