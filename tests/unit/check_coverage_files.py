#!/usr/bin/env python3
"""
Compare coverage report files with source files in the repository.
Reports files that are in the repo but not registered in coverage measurement.
"""

import os
import sys
import re
from pathlib import Path


# Repo-relative .c paths that are intentionally absent from coverage
# measurement, with the reason.  These are not compiler sources (host
# tooling, dev scratch programs) or cannot be measured by gcov.  Keep in
# sync with the unit-test build (tests/unit/arm/armv8m/Makefile).
EXCLUDED_FILES = {
    'conftest.c':
        'host build helper (c2str generator), not part of the compiler',
    'check_ir.c':
        'standalone dev scratch program, not part of the compiler',
    'check_op.c':
        'standalone dev scratch program, not part of the compiler',
    'check_vreg.c':
        'standalone dev scratch program, not part of the compiler',
    'debug_test.c':
        'standalone dev scratch program, not part of the compiler',
    'ir/gen/config.c':
        'data-only table (irop_config[]), no executable lines for gcov',
    'source/backend/arch/fpu/arm/fpv5-d16.c':
        'data-only FPU config table, no executable lines for gcov',
    'source/backend/arch/fpu/arm/fpv5-sp-d16.c':
        'data-only FPU config table, no executable lines for gcov',
    'source/backend/arch/arm/thumb/arm-thumb-scratch.c':
        'WIP extraction not wired into any build (includes nonexistent'
        ' arm-thumb-scratch.h, does not compile)',
    'source/opt/framework/example_strength.c':
        'OPT DSL example, compile-check only (OPT_DSL_EXAMPLES=no)',
}


def get_coverage_files(report_path):
    """Extract file paths from gcovr coverage report."""
    files = set()
    if not os.path.exists(report_path):
        print(f"Coverage report not found: {report_path}", file=sys.stderr)
        return files

    with open(report_path, 'r') as f:
        for line in f:
            # Match lines like: "source/opt/flat/dce/dce.c  338  186  55%".
            # gcovr wraps long filenames onto a line of their own, with the
            # stats following on the next line — accept that form too.
            match = re.match(r'^(\S+\.c)(?:\s+\d+\s+\d+\s+\d+%|\s*$)', line)
            if match:
                files.add(match.group(1))
    return files


def get_repo_source_files(repo_root, exclude_patterns=None):
    """Find all .c files in the repository, excluding tests and build directories."""
    if exclude_patterns is None:
        exclude_patterns = [
            'tests',
            'lib',
            'examples',
            'win32',
            'rp2350_examples',
            'build',
            'build_*',
        ]

    files = []
    for dirpath, dirnames, filenames in os.walk(repo_root):
        # Skip excluded directories
        dirnames[:] = [d for d in dirnames if not any(
            re.match(pat, d) for pat in exclude_patterns
        )]

        for filename in filenames:
            if filename.endswith('.c'):
                filepath = os.path.join(dirpath, filename)
                files.append(filepath)

    return sorted(files)


def main():
    if len(sys.argv) < 3:
        print("Usage: check_coverage_files.py <coverage_report.txt> <repo_root>")
        sys.exit(1)

    report_path = sys.argv[1]
    repo_root = sys.argv[2]

    # Get files from coverage report
    coverage_files = get_coverage_files(report_path)

    # Get all source files in repository
    repo_files = get_repo_source_files(repo_root)

    # Normalize paths for comparison (coverage report uses relative paths without ./)
    normalized_coverage = set()
    for f in coverage_files:
        if f.startswith('./'):
            f = f[2:]
        normalized_coverage.add(f)

    # Make repo files relative to the repo root as well: the report lists
    # paths relative to gcovr's --root, while os.walk() above returns paths
    # prefixed with repo_root (absolute when the caller passes an absolute
    # root, as the Makefile's ut-coverage target does with $(CURDIR)).
    normalized_repo = []
    for f in repo_files:
        normalized_repo.append(os.path.relpath(f, repo_root))

    # Warn about exclusions that no longer match a repo file (stale entry)
    stale = [f for f in EXCLUDED_FILES
             if not os.path.isfile(os.path.join(repo_root, f))]
    if stale:
        print("Warning: stale EXCLUDED_FILES entries (file no longer exists):",
              file=sys.stderr)
        for f in stale:
            print(f"  {f}", file=sys.stderr)

    # Drop intentionally excluded files before comparing
    excluded = [f for f in normalized_repo if f in EXCLUDED_FILES]
    normalized_repo = [f for f in normalized_repo if f not in EXCLUDED_FILES]

    # Find files in repo but not in coverage
    missing_files = [f for f in normalized_repo if f not in normalized_coverage]

    if missing_files:
        print(f"\n{'='*70}")
        print(f"FILES IN REPOSITORY BUT NOT IN COVERAGE REPORT")
        print(f"{'='*70}")
        print(f"Total source files in repo: {len(repo_files)}")
        print(f"Files in coverage report:   {len(coverage_files)}")
        print(f"Intentionally excluded:     {len(excluded)} (see EXCLUDED_FILES)")
        print(f"Missing from coverage:      {len(missing_files)}")
        print(f"{'='*70}\n")

        for f in missing_files:
            print(f"  {f}")

        print(f"\n{'='*70}")
        print(f"Consider adding missing files to the coverage filter in")
        print(f"tests/unit/arm/armv8m/Makefile (GCOVR_FILTERS variable),")
        print(f"or to UT_COVERAGE_ONLY_SRCS there if no suite compiles them.")
        print(f"{'='*70}")
    else:
        print(f"\nAll {len(normalized_repo)} source files are registered in"
              f" coverage report ({len(excluded)} intentionally excluded).")


if __name__ == '__main__':
    main()
