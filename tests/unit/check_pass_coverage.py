#!/usr/bin/env python3
"""
check_pass_coverage.py — pipeline pass → test-layer coverage ledger for tinycc.

Enumerates every optimization pass registered in ir/opt_pipeline.c (PASS / PASS_GATED)
and every SSA pass driven by SSA_RUN(...) in ir/opt/*.c, then checks whether each
pass is covered by:

  * a UT_COVERS("<pass>") marker in tests/unit/arm/armv8m/*.c, or
  * a golden-IR directory under tests/ir_tests/golden/<pass>/.

Usage:
    python3 check_pass_coverage.py              # print coverage table + gaps
    python3 check_pass_coverage.py --strict     # exit non-zero if any gap exists
    python3 check_pass_coverage.py --write-md   # regenerate PASS_COVERAGE.md table
"""

import argparse
import re
import sys
from pathlib import Path

TINYCC_ROOT = Path(__file__).resolve().parents[2]
PIPELINE_C = TINYCC_ROOT / "ir" / "opt_pipeline.c"
SSA_OPT_DIR = TINYCC_ROOT / "ir" / "opt"
UNIT_DIR = TINYCC_ROOT / "tests" / "unit" / "arm" / "armv8m"
GOLDEN_DIR = TINYCC_ROOT / "tests" / "ir_tests" / "golden"
PASS_MD = Path(__file__).resolve().parent / "PASS_COVERAGE.md"

PASS_RE = re.compile(r"^\s*(?:PASS|PASS_GATED)\s*\(\s*\"([^\"]+)\"")
ARRAY_RE = re.compile(r"static\s+const\s+IROptPass\s+(\w+)_passes\s*\[")
SSA_RUN_RE = re.compile(r"SSA_RUN\s*\(\s*\"([^\"]+)\"")

# Order in which groups appear in opt_pipeline.c; anything else goes last.
GROUP_ORDER = [
    "propagation",
    "fusion",
    "memory",
    "late_cleanup",
    "entry_store",
    "ssa",
]

# UT_COVERS markers (and hand-written suite names) often use the pass function
# basename or a descriptive name rather than the exact string registered in the
# PASS/PASS_GATED macro.  This map normalizes those markers to registered names.
# A single marker may cover multiple registered passes.
ALIAS_MAP: dict[str, list[str]] = {
    # opt_cmpfold suite
    "cmp_fold": ["cmp_expr_fold", "cmp_offset_fold"],
    # opt_const_aggregate
    "const_aggregate_fold": ["const_agg_fold"],
    # opt_constfold suite
    "float_narrowing": ["float_narrow"],
    "const_string_calls": ["string_calls"],
    "const_call_replace": ["string_calls"],
    "switch_call_replace": ["string_calls"],
    "param_addrof_const_fold": ["string_calls"],
    "local_addrof_const_fold": ["string_calls"],
    # opt_constprop suite
    "global_init_prop": ["global_init"],
    "symref_const_prop": ["symref_prop"],
    "complex_const_param_fold": ["const_prop"],
    # opt_copyprop suite helpers
    "cse_global_load": ["copy_prop"],
    "globalsym_cse": ["copy_prop"],
    "cse_param_add": ["copy_prop"],
    "local_load_cse": ["copy_prop"],
    "local_alu_cse": ["copy_prop"],
    "bool_cse": ["copy_prop"],
    # opt_jump_thread suite
    "jump_threading": ["jump_thread"],
    "eliminate_fallthrough": ["elim_fallthru"],
    # opt_setif_or_taut suite
    "setif_or_tautology": ["setif_or_taut"],
    # opt_bitfield suite
    "bitfield_insert_extract": ["bf_insert_extract"],
    "bitfield_insert_to_bfi": ["bf_insert_extract"],
    # opt_dead_lea_store suite
    "dead_lea_store_elim": ["dead_lea_store"],
    # opt_dead_vla suite
    "dead_vla_struct_elim": ["dead_vla_struct"],
    "dead_alloca_vreg_elim": ["dead_alloca_vreg"],
    # opt_xform suite
    "store_inplace_arith": ["inplace_arith"],
    # metamorphic suite / misc markers
    "self_arith_fold": ["self_arith"],
    "single_value_tmp": ["single_val_tmp"],
}


def discover_registered_passes() -> dict[str, str]:
    """Return mapping pass_name -> group_name from the pipeline and SSA driver."""
    passes: dict[str, str] = {}
    current_group = "unknown"

    text = PIPELINE_C.read_text(encoding="utf-8")
    for line in text.splitlines():
        array_m = ARRAY_RE.search(line)
        if array_m:
            current_group = array_m.group(1)
            continue
        pass_m = PASS_RE.search(line)
        if pass_m:
            name = pass_m.group(1)
            passes[name] = current_group

    # SSA passes are registered by name in the SSA driver, not in PASS arrays.
    for ssa_c in sorted(SSA_OPT_DIR.glob("*.c")):
        for line in ssa_c.read_text(encoding="utf-8").splitlines():
            m = SSA_RUN_RE.search(line)
            if m:
                name = m.group(1)
                passes[name] = "ssa"

    return passes


def discover_unit_coverage() -> tuple[dict[str, list[str]], dict[str, list[str]]]:
    """Return (resolved pass -> files, alias -> registered names actually used)."""
    raw_covers: dict[str, list[str]] = {}
    covers_re = re.compile(r"UT_COVERS\s*\(\s*\"([^\"]+)\"\s*\)")
    if UNIT_DIR.exists():
        for test in sorted(UNIT_DIR.glob("*.c")):
            for line in test.read_text(encoding="utf-8").splitlines():
                for m in covers_re.finditer(line):
                    name = m.group(1)
                    raw_covers.setdefault(name, []).append(test.name)

    resolved: dict[str, list[str]] = {}
    aliases_used: dict[str, list[str]] = {}
    for marker, files in raw_covers.items():
        targets = ALIAS_MAP.get(marker, [marker])
        for target in targets:
            resolved.setdefault(target, []).extend(files)
        if marker in ALIAS_MAP:
            aliases_used[marker] = targets
    return resolved, aliases_used


def discover_golden_coverage() -> dict[str, list[str]]:
    """Map pass_name -> list of golden-IR case names."""
    covers: dict[str, list[str]] = {}
    if not GOLDEN_DIR.exists():
        return covers
    for pass_dir in sorted(GOLDEN_DIR.iterdir()):
        if pass_dir.is_dir():
            name = pass_dir.name
            cases = sorted(p.name for p in pass_dir.iterdir() if p.is_file())
            covers[name] = cases
    return covers


def group_sort_key(group: str) -> int:
    try:
        return GROUP_ORDER.index(group)
    except ValueError:
        return len(GROUP_ORDER)


def build_report(registered: dict[str, str],
                 unit_cov: dict[str, list[str]],
                 golden_cov: dict[str, list[str]],
                 aliases_used: dict[str, list[str]]) -> tuple[str, list[str], list[str]]:
    """Return (markdown, uncovered_registered, orphaned_covers)."""
    uncovered: list[str] = []
    lines: list[str] = []

    # Group passes by their group name.
    grouped: dict[str, list[str]] = {}
    for name, group in registered.items():
        grouped.setdefault(group, []).append(name)

    total = len(registered)
    covered = 0
    for group in sorted(grouped, key=group_sort_key):
        names = sorted(grouped[group])
        lines.append(f"## {group} passes ({len(names)} registered)")
        lines.append("")
        lines.append("| Pass | Covered by | Status |")
        lines.append("|---|---|---|")
        for name in names:
            unit_files = unit_cov.get(name, [])
            golden_cases = golden_cov.get(name, [])
            parts: list[str] = []
            if unit_files:
                parts.append("unit: " + ", ".join(sorted(set(unit_files))))
            if golden_cases:
                parts.append("golden: " + ", ".join(golden_cases[:3]))
                if len(golden_cases) > 3:
                    parts[-1] += f" (+{len(golden_cases) - 3})"
            cell = "; ".join(parts) if parts else "—"
            if unit_files or golden_cases:
                status = "✅ covered"
                covered += 1
            else:
                status = "❌ uncovered"
                uncovered.append(f"{group}/{name}")
            lines.append(f"| `{name}` | {cell} | {status} |")
        lines.append("")

    lines.append(f"**Total:** {covered}/{total} registered passes covered ({covered/total:.1%}).")
    lines.append("")

    if aliases_used:
        lines.append("## Alias-normalized coverage markers")
        lines.append("")
        lines.append("The following marker names do not match a registered pass name exactly;")
        lines.append("they were mapped to registered names via the alias table. Consider aligning")
        lines.append("the UT_COVERS markers to the registered names over time.")
        lines.append("")
        lines.append("| Marker | Resolved to |")
        lines.append("|---|---|")
        for marker in sorted(aliases_used):
            lines.append(f"| `{marker}` | {', '.join(f'`{x}`' for x in aliases_used[marker])} |")
        lines.append("")

    # Orphaned UT_COVERS (marker names not matching any registered pass or alias).
    raw_markers = set()
    covers_re = re.compile(r"UT_COVERS\s*\(\s*\"([^\"]+)\"\s*\)")
    if UNIT_DIR.exists():
        for test in UNIT_DIR.glob("*.c"):
            for line in test.read_text(encoding="utf-8").splitlines():
                for m in covers_re.finditer(line):
                    raw_markers.add(m.group(1))
    orphaned: list[str] = []
    for name in sorted(raw_markers):
        if name not in registered and name not in ALIAS_MAP:
            files = [t.name for t in UNIT_DIR.glob("*.c")
                     if f'UT_COVERS("{name}")' in t.read_text(encoding="utf-8")]
            orphaned.append(f"`{name}` in {', '.join(sorted(set(files)))}")
    for name, cases in golden_cov.items():
        if name not in registered:
            orphaned.append(f"`{name}` golden dir with {len(cases)} case(s)")

    if orphaned:
        lines.append("## Orphaned coverage markers")
        lines.append("")
        lines.append("These markers do not match any registered pass name or known alias;")
        lines.append("they may cover internal helpers or be stale.")
        lines.append("")
        for o in orphaned:
            lines.append(f"- {o}")
        lines.append("")

    if uncovered:
        lines.append("## Coverage gaps")
        lines.append("")
        lines.append("The following registered passes have no UT_COVERS marker and no golden-IR directory:")
        lines.append("")
        for g in uncovered:
            lines.append(f"- `{g}`")
        lines.append("")

    return "\n".join(lines), uncovered, orphaned


def update_pass_md(report: str) -> None:
    """Replace or append the auto-generated coverage table inside PASS_COVERAGE.md."""
    marker_start = "<!-- BEGIN AUTO PASS COVERAGE -->\n"
    marker_end = "\n<!-- END AUTO PASS COVERAGE -->"
    new_section = marker_start + report + marker_end

    if PASS_MD.exists():
        text = PASS_MD.read_text(encoding="utf-8")
        if marker_start in text and marker_end in text:
            before = text[:text.index(marker_start)]
            after = text[text.index(marker_end) + len(marker_end):]
            text = before + new_section + after
        else:
            text += "\n\n" + new_section
    else:
        text = "# Pass Coverage Ledger\n\n" + new_section

    PASS_MD.write_text(text, encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="tinycc pipeline pass coverage checker")
    parser.add_argument(
        "--strict",
        action="store_true",
        help="exit non-zero if any registered pass is uncovered",
    )
    parser.add_argument(
        "--write-md",
        action="store_true",
        help="write/append the generated table to PASS_COVERAGE.md",
    )
    args = parser.parse_args()

    if not PIPELINE_C.exists():
        print(f"ERROR: pipeline file not found: {PIPELINE_C}", file=sys.stderr)
        return 1

    registered = discover_registered_passes()
    unit_cov, aliases_used = discover_unit_coverage()
    golden_cov = discover_golden_coverage()

    report, uncovered, orphaned = build_report(registered, unit_cov, golden_cov, aliases_used)

    if args.write_md:
        update_pass_md(report)
        print(f"Updated {PASS_MD}", file=sys.stderr)

    print(report)

    if args.strict and uncovered:
        print(f"\nFAIL: {len(uncovered)} registered pass(es) uncovered",
              file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
