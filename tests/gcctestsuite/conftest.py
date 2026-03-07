"""
Pytest configuration for GCC torture test suite.

This module provides test discovery and configuration for GCC torture tests.
"""

import pytest
import os
import sys
from pathlib import Path
from dataclasses import dataclass, field
from typing import List, Optional, Set

# Configuration
CURRENT_DIR = Path(__file__).parent
PROJECT_ROOT = CURRENT_DIR.parent.parent


def _detect_asan():
    """Check if the compiler was built with AddressSanitizer."""
    config_mak = PROJECT_ROOT / "config.mak"
    try:
        return "CONFIG_asan=yes" in config_mak.read_text()
    except OSError:
        return False


ASAN_ENABLED = _detect_asan()
ASAN_TIMEOUT_MULTIPLIER = 3 if ASAN_ENABLED else 1

# GCC torture tests path (can be overridden via environment)
# Default is the git submodule at tests/gcctestsuite/gcc-testsuite
DEFAULT_GCC_PATH = Path(__file__).parent / "gcc-testsuite" / "gcc" / "testsuite" / "gcc.c-torture"
GCC_TORTURE_PATH = Path(os.environ.get("GCC_TORTURE_PATH", DEFAULT_GCC_PATH))

# Optimization levels to test
OPT_LEVELS = ["-O0", "-O1"]

# GCC Torture tests expected to fail
# These tests are known to fail with armv8m-tcc
# To regenerate this list, run: make test-all and check .pytest_cache/v/cache/lastfailed
GCC_XFAIL_TESTS = {
}

# GCC Torture tests to skip entirely
# These tests use features that won't be implemented
GCC_SKIP_TESTS = {
    "pr105613", # __int128 - not supported
    "pr23135", # __uint128 - not supported
    "pr93213", # __uint128 - not supported
    "pr84748", # __int128 - not supported
}


@dataclass
class GCCTestCase:
    """Represents a single GCC torture test case."""
    source: Path
    expected_exit_code: int = 0
    timeout: int = 30 * ASAN_TIMEOUT_MULTIPLIER
    category: str = "gcc_compile"  # gcc_compile, gcc_execute
    skip_reason: Optional[str] = None
    xfail_reason: Optional[str] = None
    dg_options: str = ""  # Extra flags from /* { dg-options "..." } */


# Compiler flags from dg-options that TCC supports
TCC_SUPPORTED_DG_FLAGS = {
    "-fgnu89-inline",
    "-fno-common",
    "-fwrapv",
    "-fsigned-char",
    "-funsigned-char",
    "-finstrument-functions",
}


def parse_dg_options(test_path: Path) -> str:
    """Parse dg-options from a GCC torture test file.

    Extracts flags from: /* { dg-options "flags" } */
    Only returns flags that TCC supports.
    """
    import re
    try:
        with open(test_path, 'r') as f:
            content = f.read(4096)
        m = re.search(r'dg-options\s+"([^"]+)"', content)
        if m:
            all_flags = m.group(1).split()
            supported = [f for f in all_flags if f in TCC_SUPPORTED_DG_FLAGS]
            return " ".join(supported)
    except:
        pass
    return ""


def should_skip_gcc_test(test_path: Path) -> Optional[str]:
    """Check if a GCC test should be skipped. Returns reason or None."""
    import re as _re
    skip_patterns = {
        "mipscop",
    }
    name = test_path.name.lower()

    # Check if test is in the skip list
    test_name = test_path.stem
    if test_name in GCC_SKIP_TESTS:
        return f"Skipped: {test_name} (feature not supported)"

    try:
        with open(test_path, 'r') as f:
            content = f.read(4096)
            content_lower = content.lower()

        for pattern in skip_patterns:
            if pattern in name or pattern in content_lower:
                return f"Uses unsupported feature: {pattern}"

        # Handle dg-skip-if directives that restrict to non-ARM architectures.
        # Pattern: /* { dg-skip-if "" { ! { i?86-*-* x86_64-*-* } } } */
        # This means "skip if NOT x86", so we should skip on ARM.
        dg_skip = _re.search(r'dg-skip-if\s+"[^"]*"\s+\{\s*!\s*\{([^}]+)\}', content)
        if dg_skip:
            targets = dg_skip.group(1)
            # If the allowed targets are x86-only (no arm), skip on ARM
            arm_patterns = ['arm', 'aarch64', 'thumb']
            if not any(p in targets.lower() for p in arm_patterns):
                return f"dg-skip-if: test restricted to non-ARM targets ({targets.strip()})"

        # Tests requiring trampolines (nested functions) are now supported
        # if "dg-require-effective-target trampolines" in content:
        #     return "Requires nested functions (trampolines)"

        # Tests requiring label_values (computed goto) are now supported
        # if "dg-require-effective-target label_values" in content:
        #     return "Requires label_values (computed goto)"

        # Tests using complex numbers are now supported
        # if "__complex__" in content or "_Complex" in content:
        #     return "Uses complex numbers (not fully supported)"
    except:
        pass

    return None


def is_xfail_test(test_path: Path) -> Optional[str]:
    """Check if a GCC test is expected to fail. Returns reason or None."""
    test_name = test_path.stem
    if test_name in GCC_XFAIL_TESTS:
        return f"Known failure: {test_name}"
    return None


def discover_gcc_compile_tests() -> List[GCCTestCase]:
    """Discover GCC torture compile tests."""
    tests = []
    if not GCC_TORTURE_PATH.exists():
        return tests

    compile_dir = GCC_TORTURE_PATH / "compile"
    if compile_dir.exists():
        for c_file in sorted(compile_dir.glob("*.c")):
            tests.append(GCCTestCase(
                source=c_file,
                category="gcc_compile",
                timeout=30,
                dg_options=parse_dg_options(c_file)
            ))

    return tests


def discover_gcc_execute_tests() -> List[GCCTestCase]:
    """Discover GCC torture execute tests."""
    tests = []
    if not GCC_TORTURE_PATH.exists():
        return tests

    execute_dir = GCC_TORTURE_PATH / "execute"
    if execute_dir.exists():
        for c_file in sorted(execute_dir.glob("*.c")):
            tests.append(GCCTestCase(
                source=c_file,
                category="gcc_execute",
                timeout=30,
                dg_options=parse_dg_options(c_file)
            ))

    return tests


def pytest_configure(config):
    """Configure pytest with custom markers."""
    config.addinivalue_line("markers", "gcc_torture: GCC torture tests")
    config.addinivalue_line("markers", "gcc_compile: GCC compile tests")
    config.addinivalue_line("markers", "gcc_execute: GCC execute tests")
    config.addinivalue_line("markers", "slow: Slow tests (long timeout)")
    config.addinivalue_line("markers", "xfail: Expected to fail")


def pytest_terminal_summary(terminalreporter, exitstatus, config):
    terminalreporter.write_sep("=", "GCC Torture Test Summary")
    terminalreporter.write_line(f"GCC torture path: {GCC_TORTURE_PATH}")
    terminalreporter.write_line(f"GCC torture path exists: {GCC_TORTURE_PATH.exists()}")
    if GCC_TORTURE_PATH.exists():
        compile_tests = len(list((GCC_TORTURE_PATH / "compile").glob("*.c"))) if (GCC_TORTURE_PATH / "compile").exists() else 0
        execute_tests = len(list((GCC_TORTURE_PATH / "execute").glob("*.c"))) if (GCC_TORTURE_PATH / "execute").exists() else 0
        terminalreporter.write_line(f"Compile tests available: {compile_tests}")
        terminalreporter.write_line(f"Execute tests available: {execute_tests}")
        terminalreporter.write_line(f"Known failing tests (xfail): {len(GCC_XFAIL_TESTS)}")
