"""
tests2 C Compliance Test Suite for armv8m-tcc.

This test suite runs the tests2 C compliance tests.

Run with:
    pytest tests/tests2/ -v              # All tests2 tests
    pytest tests/tests2/ -v -k "O1"      # Only -O1 tests
    
Or use the general runner:
    python tests/run_tests.py --tests2
"""

import pytest
import re
import sys
from pathlib import Path
from typing import List

from conftest import (
    CTestCase, CURRENT_DIR, OPT_LEVELS,
    load_expect_file, parse_tagged_expect_file, discover_tests2_tests
)

# Import qemu_run
IR_TESTS_DIR = CURRENT_DIR.parent / "ir_tests"
if str(IR_TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(IR_TESTS_DIR))

try:
    from qemu_run import run_test, compile_testcase, prepare_test
    QEMU_AVAILABLE = True
except ImportError:
    QEMU_AVAILABLE = False

MACHINE = "mps2-an505"


# ============================================================================
# Helper Functions
# ============================================================================

def _escape_regex(line: str) -> str:
    """Escape regex special characters."""
    return re.escape(line)


def _expect_line(sut, expected_line: str, *, timeout: int = 1, float_tol: float = 1e-5):
    """Expect a line from QEMU output with float tolerance."""
    if expected_line is None:
        return
    
    _FLOAT_RE = r"[-+]?(?:\d+\.\d*|\d*\.\d+)(?:[eE][-+]\d+)?"
    float_matches = list(re.finditer(_FLOAT_RE, expected_line))
    
    if float_matches:
        parts = []
        expected_values = []
        last_end = 0
        for fm in float_matches:
            parts.append(re.escape(expected_line[last_end:fm.start()]))
            parts.append(rf"({_FLOAT_RE})")
            expected_values.append(float(fm.group(0)))
            last_end = fm.end()
        parts.append(re.escape(expected_line[last_end:]))
        pattern = "".join(parts)
        
        sut.expect(pattern, timeout=timeout)
        actual_values = [float(sut.match.group(i + 1)) for i in range(len(expected_values))]
        for expected_value, actual_value in zip(expected_values, actual_values):
            if abs(actual_value - expected_value) > float_tol:
                raise AssertionError(
                    f"Float mismatch: expected {expected_value} got {actual_value}"
                )
        return
    
    sut.expect(_escape_regex(expected_line), timeout=timeout)


def _strip_compiler_output(expected_lines: List[str], loglines: List[str]) -> List[str]:
    """Remove compiler output from expected lines."""
    sanitized = expected_lines.copy()
    compiler_verified = False
    for line in expected_lines:
        if compiler_verified:
            break
        for logline in loglines:
            if line in logline:
                sanitized = [l for l in sanitized if l != line]
                compiler_verified = True
                break
    return sanitized


def _sanitize_tag_for_filename(tag: str) -> str:
    """Make a tag safe for filenames."""
    return re.sub(r"[^a-zA-Z0-9_]+", "_", tag).strip("_")


# ============================================================================
# Test Execution
# ============================================================================

def run_qemu_test(test_case: CTestCase, opt_level: str, tmp_path: Path) -> None:
    """Run a test case in QEMU."""
    if not QEMU_AVAILABLE:
        pytest.skip("QEMU runner not available")
    
    expected_lines = load_expect_file(test_case.source)
    opt_suffix = f"_{opt_level.replace('-', '').replace(' ', '_')}"
    
    from qemu_run import CompileConfig
    config = CompileConfig(
        extra_cflags=f"{opt_level} {test_case.extra_cflags}".strip(),
        output_suffix=opt_suffix,
        output_dir=tmp_path
    )
    
    sut, loglines = run_test(
        [test_case.source],
        MACHINE,
        test_case.args,
        defines=test_case.defines,
        config=config
    )
    
    expected_lines = _strip_compiler_output(expected_lines, loglines)
    
    try:
        for line in expected_lines:
            _expect_line(sut, line, timeout=test_case.timeout)
        sut.wait()
        assert sut.exitstatus == test_case.expected_exit_code, \
            f"Expected exit {test_case.expected_exit_code}, got {sut.exitstatus}"
    except Exception as e:
        raise AssertionError(f"Test failed for {test_case.source} with {opt_level}: {e}") from e
    finally:
        if hasattr(sut, 'logfile') and sut.logfile:
            sut.logfile.close()


def run_tagged_test(source: Path, tag: str, expected_lines: List[str],
                    expected_exit_code: int, opt_level: str, tmp_path: Path) -> None:
    """Run a tagged variant of a test."""
    if not QEMU_AVAILABLE:
        pytest.skip("QEMU runner not available")
    
    safe_tag = _sanitize_tag_for_filename(tag)
    opt_suffix = f"_{safe_tag}_{opt_level.replace('-', '')}"
    
    from qemu_run import CompileConfig
    config = CompileConfig(
        defines=[tag],
        output_suffix=opt_suffix,
        extra_cflags=opt_level,
        output_dir=tmp_path,
        clean_before_build=False
    )
    
    result = compile_testcase([source], MACHINE, config=config)
    
    # Separate compile-time and runtime expectations
    source_basename = source.name
    compile_expected = []
    runtime_expected = []
    for line in expected_lines:
        if line and source_basename in line:
            compile_expected.append(line)
        else:
            runtime_expected.append(line)
    
    # Verify compile-time expectations
    compiler_output = "\n".join(result.output_lines)
    for line in compile_expected:
        if line and line not in compiler_output:
            raise AssertionError(f"Expected compile-time line not found: {line}")
    
    # If compilation failed, we're done (for compile-error tests)
    if not result.success:
        return
    
    # Run the test
    sut = prepare_test(MACHINE, result.elf_file)
    
    try:
        for line in runtime_expected:
            _expect_line(sut, line, timeout=1)
        sut.wait()
        assert sut.exitstatus == expected_exit_code, \
            f"Expected exit {expected_exit_code}, got {sut.exitstatus}"
    except Exception as e:
        raise AssertionError(f"Tagged test failed for {source}[{tag}]: {e}") from e
    finally:
        if hasattr(sut, 'logfile') and sut.logfile:
            sut.logfile.close()


# ============================================================================
# Tests2 Tests
# ============================================================================

TESTS2_TEST_CASES = discover_tests2_tests()


def _generate_tests2_params():
    params = []
    ids = []
    for test_case in TESTS2_TEST_CASES:
        for opt in OPT_LEVELS:
            params.append((test_case, opt))
            ids.append(f"{test_case.source.stem}{opt}")
    return params, ids


_TESTS2_PARAMS, _TESTS2_IDS = _generate_tests2_params()


@pytest.mark.tests2
@pytest.mark.execute
@pytest.mark.skipif(not TESTS2_TEST_CASES, reason="No tests2 tests found")
@pytest.mark.parametrize("test_case,opt_level", _TESTS2_PARAMS, ids=_TESTS2_IDS)
def test_tests2_execution(test_case: CTestCase, opt_level: str, tmp_path):
    """Run tests2 C tests in QEMU."""
    run_qemu_test(test_case, opt_level, tmp_path)


# ============================================================================
# Tagged Tests
# ============================================================================

TAGGED_TESTS = []
for c_file in sorted(CURRENT_DIR.glob("*.c")):
    if "+" in c_file.name:
        continue
    tagged = parse_tagged_expect_file(c_file)
    for tag, data in tagged.items():
        TAGGED_TESTS.append((c_file, tag, data["lines"], data["exit_code"]))


def _generate_tagged_params():
    params = []
    ids = []
    for source, tag, lines, exit_code in TAGGED_TESTS:
        for opt in OPT_LEVELS:
            params.append((source, tag, lines, exit_code, opt))
            ids.append(f"{source.stem}[{tag}]{opt}")
    return params, ids


_TAGGED_PARAMS, _TAGGED_IDS = _generate_tagged_params() if TAGGED_TESTS else ([], [])


@pytest.mark.tests2
@pytest.mark.execute
@pytest.mark.skipif(not TAGGED_TESTS, reason="No tagged tests found")
@pytest.mark.parametrize("source,tag,expected_lines,expected_exit_code,opt_level",
                         _TAGGED_PARAMS, ids=_TAGGED_IDS)
def test_tests2_tagged(source: Path, tag: str, expected_lines: List[str],
                       expected_exit_code: int, opt_level: str, tmp_path):
    """Run tagged variant of tests2 tests."""
    if tag == "test_data_suppression_on":
        pytest.xfail("IR backend cannot suppress data in dead code blocks")
    
    run_tagged_test(source, tag, expected_lines, expected_exit_code, opt_level, tmp_path)


# ============================================================================
# Multi-file Tests
# ============================================================================

MULTI_FILE_TESTS = [
    (["104_inline.c", "104+_inline.c"], 0),
    (["120_alias.c", "120+_alias.c"], 0),
]


def _generate_multifile_params():
    params = []
    ids = []
    for files, exit_code in MULTI_FILE_TESTS:
        for opt in OPT_LEVELS:
            sources = [CURRENT_DIR / f for f in files]
            params.append((sources, exit_code, opt))
            ids.append(f"{Path(files[0]).stem}{opt}")
    return params, ids


_MULTIFILE_PARAMS, _MULTIFILE_IDS = _generate_multifile_params()


@pytest.mark.tests2
@pytest.mark.execute
@pytest.mark.parametrize("sources,expected_exit_code,opt_level", _MULTIFILE_PARAMS, ids=_MULTIFILE_IDS)
def test_multifile_execution(sources: List[Path], expected_exit_code: int, opt_level: str, tmp_path):
    """Run multi-file tests."""
    if not QEMU_AVAILABLE:
        pytest.skip("QEMU runner not available")
    
    expect_file = sources[0].with_suffix(".expect")
    if not expect_file.exists():
        pytest.skip(f"No expect file: {expect_file}")
    
    expected_lines = load_expect_file(sources[0])
    opt_suffix = f"_{opt_level.replace('-', '').replace(' ', '_')}"
    
    from qemu_run import CompileConfig
    config = CompileConfig(
        extra_cflags=opt_level,
        output_suffix=opt_suffix,
        output_dir=tmp_path
    )
    
    sut, loglines = run_test(sources, MACHINE, config=config)
    expected_lines = _strip_compiler_output(expected_lines, loglines)
    
    try:
        for line in expected_lines:
            _expect_line(sut, line, timeout=10)
        sut.wait()
        assert sut.exitstatus == expected_exit_code
    except Exception as e:
        raise AssertionError(f"Multi-file test failed: {e}") from e
    finally:
        if hasattr(sut, 'logfile') and sut.logfile:
            sut.logfile.close()
