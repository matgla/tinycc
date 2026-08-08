#!/usr/bin/env python3
"""Pinpoint the optimization pass / knob that flips a program's output.

Given a seed (or a .c file) that diverges between two -O levels (e.g. tcc -O0
correct, tcc -O1 wrong), this script tells you *exactly* what to look at:

  Phase A -- knob bisection (QEMU-confirmed, exact):
      For every optimization knob ``-f<knob>`` known to the compiler, rebuild at
      the failing level with ``-fno-<knob>`` and re-run under QEMU.  Any knob
      whose removal restores the reference signature is reported as a culprit.

  Phase B -- pass text-diff (narrows to the specific pass + IR line):
      Dumps the IR after every optimization pass (``-dump-ir-passes=all``) at the
      failing level, walks consecutive pass outputs, and flags the pass where a
      memory read (LOAD / LOAD_INDEXED / ``***DEREF***``) at a given instruction
      address turns into a constant ``#...`` -- the classic misfold signature.
      Each flagged pass is correlated (via source/opt/engine/pipeline_table.c) to its gating knob
      and printed with the before/after IR lines.

The two phases cross-check: Phase A names the culprit knob(s); Phase B names the
specific pass and the exact transformation, filtered to the culprit knob set so
the noise from unrelated constant folds is suppressed.

This reuses tests/fuzz/fuzz_harness.py (QEMU + newlib plumbing).

Usage:
    python scripts/bisect_opt.py --seed 295
    python scripts/bisect_opt.py --seed 295 --low -O0 --high -O2
    python scripts/bisect_opt.py --file tests/fuzz/fuzz_triage_repros/seed295.c
    python scripts/bisect_opt.py --file path.c --high -O1 --skip-knobs   # IR only

Exit code: 0 if a culprit was identified, 1 otherwise.
"""

from __future__ import annotations

import argparse
import re
import shlex
import subprocess
import sys
from pathlib import Path

from sources.fuzz_common import FUZZ_DIR, REPO_ROOT, H, generate_program

IR_TESTS_DIR = H.IR_TESTS_DIR
TCC = H.TCC_BIN

# Include flags mirrored from tests/fuzz/runseed.sh so a direct armv8m-tcc
# invocation compiles the same program the Makefile-driven path does.
INC_FLAGS = [
    f"-I{IR_TESTS_DIR / 'libc_includes'}",
    f"-I{IR_TESTS_DIR / 'libc_imports'}",
    f"-I{IR_TESTS_DIR / 'libc_includes' / 'newlib'}",
    "-I/include",
    f"-I{REPO_ROOT / 'include'}",
]
BASE_TCC_FLAGS = [
    "-nostdlib", "-fvisibility=hidden", "-mcpu=cortex-m33", "-mthumb",
    "-mfloat-abi=soft", "-ffunction-sections",
]


# ---------------------------------------------------------------------------
# Static introspection of the compiler's knob / pass tables
# ---------------------------------------------------------------------------

def _parse_knobs() -> list[str]:
    """Extract the list of -f<knob> optimization flags from libtcc.c."""
    src = (REPO_ROOT / "source" / "driver" / "libtcc.c").read_text()
    return sorted(set(re.findall(r'offsetof\(TCCState, (opt_[a-z_]+)\), 0, "([a-z-]+)"', src)),
                  key=lambda t: t[1])


def _parse_pass_to_knob() -> dict[str, str]:
    """Map individual pass name -> knob name from the PASS_GATED table.

    Reads the ``PASS_GATED("name", ..., FLAG(opt_X))`` entries in
    source/opt/engine/pipeline_table.c.  Note: the per-pass IR dump labels group-level phases
    (e.g. ``entry_store_group``, ``propagation_group``) that aggregate several
    such passes, so a dump label often does NOT appear in this map.  Use
    :func:`_parse_group_labels` to recognise group labels, and
    :func:`_passes_for_knob` to list the individual passes a knob gates.
    """
    src = (REPO_ROOT / "source" / "opt" / "engine" / "pipeline_table.c").read_text()
    out: dict[str, str] = {}
    for m in re.finditer(r'PASS_GATED\(\s*"([^"]+)"[^)]*?FLAG\(opt_([a-z_]+)\)', src):
        out.setdefault(m.group(1), m.group(2))
    return out


def _parse_group_labels() -> set[str]:
    """Return the set of IRPassGroup variable names (dump labels that are
    groups rather than individual passes)."""
    src = (REPO_ROOT / "source" / "opt" / "engine" / "pipeline_table.c").read_text()
    return set(re.findall(r'IRPassGroup\s+(\w+)\s*=', src))


def _passes_for_knob(pass2knob: dict[str, str], knob: str) -> list[str]:
    """Individual pass names gated by ``knob`` (a flag name, e.g. 'store-load-fwd')."""
    field = knob.replace("-", "_")
    return sorted(p for p, k in pass2knob.items() if k == field)


# ---------------------------------------------------------------------------
# Final-IR diff between ``high`` and ``high -fno-<knob>`` (the general fallback)
# ---------------------------------------------------------------------------

def _dump_final_ir(source: Path, opt_level: str) -> str:
    """Return the final optimized IR text (``-dump-ir``) for ``source``.

    Each function is delimited by an ``=== IR AFTER OPTIMIZATIONS ===`` block;
    we keep all of them so a multi-function program diffs cleanly.
    """
    cmd = [str(TCC), "-dump-ir", *shlex.split(opt_level), *BASE_TCC_FLAGS, *INC_FLAGS,
           "-c", str(source), "-o", "/dev/null"]
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if "=== IR AFTER OPTIMIZATIONS ===" not in (proc.stdout or ""):
        raise RuntimeError(f"tcc dump failed:\n{proc.stderr.strip()}")
    return proc.stdout


# Array-initializer stores: ``StackLoc[-NN] <-- #const [STORE]`` for |NN|>=32.
# These dominate the diff with pure noise (the program's literal initializers),
# so strip them when comparing two opt variants of the same program.
_INIT_STORE_RE = re.compile(r"StackLoc\[-\d+\] <-- #")


def _filter_final_ir(text: str) -> list[str]:
    """Keep instruction lines, dropping section markers and array-init stores."""
    out = []
    for ln in text.splitlines():
        if ln.startswith("=== ") or _INIT_STORE_RE.search(ln):
            continue
        out.append(ln)
    return out


def diff_knob(source: Path, high: str, knob: str) -> int:
    """Print a unified diff of final IR: ``high``  vs  ``high -fno-<knob>``.

    This is the general-purpose fallback that catches ANY class of miscompile
    (const folds, dropped stores, control-flow rewrites) -- not just the
    memory->constant folds Phase B heuristics flag.  The knob must be one that
    Phase A found to fix the divergence, so the two IRs differ exactly in what
    that pass changes.
    """
    print(f"\n[bisect] Phase C: final-IR diff  ({high})  vs  ({high} -fno-{knob})")
    try:
        a = _filter_final_ir(_dump_final_ir(source, high))
        b = _filter_final_ir(_dump_final_ir(source, f"{high} -fno-{knob}"))
    except RuntimeError as e:
        print(f"[bisect] could not dump final IR: {e}", file=sys.stderr)
        return 0
    import difflib
    ndiff = 0
    for line in difflib.unified_diff(a, b,
                                     fromfile=f"{high} (buggy)",
                                     tofile=f"{high} -fno-{knob} (correct)",
                                     lineterm=""):
        print(line)
        if line[:1] in ("+", "-") and line[:2] not in ("++", "--"):
            ndiff += 1
    if ndiff == 0:
        print("[bisect] (no differences -- knob did not change final IR)")
    return ndiff


# ---------------------------------------------------------------------------
# IR pass-dump parsing
# ---------------------------------------------------------------------------

_PASS_HDR = re.compile(r"^=== AFTER (.+?) ===$")
_PASS_END = re.compile(r"^=== END AFTER .+? ===$")


def dump_passes(source: Path, opt_level: str) -> list[tuple[str, list[str]]]:
    """Return [(pass_name, [ir_lines]), ...] in document order for ``source``."""
    cmd = [str(TCC), "-dump-ir-passes=all", *shlex.split(opt_level), *BASE_TCC_FLAGS, *INC_FLAGS,
           "-c", str(source), "-o", "/dev/null"]
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    # Compile errors surface on stderr and produce no dump -- bubble them up.
    if proc.returncode != 0 or "=== AFTER" not in (proc.stdout or ""):
        raise RuntimeError(f"tcc dump failed:\n{proc.stderr.strip()}")
    blocks: list[tuple[str, list[str]]] = []
    cur_name, cur_lines = None, None
    for ln in (proc.stdout or "").splitlines():
        hm = _PASS_HDR.match(ln)
        if hm:
            cur_name, cur_lines = hm.group(1), []
            continue
        if _PASS_END.match(ln):
            if cur_name is not None:
                blocks.append((cur_name, cur_lines))
            cur_name, cur_lines = None, None
            continue
        if cur_name is not None:
            cur_lines.append(ln)
    return blocks


_ADDR_RE = re.compile(r"^\s*(\d+):\s*(.*)$")
# A memory read at a given instruction address: a load through a pointer
# (***DEREF***), a plain [LOAD], or a LOAD_INDEXED op.
_MEM_READ_RE = re.compile(r"(LOAD_INDEXED|\*\*\*DEREF\*\*\*|\[LOAD\])")
_CONST_ASSIGN_RE = re.compile(r"<--\s*#-?[0-9a-fA-Fx]+\b")


def _index_by_addr(lines: list[str]) -> dict[str, str]:
    """Map instruction-address -> normalized RHS text for an IR block."""
    idx: dict[str, str] = {}
    for ln in lines:
        m = _ADDR_RE.match(ln)
        if not m:
            continue
        addr, rhs = m.group(1), m.group(2)
        idx[addr] = rhs
    return idx


def find_const_folds(blocks: list[tuple[str, list[str]]]) -> list[dict]:
    """Find passes that turned a memory read into a constant at the same addr.

    Returns a list of dicts: {pass, addr, before_line, after_line}.  Operates on
    consecutive block pairs in document order; pipeline restarts (new function)
    produce totally different addr sets and are naturally ignored because no
    shared addr is both a mem-read and a const-assign.
    """
    findings: list[dict] = []
    for (name_a, lines_a), (name_b, lines_b) in zip(blocks, blocks[1:]):
        ia, ib = _index_by_addr(lines_a), _index_by_addr(lines_b)
        for addr, rhs_b in ib.items():
            rhs_a = ia.get(addr)
            if rhs_a is None:
                continue
            was_mem = bool(_MEM_READ_RE.search(rhs_a))
            now_const = bool(_CONST_ASSIGN_RE.search(rhs_b)) and ("[LOAD]" in rhs_b or "[ASSIGN]" in rhs_b)
            # The interesting transition: was a real memory read, now a constant.
            if was_mem and now_const and not _MEM_READ_RE.search(rhs_b):
                findings.append({
                    "pass": name_b, "addr": addr,
                    "before": f"{addr}: {rhs_a}", "after": f"{addr}: {rhs_b}",
                })
        # previous block for next iteration
        del ia, ib
    return findings


# ---------------------------------------------------------------------------
# Phases
# ---------------------------------------------------------------------------

def phase_knobs(source: Path, low: str, high: str, work_dir: Path) -> list[str]:
    """Phase A: which -fno-<knob> flags at ``high`` restore the ``low`` signature."""
    ref = H.run_with_tcc(source, low, work_dir)
    if not ref.ok:
        print(f"[bisect] reference level {low} did not produce output: {ref.error}", file=sys.stderr)
        return []
    ref_sig = ref.signature
    print(f"[bisect] reference {low} signature = {ref_sig[0]!r}/{ref_sig[1]}")

    bad = H.run_with_tcc(source, high, work_dir)
    if bad.ok and bad.signature == ref_sig:
        print(f"[bisect] {high} already matches {low} -- nothing to bisect.")
        return []
    if bad.ok:
        print(f"[bisect] {high} signature = {bad.signature[0]!r}/{bad.signature[1]} (DIVERGENT)")
    else:
        print(f"[bisect] {high} failed to build/run: {bad.error}", file=sys.stderr)

    knobs = _parse_knobs()
    print(f"[bisect] Phase A: testing {len(knobs)} knobs under QEMU ...")
    fixes: list[str] = []
    for i, (opt_field, flag) in enumerate(knobs, 1):
        cflags = f"{high} -fno-{flag}"
        r = H.run_with_tcc(source, cflags, work_dir)
        restored = r.ok and r.signature == ref_sig
        tag = "FIXES" if restored else "      "
        if restored:
            fixes.append(flag)
        print(f"  [{i:2d}/{len(knobs)}] {tag}  -fno-{flag:<22} -> "
              f"{r.signature[0]!r}/{r.signature[1]}" + ("" if r.ok else " (build/run fail)"))
    return fixes


def phase_passes(source: Path, high: str, culprit_knobs: list[str]) -> int:
    """Phase B: dump passes, find memory->constant folds, correlate with knobs.
    Returns the number of folds surfaced (after culprit filtering)."""
    pass2knob = _parse_pass_to_knob()
    group_labels = _parse_group_labels()
    try:
        blocks = dump_passes(source, high)
    except RuntimeError as e:
        print(f"[bisect] could not dump IR passes: {e}", file=sys.stderr)
        return 0
    print(f"\n[bisect] Phase B: {len(blocks)} pass blocks dumped at {high}; "
          f"scanning for memory->constant folds ...")
    folds = find_const_folds(blocks)
    if not folds:
        print("[bisect] no memory->constant folds detected between consecutive passes.")
        return 0

    def label_knob(label: str) -> str:
        if label in pass2knob:
            return pass2knob[label][len("opt_"):]
        if label in group_labels:
            return "<group>"
        return "?"

    culprit_set = set(culprit_knobs)
    shown = 0
    seen_passes: set[str] = set()
    for f in folds:
        knob = label_knob(f["pass"])
        seen_passes.add(f["pass"])
        # When we have culprit knobs, only surface folds whose pass is gated by
        # one of them; otherwise show everything.  Group labels aggregate many
        # passes, so we always show them (the LLM greps the label in ir/).
        is_suspect = (knob in culprit_set) or (f["pass"] in group_labels) if culprit_set else True
        if is_suspect:
            tag = f" (knob={knob})" + ("  <<" if knob in culprit_set else "")
            print(f"  pass={f['pass']:<24}{tag}")
            print(f"    BEFORE: {f['before']}")
            print(f"    AFTER : {f['after']}")
            shown += 1
    print(f"\n[bisect] passes introducing folds: {sorted(seen_passes)}")

    if culprit_set:
        print("\n[bisect] individual passes gated by each culprit knob "
              "(functions to inspect in ir/opt_*.c):")
        for knob in culprit_knobs:
            plist = _passes_for_knob(pass2knob, knob)
            print(f"  -fno-{knob}: {plist}")
    return shown


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--seed", type=int, help="gen_c.py seed to (re)generate")
    g.add_argument("--file", type=str, help="existing .c file to bisect")
    ap.add_argument("--low", default="-O0", help="reference (correct) opt level")
    ap.add_argument("--high", default="-O1", help="divergent opt level")
    ap.add_argument("--work-dir", type=str, default=None)
    ap.add_argument("--skip-knobs", action="store_true",
                    help="skip Phase A (QEMU knob sweep); do IR-only Phase B")
    ap.add_argument("--diff-knob", type=str, default=None,
                    help="run only Phase C: diff final IR at <high> vs <high> -fno-<knob>")
    ap.add_argument("--require-qemu", action="store_true")
    args = ap.parse_args(argv)

    usable, reason = H.qemu_available()
    if not usable and not args.skip_knobs and not args.diff_knob:
        print(f"[bisect] QEMU/newlib not usable: {reason}", file=sys.stderr)
        return 1 if args.require_qemu else 2

    work_dir = Path(args.work_dir) if args.work_dir else (FUZZ_DIR / "results" / "_bisect")
    work_dir.mkdir(parents=True, exist_ok=True)

    if args.file:
        source = Path(args.file)
    else:
        seed = args.seed if args.seed is not None else 295
        source = work_dir / f"fuzz_{seed}.c"
        source.write_text(generate_program(seed))

    print(f"[bisect] source: {source}")
    print(f"[bisect] low={args.low}  high={args.high}")

    # Phase C standalone: diff final IR for a single knob and exit.
    if args.diff_knob:
        diff_knob(source, args.high, args.diff_knob)
        return 0

    culprit = []
    if not args.skip_knobs:
        culprit = phase_knobs(source, args.low, args.high, work_dir)
        if culprit:
            print(f"\n[bisect] >> Culprit knob(s) [QEMU-confirmed]: {culprit}")
        else:
            print("\n[bisect] no single -fno-<knob> restored the reference.")

    folds_shown = phase_passes(source, args.high, culprit)

    # Phase C: when a culprit knob is known, always show the exact final-IR
    # delta it induces.  This is the general fallback that catches bugs Phase B's
    # fold heuristic misses (dropped stores, control-flow rewrites).  Prefer the
    # knob least likely to be a mere propagator (store-load-fwd/jump-threading
    # over const-prop, which gates the most passes).
    if culprit:
        prefer = [k for k in ("jump-threading", "store-load-fwd", "loop-unroll",
                               "dead-store-elim", "disp-fusion") if k in culprit]
        chosen = prefer[0] if prefer else culprit[0]
        diff_knob(source, args.high, chosen)

    print("\n[bisect] next steps:")
    print("  1. read the BEFORE/AFTER fold line (Phase B) and/or the final-IR diff (Phase C)")
    print("  2. open the implicated pass function in ir/opt_*.c (see docs/debugging_fuzz_divergences.md)")
    print("  3. add a reduced regression test in tests/ir_tests/ before fixing")
    return 0 if (culprit or args.skip_knobs) else 1


if __name__ == "__main__":
    raise SystemExit(main())
