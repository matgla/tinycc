#!/usr/bin/env python3
"""
Profile TinyCC compiler memory usage and performance across the test suite.

Uses heaptrack for memory profiling (heap allocations, peak memory, flamegraphs)
and GNU time for performance metrics. Collects binary size via arm-none-eabi-size.

The script creates a wrapper that intercepts compiler invocations during make,
so we profile the actual armv8m-tcc compiler, not the make process.

Usage:
    python profile_suite.py [--output-dir DIR] [--limit N] [--heaptrack] [--no-heaptrack]

Output:
    - profile_results/heaptrack_*.gz  - heaptrack data files (use heaptrack_gui to view)
    - profile_results/summary.csv     - CSV with all metrics
    - profile_results/summary.json    - JSON with all metrics
"""

import argparse
import csv
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field, asdict
from pathlib import Path
from typing import Optional

CURRENT_DIR = Path(__file__).parent
DEFAULT_OUTPUT_DIR = CURRENT_DIR / "profile_results"
MACHINE = "mps2-an505"

# Import test files from test_qemu.py
from test_qemu import TEST_FILES, FLOAT_TEST_FILES, TEST_FILES_WITH_ARGS


@dataclass
class ProfileResult:
    test_name: str
    # Compiler performance (aggregated across all compiler invocations)
    compile_time_s: float = 0.0
    user_time_s: float = 0.0
    sys_time_s: float = 0.0
    max_rss_kb: int = 0  # Peak resident set size (heap + stack + data)
    # Heaptrack metrics (if enabled)
    heap_peak_kb: int = 0
    heap_allocations: int = 0
    heap_temporary_allocs: int = 0
    heaptrack_file: str = ""
    # Binary size metrics (from arm-none-eabi-size)
    text_size: int = 0
    data_size: int = 0
    bss_size: int = 0
    total_size: int = 0
    # Status
    success: bool = True
    error: str = ""


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


def create_profiler_wrapper(real_compiler, output_dir, test_name, use_heaptrack=True):
    """
    Create a wrapper script that profiles each compiler invocation.
    Returns path to the wrapper script.

    The wrapper is named 'armv8m-tcc' so the Makefile correctly detects it as TinyCC.
    """
    # Name the wrapper armv8m-tcc so Makefile detection works
    wrapper_path = output_dir / "armv8m-tcc"
    metrics_file = output_dir / f"metrics_{test_name}.txt"

    # For heaptrack mode, we don't wrap - we'll run heaptrack separately
    # on a direct compiler invocation after getting the command from make --dry-run
    wrapper_content = f'''#!/bin/bash
# Profile wrapper for TinyCC compiler
METRICS_FILE="{metrics_file}"
REAL_COMPILER="{real_compiler}"

# Run compiler with GNU time, append metrics
/usr/bin/time -v -a -o "$METRICS_FILE" "$REAL_COMPILER" "$@"
EXIT_CODE=$?

exit $EXIT_CODE
'''

    wrapper_path.write_text(wrapper_content)
    wrapper_path.chmod(0o755)

    return wrapper_path, metrics_file


def run_heaptrack_direct(compiler, source_files, output_dir, test_name, cflags):
    """
    Run heaptrack directly on the compiler for a single compilation.
    This avoids wrapper issues by invoking heaptrack -> compiler directly.
    """
    heaptrack_out = output_dir / f"heaptrack_{test_name}"

    # Build the compiler command for compiling (not linking)
    cmd = [
        "heaptrack", "--record-only", "-o", str(heaptrack_out),
        str(compiler),
    ] + cflags + ["-c"] + [str(f) for f in source_files] + ["-o", "/dev/null"]

    result = subprocess.run(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=60,
    )

    return result, heaptrack_out


def build_compile_command(test_file, machine, output_dir, compiler):
    """Build the make command for compiling a test case."""
    make_dir = CURRENT_DIR / 'qemu' / machine
    test_files = _as_file_list(test_file)
    test_files_resolved = [str(CURRENT_DIR / Path(f)) for f in test_files]
    test_files_value = " ".join(test_files_resolved)

    output_file = output_dir / f"{_test_id(test_file)}.elf"

    cmd = [
        "make",
        "-C", str(make_dir),
        f"OUTPUT={output_dir}",
        f"TEST_FILES={test_files_value}",
        f"CC={compiler}",
        f"TARGET={output_file}",
    ]
    return cmd, output_file


def parse_time_metrics_file(metrics_file):
    """Parse aggregated GNU time output from metrics file."""
    metrics = {
        'user_time': 0.0,
        'sys_time': 0.0,
        'max_rss_kb': 0,
    }

    if not metrics_file.exists():
        return metrics

    content = metrics_file.read_text()

    # Aggregate metrics from multiple invocations
    max_rss_values = []
    for line in content.split('\n'):
        if 'User time' in line:
            match = re.search(r'(\d+\.?\d*)', line)
            if match:
                metrics['user_time'] += float(match.group(1))
        elif 'System time' in line:
            match = re.search(r'(\d+\.?\d*)', line)
            if match:
                metrics['sys_time'] += float(match.group(1))
        elif 'Maximum resident set size' in line:
            match = re.search(r'(\d+)', line)
            if match:
                max_rss_values.append(int(match.group(1)))

    if max_rss_values:
        metrics['max_rss_kb'] = max(max_rss_values)

    return metrics


def parse_heaptrack_output(heaptrack_file):
    """Parse heaptrack output using heaptrack_print."""
    metrics = {
        'heap_peak_kb': 0,
        'allocations': 0,
        'temporary_allocs': 0,
    }

    # Find the actual output file (heaptrack adds process ID and .gz extension)
    parent = heaptrack_file.parent
    pattern = heaptrack_file.name + "*.gz"
    matches = list(parent.glob(pattern))

    if not matches:
        return metrics, ""

    # If multiple files (multiple compiler invocations), aggregate them
    all_files = sorted(matches)
    gz_file = all_files[-1]  # Use the last one for the file reference

    total_allocations = 0
    total_temporary = 0
    max_peak = 0

    for gzf in all_files:
        result = subprocess.run(
            ["heaptrack_print", str(gzf)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

        output = result.stdout.decode(errors='replace')

        for line in output.split('\n'):
            if 'peak heap memory consumption' in line.lower():
                match = re.search(r'(\d+\.?\d*)\s*([KMGBkmgb])?', line.split(':')[-1])
                if match:
                    value = float(match.group(1))
                    unit = (match.group(2) or 'B').upper()
                    if unit == 'K':
                        pass  # already in KB
                    elif unit == 'M':
                        value *= 1024
                    elif unit == 'G':
                        value *= 1024 * 1024
                    elif unit == 'B':
                        value /= 1024
                    max_peak = max(max_peak, int(value))
            elif 'calls to allocation functions' in line.lower():
                match = re.search(r'(\d+)', line)
                if match:
                    total_allocations += int(match.group(1))
            elif 'temporary allocations' in line.lower():
                match = re.search(r'(\d+)', line)
                if match:
                    total_temporary += int(match.group(1))

    metrics['heap_peak_kb'] = max_peak
    metrics['allocations'] = total_allocations
    metrics['temporary_allocs'] = total_temporary

    return metrics, str(gz_file)


def get_binary_size(elf_file):
    """Get binary size metrics using arm-none-eabi-size."""
    metrics = {
        'text': 0,
        'data': 0,
        'bss': 0,
        'total': 0,
    }

    if not Path(elf_file).exists():
        return metrics

    result = subprocess.run(
        ["arm-none-eabi-size", str(elf_file)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )

    if result.returncode == 0:
        output = result.stdout.decode()
        lines = output.strip().split('\n')
        if len(lines) >= 2:
            # Format: text    data     bss     dec     hex filename
            parts = lines[1].split()
            if len(parts) >= 4:
                metrics['text'] = int(parts[0])
                metrics['data'] = int(parts[1])
                metrics['bss'] = int(parts[2])
                metrics['total'] = int(parts[3])

    return metrics


def get_compiler_cflags(machine):
    """Get the CFLAGS used for TinyCC compilation."""
    make_dir = CURRENT_DIR / 'qemu' / machine
    tcc_path = (CURRENT_DIR / "../..").resolve()
    libc_includes = (CURRENT_DIR / "../libc_includes").resolve()

    # Get ARM sysroot
    result = subprocess.run(
        ["arm-none-eabi-gcc", "-mcpu=cortex-m33", "-mthumb", "-mfloat-abi=soft", "--print-sysroot"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    arm_sysroot = result.stdout.decode().strip()

    cflags = [
        "-nostdlib", "-g", "-fvisibility=hidden",
        "-mcpu=cortex-m33", "-mthumb", "-mfloat-abi=soft",
        "-gdwarf", "-ffunction-sections",
        f"-I{libc_includes}",
        f"-I{arm_sysroot}/include",
        f"-I{tcc_path}/include",
    ]
    return cflags


def profile_test(test_file, output_dir, use_heaptrack=True):
    """Profile a single test compilation."""
    test_name = _test_id(test_file)
    result = ProfileResult(test_name=test_name)

    build_dir = output_dir / "build"
    build_dir.mkdir(parents=True, exist_ok=True)

    real_compiler = (CURRENT_DIR / "../../armv8m-tcc").resolve()

    # Create profiler wrapper (for GNU time mode)
    wrapper_path, metrics_file = create_profiler_wrapper(
        real_compiler, output_dir, test_name, use_heaptrack
    )

    # Clean before build (use real compiler for clean)
    clean_cmd, _ = build_compile_command(test_file, MACHINE, build_dir, str(real_compiler))
    subprocess.run(clean_cmd + ["clean"], stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    # Remove old metrics/heaptrack files for this test
    if metrics_file.exists():
        metrics_file.unlink()
    for old_ht in output_dir.glob(f"heaptrack_{test_name}*.gz"):
        old_ht.unlink()

    try:
        if use_heaptrack:
            # For heaptrack: run direct compilation with heaptrack, then do full build
            test_files = _as_file_list(test_file)
            source_files = [CURRENT_DIR / Path(f) for f in test_files]
            cflags = get_compiler_cflags(MACHINE)

            # Run heaptrack on direct compilation
            start = time.perf_counter()
            ht_result, heaptrack_out = run_heaptrack_direct(
                real_compiler, source_files, output_dir, test_name, cflags
            )
            ht_elapsed = time.perf_counter() - start

            if ht_result.returncode != 0:
                result.success = False
                result.error = ht_result.stderr.decode(errors='replace')[:500]
            else:
                # Parse heaptrack results
                ht_metrics, gz_file = parse_heaptrack_output(heaptrack_out)
                result.heap_peak_kb = ht_metrics['heap_peak_kb']
                result.heap_allocations = ht_metrics['allocations']
                result.heap_temporary_allocs = ht_metrics['temporary_allocs']
                result.heaptrack_file = gz_file

            # Now do actual full build (without heaptrack) for binary size
            make_cmd, elf_file = build_compile_command(test_file, MACHINE, build_dir, str(real_compiler))
            subprocess.run(make_cmd + ["clean"], stdout=subprocess.PIPE, stderr=subprocess.PIPE)

            start = time.perf_counter()
            proc_result = subprocess.run(make_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            elapsed = time.perf_counter() - start

            result.compile_time_s = elapsed

            if proc_result.returncode != 0:
                result.success = False
                result.error = proc_result.stderr.decode(errors='replace')[:500]

        else:
            # GNU time mode: use wrapper
            make_cmd, elf_file = build_compile_command(test_file, MACHINE, build_dir, str(wrapper_path))

            start = time.perf_counter()
            proc_result = subprocess.run(
                make_cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            elapsed = time.perf_counter() - start

            result.compile_time_s = elapsed

            if proc_result.returncode != 0:
                result.success = False
                stderr = proc_result.stderr.decode(errors='replace')
                stdout = proc_result.stdout.decode(errors='replace')
                result.error = (stderr + stdout)[:500]
            else:
                # Parse GNU time metrics
                time_metrics = parse_time_metrics_file(metrics_file)
                result.user_time_s = time_metrics['user_time']
                result.sys_time_s = time_metrics['sys_time']
                result.max_rss_kb = time_metrics['max_rss_kb']

        # Get binary size metrics
        if result.success:
            size_metrics = get_binary_size(elf_file)
            result.text_size = size_metrics['text']
            result.data_size = size_metrics['data']
            result.bss_size = size_metrics['bss']
            result.total_size = size_metrics['total']

    except Exception as e:
        result.success = False
        result.error = str(e)

    return result


def print_result(result: ProfileResult, idx: int, total: int):
    """Print a single result to console."""
    status = "OK" if result.success else "FAIL"

    if result.heap_peak_kb > 0:
        mem_str = f"heap_peak={result.heap_peak_kb}KB"
    else:
        mem_str = f"max_rss={result.max_rss_kb}KB"

    print(f"[{idx:3d}/{total}] {result.test_name:40s} {status:4s} "
          f"time={result.compile_time_s:.3f}s {mem_str} "
          f"bin={result.total_size}B")


def write_summary(results, output_dir):
    """Write summary CSV and JSON files."""
    # CSV
    csv_file = output_dir / "summary.csv"
    with open(csv_file, 'w', newline='') as f:
        if results:
            writer = csv.DictWriter(f, fieldnames=asdict(results[0]).keys())
            writer.writeheader()
            for r in results:
                writer.writerow(asdict(r))

    # JSON
    json_file = output_dir / "summary.json"
    with open(json_file, 'w') as f:
        json.dump([asdict(r) for r in results], f, indent=2)

    # Summary stats
    successful = [r for r in results if r.success]
    if successful:
        total_time = sum(r.compile_time_s for r in successful)
        max_heap = max(r.heap_peak_kb for r in successful) if successful[0].heap_peak_kb > 0 else 0
        max_rss = max(r.max_rss_kb for r in successful)
        total_bin_size = sum(r.total_size for r in successful)

        print("\n" + "=" * 70)
        print("SUMMARY")
        print("=" * 70)
        print(f"Tests run:        {len(results)}")
        print(f"Tests passed:     {len(successful)}")
        print(f"Tests failed:     {len(results) - len(successful)}")
        print(f"Total compile time: {total_time:.2f}s")
        if max_heap > 0:
            print(f"Max heap peak:    {max_heap} KB ({max_heap/1024:.2f} MB)")
        print(f"Max RSS:          {max_rss} KB ({max_rss/1024:.2f} MB)")
        print(f"Total binary size: {total_bin_size} bytes ({total_bin_size/1024:.2f} KB)")
        print(f"\nResults saved to: {output_dir}")
        print(f"  - {csv_file.name}")
        print(f"  - {json_file.name}")
        if successful[0].heaptrack_file:
            print(f"  - heaptrack_*.gz files (open with heaptrack_gui for flamegraphs)")


def main():
    parser = argparse.ArgumentParser(description="Profile TinyCC compiler across test suite")
    parser.add_argument("--output-dir", "-o", type=Path, default=DEFAULT_OUTPUT_DIR,
                        help="Output directory for profile data")
    parser.add_argument("--limit", "-n", type=int, default=0,
                        help="Limit number of tests to run (0 = all)")
    parser.add_argument("--heaptrack", dest="heaptrack", action="store_true", default=True,
                        help="Use heaptrack for memory profiling (default)")
    parser.add_argument("--no-heaptrack", dest="heaptrack", action="store_false",
                        help="Use GNU time only (faster, less detailed)")
    parser.add_argument("--include-float", action="store_true",
                        help="Include floating point tests")
    parser.add_argument("--test", "-t", type=str,
                        help="Run only test matching this pattern")
    args = parser.parse_args()

    # Prepare output directory
    args.output_dir.mkdir(parents=True, exist_ok=True)

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
    print(f"Memory profiler: {'heaptrack' if args.heaptrack else 'GNU time'}")
    print("=" * 70)

    results = []
    for idx, (test_file, _) in enumerate(all_tests, 1):
        result = profile_test(test_file, args.output_dir, use_heaptrack=args.heaptrack)
        results.append(result)
        print_result(result, idx, len(all_tests))

    write_summary(results, args.output_dir)

    return 0 if all(r.success for r in results) else 1


if __name__ == "__main__":
    sys.exit(main())
