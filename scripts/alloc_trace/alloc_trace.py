#!/usr/bin/env python3
"""What holds a compile's memory at its peak, by call stack.

  alloc_trace.py build  [--out DIR]                  32-bit tracing cross (TCC_ALLOC_BACKTRACE)
  alloc_trace.py record [--out DIR] TRACE -- ARGS     run it on a compile, writing TRACE + TRACE.stacks
  alloc_trace.py report [--out DIR] TRACE [--at mapped|live] [--top N] [--depth N] [--focus FUNC]

The cross is built -m32 -malign-double -mlong-double-64, so its structures
have the device compiler's sizes (its kernel.c object is byte-identical to
the release cross's).  `report` replays the trace through the device's
malloc (libs/libc/malloc.c, with a port of the page allocator), finds the
peak of mapped pages (`--at live`: of requested bytes), and prints what is
live there by allocation site (the first caller past the allocator helpers),
by function inclusive of callees (every function on a block's stack), and
as caller trees for the biggest sites.  A pool (Sym, TokenSym, sections)
grows in whichever call first runs out of room, so for those the site is
the pool, and the callers below it only say who grew it last.
"""

import argparse
import collections
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path

TINYCC = Path(__file__).resolve().parents[2]
YASOS = TINYCC.parents[1]
LIBC_MALLOC = YASOS / "libs" / "libc" / "malloc.c"
DEFAULT_OUT = YASOS / ".cache" / "alloc_trace"

# Frames that only pass an allocation on.  The site is the first frame past them.
HELPERS = {
    "alloc_trace", "default_reallocator", "tcc_malloc_at", "tcc_mallocz_at", "tcc_realloc_at",
    "tcc_malloc", "tcc_mallocz", "tcc_realloc", "tcc_strdup", "tcc_strdup_at", "tcc_free",
    "tcc_hot_mallocz_at", "tcc_hot_realloc_at", "tcc_hot_settle_at", "dynarray_add",
    "vector_grow", "vec_grow", "small_sequence_grow",
}


def sh(cmd, **kw):
    return subprocess.run(cmd, check=True, **kw)


def cmd_build(a):
    out = Path(a.out)
    src = out / "src"
    out.mkdir(parents=True, exist_ok=True)
    # a copy of the working tree: the tracing build must not touch the real cross
    sh(["rsync", "-a", "--delete", "--exclude=.git", "--exclude=armv8m-*", "--exclude=*.o", "--exclude=*.a",
        "--exclude=build/", "--exclude=tests/", "--exclude=venv/", "--exclude=bin/", f"{TINYCC}/", f"{src}/"])
    sysroot = YASOS / "rootfs"
    # -DTCC_IR_SMALL: the device build's IR layout (8-byte quads, 16-bit indexes; small_ir.h), which a
    # host build otherwise doubles
    cflags = ("-m32 -malign-double -mlong-double-64 -no-pie -g -O2 -DTCC_ALLOC_BACKTRACE -DCONFIG_TCC_CHECKS=0 "
              "-DTCC_IR_SMALL "
              "-DTCC_DEBUG=0 -DTARGETOS_YasOS=1 -DCONFIG_TCC_BCHECK=0 -Wall -Werror "
              "-DCONFIG_TCC_DEFAULT_FPU=ARM_FPU_NONE -Wno-unused-variable" + (" " + a.cflags if a.cflags else ""))
    sh(["./configure", "--cpu=i386", "--extra-ldflags=-m32 -no-pie", f"--extra-cflags={cflags}", "--enable-cross",
        "--config-asm=yes", "--config-bcheck=no", "--config-pie=yes", "--config-pic=yes", f"--prefix={src}",
        f"--sysroot={sysroot}", f"--libpaths={sysroot}/usr/lib:{{B}}:{sysroot}/lib", f"--crtprefix={sysroot}/usr/lib",
        f"--sysincludepaths={sysroot}/usr/include:{{B}}/include"], cwd=src, stdout=subprocess.DEVNULL)
    shutil.copy(src / "config.h", src / "config.h.cross")
    log = out / "build.log"
    with open(log, "w") as f:
        r = subprocess.run(["make", f"-j{os.cpu_count()}", "AR=ar", f"CROSS_FLAGS=-I{YASOS}/libs/libc",
                            f"INC-armv8m={sysroot}/usr/include:{{B}}/include", "armv8m-tcc"],
                           cwd=src, stdout=f, stderr=subprocess.STDOUT)
    if r.returncode:
        sys.exit(f"build failed, see {log}")
    shutil.copy(src / "armv8m-tcc", out / "armv8m-tcc-trace")
    print(out / "armv8m-tcc-trace")


def cmd_record(a):
    tcc = Path(a.out) / "armv8m-tcc-trace"
    if not tcc.exists():
        sys.exit(f"{tcc} missing: run `build` first")
    env = dict(os.environ, TCC_ALLOCTRACE=str(Path(a.trace).resolve()))
    # the report symbolizes with the binary that made the trace, not a later build
    shutil.copy(tcc, a.trace + ".bin")
    args = a.args[1:] if a.args and a.args[0] == "--" else a.args
    sys.exit(subprocess.run([str(tcc), *args], env=env).returncode)


def peak_tool(out):
    exe = Path(out) / "alloc_peak_dev"
    src = Path(__file__).with_name("alloc_peak.c")
    if not exe.exists() or exe.stat().st_mtime < max(src.stat().st_mtime, LIBC_MALLOC.stat().st_mtime):
        sh(["gcc", "-m32", "-O2", "-Wall", "-Wno-nonnull-compare", f'-DMALLOC_C="{LIBC_MALLOC}"', "-o", str(exe),
            str(src)])
    return exe


def read_stacks(path):
    stacks = {}
    data = Path(path).read_bytes()
    i = 0
    while i + 8 <= len(data):
        sid, n = struct.unpack_from("<II", data, i)
        stacks[sid] = struct.unpack_from(f"<{n}I", data, i + 8)
        i += 8 + 4 * n
    return stacks


def symbolize(binary, pcs):
    """pc -> (function, file:line) of the call, inlined frames folded into the outermost."""
    pcs = sorted(pcs)
    out = subprocess.run(["addr2line", "-f", "-e", str(binary)] + [hex(p - 1) for p in pcs],
                         capture_output=True, text=True, check=True).stdout.splitlines()
    res = {}
    for k, p in enumerate(pcs):
        fn, loc = out[2 * k], out[2 * k + 1]
        loc = os.path.normpath(loc.split(" (")[0])
        if "/source/" in loc:
            loc = "source/" + loc.split("/source/", 1)[1]
        res[p] = (fn.split(".")[0], loc)
    return res


def mb(n):
    return f"{n / 1e6:7.3f}"


def cmd_report(a):
    exe = peak_tool(a.out)
    first = subprocess.run([str(exe), a.trace], capture_output=True, text=True, check=True).stdout
    info = {}
    for line in first.splitlines():
        p = line.split()
        info[p[1]] = p[2:]
    stop = int(info["mapped_peak" if a.at == "mapped" else "live_peak"][2])
    passes = []
    if os.path.exists(a.trace + ".passes"):
        names = dict(l.rstrip("\n").split(" ", 1) for l in open(a.trace + ".passes") if " " in l)
        for line in first.splitlines():
            p = line.split()
            if p[0] == "P":
                passes.append((int(p[2]), int(p[3]), names.get(p[1], p[1]), int(p[4]) if len(p) > 4 else -1,
                               int(p[5]) if len(p) > 6 else 0, int(p[6]) if len(p) > 6 else -1))
    if a.pass_name:
        hit = [x for x in passes if x[2] == a.pass_name]
        if not hit or hit[0][3] < 0:
            sys.exit(f"no pass {a.pass_name} in {a.trace}.passes")
        stop = hit[0][5] if a.at == "mapped" and hit[0][5] >= 0 else hit[0][3]
    second = subprocess.run([str(exe), a.trace, "--stop", str(stop)], capture_output=True, text=True,
                            check=True).stdout
    per_stack, classes, mstats = {}, [], None
    for line in second.splitlines():
        p = line.split()
        if p[0] == "S":
            per_stack[int(p[1])] = tuple(int(x) for x in p[2:9])
        elif p[0] == "B":
            classes.append((int(p[1]), int(p[2]), int(p[3])))
        elif p[0] == "M":
            mstats = [int(x) for x in p[1:]]
        elif p[0] == "E":
            info[p[1]] = p[2:]
    stacks = read_stacks(a.trace + ".stacks")
    binary = Path(a.trace + ".bin")
    if not binary.exists():
        binary = Path(a.out) / "armv8m-tcc-trace"
    pcs = {pc for sid in per_stack for pc in stacks.get(sid, ())}
    sym = symbolize(binary, pcs)

    live_now = int(info["live_now"][0])
    mapped_now = int(info["mapped_now"][0])
    print(f"events {info['events'][0]}   peak at event {stop} ({'pass ' + a.pass_name if a.pass_name else a.at})")
    print(f"mapped {mb(mapped_now)} MB   live (requested) {mb(live_now)} MB   "
          f"allocator overhead {mb(mapped_now - live_now)} MB   "
          f"(live peak {mb(int(info['live_peak'][0]))} MB, mapped peak {mb(int(info['mapped_peak'][0]))} MB)")
    if mstats:
        names = ["pools", "pool_bytes", "free", "free_pages", "bump", "spare", "retired", "retired_bytes",
                 "retired_free", "retired_free_pages", "parked"]
        print("malloc: " + "  ".join(f"{n}={v}" for n, v in zip(names, mstats)))
        print("free chunks: " + "  ".join(f"{k}={v}" for k, v in zip(
            ["<64", "<256", "<1K", "<2K", "<4K", "<8K", "<16K", ">=16K"], mstats[11:19])))

    def frames(sid):
        return [sym[pc] for pc in stacks.get(sid, ())]

    def site_index(fr):
        k = 0
        while k < len(fr) - 1 and fr[k][0] in HELPERS:
            k += 1
        return k

    by_site = collections.defaultdict(lambda: [0, 0, 0, 1 << 31, 0, 0, 0])
    by_func = collections.defaultdict(lambda: [0, 0])
    site_callers = collections.defaultdict(lambda: collections.defaultdict(lambda: [0, 0]))
    for sid, vals in per_stack.items():
        b, n, g, smin, smax, over = vals[:6]
        spk = vals[6] if len(vals) > 6 else 0
        fr = frames(sid)
        if a.focus and not any(f[0] == a.focus for f in fr):
            continue
        k = site_index(fr)
        site = f"{fr[k][0]}  {fr[k][1]}" if fr else "?"
        by_site[site][0] += b
        by_site[site][1] += n
        by_site[site][2] += g
        by_site[site][3] = min(by_site[site][3], smin)
        by_site[site][4] = max(by_site[site][4], smax)
        by_site[site][5] += over
        by_site[site][6] += spk
        for fn in {f[0] for f in fr}:
            by_func[fn][0] += b
            by_func[fn][1] += n
        path = " <- ".join(f[0] for f in fr[k + 1:k + 1 + a.depth])
        site_callers[site][path][0] += b
        site_callers[site][path][1] += n

    if "in_function" in info and info["in_function"][0] == "1":
        spike, base = int(info["in_function"][4]), int(info["in_function"][6])
        print(f"in a function's compile (since event {info['in_function'][2]}): {mb(spike)} MB born there, "
              f"{mb(base)} MB from before (the base it runs on)")
        for title, idx, other in (("spike: born in the function being compiled", 6, False),
                                  ("base: there before it started", 6, True)):
            print(f"  == {title}")
            rows = sorted(by_site.items(), key=lambda x: -((x[1][0] - x[1][6]) if other else x[1][6]))
            for site, v in rows[:a.top // 2 or 10]:
                val = (v[0] - v[6]) if other else v[6]
                if val <= 0:
                    break
                print(f"  {mb(val)}  {site}")
    total = sum(v[0] for v in by_site.values())
    print(f"\n== by allocation site (first caller past the allocator) -- {mb(total)} MB" +
          (f" under {a.focus}" if a.focus else ""))
    grown = sum(v[2] for v in by_site.values())
    print(f"(grown: in blocks a realloc last grew -- growable arrays, {mb(grown)} MB in all)")
    print(f"{'MB':>7} {'%':>5} {'blocks':>8} {'grown':>7} {'sizes':>13}  site")
    for site, (b, n, g, smin, smax, _, _) in sorted(by_site.items(), key=lambda x: -x[1][0])[:a.top]:
        sizes = f"{smin}" if smin == smax else f"{smin}-{smax}"
        print(f"{mb(b)} {100 * b / live_now:5.1f} {n:8d} {mb(g)} {sizes:>13}  {site}")
    if a.small:
        # small blocks by site: what an 8-byte header and pool rounding cost each, and whether
        # the site allocates one size (a fixed-size type a slab would hold without headers)
        print(f"\n== small blocks (< {a.small} B) by site: count, header+rounding, sizes")
        rows = [(over, n, b, smin, smax, site) for site, (b, n, g, smin, smax, over, _) in by_site.items()
                if smax and smax < a.small]
        tot = sum(r[0] for r in rows)
        print(f"   {mb(tot)} MB of headers and rounding in {sum(r[1] for r in rows)} blocks")
        for over, n, b, smin, smax, site in sorted(rows, reverse=True)[:a.top]:
            sizes = f"{smin}" if smin == smax else f"{smin}-{smax}"
            print(f"   {mb(over)} {n:8d} {mb(b)} {sizes:>11}  {site}")

    print(f"\n== by function, inclusive (every function on the block's stack)")
    for fn, (b, n) in sorted(by_func.items(), key=lambda x: -x[1][0])[:a.top]:
        if fn in HELPERS:
            continue
        print(f"{mb(b)} {100 * b / live_now:5.1f} {n:8d}  {fn}")

    print(f"\n== callers of the top sites ({a.depth} frames)")
    for site, _ in sorted(by_site.items(), key=lambda x: -x[1][0])[:a.tree]:
        print(f"{mb(by_site[site][0])}  {site}")
        for path, (b, n) in sorted(site_callers[site].items(), key=lambda x: -x[1][0])[:6]:
            print(f"   {mb(b)} {n:8d}  <- {path}")

    if passes:
        print("\n== passes: the most each held above what was live when it started (any function),"
              " and the most live while it ran")
        for d, l, name, _, m, _ in sorted(passes, reverse=True)[:a.top]:
            print(f"  {mb(d)} MB  (live up to {mb(l)}, mapped up to {mb(m)})  {name}")
        print("\n== passes by the most mapped while they ran")
        for d, l, name, _, m, _ in sorted(passes, key=lambda x: -x[4])[:a.top]:
            print(f"  mapped up to {mb(m)}  (live up to {mb(l)})  {name}")
    print("\n== live block sizes")
    for c, b, n in classes:
        if b > live_now / 200:
            print(f"  <= {c:8d}  {mb(b)} MB  {n:8d} blocks")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=str(DEFAULT_OUT), help="build and tool directory (default .cache/alloc_trace)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build")
    b.add_argument("--cflags", default="", help="extra compiler flags, e.g. -DSYM_DEBUG (every Sym its own block)")
    r = sub.add_parser("record")
    r.add_argument("trace")
    r.add_argument("args", nargs=argparse.REMAINDER)
    p = sub.add_parser("report")
    p.add_argument("trace")
    p.add_argument("--at", choices=["mapped", "live"], default="mapped")
    p.add_argument("--top", type=int, default=30)
    p.add_argument("--tree", type=int, default=10, help="caller trees for this many sites")
    p.add_argument("--depth", type=int, default=4, help="caller frames in a tree path")
    p.add_argument("--focus", help="only blocks with this function on their stack")
    p.add_argument("--small", type=int, default=0, help="also list sites of blocks under this size")
    p.add_argument("--pass", dest="pass_name",
                   help="report at the most this pass held (mapped, or live with --at live), not at the peak")
    a = ap.parse_args()
    {"build": cmd_build, "record": cmd_record, "report": cmd_report}[a.cmd](a)


if __name__ == "__main__":
    main()
