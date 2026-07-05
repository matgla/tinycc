#!/usr/bin/env python3
"""
asan_sweep.py — corpus enumeration + sweep driver + dedup/report for the
tinycc ASAN/UBSan bug-hunting sweep (Phase BH, Track 1).

This is a *helper* invoked by scripts/asan_sweep.sh; the bash script remains the
entry point.  It exists because robust corpus enumeration (gcc-torture builtins
source expansion, shardable file lists), per-file compile invocation, sanitizer
signature detection and stack-frame dedup are far cleaner in Python than in bash.

The oracle is the sanitizer output printed by `armv8m-tcc` (built with
-fsanitize=address by default).  An ordinary "unsupported feature" compile error
(nonzero exit, no sanitizer line) is NOT a hit; only a real sanitizer report is.

Test/tooling only.  Does not modify production code or config.mak.
"""

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# Sanitizer signatures that mark a genuine hit.  We deliberately key on the
# sanitizer's own markers, NOT on the compiler exit code (a plain "unsupported
# feature" error also exits nonzero but prints none of these).
SANITIZER_RE = re.compile(
    r"(ERROR: AddressSanitizer"
    r"|ERROR: LeakSanitizer"
    r"|LeakSanitizer: detected memory leaks"
    r"|runtime error:"          # UBSan
    r"|SUMMARY: .*Sanitizer)"
)

# A SUMMARY line is the most human-readable one-liner for the report.
SUMMARY_RE = re.compile(r"SUMMARY: .*?Sanitizer:.*")
# UBSan runtime errors do not always emit a SUMMARY; capture the first one.
UBSAN_RE = re.compile(r".*runtime error:.*")

# Backtrace frame:  "    #3 0x... in <symbol> (...)"
FRAME_RE = re.compile(r"#\d+\s+0x[0-9a-f]+\s+in\s+(\S+)")

# Generic allocator / wrapper / runtime frames that are NOT the root cause and
# must be skipped when building a dedup key (otherwise every leak collapses into
# one bucket regardless of where it was actually allocated).
NOISE_FRAMES = {
    "malloc", "calloc", "realloc", "free", "reallocarray",
    "realloc.part.0", "malloc.part.0",
    "operator new", "operator new[]",
    "default_reallocator", "default_realloc",
    "tcc_malloc", "tcc_mallocz", "tcc_realloc", "tcc_realloc_debug",
    "tcc_malloc_debug", "tcc_mallocz_debug", "tcc_free", "tcc_strdup",
    "__interceptor_malloc", "__interceptor_calloc", "__interceptor_realloc",
    "__libc_start_main", "__libc_start_call_main", "_start", "main",
    "__asan_memcpy", "__asan_memset", "__asan_memmove",
    "__sanitizer_print_stack_trace",
}


def _is_noise(sym):
    if sym in NOISE_FRAMES:
        return True
    # libasan internal frames have no real symbol of interest.
    if sym.startswith("__asan_") or sym.startswith("__ubsan_") or sym.startswith("__lsan_"):
        return True
    if sym.startswith("__interceptor_"):
        return True
    return False


def meaningful_frames(stderr_text, k=3):
    """Return the first k meaningful (non-noise) backtrace symbols across the
    whole report, in order.  This is the dedup key — the same bug across many
    files collapses to a single entry."""
    frames = []
    for m in FRAME_RE.finditer(stderr_text):
        sym = m.group(1)
        if _is_noise(sym):
            continue
        frames.append(sym)
        if len(frames) >= k:
            break
    return frames


def summary_line(stderr_text):
    m = SUMMARY_RE.search(stderr_text)
    if m:
        return m.group(0).strip()
    m = UBSAN_RE.search(stderr_text)
    if m:
        return m.group(0).strip()[:200]
    # Fall back to the ERROR line.
    for line in stderr_text.splitlines():
        if "Sanitizer" in line and ("ERROR" in line or "WARNING" in line):
            return line.strip()
    return "Sanitizer report (no SUMMARY line)"


# --------------------------------------------------------------------------
# Corpus enumeration
# --------------------------------------------------------------------------

def _gcc_torture_root():
    return REPO / "tests/gcctestsuite/gcc-testsuite/gcc/testsuite/gcc.c-torture"


def expand_gcc_builtin_sources(source):
    """Mirror tests/ir_tests/run.py:expand_gcc_builtin_sources — a builtins/
    execute test needs its <name>-lib.c companion plus lib/main.c so the
    compile actually exercises the same multi-TU shape the real harness uses."""
    extra = []
    if source.name.endswith("-lib.c"):
        return extra
    parent = source.parent
    if parent.name != "builtins":
        return extra
    if parent.parent.name != "execute":
        return extra
    if parent.parent.parent.name != "gcc.c-torture":
        return extra
    lib_file = source.with_name(f"{source.stem}-lib.c")
    builtins_main = parent / "lib" / "main.c"
    for f in (lib_file, builtins_main):
        if f.exists():
            extra.append(f)
    return extra


def enumerate_corpus(corpus):
    """Return a list of (primary_source: Path, extra_sources: [Path]) work items."""
    items = []

    def add_gcc_torture():
        root = _gcc_torture_root()
        if not root.exists():
            print(f"warning: gcc-torture not found at {root} "
                  f"(run 'make download-gcc-tests')", file=sys.stderr)
            return
        execute = root / "execute"
        # Top-level + ieee + builtins, recursively; skip -lib.c companions and
        # files inside lib/ (they are pulled in as extra sources, not compiled
        # standalone).
        for c in sorted(execute.rglob("*.c")):
            if c.name.endswith("-lib.c"):
                continue
            if c.parent.name == "lib":
                continue
            items.append((c, expand_gcc_builtin_sources(c)))
        compile_dir = root / "compile"
        if compile_dir.exists():
            for c in sorted(compile_dir.glob("*.c")):
                items.append((c, []))

    if corpus in ("gcc-torture", "all"):
        add_gcc_torture()
    if corpus in ("tests2", "all"):
        for c in sorted((REPO / "tests/tests2").glob("*.c")):
            items.append((c, []))
    if corpus in ("ir_tests", "all"):
        for c in sorted((REPO / "tests/ir_tests").glob("*.c")):
            items.append((c, []))

    return items


def apply_shard_limit(items, shard, limit):
    if shard:
        i, n = shard
        items = [it for idx, it in enumerate(items) if idx % n == (i - 1)]
    if limit:
        items = items[:limit]
    return items


# --------------------------------------------------------------------------
# Compile
# --------------------------------------------------------------------------

def build_compile_cmd(compiler, include_flags, abi_flags, opt, sources):
    cmd = [str(compiler), f"-B{REPO}"]
    cmd += abi_flags
    cmd += include_flags
    cmd += [opt, "-c"]
    cmd += [str(s) for s in sources]
    cmd += ["-o", "/dev/null"]
    return cmd


def run_one(compiler, include_flags, abi_flags, opt, primary, extras, timeout):
    sources = [primary] + list(extras)
    cmd = build_compile_cmd(compiler, include_flags, abi_flags, opt, sources)
    try:
        proc = subprocess.run(
            cmd,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            timeout=timeout,
        )
        stderr = proc.stderr.decode("utf-8", errors="replace")
        rc = proc.returncode
    except subprocess.TimeoutExpired as e:
        stderr = (e.stderr or b"").decode("utf-8", errors="replace")
        rc = -1
    return rc, stderr


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--compiler", default=str(REPO / "armv8m-tcc"),
                    help="path to the cross compiler (ASAN-built armv8m-tcc)")
    ap.add_argument("--corpus", default="all",
                    choices=["gcc-torture", "tests2", "ir_tests", "all"])
    ap.add_argument("--olevels", default="-O0,-O1,-O2",
                    help="comma-separated optimization levels")
    ap.add_argument("--shard", default=None,
                    help="i/N — sweep only shard i of N (1-based)")
    ap.add_argument("--limit", type=int, default=0,
                    help="cap number of files swept (after sharding)")
    ap.add_argument("--timeout", type=int, default=60,
                    help="per-compile timeout in seconds")
    ap.add_argument("--include-flags", default="",
                    help="space-separated -I flags from the harness Makefile")
    ap.add_argument("--abi-flags", default="",
                    help="space-separated ABI/codegen flags from the Makefile")
    ap.add_argument("--report", default=None,
                    help="write the deduped report here (also printed to stdout)")
    ap.add_argument("--list-hits-raw", default=None,
                    help="append every raw hit line (file|olevel|key) here")
    ap.add_argument("--progress-every", type=int, default=100)
    args = ap.parse_args()

    shard = None
    if args.shard:
        i, n = args.shard.split("/")
        shard = (int(i), int(n))
        if not (1 <= shard[0] <= shard[1]):
            print(f"error: bad shard {args.shard}", file=sys.stderr)
            return 2

    olevels = [o.strip() for o in args.olevels.split(",") if o.strip()]
    include_flags = args.include_flags.split()
    abi_flags = args.abi_flags.split()

    items = enumerate_corpus(args.corpus)
    total_files = len(items)
    items = apply_shard_limit(items, shard, args.limit)

    compiler = Path(args.compiler)
    if not compiler.exists():
        print(f"error: compiler not found: {compiler}", file=sys.stderr)
        return 2

    # bug_key -> dict(summary, key_frames, count, repros=[(file, olevel)])
    bugs = {}
    swept = 0
    hit_compiles = 0
    raw_hits = []

    for idx, (primary, extras) in enumerate(items):
        for opt in olevels:
            swept += 1
            rc, stderr = run_one(compiler, include_flags, abi_flags,
                                 opt, primary, extras, args.timeout)
            if not SANITIZER_RE.search(stderr):
                continue
            hit_compiles += 1
            frames = meaningful_frames(stderr, k=3)
            key = " <- ".join(frames) if frames else "(no meaningful frames)"
            summ = summary_line(stderr)
            rel = os.path.relpath(primary, REPO)
            raw_hits.append(f"{rel}|{opt}|{key}")
            b = bugs.setdefault(key, {
                "summary": summ,
                "frames": frames,
                "count": 0,
                "repro": None,
                "files": set(),
            })
            b["count"] += 1
            b["files"].add(rel)
            if b["repro"] is None:
                b["repro"] = (rel, opt)
            # Prefer the most informative summary if a later one is richer.
            if summ and len(summ) > len(b["summary"]):
                b["summary"] = summ
        if args.progress_every and (idx + 1) % args.progress_every == 0:
            print(f"  ... {idx + 1}/{len(items)} files, "
                  f"{len(bugs)} unique bug(s)", file=sys.stderr)

    # ---- report ----
    lines = []
    lines.append("=" * 78)
    lines.append("ASAN/UBSan sweep report")
    lines.append("=" * 78)
    lines.append(f"corpus            : {args.corpus}")
    lines.append(f"olevels           : {','.join(olevels)}")
    if shard:
        lines.append(f"shard             : {shard[0]}/{shard[1]}")
    if args.limit:
        lines.append(f"limit             : {args.limit}")
    lines.append(f"files in corpus   : {total_files}")
    lines.append(f"files this run    : {len(items)}")
    lines.append(f"compiles run      : {swept}")
    lines.append(f"sanitizer hits    : {hit_compiles} compile(s)")
    lines.append(f"unique bugs       : {len(bugs)}")
    lines.append("")

    if bugs:
        # Sort by count descending so the most-frequent bug is first.
        for n, (key, b) in enumerate(
                sorted(bugs.items(), key=lambda kv: -kv[1]["count"]), 1):
            repro_file, repro_opt = b["repro"]
            lines.append(f"[BUG {n}] {key}")
            lines.append(f"    summary : {b['summary']}")
            lines.append(f"    seen in : {b['count']} compile(s) "
                         f"across {len(b['files'])} file(s)")
            lines.append(f"    repro   : {repro_file} {repro_opt}")
            lines.append("")
    else:
        lines.append("No sanitizer hits in this slice.")
        lines.append("")

    report = "\n".join(lines)
    print(report)

    if args.report:
        Path(args.report).write_text(report)
    if args.list_hits_raw and raw_hits:
        with open(args.list_hits_raw, "a") as f:
            for h in raw_hits:
                f.write(h + "\n")

    return 0


if __name__ == "__main__":
    sys.exit(main())
