#!/usr/bin/env python3
"""
Unified test runner for armv8m-tcc.

This script runs test suites:
- gcctestsuite/ - GCC torture tests (default)
- ir_tests/ - IR-level tests (via --ir flag)
- tests2/ - C compliance tests (via --tests2 flag, not all executable!)
- frontend/ - Frontend tests (via --frontend flag)
- linker/ - Object/linker golden tests (via --linker flag)
- debug/ - Debug-info tests (via --debug flag)
- runtime/ - Runtime-library tests (via --runtime flag)
- selfhost/ - Self-host bootstrap gate (via --selfhost flag)

Note: tests2 tests are normally executed via ir_tests/test_qemu.py which runs
a curated subset. Using --tests2 runs ALL tests2 tests, some may fail.

Usage:
    python run_tests.py                      # Run GCC torture tests
    python run_tests.py --gcc                # Run only GCC torture tests
    python run_tests.py --ir                 # Run only IR tests
    python run_tests.py --tests2             # Run tests2 (not all executable!)
    python run_tests.py --frontend           # Run frontend tests
    python run_tests.py --linker             # Run linker tests
    python run_tests.py --debug              # Run debug-info tests
    python run_tests.py --runtime            # Run runtime-library tests
    python run_tests.py --selfhost           # Run self-host bootstrap gate
    python run_tests.py --download-gcc       # Download GCC tests first
    python run_tests.py -v -x                # Verbose, stop on first failure

Environment Variables:
    GCC_TORTURE_PATH    Path to GCC torture tests
    TCC_PATH            Path to armv8m-tcc compiler
"""

import argparse
import subprocess
import sys
import os
from pathlib import Path


# Test directories
TESTS_DIR = Path(__file__).parent
TESTS2_DIR = TESTS_DIR / "tests2"
GCC_DIR = TESTS_DIR / "gcctestsuite"
IR_DIR = TESTS_DIR / "ir_tests"
FRONTEND_DIR = TESTS_DIR / "frontend"
LINKER_DIR = TESTS_DIR / "linker"
DEBUG_DIR = TESTS_DIR / "debug"
RUNTIME_DIR = TESTS_DIR / "runtime"
SELFHOST_DIR = TESTS_DIR / "selfhost"


def run_pytest(test_dir: Path, markers: str = None, args: list = None, env: dict = None, verbose: bool = False) -> int:
    """Run pytest on a test directory."""
    # Use the same python interpreter that is running run_tests.py so that
    # an activated virtualenv (or any python with pytest installed) is reused.
    cmd = [sys.executable, "-m", "pytest", str(test_dir)]
    if verbose:
        cmd.append("-v")

    if markers:
        cmd.extend(["-m", markers])

    if args:
        cmd.extend(args)

    env = env or os.environ.copy()

    print(f"\n{'='*60}")
    print(f"Running: {' '.join(cmd)}")
    print(f"{'='*60}\n")

    result = subprocess.run(cmd, env=env)
    return result.returncode


def download_gcc_tests() -> bool:
    """Download GCC torture tests."""
    download_script = GCC_DIR / "download_gcc_tests.sh"
    if not download_script.exists():
        print(f"Download script not found: {download_script}")
        return False

    print("Downloading GCC torture tests...")
    result = subprocess.run(["bash", str(download_script)])
    return result.returncode == 0


def main():
    parser = argparse.ArgumentParser(
        description="Run unified test suite for armv8m-tcc",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python run_tests.py                      # Run GCC torture tests (default)
  python run_tests.py --gcc -v             # Run GCC torture tests
  python run_tests.py --gcc --compile-only # GCC compile tests only
  python run_tests.py --ir -n auto         # IR tests with parallel execution
  python run_tests.py --tests2             # Run tests2 (WARNING: not all executable!)
  python run_tests.py --frontend           # Run frontend tests
  python run_tests.py --linker             # Run linker tests
  python run_tests.py --debug              # Run debug-info tests
  python run_tests.py --runtime            # Run runtime-library tests
  python run_tests.py --selfhost           # Run self-host bootstrap gate
        """
    )

    # Test selection
    parser.add_argument("--tests2", action="store_true",
                        help="Run tests2 tests")
    parser.add_argument("--gcc", action="store_true",
                        help="Run GCC torture tests")
    parser.add_argument("--ir", action="store_true",
                        help="Run IR tests")
    parser.add_argument("--frontend", action="store_true",
                        help="Run frontend tests")
    parser.add_argument("--linker", action="store_true",
                        help="Run linker tests")
    parser.add_argument("--debug", action="store_true",
                        help="Run debug-info tests")
    parser.add_argument("--runtime", action="store_true",
                        help="Run runtime-library tests")
    parser.add_argument("--selfhost", action="store_true",
                        help="Run self-host bootstrap gate")
    parser.add_argument("--download-gcc", action="store_true",
                        help="Download GCC torture tests first")

    # Test type filters
    parser.add_argument("--compile-only", action="store_true",
                        help="Run only compile tests")
    parser.add_argument("--execute", action="store_true",
                        help="Run only execute tests")

    # Pytest passthrough options
    parser.add_argument("-v", "--verbose", action="store_true",
                        help="Verbose output")
    parser.add_argument("-k", "--keyword", type=str,
                        help="Run tests matching keyword")
    parser.add_argument("-x", "--exitfirst", action="store_true",
                        help="Stop on first failure")
    parser.add_argument("--tb", type=str, default="short",
                        help="Traceback style")
    parser.add_argument("-n", type=str, dest="numprocesses",
                        help="Number of parallel processes")
    parser.add_argument("--timeout", type=int, default=None,
                        help="Test timeout in seconds (requires pytest-timeout)")

    args, extra_args = parser.parse_known_args()

    # If no specific test suite selected, run GCC torture tests only
    # Note: tests2 tests are executed via ir_tests, not directly
    run_default = not (args.tests2 or args.gcc or args.ir or args.frontend or args.linker or args.debug or args.runtime or args.selfhost)

    # Download GCC tests if requested
    if args.download_gcc:
        if not download_gcc_tests():
            print("Failed to download GCC tests")
            return 1

    # Build pytest arguments
    pytest_args = []
    if args.verbose:
        pytest_args.append("-v")
    if args.keyword:
        pytest_args.extend(["-k", args.keyword])
    if args.exitfirst:
        pytest_args.append("-x")
    if args.tb:
        pytest_args.extend(["--tb", args.tb])
    if args.numprocesses:
        pytest_args.extend(["-n", args.numprocesses])
    if args.timeout is not None:
        pytest_args.extend(["--timeout", str(args.timeout)])
    pytest_args.extend(extra_args)

    # Determine markers
    markers = []
    if args.compile_only:
        markers.append("compile_only")
    if args.execute:
        markers.append("execute")
    marker_expr = " and ".join(markers) if markers else None

    # Run tests
    exit_codes = []

    # tests2 tests are executed via ir_tests/test_qemu.py, not directly here
    # They can still be run explicitly with --tests2 flag
    if args.tests2:
        print("\n" + "="*60)
        print("Running tests2 C compliance tests")
        print("="*60)
        print("WARNING: Not all tests2 tests may be executable!")
        print("The ir_tests suite runs a curated subset of tests2.\n")
        code = run_pytest(TESTS2_DIR, marker_expr, pytest_args, verbose=args.verbose)
        exit_codes.append(code)

    if run_default or args.gcc:
        # All GCC torture tests (compile + execute) are now in test_gcc_torture_ir.py
        gcc_torture_file = IR_DIR / "test_gcc_torture_ir.py"
        print("\n" + "="*60)
        print("Running GCC torture compile tests")
        print("="*60)
        compile_markers = "gcc_torture and gcc_compile"
        if markers:
            compile_markers = f"({compile_markers}) and ({markers})"
        code = run_pytest(gcc_torture_file, compile_markers, pytest_args, verbose=args.verbose)
        exit_codes.append(code)

        # Execute tests (need newlib for linking)
        if not args.compile_only:
            print("\n" + "="*60)
            print("Running GCC torture execute tests")
            print("="*60)
            execute_markers = "gcc_torture and gcc_execute"
            if markers:
                execute_markers = f"({execute_markers}) and ({markers})"
            code = run_pytest(gcc_torture_file, execute_markers, pytest_args, verbose=args.verbose)
            exit_codes.append(code)

    if run_default or args.ir:
        print("\n" + "="*60)
        print("Running IR tests")
        print("="*60)
        # IR tests may have different requirements
        ir_args = pytest_args.copy()
        if args.numprocesses and "-n" not in ir_args:
            ir_args.extend(["-n", args.numprocesses])
        code = run_pytest(IR_DIR, marker_expr, ir_args, verbose=args.verbose)
        exit_codes.append(code)

    if args.frontend:
        print("\n" + "="*60)
        print("Running frontend tests")
        print("="*60)
        code = run_pytest(FRONTEND_DIR, marker_expr, pytest_args, verbose=args.verbose)
        exit_codes.append(code)

    if args.linker:
        print("\n" + "="*60)
        print("Running linker tests")
        print("="*60)
        code = run_pytest(LINKER_DIR, marker_expr, pytest_args, verbose=args.verbose)
        exit_codes.append(code)

    if args.debug:
        print("\n" + "="*60)
        print("Running debug-info tests")
        print("="*60)
        code = run_pytest(DEBUG_DIR, marker_expr, pytest_args, verbose=args.verbose)
        exit_codes.append(code)

    if args.runtime:
        print("\n" + "="*60)
        print("Running runtime-library tests")
        print("="*60)
        code = run_pytest(RUNTIME_DIR, marker_expr, pytest_args, verbose=args.verbose)
        exit_codes.append(code)

    if args.selfhost:
        print("\n" + "="*60)
        print("Running self-host bootstrap gate")
        print("="*60)
        code = run_pytest(SELFHOST_DIR, marker_expr, pytest_args, verbose=args.verbose)
        exit_codes.append(code)

    # Summary
    print("\n" + "="*60)
    print("Test Run Summary")
    print("="*60)
    print(f"Test suites run: {len([c for c in exit_codes if c is not None])}")
    print(f"Failures: {sum(1 for c in exit_codes if c != 0)}")

    return max(exit_codes) if exit_codes else 0


if __name__ == "__main__":
    sys.exit(main())
