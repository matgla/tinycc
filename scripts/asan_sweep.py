#!/usr/bin/env python3
"""
asan_sweep.py -- sweep the corpus with an instrumented armv8m-tcc and report
the COMPILER's own memory-safety / undefined-behaviour bugs, deduplicated.

The oracle is the sanitizer (or valgrind) report printed by tcc itself, never
the compile exit code: a plain "unsupported feature" error is not a hit.  Each
compile can yield several findings (one ASan error, any number of UBSan
runtime errors, LSan leak blocks, valgrind error blocks); a finding's dedup key
is its kind plus its top meaningful frames (UBSan: its source location), so one
bug seen across a thousand files is one entry.

Compilers.  The tree's armv8m-tcc is NOT instrumented (config.mak has no
-fsanitize by default).  Build an instrumented one out of tree:

    scripts/build_sanitized_cross.sh DIR [asan|lowmem|o0only|valgrind]

or let this script do it (--variant, built under --build-root), so the tree's
armv8m-tcc never changes under anyone running tests against it.  Variants:
  asan      ASan+UBSan (clang when available: gcc's libubsan is often absent)
  lowmem    asan + -DCONFIG_TCC_LOW_MEM   (device-sized tables and arenas)
  o0only    asan + -DCONFIG_TCC_O0_ONLY   (the Pico 2 compiler; -O flags inert)
  valgrind  plain -O2 -g build, every compile run under valgrind memcheck
            (--track-origins=yes): uninitialised reads ASan cannot see

Shapes (--shape, comma-separated):
  single    one file per compile                              (-c f.c)
  multi-c   two files on one command line, -c                 (-c a.c b.c)
            tcc compiles them one after another in fresh TCCStates -- any
            static state that survives tcc_delete leaks into the second file
  multi-r   two files into one relocatable object, -r         (-r a.c b.c -o x.o)
            both files share ONE TCCState: per-file tables, token-keyed caches

Corpora: gcc-torture (execute + compile), tests2, ir_tests, all.  Multi-file
shapes pair files of the chosen corpus deterministically (i with i+1, and i
with i+N/2), so every file appears first and second.

Examples:
  make asan-sweep                              # default matrix, see Makefile
  scripts/asan_sweep.py --variant asan --corpus tests2 -j 8
  scripts/asan_sweep.py --variant asan --corpus all --olevels -O0,-O1,-O2,-Os \\
        --debug-info both --shape single,multi-c,multi-r -j 8 --report r.txt
  scripts/asan_sweep.py --variant valgrind --corpus ir_tests --sample 200 -j 8
  scripts/asan_sweep.py --compiler /path/to/armv8m-tcc --corpus tests2

Leaks are off by default (tcc keeps per-TU arenas until exit on purpose);
--leaks turns LeakSanitizer on.  Exit status is 1 when any finding is
reported, so the sweep can gate a CI job.
"""

import argparse
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile
import threading
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# --------------------------------------------------------------------------
# Report parsing
# --------------------------------------------------------------------------

ASAN_ERR_RE = re.compile(r"ERROR: AddressSanitizer: (\S+)")
ASAN_SEGV_RE = re.compile(r"AddressSanitizer: (SEGV|stack-overflow|ABRT|BUS|FPE)")
LSAN_BLOCK_RE = re.compile(r"^(Direct|Indirect) leak of \d+ byte\(s\) in \d+ object\(s\) allocated from:",
                           re.M)
UBSAN_RE = re.compile(r"^(?P<loc>\S+:\d+:\d+): runtime error: (?P<msg>.*)$", re.M)
FRAME_RE = re.compile(r"#\d+\s+0x[0-9a-f]+\s+in\s+(\S+)")
VG_ERR_HEAD_RE = re.compile(r"^==\d+== (?P<kind>(Invalid (read|write|free)[^\n]*"
                            r"|Conditional jump or move depends on uninitialised value\(s\)"
                            r"|Use of uninitialised value[^\n]*"
                            r"|Syscall param[^\n]*uninitialised[^\n]*"
                            r"|Source and destination overlap[^\n]*"
                            r"|Mismatched free[^\n]*"
                            r"|Argument '[^']*' of function \S+ has a fishy[^\n]*"
                            r"|Process terminating with default action[^\n]*))$", re.M)
VG_FRAME_RE = re.compile(r"^==\d+==\s+(?:at|by) 0x[0-9A-F]+: (\S+)", re.M)

NOISE_FRAMES = {
    "malloc", "calloc", "realloc", "free", "reallocarray", "memcpy", "memmove",
    "memset", "strlen", "strcmp", "memcmp", "strcpy", "strncmp",
    "realloc.part.0", "malloc.part.0",
    "default_reallocator", "default_realloc",
    "tcc_malloc", "tcc_mallocz", "tcc_realloc", "tcc_realloc_debug",
    "tcc_malloc_debug", "tcc_mallocz_debug", "tcc_free", "tcc_strdup",
    "__libc_start_main", "__libc_start_call_main", "_start", "main",
}


def _is_noise(sym):
    if sym in NOISE_FRAMES:
        return True
    for p in ("__asan", "__ubsan", "__lsan", "__interceptor_", "__sanitizer",
              "___interceptor_", "_vgr", "__GI_", "__memcpy", "__memmove",
              "__memset", "__strlen", "__libc_", "operator"):
        if sym.startswith(p):
            return True
    return False


def _top_frames(text, regex, k=3):
    out = []
    for m in regex.finditer(text):
        sym = m.group(1)
        if _is_noise(sym):
            continue
        out.append(sym)
        if len(out) >= k:
            break
    return out


def parse_findings(stderr, leaks):
    """Return [(key, summary)] for every distinct finding in one compile."""
    found = []
    # UBSan: one per runtime error line, keyed by location + message shape.
    for m in UBSAN_RE.finditer(stderr):
        loc = re.sub(r"^.*?/(source/)", r"\1", m.group("loc"))
        msg = re.sub(r"-?\b\d+\b|0x[0-9a-f]+", "N", m.group("msg"))
        found.append((f"ubsan {loc}: {msg}", f"{loc}: runtime error: {m.group('msg')}"[:240]))
    # ASan: one error per process (it aborts).
    m = ASAN_ERR_RE.search(stderr) or ASAN_SEGV_RE.search(stderr)
    if m:
        tail = stderr[m.start():]
        frames = _top_frames(tail.split("allocated by thread")[0].split("freed by thread")[0],
                             FRAME_RE)
        key = f"asan {m.group(1)}: " + " <- ".join(frames)
        line = stderr[m.start():].splitlines()[0]
        found.append((key, line.strip()[:240]))
    # LSan: one per leak block.
    if leaks:
        for mm in LSAN_BLOCK_RE.finditer(stderr):
            body = stderr[mm.end():mm.end() + 4000].split("\n\n")[0]
            frames = _top_frames(body, FRAME_RE)
            found.append((f"lsan {mm.group(1).lower()}: " + " <- ".join(frames),
                          mm.group(0)))
    # valgrind memcheck: one per error block.
    for mm in VG_ERR_HEAD_RE.finditer(stderr):
        body = stderr[mm.end():mm.end() + 6000]
        nxt = re.search(r"^==\d+== \S", body, re.M)
        # stop at the next error head (a line with text right after "== ")
        body = body[:nxt.start()] if nxt else body
        frames = _top_frames(body.split("Address 0x")[0].split("Uninitialised value was")[0],
                             VG_FRAME_RE)
        kind = re.sub(r"\d+", "N", mm.group("kind"))
        found.append((f"valgrind {kind}: " + " <- ".join(frames), mm.group("kind")))
    # dedup within the compile
    seen, out = set(), []
    for k, s in found:
        if k not in seen:
            seen.add(k)
            out.append((k, s))
    return out


# --------------------------------------------------------------------------
# Corpus
# --------------------------------------------------------------------------

def _gcc_torture_root():
    return REPO / "tests/gcctestsuite/gcc-testsuite/gcc/testsuite/gcc.c-torture"


def enumerate_corpus(corpus):
    files = []
    if corpus in ("gcc-torture", "all"):
        root = _gcc_torture_root()
        if not root.exists():
            print(f"warning: gcc-torture not found at {root} "
                  f"(run 'make download-gcc-tests')", file=sys.stderr)
        else:
            files += sorted((root / "execute").rglob("*.c"))
            files += sorted((root / "compile").glob("*.c"))
    if corpus in ("tests2", "all"):
        files += sorted((REPO / "tests/tests2").glob("*.c"))
    if corpus in ("ir_tests", "all"):
        files += sorted((REPO / "tests/ir_tests").glob("*.c"))
    return files


def make_pairs(files):
    n = len(files)
    pairs = []
    for i in range(0, n - 1, 2):
        pairs.append((files[i], files[i + 1]))
    h = n // 2
    for i in range(h):
        if i + h < n:
            pairs.append((files[i + h], files[i]))
    return pairs


# --------------------------------------------------------------------------
# Flags (mirror tests/ir_tests/qemu/mps2-an505/Makefile for armv8m-tcc)
# --------------------------------------------------------------------------

GCC_ABI_FLAGS = ["-mcpu=cortex-m33", "-mthumb", "-mfloat-abi=soft"]
DEFAULT_ABI_FLAGS = ["-nostdlib", "-fvisibility=hidden", *GCC_ABI_FLAGS, "-ffunction-sections"]


def arm_sysroot():
    try:
        proc = subprocess.run(["arm-none-eabi-gcc", *GCC_ABI_FLAGS, "--print-sysroot"],
                              capture_output=True, text=True)
        if proc.returncode == 0 and proc.stdout.strip():
            return proc.stdout.strip()
    except (OSError, subprocess.SubprocessError):
        pass
    return "/usr/arm-none-eabi"


def default_include_flags():
    libc = (REPO / "tests" / "ir_tests" / "libc_includes").resolve()
    imports = (REPO / "tests" / "ir_tests" / "libc_imports").resolve()
    return [f"-I{libc}", f"-I{imports}", f"-I{libc / 'newlib'}",
            f"-I{arm_sysroot()}/include", f"-I{REPO / 'include'}"]


# --------------------------------------------------------------------------
# Running
# --------------------------------------------------------------------------

VALGRIND_CMD = ["valgrind", "--tool=memcheck", "--track-origins=yes",
                "--error-limit=no", "--num-callers=16", "-q"]


def san_env(leaks):
    env = dict(os.environ)
    env["ASAN_OPTIONS"] = (f"detect_leaks={1 if leaks else 0}:abort_on_error=0:"
                           "allocator_may_return_null=0:detect_stack_use_after_return=0")
    env["LSAN_OPTIONS"] = f"detect_leaks={1 if leaks else 0}"
    env["UBSAN_OPTIONS"] = "print_stacktrace=1:halt_on_error=0"
    return env


def build_cmd(compiler, base_flags, opt, dbg, shape, sources, valgrind):
    cmd = [str(compiler), f"-B{REPO}", *base_flags, opt]
    if dbg:
        cmd.append("-g")
    if shape == "multi-r":
        cmd += ["-r", *[str(s) for s in sources], "-o", "out.o"]
    elif shape == "multi-c":
        cmd += ["-c", *[str(s) for s in sources]]
    else:
        cmd += ["-c", *[str(s) for s in sources], "-o", "out.o"]
    if valgrind:
        cmd = VALGRIND_CMD + cmd
    return cmd


def run_job(cmd, env, timeout):
    with tempfile.TemporaryDirectory(prefix="sweep.") as d:
        try:
            p = subprocess.run(cmd, cwd=d, env=env, stdout=subprocess.DEVNULL,
                               stderr=subprocess.PIPE, timeout=timeout)
            return p.returncode, p.stderr.decode("utf-8", "replace")
        except subprocess.TimeoutExpired as e:
            return -1, (e.stderr or b"").decode("utf-8", "replace") + "\n[timeout]"


def build_variant(variant, build_root):
    dest = Path(build_root) / variant
    script = REPO / "scripts" / "build_sanitized_cross.sh"
    print(f"building {variant} compiler in {dest} ...", file=sys.stderr)
    subprocess.run([str(script), str(dest), variant], check=True)
    return dest / "armv8m-tcc"


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--compiler", default=None,
                    help="instrumented armv8m-tcc to use (default: build --variant)")
    ap.add_argument("--variant", default="asan",
                    choices=["asan", "lowmem", "o0only", "valgrind"],
                    help="compiler build variant (built out of tree unless --compiler)")
    ap.add_argument("--valgrind", action="store_true",
                    help="run every compile under valgrind (implied by --variant valgrind)")
    ap.add_argument("--build-root", default=str(REPO / ".sanitize"),
                    help="where --variant compilers are built (default: .sanitize/)")
    ap.add_argument("--corpus", default="all",
                    choices=["gcc-torture", "tests2", "ir_tests", "all"])
    ap.add_argument("--olevels", default="-O0,-O1,-O2,-Os")
    ap.add_argument("--debug-info", default="no", choices=["no", "yes", "both"],
                    help="compile without -g, with -g, or both")
    ap.add_argument("--shape", default="single",
                    help="comma list of single,multi-c,multi-r")
    ap.add_argument("-j", "--jobs", type=int, default=8)
    ap.add_argument("--shard", default=None, help="i/N -- only shard i of N (1-based)")
    ap.add_argument("--limit", type=int, default=0, help="cap work items (after sharding)")
    ap.add_argument("--sample", type=int, default=0,
                    help="random (seeded) sample of this many files per corpus pass")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--timeout", type=int, default=120)
    ap.add_argument("--leaks", action="store_true", help="enable LeakSanitizer findings")
    ap.add_argument("--include-flags", default="")
    ap.add_argument("--abi-flags", default="")
    ap.add_argument("--extra-flags", default="", help="appended to every compile")
    ap.add_argument("--report", default=None)
    ap.add_argument("--list-hits-raw", default=None,
                    help="append every hit as file|flags|key here")
    ap.add_argument("--keep-logs", default=None,
                    help="directory: save the full stderr of the first hit of each key")
    args = ap.parse_args()

    valgrind = args.valgrind or args.variant == "valgrind"
    if valgrind and not shutil.which("valgrind"):
        print("error: valgrind not found", file=sys.stderr)
        return 2
    compiler = Path(args.compiler) if args.compiler else build_variant(args.variant, args.build_root)
    if not (compiler.is_file() and os.access(compiler, os.X_OK)):
        print(f"error: compiler not found or not executable: {compiler}", file=sys.stderr)
        return 2

    include_flags = args.include_flags.split() or default_include_flags()
    abi_flags = args.abi_flags.split() or DEFAULT_ABI_FLAGS
    base_flags = abi_flags + include_flags + args.extra_flags.split()
    olevels = [o.strip() for o in args.olevels.split(",") if o.strip()]
    dbgs = {"no": [False], "yes": [True], "both": [False, True]}[args.debug_info]
    shapes = [s.strip() for s in args.shape.split(",") if s.strip()]

    files = enumerate_corpus(args.corpus)
    if args.sample and args.sample < len(files):
        files = sorted(random.Random(args.seed).sample(files, args.sample))

    jobs = []
    for shape in shapes:
        units = [(f,) for f in files] if shape == "single" else make_pairs(files)
        for u in units:
            for opt in olevels:
                for dbg in dbgs:
                    jobs.append((shape, u, opt, dbg))
    if args.shard:
        i, n = (int(x) for x in args.shard.split("/"))
        jobs = [j for idx, j in enumerate(jobs) if idx % n == i - 1]
    if args.limit:
        jobs = jobs[:args.limit]

    env = san_env(args.leaks)
    bugs = {}
    lock = threading.Lock()
    counters = {"done": 0, "hits": 0, "timeouts": 0}
    raw = []
    if args.keep_logs:
        Path(args.keep_logs).mkdir(parents=True, exist_ok=True)

    def work(job):
        shape, units, opt, dbg = job
        cmd = build_cmd(compiler, base_flags, opt, dbg, shape, units, valgrind)
        rc, err = run_job(cmd, env, args.timeout * (6 if valgrind else 1))
        findings = parse_findings(err, args.leaks)
        rel = " ".join(os.path.relpath(u, REPO) for u in units)
        flags = f"{shape} {opt}{' -g' if dbg else ''}"
        with lock:
            counters["done"] += 1
            if rc == -1:
                counters["timeouts"] += 1
            if findings:
                counters["hits"] += 1
            for key, summ in findings:
                b = bugs.setdefault(key, {"summary": summ, "count": 0, "files": set(),
                                          "repro": (rel, flags), "cmd": cmd})
                if b["count"] == 0 and args.keep_logs:
                    idx = len(bugs)
                    Path(args.keep_logs, f"hit{idx:03d}.log").write_text(
                        " ".join(cmd) + "\n\n" + err)
                b["count"] += 1
                b["files"].add(rel)
                raw.append(f"{rel}|{flags}|{key}")
            if counters["done"] % 500 == 0:
                print(f"  ... {counters['done']}/{len(jobs)} compiles, "
                      f"{len(bugs)} distinct finding(s)", file=sys.stderr, flush=True)

    with ThreadPoolExecutor(max_workers=args.jobs) as ex:
        list(ex.map(work, jobs))

    lines = ["=" * 78, "sanitizer sweep report", "=" * 78,
             f"compiler          : {compiler}",
             f"variant           : {'valgrind' if valgrind else args.variant}",
             f"corpus            : {args.corpus} ({len(files)} files)",
             f"olevels           : {','.join(olevels)}",
             f"debug info        : {args.debug_info}",
             f"shapes            : {','.join(shapes)}",
             f"compiles run      : {counters['done']}",
             f"timeouts          : {counters['timeouts']}",
             f"compiles with hit : {counters['hits']}",
             f"distinct findings : {len(bugs)}", ""]
    for n, (key, b) in enumerate(sorted(bugs.items(), key=lambda kv: -kv[1]["count"]), 1):
        lines.append(f"[{n}] {key}")
        lines.append(f"    summary : {b['summary']}")
        lines.append(f"    seen in : {b['count']} compile(s) across {len(b['files'])} unit(s)")
        lines.append(f"    repro   : {b['repro'][1]} {b['repro'][0]}")
        lines.append("")
    if not bugs:
        lines.append("No findings.")
    report = "\n".join(lines)
    print(report)
    if args.report:
        Path(args.report).write_text(report + "\n")
    if args.list_hits_raw and raw:
        with open(args.list_hits_raw, "a") as f:
            f.write("\n".join(raw) + "\n")
    return 1 if bugs else 0


if __name__ == "__main__":
    sys.exit(main())
