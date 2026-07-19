#!/usr/bin/env python3
"""Compile, link and run the QEMU corpus once, yielding per test BOTH a
functional verdict and a deterministic cycle count.

Links tests/ir_tests/qemu/mps2-an505/cyc_shim.c alongside each test and runs it
under `qemu -icount`, which makes SysTick a deterministic function of
instructions retired.  Mechanism, rejected alternatives and caveats:
docs/qemu_cycle_profiling.md.

Cycles are instructions-retired, not silicon cycles -- reproducible, and so
diffable between two builds, which is what metrics/compare_worktree.py wants.
For real cycles use tests/benchmarks/run_benchmark.py (RP2350 hardware).

The `passed` verdict is deliberately cruder than tests/ir_tests/test_qemu.py:
in-order line matching (float-tolerant) plus the exit code, with none of its
tagged/args/xfail handling.  It is not a replacement for `make test` as an
absolute pass/fail -- compare_worktree.py runs this over BOTH sides and gates on
tests that pass on the baseline but fail on the worktree, so any test this
harness systematically mishandles fails identically on both sides and cancels
out.  Only a real behaviour change moves the verdict.

Standalone:
    python3 metrics/qemu_corpus.py --opt=-O2 --suite ir -j8
"""

import argparse
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent
MACHINE_DIR = REPO_ROOT / "tests" / "ir_tests" / "qemu" / "mps2-an505"
SHIM_SRC = MACHINE_DIR / "cyc_shim.c"
MACHINE = "mps2-an505"

# ~1 tick/instruction; exact below the 24-bit SysTick wrap (~16.7M instructions).
DEFAULT_SHIFT = 5
WRAP_CEILING = 0x00FFFFFF

# Suites whose tests link and run; the rest are compile-only checks.
RUNNABLE_SUITES = {"ir", "tests2", "float", "bug", "gcc-execute"}

CYCLES_RE = re.compile(r"^##CYCLES (\d+)$", re.M)
PROBE_C = "int main(void){return 0;}\n"

FLOAT_RE = re.compile(r"[-+]?\d+\.\d+")
FLOAT_TOL = 1e-5


def _expect_lines(src: str):
    """(expected_lines, expected_exit) for a test, or (None, 0) when it has no
    .expect file (the gcc torture contract: abort() on failure, else exit 0)."""
    sys.path.insert(0, str(REPO_ROOT / "tests" / "ir_tests"))
    from test_qemu import load_expect_file
    try:
        lines, exit_code = load_expect_file(src)
    except (FileNotFoundError, OSError):
        return None, 0
    return lines, 0 if exit_code is None else exit_code


def _line_matches(expected: str, actual: str) -> bool:
    """Exact match, else compare embedded floats numerically -- printf rounding
    differs harmlessly between builds (mirrors test_qemu._expect_line)."""
    if expected in actual:
        return True
    floats = list(FLOAT_RE.finditer(expected))
    if not floats:
        return False
    pattern, last = [], 0
    for fm in floats:
        pattern.append(re.escape(expected[last:fm.start()]))
        pattern.append(r"([-+]?\d+\.\d+)")
        last = fm.end()
    pattern.append(re.escape(expected[last:]))
    m = re.search("".join(pattern), actual)
    if not m:
        return False
    return all(abs(float(m.group(i + 1)) - float(fm.group(0))) <= FLOAT_TOL
               for i, fm in enumerate(floats))


def verify(src: str, stdout: str, exit_status: int) -> bool:
    """Did this test behave? In-order line match plus exit code."""
    expected, want_exit = _expect_lines(src)
    if exit_status != want_exit:
        return False
    if expected is None:
        return True
    out = stdout.splitlines()
    i = 0
    for want in expected:
        if not want.strip():
            continue
        while i < len(out) and not _line_matches(want, out[i]):
            i += 1
        if i >= len(out):
            return False
        i += 1
    return True


def eprint(msg: str) -> None:
    print(f"[cycles] {msg}", file=sys.stderr, flush=True)


class LinkEnv:
    """The Makefile's link recipe, learned once and replayed per test.

    Invoking `make` per test would re-evaluate its dozen $(shell ...) probes
    every time -- unusable across thousands of tests.  So run it once to build
    boot.o, capture the link line with `make -n`, and substitute per test.
    Relies on the Makefile passing out-of-tree TEST_FILES straight to the link
    step (its patsubst only rewrites .c under the machine dir), so an object we
    compiled ourselves drops into the same slot.
    """

    def __init__(self, tcc: Path, opt: str, workdir: Path):
        self.tcc = tcc
        self.opt = opt
        self.workdir = workdir
        self.probe = workdir / "probe.c"
        self.probe.write_text(PROBE_C)
        self.argv = self._learn()
        self.shim_obj = self._compile_shim()

    def _make_cmd(self, dry: bool) -> list[str]:
        return ["make", *(["-n"] if dry else ["-s"]), "-C", str(MACHINE_DIR),
                f"CC={self.tcc}", f"EXTRA_CFLAGS={self.opt}",
                f"TEST_FILES={self.probe}", f"OUTPUT={self.workdir}",
                f"TARGET={self.workdir / 'probe.elf'}"]

    def _learn(self) -> list[str]:
        # `-n` must run BEFORE the real build: once probe.elf exists make
        # reports it up to date and prints no recipe to capture.
        dry = subprocess.run(self._make_cmd(dry=True), capture_output=True, text=True)
        argv = None
        for line in reversed(dry.stdout.splitlines()):
            if str(self.tcc) in line and "probe.elf" in line and "-c" not in line.split():
                argv = shlex.split(line)
                break
        if argv is None:
            raise RuntimeError(
                f"could not find the link line in `make -n` output (exit "
                f"{dry.returncode})\n{dry.stdout[-800:]}{dry.stderr[-800:]}")
        real = subprocess.run(self._make_cmd(dry=False), capture_output=True, text=True)
        if not (self.workdir / "boot.o").exists():
            raise RuntimeError(
                f"probe build produced no boot.o (exit {real.returncode}); "
                f"newlib may be unbuilt\n{real.stderr[-800:]}")
        return argv

    def _compile_shim(self) -> Path:
        """Compile the shim as its own TU at -O0.

        Mandatory, not a preference: at -O1+ const-prop folds the shim's
        constant MMIO address into the dereference and trips the
        constant-address deref miscompile (docs/bugs.md), so every SysTick read
        returns the same constant and every test reports 0 cycles.
        """
        obj = self.workdir / "cyc_shim.o"
        cflags = [f for f in self.argv[1:] if f.startswith(("-I", "-m", "-f", "-B"))
                  and not f.startswith("-fvisibility")]
        r = subprocess.run([str(self.tcc), "-O0", *cflags, "-c", str(SHIM_SRC),
                            "-o", str(obj)], capture_output=True, text=True)
        if r.returncode != 0 or not obj.exists():
            raise RuntimeError(f"shim compile failed (exit {r.returncode})\n{r.stderr[-800:]}")
        return obj

    def link_argv(self, src: str, elf: Path, extra_flags: str = "") -> list[str]:
        out = []
        for tok in self.argv:
            if tok == str(self.probe):
                out.extend([src, str(self.shim_obj)])
            elif tok == str(self.workdir / "probe.elf"):
                out.append(str(elf))
            else:
                out.append(tok)
        return out + (shlex.split(extra_flags) if extra_flags else [])


def run_one(env: LinkEnv, suite: str, src: str, flags: str, shift: int,
            timeout: int) -> dict | None:
    """Link + run one test.

    Returns a row for anything that ran, with `passed` set and `cycles` possibly
    None -- a crash or hang yields no cycle marker but IS the failure the gate
    exists to catch, so it must not be dropped.  None means the test never ran
    (link failed), which is not a runtime verdict.
    """
    test = Path(src).stem
    elf = env.workdir / f"{suite}_{test}.elf"
    row = {"suite": suite, "test": test, "cycles": None, "passed": 0}
    try:
        link = subprocess.run(env.link_argv(src, elf, flags), capture_output=True,
                              text=True, timeout=timeout)
        if link.returncode != 0 or not elf.exists():
            return None
        run = subprocess.run(
            ["qemu-system-arm", "-machine", MACHINE, "-nographic", "-semihosting",
             "-icount", f"shift={shift}", "-kernel", str(elf)],
            capture_output=True, text=True, timeout=timeout,
            # stdin MUST NOT be the terminal: `-nographic` wires the serial to
            # stdio, and qemu then puts the tty in raw + O_NONBLOCK mode.  fds
            # 0/1/2 share one open file description, so that turns the caller's
            # stderr non-blocking too and concurrent writes die with EAGAIN --
            # and a killed qemu never restores it, wrecking the terminal.
            stdin=subprocess.DEVNULL)
    except subprocess.TimeoutExpired:
        return row              # hang: ran, did not pass
    except OSError:
        return None
    finally:
        elf.unlink(missing_ok=True)

    row["passed"] = int(verify(src, run.stdout, run.returncode))
    m = CYCLES_RE.search(run.stdout)
    if m:
        cycles = int(m.group(1))
        # Past the wrap the shim undercounts silently, which reads as a win.
        if cycles >= WRAP_CEILING:
            eprint(f"{suite}/{test}: {cycles} at/over the SysTick wrap -- no "
                   f"count (would undercount; see docs/qemu_cycle_profiling.md)")
        else:
            row["cycles"] = cycles
    return row


def run_corpus(tcc: Path, opt: str, suite_filter: str, jobs: int,
               shift: int = DEFAULT_SHIFT, timeout: int = 60) -> list[dict]:
    """Verdict + cycle rows for every runnable test in `suite_filter` at `opt`."""
    sys.path.insert(0, str(REPO_ROOT / "scripts"))
    from regression_disasm import collect_tests

    tests = [(s, src, f) for s, src, f in collect_tests(suite_filter)
             if s in RUNNABLE_SUITES]
    if not tests:
        eprint(f"no runnable tests for suite={suite_filter}")
        return []

    workdir = Path(tempfile.mkdtemp(prefix="tcc-cycles."))
    try:
        env = LinkEnv(tcc, opt, workdir)
        eprint(f"measuring {len(tests)} tests at {opt} "
               f"(icount shift={shift}, jobs={jobs}) ...")
        with ThreadPoolExecutor(max_workers=jobs) as ex:
            rows = list(ex.map(
                lambda t: run_one(env, t[0], t[1], t[2], shift, timeout), tests))
    finally:
        shutil.rmtree(workdir, ignore_errors=True)

    got = [r for r in rows if r]
    timed = [r for r in got if r["cycles"] is not None]
    failed = [r for r in got if not r["passed"]]
    total = sum(r["cycles"] for r in timed)
    eprint(f"{opt}: {len(got)}/{len(tests)} tests ran, {len(failed)} failed; "
           f"{len(timed)} timed, {total:,} cycles total")
    return got


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--tcc", default=str(REPO_ROOT / "armv8m-tcc"))
    p.add_argument("--opt", default="-O2")
    p.add_argument("--suite", default="all")
    p.add_argument("--shift", type=int, default=DEFAULT_SHIFT)
    p.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 4)
    args = p.parse_args(argv)

    rows = run_corpus(Path(args.tcc), args.opt, args.suite, args.jobs, args.shift)
    print("suite,test,passed,cycles")
    for r in sorted(rows, key=lambda r: -(r["cycles"] or 0)):
        print(f"{r['suite']},{r['test']},{r['passed']},"
              f"{'' if r['cycles'] is None else r['cycles']}")
    return 0 if rows else 1


if __name__ == "__main__":
    sys.exit(main())
