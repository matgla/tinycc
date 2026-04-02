#!/usr/bin/env python3
"""
Profile TinyCC compiler memory usage and performance across the test suite.

Uses the unified qemu_run.py infrastructure with profiling support.

Usage:
    python profile_suite.py [--output-dir DIR] [--limit N] [--profiler heaptrack|callgrind|time|perf] [--cflags "..."] [--auto-pch] [--perf-raw-only]

Output:
    - profile_results/heaptrack_*.zst - heaptrack data files (use heaptrack_gui to view)
    - profile_results/callgrind_*.out - callgrind data files (use kcachegrind/qcachegrind/callgrind_annotate)
    - profile_results/time_*.txt      - GNU time output files
    - profile_results/perf_*.data     - perf data files (use perf report/perf script to view)
    - profile_results/perf_*.svg      - CPU flamegraph SVG files (open in browser, unless --perf-raw-only)
    - profile_results/summary.csv     - CSV with all metrics
    - profile_results/summary.json    - JSON with all metrics
"""

import argparse
import csv
import json
import os
import shutil
import subprocess
import sys
from dataclasses import asdict
from pathlib import Path

from _venv_bootstrap import ensure_venv

ensure_venv()

from qemu_run import (
    compile_testcase,
    CompileConfig,
    ProfileConfig,
    CompileResult,
    reset_clean_state,
    CURRENT_DIR,
    DEFAULT_PERF_FREQUENCY,
)
from test_qemu import (
    FLOAT_TEST_FILES,
    TAGGED_TEST_FILES,
    TEST_FILES,
    TEST_FILES_WITH_ARGS,
    load_tagged_expect_file,
)

DEFAULT_OUTPUT_DIR = CURRENT_DIR / "profile_results"
MACHINE = "mps2-an505"
REPO_ROOT = CURRENT_DIR.parent.parent
PROFILE_AUTO_PCH_HEADERS = ("stdio.h", "stdlib.h", "string.h")


def _target_subdir_name(compiler: Path) -> str:
    name = compiler.name
    if name.endswith("-tcc"):
        return name[:-3]
    if name == "tcc":
        return "native"
    return f"{compiler.stem}-"


def _arm_sysroot_include() -> Path | None:
    result = subprocess.run(
        ["arm-none-eabi-gcc", "-mcpu=cortex-m33", "-mthumb", "-mfloat-abi=soft", "--print-sysroot"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if result.returncode != 0:
        return None
    sysroot = result.stdout.strip()
    if not sysroot:
        return None
    include_dir = Path(sysroot) / "include"
    return include_dir if include_dir.is_dir() else None


def cleanup_profile_auto_pch(compiler: Path) -> None:
    compiler = compiler.resolve()
    pch_dir = REPO_ROOT / "pch" / _target_subdir_name(compiler)
    if not pch_dir.exists():
        return

    index_path = pch_dir / "auto.index"
    if index_path.exists():
        filtered_lines = [
            line for line in index_path.read_text().splitlines()
            if "\tprofile-" not in line
        ]
        if filtered_lines:
            index_path.write_text("\n".join(filtered_lines) + "\n")
        else:
            index_path.unlink()

    for pch_path in pch_dir.glob("profile-*.pch"):
        pch_path.unlink()

    probe_dir = pch_dir / ".profile-suite-probes"
    if probe_dir.exists():
        shutil.rmtree(probe_dir, ignore_errors=True)


def ensure_profile_auto_pch(compiler: Path) -> None:
    compiler = compiler.resolve()
    if not compiler.exists():
        return

    cleanup_profile_auto_pch(compiler)

    pch_dir = REPO_ROOT / "pch" / _target_subdir_name(compiler)
    pch_dir.mkdir(parents=True, exist_ok=True)

    libc_includes = CURRENT_DIR / "libc_includes"
    libc_imports = CURRENT_DIR / "libc_imports"
    newlib_includes = libc_includes / "newlib"
    include_dirs = [libc_includes, libc_imports, newlib_includes]
    sysroot_include = _arm_sysroot_include()
    if sysroot_include is not None:
        include_dirs.append(sysroot_include)
    include_dirs.append(REPO_ROOT / "include")

    common_flags = [
        "-nostdlib",
        "-fvisibility=hidden",
        "-mcpu=cortex-m33",
        "-mthumb",
        "-mfloat-abi=soft",
        "-ffunction-sections",
    ]

    filtered_index_lines = []
    index_path = pch_dir / "auto.index"
    if index_path.exists():
        for line in index_path.read_text().splitlines():
            if "\tprofile-" not in line:
                filtered_index_lines.append(line)

    probe_dir = pch_dir / ".profile-suite-probes"
    probe_dir.mkdir(parents=True, exist_ok=True)
    generated_lines = []

    for header in PROFILE_AUTO_PCH_HEADERS:
        header_path = (newlib_includes / header).resolve()
        if not header_path.exists():
            continue

        pch_name = f"profile-{header}.pch"
        pch_path = pch_dir / pch_name
        probe_path = probe_dir / f"{header}.c"
        probe_path.write_text(f'#include <{header}>\nint main(void) {{ return 0; }}\n')

        generate_cmd = [str(compiler), f"-B{REPO_ROOT}", *common_flags]
        for include_dir in include_dirs:
            generate_cmd.extend(["-I", str(include_dir)])
        generate_cmd.extend(["-generate-pch", str(header_path), "-o", str(pch_path)])
        generate = subprocess.run(generate_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if generate.returncode != 0 or not pch_path.exists():
            if pch_path.exists():
                pch_path.unlink()
            continue

        candidate_lines = [*filtered_index_lines, *generated_lines, f"{header_path}\t{pch_name}"]
        index_path.write_text("\n".join(candidate_lines) + "\n")

        validate_cmd = [str(compiler), f"-B{REPO_ROOT}", *common_flags]
        for include_dir in include_dirs:
            validate_cmd.extend(["-I", str(include_dir)])
        validate_cmd.extend(["-c", str(probe_path), "-o", str(probe_dir / f"{header}.o")])
        validate = subprocess.run(validate_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        combined = (validate.stdout or "") + (validate.stderr or "")
        if validate.returncode != 0 or "ignoring PCH" in combined:
            if pch_path.exists():
                pch_path.unlink()
            index_path.write_text("\n".join(filtered_index_lines + generated_lines) + ("\n" if filtered_index_lines or generated_lines else ""))
            continue

        generated_lines.append(f"{header_path}\t{pch_name}")

    final_lines = filtered_index_lines + generated_lines
    if final_lines:
        index_path.write_text("\n".join(final_lines) + "\n")
    elif index_path.exists():
        index_path.unlink()


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


def _test_path(test_file):
    primary = _primary_file(test_file)
    if primary is None:
        return ""
    primary_path = Path(primary)
    try:
        return str(primary_path.relative_to(CURRENT_DIR))
    except ValueError:
        return str(primary_path)


def _test_suite(test_file):
    test_path = _test_path(test_file)
    if test_path.startswith("../tests2/"):
        return "tests2"
    return "ir_tests"


def _sanitize_tag_for_filename(tag: str) -> str:
    return "".join(char if char.isalnum() or char == "_" else "_" for char in tag).strip("_")


def _make_profile_case(test_file, *, test_name: str | None = None, artifact_name: str | None = None, build_suffix: str = "", defines=None):
    resolved_test_name = test_name or _test_id(test_file)
    return {
        "test_file": test_file,
        "test_name": resolved_test_name,
        "artifact_name": artifact_name or resolved_test_name,
        "build_suffix": build_suffix,
        "defines": list(defines or []),
        "expected_lines": None,
    }


def _collect_profile_cases(include_float: bool) -> list[dict]:
    cases = [_make_profile_case(test_file) for test_file, _ in TEST_FILES]

    if include_float:
        cases.extend(_make_profile_case(test_file) for test_file, _ in FLOAT_TEST_FILES)

    for test_file, _args, _expected in TEST_FILES_WITH_ARGS:
        if _test_suite(test_file) == "tests2":
            cases.append(_make_profile_case(test_file, build_suffix="_args"))

    for test_file in TAGGED_TEST_FILES:
        if _test_suite(test_file) != "tests2":
            continue
        tag_data = load_tagged_expect_file(test_file)
        for tag, data in tag_data.items():
            safe_tag = _sanitize_tag_for_filename(tag)
            case = _make_profile_case(
                test_file,
                test_name=f"{_test_id(test_file)}[{tag}]",
                artifact_name=f"{_test_id(test_file)}_{safe_tag}",
                build_suffix=f"_{safe_tag}",
                defines=[tag],
            )
            case["expected_lines"] = data["lines"]
            cases.append(case)

    return cases


def _tagged_case_passed(result: CompileResult, test_file, expected_lines: list[str] | None) -> bool:
    if expected_lines is None:
        return result.success

    source_basename = Path(_primary_file(test_file)).name
    compile_expected = [
        line for line in expected_lines
        if line and source_basename in line
    ]
    compiler_output = "\n".join(result.output_lines)

    for line in compile_expected:
        if line not in compiler_output:
            return False

    if result.success:
        return True

    if not expected_lines and "undefined symbol 'main'" in compiler_output:
        return True

    return bool(compile_expected)


def profile_test(test_file, output_dir, profiler_tool="heaptrack", extra_cflags: str = "", compiler: Path = None, two_phase: bool = False, perf_postprocess: bool = True, perf_frequency: int = DEFAULT_PERF_FREQUENCY, defines=None, test_name: str | None = None, artifact_name: str | None = None, build_suffix: str = ""):
    """Profile a single test compilation."""
    resolved_test_name = test_name or _test_id(test_file)
    output_prefix = artifact_name or resolved_test_name

    # Resolve test files
    test_files = _as_file_list(test_file)
    source_files = [CURRENT_DIR / Path(f) for f in test_files]

    # Configure profiling
    profile_config = ProfileConfig(
        tool=profiler_tool,
        output_dir=output_dir,
        output_prefix=output_prefix,
        perf_frequency=perf_frequency,
        perf_postprocess=perf_postprocess,
    )

    config = CompileConfig(
        compiler=compiler,
        profiler=profile_config,
        extra_cflags=extra_cflags or "",
        two_phase=two_phase,
        defines=defines,
        output_dir=output_dir / "build",
        clean_before_build=True,
        output_suffix=build_suffix,
    )

    # Compile with profiling
    result = compile_testcase(source_files, MACHINE, config=config)

    return result, resolved_test_name


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
    elif result.callgrind_summary > 0:
        event_name = result.callgrind_event or "events"
        extra = f" {event_name}={result.callgrind_summary:,}"
    if result.flamegraph_file:
        extra += " [flamegraph]"
    if result.pch_ignored:
        extra += " [pch-ignored]"

    print(f"[{idx:3d}/{total}] {test_name:40s} {status:4s} "
          f"time={result.compile_time_s:.3f}s {mem_str} "
          f"bin={result.total_size}B{extra}")


def result_to_dict(result: CompileResult, test_name: str, test_file) -> dict:
    """Convert CompileResult to dictionary for serialization."""
    return {
        "test_name": test_name,
        "test_suite": _test_suite(test_file),
        "test_path": _test_path(test_file),
        "success": result.success,
        "compile_time_s": result.compile_time_s,
        "user_time_s": result.user_time_s,
        "sys_time_s": result.sys_time_s,
        "max_rss_kb": result.max_rss_kb,
        "heap_peak_kb": result.heap_peak_kb,
        "heap_allocations": result.heap_allocations,
        "heap_temporary_allocs": result.heap_temporary_allocs,
        "callgrind_event": result.callgrind_event,
        "callgrind_summary": result.callgrind_summary,
        "perf_samples": result.perf_samples,
        "flamegraph_file": result.flamegraph_file,
        "profile_file": result.profile_file,
        "text_size": result.text_size,
        "data_size": result.data_size,
        "bss_size": result.bss_size,
        "total_size": result.total_size,
        "pch_ignored": result.pch_ignored,
        "pch_warning": result.pch_warning,
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
        callgrind_count = sum(1 for r in successful if r.get("callgrind_summary", 0) > 0)
        pch_ignored_count = sum(1 for r in results if r.get("pch_ignored"))

        print(f"\nResults saved to: {output_dir}")
        print(f"  - {csv_file.name}")
        print(f"  - {json_file.name}")
        if max_heap > 0:
            print(f"  - heaptrack_*.zst files (open with heaptrack_gui for memory flamegraphs)")
        if callgrind_count > 0:
            print(f"  - {callgrind_count} callgrind_*.out file(s) (open with kcachegrind/qcachegrind or callgrind_annotate)")
        if flamegraph_count > 0:
            print(f"  - {flamegraph_count} perf_*.svg flamegraph(s) (open in browser for CPU profiling)")
        if pch_ignored_count > 0:
            print(f"  - warning: {pch_ignored_count} compile(s) reported ignored PCH")


def main():
    parser = argparse.ArgumentParser(description="Profile TinyCC compiler across test suite")
    parser.add_argument("--output-dir", "-o", type=Path, default=DEFAULT_OUTPUT_DIR,
                        help="Output directory for profile data")
    parser.add_argument("--limit", "-n", type=int, default=0,
                        help="Limit number of tests to run (0 = all)")
    default_profiler = "time" if sys.platform == "darwin" else "heaptrack"
    profiler_choices = ["heaptrack", "callgrind", "time", "perf"]
    if sys.platform == "darwin":
        profiler_choices.append("xctrace")
    parser.add_argument("--profiler", "-p", choices=profiler_choices, default=default_profiler,
                        help=f"Profiler tool to use (default: {default_profiler})")
    parser.add_argument("--include-float", action="store_true",
                        help="Include floating point tests")
    parser.add_argument("--cflags", type=str, default="",
                        help="Additional CFLAGS to pass to the compiler (e.g. '-Wl,--gc-sections-aggressive')")
    parser.add_argument("--test", "-t", type=str,
                        help="Run only test matching this pattern")
    parser.add_argument("--compiler", "-c", type=Path, default=None,
                        help="Path to compiler binary (default: use armv8m-tcc from repo root)")
    parser.add_argument("--two-phase", action="store_true",
                        help="Use two-phase compilation (reduces memory usage)")
    parser.add_argument(
        "--auto-pch",
        action="store_true",
        help=(
            "Enable profiling-only auto-PCH generation for common libc headers. "
            "Disabled by default because overlapping per-header PCHs can replay "
            "duplicate declarations and fail otherwise valid tests."
        ),
    )
    parser.add_argument(
        "--perf-raw-only",
        action="store_true",
        help=(
            "With --profiler perf, keep raw perf_*.data files in profile_results and skip "
            "perf report/flamegraph post-processing."
        ),
    )
    parser.add_argument(
        "--perf-frequency",
        type=int,
        default=DEFAULT_PERF_FREQUENCY,
        help=(
            "Sampling frequency for perf in Hz. Defaults to a higher rate so short "
            "sub-second compiler runs still collect useful sample counts."
        ),
    )
    args = parser.parse_args()

    # Prepare output directory
    args.output_dir.mkdir(parents=True, exist_ok=True)

    compiler_path = args.compiler.resolve() if args.compiler else (CURRENT_DIR / "../../armv8m-tcc").resolve()
    cleanup_profile_auto_pch(compiler_path)
    if args.auto_pch:
        ensure_profile_auto_pch(compiler_path)

    # Reset clean state for fresh profiling run
    reset_clean_state()

    # Collect all tests
    all_tests = _collect_profile_cases(args.include_float)

    # Filter by pattern if specified
    if args.test:
        all_tests = [
            case for case in all_tests
            if args.test in case["test_name"] or args.test in _test_path(case["test_file"])
        ]

    # Apply limit
    if args.limit > 0:
        all_tests = all_tests[:args.limit]

    print(f"Profiling {len(all_tests)} tests")
    print(f"Output directory: {args.output_dir}")
    print(f"Profiler: {args.profiler}")
    if args.profiler == "perf":
        print(f"Perf frequency: {args.perf_frequency} Hz")
    if args.profiler == "perf" and args.perf_raw_only:
        print("Perf post-processing: disabled (raw perf.data only)")
    if args.compiler:
        print(f"Compiler: {args.compiler}")
    if args.cflags:
        print(f"Extra CFLAGS: {args.cflags}")
    print("=" * 70)

    results = []
    for idx, case in enumerate(all_tests, 1):
        test_file = case["test_file"]
        result, test_name = profile_test(
            test_file,
            args.output_dir,
            profiler_tool=args.profiler,
            extra_cflags=args.cflags,
            compiler=args.compiler,
            two_phase=args.two_phase,
            perf_postprocess=not args.perf_raw_only,
            perf_frequency=args.perf_frequency,
            defines=case["defines"],
            test_name=case["test_name"],
            artifact_name=case["artifact_name"],
            build_suffix=case["build_suffix"],
        )
        result.success = _tagged_case_passed(result, test_file, case.get("expected_lines"))
        result_dict = result_to_dict(result, test_name, test_file)
        results.append(result_dict)
        print_result(result, test_name, idx, len(all_tests))

    write_summary(results, args.output_dir)

    return 0 if all(r["success"] for r in results) else 1


if __name__ == "__main__":
    sys.exit(main())
