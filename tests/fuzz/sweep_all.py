#!/usr/bin/env python3
"""sweep_all.py — one entry point that fuzzes EVERY generator profile and rolls
the results into a single combined report.

Why this exists
---------------
`gen_c.py` now has sixteen profiles: the wave-1 set (int, float, fnptr,
bitfield, switch, struct_byval, varargs, ptr) plus the wave-2 set (longlong,
signed, combo, combo_num, fp_deep, fp_round, volatile, agg_deep — see
docs/plan_fuzz_wave2.md).  Each samples a *different* slice of C, so the
highest-leverage thing you can do with a fixed budget is **breadth across
profiles**, not depth in one (fnptr/varargs stayed clean over 5000 seeds while a
sibling profile found a crash by seed 62).  This script runs them all with the
right oracle each, ordered by historical yield (wave-2 profiles are unswept as
of landing — placed after the measured wave-1 order, ranked by the wave-2
plan's a-priori density estimate), and aggregates.

Processing strategy (the defaults encode it)
--------------------------------------------
1. BREADTH FIRST.  All profiles over the SAME band before any goes deep.
2. YIELD ORDER.  Profiles run most-productive-first (ptr, bitfield, float, switch,
   struct_byval, then the historically-clean fnptr, the certified-baseline int,
   and varargs) so a time-boxed run hits the rich seams early.
3. TWO PHASE.  `--mode prescan` (default) runs the fast batch_sweep pre-scan
   (~200 seeds/qemu-boot, ~80% recall) to FIND candidates.  It sweeps tcc
   -O0/-O1/-O2 only (no -Os — the -Os-exclusive miscompile class is vanishingly
   rare and gcc, not the extra tcc level, is the expensive part of a sweep).  For
   profiles with a vs-gcc oracle (below), batch_sweep links a per-seed
   `arm-none-eabi-gcc -O0` object into the SAME batched ELF
   (`--olevels -O0,-O1,-O2,gcc-O0`), so ONE pass finds olevels self-consistency
   AND vs-gcc candidates at no extra qemu-boot cost.  gcc-O0 is the reference
   (not gcc-O2): it is both cheaper to compile and the more trustworthy oracle —
   the known gcc wrong-code bugs are all at -O2 (e.g. bitfield seed 1486).  This
   halves the gcc compile cost (>half of a cold sweep) vs the old two-gcc-level
   pass; the trade is that a single gcc level has no automated self-consistency
   quarantine, so a (near-nonexistent) gcc-O0 miscompile would surface as a
   vs-gcc candidate that the triage/bisect step dismisses by hand — no separate
   per-seed pytest pass in this mode.  `--mode triage` does NOT use batch_sweep at all:
   it runs triage_olevels.sh's exhaustive per-seed sweep over the WHOLE band
   (full recall, no batch) and culprit-bisects every divergent seed in the same
   pass (this is what CERTIFIES a band); the vs-gcc side still runs the
   exhaustive per-seed pytest pass here (full recall matters more than speed
   when certifying), and only the vs-gcc-only seeds (the O0-WRONG/ABI class the
   olevels sweep can't see) get triaged.
4. RIGHT ORACLE.  olevels (-O1/-O2) self-consistency PLUS a gcc-O0 reference for
   EVERY profile except fp_round — the gcc reference is the only oracle that sees
   the O0-WRONG class (all tcc levels agree but are wrong), and it doubles as the
   correctness reference that lets us drop tcc -O0 from the prescan.  fp_round is
   the sole olevels-only profile (gcc's compile-time FP rounding differs from
   tcc's runtime softfloat, so it would false-positive); it keeps tcc -O0.  The
   gcc-O0 reference objects are cached persistently (tests/fuzz/.sweep_cache,
   keyed on gen_c.py + gcc version, NOT tcc), so they are compiled once and
   reused across every re-sweep.
5. RECALL CAVEAT.  batch_sweep under-recalls the context-sensitive uninit/alias
   class that ptr & struct_byval target, so a "0 divergent" pre-scan for those is
   NOT a clean certificate — use `--mode triage` (or triage_olevels.sh directly).

On seeds
--------
Seeds are arbitrary non-negative ints (Python big-int; bash tooling tops out near
2^63).  There is no useful "maximum" — the generator's bounded structure means new
*bug classes* get rarer as seeds grow, so prefer widening the PROFILE set / band
over chasing a six-figure seed on one profile.

Parallel profiles
-----------------
Profiles run --parallel-profiles at a time (default 3): a profile's
qemu-latency-bound run phase overlaps a sibling's CPU-bound compile phase, so
the sweep no longer serializes on each profile's slowest stage.  --jobs is a
TOTAL budget, split evenly across the concurrent profiles (each child gets
jobs // parallel), so peak process/qemu pressure stays at the sequential
level.  Every streamed child line is prefixed with its [profile] so the live
log stays attributable; the report table stays in yield order regardless of
completion order.  --parallel-profiles 1 restores strictly sequential runs.

Usage
-----
    # default: pre-scan all profiles over 0..4999, both oracles where they apply
    python3 tests/fuzz/sweep_all.py
    python3 tests/fuzz/sweep_all.py 0 9999                 # a wider band
    python3 tests/fuzz/sweep_all.py 0 2000 --profiles ptr,bitfield,switch
    python3 tests/fuzz/sweep_all.py 0 4999 --olevels-only  # skip the vs-gcc pass
    python3 tests/fuzz/sweep_all.py 0 999  --mode triage    # full-recall sweep + bisect (no batch)
    python3 tests/fuzz/sweep_all.py 0 4999 --jobs 24 --out my_report.md
    python3 tests/fuzz/sweep_all.py 0 4999 --parallel-profiles 1   # old sequential behavior

Writes a combined report to fuzz_triage_all_<lo>_<hi>.md and prints a summary.
Exit code is the number of profiles that diverged (0 = everything clean).
"""

from __future__ import annotations

import argparse
import concurrent.futures as cf
import os
import re
import subprocess
import sys
import threading
import time
from pathlib import Path

THIS_DIR = Path(__file__).resolve().parent
REPO_ROOT = THIS_DIR.parent.parent
BATCH_SWEEP = THIS_DIR / "batch_sweep.py"
TRIAGE_SH = THIS_DIR / "triage_olevels.sh"
VSGCC_TEST = THIS_DIR / "test_random_c_vs_gcc.py"

# Profiles in DESCENDING historical yield, each tagged with the oracle(s) that see
# its bug class.  "olevels" = O0/O1/O2 self-consistency only (batch_sweep).
# "both" = ALSO run a per-seed arm-none-eabi-gcc-O0 reference (catches O0-WRONG /
# ABI).  gcc-O0 is a valid value oracle for every UB-free generator EXCEPT
# fp_round (gcc constant-folds FP with different last-bit rounding than tcc's
# runtime softfloat -> false positives), so fp_round is the sole "olevels" entry;
# it keeps tcc -O0 as its correctness reference.  Every "both" profile drops tcc
# -O0 from the prescan (gcc-O0 is the reference; triage re-adds -O0 to classify).
PROFILES = [
    ("ptr",          "both",    "densest alias/deref seam (DSE/load-CSE/store-fwd)"),
    ("bitfield",     "both",    "bitfield RMW insert/extract + packed access"),
    ("float",        "both",    "softfloat arith + FP compare (open backlog)"),
    ("switch",       "both",    "jump-table vs if-chain dispatch"),
    ("struct_byval", "both",    "AAPCS struct passing + sret (crash class)"),
    ("fnptr",        "both",    "indirect-call ABI / sret-through-fnptr"),
    ("int",          "both",    "baseline integer stream (regression gate; certified 0-9999)"),
    ("varargs",      "both",    "stdarg frame layout / r0-r3 spill"),
    # --- wave 2 (docs/plan_fuzz_wave2.md); unswept as of landing, ranked by the
    # plan's a-priori density estimate rather than measured yield ---
    ("longlong",     "both",    "64-bit register-pair codegen + aeabi div/mod/shift/cmp libcalls"),
    ("signed",       "both",    "SDIV/magic-number strength reduction, ASR vs LSR, SXT narrowing"),
    ("combo",        "both",    "cross-feature seams: ptr+switch+bitfield+struct_byval"),
    ("combo_num",    "both",    "cross-feature seams: longlong+float+signed"),
    ("fp_deep",      "both",    "EXACT int<->fp round trip, integer-exact a*b+c, loop-carried FP"),
    # olevels-ONLY (the ONE exception to the gcc-for-all rule): full-mantissa
    # (non-exact) FP ops; a correctly-rounded but DIFFERENT soft-float library
    # could legally disagree with tcc in the last bit -- never promote this to
    # "both" (see gen_c.py _fconst_round() and docs/plan_fuzz_wave2.md SS4.4).
    # Keeps tcc -O0 (its only correctness reference, since gcc can't serve here).
    ("fp_round",     "olevels", "full-mantissa FP rounding stress (GRS logic); olevels-only by design"),
    ("volatile",     "both",    "volatile access ordering vs DSE/load-CSE over-elimination"),
    ("agg_deep",     "both",    "nested structs, 2-D arrays, 2-level pointers -- deeper GEP/offset"),
]
# batch_sweep's ~80% recall under-reports exactly these profiles' bug class.
LOW_RECALL_ON_PRESCAN = {"ptr", "struct_byval"}

_CHILD_ENV = dict(os.environ)
_CHILD_ENV.setdefault("ASAN_OPTIONS", "detect_leaks=0")

_PRINT_LOCK = threading.Lock()


def _emit_factory(tag: str | None):
    """A line printer for one profile's live output.  With a tag (parallel
    mode) every line is prefixed `[tag]` so interleaved profiles stay
    attributable; the lock keeps concurrent lines from splicing mid-line."""
    pfx = f"  [{tag}] " if tag else "  "

    def emit(line: str) -> None:
        with _PRINT_LOCK:
            sys.stdout.write(pfx + line + "\n")
            sys.stdout.flush()
    return emit


def _stream(cmd: list[str], env: dict, emit) -> tuple[int | None, str]:
    """Run ``cmd``, tee its merged stdout/stderr live through ``emit`` (one call
    per line), and return (returncode, full_output).  rc is None on a launch
    failure (full_output then holds the error string)."""
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
        emit(line.rstrip("\n"))
    proc.wait()
    return proc.returncode, "".join(buf)


def _stream_child(cmd: list[str], env: dict, emit) -> tuple[int | None, str]:
    """Run ``cmd`` streaming its STDERR (progress) live through ``emit`` while
    capturing STDOUT (the parseable result) separately; returns (rc, stdout).
    The stderr pump runs on its own thread so neither pipe can back up and
    deadlock the child."""
    try:
        proc = subprocess.Popen(cmd, cwd=str(REPO_ROOT), env=env,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                text=True, bufsize=1)
    except Exception as e:                                  # pragma: no cover
        return None, f"failed to launch: {e}"

    def pump() -> None:
        assert proc.stderr is not None
        for line in proc.stderr:
            emit(line.rstrip("\n"))

    t = threading.Thread(target=pump, daemon=True)
    t.start()
    assert proc.stdout is not None
    out = proc.stdout.read()
    proc.wait()
    t.join()
    return proc.returncode, out


def run_olevels_prescan(profile: str, lo: int, hi: int, jobs: int, emit) -> tuple[list[int], str]:
    """Fast batch_sweep pre-scan; returns (divergent_seeds, error_or_empty).

    batch_sweep prints its native progress ("generated X/Y", "compiled X/Y",
    "-O0 done", "swept N — M divergent") to STDERR; we stream that through
    ``emit`` for live feedback while capturing STDOUT (the divergent seed list)
    for parsing.  ``-u`` keeps the child unbuffered so the progress appears
    promptly.
    """
    cmd = [sys.executable, "-u", str(BATCH_SWEEP), str(lo), str(hi),
           "--profile", profile, "--jobs", str(jobs),
           "--olevels=-O0,-O1,-O2"]     # drop -Os: the -Os-exclusive class is rare
    rc, out = _stream_child(cmd, _CHILD_ENV, emit)
    if rc is None:
        return [], f"batch_sweep failed to launch: {out}"
    if rc != 0:
        return [], f"batch_sweep rc={rc} (see streamed output above)"
    seeds = sorted(int(x) for x in out.split())
    return seeds, ""


def run_olevels_prescan_with_gcc(profile: str, lo: int, hi: int, jobs: int, emit) -> tuple[list[int], list[int], list[int], str]:
    """Merged batch_sweep pre-scan: links a per-seed `arm-none-eabi-gcc -O0`
    object into the SAME batched ELF as the tcc -O0/-O1/-O2 objects, so ONE
    qemu-boot-per-batch pass yields BOTH the olevels self-consistency verdict and
    the vs-gcc (O0-WRONG class) verdict — replacing a separate per-seed pytest
    vs-gcc pass for prescan mode.

    gcc-O0 is the reference (not gcc-O2): it is both cheaper to compile and the
    more trustworthy oracle — the known gcc wrong-code bugs are all at -O2
    (bitfield seed 1486 is a confirmed gcc -O2 miscompile of a UB-free program).
    With a single gcc level there is no automated self-consistency cross-check, so
    ``gccbad_seeds`` is always empty here (kept in the return signature for
    back-compat); a rare gcc-O0 miscompile would appear as a vs-gcc candidate and
    be dismissed in the triage/bisect step.  Returns (olevels_seeds, vsgcc_seeds,
    gccbad_seeds, error_or_empty).  Same ~80% recall caveat as run_olevels_prescan
    applies to the vs-gcc side too — this is a fast candidate-finder, not a
    certifying sweep (use --mode triage for that).
    """
    # No tcc -O0 here: for a gcc-oracle profile, gcc-O0 is the correctness
    # reference, so tcc -O0's only role (being the "known-good" level) is covered
    # — and gcc-O0 additionally backstops the class where tcc -O1 AND -O2 are both
    # wrong the SAME way (they'd agree with each other, but disagree with gcc ->
    # VSGCC).  We test both opt levels (-O1,-O2) against that reference; the
    # codegen-vs-optimization split that needs -O0 happens at triage time, which
    # re-runs full -O0/-O1/-O2/-Os on the few flagged seeds.
    cmd = [sys.executable, "-u", str(BATCH_SWEEP), str(lo), str(hi),
           "--profile", profile, "--jobs", str(jobs),
           "--olevels=-O1,-O2,gcc-O0"]
    rc, out = _stream_child(cmd, _CHILD_ENV, emit)
    if rc is None:
        return [], [], [], f"batch_sweep failed to launch: {out}"
    if rc != 0:
        return [], [], [], f"batch_sweep rc={rc} (see streamed output above)"
    ol_seeds: list[int] = []
    vg_seeds: list[int] = []
    gccbad_seeds: list[int] = []
    for line in out.splitlines():
        if line.startswith("OLEVELS"):
            ol_seeds = sorted(int(x) for x in line.split()[1:])
        elif line.startswith("VSGCC"):
            vg_seeds = sorted(int(x) for x in line.split()[1:])
        elif line.startswith("GCCBAD"):
            gccbad_seeds = sorted(int(x) for x in line.split()[1:])
    return ol_seeds, vg_seeds, gccbad_seeds, ""


def run_vsgcc(profile: str, lo: int, hi: int, jobs: int, emit) -> tuple[list[int], str]:
    """vs-gcc differential over [lo,hi]; returns (divergent_seeds, error_or_empty).

    Parses `seed<N>` out of pytest FAILED lines.  Skips cleanly if the gcc/QEMU
    reference runtime is not prepared.
    """
    env = dict(_CHILD_ENV, FUZZ_PROFILE=profile, FUZZ_VSGCC_SEEDS=f"{lo}-{hi}")
    cmd = [sys.executable, "-m", "pytest", str(VSGCC_TEST), "-q", "-p", "no:cacheprovider"]
    if jobs > 1:
        cmd += ["-n", str(jobs)]
    rc, out = _stream(cmd, env, emit)           # live-tee pytest's progress dots
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


def _triage_report_path(profile: str, lo: int, hi: int) -> Path:
    """Where triage_olevels.sh writes its per-seed culprit table — mirrors the
    OUT= naming in that script (the `int` profile omits its name)."""
    stem = f"fuzz_triage_{lo}_{hi}.md" if profile == "int" else f"fuzz_triage_{profile}_{lo}_{hi}.md"
    return REPO_ROOT / stem


def _parse_triage_report(path: Path) -> list[int]:
    """Extract the divergent seed ids from a triage_olevels.sh markdown table —
    the first column of each `| <seed> | ... |` row.  Header/separator rows have
    no leading integer so they're skipped."""
    if not path.exists():
        return []
    seeds = set()
    for line in path.read_text().splitlines():
        m = re.match(r"\|\s*(\d+)\s*\|", line)
        if m:
            seeds.add(int(m.group(1)))
    return sorted(seeds)


def run_olevels_triage_sweep(profile: str, lo: int, hi: int, jobs: int, emit) -> tuple[list[int], str]:
    """Full-recall olevels discovery for --mode triage.  Runs triage_olevels.sh
    over the whole band with NO SEEDS and NO FAST_SWEEP, so it does its exhaustive
    per-seed sweep_one (no batch_sweep, no ~80% recall gap) AND culprit-bisects the
    divergent seeds in one pass.  Returns (divergent_seeds, error_or_empty); the
    per-seed table is written to fuzz_triage_[<profile>_]<lo>_<hi>.md as a side
    effect (the script skips writing it when nothing diverges)."""
    env = dict(_CHILD_ENV, FUZZ_PROFILE=profile)   # no SEEDS / no FAST_SWEEP => full-recall sweep
    cmd = ["bash", str(TRIAGE_SH), str(lo), str(hi), str(jobs)]
    rc, out = _stream(cmd, env, emit)
    if rc is None:
        return [], "triage_olevels.sh " + out
    if rc != 0:
        return [], f"triage_olevels.sh rc={rc} (see streamed output above)"
    return _parse_triage_report(_triage_report_path(profile, lo, hi)), ""


def run_triage(profile: str, seeds: list[int], jobs: int, emit) -> str:
    """Exhaustive culprit-bisect of an explicit seed list via triage_olevels.sh.
    Returns the path of the markdown table it wrote (best-effort)."""
    if not seeds:
        return ""
    env = dict(_CHILD_ENV, FUZZ_PROFILE=profile, SEEDS=" ".join(str(s) for s in seeds))
    cmd = ["bash", str(TRIAGE_SH), "0", "0", str(jobs)]   # SEEDS overrides lo/hi
    _stream(cmd, env, emit)
    return f"fuzz_triage_{profile}_*.md (per triage_olevels.sh)"


def run_profile(idx: int, n_profiles: int, name: str, oracle: str, blurb: str,
                args, jobs: int, start: float, emit) -> dict:
    """Sweep ONE profile end-to-end (both oracles as configured) and return its
    report row + detail.  All live output goes through ``emit`` so concurrent
    profiles interleave line-by-line with attribution.  Returns a dict with
    keys: ol_cell, recall_note, vg_display, flagged, detail, elapsed."""
    t0 = time.monotonic()
    with _PRINT_LOCK:
        print(f"\n[{idx}/{n_profiles}] {name} — {blurb}   (t+{t0 - start:.0f}s)", flush=True)

    ol_seeds: list[int] = []
    ol_err = ""
    vg_cell = "—"
    vg_seeds: list[int] = []
    gcc_bad: list[int] = []   # gcc self-inconsistent (oracle-unreliable, quarantined)
    merge_gcc = (args.mode != "triage" and not args.olevels_only
                 and oracle in ("vsgcc", "both"))

    if merge_gcc:
        # Single batched pass: link a per-seed arm-none-eabi-gcc -O0 object into
        # the SAME runner ELF as the tcc -O0/-O1/-O2 objects, so one
        # qemu-boot-per-batch yields BOTH the olevels self-consistency verdict
        # and the vs-gcc (O0-WRONG class) verdict — no separate per-seed pytest
        # pass needed in prescan mode.  gcc-O0 is the reference (cheaper, and the
        # known gcc wrong-code bugs are all -O2); with one gcc level there is no
        # automated quarantine, so gcc_bad stays empty (a rare gcc-O0 miscompile
        # is dismissed by hand in triage).
        emit(f"olevels+gcc merged pre-scan [{args.lo}..{args.hi}] (one batch, both oracles) ...")
        ol_seeds, vg_seeds, gcc_bad, ol_err = run_olevels_prescan_with_gcc(
            name, args.lo, args.hi, jobs, emit)
        ol_cell = ol_err or str(len(ol_seeds))
        vg_cell = ol_err or str(len(vg_seeds))
        emit(f"olevels = {ol_cell}, vs-gcc = {vg_cell}"
             + (f", gcc-inconsistent (quarantined) = {len(gcc_bad)}" if gcc_bad else "")
             + f"  [{time.monotonic() - t0:.0f}s]"
             + (f"  -> olevels {ol_seeds[:20]}{' ...' if len(ol_seeds) > 20 else ''}" if ol_seeds else "")
             + (f"  vs-gcc {vg_seeds[:20]}{' ...' if len(vg_seeds) > 20 else ''}" if vg_seeds else "")
             + (f"  gcc-bad {gcc_bad[:20]}{' ...' if len(gcc_bad) > 20 else ''}" if gcc_bad else ""))
    else:
        # olevels discovery.  prescan = fast lossy batch_sweep; triage = the
        # exhaustive full-recall sweep_one in triage_olevels.sh (NO batch), which
        # also culprit-bisects every divergent seed in the same pass.
        if args.mode == "triage":
            emit(f"olevels full-recall sweep+triage [{args.lo}..{args.hi}] (no batch) ...")
            ol_seeds, ol_err = run_olevels_triage_sweep(name, args.lo, args.hi, jobs, emit)
        else:
            emit(f"olevels pre-scan [{args.lo}..{args.hi}] ...")
            ol_seeds, ol_err = run_olevels_prescan(name, args.lo, args.hi, jobs, emit)
        ol_cell = ol_err or str(len(ol_seeds))
        emit(f"olevels = {ol_cell}  [{time.monotonic() - t0:.0f}s]"
             + (f"  -> {ol_seeds[:20]}{' ...' if len(ol_seeds) > 20 else ''}" if ol_seeds else ""))

        if not args.olevels_only and oracle in ("vsgcc", "both"):
            t1 = time.monotonic()
            emit(f"vs-gcc sweep [{args.lo}..{args.hi}] ...")
            vg_seeds, vg_err = run_vsgcc(name, args.lo, args.hi, jobs, emit)
            vg_cell = vg_err or str(len(vg_seeds))
            emit(f"vs-gcc  = {vg_cell}  [{time.monotonic() - t1:.0f}s]"
                 + (f"  -> {vg_seeds[:20]}{' ...' if len(vg_seeds) > 20 else ''}" if vg_seeds else ""))

    # the ⚠low-recall caveat is a property of batch_sweep, so it applies to the
    # prescan pass ONLY — the triage sweep is full-recall by construction.
    recall_note = (" ⚠low-recall pre-scan"
                   if (args.mode != "triage" and name in LOW_RECALL_ON_PRESCAN and not ol_err)
                   else "")
    # gcc self-inconsistent seeds are NOT tcc bugs and are NOT listed as
    # findings (they'd otherwise get re-triaged as tcc divergences).  The
    # table cell keeps a compact count so the report still records that N
    # seeds were set aside; the specific seed ids are in the live sweep log.
    vg_display = vg_cell + (f" (+{len(gcc_bad)} gcc-bad quarantined)" if gcc_bad else "")

    detail: list[str] = []
    flagged = sorted(set(ol_seeds) | set(vg_seeds))
    if flagged:
        detail.append(f"\n## `{name}` — {len(flagged)} divergent seed(s)\n")
        detail.append("```\n" + " ".join(str(s) for s in flagged) + "\n```\n")
        if args.mode == "triage":
            # olevels-divergent seeds were already culprit-bisected by the
            # full-recall sweep above; triage only the vs-gcc-ONLY seeds (the
            # O0-WRONG/ABI class the olevels sweep can't see) to complete coverage.
            vg_only = sorted(set(vg_seeds) - set(ol_seeds))
            if vg_only:
                emit(f"triage: bisecting {len(vg_only)} vs-gcc-only seed(s) ...")
                run_triage(name, vg_only, jobs, emit)
            refs = []
            if ol_seeds:
                refs.append(f"`{_triage_report_path(name, args.lo, args.hi).name}` (olevels, full-recall)")
            if vg_only:
                refs.append(f"`{_triage_report_path(name, 0, 0).name}` (vs-gcc-only)")
            if refs:
                detail.append("Culprit bisect: see " + " and ".join(refs) + ".\n")
    emit(f"[{name} done in {time.monotonic() - t0:.0f}s · {len(flagged)} flagged]")
    return {"ol_cell": ol_cell, "recall_note": recall_note,
            "vg_display": vg_display, "flagged": flagged, "detail": detail}


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
    ap.add_argument("--parallel-profiles", type=int, default=3, metavar="N",
                    help="profiles swept concurrently; --jobs is split evenly "
                         "across them (default 3; 1 = sequential)")
    ap.add_argument("--out", default="", help="report path (default fuzz_triage_all_<lo>_<hi>.md)")
    args = ap.parse_args(argv)

    wanted = [p.strip() for p in args.profiles.split(",") if p.strip()]
    profiles = [t for t in PROFILES if (not wanted or t[0] in wanted)]
    if not profiles:
        sys.exit(f"no matching profiles in {args.profiles!r}; known: {[p[0] for p in PROFILES]}")

    n_par = max(1, min(args.parallel_profiles, len(profiles)))
    child_jobs = args.jobs if n_par == 1 else max(2, args.jobs // n_par)

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
          f"oracles=olevels{'' if args.olevels_only else '+vs-gcc'}"
          + (f" · {n_par} profiles at a time ({child_jobs} jobs each)" if n_par > 1 else ""))
    print("yield order:  " + "  >  ".join(p[0] for p in profiles), flush=True)

    # Sweep n_par profiles concurrently: one profile's qemu-latency-bound run
    # phase overlaps a sibling's CPU-bound compile phase.  Submission order is
    # yield order, so the richest seams still start first; the report rows are
    # assembled in yield order below no matter which profile finishes first.
    results: dict[str, dict] = {}
    with cf.ThreadPoolExecutor(max_workers=n_par) as ex:
        futs = {ex.submit(run_profile, i, len(profiles), name, oracle, blurb,
                          args, child_jobs, start,
                          _emit_factory(name if n_par > 1 else None)): name
                for i, (name, oracle, blurb) in enumerate(profiles, 1)}
        for fut in cf.as_completed(futs):
            name = futs[fut]
            try:
                results[name] = fut.result()
            except Exception as e:                          # pragma: no cover
                results[name] = {"ol_cell": f"error: {e}", "recall_note": "",
                                 "vg_display": "—", "flagged": [], "detail": []}

    for name, oracle, blurb in profiles:
        r = results[name]
        lines.append(f"| `{name}` | {r['ol_cell']}{r['recall_note']} | {r['vg_display']} | {blurb} |")
        if r["flagged"]:
            n_diverged += 1
        detail.extend(r["detail"])

    lines.append("")
    if args.mode == "triage":
        lines.append("> ✓ triage mode: olevels counts are full-recall exhaustive sweeps "
                     "(no batch_sweep); every divergent seed is culprit-bisected.")
        lines.append("")
    elif any(p in LOW_RECALL_ON_PRESCAN for p, _, _ in profiles):
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
