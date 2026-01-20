#!/usr/bin/env python3
"""
Profile TinyCC compiler memory usage and performance across the test suite.

Uses the unified qemu_run.py infrastructure with profiling support.

Usage:
    python profile_suite.py [--output-dir DIR] [--limit N] [--profiler heaptrack|time|perf] [--cflags "..."]

Output:
    - profile_results/heaptrack_*.zst - heaptrack data files (use heaptrack_gui to view)
    - profile_results/time_*.txt      - GNU time output files
    - profile_results/perf_*.data     - perf data files (use perf report to view)
    - profile_results/perf_*.svg      - CPU flamegraph SVG files (open in browser)
    - profile_results/summary.csv     - CSV with all metrics
    - profile_results/summary.json    - JSON with all metrics
"""

import argparse
import csv
import json
import sys
from dataclasses import asdict
from pathlib import Path

from qemu_run import (
    compile_testcase,
    CompileConfig,
    ProfileConfig,
    CompileResult,
    reset_clean_state,
    CURRENT_DIR,
)
from test_qemu import TEST_FILES, FLOAT_TEST_FILES

DEFAULT_OUTPUT_DIR = CURRENT_DIR / "profile_results"
MACHINE = "mps2-an505"


def _as_file_list(test_file):
    if isinstance(test_file, (list, tuple)):
        return list(test_file)
    return [test_file]


def _primary_file(test_file):
    files = _as_file_list(test_file)
    return files[0] if files else None


def _test_id(test_file):
    primary = _primary_file(test_file)
    return Path(primary).stem if primary else "unknown"


def profile_test(test_file, output_dir, profiler_tool="heaptrack", extra_cflags: str = "", compiler: Path = None):
    """Profile a single test compilation."""
    test_name = _test_id(test_file)

    # Resolve test files
    test_files = _as_file_list(test_file)
    source_files = [CURRENT_DIR / Path(f) for f in test_files]

    # Configure profiling
    profile_config = ProfileConfig(
        tool=profiler_tool,
        output_dir=output_dir,
        output_prefix=test_name,
    )

    config = CompileConfig(
        compiler=compiler,
        profiler=profile_config,
        extra_cflags=extra_cflags or "",
        output_dir=output_dir / "build",
        clean_before_build=True,
    )

    # Compile with profiling
    result = compile_testcase(source_files, MACHINE, config=config)

    return result, test_name


def print_result(result: CompileResult, test_name: str, idx: int, total: int):
    """Print a single result to console."""
    status = "OK" if result.success else "FAIL"

    if result.heap_peak_kb > 0:
        mem_str = f"heap_peak={result.heap_peak_kb}KB"
    elif result.max_rss_kb > 0:
        mem_str = f"max_rss={result.max_rss_kb}KB"
    else:
        mem_str = "mem=N/A"

    extra = ""
    if result.perf_samples > 0:
        extra = f" samples={result.perf_samples}"
        # Show memory alongside perf samples if available
        if result.max_rss_kb > 0 and result.heap_peak_kb == 0:
            extra += f" rss={result.max_rss_kb}KB"
    if result.flamegraph_file:
        extra += " [flamegraph]"

    print(f"[{idx:3d}/{total}] {test_name:40s} {status:4s} "
          f"time={result.compile_time_s:.3f}s {mem_str} "
          f"bin={result.total_size}B{extra}")


def result_to_dict(result: CompileResult, test_name: str) -> dict:
    """Convert CompileResult to dictionary for serialization."""
    return {
        "test_name": test_name,
        "success": result.success,
        "compile_time_s": result.compile_time_s,
        "user_time_s": result.user_time_s,
        "sys_time_s": result.sys_time_s,
        "max_rss_kb": result.max_rss_kb,
        "heap_peak_kb": result.heap_peak_kb,
        "heap_allocations": result.heap_allocations,
        "heap_temporary_allocs": result.heap_temporary_allocs,
        "perf_samples": result.perf_samples,
        "flamegraph_file": result.flamegraph_file,
        "profile_file": result.profile_file,
        "text_size": result.text_size,
        "data_size": result.data_size,
        "bss_size": result.bss_size,
        "total_size": result.total_size,
        "error": result.error[:200] if result.error else "",
    }


def write_summary(results, output_dir):
    """Write summary CSV and JSON files."""
    # CSV
    csv_file = output_dir / "summary.csv"
    with open(csv_file, 'w', newline='') as f:
        if results:
            writer = csv.DictWriter(f, fieldnames=results[0].keys())
            writer.writeheader()
            for r in results:
                writer.writerow(r)

    # JSON
    json_file = output_dir / "summary.json"
    with open(json_file, 'w') as f:
        json.dump(results, f, indent=2)

    # Summary stats
    successful = [r for r in results if r["success"]]
    if successful:
        total_time = sum(r["compile_time_s"] for r in successful)
        max_heap = max((r["heap_peak_kb"] for r in successful), default=0)
        max_rss = max((r["max_rss_kb"] for r in successful), default=0)
        total_bin_size = sum(r["total_size"] for r in successful)

        print("\n" + "=" * 70)
        print("SUMMARY")
        print("=" * 70)
        print(f"Tests run:        {len(results)}")
        print(f"Tests passed:     {len(successful)}")
        print(f"Tests failed:     {len(results) - len(successful)}")
        print(f"Total compile time: {total_time:.2f}s")
        if max_heap > 0:
            print(f"Max heap peak:    {max_heap} KB ({max_heap/1024:.2f} MB)")
        if max_rss > 0:
            print(f"Max RSS:          {max_rss} KB ({max_rss/1024:.2f} MB)")
        print(f"Total binary size: {total_bin_size} bytes ({total_bin_size/1024:.2f} KB)")
        # Count flamegraphs generated
        flamegraph_count = sum(1 for r in successful if r.get("flamegraph_file"))

        print(f"\nResults saved to: {output_dir}")
        print(f"  - {csv_file.name}")
        print(f"  - {json_file.name}")
        if max_heap > 0:
            print(f"  - heaptrack_*.zst files (open with heaptrack_gui for memory flamegraphs)")
        if flamegraph_count > 0:
            print(f"  - {flamegraph_count} perf_*.svg flamegraph(s) (open in browser for CPU profiling)")


def main():
    parser = argparse.ArgumentParser(description="Profile TinyCC compiler across test suite")
    parser.add_argument("--output-dir", "-o", type=Path, default=DEFAULT_OUTPUT_DIR,
                        help="Output directory for profile data")
    parser.add_argument("--limit", "-n", type=int, default=0,
                        help="Limit number of tests to run (0 = all)")
    parser.add_argument("--profiler", "-p", choices=["heaptrack", "time", "perf"], default="heaptrack",
                        help="Profiler tool to use (default: heaptrack)")
    parser.add_argument("--include-float", action="store_true",
                        help="Include floating point tests")
    parser.add_argument("--cflags", type=str, default="",
                        help="Additional CFLAGS to pass to the compiler (e.g. '-O0 -g -DDEBUG')")
    parser.add_argument("--test", "-t", type=str,
                        help="Run only test matching this pattern")
    parser.add_argument("--compiler", "-c", type=Path, default=None,
                        help="Path to compiler binary (default: use armv8m-tcc from repo root)")
    args = parser.parse_args()

    # Prepare output directory
    args.output_dir.mkdir(parents=True, exist_ok=True)

    # Reset clean state for fresh profiling run
    reset_clean_state()

    # Collect all tests
    all_tests = [(f, code) for f, code in TEST_FILES]
    if args.include_float:
        all_tests.extend([(f, code) for f, code in FLOAT_TEST_FILES])

    # Filter by pattern if specified
    if args.test:
        all_tests = [(f, c) for f, c in all_tests if args.test in _test_id(f)]

    # Apply limit
    if args.limit > 0:
        all_tests = all_tests[:args.limit]

    print(f"Profiling {len(all_tests)} tests")
    print(f"Output directory: {args.output_dir}")
    print(f"Profiler: {args.profiler}")
    if args.compiler:
        print(f"Compiler: {args.compiler}")
    if args.cflags:
        print(f"Extra CFLAGS: {args.cflags}")
    print("=" * 70)

    results = []
    for idx, (test_file, _) in enumerate(all_tests, 1):
        result, test_name = profile_test(
            test_file,
            args.output_dir,
            profiler_tool=args.profiler,
            extra_cflags=args.cflags,
            compiler=args.compiler,
        )
        result_dict = result_to_dict(result, test_name)
        results.append(result_dict)
        print_result(result, test_name, idx, len(all_tests))

    write_summary(results, args.output_dir)

    return 0 if all(r["success"] for r in results) else 1


if __name__ == "__main__":
    sys.exit(main())
