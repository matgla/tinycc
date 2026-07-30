#!/usr/bin/env python3
"""Track 3 -- differential vs arm-none-eabi-gcc.

Oracle: **gcc** (trusted).  The same C program is compiled by ``armv8m-tcc``
(at each O-level) and by ``arm-none-eabi-gcc -O2``, both run under the SAME QEMU
``mps2-an505`` harness (reused from ``tests/fuzz/fuzz_harness.py``).  Any tcc
level whose (stdout, exit) signature differs from gcc's is a candidate
miscompile -- including bugs where all tcc levels AGREE but are wrong, which
Track 2 cannot catch.

Two modes
---------
``--mode random`` (default, the priority path)
    Generate UB-free random C programs (``tests/fuzz/gen_c.py``) and diff each
    tcc O-level against the gcc reference.  UB-freedom is guaranteed by the
    generator, so a divergence is a real wrong-output bug (re-verify generator
    guarantees before filing, per plan rules).

``--mode torture``
    Run the existing gcc c-torture **execute** tests through tcc.  These tests
    are self-checking -- they ``abort()`` (non-zero exit) on a wrong result --
    so we treat a non-zero exit as a candidate miscompile, triaged against the
    suite's known skip / xfail lists (reused from ``tests/gcctestsuite``).  No
    gcc run is needed in this mode (the program is its own oracle).

Usage:
    python scripts/diff_vs_gcc.py --seeds 0-49
    python scripts/diff_vs_gcc.py --mode random --count 100 --start 0
    python scripts/diff_vs_gcc.py --file prog.c --gcc-opt -O2
    python scripts/diff_vs_gcc.py --mode torture --limit 200

Exit code: 0 if everything matched gcc / passed; 1 on any candidate miscompile
(or harness unusable with --require-qemu).
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from sources.fuzz_common import FUZZ_DIR, REPO_ROOT, H, generate_program, parse_seed_spec

DEFAULT_TCC_OPT_LEVELS = ["-O0", "-O1", "-O2"]




def _save_divergence(results_dir: Path, tag: str, source: Path, ref, tcc_results) -> Path:
    results_dir.mkdir(parents=True, exist_ok=True)
    case_dir = results_dir / tag
    case_dir.mkdir(parents=True, exist_ok=True)
    (case_dir / source.name).write_text(Path(source).read_text())
    lines = [f"# tcc-vs-gcc divergence: {tag}", ""]
    lines.append(f"[{ref.label} REFERENCE] ok={ref.ok} exit={ref.exit_code} "
                 f"stdout={ref.stdout.strip()!r} err={ref.error.strip()!r}")
    for r in tcc_results:
        agree = "MATCH" if (r.ok and ref.ok and r.signature == ref.signature) else "DIFF"
        lines.append(f"[{r.label}] {agree} ok={r.ok} exit={r.exit_code} "
                     f"stdout={r.stdout.strip()!r} err={r.error.strip()!r}")
    (case_dir / "outputs.txt").write_text("\n".join(lines) + "\n")
    return case_dir


# ---------------------------------------------------------------------------
# Mode: random
# ---------------------------------------------------------------------------

def run_random(args) -> int:
    ok_ref, reason = H.gcc_reference_available()
    if not ok_ref:
        print(f"[diff_vs_gcc] gcc reference not usable: {reason}", file=sys.stderr)
        return 1 if args.require_qemu else 0

    tcc_opts = [o.strip() for o in args.tcc_opt_levels.split(",") if o.strip()]
    gcc_opt = args.gcc_opt
    results_dir = Path(args.results_dir) if args.results_dir else (FUZZ_DIR / "results" / "vs_gcc")
    work_dir = Path(args.work_dir) if args.work_dir else (results_dir / "_build")
    work_dir.mkdir(parents=True, exist_ok=True)

    divergences = 0
    checked = 0

    def diff_source(source: Path, tag: str):
        nonlocal divergences, checked
        ref = H.run_with_gcc(source, gcc_opt, work_dir)
        checked += 1
        if not ref.ok:
            print(f"[GCC-FAIL] {tag}: reference build/run failed: "
                  f"{ref.error.strip().splitlines()[0] if ref.error.strip() else '?'}")
            return
        tcc_results = [H.run_with_tcc(source, o, work_dir) for o in tcc_opts]
        mismatched = [r for r in tcc_results if not (r.ok and r.signature == ref.signature)]
        if not mismatched:
            print(f"[OK    ] {tag}: gcc{gcc_opt}={ref.stdout.strip()!r}/{ref.exit_code} "
                  f"(all tcc levels match)")
            return
        divergences += 1
        parts = [f"gcc{gcc_opt}={ref.stdout.strip()!r}/{ref.exit_code}"]
        for r in tcc_results:
            mark = "" if (r.ok and r.signature == ref.signature) else "  <-- DIFF"
            parts.append(f"{r.label}={r.stdout.strip()!r}/{r.exit_code}{mark}")
        print(f"[DIVERGE] {tag}:\n          " + "\n          ".join(parts))
        d = _save_divergence(results_dir, tag.replace(" ", "_"), source, ref, tcc_results)
        print(f"          repro saved -> {d}")

    if args.file:
        diff_source(Path(args.file), Path(args.file).stem)
    else:
        for seed in parse_seed_spec(args):
            src = work_dir / f"fuzz_{seed}.c"
            src.write_text(generate_program(seed))
            diff_source(src, f"seed_{seed}")

    print(f"\n[diff_vs_gcc:random] checked={checked} divergences={divergences} "
          f"tcc_opts={tcc_opts} gcc_opt={gcc_opt}")
    return 1 if divergences else 0


# ---------------------------------------------------------------------------
# Mode: torture (self-checking gcc execute tests through tcc)
# ---------------------------------------------------------------------------

def run_torture(args) -> int:
    usable, reason = H.qemu_available()
    if not usable:
        print(f"[diff_vs_gcc] QEMU/newlib not usable: {reason}", file=sys.stderr)
        return 1 if args.require_qemu else 0

    # Reuse the gcctestsuite discovery + skip/xfail lists.
    import importlib.util
    gcc_conf_path = REPO_ROOT / "tests" / "gcctestsuite" / "conftest.py"
    spec = importlib.util.spec_from_file_location("gcc_conftest", gcc_conf_path)
    gcc_conf = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(gcc_conf)

    if not gcc_conf.GCC_TORTURE_PATH.exists():
        print(f"[diff_vs_gcc:torture] torture tests not found at "
              f"{gcc_conf.GCC_TORTURE_PATH}; run 'make download-gcc-tests'",
              file=sys.stderr)
        return 1 if args.require_qemu else 0

    tcc_opts = [o.strip() for o in args.tcc_opt_levels.split(",") if o.strip()]
    results_dir = Path(args.results_dir) if args.results_dir else (FUZZ_DIR / "results" / "torture")
    work_dir = Path(args.work_dir) if args.work_dir else (results_dir / "_build")
    work_dir.mkdir(parents=True, exist_ok=True)

    cases = gcc_conf.discover_gcc_execute_tests()
    if args.limit:
        cases = cases[: args.limit]

    candidates = 0
    ran = 0
    skipped = 0

    for tc in cases:
        skip = gcc_conf.should_skip_gcc_test(tc.source)
        xfail = gcc_conf.is_xfail_test(tc.source)
        if skip or xfail:
            skipped += 1
            continue
        for opt in tcc_opts:
            cflags = opt
            if tc.dg_options:
                cflags = f"{opt} {tc.dg_options}"
            # Reuse the tcc QEMU path; the program self-checks via abort().
            res = H.run_with_tcc(tc.source, cflags, work_dir)
            ran += 1
            # A self-checking execute test passes iff it exits 0.
            passed = res.ok and res.exit_code == 0
            if passed:
                continue
            candidates += 1
            reason = (res.error.strip().splitlines()[0]
                      if res.error.strip() else f"exit={res.exit_code}")
            print(f"[CANDIDATE] {tc.source.stem} {opt}: {reason}")
            results_dir.mkdir(parents=True, exist_ok=True)
            log = results_dir / f"{tc.source.stem}{opt.replace('-', '')}.txt"
            log.write_text(
                f"# torture candidate miscompile: {tc.source} {opt}\n"
                f"exit={res.exit_code} ok={res.ok}\n"
                f"stdout={res.stdout.strip()!r}\n"
                f"error={res.error.strip()!r}\n"
            )

    print(f"\n[diff_vs_gcc:torture] ran={ran} candidates={candidates} "
          f"skipped(known)={skipped} tcc_opts={tcc_opts}")
    return 1 if candidates else 0


# ---------------------------------------------------------------------------

def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mode", choices=["random", "torture"], default="random",
                    help="random C generator (default) or gcc-torture execute tests")
    # random-mode inputs
    ap.add_argument("--seed", type=int, action="append", help="single seed (repeatable)")
    ap.add_argument("--seeds", type=str, help="comma list / ranges, e.g. '0-49,100'")
    ap.add_argument("--count", type=int, default=0, help="number of seeds from --start")
    ap.add_argument("--start", type=int, default=0, help="first seed for --count")
    ap.add_argument("--file", type=str, default=None, help="diff a fixed .c file")
    ap.add_argument("--gcc-opt", type=str, default="-O2", help="gcc reference O-level")
    ap.add_argument("--tcc-opt-levels", type=str, default=",".join(DEFAULT_TCC_OPT_LEVELS),
                    help="comma-separated tcc opt levels")
    # torture-mode inputs
    ap.add_argument("--limit", type=int, default=0,
                    help="(torture) cap the number of discovered tests")
    # shared
    ap.add_argument("--results-dir", type=str, default=None)
    ap.add_argument("--work-dir", type=str, default=None)
    ap.add_argument("--require-qemu", action="store_true",
                    help="exit non-zero if QEMU/newlib is unprepared (default: skip)")
    args = ap.parse_args(argv)

    if args.mode == "torture":
        return run_torture(args)
    return run_random(args)


if __name__ == "__main__":
    raise SystemExit(main())
