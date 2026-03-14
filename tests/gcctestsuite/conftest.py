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
# Entries can be plain stems ("test_name") or directory-prefixed ("ieee/test_name")
# to disambiguate tests with the same name in different directories.
GCC_XFAIL_TESTS = {
    # execute/ tests — setjmp/longjmp relocation errors (R_ARM_THM_JUMP11)
    "20210505-1",
    "pr56982",
    # ieee/ tests — IEEE floating-point edge cases, long double, NaN/Inf handling
    "ieee/20000320-1",
    "ieee/cdivchkd",
    "ieee/cdivchkf",
    "ieee/cdivchkld",
    "ieee/compare-fp-1",
    "ieee/compare-fp-3",
    "ieee/copysign2",
    "ieee/fp-cmp-4",
    "ieee/fp-cmp-4f",
    "ieee/fp-cmp-4l",
    "ieee/fp-cmp-5",
    "ieee/fp-cmp-6",
    "ieee/fp-cmp-7",
    "ieee/fp-cmp-8",
    "ieee/fp-cmp-8f",
    "ieee/fp-cmp-8l",
    "ieee/fp-cmp-9",
    "ieee/fp-cmp-cond-1",
    "ieee/mzero3",
    "ieee/pr109386",
    "ieee/pr38016",
    "ieee/pr50310",
    "ieee/pr72824",
    "ieee/pr72824-2",
    "ieee/rbug",
    # builtins/ tests — builtin override tests requiring lib/main.c framework
    "builtins/abs-1",
    "builtins/abs-2",
    "builtins/abs-3",
    "builtins/complex-1",
    "builtins/fprintf",
    "builtins/fputs",
    "builtins/memchr",
    "builtins/memcmp",
    "builtins/memcpy-chk",
    "builtins/memmove",
    "builtins/memmove-2",
    "builtins/memmove-chk",
    "builtins/memops-asm",
    "builtins/mempcpy",
    "builtins/mempcpy-2",
    "builtins/mempcpy-chk",
    "builtins/memset-chk",
    "builtins/pr23484-chk",
    "builtins/pr93262-chk",
    "builtins/printf",
    "builtins/snprintf-chk",
    "builtins/sprintf",
    "builtins/sprintf-chk",
    "builtins/stpcpy-chk",
    "builtins/stpncpy-chk",
    "builtins/strcat",
    "builtins/strcat-chk",
    "builtins/strchr",
    "builtins/strcmp",
    "builtins/strcpy",
    "builtins/strcpy-2",
    "builtins/strcpy-chk",
    "builtins/strcspn",
    "builtins/strlen",
    "builtins/strlen-2",
    "builtins/strlen-3",
    "builtins/strncat",
    "builtins/strncat-chk",
    "builtins/strncmp",
    "builtins/strncpy",
    "builtins/strncpy-chk",
    "builtins/strnlen",
    "builtins/strpbrk",
    "builtins/strpcpy",
    "builtins/strpcpy-2",
    "builtins/strrchr",
    "builtins/strspn",
    "builtins/strstr",
    "builtins/strstr-asm",
    "builtins/uabs-1",
    "builtins/uabs-2",
    "builtins/uabs-3",
    "builtins/vsnprintf-chk",
    "builtins/vsprintf-chk",
    # compile/ tests — compilation failures (parser, type system, unsupported features)
    "compile/20000120-2",
    "compile/20001222-1",
    "compile/20010605-1",
    "compile/20010605-2",
    "compile/20010714-1",
    "compile/20011106-1",
    "compile/20011119-2",
    "compile/20011219-1",
    "compile/20020210-1",
    "compile/20020330-1",
    "compile/20021120-1",
    "compile/20021120-2",
    "compile/20030305-1",
    "compile/20031023-4",
    "compile/20050215-1",
    "compile/20050215-2",
    "compile/20050215-3",
    "compile/920520-1",
    "compile/920521-1",
    "compile/930525-1",
    "compile/950919-1",
    "compile/asmgoto-2",
    "compile/asmgoto-3",
    "compile/asmgoto-4",
    "compile/attr-complex-method",
    "compile/attr-complex-method-2",

    "compile/dce-inline-asm-1",
    "compile/dce-inline-asm-2",
    "compile/dll",
    "compile/ex",
    "compile/limits-exprparen",
    "compile/pr103682",
    "compile/pr108237",
    "compile/pr108892",
    "compile/pr111059-10",
    "compile/pr111059-11",
    "compile/pr111059-12",
    "compile/pr111059-7",
    "compile/pr111059-8",
    "compile/pr111059-9",
    "compile/pr111911-2",

    "compile/pr123365",
    "compile/pr123703",
    "compile/pr27341-2",
    "compile/pr27528",
    "compile/pr27889",
    "compile/pr28865",
    "compile/pr30132",
    "compile/pr34885",
    "compile/pr35318",
    "compile/pr37669",
    "compile/pr41987",
    "compile/pr44197",
    "compile/pr46534",
    "compile/pr46866",
    "compile/pr48517",
    "compile/pr51694",
    "compile/pr54559",
    "compile/pr54713-3",
    "compile/pr65680",
    "compile/pr72802",
    "compile/pr77754-6",
    "compile/pr78694",
    "compile/pr82564",
    "compile/pr83222",
    "compile/pr85401",
    "compile/pr92449",
    "compile/pr93335",
    "compile/pr96998",
    "compile/pr98096",
    "compile/pr99324",
    "compile/simd-1",
    "compile/sizeof-macros-1",  # test infrastructure: no main(), link fails
    "compile/uuarg",
    "compile/vector-1",
    "compile/vector-2",
    "compile/vector-3",
    "compile/vector-shift-1",
}

# GCC Torture tests to skip entirely
# These tests use features that won't be implemented
# Entries can be plain stems (for execute/) or directory-prefixed ("compile/name").
GCC_SKIP_TESTS = {
    "pr105613", # __int128 - not supported
    "pr23135", # __uint128 - not supported
    "pr93213", # __uint128 - not supported
    "pr84748", # __int128 - not supported
    # execute/ tests — require mmap (not available on bare-metal ARM)
    "loop-2f", # requires mmap, includes <sys/mman.h>
    "loop-2g", # requires mmap, includes <sys/mman.h>
    # compile/ tests — timeouts
    "compile/limits-fndefn", # compilation timeout (>10s)
    # compile/ tests — x86-only or GCC-internal (not applicable to ARM target)
    "compile/pr30311", # x86-only: asm "=t" constraint (x87 FP stack)
    "compile/pr44707", # PowerPC-only: asm "nro" constraint
    "compile/pr110386-2", # x86-only: AVX intrinsics (_mm_abs_epi32, etc.)
    "compile/pr115143-2", # GCC internal: __GIMPLE(ssa) test format
    "compile/pr115143-3", # GCC internal: __GIMPLE(ssa) test format
    # compile/ tests — __int128 (not available on 32-bit ARM)
    "compile/bitfield-1", # __uint128_t bitfield
    "compile/bitfield-endian-1", # __uint128_t bitfield + scalar_storage_order
    "compile/bitfield-endian-2", # __uint128_t bitfield + scalar_storage_order
    "compile/pr70355", # __int128 vector type
    "compile/pr99822", # __int128 type
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
    extra_sources: List[Path] = field(default_factory=list)  # Additional source files (e.g., builtins lib files)


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

    # Check if test is in the skip list (directory-prefixed key first, then plain stem)
    key = _test_key(test_path)
    if key in GCC_SKIP_TESTS:
        return f"Skipped: {key} (feature not supported)"
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

        # Tests requiring mmap are not available on bare-metal ARM
        if "dg-require-effective-target mmap" in content:
            return "Requires mmap (not available on bare-metal ARM)"

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


def _test_key(test_path: Path) -> str:
    """Get directory-prefixed key for a test.

    Returns plain stem for execute/ top-level tests (e.g., 'test_name'),
    and directory-prefixed keys for subdirectories and compile tests
    (e.g., 'ieee/fp-cmp-1', 'compile/pr27889').
    """
    parent = test_path.parent.name
    if parent == "execute":
        return test_path.stem
    if parent == "compile":
        return f"compile/{test_path.stem}"
    return f"{parent}/{test_path.stem}"


def is_xfail_test(test_path: Path) -> Optional[str]:
    """Check if a GCC test is expected to fail. Returns reason or None."""
    key = _test_key(test_path)
    if key in GCC_XFAIL_TESTS:
        return f"Known failure: {key}"
    # Also check plain stem for backward compatibility
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
    """Discover GCC torture execute tests.

    Recursively discovers tests in execute/ and its subdirectories
    (builtins/, ieee/). For builtins/ tests, pairs main files with
    their corresponding -lib.c files and lib/main.c.
    """
    tests = []
    if not GCC_TORTURE_PATH.exists():
        return tests

    execute_dir = GCC_TORTURE_PATH / "execute"
    if not execute_dir.exists():
        return tests

    # Top-level execute tests (single-file)
    for c_file in sorted(execute_dir.glob("*.c")):
        tests.append(GCCTestCase(
            source=c_file,
            category="gcc_execute",
            timeout=30,
            dg_options=parse_dg_options(c_file)
        ))

    # ieee/ subdirectory — standalone single-file tests
    ieee_dir = execute_dir / "ieee"
    if ieee_dir.exists():
        for c_file in sorted(ieee_dir.glob("*.c")):
            tests.append(GCCTestCase(
                source=c_file,
                category="gcc_execute",
                timeout=30,
                dg_options=parse_dg_options(c_file)
            ))

    # builtins/ subdirectory — multi-file tests
    # Each test has a main file (e.g., abs-1.c) defining main_test(),
    # a companion lib file (abs-1-lib.c) with helper overrides, and
    # lib/main.c which provides the actual main() entry point.
    builtins_dir = execute_dir / "builtins"
    if builtins_dir.exists():
        builtins_main = builtins_dir / "lib" / "main.c"
        for c_file in sorted(builtins_dir.glob("*.c")):
            # Skip -lib.c companion files — they are linked via extra_sources
            if c_file.name.endswith("-lib.c"):
                continue
            # Skip files inside lib/ subdirectory
            if c_file.parent.name == "lib":
                continue

            extra = []
            # Pair with corresponding -lib.c if it exists
            lib_file = c_file.with_name(c_file.stem + "-lib.c")
            if lib_file.exists():
                extra.append(lib_file)
            # Always include lib/main.c (provides main())
            if builtins_main.exists():
                extra.append(builtins_main)

            tests.append(GCCTestCase(
                source=c_file,
                category="gcc_execute",
                timeout=30,
                dg_options=parse_dg_options(c_file),
                extra_sources=extra
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
        execute_top = len(list((GCC_TORTURE_PATH / "execute").glob("*.c"))) if (GCC_TORTURE_PATH / "execute").exists() else 0
        execute_ieee = len(list((GCC_TORTURE_PATH / "execute" / "ieee").glob("*.c"))) if (GCC_TORTURE_PATH / "execute" / "ieee").exists() else 0
        builtins_dir = GCC_TORTURE_PATH / "execute" / "builtins"
        execute_builtins = len([f for f in builtins_dir.glob("*.c") if not f.name.endswith("-lib.c")]) if builtins_dir.exists() else 0
        execute_total = execute_top + execute_ieee + execute_builtins
        terminalreporter.write_line(f"Compile tests available: {compile_tests}")
        terminalreporter.write_line(f"Execute tests available: {execute_total} (top-level: {execute_top}, ieee: {execute_ieee}, builtins: {execute_builtins})")
        terminalreporter.write_line(f"Known failing tests (xfail): {len(GCC_XFAIL_TESTS)}")
