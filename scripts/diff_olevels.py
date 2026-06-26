#!/usr/bin/env python3
"""Track 2 -- optimization-level self-consistency differential.

Oracle: a program's observable output (stdout + exit code) must be **identical**
at ``-O0``, ``-O1`` and ``-O2``.  Any divergence means an optimization changed
behaviour -> a candidate miscompile, with the offending O-level pinned.

For each seed we generate a UB-free random C program (``tests/fuzz/gen_c.py``),
compile it with ``armv8m-tcc`` at each O-level, run each under QEMU
``mps2-an505`` (reusing the ``tests/ir_tests`` plumbing via
``tests/fuzz/fuzz_harness.py``), and compare the (stdout, exit) signatures.

On divergence the offending ``.c`` and the per-level outputs are saved to a
results directory and the seed is reported.  Because the generator is UB-free by
construction, a divergence here is a real self-consistency failure (re-check the
generator's guarantees before filing, per the plan's rules).

Usage:
    python scripts/diff_olevels.py --seeds 0-49
    python scripts/diff_olevels.py --seed 0 --seed 7 --seed 42
    python scripts/diff_olevels.py --count 100 --start 0 --results-dir /tmp/fuzz_olevels
    python scripts/diff_olevels.py --file path/to/program.c        # one fixed file

Exit code: 0 if all consistent, 1 if any divergence (or harness unusable when
``--require-qemu`` is given).
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

# Make tests/fuzz importable.
REPO_ROOT = Path(__file__).resolve().parent.parent
FUZZ_DIR = REPO_ROOT / "tests" / "fuzz"
if str(FUZZ_DIR) not in sys.path:
    sys.path.insert(0, str(FUZZ_DIR))

import fuzz_harness as H            # noqa: E402
from gen_c import generate_program  # noqa: E402

DEFAULT_OPT_LEVELS = ["-O0", "-O1", "-O2"]


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


def _save_divergence(results_dir: Path, tag: str, source: Path, results) -> Path:
    results_dir.mkdir(parents=True, exist_ok=True)
    case_dir = results_dir / tag
    case_dir.mkdir(parents=True, exist_ok=True)
    dest_c = case_dir / source.name
    dest_c.write_text(Path(source).read_text())
    summary = [f"# O-level self-consistency divergence: {tag}", ""]
    for r in results:
        summary.append(f"[{r.label}] ok={r.ok} exit={r.exit_code} "
                       f"stdout={r.stdout.strip()!r} err={r.error.strip()!r}")
    (case_dir / "outputs.txt").write_text("\n".join(summary) + "\n")
    return case_dir


def check_one(source: Path, opt_levels, work_dir: Path):
    """Run ``source`` at every opt level; return (consistent, results)."""
    results = [H.run_with_tcc(source, o, work_dir) for o in opt_levels]
    # A build/run failure is itself a divergence-worthy event to report.
    if not all(r.ok for r in results):
        return False, results
    sigs = {r.signature for r in results}
    return (len(sigs) == 1), results


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seed", type=int, action="append", help="a single seed (repeatable)")
    ap.add_argument("--seeds", type=str, help="comma list / ranges, e.g. '0-49,100'")
    ap.add_argument("--count", type=int, default=0, help="number of seeds from --start")
    ap.add_argument("--start", type=int, default=0, help="first seed for --count")
    ap.add_argument("--file", type=str, default=None,
                    help="diff a fixed .c file instead of generated seeds")
    ap.add_argument("--opt-levels", type=str, default=",".join(DEFAULT_OPT_LEVELS),
                    help="comma-separated opt levels (default -O0,-O1,-O2)")
    ap.add_argument("--results-dir", type=str, default=None,
                    help="where to save divergences (default tests/fuzz/results/olevels)")
    ap.add_argument("--work-dir", type=str, default=None,
                    help="scratch build dir (default <results>/_build)")
    ap.add_argument("--require-qemu", action="store_true",
                    help="exit non-zero if QEMU/newlib is unprepared (default: skip)")
    args = ap.parse_args(argv)

    usable, reason = H.qemu_available()
    if not usable:
        msg = f"[diff_olevels] QEMU/newlib not usable: {reason}"
        print(msg, file=sys.stderr)
        return 1 if args.require_qemu else 0

    opt_levels = [o.strip() for o in args.opt_levels.split(",") if o.strip()]
    results_dir = Path(args.results_dir) if args.results_dir else (FUZZ_DIR / "results" / "olevels")
    work_dir = Path(args.work_dir) if args.work_dir else (results_dir / "_build")
    work_dir.mkdir(parents=True, exist_ok=True)

    divergences = 0
    checked = 0

    if args.file:
        source = Path(args.file)
        consistent, results = check_one(source, opt_levels, work_dir)
        checked += 1
        status = "OK " if consistent else "DIVERGE"
        sigs = " | ".join(f"{r.label}={r.stdout.strip()!r}/{r.exit_code}" for r in results)
        print(f"[{status}] {source.name}: {sigs}")
        if not consistent:
            divergences += 1
            d = _save_divergence(results_dir, source.stem, source, results)
            print(f"        saved -> {d}")
    else:
        seeds = parse_seed_spec(args)
        for seed in seeds:
            src = work_dir / f"fuzz_{seed}.c"
            src.write_text(generate_program(seed))
            consistent, results = check_one(src, opt_levels, work_dir)
            checked += 1
            if consistent:
                ref = results[0].stdout.strip()
                print(f"[OK    ] seed {seed}: {ref!r} exit={results[0].exit_code}")
            else:
                divergences += 1
                sigs = " | ".join(
                    f"{r.label}={r.stdout.strip()!r}/{r.exit_code}"
                    f"{'' if r.ok else ' (' + r.error.strip().splitlines()[0] + ')' if r.error.strip() else ''}"
                    for r in results
                )
                print(f"[DIVERGE] seed {seed}: {sigs}")
                d = _save_divergence(results_dir, f"seed_{seed}", src, results)
                print(f"          repro saved -> {d}")

    print(f"\n[diff_olevels] checked={checked} divergences={divergences} "
          f"opt_levels={opt_levels}")
    return 1 if divergences else 0


if __name__ == "__main__":
    raise SystemExit(main())
