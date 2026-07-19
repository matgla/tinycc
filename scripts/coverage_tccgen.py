#!/usr/bin/env python3
"""
coverage_tccgen.py -- merged line-coverage report for tccgen.c.

tccgen.c is the C parser / type checker / IR-emission frontend.  Most of it
only runs inside the full compile pipeline, so the isolated unit-test binary
(tests/unit/arm/armv8m, which #includes tccgen.c and stubs the IR/ELF/pp
boundary) can only reach its pure `static` helpers.  This script produces the
*merged* picture by combining two coverage sources for the same source file:

  1. the real cross compiler (armv8m-tcc) with tccgen.c instrumented, run over
     the whole compile-test corpus (ir_tests, tests2, frontend, gcc-torture) at
     several -O levels -- this exercises the parser/codegen pipeline; and
  2. the isolated tccgen unit tests (make -C tests/unit/arm/armv8m COVERAGE=1),
     which cover the leaf helpers and error/diagnostic branches the -O2 build
     folds away.

The two are unioned per source line with lcov (gcovr's line-keyed merge cannot
combine two different compilations of the same file -- the -O0 unit build and
the -O2 real build expose different executable-line sets).

Only tccgen.o is instrumented (not the whole compiler): a full-tree --coverage
build makes every armv8m-tcc invocation flush ~80 .gcda files, ~10x slower.

The normal (uninstrumented) build is restored on exit -- armv8m-tcc is left
exactly as it was found, so this is safe to run against a working tree.  The
restore also runs on SIGINT/SIGTERM, so interrupting the sweep does not leave
an instrumented compiler behind.

Every option can also be set via its COV_* environment variable (the CLI flag
wins): COV_TARGET, COV_JOBS, COV_OLEVELS, COV_OUT, COV_NO_TORTURE=1.

Usage:  make coverage-tccgen        (preferred)
        scripts/coverage_tccgen.py [--jobs N] [--olevels "-O0 -O2"] ...
"""

import argparse
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

TOP = Path(__file__).resolve().parent.parent

ARMF = ["-mcpu=cortex-m33", "-mthumb", "-mfloat-abi=soft"]


class Cfg:
    """Resolved run configuration (CLI flag > COV_* env var > default)."""

    def __init__(self, args):
        self.target = args.target
        self.x = f"{self.target}-"
        self.jobs = args.jobs
        self.olevels = args.olevels.split()
        self.out = Path(args.out)
        self.no_torture = args.no_torture
        self.obj = f"{self.x}tccgen.o"
        self.bin = f"{self.x}tcc"
        self.gcno = f"{self.x}tccgen.gcno"
        self.gcda = f"{self.x}tccgen.gcda"
        self.unitdir = TOP / "tests" / "unit" / "arm" / "armv8m"
        self.unit_gcda = self.unitdir / "build_tccgen" / "test_tccgen.gcda"


def run(cmd, **kw):
    """Run a command list (or shell string) rooted at TOP, raising on failure."""
    return subprocess.run(cmd, cwd=TOP, check=True, **kw)


def derive_make_command(cfg: Cfg, target: str, pattern: str) -> str:
    """Pull a single compile/link command line out of `make -n` output."""
    proc = subprocess.run(
        ["make", "-n", f"CROSS_TARGET={cfg.target}", target],
        cwd=TOP, capture_output=True, text=True,
    )
    rx = re.compile(pattern)
    for line in proc.stdout.splitlines():
        if rx.search(line):
            return line
    return ""


def collect_corpus(cfg: Cfg) -> tuple[list, list]:
    """(needs-newlib-includes, freestanding) source lists -- only what exists."""
    inc: list = []
    inc += sorted((TOP / "tests" / "ir_tests").glob("*.c"))
    inc += sorted((TOP / "tests" / "tests2").glob("*.c"))
    frontend = TOP / "tests" / "frontend"
    if frontend.is_dir():
        inc += sorted(frontend.rglob("*.c"))

    free: list = []
    if not cfg.no_torture:
        torture = TOP / "tests" / "gcctestsuite"
        if torture.is_dir():
            free += sorted(torture.rglob("*.c"))
    return inc, free


def sweep(cfg: Cfg, sources: list, extra_flags: list) -> None:
    """Compile every source at every -O level through the instrumented compiler.

    Compile failures are expected and ignored -- the point is to drive tccgen.c,
    not to pass the corpus.
    """
    if not sources:
        return
    compiler = str(TOP / cfg.bin)

    def compile_one(item):
        opt, src = item
        # NB: the shell original looped over $OLEVELS but never passed $O to the
        # compiler, so it swept the same default level N times.  Pass it here.
        cmd = [compiler, "-c", "-w", opt, *extra_flags, str(src), "-o", "/dev/null"]
        try:
            subprocess.run(cmd, cwd=TOP, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, timeout=120)
        except (subprocess.SubprocessError, OSError):
            pass

    work = [(opt, src) for opt in cfg.olevels for src in sources]
    with ThreadPoolExecutor(max_workers=cfg.jobs) as pool:
        list(pool.map(compile_one, work))


def cleanup(cfg: Cfg, work: Path) -> None:
    print("==> restoring normal (uninstrumented) build")
    for f in (cfg.obj, cfg.bin, cfg.gcno, cfg.gcda):
        (TOP / f).unlink(missing_ok=True)
    subprocess.run(["make", "cross"], cwd=TOP,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    shutil.rmtree(work, ignore_errors=True)


def parse_args(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--target", default=os.environ.get("COV_TARGET", "armv8m"),
                    help="cross target prefix (env COV_TARGET)")
    ap.add_argument("--jobs", type=int, default=int(os.environ.get("COV_JOBS", 8)),
                    help="parallel compiles (env COV_JOBS)")
    ap.add_argument("--olevels", default=os.environ.get("COV_OLEVELS") or "-O0 -O1 -O2 -Os",
                    help="optimisation levels to sweep (env COV_OLEVELS)")
    ap.add_argument("--out", default=os.environ.get("COV_OUT", str(TOP / "coverage-tccgen")),
                    help="output directory (env COV_OUT)")
    ap.add_argument("--no-torture", action="store_true",
                    default=os.environ.get("COV_NO_TORTURE", "0") == "1",
                    help="skip the gcc-torture corpus, faster (env COV_NO_TORTURE=1)")
    return ap.parse_args(argv)


def main() -> int:
    cfg = Cfg(parse_args())

    for tool in ("lcov", "geninfo", "genhtml"):
        if shutil.which(tool) is None:
            print(f"coverage_tccgen: missing required tool '{tool}'", file=sys.stderr)
            return 2

    work = Path(tempfile.mkdtemp())
    done = False

    def on_signal(signum, _frame):
        # Restore the normal build before dying, then re-raise the default action.
        nonlocal done
        if not done:
            done = True
            cleanup(cfg, work)
        signal.signal(signum, signal.SIG_DFL)
        os.kill(os.getpid(), signum)

    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, on_signal)

    try:
        print("==> building baseline cross compiler")
        run(["make", "cross"], stdout=subprocess.DEVNULL)

        print(f"==> instrumenting {cfg.obj} (tccgen.c only) and relinking {cfg.bin}")
        (TOP / cfg.obj).unlink(missing_ok=True)
        (TOP / cfg.bin).unlink(missing_ok=True)

        cc_cmd = derive_make_command(cfg, cfg.obj, r"(gcc|cc).* -c tccgen\.c( |$)")
        if not cc_cmd:
            print(f"coverage_tccgen: could not derive compile command for {cfg.obj}",
                  file=sys.stderr)
            return 3
        run(cc_cmd + " --coverage -fprofile-update=atomic", shell=True)

        link_cmd = derive_make_command(cfg, cfg.bin, rf"(gcc|cc) -o {re.escape(cfg.bin)} ")
        if not link_cmd:
            print(f"coverage_tccgen: could not derive link command for {cfg.bin}",
                  file=sys.stderr)
            return 3
        run(link_cmd + " --coverage", shell=True)

        print(f"==> unit-test coverage ({cfg.unitdir.relative_to(TOP)})")
        run(["make", "-C", str(cfg.unitdir), "COVERAGE=1", "run-tccgen"],
            stdout=subprocess.DEVNULL)

        print(f"==> compiling test corpus through instrumented {cfg.bin} "
              f"(levels:{' '.join(cfg.olevels)} jobs:{cfg.jobs})")
        libc = TOP / "tests" / "ir_tests" / "libc_includes"
        inc_flags = [
            f"-I{libc}",
            f"-I{TOP / 'tests' / 'ir_tests' / 'libc_imports'}",
            f"-I{libc / 'newlib'}",
            f"-I{TOP / 'include'}",
        ]
        inc_sources, free_sources = collect_corpus(cfg)
        sweep(cfg, inc_sources, ARMF + inc_flags)
        sweep(cfg, free_sources, [])

        if not (TOP / cfg.gcda).is_file():
            print(f"coverage_tccgen: no {cfg.gcda} produced -- corpus empty?",
                  file=sys.stderr)
            return 4

        print("==> merging coverage (real corpus + unit tests)")
        real_info, unit_info = work / "real.info", work / "unit.info"
        merged_info, tccgen_info = work / "merged.info", work / "tccgen.info"
        run(["geninfo", str(TOP / cfg.gcda), "-o", str(real_info),
             "--gcov-tool", "gcov", "-q"], stderr=subprocess.DEVNULL)
        run(["geninfo", str(cfg.unit_gcda), "-o", str(unit_info),
             "--gcov-tool", "gcov", "-q"], stderr=subprocess.DEVNULL)
        run(["lcov", "-a", str(real_info), "-a", str(unit_info),
             "-o", str(merged_info), "--rc", "geninfo_unexecuted_blocks=1", "-q"],
            stderr=subprocess.DEVNULL)
        run(["lcov", "--extract", str(merged_info), "*/tccgen.c",
             "-o", str(tccgen_info), "-q"], stderr=subprocess.DEVNULL)

        cfg.out.mkdir(parents=True, exist_ok=True)
        shutil.copy(tccgen_info, cfg.out / "tccgen.info")
        run(["genhtml", str(tccgen_info), "-o", str(cfg.out), "-q", "--title",
             "tccgen.c merged coverage (unit tests + real-compiler corpus)"],
            stderr=subprocess.DEVNULL)

        summary = subprocess.run(["lcov", "--summary", str(tccgen_info)],
                                 cwd=TOP, capture_output=True, text=True)
        print()
        print("======================= tccgen.c merged coverage =======================")
        for line in (summary.stdout + summary.stderr).splitlines():
            if re.search(r"lines|functions", line, re.I):
                print(line)
        print(f"  report:    {cfg.out / 'index.html'}")
        print(f"  tracefile: {cfg.out / 'tccgen.info'}")
        print("========================================================================")
        return 0
    finally:
        if not done:
            done = True
            cleanup(cfg, work)


if __name__ == "__main__":
    sys.exit(main())
