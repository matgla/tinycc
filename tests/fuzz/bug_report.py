#!/usr/bin/env python3
"""bug_report.py — auto-file a docs/bugs report for a confirmed fuzz divergence.

`triage_olevels.sh` confirms a divergent seed (per-level checksums against the
gcc ground truth, a class, and a culprit knob) but used to leave the
bug-tracking step to whoever read fuzz_triage_*.md next.  This helper closes
that gap: it writes `docs/bugs/fuzz_<profile>_seed<N>.md` following the
standard report format (docs/bugs/README.md), freezes the seed's generated
source as `docs/bugs/repro/fuzz_<profile>_seed<N>.c`, and links both the copy
and the regen command from the Reproducer section.  The copy matters because
the source is only deterministic in (gen_c.py, profile, seed) — freezing it
keeps the report valid when the generator evolves; the seed still lets anyone
regenerate and re-diff.

Dedup is FILENAME-BASED: when the report .md already exists, the helper does
nothing (per the README workflow, the fixing session deletes the report — a
re-sweep that still finds the seed must not file a duplicate).  Creation is
exclusive (`open(..., "x")`), so the parallel triage workers under `xargs -P`
cannot double-file the same seed.

Usage (as triage_olevels.sh invokes it):

    python3 tests/fuzz/bug_report.py --profile int --seed 172 --class O2 \\
        --ref 1e63d9aa --o0 1e63d9aa --o1 1e63d9aa --o2 a45f9daa \\
        --os a45f9daa --culprit="-fno-cse" \\
        --src tests/fuzz/fuzz_triage_repros/seed172.c \\
        --via "triage_olevels.sh 0-4999"

(note the `--culprit=` form: culprit knobs start with `-`, which argparse's
space form would take for an option.)

Prints `filed: <path>` or `exists: <path> (skipped)`; exit code is 0 in both
cases (a duplicate is not an error).  `$FUZZ_BUGS_DIR` / `--bugs-dir` override
the destination (default `<repo>/docs/bugs`).
"""
from __future__ import annotations

import argparse
import datetime
import os
import shutil
import sys
from pathlib import Path

THIS_DIR = Path(__file__).resolve().parent
ROOT = THIS_DIR.parent.parent                  # libs/tinycc

# Class -> (title phrase, severity).  A "/CRASH" suffix on the class (the seed
# hard-faults or locks up at the divergent level) is handled separately.
_CLASS_INFO = {
    "O0-WRONG":     ("all tcc O-levels agree with each other but disagree "
                     "with the gcc reference", "correctness (miscompile)"),
    "O1":           ("tcc -O1 miscompile", "correctness (miscompile)"),
    "O2":           ("tcc -O2 miscompile", "correctness (miscompile)"),
    "Os":           ("tcc -Os miscompile", "correctness (miscompile)"),
    "COMPILE_CRASH": ("tcc fails to compile the seed at an optimizing level",
                      "correctness (compile failure)"),
    "TCC-ASAN":     ("the ASan-built tcc trips AddressSanitizer while "
                     "compiling the seed", "correctness (sanitizer trip in tcc itself)"),
    "TCC-LSAN":     ("the ASan-built tcc trips LeakSanitizer while compiling "
                     "the seed", "correctness (sanitizer trip in tcc itself)"),
    "?":            ("unclassified O-level divergence", "correctness (unclassified)"),
}


def _classify(cls: str) -> tuple[str, str]:
    """Map a triage class to (title phrase, severity), honouring /CRASH."""
    crash = cls.endswith("/CRASH")
    base = cls[: -len("/CRASH")] if crash else cls
    title, severity = _CLASS_INFO.get(base, (base, "correctness (unclassified)"))
    if crash:
        title += " and crashes (HardFault/Lockup) at that level"
        severity += " + guest crash"
    return title, severity


def report_slug(profile: str, seed: int) -> str:
    """The dedup key: stable in (profile, seed) — no date, so the same bug
    found by a later sweep maps to the same filename."""
    return f"fuzz_{profile}_seed{seed}"


def _render(*, profile: str, seed: int, cls: str, ref: str, o0: str, o1: str,
            o2: str, os_res: str, culprit: str, repro_name: str | None,
            via: str, date: str) -> str:
    title, severity = _classify(cls)
    lines = [
        f"# Fuzz `{profile}` seed {seed}: {title}",
        "",
        f"**Status:** open · **Severity:** {severity} · **Found:** {date} "
        f"(fuzz differential sweep, `{profile}` profile, seed {seed}, via {via})",
        "",
        "## Summary",
        "",
        "Auto-filed by `tests/fuzz/bug_report.py` (driven from",
        "`tests/fuzz/triage_olevels.sh`): the sweep found this seed's tcc",
        "outputs diverging and the per-seed triage classified it. Not yet",
        "root-caused — this report is the tracking entry; fill the sections",
        "below in as triage narrows it down.",
        "",
        "| seed | class | ref (`gcc -m32 -funsigned-char`) | `-O0` | `-O1` | "
        "`-O2` | `-Os` | culprit knob |",
        "|---|---|---|---|---|---|---|---|",
        f"| {seed} | {cls} | {ref} | {o0} | {o1} | {o2} | {os_res} | {culprit} |",
        "",
        "## Reproducer",
        "",
    ]
    if repro_name:
        lines += [
            f"Frozen copy of the generated source: [`{repro_name}`]({repro_name}).",
            "Regenerate either way:",
            "",
            "```",
            f"python3 tests/fuzz/gen_c.py --seed {seed} --profile {profile} "
            f"-o seed{seed}.c",
            "```",
            "",
        ]
    else:
        lines += [
            "Generate the source (no frozen copy was available):",
            "",
            "```",
            f"python3 tests/fuzz/gen_c.py --seed {seed} --profile {profile} "
            f"-o seed{seed}.c",
            "```",
            "",
        ]
    lines += [
        "Serial per-seed A/B: `python3 scripts/diff_olevels.py --seed "
        f"{seed} --require-qemu`.",
        "",
        "## Root cause",
        "",
    ]
    if culprit not in ("-", "?", ""):
        lines += [
            "Not yet located. The triage culprit bisect found that "
            f"`{culprit}` restores the gcc reference value at the divergent "
            "level — start from the pass that knob disables.",
        ]
    else:
        lines += [
            "Not yet located — no single culprit knob isolated the divergence "
            "(see the table above).",
        ]
    lines += [
        "",
        "## Regression lock",
        "",
        "None yet.",
        "",
        "## Likely fix",
        "",
        "TBD — root-cause first, then pin with a regression test (a minimized "
        "version of the reproducer).",
        "",
    ]
    return "\n".join(lines)


def file_bug_report(*, profile: str, seed: int, cls: str, ref: str,
                    o0: str, o1: str, o2: str, os_res: str, culprit: str,
                    src: Path | str | None, via: str = "fuzz sweep",
                    bugs_dir: Path | str | None = None,
                    date: str | None = None) -> tuple[Path, bool]:
    """File `docs/bugs/fuzz_<profile>_seed<N>.md`; returns (path, created).

    created is False when a report with that filename already exists — the
    filename IS the dedup check, so callers re-running a sweep stay silent.
    """
    bugs = Path(bugs_dir or os.environ.get("FUZZ_BUGS_DIR")
                or ROOT / "docs" / "bugs")
    slug = report_slug(profile, seed)
    report = bugs / f"{slug}.md"
    if report.exists():
        return report, False

    repro_name: str | None = None
    if src is not None and Path(src).exists():
        repro_dir = bugs / "repro"
        repro_dir.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, repro_dir / f"{slug}.c")
        repro_name = f"repro/{slug}.c"

    text = _render(profile=profile, seed=seed, cls=cls, ref=ref, o0=o0,
                   o1=o1, o2=o2, os_res=os_res, culprit=culprit,
                   repro_name=repro_name,
                   via=via, date=date or datetime.date.today().isoformat())
    try:
        with open(report, "x", encoding="utf-8") as fh:   # exclusive create:
            fh.write(text)                                # parallel triage safe
    except FileExistsError:
        return report, False
    return report, True


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--profile", required=True, help="generator profile (int, ptr, ...)")
    ap.add_argument("--seed", required=True, type=int)
    ap.add_argument("--class", dest="cls", required=True,
                    help="triage class (O0-WRONG|O1|O2|Os|O1/CRASH|O2/CRASH|"
                         "COMPILE_CRASH|TCC-ASAN|TCC-LSAN|?)")
    ap.add_argument("--ref", default="?", help="gcc ground-truth checksum")
    ap.add_argument("--o0", default="?", help="-O0 result")
    ap.add_argument("--o1", default="?", help="-O1 result")
    ap.add_argument("--o2", default="?", help="-O2 result")
    ap.add_argument("--os", dest="os", default="?", help="-Os result")
    ap.add_argument("--culprit", default="-", help="culprit knob from the bisect ('-' if none)")
    ap.add_argument("--src", default=None, help="the seed's generated .c (frozen into repro/)")
    ap.add_argument("--via", default="fuzz sweep", help="provenance for the Found line")
    ap.add_argument("--bugs-dir", default=None,
                    help="bugs directory (default $FUZZ_BUGS_DIR or <repo>/docs/bugs)")
    args = ap.parse_args(argv)

    report, created = file_bug_report(
        profile=args.profile, seed=args.seed, cls=args.cls, ref=args.ref,
        o0=args.o0, o1=args.o1, o2=args.o2, os_res=args.os,
        culprit=args.culprit, src=args.src, via=args.via, bugs_dir=args.bugs_dir)
    print(("filed" if created else "exists (skipped)") + f": {report}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
