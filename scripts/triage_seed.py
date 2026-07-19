#!/usr/bin/env python3
"""One-command per-seed triage collector for the fuzz-divergence playbook.

Given a sweep-report entry like ``longlong 3161`` this script recollects, in
one run, every artifact the per-bug investigation loop in
``docs/debugging_fuzz_divergences.md`` starts from:

  <out>/
    seed.c             the generated program (gen_c.py --profile <suite> --seed N)
    outputs.txt        tcc signatures at every O-level -- FULL stdout, so a
                       HardFault keeps its PC=/CFSR=/BFAR= register dump
    gcc_reference.txt  arm-none-eabi-gcc -O2 ground truth (the trusted oracle)
    reduced.c          line-granularity reduction that preserves the divergence
    bisect.txt         scripts/bisect_opt.py Phase A/B/C output (reduced repro)
    crash_disasm.txt   (crash signatures only) force-thumb disassembly window
                       around the faulting PC of the divergent tcc ELF
    SUMMARY.md         one-page digest of all of the above

The sweep reports (fuzz_triage_*.md) list seeds PER SUITE/PROFILE: ``ptr 5759``
is seed 5759 of gen_c.py's ``ptr`` profile, which is NOT the program that
``diff_olevels.py --seed 5759`` (default profile) generates.  This script owns
that mapping so nobody has to re-derive it.

Usage:
    python3 scripts/triage_seed.py --suite longlong --seed 3161
    python3 scripts/triage_seed.py --suite ptr --seed 5759 --olevels -O0,-O2
    python3 scripts/triage_seed.py --file repro.c            # existing repro
    python3 scripts/triage_seed.py --suite ptr --seed 5759 --skip-reduce

Exit code: 0 = consistent (nothing to triage), 1 = divergence collected,
2 = harness/infra error.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

from sources.fuzz_common import (
    REPO_ROOT,
    H,
    CompileConfig,
    compile_testcase,
    MACHINE,
    generate_program,
    PROFILES,
)

DEFAULT_OPT_LEVELS = ["-O0", "-O1", "-O2", "-Os"]
OBJDUMP = "arm-none-eabi-objdump"


def log(msg: str) -> None:
    print(f"[triage_seed] {msg}", flush=True)


def run_tcc_keep_elf(source: Path, opt_level: str, out_dir: Path):
    """Like fuzz_harness.run_with_tcc, but also return the ELF path so a crash
    signature can be disassembled afterwards."""
    label = f"tcc{opt_level}"
    out_dir.mkdir(parents=True, exist_ok=True)
    suffix = "_" + opt_level.replace("-", "").replace(" ", "_")
    config = CompileConfig(
        extra_cflags=opt_level,
        output_dir=out_dir,
        output_suffix=suffix,
        clean_before_build=False,
    )
    result = compile_testcase([Path(source)], MACHINE, config=config)
    if not result.success:
        return (H.RunResult(label, False, "", None,
                            error="tcc compile failed: " + (result.error or "").strip()),
                None)
    return H._run_elf(result.elf_file, label), Path(result.elf_file)


def crash_pc(stdout: str):
    """Extract the stacked PC from a HardFault register dump, if present."""
    m = re.search(r"PC=0x([0-9A-Fa-f]+)", stdout)
    return int(m.group(1), 16) if m else None


def disassemble_window(elf: Path, pc: int, before: int = 0x80, after: int = 0x40) -> str:
    """force-thumb disassembly window around the faulting PC.  A HardFault PC
    inside what objdump renders as garbage/data usually means execution fell
    into a literal pool or jump table -- exactly the layout-bug signature."""
    cmd = [OBJDUMP, "-d", "-M", "force-thumb",
           f"--start-address={max(pc - before, 0):#x}",
           f"--stop-address={pc + after:#x}", str(elf)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    header = f"$ {' '.join(cmd)}\n(faulting PC: {pc:#x})\n\n"
    return header + (r.stdout or r.stderr)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--suite", "--profile", dest="suite", type=str, default=None,
                    choices=sorted(PROFILES.keys()),
                    help="gen_c.py profile the seed belongs to (sweep-report section name)")
    ap.add_argument("--seed", type=int, default=None, help="seed within --suite")
    ap.add_argument("--file", type=str, default=None,
                    help="triage an existing .c repro instead of generating one")
    ap.add_argument("--olevels", type=str, default=",".join(DEFAULT_OPT_LEVELS),
                    help=f"comma-separated opt levels (default {','.join(DEFAULT_OPT_LEVELS)})")
    ap.add_argument("--out", type=str, default=None,
                    help="artifact dir (default tests/fuzz/results/triage/<suite>_<seed>)")
    ap.add_argument("--skip-reduce", action="store_true",
                    help="skip reduce_divergence.py (bisect runs on the full repro)")
    ap.add_argument("--skip-bisect", action="store_true",
                    help="skip bisect_opt.py")
    args = ap.parse_args(argv)

    if args.file is None and (args.suite is None or args.seed is None):
        ap.error("either --suite AND --seed, or --file is required")

    usable, reason = H.qemu_available()
    if not usable:
        log(f"QEMU/newlib not usable: {reason}")
        return 2

    opt_levels = [o.strip() for o in args.olevels.split(",") if o.strip()]
    if "-O0" not in opt_levels:
        opt_levels.insert(0, "-O0")  # -O0 is the trusted self-consistency oracle

    tag = f"{args.suite}_{args.seed}" if args.file is None else Path(args.file).stem
    out_dir = Path(args.out) if args.out else REPO_ROOT / "tests" / "fuzz" / "results" / "triage" / tag
    out_dir.mkdir(parents=True, exist_ok=True)
    build_dir = out_dir / "_build"

    # -- 1. the program -----------------------------------------------------
    source = out_dir / "seed.c"
    if args.file is not None:
        source.write_text(Path(args.file).read_text())
        log(f"copied repro {args.file} -> {source}")
    else:
        source.write_text(generate_program(args.seed, profile=args.suite))
        log(f"generated {args.suite} seed {args.seed} -> {source}")

    # -- 2. tcc signatures at every O-level (full output, incl. fault dumps) -
    results = {}
    elfs = {}
    for o in opt_levels:
        res, elf = run_tcc_keep_elf(source, o, build_dir)
        results[o], elfs[o] = res, elf
        log(f"{res.label}: exit={res.exit_code} stdout={res.stdout.strip()!r}"
            + (f" err={res.error}" if res.error else ""))
    outputs = [f"[{results[o].label}] ok={results[o].ok} exit={results[o].exit_code}\n"
               f"{results[o].stdout.rstrip()}\n" for o in opt_levels]
    (out_dir / "outputs.txt").write_text("\n".join(outputs))

    ref_sig = results["-O0"].signature
    divergent = [o for o in opt_levels
                 if o != "-O0" and (not results[o].ok or results[o].signature != ref_sig)]

    # -- 3. gcc ground truth (must equal tcc -O0) ---------------------------
    gcc_line = "unavailable"
    gcc_ok, gcc_reason = H.gcc_reference_available()
    if gcc_ok:
        gcc_res = H.run_with_gcc(source, "-O2", out_dir / "_gccbuild")
        gcc_line = f"exit={gcc_res.exit_code} stdout={gcc_res.stdout.strip()!r}"
        (out_dir / "gcc_reference.txt").write_text(
            f"[{gcc_res.label}] ok={gcc_res.ok} {gcc_line}\n")
        log(f"gcc -O2 reference: {gcc_line}")
        if gcc_res.ok and gcc_res.signature != ref_sig:
            log("WARNING: gcc -O2 disagrees with tcc -O0 -- one oracle is "
                "miscompiling; cross-check before trusting either "
                "(see the gcc-bad quarantine cases in the sweep reports)")
    else:
        log(f"gcc reference skipped: {gcc_reason}")

    if not divergent:
        (out_dir / "SUMMARY.md").write_text(
            f"# {tag}: CONSISTENT\n\nAll of {', '.join(opt_levels)} produced "
            f"{ref_sig!r}; gcc -O2: {gcc_line}.\nNothing to triage.\n")
        log(f"CONSISTENT across {','.join(opt_levels)} -- nothing to triage")
        return 0

    high = divergent[0]
    log(f"DIVERGENT at {','.join(divergent)}; using --high={high}")

    # -- 4. crash disassembly (before reduce: layout bugs die under reduction
    #       of a DIFFERENT kind, and the full seed is what actually faulted) --
    pc = crash_pc(results[high].stdout) if results[high].ok else None
    if pc is not None and elfs[high] is not None:
        (out_dir / "crash_disasm.txt").write_text(disassemble_window(elfs[high], pc))
        log(f"crash at PC={pc:#x}: disassembly window -> crash_disasm.txt")

    # -- 5. reduce ----------------------------------------------------------
    reduced = out_dir / "reduced.c"
    bisect_input = source
    if args.skip_reduce:
        log("reduction skipped (--skip-reduce)")
    else:
        log(f"reducing (low=-O0 high={high}) ... this can take a few minutes")
        # Per-seed scratch dir: reduce_divergence.py otherwise defaults to the
        # SHARED tests/fuzz/results/_reduce, so two triages running at once
        # overwrite each other's candidate.c and the oracle answers about the
        # sibling's program -- which "reduces" into a file that does not even
        # compile.
        r = subprocess.run(
            [sys.executable, str(REPO_ROOT / "scripts" / "reduce_divergence.py"),
             str(source), f"--low=-O0", f"--high={high}", "-o", str(reduced),
             "--work-dir", str(out_dir / "_reduce")],
            capture_output=True, text=True)
        if r.returncode == 0 and reduced.exists():
            bisect_input = reduced
            log(f"reduced -> {reduced} ({sum(1 for _ in open(reduced))} lines)")
        else:
            log(f"reduction failed (rc={r.returncode}); bisecting the full seed\n"
                + (r.stderr or r.stdout).strip())

    # -- 6. bisect (Phase A knobs / Phase B folds / Phase C final-IR diff) ---
    culprits = "not run"
    if args.skip_bisect:
        log("bisect skipped (--skip-bisect)")
    else:
        log(f"bisecting {bisect_input.name} at {high} ...")
        # Same sharing hazard as the reduce step above: bisect_opt.py defaults
        # to the shared tests/fuzz/results/_bisect, and every triage feeds it a
        # file named reduced.c, so concurrent runs collide on one ELF name.
        r = subprocess.run(
            [sys.executable, str(REPO_ROOT / "scripts" / "bisect_opt.py"),
             "--file", str(bisect_input), f"--high={high}",
             "--work-dir", str(out_dir / "_bisect")],
            capture_output=True, text=True)
        (out_dir / "bisect.txt").write_text(r.stdout + r.stderr)
        m = re.search(r"Culprit knob\(s\).*?:\s*(.*)", r.stdout)
        culprits = m.group(1).strip() if m else "none found (see bisect.txt)"
        log(f"culprit knob(s): {culprits}")

    # -- 7. summary ----------------------------------------------------------
    lines = [f"# Triage data: {tag}", ""]
    if args.file is None:
        lines.append(f"Suite/profile: `{args.suite}`  seed: `{args.seed}`")
    lines += [
        "",
        "| level | exit | output |",
        "|---|---|---|",
        *[f"| `tcc {o}` | {results[o].exit_code} | `{results[o].stdout.strip()!r}` |"
          for o in opt_levels],
        f"| `gcc -O2` (oracle) | | `{gcc_line}` |",
        "",
        f"Divergent level(s): **{', '.join(divergent)}**",
        f"Culprit knob(s) [Phase A]: **{culprits}**",
    ]
    if pc is not None:
        lines += [
            f"Crash: faulting PC `{pc:#x}` -- see `crash_disasm.txt`.",
            "",
            "> Many unrelated \"fixing\" knobs + a wild PC/BFAR usually means a",
            "> layout-sensitive BACKEND bug (literal pool / IT block / branch",
            "> range), not an IR misfold: read `crash_disasm.txt` around the PC",
            "> first (is it inside pool data? right after an IT block?).",
        ]
    lines += [
        "",
        "Artifacts: `seed.c`, `outputs.txt`, `gcc_reference.txt`, "
        "`reduced.c`, `bisect.txt`"
        + (", `crash_disasm.txt`" if pc is not None else "") + ".",
        "",
        "Next steps: docs/debugging_fuzz_divergences.md sections 3-5 "
        "(read the IR, write the regression test FIRST, then fix).",
    ]
    (out_dir / "SUMMARY.md").write_text("\n".join(lines) + "\n")
    log(f"summary -> {out_dir / 'SUMMARY.md'}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
