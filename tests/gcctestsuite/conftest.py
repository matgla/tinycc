"""
Pytest configuration for GCC torture test suite.

This module provides test discovery and configuration for GCC torture tests.
"""

import pytest
import os
import re
import shlex
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
    # builtins/ tests — builtin override tests requiring lib/main.c framework
    # compile/ tests — compilation failures (parser, type system, unsupported features)
    # always_inline related failures (need proper fix for inline expansion)
}

# GCC Torture tests expected to fail only at -O1
# These pass at -O0 but require advanced optimizations (e.g., contradictory
# condition elimination) that TCC does not implement.
GCC_XFAIL_O1_TESTS = {
    # builtins/ tests — TCC doesn't constant-fold builtin calls at -O1, so the
    # custom override functions (which abort when __OPTIMIZE__ && inside_main)
    # get called instead of being optimized away.
    "builtins/abs-2",
    "builtins/abs-3",
    "builtins/fprintf",
    "builtins/fputs",
    "builtins/memchr",
    "builtins/memcmp",
    "builtins/memmove",
    "builtins/memmove-2",
    "builtins/mempcpy",
    "builtins/printf",
    "builtins/sprintf",
    "builtins/strcat",
    "builtins/strchr",
    "builtins/strcmp",
    "builtins/strcpy",
    "builtins/strcpy-2",
    "builtins/strcspn",
    "builtins/strncat",
    "builtins/strncpy",
    "builtins/strlen",
    "builtins/strlen-2",
    "builtins/strlen-3",
    "builtins/strnlen",
    "builtins/strpbrk",
    "builtins/strrchr",
    "builtins/strstr",
    "builtins/strstr-asm",
    "builtins/uabs-2",
    "builtins/uabs-3",
    # builtins/ tests — require GCC-level optimizations beyond chk inlining:
    # inline stores (_disallowed checks), value range analysis, conditional
    # pointer tracking. TCC inlines __builtin___*_chk but can't optimize away
    # the underlying library calls or prove value bounds.
    "builtins/memcpy-chk",    # test3: conditional ptr tracking + value range
    "builtins/memmove-chk",   # test1: memmove_disallowed (unconditional on ARM)
    "builtins/mempcpy-chk",   # test2: mempcpy_disallowed (unconditional)
    "builtins/memset-chk",    # test1: memset_disallowed (unconditional)
    "builtins/pr23484-chk",   # ternary length requires value range analysis
    "builtins/snprintf-chk",  # test2: conditional ptr tracking + value range
    "builtins/sprintf-chk",   # test1: sprintf_disallowed (unconditional)
    "builtins/stpcpy-chk",    # test1: stpcpy_disallowed (x86); test3: cond ptr
    "builtins/stpncpy-chk",   # test1: stpncpy_disallowed (unconditional)
    "builtins/strcat-chk",    # test1: strcat_disallowed (unconditional)
    "builtins/strcpy-chk",    # test1: strcpy_disallowed (non-Os)
    "builtins/strncat-chk",   # test1: strncat_disallowed (unconditional)
    "builtins/strncpy-chk",   # test1: strncpy_disallowed (unconditional)
    "builtins/strpcpy",       # __builtin_stpcpy not implemented
    "builtins/vsnprintf-chk", # test2: conditional ptr tracking + value range
    "builtins/vsprintf-chk",  # test1: vsprintf_disallowed (unconditional)
}

# GCC Torture tests to skip entirely
# These tests use features that won't be implemented
# Entries can be plain stems (for execute/) or directory-prefixed ("compile/name").
GCC_SKIP_TESTS = {
    "pr105613", # __int128 - not supported
    "pr23135", # __uint128 - not supported
    "pr93213", # __uint128 - not supported
    "pr84748", # __int128 - not supported
    "compile/20050215-1", # test infrastructure: compile-only test with no main(), current harness links and fails
    "compile/920520-1", # ARM GCC also rejects operand-only inline asm after %0 substitution (bad instruction 'rN')
    "compile/920521-1", # ARM GCC also rejects bare literal inline asm templates ('f' / 'g')
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
    expected_compile_failure: bool = False
    expected_error_patterns: List[str] = field(default_factory=list)


# Compiler flags from dg-options that TCC supports
TCC_SUPPORTED_DG_FLAGS = {
    "-fgnu89-inline",
    "-fno-common",
    "-fwrapv",
    "-fsigned-char",
    "-funsigned-char",
    "-finstrument-functions",
}

# Prefix patterns for dg-options flags that TCC supports (matched with startswith)
TCC_SUPPORTED_DG_FLAG_PREFIXES = (
    "-fno-builtin-",
    "-std=",
)

# Per-test flag overrides for cases where GCC torture semantics depend on
# specific dg-options and we want that behavior applied unconditionally.
GCC_TEST_FLAG_OVERRIDES = {
    "compile/20021120-1": "-fgnu89-inline",
    "compile/20021120-2": "-fgnu89-inline",
    "compile/20021120-3": "-fgnu89-inline",
}


def _is_supported_dg_flag(flag: str) -> bool:
    """Check if a dg-options flag is supported by TCC."""
    if flag in TCC_SUPPORTED_DG_FLAGS:
        return True
    return any(flag.startswith(p) for p in TCC_SUPPORTED_DG_FLAG_PREFIXES)


def parse_x_file(test_path: Path) -> str:
    """Parse a .x companion file for additional compiler flags.

    GCC torture tests use .x files (Tcl scripts) to specify extra flags:
        set additional_flags -fno-builtin-abs
    Returns supported flags as a space-separated string.
    """
    import re
    x_file = test_path.with_suffix('.x')
    if not x_file.exists():
        return ""
    try:
        content = x_file.read_text()
        m = re.search(r'set\s+additional_flags\s+(.*)', content)
        if m:
            raw_flags = m.group(1).strip()
            try:
                all_flags = shlex.split(raw_flags)
            except ValueError:
                all_flags = raw_flags.split()
            supported = [f for f in all_flags if _is_supported_dg_flag(f)]
            return " ".join(supported)
    except:
        pass
    return ""


def parse_dg_options(test_path: Path) -> str:
    """Parse dg-options from a GCC torture test file and its .x companion.

    Extracts flags from: /* { dg-options "flags" } */ and
    /* { dg-additional-options "flags" } */ in the .c file,
    and from 'set additional_flags ...' in a companion .x file.
    Only returns flags that TCC supports.
    """
    import re
    flags = []
    try:
        with open(test_path, 'r') as f:
            content = f.read(4096)
        for m in re.finditer(r'dg-(?:additional-)?options\s+"([^"]+)"', content):
            all_flags = m.group(1).split()
            flags.extend(f for f in all_flags if _is_supported_dg_flag(f))
    except:
        pass
    # Also parse companion .x file for additional_flags
    x_flags = parse_x_file(test_path)
    if x_flags:
        flags.extend(x_flags.split())

    override_flags = GCC_TEST_FLAG_OVERRIDES.get(_test_key(test_path), "")
    if override_flags:
        for flag in override_flags.split():
            if flag not in flags:
                flags.append(flag)

    return " ".join(flags)


def _effective_target_matches(target_expr: Optional[str]) -> bool:
    """Evaluate a small subset of GCC effective-target expressions.

    The ARMv8-M torture harness is ILP32, not LP64.
    """
    if not target_expr:
        return True

    expr = target_expr.replace("{", " ").replace("}", " ").strip()
    expr = " ".join(expr.split())
    simple_targets = {
        "size32plus": True,
        "lp64": False,
        "ilp32": True,
        "int128": False,
        "asm_goto_with_outputs": False,
    }

    if expr.startswith("!"):
        return not _effective_target_matches(expr[1:].strip())

    if expr in simple_targets:
        return simple_targets[expr]

    return True


def parse_dg_errors(test_path: Path) -> List[str]:
    """Parse dg-error directives from a GCC torture test file.

    Returns the regex patterns from comments like:
        /* { dg-error "pattern" } */

    Empty patterns are preserved to indicate a compile-fail expectation even
    when the test doesn't care about the exact diagnostic text.
    """
    try:
        content = test_path.read_text()
    except OSError:
        return []

    patterns = []
    dg_error_re = re.compile(
        r'dg-error\s+"([^"]*)"(?:\s+"[^"]*")?(?:\s+\{\s*target\s+\{\s*([^}]*)\s*\}\s*\})?'
    )
    for m in dg_error_re.finditer(content):
        if _effective_target_matches(m.group(2)):
            patterns.append(m.group(1))

    return patterns


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

        # Handle explicit dg-do target restrictions such as:
        #   /* { dg-do compile { target i?86-*-* x86_64-*-* } } */
        # These are target-selection directives rather than feature tests, so
        # x86-only cases should be skipped in the ARM harness.
        dg_do_target = _re.search(r'dg-do\s+\w+\s+\{\s*target\s+(.+?)\s*\}\s*\*/', content)
        if dg_do_target:
            targets = dg_do_target.group(1).strip()
            targets_lower = targets.lower()
            arm_patterns = ['arm', 'aarch64', 'thumb']
            triplet_markers = ['-*-', 'i?86', 'x86_64', 'ia32', 'powerpc', 'mips', 'riscv', 'sparc', 'alpha']
            if any(marker in targets_lower for marker in triplet_markers) and not any(
                p in targets_lower for p in arm_patterns
            ):
                return f"dg-do target: test restricted to non-ARM targets ({targets})"
            if not any(marker in targets_lower for marker in triplet_markers) and not _effective_target_matches(targets):
                return f"dg-do target: test requires unsupported target predicate ({targets})"

        # Tests requiring mmap are not available on bare-metal ARM
        if "dg-require-effective-target mmap" in content:
            return "Requires mmap (not available on bare-metal ARM)"

        # Tests requiring DLL import/export semantics are PE/COFF-specific.
        # The ARMv8-M harness targets ELF bare-metal, so these should be
        # skipped rather than treated as compiler failures.
        if "dg-require-dll" in content:
            return "Requires DLL target support (not available on ARM ELF)"

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


def is_xfail_o1_test(test_path: Path) -> Optional[str]:
    """Check if a GCC test is expected to fail only at -O1. Returns reason or None."""
    key = _test_key(test_path)
    if key in GCC_XFAIL_O1_TESTS:
        return f"Known failure at -O1: {key}"
    test_name = test_path.stem
    if test_name in GCC_XFAIL_O1_TESTS:
        return f"Known failure at -O1: {test_name}"
    return None


def discover_gcc_compile_tests() -> List[GCCTestCase]:
    """Discover GCC torture compile tests."""
    tests = []
    if not GCC_TORTURE_PATH.exists():
        return tests

    compile_dir = GCC_TORTURE_PATH / "compile"
    if compile_dir.exists():
        for c_file in sorted(compile_dir.glob("*.c")):
            dg_errors = parse_dg_errors(c_file)
            tests.append(GCCTestCase(
                source=c_file,
                category="gcc_compile",
                timeout=30,
                dg_options=parse_dg_options(c_file),
                expected_compile_failure=bool(dg_errors),
                expected_error_patterns=dg_errors,
                expected_exit_code=1 if dg_errors else 0,
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
