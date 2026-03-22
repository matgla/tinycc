#!/usr/bin/env python3
"""
Categorize GCC torture compile test failures by error type.

Usage: python3 categorize_compile_failures.py [--opt O0|O1] [--limit N]
"""

import subprocess
import sys
import re
from pathlib import Path
from collections import defaultdict, Counter

PROJECT_ROOT = Path(__file__).parent.parent.parent
COMPILER = PROJECT_ROOT / "armv8m-tcc"

sys.path.insert(0, str(PROJECT_ROOT / "tests" / "gcctestsuite"))
from conftest import discover_gcc_compile_tests, should_skip_gcc_test

def compile_test(source: Path, opt_level: str, dg_options: str = "") -> tuple:
    """Compile a single test. Returns (success, error_output)."""
    cmd = [str(COMPILER), opt_level, "-c", str(source), "-o", "/dev/null"]
    if dg_options:
        cmd.extend(dg_options.split())
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        if result.returncode == 0:
            return True, ""
        return False, result.stderr
    except subprocess.TimeoutExpired:
        return False, "TIMEOUT"
    except Exception as e:
        return False, f"EXCEPTION: {e}"


def categorize_error(stderr: str) -> str:
    """Extract the primary error category from compiler stderr."""
    if stderr == "TIMEOUT":
        return "TIMEOUT"
    if stderr.startswith("EXCEPTION:"):
        return stderr

    # Look for the first error line
    for line in stderr.splitlines():
        # TCC error format: "file:line: error: ..."
        m = re.search(r"error:\s*(.+)", line)
        if m:
            msg = m.group(1).strip()
            # Normalize: remove file-specific parts
            # "identifier expected" -> "identifier expected"
            # "'foo' undeclared" -> "'...' undeclared"
            msg = re.sub(r"'[^']*'", "'...'", msg)
            # Truncate long messages
            if len(msg) > 80:
                msg = msg[:77] + "..."
            return msg

    # Check for other patterns
    if "internal compiler error" in stderr.lower():
        return "INTERNAL COMPILER ERROR"
    if "crash" in stderr.lower() or "segfault" in stderr.lower():
        return "CRASH"
    if stderr.strip():
        # Return first non-empty line truncated
        first = stderr.strip().splitlines()[0][:80]
        return first

    return "UNKNOWN ERROR (empty stderr)"


def main():
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("--opt", default="-O0", help="Optimization level (default: -O0)")
    parser.add_argument("--limit", type=int, default=0, help="Max tests to run (0=all)")
    parser.add_argument("--verbose", "-v", action="store_true", help="Show each failure")
    args = parser.parse_args()

    tests = discover_gcc_compile_tests()
    print(f"Discovered {len(tests)} compile tests")

    # Filter skipped
    active = [(t, should_skip_gcc_test(t.source)) for t in tests]
    active = [(t, sr) for t, sr in active if not sr]
    print(f"Active (non-skipped): {len(active)}")

    if args.limit:
        active = active[:args.limit]
        print(f"Running first {args.limit} tests")

    error_groups = defaultdict(list)
    passed = 0
    failed = 0

    for i, (test, _) in enumerate(active):
        if (i + 1) % 100 == 0:
            print(f"  Progress: {i+1}/{len(active)} ({passed} passed, {failed} failed)", file=sys.stderr)

        success, stderr = compile_test(test.source, args.opt, test.dg_options)
        if success:
            passed += 1
        else:
            failed += 1
            category = categorize_error(stderr)
            error_groups[category].append(test.source.stem)
            if args.verbose:
                print(f"  FAIL: {test.source.stem}: {category}")

    print(f"\n{'='*80}")
    print(f"RESULTS: {passed} passed, {failed} failed out of {len(active)} (opt={args.opt})")
    print(f"{'='*80}\n")

    # Sort groups by count (largest first)
    sorted_groups = sorted(error_groups.items(), key=lambda x: -len(x[1]))

    print(f"{'Count':>6}  Error Category")
    print(f"{'-----':>6}  {'-'*70}")
    for category, tests_list in sorted_groups:
        print(f"{len(tests_list):>6}  {category}")
        # Show first few test names
        sample = tests_list[:5]
        more = len(tests_list) - len(sample)
        for name in sample:
            print(f"         - {name}")
        if more > 0:
            print(f"         ... and {more} more")
        print()

    # Summary
    print(f"\n{'='*80}")
    print(f"SUMMARY: {len(sorted_groups)} distinct error categories")
    print(f"Top 5 categories account for {sum(len(v) for _, v in sorted_groups[:5])} / {failed} failures")


if __name__ == "__main__":
    main()
