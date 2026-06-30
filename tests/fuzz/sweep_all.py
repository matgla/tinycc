#!/usr/bin/env python3
"""sweep_all.py — one entry point that fuzzes EVERY generator profile and rolls
the results into a single combined report.

Why this exists
---------------
`gen_c.py` now has eight profiles (int, float, fnptr, bitfield, switch,
struct_byval, varargs, ptr).  Each samples a *different* slice of C, so the
highest-leverage thing you can do with a fixed budget is **breadth across
profiles**, not depth in one (fnptr/varargs stayed clean over 5000 seeds while a
sibling profile found a crash by seed 62).  This script runs them all with the
right oracle each, ordered by historical yield, and aggregates.

Processing strategy (the defaults encode it)
--------------------------------------------
1. BREADTH FIRST.  All profiles over the SAME band before any goes deep.
2. YIELD ORDER.  Profiles run most-productive-first (ptr, bitfield, float, switch,
   struct_byval, then the historically-clean fnptr, the certified-baseline int,
   and varargs) so a time-boxed run hits the rich seams early.
3. TWO PHASE.  `--mode prescan` (default) runs the fast batch_sweep pre-scan
   (~200 seeds/qemu-boot, ~80% recall) to FIND candidates.  `--mode triage`
   additionally hands each profile's flagged seeds to triage_olevels.sh for the
   exhaustive, full-recall culprit bisect (this is what CERTIFIES a band).
4. RIGHT ORACLE.  olevels self-consistency for every profile; PLUS vs-gcc for the
   ABI/value-shaped ones (float/bitfield/struct_byval/fnptr/varargs) — the only
   oracle that sees the O0-WRONG class (all tcc levels agree but are wrong).  int
   and the no-ABI switch/ptr run olevels-only by default.
5. RECALL CAVEAT.  batch_sweep under-recalls the context-sensitive uninit/alias
   class that ptr & struct_byval target, so a "0 divergent" pre-scan for those is
   NOT a clean certificate — use `--mode triage` (or triage_olevels.sh directly).

On seeds
--------
Seeds are arbitrary non-negative ints (Python big-int; bash tooling tops out near
2^63).  There is no useful "maximum" — the generator's bounded structure means new
*bug classes* get rarer as seeds grow, so prefer widening the PROFILE set / band
over chasing a six-figure seed on one profile.

Usage
-----
    # default: pre-scan all profiles over 0..4999, both oracles where they apply
    python3 tests/fuzz/sweep_all.py
    python3 tests/fuzz/sweep_all.py 0 9999                 # a wider band
    python3 tests/fuzz/sweep_all.py 0 2000 --profiles ptr,bitfield,switch
    python3 tests/fuzz/sweep_all.py 0 4999 --olevels-only  # skip the vs-gcc pass
    python3 tests/fuzz/sweep_all.py 0 999  --mode triage    # + exhaustive bisect
    python3 tests/fuzz/sweep_all.py 0 4999 --jobs 24 --out my_report.md

Writes a combined report to fuzz_triage_all_<lo>_<hi>.md and prints a summary.
Exit code is the number of profiles that diverged (0 = everything clean).
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import time
from pathlib import Path

THIS_DIR = Path(__file__).resolve().parent
REPO_ROOT = THIS_DIR.parent.parent
BATCH_SWEEP = THIS_DIR / "batch_sweep.py"
TRIAGE_SH = THIS_DIR / "triage_olevels.sh"
VSGCC_TEST = THIS_DIR / "test_random_c_vs_gcc.py"

# Profiles in DESCENDING historical yield, each tagged with the oracle(s) that see
# its bug class.  "olevels" = O0/O1/O2/Os self-consistency (batch_sweep).
# "vsgcc"   = arm-none-eabi-gcc -O2 gold (catches O0-WRONG / ABI).  "both" = run each.
PROFILES = [
    ("ptr",          "olevels", "densest alias/deref seam (DSE/load-CSE/store-fwd)"),
    ("bitfield",     "both",    "bitfield RMW insert/extract + packed access"),
    ("float",        "both",    "softfloat arith + FP compare (open backlog)"),
    ("switch",       "olevels", "jump-table vs if-chain dispatch"),
    ("struct_byval", "both",    "AAPCS struct passing + sret (crash class)"),
    ("fnptr",        "vsgcc",   "indirect-call ABI / sret-through-fnptr"),
    ("int",          "olevels", "baseline integer stream (regression gate; certified 0-9999)"),
    ("varargs",      "vsgcc",   "stdarg frame layout / r0-r3 spill"),
]
# batch_sweep's ~80% recall under-reports exactly these profiles' bug class.
LOW_RECALL_ON_PRESCAN = {"ptr", "struct_byval"}

_CHILD_ENV = dict(os.environ)
_CHILD_ENV.setdefault("ASAN_OPTIONS", "detect_leaks=0")


def _stream(cmd: list[str], env: dict, prefix: str = "    ") -> tuple[int | None, str]:
    """Run ``cmd``, tee its merged stdout/stderr to the terminal live (each line
    ``prefix``-indented), and return (returncode, full_output).  rc is None on a
    launch failure (full_output then holds the error string)."""
    try:
        proc = subprocess.Popen(cmd, cwd=str(REPO_ROOT), env=env,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, bufsize=1)
    except Exception as e:                                  # pragma: no cover
        return None, f"failed to launch: {e}"
    buf = []
    assert proc.stdout is not None
    for line in proc.stdout:
        buf.append(line)
        sys.stdout.write(prefix + line)
        sys.stdout.flush()
    proc.wait()
    return proc.returncode, "".join(buf)


def run_olevels_prescan(profile: str, lo: int, hi: int, jobs: int) -> tuple[list[int], str]:
    """Fast batch_sweep pre-scan; returns (divergent_seeds, error_or_empty).

    batch_sweep prints its native progress ("generated X/Y", "compiled X/Y",
    "-O0 done", "swept N — M divergent") to STDERR; we let that stream straight to
    the terminal (stderr inherited) for live feedback while capturing STDOUT (the
    divergent seed list) for parsing.  ``-u`` keeps the child unbuffered so the
    progress appears promptly.
    """
    cmd = [sys.executable, "-u", str(BATCH_SWEEP), str(lo), str(hi),
           "--profile", profile, "--jobs", str(jobs)]
    try:
        p = subprocess.run(cmd, cwd=str(REPO_ROOT), env=_CHILD_ENV,
                           stdout=subprocess.PIPE, text=True)   # stderr -> terminal (live)
    except Exception as e:                                  # pragma: no cover
        return [], f"batch_sweep failed to launch: {e}"
    if p.returncode != 0:
        return [], f"batch_sweep rc={p.returncode} (see streamed output above)"
    seeds = sorted(int(x) for x in p.stdout.split())
    return seeds, ""


def run_vsgcc(profile: str, lo: int, hi: int, jobs: int) -> tuple[list[int], str]:
    """vs-gcc differential over [lo,hi]; returns (divergent_seeds, error_or_empty).

    Parses `seed<N>` out of pytest FAILED lines.  Skips cleanly if the gcc/QEMU
    reference runtime is not prepared.
    """
    env = dict(_CHILD_ENV, FUZZ_PROFILE=profile, FUZZ_VSGCC_SEEDS=f"{lo}-{hi}")
    cmd = [sys.executable, "-m", "pytest", str(VSGCC_TEST), "-q", "-p", "no:cacheprovider"]
    if jobs > 1:
        cmd += ["-n", str(jobs)]
    rc, out = _stream(cmd, env)                 # live-tee pytest's progress dots
    if rc is None:
        return [], "pytest " + out
    if "no tests ran" in out and "skipped" in out:
        return [], "vs-gcc skipped (gcc/QEMU reference runtime not prepared)"
    fails = sorted({int(m) for m in re.findall(r"seed(\d+)", _failed_block(out))})
    return fails, ""


def _failed_block(pytest_out: str) -> str:
    """Restrict seed-number extraction to FAILED/ERROR lines so passing-seed ids
    (which pytest never prints anyway) can't leak in."""
    return "\n".join(l for l in pytest_out.splitlines()
                     if l.startswith(("FAILED", "ERROR")) or " failed" in l)


def run_triage(profile: str, seeds: list[int], jobs: int) -> str:
    """Exhaustive culprit-bisect of an explicit seed list via triage_olevels.sh.
    Returns the path of the markdown table it wrote (best-effort)."""
    if not seeds:
        return ""
    env = dict(_CHILD_ENV, FUZZ_PROFILE=profile, SEEDS=" ".join(str(s) for s in seeds))
    cmd = ["bash", str(TRIAGE_SH), "0", "0", str(jobs)]   # SEEDS overrides lo/hi
    subprocess.run(cmd, cwd=str(REPO_ROOT), env=env)
    return f"fuzz_triage_{profile}_*.md (per triage_olevels.sh)"


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("lo", nargs="?", type=int, default=0)
    ap.add_argument("hi", nargs="?", type=int, default=4999)
    ap.add_argument("--profiles", default="",
                    help="comma list to restrict (default: all, yield-ordered)")
    ap.add_argument("--mode", choices=["prescan", "triage"], default="prescan",
                    help="prescan = fast find (default); triage = + exhaustive bisect of flagged seeds")
    ap.add_argument("--olevels-only", action="store_true",
                    help="skip the vs-gcc pass (olevels self-consistency only)")
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 4) - 2))
    ap.add_argument("--out", default="", help="report path (default fuzz_triage_all_<lo>_<hi>.md)")
    args = ap.parse_args(argv)

    wanted = [p.strip() for p in args.profiles.split(",") if p.strip()]
    profiles = [t for t in PROFILES if (not wanted or t[0] in wanted)]
    if not profiles:
        sys.exit(f"no matching profiles in {args.profiles!r}; known: {[p[0] for p in PROFILES]}")

    out_path = Path(args.out) if args.out else REPO_ROOT / f"fuzz_triage_all_{args.lo}_{args.hi}.md"
    lines = [f"# Combined fuzz sweep — seeds {args.lo}..{args.hi}",
             "",
             f"Mode: **{args.mode}** · jobs: {args.jobs} · "
             f"oracles: olevels{'' if args.olevels_only else ' + vs-gcc (ABI profiles)'}",
             "",
             "| profile | olevels | vs-gcc | yield rank seam |",
             "|---|---|---|---|"]
    n_diverged = 0
    detail: list[str] = []
    start = time.monotonic()
    span = args.hi - args.lo + 1
    print(f"sweep_all: {len(profiles)} profile(s) × {span} seed(s) [{args.lo}..{args.hi}] · "
          f"mode={args.mode} · jobs={args.jobs} · "
          f"oracles=olevels{'' if args.olevels_only else '+vs-gcc'}")
    print("yield order:  " + "  >  ".join(p[0] for p in profiles), flush=True)

    for i, (name, oracle, blurb) in enumerate(profiles, 1):
        t0 = time.monotonic()
        print(f"\n[{i}/{len(profiles)}] {name} — {blurb}   (t+{t0 - start:.0f}s)", flush=True)

        print(f"  olevels pre-scan [{args.lo}..{args.hi}] ...", flush=True)
        ol_seeds, ol_err = run_olevels_prescan(name, args.lo, args.hi, args.jobs)
        ol_cell = ol_err or str(len(ol_seeds))
        print(f"  olevels = {ol_cell}  [{time.monotonic() - t0:.0f}s]"
              + (f"  -> {ol_seeds[:20]}{' ...' if len(ol_seeds) > 20 else ''}" if ol_seeds else ""),
              flush=True)

        vg_cell = "—"
        vg_seeds: list[int] = []
        if not args.olevels_only and oracle in ("vsgcc", "both"):
            t1 = time.monotonic()
            print(f"  vs-gcc sweep [{args.lo}..{args.hi}] ...", flush=True)
            vg_seeds, vg_err = run_vsgcc(name, args.lo, args.hi, args.jobs)
            vg_cell = vg_err or str(len(vg_seeds))
            print(f"  vs-gcc  = {vg_cell}  [{time.monotonic() - t1:.0f}s]"
                  + (f"  -> {vg_seeds[:20]}{' ...' if len(vg_seeds) > 20 else ''}" if vg_seeds else ""),
                  flush=True)

        recall_note = " ⚠low-recall pre-scan" if (name in LOW_RECALL_ON_PRESCAN and not ol_err) else ""
        lines.append(f"| `{name}` | {ol_cell}{recall_note} | {vg_cell} | {blurb} |")

        flagged = sorted(set(ol_seeds) | set(vg_seeds))
        if flagged:
            n_diverged += 1
            detail.append(f"\n## `{name}` — {len(flagged)} divergent seed(s)\n")
            detail.append("```\n" + " ".join(str(s) for s in flagged) + "\n```\n")
            if args.mode == "triage":
                print(f"  triage: bisecting {len(flagged)} flagged seed(s) ...", flush=True)
                tbl = run_triage(name, flagged, args.jobs)
                detail.append(f"Culprit bisect: see `{tbl}`.\n")
        print(f"  [{name} done in {time.monotonic() - t0:.0f}s · "
              f"{len(flagged)} flagged]", flush=True)

    lines.append("")
    if any(p in LOW_RECALL_ON_PRESCAN for p, _, _ in profiles):
        lines.append("> ⚠ `ptr`/`struct_byval` pre-scan counts are ~80%-recall lower bounds "
                     "(batch_sweep under-reports their context-sensitive class). "
                     "Run `--mode triage` for the certifying full-recall sweep.")
        lines.append("")
    lines += detail
    out_path.write_text("\n".join(lines) + "\n")

    print(f"\n{'='*60}\nReport: {out_path}")
    print(f"Profiles with divergences: {n_diverged}/{len(profiles)}   "
          f"total time {time.monotonic() - start:.0f}s")
    return n_diverged


if __name__ == "__main__":
    raise SystemExit(main())
