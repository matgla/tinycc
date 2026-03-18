"""
GCC Torture Tests integrated with ir_tests framework.

This runs GCC torture execute and compile tests using the ir_tests QEMU
framework, which provides proper linking with newlib and execution verification.

Execute tests are discovered recursively from GCC_TORTURE_PATH/execute directory
(including builtins/ and ieee/ subdirectories).
Compile tests are discovered from GCC_TORTURE_PATH/compile directory.

Each execute test is expected to exit with code 0 for success.
Compile tests only verify successful compilation (no linking/execution).
"""

import pytest
import re
import subprocess
import sys
import time
from pathlib import Path

from qemu_run import run_test, compile_testcase, CompileConfig, ASAN_ENABLED, VALGRIND_ENABLED

# Import gcctestsuite conftest explicitly (avoid shadowing by local conftest.py)
GCC_TESTS_DIR = Path(__file__).parent.parent / "gcctestsuite"
import importlib.util
_spec = importlib.util.spec_from_file_location("gcc_conftest", GCC_TESTS_DIR / "conftest.py")
_gcc_conftest = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_gcc_conftest)

GCC_TORTURE_PATH = _gcc_conftest.GCC_TORTURE_PATH
OPT_LEVELS = _gcc_conftest.OPT_LEVELS
discover_gcc_execute_tests = _gcc_conftest.discover_gcc_execute_tests
discover_gcc_compile_tests = _gcc_conftest.discover_gcc_compile_tests
should_skip_gcc_test = _gcc_conftest.should_skip_gcc_test
is_xfail_test = _gcc_conftest.is_xfail_test
is_xfail_o1_test = _gcc_conftest.is_xfail_o1_test

MACHINE = "mps2-an505"
CURRENT_DIR = Path(__file__).parent

# Tests too slow under instrumentation (ASan / valgrind) — skip to avoid timeouts.
# Includes tests that trigger valgrind "uninitialised value" errors (false positives
# from GCC torture edge cases) and tests that time out under instrumentation.
SLOW_UNDER_INSTRUMENTATION = {
    "memclr",
    # Compilation timeouts under valgrind
    "memcpy-a1",
    "memcpy-a2",
    "memcpy-a4",
    "memcpy-a8",
    # Valgrind "Conditional jump or move depends on uninitialised value(s)"
    "20061220-1",
    "20020107-1",
    "pr110252-2",
    "pr103376",
    "pr41239",
    "pr49279",
    "pr45695",
    "pr49390",
    "990130-1",
    "pr38533",
    "pr65053-1",
    "pr65053-2",
    "pr43560",
    "pr52286",
    "pr40657",
    "pr65956",
    "pr88904",
    "pr84524",
    "pr85156",
    "stkalign",
    # Build failures (include errors, warnings-as-errors)
    "20030222-1",
    "pr43385",
}

# Discover GCC execute tests (recursive: top-level + ieee/ + builtins/)
GCC_EXECUTE_TESTS = discover_gcc_execute_tests()


def _sut_has_exited(sut):
    if hasattr(sut, "_proc"):
        return sut._proc.poll() is not None
    if hasattr(sut, "isalive"):
        return not sut.isalive()
    return getattr(sut, "exitstatus", None) is not None


def _test_id(test_case, opt_level):
    """Generate a unique test ID including subdirectory prefix."""
    execute_dir = GCC_TORTURE_PATH / "execute"
    try:
        rel = test_case.source.parent.relative_to(execute_dir)
        if rel != Path("."):
            return f"{rel}/{test_case.source.stem}{opt_level}"
    except ValueError:
        pass
    return f"{test_case.source.stem}{opt_level}"


def _generate_execute_params():
    """Generate test parameters for GCC execute tests."""
    params = []
    ids = []
    for test_case in GCC_EXECUTE_TESTS:
        skip_reason = should_skip_gcc_test(test_case.source)
        if skip_reason:
            test_case.skip_reason = skip_reason

        xfail_reason = is_xfail_test(test_case.source)
        if xfail_reason:
            test_case.xfail_reason = xfail_reason

        for opt in OPT_LEVELS:
            params.append((test_case, opt))
            ids.append(_test_id(test_case, opt))
    return params, ids


_GCC_EXECUTE_PARAMS, _GCC_EXECUTE_IDS = _generate_execute_params() if GCC_EXECUTE_TESTS else ([], [])


@pytest.mark.gcc_torture
@pytest.mark.gcc_execute
@pytest.mark.slow
@pytest.mark.skipif(not GCC_TORTURE_PATH.exists(), reason="GCC torture tests not found")
@pytest.mark.parametrize("test_case,opt_level", _GCC_EXECUTE_PARAMS, ids=_GCC_EXECUTE_IDS)
def test_gcc_execute_ir(test_case, opt_level, tmp_path):
    """Run GCC torture execute tests via QEMU.

    Tests are compiled, linked with newlib, and executed in QEMU.
    Success is determined by exit code 0.
    """
    if test_case.skip_reason:
        pytest.skip(test_case.skip_reason)

    if (ASAN_ENABLED or VALGRIND_ENABLED) and test_case.source.stem in SLOW_UNDER_INSTRUMENTATION:
        pytest.skip("Skipped under ASan/valgrind (too slow)")

    if test_case.xfail_reason:
        pytest.xfail(test_case.xfail_reason)

    # O1-only xfails: tests that pass at -O0 but need advanced optimizations
    if opt_level == "-O1":
        o1_reason = is_xfail_o1_test(test_case.source)
        if o1_reason:
            pytest.xfail(o1_reason)

    extra_flags = opt_level
    if test_case.dg_options:
        extra_flags = f"{opt_level} {test_case.dg_options}"

    config = CompileConfig(
        extra_cflags=extra_flags,
        output_dir=tmp_path,
        clean_before_build=False,
        timeout=test_case.timeout
    )

    # Build the source file list (main + extra sources for multi-file tests)
    source_files = [test_case.source] + test_case.extra_sources

    # Run the test - it should compile, link, and run successfully
    sut, _ = run_test(source_files, MACHINE, config=config)

    # Wait for program to complete and check exit status
    # GCC torture tests should exit cleanly (exit code 0)
    # Poll until process exits (max 5 seconds)
    start = time.monotonic()
    while time.monotonic() - start < 5:
        if _sut_has_exited(sut):
            break
        time.sleep(0.01)
    sut.close()

    # Exit code 0 means success
    assert sut.exitstatus == 0, f"Test exited with code {sut.exitstatus}"


# Placeholder when tests not available
if not GCC_EXECUTE_TESTS:
    @pytest.mark.gcc_torture
    @pytest.mark.gcc_execute
    @pytest.mark.skip(reason="GCC execute tests not available - run 'make download-gcc-tests'")
    def test_gcc_execute_ir__no_tests():
        """Placeholder when GCC tests are not available."""
        pass


# ============================================================================
# GCC Compile-Only Tests
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
def test_gcc_compile_ir(test_case, opt_level, tmp_path):
    """Compile GCC torture compile-only tests.

    These tests only verify successful compilation (no linking or execution).
    They come from the gcc.c-torture/compile/ directory.
    Invokes armv8m-tcc -c directly to produce a .o file.
    """
    if test_case.skip_reason:
        pytest.skip(test_case.skip_reason)

    if test_case.xfail_reason:
        pytest.xfail(test_case.xfail_reason)

    compiler = CURRENT_DIR / "../../armv8m-tcc"
    project_root = (CURRENT_DIR / "../..").resolve()
    libc_includes = CURRENT_DIR / "libc_includes"
    libc_imports = CURRENT_DIR / "libc_imports"
    newlib_includes = libc_includes / "newlib"
    output_obj = tmp_path / f"{test_case.source.stem}.o"

    cmd = [
        str(compiler),
        f"-B{project_root}",
        f"-I{libc_includes}",
        f"-I{libc_imports}",
        f"-I{newlib_includes}",
        f"-I{project_root / 'include'}",
        opt_level,
    ]
    if test_case.dg_options:
        cmd.extend(test_case.dg_options.split())
    cmd.extend([
        "-c", str(test_case.source),
        "-o", str(output_obj),
    ])

    try:
        result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=test_case.timeout)
    except subprocess.TimeoutExpired:
        pytest.fail(f"Compilation timed out after {test_case.timeout}s")
    output = ((result.stderr.decode(errors="replace") if result.stderr else "")
              + (result.stdout.decode(errors="replace") if result.stdout else "")).strip()

    if getattr(test_case, "expected_compile_failure", False):
        assert result.returncode != 0, "Compilation unexpectedly succeeded for expected-failure test"

        expected_patterns = sorted({p for p in getattr(test_case, "expected_error_patterns", []) if p})
        missing = []
        for pattern in expected_patterns:
            if not re.search(pattern, output, re.MULTILINE):
                missing.append(pattern)

        assert not missing, (
            "Compilation failed, but expected diagnostics were missing:\n"
            + "\n".join(repr(p) for p in missing)
            + "\n\nCompiler output:\n"
            + output
        )
    else:
        assert result.returncode == 0, f"Compilation failed (exit {result.returncode}):\n{output}"


# Placeholder when compile tests not available
if not GCC_COMPILE_TESTS:
    @pytest.mark.gcc_torture
    @pytest.mark.gcc_compile
    @pytest.mark.skip(reason="GCC compile tests not available - run 'make download-gcc-tests'")
    def test_gcc_compile_ir__no_tests():
        """Placeholder when GCC compile tests are not available."""
        pass


@pytest.mark.gcc_torture
@pytest.mark.gcc_compile
@pytest.mark.skipif(not GCC_TORTURE_PATH.exists(), reason="GCC torture tests not found")
@pytest.mark.parametrize(
    "source_name,extra_args,expected_pattern,output_name",
    [
        (
            "20050215-2.c",
            [],
            r"redefinition of ['‘`]?f2['’`]?",
            "20050215-2.o",
        ),
        (
            "920520-1.c",
            ["-std=gnu89"],
            r"known instruction expected",
            "920520-1.o",
        ),
    ],
    ids=["20050215-2", "920520-1"],
)
def test_gcc_compile_ir_reports_known_diagnostics(tmp_path, source_name, extra_args, expected_pattern, output_name):
    """Verify selected failing GCC compile tests report stable diagnostics."""
    compiler = CURRENT_DIR / "../../armv8m-tcc"
    source = GCC_TORTURE_PATH / "compile" / source_name
    output_obj = tmp_path / output_name

    result = subprocess.run(
        [str(compiler), *extra_args, "-c", str(source), "-o", str(output_obj)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=30,
    )

    output = ((result.stderr.decode(errors="replace") if result.stderr else "")
              + (result.stdout.decode(errors="replace") if result.stdout else "")).strip()

    assert result.returncode != 0, "Compilation unexpectedly succeeded"
    assert re.search(expected_pattern, output, re.MULTILINE), (
        "Expected diagnostic was missing:\n\n" + output
    )
