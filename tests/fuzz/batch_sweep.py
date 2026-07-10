#!/usr/bin/env python3
"""batch_sweep.py — find O-level-divergent fuzz seeds with FEW qemu boots.

The differential sweep used to do one qemu run per (seed, O-level) — 4N boots
for N seeds.  qemu's ~100 ms boot/startup is ~93% of a run's cost (the tcc
compile+link is ~7 ms), so those boots, not the compiles, dominate the wall
clock.

This tool compiles a *batch* of B seeds into a single ELF per O-level and runs
each ELF once.  Every generated program keeps all helpers ``static`` and exposes
only ``main``; we rename each seed's ``main`` (``-Dmain=seed_<S>``) and emit a
runner whose real ``main`` prints ``S<seed>`` then calls each seed (which prints
``checksum=<hex>``).  One boot therefore yields B results.  qemu boots drop from
4N to ~4*ceil(N/B) — a ~200x cut at B=250.

A HardFault (the board's handler does SYS_EXIT) or a Lockup (timeout) cuts a
batch at the offending seed; that seed is re-run on its own to classify it
(HardFault/Lockup), and the tail of the batch is re-batched.  COMPILE_FAIL seeds
are detected at compile time and excluded from their batch.

!! RECALL CAVEAT — this is a FAST PRE-SCAN, not a replacement for the exhaustive
   per-seed sweep (triage_olevels.sh).  A seed compiled here is byte-identical to
   the standalone build (compiled as `main`, then the symbol is renamed in the
   .o with objcopy), but it runs ONE CALL FRAME DEEP inside a runner instead of
   as the crt0 entry.  A class of miscompiles — tcc eliding a store so the code
   reads UNINITIALISED stack/registers — only diverges under crt0's exact entry
   state (the real __StackTop and crt0's low-stack leftovers).  Running after a
   prior seed's printf/activity perturbs that state, so such bugs compute the
   *correct* value here and are NOT flagged (false negatives, ~1 in 5 in
   sampling; e.g. seed 8300).  Poisoning the stack/registers does not help — it
   masks them, not exposes them.  Context-INSENSITIVE miscompiles (const-prop,
   dead-store, loop-unroll, jump-threading, COMPILE_CRASH) ARE caught.
   => Use for rapid iteration; run the full sweep before certifying a range clean.

GCC REFERENCE LEVEL — pass a level like ``gcc-O2`` in ``--olevels`` (e.g.
``--olevels -O0,-O1,-O2,-Os,gcc-O2``) to compile that seed with
``arm-none-eabi-gcc`` instead of ``armv8m-tcc`` and link its object into the
SAME batched runner ELF, at no extra qemu-boot cost.  A seed then counts as
divergent either because the tcc O-levels disagree with each other
(self-consistency) OR because they all agree with each other but disagree with
the gcc reference (the O0-WRONG class self-consistency alone can't see) — one
merged pass finds both, replacing a separate per-seed vs-gcc differential.
When a gcc-* level is requested, divergent seeds print as tagged lines
(``OLEVELS <seeds...>`` / ``VSGCC <seeds...>`` / ``GCCBAD <seeds...>``) instead
of the plain one-per-line list — the plain list stays the contract when no gcc-*
level is present, so existing callers (triage_olevels.sh's FAST_SWEEP) are
unaffected.
The same recall caveat above applies to the gcc side too: a batched run misses
context-sensitive divergences a standalone crt0-entry run would catch.

ORACLE SELF-CONSISTENCY — gcc is not infallible: it miscompiles some UB-free
programs at -O2 (bitfield seed 1486 is a confirmed case — gcc -O2 alone disagrees
with gcc -O0/-O1, clang, tcc, and an exact reference model).  Pass TWO gcc levels
(``gcc-O0,gcc-O2``) and a seed where they DISAGREE WITH EACH OTHER is reported as
``GCCBAD`` (oracle-unreliable, quarantined) rather than blamed on tcc; only when
the gcc levels agree can gcc-vs-tcc count as ``VSGCC``.  With a single gcc level
there is nothing to cross-check, so this guard is inert (back-compatible).

SPEED — with the run phase batched, the wall clock is dominated by process
spawning and the compile phase, so both are batched too:
  * generation uses gen_c.py's --count/--out-dir mode (one interpreter per
    shard, not one ~50ms python startup per seed);
  * compiles+objcopy run as per-chunk shell scripts (~100 per `sh`) — the exact
    same per-file command lines (objects stay byte-identical to a standalone
    build), but ~2 orders of magnitude fewer processes spawned from Python,
    which serializes spawns on the GIL at ~550/s no matter how many --jobs;
  * generated sources and gcc-* reference objects are cached persistently in
    tests/fuzz/.sweep_cache/, keyed on (gen_c.py content hash, profile, gcc
    version) — a re-sweep after a tcc fix regenerates nothing and recompiles
    only the tcc levels.  `--no-cache` bypasses it; `rm -rf` the directory to
    reclaim space (it is safe to delete at any time).  NOTE the key does NOT
    cover the libc headers in tests/ir_tests/libc_includes — wipe the cache if
    you change those.

Usage:
    batch_sweep.py [LO] [HI] [--batch B] [--jobs J] [--olevels -O0,-O1,-O2,-Os]
    batch_sweep.py --seeds 588,860,1005          # explicit list
    batch_sweep.py 0 4999 --olevels=-O0,-O1,-O2,-Os,gcc-O0,gcc-O2  # + gcc reference w/ self-consistency (note the `=`: a leading `-O0` after a bare space looks like a new option to argparse)

Prints the divergent seeds (one per line) on stdout — drop-in for the seed
enumeration in triage_olevels.sh.  Progress/stats go to stderr.
"""
from __future__ import annotations

import argparse
import atexit
import concurrent.futures as cf
import hashlib
import itertools
import os
import queue
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]          # libs/tinycc
TCC = ROOT / "armv8m-tcc"
MPS = ROOT / "tests" / "ir_tests" / "qemu" / "mps2-an505"
NL = MPS / "newlib_build"
GENC = ROOT / "tests" / "fuzz" / "gen_c.py"

INC = [
    f"-I{ROOT}/tests/ir_tests/libc_includes",
    f"-I{ROOT}/tests/ir_tests/libc_imports",
    f"-I{ROOT}/tests/ir_tests/libc_includes/newlib",
    "-I/include",
    f"-I{ROOT}/include",
]
ARMCC = ["arm-none-eabi-gcc", "-mcpu=cortex-m33", "-mthumb", "-mfloat-abi=soft"]
CF_COMMON = ["-nostdlib", "-fvisibility=hidden", "-mcpu=cortex-m33", "-mthumb",
             "-mfloat-abi=soft", "-ffunction-sections"]

_SIG_RE = re.compile(r"checksum=([0-9a-f]+)|HardFault|Lockup")
_PROGRESS_LOCK = threading.Lock()


def _run(cmd, **kw):
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          text=True, **kw)


def _elapsed(start: float) -> str:
    secs = int(time.monotonic() - start)
    mins, secs = divmod(secs, 60)
    hours, mins = divmod(mins, 60)
    if hours:
        return f"{hours:d}h{mins:02d}m{secs:02d}s"
    if mins:
        return f"{mins:d}m{secs:02d}s"
    return f"{secs:d}s"


def progress(msg: str) -> None:
    with _PROGRESS_LOCK:
        print(msg, file=sys.stderr, flush=True)


def discover_toolchain() -> dict:
    """Resolve the multilib objects/libs exactly as runseed.sh does."""
    def pf(flag):
        return _run([*ARMCC, flag]).stdout.strip()

    def find(*globs):
        for g in globs:
            hits = sorted(NL.rglob(g))
            if hits:
                return str(hits[0])
        return None

    tc = {
        "crti": pf("-print-file-name=crti.o"),
        "crtn": pf("-print-file-name=crtn.o"),
        "crtend": pf("-print-file-name=crtend.o"),
        "libgcc": pf("-print-libgcc-file-name"),
        "rdimon_crt0": find("rdimon-crt0.o"),
        "librdimon": find("librdimon.a"),
        "libc": find("*newlib*/libc.a", "libc.a"),
        "libm": find("*newlib*/libm.a", "libm.a"),
    }
    missing = [k for k, v in tc.items() if not v]
    if missing:
        sys.exit(f"toolchain pieces not found: {missing} (build newlib / make cross)")
    return tc


def compile_boot(wd: Path, tc) -> Path:
    """Assemble the board boot object once (identical for every program).

    boot.S carries .isr_vector (initial SP + reset vector), Reset_Handler and
    the fault handlers — every ELF MUST link it or the CPU boots into garbage."""
    boot_o = wd / "boot.o"
    rc = _run([str(TCC), *CF_COMMON, "-O0", *INC, "-c",
               str(MPS / "boot.S"), "-o", str(boot_o)])
    if not boot_o.exists():
        sys.exit("failed to assemble boot.S:\n" + rc.stderr)
    tc["boot_obj"] = str(boot_o)
    return boot_o


def link_cmd(objs, out_elf, tc) -> list:
    return [str(TCC), *CF_COMMON, *INC, *map(str, objs), tc["boot_obj"],
            tc["crti"], tc["rdimon_crt0"], tc["crtend"], tc["crtn"],
            "-o", str(out_elf),
            "-Wl,--gc-sections", f"-B{ROOT}",
            f"-L{ROOT}/lib", f"-L{ROOT}/lib/fp", f"-L{ROOT}",
            "-Wl,--start-group", "-larmv8m-libtcc1.a", "-lsoftfp",
            tc["libc"], tc["librdimon"], tc["libm"], tc["libgcc"],
            "-Wl,--end-group", "-Wl,-oformat=elf32-littlearm",
            "-T", str(MPS / "linker_script.ld")]


# ---------------------------------------------------------------------------
# generation + compilation (per seed, per O-level)
# ---------------------------------------------------------------------------
# Generator feature profile (Axis 2 of docs/plan_fuzz_reach_expansion.md); set
# from --profile / FUZZ_PROFILE.  "int" (default) = historical byte-identical stream.
_PROFILE_ALIASES = {"integer": "int"}


def normalize_profile(profile: str) -> str:
    return _PROFILE_ALIASES.get(profile, profile)


GEN_PROFILE = normalize_profile(os.environ.get("FUZZ_PROFILE", "int"))


def gen_seed(seed: int, wd: Path) -> Path | None:
    """Per-seed fallback path (and straggler retry for the batched generator)."""
    src = wd / f"fuzz_{seed}.c"
    rc = _run(["python3", str(GENC), "--seed", str(seed),
               "--profile", GEN_PROFILE, "-o", str(src)])
    return src if rc.returncode == 0 and src.exists() else None


def _contiguous_runs(seeds: list[int]) -> list[tuple[int, int]]:
    """Collapse a sorted seed list into (start, count) runs so gen_c.py --count
    (which only takes contiguous ranges) can cover an arbitrary --seeds list."""
    runs: list[tuple[int, int]] = []
    for s in seeds:
        if runs and s == runs[-1][0] + runs[-1][1]:
            runs[-1] = (runs[-1][0], runs[-1][1] + 1)
        else:
            runs.append((s, 1))
    return runs


def _cache_fetch(cache: Path | None, name: str, dst: Path) -> bool:
    if cache is None or not (cache / name).exists():
        return False
    shutil.copyfile(cache / name, dst)
    return True


def _cache_store(cache: Path | None, src: Path) -> None:
    """Publish `src` into the cache atomically (concurrent sweeps may race)."""
    if cache is None or (cache / src.name).exists():
        return
    tmp = cache / f"{src.name}.tmp{os.getpid()}"
    shutil.copyfile(src, tmp)
    os.replace(tmp, cache / src.name)


def generate_sources(seeds: list[int], wd: Path, jobs: int,
                     progress_every: int, src_cache: Path | None) -> dict[int, Path]:
    """Generate every seed's source as wd/fuzz_<S>.c, batched: one gen_c.py
    --count call per shard instead of one interpreter start per seed (the
    ~50ms python startup dominates the ~10ms generation itself).  Seeds found
    in `src_cache` are copied in and skipped; fresh ones are published back.
    Returns {seed: src_path}; a seed missing from the map failed to generate."""
    started = time.monotonic()
    srcs: dict[int, Path] = {}
    todo: list[int] = []
    for s in seeds:
        dst = wd / f"fuzz_{s}.c"
        if _cache_fetch(src_cache, dst.name, dst):
            srcs[s] = dst
        else:
            todo.append(s)
    if srcs:
        progress(f"  generated {len(srcs)}/{len(seeds)} seeds from cache ({_elapsed(started)})")
    shard = max(1, min(500, -(-len(todo) // max(1, jobs))))
    calls: list[tuple[int, int]] = []
    for start, count in _contiguous_runs(todo):
        for off in range(0, count, shard):
            calls.append((start + off, min(shard, count - off)))

    def _gen(call: tuple[int, int]) -> int:
        start, count = call
        _run(["python3", str(GENC), "--seed", str(start), "--count", str(count),
              "--profile", GEN_PROFILE, "--out-dir", str(wd)])
        return count

    done = len(srcs)
    last = done
    with cf.ThreadPoolExecutor(max_workers=jobs) as ex:
        for count in ex.map(_gen, calls):
            done += count
            if (progress_every and done - last >= progress_every) or done == len(seeds):
                last = done
                progress(f"  generated {done}/{len(seeds)} seeds ({_elapsed(started)})")
    for s in todo:
        src = wd / f"fuzz_{s}.c"
        if not src.exists() and gen_seed(s, wd) is None:   # straggler retry
            continue
        srcs[s] = src
        _cache_store(src_cache, src)
    return srcs


OBJCOPY = "arm-none-eabi-objcopy"
GCC_BIN = "arm-none-eabi-gcc"


def is_gcc_level(olevel: str) -> bool:
    return olevel.startswith("gcc-")


def compile_seed(seed: int, src: Path, olevel: str, wd: Path):
    """Compile one seed at one O-level, then rename main -> seed_<S> in the .o.

    The compile uses the real `main` (NOT -Dmain=...), so the emitted code is
    byte-identical to the standalone build — `main` may be special-cased by the
    optimizer, and we must not perturb codegen.  The symbol is renamed only
    afterwards, in the object file, so the runner can call it.

    A `gcc-<flag>` olevel (e.g. `gcc-O2`) compiles with `arm-none-eabi-gcc
    <flag>` instead of `armv8m-tcc <olevel>` -- the SAME CF_COMMON/INC flags
    apply, since gcc and tcc target the same AAPCS ABI.  The resulting object
    links into the same batched runner ELF as any tcc-compiled level (the tcc
    link driver already links gcc-toolchain objects -- crti/crtn/libgcc/newlib
    -- for every run, so this is not a new capability).

    Returns (obj_path, None) on success, or (None, "COMPILE_FAIL") on a real
    compiler error.  Transient infra errors are retried.
    """
    obj = wd / f"seed{seed}{olevel}.o"
    compiler = GCC_BIN if is_gcc_level(olevel) else str(TCC)
    opt = olevel[len("gcc"):] if is_gcc_level(olevel) else olevel
    cmd = [compiler, *CF_COMMON, opt, *INC, "-c", str(src), "-o", str(obj)]
    for _ in range(3):
        rc = _run(cmd)
        if obj.exists():
            ro = _run([OBJCOPY, "--redefine-sym", f"main=seed_{seed}", str(obj)])
            if ro.returncode != 0:
                return None, "COMPILE_FAIL"
            return obj, None
        blob = rc.stderr + rc.stdout
        if re.search(r"error:|compiler_error|assert|signal|Sanitizer", blob, re.I):
            return None, "COMPILE_FAIL"
        # else transient (full /tmp, killed child) — retry
    return None, "COMPILE_FAIL"


COMPILE_CHUNK = 100          # compile+objcopy pairs per spawned shell


def compile_objects(seeds: list[int], srcs: dict[int, Path], olevels: list[str],
                    wd: Path, jobs: int, progress_every: int,
                    gcc_cache: Path | None) -> tuple[dict, dict]:
    """Compile every (seed, O-level) object, chunked: ~COMPILE_CHUNK
    compile-and-rename pairs run inside ONE spawned `sh` script per work item.
    The per-file command lines are IDENTICAL to compile_seed's (same absolute
    src path, same -o), so the objects are byte-for-byte what the per-seed path
    produces — only the process count changes (Python serializes subprocess
    spawns on the GIL at ~550/s, which throttled the old one-spawn-per-object
    pool far below --jobs).  Each pair compiles to a .tmp, renames main inside
    it, and only then mv's to the final name, so "final .o exists" is a
    trustworthy per-seed success test; missing ones fall back to compile_seed
    (which retries transients and classifies COMPILE_FAIL).

    gcc-* objects found in `gcc_cache` (already main-renamed) are copied in and
    skipped; freshly built ones are published back — gcc never changes when tcc
    is being fixed, so re-sweeps skip 2 of the 6 levels entirely.

    Returns (objs, fails): objs maps olevel -> {seed: obj_path}; fails maps
    (seed, olevel) -> "COMPILE_FAIL"."""
    objs: dict[str, dict[int, Path]] = {o: {} for o in olevels}
    fails: dict[tuple[int, str], str] = {}
    started = time.monotonic()
    n_total = 0
    n_cached = 0
    chunks: list[tuple[str, list[int]]] = []
    for o in olevels:
        todo: list[int] = []
        for s in seeds:
            if s not in srcs:
                continue
            n_total += 1
            obj = wd / f"seed{s}{o}.o"
            if is_gcc_level(o) and _cache_fetch(gcc_cache, obj.name, obj):
                objs[o][s] = obj
                n_cached += 1
                continue
            todo.append(s)
        for i in range(0, len(todo), COMPILE_CHUNK):
            chunks.append((o, todo[i:i + COMPILE_CHUNK]))
    # gcc chunks are ~5x slower per file than tcc ones — schedule them first so
    # the slow tail doesn't run alone at the end.
    chunks.sort(key=lambda c: not is_gcc_level(c[0]))
    if n_cached:
        progress(f"  compiled {n_cached}/{n_total} objects from cache ({_elapsed(started)})")

    def _compile_chunk(item: tuple[int, tuple[str, list[int]]]) -> tuple[str, list[int]]:
        idx, (o, chunk) = item
        compiler = GCC_BIN if is_gcc_level(o) else str(TCC)
        opt = o[len("gcc"):] if is_gcc_level(o) else o
        lines = []
        for s in chunk:
            obj = wd / f"seed{s}{o}.o"
            tmp = wd / f"seed{s}{o}.o.tmp"
            lines.append(
                shlex.join([compiler, *CF_COMMON, opt, *INC, "-c", str(srcs[s]), "-o", str(tmp)])
                + " && "
                + shlex.join([OBJCOPY, "--redefine-sym", f"main=seed_{s}", str(tmp)])
                + " && " + shlex.join(["mv", str(tmp), str(obj)]))
        script = wd / f"cc_{o}_{idx}.sh"
        script.write_text("\n".join(lines) + "\nexit 0\n")
        _run(["sh", str(script)])
        return o, chunk

    done = n_cached
    last = done
    with cf.ThreadPoolExecutor(max_workers=jobs) as ex:
        for o, chunk in ex.map(_compile_chunk, enumerate(chunks)):
            for s in chunk:
                obj = wd / f"seed{s}{o}.o"
                if not obj.exists():                 # per-seed fallback / classify
                    obj, err = compile_seed(s, srcs[s], o, wd)
                if obj is not None:
                    objs[o][s] = obj
                    if is_gcc_level(o):
                        _cache_store(gcc_cache, obj)
                else:
                    fails[(s, o)] = err
            done += len(chunk)
            if (progress_every and done - last >= progress_every) or done == n_total:
                last = done
                progress(f"  compiled {done}/{n_total} objects ({_elapsed(started)})")
    return objs, fails


# ---------------------------------------------------------------------------
# batching: build a runner over a list of seeds, link, run, parse
# ---------------------------------------------------------------------------
def build_runner_obj(seeds: list[int], olevel: str, wd: Path, tc, uid) -> Path:
    """A runner whose main calls each seed_<S> in order, bracketed by markers.

    ``uid`` makes the emitted .c/.o names unique per work item — required now
    that batches run concurrently across --jobs workers (a chunk that overflows
    FLASH splits into halves that keep the same first-seed, so first-seed+len is
    no longer a collision-free key)."""
    decls = "".join(f"extern int seed_{s}(void);\n" for s in seeds)
    calls = "".join(
        f'  printf("S{s}\\n"); seed_{s}();\n' for s in seeds)
    runner_c = wd / f"runner_{olevel}_{uid}.c"
    runner_c.write_text(
        "#include <stdio.h>\n"
        f"{decls}"
        "int main(void){\n"
        "  setvbuf(stdout, 0, _IONBF, 0);\n"
        f"{calls}"
        '  printf("DONE\\n");\n'
        "  return 0;\n"
        "}\n")
    obj = wd / f"{runner_c.stem}.o"
    # runner is trivial — compile at -O0 (no -Dmain: it owns the real main).
    rc = _run([str(TCC), *CF_COMMON, "-O0", *INC, "-c", str(runner_c), "-o", str(obj)])
    if not obj.exists():
        raise RuntimeError(f"runner compile failed:\n{rc.stderr}")
    return obj


def run_elf(elf: Path, timeout: float) -> tuple[str, bool]:
    """Run one ELF under qemu; return (stdout, timed_out).  Partial output on
    timeout is preserved by capturing to a file."""
    out = elf.with_suffix(".out")
    with open(out, "w") as fh:
        # stdin MUST be detached from the controlling tty: `-nographic` muxes the
        # serial+monitor onto stdio and puts a tty stdin into RAW mode (echo off).
        # On the timeout path below we SIGKILL qemu, so it never restores termios
        # — that leaves the user's terminal silent/broken (and with parallel
        # workers it's near-certain on any slow seed).  /dev/null is not a tty, so
        # qemu leaves the terminal alone.  The guest never reads stdin anyway.
        p = subprocess.Popen(
            ["qemu-system-arm", "-machine", "mps2-an505", "-nographic",
             "-semihosting", "-kernel", str(elf)],
            stdin=subprocess.DEVNULL,
            stdout=fh, stderr=subprocess.STDOUT)
        try:
            p.wait(timeout=timeout)
            timed_out = False
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
            timed_out = True
    return out.read_text(errors="replace"), timed_out


def parse_batch(text: str, timed_out: bool) -> tuple[dict, int | None, str | None]:
    """Pair S<seed> markers with the following checksum line.

    Returns (results, crashed_seed, crash_kind).  results maps seed->signature
    ('<hex>'|'HardFault'|'Lockup') for every seed that produced one.  If the run
    was cut, crashed_seed is the in-progress seed and crash_kind its signature.
    """
    results: dict[int, str] = {}
    cur: int | None = None
    saw_hardfault = False
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("S") and line[1:].isdigit():
            cur = int(line[1:])
        elif line.startswith("checksum=") and cur is not None:
            results[cur] = line[len("checksum="):]
            cur = None
        elif line.startswith("HardFault"):
            saw_hardfault = True
    # `cur` left set means its S printed but no checksum followed -> it crashed.
    crashed, kind = None, None
    if cur is not None:
        crashed = cur
        kind = "HardFault" if saw_hardfault else ("Lockup" if timed_out else None)
    return results, crashed, kind


def _process_chunk(olevel: str, chunk: list[int], objs: dict, wd: Path, tc,
                   timeout_base: float, uid) -> tuple[dict, list]:
    """Build, link, and run ONE batched ELF for `chunk` at `olevel`.

    `objs` maps olevel -> {seed: compiled .o}.  Returns (results, requeue):
    `results` maps the seeds this batch resolved to their signatures; `requeue`
    is a list of (olevel, subchunk) work items to push back — the two halves of
    a chunk that overflowed FLASH (the mps2 image budget is 512K, so batch size
    self-tunes to whatever fits), or the tail of a chunk cut short by a crash."""
    runner = build_runner_obj(chunk, olevel, wd, tc, uid)
    elf = wd / f"batch_{olevel}_{uid}.elf"
    _run(link_cmd([objs[olevel][s] for s in chunk] + [runner], elf, tc))
    if not elf.exists():
        if len(chunk) > 1:                       # likely FLASH overflow -> split
            mid = len(chunk) // 2
            return {}, [(olevel, chunk[:mid]), (olevel, chunk[mid:])]
        # a single seed that won't link
        return {chunk[0]: classify_one(chunk[0], olevel, wd, tc, timeout_base)}, []
    # runtime grows with batch size; give each program ~80ms headroom.
    text, timed_out = run_elf(elf, timeout_base + 0.08 * len(chunk))
    res, crashed, kind = parse_batch(text, timed_out)
    results = dict(res)
    if crashed is not None:
        results[crashed] = kind or classify_one(crashed, olevel, wd, tc, timeout_base)
        done = set(res) | {crashed}
        tail = [s for s in chunk if s not in done]   # post-crash tail
        return results, ([(olevel, tail)] if tail else [])
    # clean DONE (or a cut we couldn't attribute) — classify any stragglers
    for s in chunk:
        if s not in results:
            results[s] = classify_one(s, olevel, wd, tc, timeout_base)
    return results, []


def run_batches(seeds: list[int], olevels: list[str], objs: dict, wd: Path, tc,
                batch: int, timeout_base: float, jobs: int,
                progress_every: int = 0) -> dict:
    """Run every (seed, O-level) via batched ELFs across `jobs` qemu workers.

    All O-levels share ONE pool of `jobs` workers.  (Previously each O-level got
    its own thread and processed its chunks serially, so qemu concurrency was
    hard-capped at len(olevels) — usually 4 — no matter how large --jobs was;
    this makes the run phase scale with --jobs like generate/compile do.)

    Work items are (olevel, chunk) pairs on a shared queue; `_process_chunk` may
    push more items back (FLASH-overflow split halves, post-crash tail), so the
    queue grows dynamically.  Returns {olevel: {seed: signature}}."""
    results: dict[str, dict[int, str]] = {o: {} for o in olevels}
    q: queue.Queue = queue.Queue()
    n_expected = 0
    for o in olevels:
        avail = [s for s in seeds if s in objs[o]]
        n_expected += len(avail)
        for i in range(0, len(avail), batch):
            q.put((o, avail[i:i + batch]))

    lock = threading.Lock()
    uids = itertools.count()                     # next() is atomic under the GIL
    started = time.monotonic()
    state = {"done": 0, "last": -1}

    def maybe_report(force: bool = False) -> None:
        # caller holds `lock`
        done = state["done"]
        if force and done == state["last"]:
            return
        if not force and (not progress_every or done - state["last"] < progress_every):
            return
        state["last"] = done
        progress(f"  run: {done}/{n_expected} seeds "
                 f"({q.qsize()} batch(es) queued, {_elapsed(started)})")

    def worker() -> None:
        while True:
            item = q.get()
            if item is None:                     # sentinel: no more work
                q.task_done()
                return
            olevel, chunk = item
            try:
                res, requeue = _process_chunk(olevel, chunk, objs, wd, tc,
                                              timeout_base, next(uids))
                for it in requeue:               # push before task_done so q.join
                    q.put(it)                    # can't see the queue as drained
                with lock:
                    results[olevel].update(res)
                    state["done"] += len(res)
                    maybe_report()
            finally:
                q.task_done()

    with lock:
        maybe_report(force=True)
    threads = [threading.Thread(target=worker, daemon=True)
               for _ in range(max(1, jobs))]
    for t in threads:
        t.start()
    q.join()                                     # all real (non-sentinel) work done
    for _ in threads:
        q.put(None)
    for t in threads:
        t.join()
    with lock:
        maybe_report(force=True)
    return results


def classify_one(seed: int, olevel: str, wd: Path, tc, timeout_base: float) -> str:
    """Slow path: run a single seed in its own ELF and return its signature."""
    obj = wd / f"seed{seed}{olevel}.o"
    if not obj.exists():
        return "COMPILE_FAIL"
    runner = build_runner_obj([seed], olevel, wd, tc, f"solo{seed}")
    elf = wd / f"solo_{olevel}_{seed}.elf"
    if not _run(link_cmd([obj, runner], elf, tc)) or not elf.exists():
        return "COMPILE_FAIL"
    text, timed_out = run_elf(elf, timeout_base)
    res, crashed, kind = parse_batch(text, timed_out)
    if seed in res:
        return res[seed]
    if crashed == seed and kind:
        return kind
    return "Lockup" if timed_out else "COMPILE_FAIL"


# ---------------------------------------------------------------------------
# orchestration
# ---------------------------------------------------------------------------
def main(argv=None) -> int:
    global GEN_PROFILE
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("lo", nargs="?", type=int, default=0)
    ap.add_argument("hi", nargs="?", type=int, default=4999)
    ap.add_argument("--seeds", help="explicit comma/space seed list (overrides lo/hi)")
    ap.add_argument("--batch", type=int, default=200,
                    help="seeds per ELF (default 200; auto-halves on FLASH overflow)")
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 4) - 2))
    ap.add_argument("--olevels", default="-O0,-O1,-O2,-Os",
                    help="comma list of tcc -O flags; a `gcc-<flag>` entry "
                         "(e.g. gcc-O2) compiles that level with "
                         "arm-none-eabi-gcc instead, as a reference")
    ap.add_argument("--timeout", type=float, default=20.0, help="base qemu timeout (s)")
    ap.add_argument("--progress-every", type=int, default=500,
                    help="print progress every N completed items/seeds (0 disables)")
    ap.add_argument("--keep", action="store_true", help="keep the work dir")
    ap.add_argument("--no-cache", action="store_true",
                    help="bypass the persistent source/gcc-object cache "
                         "(tests/fuzz/.sweep_cache)")
    ap.add_argument("--profile", default=GEN_PROFILE,
                    help="generator feature profile (int/integer|float|...); default $FUZZ_PROFILE or int")
    args = ap.parse_args(argv)
    GEN_PROFILE = normalize_profile(args.profile)

    if not TCC.exists():
        sys.exit(f"no {TCC} — run 'make cross'")
    if shutil.which("qemu-system-arm") is None:
        sys.exit("qemu-system-arm not on PATH")

    olevels = [o.strip() for o in args.olevels.replace(",", " ").split()]
    if any(is_gcc_level(o) for o in olevels) and shutil.which(GCC_BIN) is None:
        sys.exit(f"{GCC_BIN} not on PATH (required for a gcc-* reference level)")
    if args.seeds:
        seeds = sorted({int(x) for x in args.seeds.replace(",", " ").split()})
    else:
        seeds = list(range(args.lo, args.hi + 1))

    tc = discover_toolchain()
    wd = Path(tempfile.mkdtemp(prefix="batchsweep_"))
    # Clean up the (often ~500MB) workdir on ANY exit, not just a normal return.
    # sweep_all.py drives us as a subprocess, so an interrupted band (Ctrl-C ->
    # SIGINT, or a SIGTERM from the parent) used to skip the tail rmtree and leak
    # the whole dir into /tmp -- enough interrupted bands filled tmpfs.  atexit
    # fires on normal exit, unhandled exceptions and sys.exit; the SIGTERM handler
    # routes that signal through sys.exit so cleanup fires for it too.
    if not args.keep:
        atexit.register(shutil.rmtree, wd, ignore_errors=True)
        signal.signal(signal.SIGTERM, lambda *_: sys.exit(1))
    compile_boot(wd, tc)

    # Persistent cache: sources depend only on (gen_c.py, profile, seed); gcc-*
    # reference objects additionally on the gcc version — none of which change
    # while tcc is being fixed, so re-sweeps of a band skip both entirely.
    src_cache = gcc_cache = None
    if not args.no_cache:
        gen_key = hashlib.sha256(GENC.read_bytes()).hexdigest()[:16]
        base = ROOT / "tests" / "fuzz" / ".sweep_cache"
        src_cache = base / f"src-{gen_key}-{GEN_PROFILE}"
        src_cache.mkdir(parents=True, exist_ok=True)
        if any(is_gcc_level(o) for o in olevels):
            gcc_ver = _run([GCC_BIN, "--version"]).stdout.splitlines()[0]
            gcc_key = hashlib.sha256(f"{gen_key} {gcc_ver}".encode()).hexdigest()[:16]
            gcc_cache = base / f"gccobj-{gcc_key}-{GEN_PROFILE}"
            gcc_cache.mkdir(parents=True, exist_ok=True)

    progress(f"batch sweep: {len(seeds)} seeds x {len(olevels)} O-levels, "
             f"batch={args.batch}, jobs={args.jobs}"
             f"{', cache off' if args.no_cache else ''}\n  workdir {wd}")

    # signatures[seed][olevel] = '<hex>'|HardFault|Lockup|COMPILE_FAIL
    signatures: dict[int, dict[str, str]] = {s: {} for s in seeds}

    # 1) generate sources (batched gen_c.py --count shards + cache).
    srcs = generate_sources(seeds, wd, args.jobs, args.progress_every, src_cache)
    for s in seeds:
        if s not in srcs:
            for o in olevels:
                signatures[s][o] = "COMPILE_FAIL"

    # 2) compile every (seed, O-level) object (chunked shells + gcc cache).
    objs, comp_fails = compile_objects(seeds, srcs, olevels, wd,
                                       args.jobs, args.progress_every, gcc_cache)
    for (s, o), err in comp_fails.items():
        signatures[s][o] = err

    # 3) batched run across ALL (O-level, batch) pairs over `jobs` qemu workers.
    #    (Was one thread per O-level, so qemu concurrency capped at len(olevels);
    #    now the run phase scales with --jobs like generate/compile above.)
    run_results = run_batches(seeds, olevels, objs, wd, tc,
                              args.batch, args.timeout, args.jobs, args.progress_every)
    for o in olevels:
        for s, sig in run_results[o].items():
            signatures[s][o] = sig
        progress(f"  {o} done")

    # 4) classify divergences.  With no gcc-* level requested this reproduces the
    #    old olevels-only self-consistency test: any two tcc O-levels disagreeing
    #    (matches the old sweep's val()-equality test).  With a gcc-* level
    #    present, a seed additionally counts as vs-gcc-divergent if all tcc
    #    O-levels agree WITH EACH OTHER but not with the gcc reference -- the
    #    O0-WRONG class self-consistency alone can't see -- so one merged batch
    #    can replace a separate per-seed vs-gcc differential for the caller.
    #
    #    ORACLE SELF-CONSISTENCY: gcc is not an infallible oracle -- it miscompiles
    #    some UB-free programs at -O2 (e.g. bitfield seed 1486: gcc -O2 alone
    #    disagrees with gcc -O0/-O1, clang, tcc, and an exact reference model).  A
    #    single gcc level can't tell "tcc is wrong" from "gcc is wrong", so pass
    #    TWO gcc levels (e.g. gcc-O0,gcc-O2): if they disagree WITH EACH OTHER the
    #    seed is oracle-unreliable and quarantined (GCCBAD) instead of being blamed
    #    on tcc; only when the gcc levels agree does gcc-vs-tcc count as vsgcc.
    tcc_levels = [o for o in olevels if not is_gcc_level(o)]
    gcc_levels = [o for o in olevels if is_gcc_level(o)]

    olevels_bad, vsgcc_bad, gcc_inconsistent = [], [], []
    for s in seeds:
        tvals = [signatures[s].get(o, "?") for o in tcc_levels]
        if len(set(tvals)) > 1:
            olevels_bad.append(s)
        elif gcc_levels:
            # Distinct gcc outputs that actually built ("?" = gcc failed for this
            # seed at that level -- an infra/build gap, not an oracle signal).
            gvals = {signatures[s].get(o, "?") for o in gcc_levels} - {"?"}
            if len(gvals) > 1:
                gcc_inconsistent.append(s)      # gcc disagrees with itself -> quarantine
            elif len(gvals) == 1 and gvals - set(tvals):
                vsgcc_bad.append(s)             # gcc self-consistent AND != tcc -> real
            # len(gvals) == 0: gcc built for no level -> nothing to compare, skip.
    divergent = sorted(set(olevels_bad) | set(vsgcc_bad))

    if gcc_levels:
        progress(f"\nswept {len(seeds)} seeds — {len(divergent)} divergent "
                 f"(olevels={len(olevels_bad)}, vsgcc-only={len(vsgcc_bad)}, "
                 f"gcc-inconsistent/quarantined={len(gcc_inconsistent)})")
        print("OLEVELS", *olevels_bad)
        print("VSGCC", *vsgcc_bad)
        print("GCCBAD", *gcc_inconsistent)
    else:
        progress(f"\nswept {len(seeds)} seeds — {len(divergent)} divergent")
        for s in divergent:
            print(s)

    return 0  # workdir cleanup is handled by the atexit hook registered above


if __name__ == "__main__":
    raise SystemExit(main())
