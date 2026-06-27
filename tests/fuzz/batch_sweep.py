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

Usage:
    batch_sweep.py [LO] [HI] [--batch B] [--jobs J] [--olevels -O0,-O1,-O2,-Os]
    batch_sweep.py --seeds 588,860,1005          # explicit list

Prints the divergent seeds (one per line) on stdout — drop-in for the seed
enumeration in triage_olevels.sh.  Progress/stats go to stderr.
"""
from __future__ import annotations

import argparse
import concurrent.futures as cf
import os
import re
import shutil
import subprocess
import sys
import tempfile
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
_ENV = {**os.environ, "ASAN_OPTIONS": "detect_leaks=0:abort_on_error=0"}


def _run(cmd, **kw):
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          text=True, env=_ENV, **kw)


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
GEN_PROFILE = os.environ.get("FUZZ_PROFILE", "int")


def gen_seed(seed: int, wd: Path) -> Path | None:
    src = wd / f"seed{seed}.c"
    rc = _run(["python3", str(GENC), "--seed", str(seed),
               "--profile", GEN_PROFILE, "-o", str(src)])
    return src if rc.returncode == 0 and src.exists() else None


OBJCOPY = "arm-none-eabi-objcopy"


def compile_seed(seed: int, src: Path, olevel: str, wd: Path):
    """Compile one seed at one O-level, then rename main -> seed_<S> in the .o.

    The compile uses the real `main` (NOT -Dmain=...), so the emitted code is
    byte-identical to the standalone build — `main` may be special-cased by the
    optimizer, and we must not perturb codegen.  The symbol is renamed only
    afterwards, in the object file, so the runner can call it.

    Returns (obj_path, None) on success, or (None, "COMPILE_FAIL") on a real
    tcc error.  Transient infra errors are retried.
    """
    obj = wd / f"seed{seed}{olevel}.o"
    cmd = [str(TCC), *CF_COMMON, olevel, *INC, "-c", str(src), "-o", str(obj)]
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


# ---------------------------------------------------------------------------
# batching: build a runner over a list of seeds, link, run, parse
# ---------------------------------------------------------------------------
def build_runner_obj(seeds: list[int], olevel: str, wd: Path, tc) -> Path:
    """A runner whose main calls each seed_<S> in order, bracketed by markers."""
    decls = "".join(f"extern int seed_{s}(void);\n" for s in seeds)
    calls = "".join(
        f'  printf("S{s}\\n"); seed_{s}();\n' for s in seeds)
    runner_c = wd / f"runner_{olevel}_{seeds[0]}_{len(seeds)}.c"
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
        p = subprocess.Popen(
            ["qemu-system-arm", "-machine", "mps2-an505", "-nographic",
             "-semihosting", "-kernel", str(elf)],
            stdout=fh, stderr=subprocess.STDOUT, env=_ENV)
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


def sweep_olevel(seeds: list[int], olevel: str, objs: dict, wd: Path, tc,
                 batch: int, timeout_base: float) -> dict:
    """Run every seed at one O-level using batched ELFs; return seed->signature.

    `objs` maps seed -> compiled .o for THIS olevel (missing => COMPILE_FAIL,
    already recorded by the caller and excluded here).

    Work is a deque of chunks.  A chunk that fails to LINK is almost always a
    FLASH overflow (the mps2 image budget is 512K) — split it in half and retry
    as smaller batches, so batch size self-tunes to whatever fits; only a lone
    seed that won't link is recorded individually.  A chunk cut by a crash
    re-queues its post-crash tail."""
    from collections import deque
    results: dict[int, str] = {}
    avail = [s for s in seeds if s in objs]
    chunks = deque(avail[i:i + batch] for i in range(0, len(avail), batch))
    while chunks:
        chunk = chunks.popleft()
        runner = build_runner_obj(chunk, olevel, wd, tc)
        elf = wd / f"batch_{olevel}_{chunk[0]}_{len(chunk)}.elf"
        _run(link_cmd([objs[s] for s in chunk] + [runner], elf, tc))
        if not elf.exists():
            if len(chunk) > 1:                       # likely FLASH overflow -> split
                mid = len(chunk) // 2
                chunks.appendleft(chunk[mid:])
                chunks.appendleft(chunk[:mid])
            else:                                    # a single seed that won't link
                results[chunk[0]] = classify_one(chunk[0], olevel, wd, tc, timeout_base)
            continue
        # runtime grows with batch size; give each program ~80ms headroom.
        text, timed_out = run_elf(elf, timeout_base + 0.08 * len(chunk))
        res, crashed, kind = parse_batch(text, timed_out)
        results.update(res)
        if crashed is not None:
            results[crashed] = kind or classify_one(crashed, olevel, wd, tc, timeout_base)
            done = set(res) | {crashed}
            tail = [s for s in chunk if s not in done]   # post-crash tail
            if tail:
                chunks.appendleft(tail)
        else:
            # clean DONE (or a cut we couldn't attribute) — classify any stragglers
            for s in chunk:
                if s not in results:
                    results[s] = classify_one(s, olevel, wd, tc, timeout_base)
    return results


def classify_one(seed: int, olevel: str, wd: Path, tc, timeout_base: float) -> str:
    """Slow path: run a single seed in its own ELF and return its signature."""
    obj = wd / f"seed{seed}{olevel}.o"
    if not obj.exists():
        return "COMPILE_FAIL"
    runner = build_runner_obj([seed], olevel, wd, tc)
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
    ap.add_argument("--olevels", default="-O0,-O1,-O2,-Os")
    ap.add_argument("--timeout", type=float, default=20.0, help="base qemu timeout (s)")
    ap.add_argument("--keep", action="store_true", help="keep the work dir")
    ap.add_argument("--profile", default=GEN_PROFILE,
                    help="generator feature profile (int|float|...); default $FUZZ_PROFILE or int")
    args = ap.parse_args(argv)
    GEN_PROFILE = args.profile

    if not TCC.exists():
        sys.exit(f"no {TCC} — run 'make cross'")
    if shutil.which("qemu-system-arm") is None:
        sys.exit("qemu-system-arm not on PATH")

    olevels = [o.strip() for o in args.olevels.replace(",", " ").split()]
    if args.seeds:
        seeds = sorted({int(x) for x in args.seeds.replace(",", " ").split()})
    else:
        seeds = list(range(args.lo, args.hi + 1))

    tc = discover_toolchain()
    wd = Path(tempfile.mkdtemp(prefix="batchsweep_"))
    compile_boot(wd, tc)
    print(f"batch sweep: {len(seeds)} seeds x {len(olevels)} O-levels, "
          f"batch={args.batch}, jobs={args.jobs}\n  workdir {wd}", file=sys.stderr)

    # signatures[seed][olevel] = '<hex>'|HardFault|Lockup|COMPILE_FAIL
    signatures: dict[int, dict[str, str]] = {s: {} for s in seeds}

    # 1) generate sources (parallel).
    srcs: dict[int, Path] = {}
    with cf.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        for s, src in zip(seeds, ex.map(lambda s: gen_seed(s, wd), seeds)):
            if src is None:
                for o in olevels:
                    signatures[s][o] = "COMPILE_FAIL"
            else:
                srcs[s] = src
    print(f"  generated {len(srcs)}/{len(seeds)} sources", file=sys.stderr)

    # 2) compile every (seed, O-level) object (parallel).  COMPILE_FAIL recorded.
    objs: dict[str, dict[int, Path]] = {o: {} for o in olevels}
    work = [(s, o) for o in olevels for s in srcs]

    def _compile(item):
        s, o = item
        obj, err = compile_seed(s, srcs[s], o, wd)
        return s, o, obj, err

    done_c = 0
    with cf.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        for s, o, obj, err in ex.map(_compile, work):
            done_c += 1
            if obj is not None:
                objs[o][s] = obj
            else:
                signatures[s][o] = err
            if done_c % 500 == 0 or done_c == len(work):
                print(f"\r  compiled {done_c}/{len(work)}   ", end="", file=sys.stderr)
    print(file=sys.stderr)

    # 3) batched run per O-level (the O-levels run concurrently).
    with cf.ThreadPoolExecutor(max_workers=len(olevels)) as ex:
        futs = {ex.submit(sweep_olevel, seeds, o, objs[o], wd, tc,
                          args.batch, args.timeout): o for o in olevels}
        for fut in cf.as_completed(futs):
            o = futs[fut]
            for s, sig in fut.result().items():
                signatures[s][o] = sig
            print(f"  {o} done", file=sys.stderr)

    # 4) classify divergences: a seed is divergent if its O-level signatures
    #    are not all identical (matches the old sweep's val()-equality test).
    divergent = []
    for s in seeds:
        vals = [signatures[s].get(o, "?") for o in olevels]
        if len(set(vals)) > 1:
            divergent.append(s)

    print(f"\nswept {len(seeds)} seeds — {len(divergent)} divergent", file=sys.stderr)
    for s in divergent:
        print(s)

    if not args.keep:
        shutil.rmtree(wd, ignore_errors=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
