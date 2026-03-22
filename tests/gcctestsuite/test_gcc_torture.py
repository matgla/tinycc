"""
GCC Torture Test Suite for armv8m-tcc.

This test suite runs the GCC c-torture tests against armv8m-tcc.
Tests are auto-discovered from GCC_TORTURE_PATH.

Run with:
    pytest tests/gcctestsuite/ -v              # All GCC tests
    pytest tests/gcctestsuite/ -v -m gcc_compile   # Compile-only tests
    pytest tests/gcctestsuite/ -v -m gcc_execute   # Execute tests

Environment:
    GCC_TORTURE_PATH    Path to GCC torture tests
"""

import pytest
import re
import resource
import subprocess
import sys
from pathlib import Path

from conftest import (
    GCCTestCase, GCC_TORTURE_PATH, OPT_LEVELS,
    discover_gcc_compile_tests, discover_gcc_execute_tests,
    should_skip_gcc_test, is_xfail_test
)

# Add ir_tests to path for qemu_run
IR_TESTS_DIR = Path(__file__).parent.parent / "ir_tests"
if str(IR_TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(IR_TESTS_DIR))

# Try to import qemu_run
try:
    from qemu_run import compile_testcase, CompileConfig
    QEMU_AVAILABLE = True
except ImportError:
    QEMU_AVAILABLE = False


# ============================================================================
# Test Execution Functions
# ============================================================================

def _compile_test(test_case: GCCTestCase, opt_level: str, tmp_path: Path) -> tuple[bool, str]:
    """Compile a test and return `(success, compiler_output)`."""
    extra_flags = opt_level
    if test_case.dg_options:
        extra_flags = f"{opt_level} {test_case.dg_options}"
    # Compile-only torture tests should only check frontend/codegen acceptance.
    # They often intentionally omit `main()`, so routing them through the QEMU
    # helper (which links a full ELF) turns valid compile tests into spurious
    # link failures.
    if QEMU_AVAILABLE and test_case.category != "gcc_compile":
        config = CompileConfig(
            extra_cflags=extra_flags,
            output_dir=tmp_path,
            clean_before_build=False,
            timeout=test_case.timeout
        )
        result = compile_testcase([test_case.source], "mps2-an505", config=config)
        output = result.error if result.error else "\n".join(result.output_lines)
        return result.success, output
    else:
        # Direct compiler invocation for compile-only tests and as a fallback.
        compiler = Path(__file__).parent.parent.parent / "armv8m-tcc"
        if not compiler.exists():
            compiler = Path(__file__).parent.parent.parent / "bin" / "armv8m-tcc"
        cmd = [
            str(compiler),
            *extra_flags.split(),
            "-c",
            str(test_case.source),
            "-o",
            str(tmp_path / "test.o")
        ]

        def _raise_stack_limit():
            try:
                soft, hard = resource.getrlimit(resource.RLIMIT_STACK)
                target = hard if hard != resource.RLIM_INFINITY else resource.RLIM_INFINITY
                resource.setrlimit(resource.RLIMIT_STACK, (target, hard))
            except Exception:
                pass

        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=test_case.timeout,
            preexec_fn=_raise_stack_limit,
        )
        return result.returncode == 0, (result.stderr or "") + (result.stdout or "")


def _assert_expected_diagnostics(test_case: GCCTestCase, output: str) -> None:
    """Validate that expected dg-error regexes are present in compiler output."""
    expected_patterns = sorted({pattern for pattern in test_case.expected_error_patterns if pattern})
    if not expected_patterns:
        return

    missing = []
    for pattern in expected_patterns:
        actual_count = len(list(re.finditer(pattern, output, re.MULTILINE)))
        if actual_count < 1:
            missing.append(f"{pattern!r} (expected at least 1 match, found 0)")

    assert not missing, (
        "Compilation failed, but expected diagnostics were missing:\n"
        + "\n".join(missing)
        + "\n\nCompiler output:\n"
        + output
    )


def run_compile_test(test_case: GCCTestCase, opt_level: str, tmp_path: Path) -> None:
    """Run a compile-only test, including expected-failure tests."""
    success, output = _compile_test(test_case, opt_level, tmp_path)

    if test_case.expected_compile_failure:
        assert not success, "Compilation unexpectedly succeeded for expected-failure test"
        _assert_expected_diagnostics(test_case, output)
    else:
        assert success, f"Compilation failed:\n{output}"


def run_execute_test(test_case: GCCTestCase, opt_level: str, tmp_path: Path) -> None:
    """Run an execute test (compile + link, skip execution for now)."""
    # For now, compile and link only - execution requires expected output handling
    # TODO: Add execution with proper expected output comparison
    run_compile_test(test_case, opt_level, tmp_path)


# ============================================================================
# GCC Compile Tests
# ============================================================================

GCC_COMPILE_TESTS = discover_gcc_compile_tests()


def _generate_compile_params():
    """Generate test parameters for GCC compile tests."""
    params = []
    ids = []
    for test_case in GCC_COMPILE_TESTS:
        skip_reason = should_skip_gcc_test(test_case.source)
        if skip_reason:
            test_case.skip_reason = skip_reason

        xfail_reason = is_xfail_test(test_case.source)
        if xfail_reason:
            test_case.xfail_reason = xfail_reason

        for opt in OPT_LEVELS:
            params.append((test_case, opt))
            ids.append(f"{test_case.source.stem}{opt}")
    return params, ids


_GCC_COMPILE_PARAMS, _GCC_COMPILE_IDS = _generate_compile_params() if GCC_COMPILE_TESTS else ([], [])


@pytest.mark.gcc_torture
@pytest.mark.gcc_compile
@pytest.mark.skipif(not GCC_TORTURE_PATH.exists(), reason="GCC torture tests not found")
@pytest.mark.parametrize("test_case,opt_level", _GCC_COMPILE_PARAMS, ids=_GCC_COMPILE_IDS)
def test_gcc_compile(test_case: GCCTestCase, opt_level: str, tmp_path):
    """Compile GCC torture tests (compile directory)."""
    if test_case.skip_reason:
        pytest.skip(test_case.skip_reason)

    if test_case.xfail_reason:
        pytest.xfail(test_case.xfail_reason)

    run_compile_test(test_case, opt_level, tmp_path)


# Placeholder when tests not available
if not GCC_COMPILE_TESTS:
    @pytest.mark.gcc_torture
    @pytest.mark.gcc_compile
    @pytest.mark.skip(reason="GCC compile tests not available - run 'make download-gcc-tests'")
    def test_gcc_compile__no_tests():
        """Placeholder when GCC tests are not available."""
        pass


# ============================================================================
# GCC Execute Tests
# ============================================================================

GCC_EXECUTE_TESTS = discover_gcc_execute_tests()


def _generate_execute_params():
    """Generate test parameters for GCC execute tests."""
    params = []
    ids = []
    for test_case in GCC_EXECUTE_TESTS:
        skip_reason = should_skip_gcc_test(test_case.source)
        if skip_reason:
            test_case.skip_reason = skip_reason

        for opt in OPT_LEVELS:
            params.append((test_case, opt))
            ids.append(f"{test_case.source.stem}{opt}")
    return params, ids


_GCC_EXECUTE_PARAMS, _GCC_EXECUTE_IDS = _generate_execute_params() if GCC_EXECUTE_TESTS else ([], [])


# Note: GCC execute tests are now run via ir_tests/test_gcc_torture_ir.py
# which uses the QEMU framework for proper linking and execution.
# This module only handles compile tests.
