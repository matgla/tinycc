#!/usr/bin/env python3
"""
gen_source_coverage.py — source-file → test-layer coverage ledger for tinycc.

Scans the tinycc source tree, maps each TU to the test layer that covers it
(unit, QEMU ir_tests, smoke tests, runtime libs, or none), and writes a
human-readable report.  The checked-in source_coverage_map.json is the editable
ledger; this script only (re)generates the report and can (re)seed missing map
entries.

Usage:
    python3 gen_source_coverage.py              # generate SOURCE_COVERAGE.md
    python3 gen_source_coverage.py --write-map  # seed missing map entries
"""

import argparse
import datetime
import json
import sys
from pathlib import Path

TINYCC_ROOT = Path(__file__).resolve().parents[2]
UNIT_DIR = TINYCC_ROOT / "tests" / "unit" / "arm" / "armv8m"
MAP_FILE = Path(__file__).resolve().parent / "source_coverage_map.json"
REPORT_MD = Path(__file__).resolve().parent / "SOURCE_COVERAGE.md"

KIND_ORDER = [
    "unit",
    "golden_ir",
    "codegen_asm",
    "ir_test",
    "smoke",
    "runtime_lib",
    "tool",
    "partial",
    "none",
]

KIND_LABEL = {
    "unit": "Covered by unit suite",
    "golden_ir": "Covered by golden-IR snapshot",
    "codegen_asm": "Covered by codegen disassembly test",
    "ir_test": "Covered by QEMU ir_tests corpus",
    "smoke": "Covered by smoke tests",
    "runtime_lib": "Runtime library (exercised by compiled programs)",
    "tool": "Build/test helper, not product code",
    "partial": "Partial coverage only",
    "none": "No known dedicated coverage",
}

# Source directories to inventory, relative to tinycc root.
SOURCE_GLOBS = [
    ("*.c", False),
    ("ir/*.c", False),
    ("arch/arm/**/*.c", True),
    ("lib/*.c", False),
]

# Files excluded from the inventory (test fixtures, examples, helpers).
EXCLUDE_PATTERNS = [
    "conftest.c",
    "debug_test.c",
    "examples/*",
    ".cache/*",
    "tests/*",
    "build/*",
]


def is_excluded(rel: str) -> bool:
    """Return True if a source path should not appear in the ledger."""
    name = Path(rel).name
    if name.startswith("check_"):
        return True
    for pat in EXCLUDE_PATTERNS:
        if pat.endswith("/*"):
            if rel.startswith(pat[:-1]):
                return True
        elif rel == pat or rel.endswith("/" + pat):
            return True
    return False


def discover_source_files() -> list[str]:
    """Return sorted relative paths of source files to track."""
    files: set[str] = set()
    for glob, _recursive in SOURCE_GLOBS:
        for p in TINYCC_ROOT.glob(glob):
            rel = p.relative_to(TINYCC_ROOT).as_posix()
            if not is_excluded(rel):
                files.add(rel)
    return sorted(files)


def unit_mapping() -> dict[str, str]:
    """Map source files to their direct unit-test file names."""
    mapping: dict[str, str] = {}
    if not UNIT_DIR.exists():
        return mapping
    for test in sorted(UNIT_DIR.glob("test_*.c")):
        name = test.stem  # e.g. test_opt_neg_chain
        if name == "test_opt_licm":
            mapping["ir/licm.c"] = name + ".c"
        elif name.startswith("test_opt_"):
            mapping[f"ir/opt_{name[9:]}.c"] = name + ".c"
        elif name.startswith("test_thop_"):
            mapping[f"arch/arm/thumb/thop_{name[10:]}.c"] = name + ".c"
        elif name == "test_ir_pool":
            mapping["ir/pool.c"] = name + ".c"
        elif name == "test_ir_type":
            mapping["ir/type.c"] = name + ".c"
        elif name == "test_ir_vreg":
            mapping["ir/vreg.c"] = name + ".c"
        elif name == "test_ir_core":
            mapping["ir/core.c"] = name + ".c"
        elif name == "test_ir_dump":
            mapping["ir/dump.c"] = name + ".c"
        elif name == "test_ir_stack":
            mapping["ir/stack.c"] = name + ".c"
        elif name == "test_ir_ssa":
            mapping["ir/ssa.c"] = name + ".c"
        elif name == "test_ir_operand":
            mapping["tccir_operand.c"] = name + ".c"
        elif name == "test_svalue":
            mapping["svalue.c"] = name + ".c"
    return mapping


def default_kind(rel: str) -> tuple[str, list[str], str]:
    """Return (kind, via list, note) for a source file not in the map."""
    if rel.startswith("lib/"):
        return "runtime_lib", ["runtime tests (compiled programs)"], ""
    if rel.startswith("ir/") or rel.startswith("arch/arm/") or "/" not in rel:
        return "ir_test", ["tests/ir_tests/*.expect QEMU corpus"], "no dedicated unit/golden/asm test yet"
    return "none", [], ""


def load_map() -> dict[str, dict]:
    if MAP_FILE.exists():
        with open(MAP_FILE, "r", encoding="utf-8") as f:
            return json.load(f)
    return {}


def save_map(data: dict[str, dict]) -> None:
    with open(MAP_FILE, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2, sort_keys=True)
        f.write("\n")


def build_ledger(write_map: bool = False) -> dict[str, dict]:
    """Build the full file → coverage ledger."""
    sources = discover_source_files()
    umap = unit_mapping()
    manual = load_map()

    if write_map:
        updated = dict(manual)
        for rel in sources:
            if rel in updated:
                continue
            if rel in umap:
                updated[rel] = {
                    "kind": "unit",
                    "via": [f"tests/unit/arm/armv8m/{umap[rel]}"],
                    "note": "",
                }
            else:
                kind, via, note = default_kind(rel)
                updated[rel] = {"kind": kind, "via": via, "note": note}
        save_map(updated)
        print(f"Wrote {MAP_FILE} with {len(updated)} entries", file=sys.stderr)
        return updated

    ledger: dict[str, dict] = {}
    for rel in sources:
        if rel in umap:
            ledger[rel] = {
                "kind": "unit",
                "via": [f"tests/unit/arm/armv8m/{umap[rel]}"],
                "note": "",
            }
        elif rel in manual:
            ledger[rel] = manual[rel]
        else:
            kind, via, note = default_kind(rel)
            ledger[rel] = {"kind": kind, "via": via, "note": note}
    return ledger


def generate_markdown(ledger: dict[str, dict]) -> str:
    total = len(ledger)
    counts: dict[str, int] = {k: 0 for k in KIND_ORDER}
    for entry in ledger.values():
        counts[entry.get("kind", "none")] = counts.get(entry.get("kind", "none"), 0) + 1

    lines: list[str] = []
    lines.append("# tinycc source-file coverage ledger")
    lines.append("")
    lines.append("> Generated by `gen_source_coverage.py`.  Do not edit by hand; update "
              "`source_coverage_map.json` and rerun the generator.")
    lines.append("")
    lines.append("## Summary")
    lines.append("")
    lines.append("| Layer | Count | Fraction |")
    lines.append("|---|---|---|")
    for kind in KIND_ORDER:
        if counts[kind]:
            frac = counts[kind] / total
            lines.append(f"| {KIND_LABEL[kind]} | {counts[kind]} | {frac:.1%} |")
    lines.append(f"| **Total tracked files** | **{total}** | **100%** |")
    lines.append("")

    for kind in KIND_ORDER:
        if not counts[kind]:
            continue
        lines.append(f"## {KIND_LABEL[kind]}")
        lines.append("")
        lines.append("| Source file | Covered via | Note |")
        lines.append("|---|---|---|")
        for rel in sorted(ledger):
            entry = ledger[rel]
            if entry.get("kind") != kind:
                continue
            via = ", ".join(entry.get("via", [])) or "—"
            note = entry.get("note", "") or "—"
            lines.append(f"| `{rel}` | {via} | {note} |")
        lines.append("")

    return "\n".join(lines)


def check_ledger() -> list[str]:
    """Return a list of human-readable problems with the current ledger."""
    problems: list[str] = []
    sources = discover_source_files()
    umap = unit_mapping()
    manual = load_map()

    missing = [rel for rel in sources if rel not in manual and rel not in umap]
    if missing:
        problems.append("Missing map entries for source files (run with --write-map to seed):")
        for rel in missing:
            problems.append(f"  - {rel}")

    ledger = build_ledger(write_map=False)
    expected_md = generate_markdown(ledger)
    if REPORT_MD.exists():
        actual_md = REPORT_MD.read_text(encoding="utf-8")
        if actual_md != expected_md:
            problems.append(f"{REPORT_MD} is stale (run generator to update)")
    else:
        problems.append(f"{REPORT_MD} does not exist")

    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description="tinycc source-file coverage ledger generator")
    parser.add_argument(
        "--write-map",
        action="store_true",
        help="(re)seed source_coverage_map.json from the current source tree",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="exit non-zero if the ledger is incomplete or the report is stale",
    )
    args = parser.parse_args()

    if args.check:
        problems = check_ledger()
        if problems:
            for p in problems:
                print(f"ERROR: {p}", file=sys.stderr)
            return 1
        print(f"OK: source coverage ledger is complete and {REPORT_MD} is up to date", file=sys.stderr)
        return 0

    ledger = build_ledger(write_map=args.write_map)
    md = generate_markdown(ledger)
    REPORT_MD.write_text(md, encoding="utf-8")
    print(f"Wrote {REPORT_MD} ({len(ledger)} files)", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
