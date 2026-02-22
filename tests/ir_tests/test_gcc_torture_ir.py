"""
GCC Torture Execute Tests integrated with ir_tests framework.

This runs GCC torture execute tests using the ir_tests QEMU framework,
which provides proper linking with newlib and execution verification.

Tests are discovered from GCC_TORTURE_PATH/execute directory.
Each test is expected to exit with code 0 for success.
"""

import pytest
import sys
import time
from pathlib import Path

from qemu_run import run_test, CompileConfig

# Add gcctestsuite to path for test discovery
GCC_TESTS_DIR = Path(__file__).parent.parent / "gcctestsuite"
if str(GCC_TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(GCC_TESTS_DIR))

from conftest import (
    GCC_TORTURE_PATH, OPT_LEVELS,
    discover_gcc_execute_tests,
    should_skip_gcc_test,
    is_xfail_test
)

MACHINE = "mps2-an505"
CURRENT_DIR = Path(__file__).parent

# Discover GCC execute tests
GCC_EXECUTE_TESTS = discover_gcc_execute_tests()


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
            ids.append(f"{test_case.source.stem}{opt}")
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
    
    if test_case.xfail_reason:
        pytest.xfail(test_case.xfail_reason)

    config = CompileConfig(
        extra_cflags=opt_level,
        output_dir=tmp_path,
        clean_before_build=False,
        timeout=test_case.timeout
    )

    # Run the test - it should compile, link, and run successfully
    sut, _ = run_test(test_case.source, MACHINE, config=config)

    # Wait for program to complete and check exit status
    # GCC torture tests should exit cleanly (exit code 0)
    # Poll until process exits (max 5 seconds)
    start = time.monotonic()
    while time.monotonic() - start < 5:
        if sut._proc.poll() is not None:
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
