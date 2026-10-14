#!/usr/bin/env python3
"""
coverage_opt.py -- line/branch coverage map of the optimizer and back end.

Instruments source/opt/**, source/ir/**, source/backend/** and source/machine/**
of the armv8m cross compiler, compiles the suite's corpus through it at
-O0/-O1/-O2/-Os, and writes:

  <out>/opt.info        lcov tracefile (lines + branches)
  <out>/index.html      genhtml report
  <out>/ranking.txt     the coverage map: per-directory totals, functions ranked
                        by uncovered lines, never-entered functions, and every
                        never-taken bail-out (a guard that stops a transform) next
                        to the transform paths that never ran

The instrumented compiler is built OUT OF TREE (an rsync copy under
<out>/build), so the working tree's armv8m-tcc and objects are never touched and
a `make test` running alongside is unaffected.  Only the four directories above
are instrumented (built -O0 so line and branch records map 1:1 onto source); the
frontend, linker and driver are the normal -O2 build -- a whole-tree --coverage
compiler flushes ~150 .gcda files per invocation.

The corpus is the one the suite compiles: tests/ir_tests/*.c, tests/tests2/*.c
and the gcc torture compile + execute trees (tests/gcctestsuite).  Compile errors
are ignored -- the point is to drive the optimizer, not to pass the corpus.

Every option can also be set by its COV_* environment variable (the CLI flag
wins): COV_JOBS, COV_OLEVELS, COV_OUT, COV_NO_TORTURE=1, COV_EXTRA (extra
sources, whitespace separated, e.g. new tests to measure on top of a corpus run).

Usage:  make coverage-opt                     (preferred)
        scripts/coverage_opt.py [--jobs N] [--olevels "-O1 -O2"] [--no-torture]
        scripts/coverage_opt.py --rank-only   (re-rank an existing opt.info)
        scripts/coverage_opt.py --reuse-build --only FILE.c ...
                                (zero the counters, compile just FILE.c with the
                                 existing instrumented compiler -- "does my new
                                 test reach line N?")
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

TOP = Path(__file__).resolve().parent.parent
TARGET = "armv8m"
X = f"{TARGET}-"
ARMF = ["-mcpu=cortex-m33", "-mthumb", "-mfloat-abi=soft"]
INSTRUMENTED = ("source/opt", "source/ir", "source/backend", "source/machine")
# Unity TUs (source/unity/<group>.c) that #include instrumented sources.
UNITY_PREFIXES = ("opt_", "ir", "arm", "machine")
LCOV_RC = ["--rc", "branch_coverage=1", "--ignore-errors",
           "inconsistent,unused,empty,mismatch,source,negative"]

BAIL_RX = re.compile(r"^\s*(\}?\s*)?(return\b[^;]*;|continue\s*;|break\s*;|goto\s+\w+\s*;)\s*\}?\s*$")
IF_RX = re.compile(r"^\s*(\}\s*else\s+)?if\s*\(|^\s*(while|for)\s*\(|&&|\|\|")


def run(cmd, cwd, **kw):
    return subprocess.run(cmd, cwd=cwd, check=True, **kw)


def parse_args(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    env = os.environ.get
    ap.add_argument("--jobs", type=int, default=int(env("COV_JOBS", 8)))
    ap.add_argument("--olevels", default=env("COV_OLEVELS") or "-O0 -O1 -O2 -Os")
    ap.add_argument("--out", default=env("COV_OUT", str(TOP / "coverage-opt")))
    ap.add_argument("--no-torture", action="store_true",
                    default=env("COV_NO_TORTURE", "0") == "1")
    ap.add_argument("--extra", default=env("COV_EXTRA", ""),
                    help="extra sources compiled on top of the corpus")
    ap.add_argument("--only", nargs="*", default=None,
                    help="compile only these sources (implies a fresh counter set)")
    ap.add_argument("--reuse-build", action="store_true",
                    help="keep <out>/build's instrumented compiler (skip rebuild)")
    ap.add_argument("--rank-only", action="store_true",
                    help="only re-rank <out>/opt.info")
    ap.add_argument("--no-html", action="store_true",
                    help="skip genhtml (quick --only checks)")
    ap.add_argument("--top", type=int, default=60, help="rows per ranking table")
    return ap.parse_args(argv)


# --------------------------------------------------------------------------
# build

def build_instrumented(build: Path, jobs: int) -> Path:
    """rsync the tree to BUILD, build normally, then rebuild the instrumented
    directories with --coverage -O0 and relink.  Returns the compiler path."""
    build.mkdir(parents=True, exist_ok=True)
    print(f"==> syncing sources to {build}")
    run(["rsync", "-a", "--delete",
         "--exclude=/tests/", "--exclude=/.venv/", "--exclude=/.git",
         "--exclude=/coverage-*/", "--exclude=/build*/",
         "--exclude=*.o", "--exclude=*.a", "--exclude=*.gcda", "--exclude=*.gcno",
         "--exclude=/armv8m-tcc", "--exclude=/lib/fp/build/",
         f"{TOP}/", f"{build}/"], cwd=TOP)
    print("==> baseline (uninstrumented) cross build")
    run(["make", f"-j{jobs}", f"{X}tcc"], cwd=build, stdout=subprocess.DEVNULL)

    # Drop exactly the objects that come from the instrumented directories, plus
    # every library and the compiler, then rebuild them with --coverage.
    objroot = build / f"{X}source"
    for d in INSTRUMENTED:
        sub = objroot / d[len("source/"):]
        if sub.is_dir():
            for o in sub.rglob("*.o"):
                o.unlink()
    unity = objroot / "unity"
    if unity.is_dir():
        for o in unity.glob("*.o"):
            if o.name.startswith(UNITY_PREFIXES):
                o.unlink()
    for a in objroot.rglob("*.a"):
        a.unlink()
    (build / f"{X}tcc").unlink(missing_ok=True)

    print("==> instrumented rebuild of " + " ".join(INSTRUMENTED))
    # CPPFLAGS is appended to CFLAGS by the Makefile (and forwarded to the arch
    # sub-make), so it adds the instrumentation without clobbering the
    # configure/Makefile flags.  -O0 last: exact line/branch attribution.
    run(["make", f"-j{jobs}", f"{X}tcc",
         "CPPFLAGS=--coverage -fprofile-update=atomic -O0",
         "LDFLAGS=--coverage"], cwd=build, stdout=subprocess.DEVNULL)
    tcc = build / f"{X}tcc"
    gcnos = list(objroot.rglob("*.gcno"))
    print(f"    {len(gcnos)} instrumented objects")
    if not gcnos:
        sys.exit("coverage_opt: no .gcno produced -- instrumentation failed")
    return tcc


# --------------------------------------------------------------------------
# sweep

def collect_corpus(no_torture: bool) -> list:
    srcs = []
    srcs += sorted((TOP / "tests" / "ir_tests").glob("*.c"))
    srcs += sorted((TOP / "tests" / "tests2").glob("*.c"))
    if not no_torture:
        t = (TOP / "tests" / "gcctestsuite" / "gcc-testsuite" / "gcc-testsuite"
             / "gcc" / "testsuite" / "gcc.c-torture")
        for sub in ("compile", "execute"):
            if (t / sub).is_dir():
                srcs += sorted((t / sub).rglob("*.c"))
    return srcs


def sweep(tcc: Path, build: Path, sources: list, olevels: list, jobs: int) -> None:
    libc = TOP / "tests" / "ir_tests" / "libc_includes"
    flags = ARMF + [f"-B{build}", f"-I{build / 'include'}",
                    f"-I{libc}", f"-I{TOP / 'tests' / 'ir_tests' / 'libc_imports'}",
                    f"-I{libc / 'newlib'}"]
    work = [(o, s) for o in olevels for s in sources]
    done = [0]

    def one(item):
        opt, src = item
        cmd = [str(tcc), "-c", "-w", opt, *flags, str(src), "-o", "/dev/null"]
        try:
            subprocess.run(cmd, cwd=src.parent, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, timeout=180)
        except (subprocess.SubprocessError, OSError):
            pass
        done[0] += 1
        if done[0] % 2000 == 0:
            print(f"    {done[0]}/{len(work)}", flush=True)

    print(f"==> compiling {len(sources)} sources x {' '.join(olevels)} "
          f"({len(work)} compiles, {jobs} jobs)")
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        list(pool.map(one, work))


def capture(build: Path, out: Path) -> Path:
    info = out / "opt.info"
    raw = out / "raw.info"
    print("==> capturing coverage")
    run(["geninfo", str(build / f"{X}source"), "-o", str(raw), "-q",
         "--base-directory", str(build), *LCOV_RC], cwd=build,
        stderr=subprocess.DEVNULL)
    pats = [f"*/{d}/*" for d in INSTRUMENTED]
    run(["lcov", "--extract", str(raw), *pats, "-o", str(info), "-q", *LCOV_RC],
        cwd=build, stderr=subprocess.DEVNULL)
    raw.unlink(missing_ok=True)
    return info


# --------------------------------------------------------------------------
# ranking

class FileCov:
    def __init__(self, path):
        self.path = path
        self.lines = {}            # line -> hits
        self.funcs = {}            # name -> start line
        self.fhits = {}            # name -> hits
        self.br = defaultdict(list)  # line -> [taken or None]


def parse_info(info: Path) -> dict:
    files = {}
    cur = None
    for ln in info.read_text(errors="replace").splitlines():
        if ln.startswith("SF:"):
            p = ln[3:]
            cur = files.setdefault(p, FileCov(p))
        elif cur is None:
            continue
        elif ln.startswith("DA:"):
            l, h = ln[3:].split(",")[:2]
            cur.lines[int(l)] = max(cur.lines.get(int(l), 0), int(h))
        elif ln.startswith("FN:"):
            parts = ln[3:].split(",")
            cur.funcs[parts[-1]] = int(parts[0])
        elif ln.startswith("FNDA:"):
            h, name = ln[5:].split(",", 1)
            cur.fhits[name] = max(cur.fhits.get(name, 0), int(h))
        elif ln.startswith("BRDA:"):
            parts = ln[5:].split(",")
            taken = None if parts[-1] == "-" else int(parts[-1])
            cur.br[int(parts[0])].append(taken)
        elif ln == "end_of_record":
            cur = None
    return files


def rel(p: str) -> str:
    i = p.find("source/")
    return p[i:] if i >= 0 else p


def group_of(path: str) -> str:
    parts = rel(path).split("/")
    # source/opt/ssa/scalar/x.c -> source/opt/ssa ; source/ir/x.c -> source/ir
    if parts[1] == "opt" and len(parts) > 3:
        return "/".join(parts[:3])
    if parts[1] == "backend" and len(parts) > 3:
        return "/".join(parts[:-1][:5])
    return "/".join(parts[:2])


def func_ranges(fc: FileCov):
    starts = sorted((l, n) for n, l in fc.funcs.items())
    return starts


def func_at(starts, line):
    lo, hi, best = 0, len(starts) - 1, None
    while lo <= hi:
        mid = (lo + hi) // 2
        if starts[mid][0] <= line:
            best = starts[mid][1]
            lo = mid + 1
        else:
            hi = mid - 1
    return best


def read_src(path: str):
    try:
        return Path(path).read_text(errors="replace").splitlines()
    except OSError:
        return []


def rank(info: Path, out: Path, top: int) -> str:
    files = parse_info(info)
    lines_out = []
    w = lines_out.append

    # ---- per-directory totals
    tot = defaultdict(lambda: [0, 0, 0, 0, 0, 0])   # lf lh brf brh fnf fnh
    for p, fc in files.items():
        g = tot[group_of(p)]
        g[0] += len(fc.lines)
        g[1] += sum(1 for h in fc.lines.values() if h > 0)
        allb = [t for v in fc.br.values() for t in v]
        g[2] += len(allb)
        g[3] += sum(1 for t in allb if t)
        g[4] += len(fc.funcs)
        g[5] += sum(1 for n in fc.funcs if fc.fhits.get(n, 0) > 0)
    w("== per-directory coverage ==")
    w(f"{'directory':40} {'lines':>16} {'branches':>16} {'functions':>14}")
    grand = [0] * 6
    for g in sorted(tot):
        v = tot[g]
        grand = [a + b for a, b in zip(grand, v)]
        w(f"{g:40} {v[1]:6}/{v[0]:<6} {pct(v[1], v[0]):>5} "
          f"{v[3]:6}/{v[2]:<6} {pct(v[3], v[2]):>5} {v[5]:5}/{v[4]:<5} {pct(v[5], v[4]):>5}")
    v = grand
    w(f"{'TOTAL':40} {v[1]:6}/{v[0]:<6} {pct(v[1], v[0]):>5} "
      f"{v[3]:6}/{v[2]:<6} {pct(v[3], v[2]):>5} {v[5]:5}/{v[4]:<5} {pct(v[5], v[4]):>5}")

    # ---- functions: never entered + partially covered ranked by uncovered lines
    never, partial, guards, paths = [], [], [], []
    for p, fc in files.items():
        starts = func_ranges(fc)
        src = read_src(p)
        per_fn_lines = defaultdict(lambda: [0, 0])
        for l, h in fc.lines.items():
            f = func_at(starts, l)
            if f is None:
                continue
            per_fn_lines[f][0] += 1
            if h == 0:
                per_fn_lines[f][1] += 1
        for f, (n, unc) in per_fn_lines.items():
            if fc.fhits.get(f, 0) == 0:
                never.append((n, rel(p), f, fc.funcs[f]))
            elif unc:
                partial.append((unc, n, rel(p), f, fc.funcs[f]))

        # ---- guards vs transform paths inside executed functions
        sorted_lines = sorted(fc.lines)
        i = 0
        while i < len(sorted_lines):
            l = sorted_lines[i]
            f = func_at(starts, l)
            if fc.lines[l] != 0 or f is None or fc.fhits.get(f, 0) == 0:
                i += 1
                continue
            # a run of consecutive uncovered executable lines
            j = i
            while j + 1 < len(sorted_lines) and fc.lines[sorted_lines[j + 1]] == 0 \
                    and func_at(starts, sorted_lines[j + 1]) == f:
                j += 1
            run_lines = sorted_lines[i:j + 1]
            first = run_lines[0]
            txt = src[first - 1].strip() if 0 < first <= len(src) else ""
            # the controlling line: nearest executed line above the run
            k = i - 1
            ctrl = sorted_lines[k] if k >= 0 else None
            ctrl_txt = src[ctrl - 1].strip() if ctrl and 0 < ctrl <= len(src) else ""
            is_bail = len(run_lines) <= 3 and all(
                BAIL_RX.match(src[x - 1]) or src[x - 1].strip() in ("{", "}", "")
                for x in run_lines if 0 < x <= len(src)) and any(
                BAIL_RX.match(src[x - 1]) for x in run_lines if 0 < x <= len(src))
            # A one-line `if (c) return;` shows up as a covered line with an
            # untaken branch rather than an uncovered run; handled below.
            if is_bail and ctrl and (IF_RX.search(ctrl_txt) or ctrl_txt.endswith("{")):
                guards.append((rel(p), f, ctrl, ctrl_txt[:90], txt[:60]))
            else:
                paths.append((len(run_lines), rel(p), f, first, txt[:80]))
            i = j + 1

        # single-line guards: `if (cond) return x;` on one line, branch never taken
        for l, takens in fc.br.items():
            f = func_at(starts, l)
            if f is None or fc.fhits.get(f, 0) == 0 or fc.lines.get(l, 0) == 0:
                continue
            if not any(t == 0 or t is None for t in takens):
                continue
            t = src[l - 1].strip() if 0 < l <= len(src) else ""
            if re.match(r"^(\}\s*else\s+)?if\s*\(.*\)\s*(return\b[^;]*|continue|break|goto\s+\w+)\s*;\s*$", t):
                guards.append((rel(p), f, l, t[:90], "(same line)"))

    w("")
    w(f"== never-entered functions ({len(never)}; by executable lines) ==")
    for n, p, f, l in sorted(never, reverse=True)[:top]:
        w(f"{n:5}  {p}:{l}  {f}")
    w("")
    w(f"== partially covered functions ranked by uncovered lines (top {top}) ==")
    for unc, n, p, f, l in sorted(partial, reverse=True)[:top]:
        w(f"{unc:5}/{n:<5} {p}:{l}  {f}")
    w("")
    w(f"== never-taken bail-out guards in executed functions ({len(guards)}) ==")
    for p, f, l, ctrl, bail in sorted(guards):
        w(f"{p}:{l}  [{f}]  {ctrl}  ->  {bail}")
    w("")
    w(f"== transform paths never executed in executed functions "
      f"(runs >= 4 lines, {sum(1 for x in paths if x[0] >= 4)}) ==")
    for n, p, f, l, t in sorted((x for x in paths if x[0] >= 4), reverse=True)[:top * 3]:
        w(f"{n:4}  {p}:{l}  [{f}]  {t}")
    text = "\n".join(lines_out) + "\n"
    (out / "ranking.txt").write_text(text)
    return text


def pct(a, b):
    return f"{100.0 * a / b:.1f}%" if b else "-"


# --------------------------------------------------------------------------

def main() -> int:
    a = parse_args()
    out = Path(a.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    build = out / "build"

    if a.rank_only:
        print(rank(out / "opt.info", out, a.top).split("\n\n")[0])
        return 0

    for tool in ("lcov", "geninfo", "genhtml", "rsync"):
        if shutil.which(tool) is None:
            print(f"coverage_opt: missing required tool '{tool}'", file=sys.stderr)
            return 2

    tcc = build / f"{X}tcc"
    if not (a.reuse_build and tcc.is_file()):
        tcc = build_instrumented(build, a.jobs)
    for g in (build / f"{X}source").rglob("*.gcda"):
        g.unlink()

    olevels = a.olevels.split()
    if a.only is not None:
        sources = [Path(s).resolve() for s in a.only]
    else:
        sources = collect_corpus(a.no_torture)
        sources += [Path(s).resolve() for s in a.extra.split()]
    sweep(tcc, build, sources, olevels, a.jobs)

    info = capture(build, out)
    if not a.no_html:
        run(["genhtml", str(info), "-o", str(out), "-q", *LCOV_RC,
             "--title", "optimizer + back end coverage (suite corpus)"],
            cwd=build, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    text = rank(info, out, a.top)
    print()
    print(text.split("\n\n")[0])
    print(f"  report:    {out / 'index.html'}")
    print(f"  ranking:   {out / 'ranking.txt'}")
    print(f"  tracefile: {info}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
