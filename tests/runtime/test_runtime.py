"""Phase 6: runtime library coverage tests.

This layer exercises the runtime libraries that are normally only reached
indirectly by compiled programs:

* Host-native tests for the architecture-independent soft-FP algorithms in
  ``lib/fp/soft/*.c``.
* Cross-compiled mini-tests that force references to ARM EABI runtime helpers
  (``__aeabi_*``), 64-bit integer math, string helpers, setjmp/longjmp, and
  VLA helpers, then verify the expected symbols are emitted.
"""

import re
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).parent.parent.parent  # libs/tinycc
RUNTIME_DIR = Path(__file__).parent
BUILD_DIR = RUNTIME_DIR / "build"

NM = "arm-none-eabi-nm"


def _base_cflags():
    """Default cross-compile flags used elsewhere in the test suite."""
    return [
        "-O1",
        "-nostdlib",
        "-fvisibility=hidden",
        "-mcpu=cortex-m33",
        "-mthumb",
        "-mfloat-abi=soft",
        "-ffunction-sections",
        "-c",
    ]


def _cross_compile(name, compiler, extra_cflags=()):
    """Cross-compile a case in cross/<name>.c to a relocatable object."""
    src = RUNTIME_DIR / "cross" / f"{name}.c"
    obj = BUILD_DIR / "cross" / f"{name}.o"
    obj.parent.mkdir(parents=True, exist_ok=True)

    cflags = _base_cflags() + list(extra_cflags)
    cmd = [str(compiler)] + cflags + [str(src), "-o", str(obj)]
    result = subprocess.run(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        errors="replace",
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"Compile failed for cross/{name}: {cmd}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return obj


def _nm_symbols(obj):
    """Return {(name, type)} from nm output for an object or binary."""
    result = subprocess.run(
        [NM, str(obj)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        errors="replace",
    )
    assert result.returncode == 0, f"nm failed for {obj}: {result.stderr}"

    symbols = {}
    for line in result.stdout.splitlines():
        # Object files: 00000000 T force_softfp
        #               U __aeabi_dadd
        # Binaries:     00008000 T __aeabi_dadd
        m = re.match(r"\s*(?:[0-9a-f]+\s+)?([A-Za-z])\s+(\S+)", line)
        if m:
            symbols[m.group(2)] = m.group(1)
    return symbols


def _host_compile_and_run(src, defines=(), cflags=(), link=()):
    """Compile a source file with the host compiler and run it.

    Returns (returncode, stdout, stderr, cmd).
    """
    out = BUILD_DIR / "host" / src.stem
    out.parent.mkdir(parents=True, exist_ok=True)

    cc = "gcc"
    cmd = [cc, "-O2", "-Wall", "-Wextra"] + list(defines) + list(cflags) + [str(src)] + list(link) + ["-o", str(out)]
    compile_result = subprocess.run(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        errors="replace",
    )
    if compile_result.returncode != 0:
        return compile_result.returncode, compile_result.stdout, compile_result.stderr, cmd

    run_result = subprocess.run(
        [str(out)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        errors="replace",
    )
    return run_result.returncode, run_result.stdout, run_result.stderr, cmd


# -----------------------------------------------------------------------------
# Host-native soft-FP tests (lib/fp/soft/*.c)
# -----------------------------------------------------------------------------
@pytest.mark.runtime
@pytest.mark.runtime_host
def test_host_aeabi_all():
    """Run the existing comprehensive aeabi host test."""
    src = ROOT / "lib" / "fp" / "soft" / "test_aeabi_all.c"
    rc, stdout, stderr, cmd = _host_compile_and_run(src, defines=["-DHOST_TEST"], link=["-lm"])
    if rc != 0:
        pytest.fail(f"test_aeabi_all failed: {cmd}\nstdout:\n{stdout}\nstderr:\n{stderr}")
    assert "ALL TESTS PASSED" in stdout, f"test_aeabi_all did not report pass:\n{stdout}\n{stderr}"


@pytest.mark.runtime
@pytest.mark.runtime_host
def test_host_soft_float_division():
    """Run the existing host-side ddiv/fdiv test."""
    src = ROOT / "lib" / "fp" / "soft" / "test_host.c"
    rc, stdout, stderr, cmd = _host_compile_and_run(src, defines=["-DHOST_TEST"], link=["-lm"])
    if rc != 0:
        pytest.fail(f"test_host failed: {cmd}\nstdout:\n{stdout}\nstderr:\n{stderr}")
    assert "ALL TESTS PASSED" in stdout, f"test_host did not report pass:\n{stdout}\n{stderr}"


@pytest.mark.runtime
@pytest.mark.runtime_host
def test_host_soft_float_mul():
    """Run the existing host-side dmul test."""
    src = ROOT / "lib" / "fp" / "soft" / "test_dmul_host.c"
    rc, stdout, stderr, cmd = _host_compile_and_run(src, defines=["-DHOST_TEST"], link=["-lm"])
    if rc != 0:
        pytest.fail(f"test_dmul_host failed: {cmd}\nstdout:\n{stdout}\nstderr:\n{stderr}")
    assert "ALL TESTS PASSED" in stdout, f"test_dmul_host did not report pass:\n{stdout}\n{stderr}"


@pytest.mark.runtime
@pytest.mark.runtime_host
def test_host_armeabi_helpers():
    """Run host-native algorithmic tests for lib/armeabi.c EABI helpers."""
    src = RUNTIME_DIR / "host" / "test_armeabi_host.c"
    rc, stdout, stderr, cmd = _host_compile_and_run(src, defines=["-DHOST_TEST"])
    if rc != 0:
        pytest.fail(f"test_armeabi_host failed: {cmd}\nstdout:\n{stdout}\nstderr:\n{stderr}")
    assert "ALL TESTS PASSED" in stdout, f"test_armeabi_host did not report pass:\n{stdout}\n{stderr}"


@pytest.mark.runtime
@pytest.mark.runtime_host
def test_host_builtin_helpers():
    """Run host-native algorithmic tests for lib/builtin.c bit/string helpers."""
    src = RUNTIME_DIR / "host" / "test_builtin_host.c"
    rc, stdout, stderr, cmd = _host_compile_and_run(
        src,
        defines=["-DHOST_TEST"],
        cflags=["-fno-builtin", "-Wno-builtin-declaration-mismatch"],
    )
    if rc != 0:
        pytest.fail(f"test_builtin_host failed: {cmd}\nstdout:\n{stdout}\nstderr:\n{stderr}")
    assert "ALL TESTS PASSED" in stdout, f"test_builtin_host did not report pass:\n{stdout}\n{stderr}"


# -----------------------------------------------------------------------------
# Cross-compiled runtime helper references
# -----------------------------------------------------------------------------
@pytest.mark.runtime
@pytest.mark.runtime_cross
def test_cross_aeabi_softfp(runtime_compiler):
    obj = _cross_compile("aeabi_softfp", runtime_compiler)
    syms = _nm_symbols(obj)

    # Soft-float double and single operations are lowered to EABI helpers
    # provided by lib/fp/libsoftfp.a.
    expected = {
        "__aeabi_dadd",
        "__aeabi_dmul",
        "__aeabi_d2iz",
        "__aeabi_fadd",
        "__aeabi_fmul",
        "__aeabi_f2iz",
    }
    missing = expected - set(syms)
    assert not missing, f"missing expected soft-float EABI symbols: {missing}"


@pytest.mark.runtime
@pytest.mark.runtime_cross
def test_cross_aeabi_divmod(runtime_compiler):
    obj = _cross_compile("aeabi_divmod", runtime_compiler)
    syms = _nm_symbols(obj)

    # On Cortex-M33 the compiler uses SDIV/UDIV instructions for 32-bit
    # division, so no __aeabi_idiv/__aeabi_uidiv references are emitted.
    # 64-bit division/modulo uses the combined EABI helpers from
    # lib/armeabi_divmod.S.
    expected = {
        "__aeabi_ldivmod",
        "__aeabi_lmod",
        "__aeabi_uldivmod",
        "__aeabi_ulmod",
    }
    missing = expected - set(syms)
    assert not missing, f"missing expected 64-bit divmod symbols: {missing}"

    # Sanity: the 32-bit paths did not fall back to EABI helpers on this target.
    assert "__aeabi_idiv" not in syms
    assert "__aeabi_uidiv" not in syms


@pytest.mark.runtime
@pytest.mark.runtime_cross
def test_cross_string_helpers(runtime_compiler):
    obj = _cross_compile("string", runtime_compiler)
    syms = _nm_symbols(obj)

    # memcpy/memset are referenced from the runtime library.
    assert "memcpy" in syms, "missing memcpy reference"
    assert "memset" in syms, "missing memset reference"


@pytest.mark.runtime
@pytest.mark.runtime_cross
def test_cross_longjmp(runtime_compiler):
    obj = _cross_compile("longjmp", runtime_compiler)
    syms = _nm_symbols(obj)

    # setjmp/longjmp are provided by libtcc1.
    assert "setjmp" in syms, "missing setjmp reference"
    assert "longjmp" in syms, "missing longjmp reference"





@pytest.mark.runtime
@pytest.mark.runtime_cross
def test_cross_vla(runtime_compiler):
    obj = _cross_compile("vla", runtime_compiler)
    syms = _nm_symbols(obj)

    # This fork lowers alloca()/VLA inline by manipulating SP rather than
    # calling the alloca helper in lib/alloca.S.  The test documents that
    # current behaviour: no alloca symbol is referenced and the function
    # compiles successfully.
    assert "alloca" not in syms
    assert "__alloca" not in syms


@pytest.mark.runtime
@pytest.mark.runtime_cross
def test_cross_builtin_bitops(runtime_compiler):
    obj = _cross_compile("builtin_bitops", runtime_compiler)
    syms = _nm_symbols(obj)

    # Builtin bswap/ctz/popcount are lowered to libgcc-style symbols that are
    # resolved by the armv8m-libtcc1.a runtime library.
    expected = {
        "__bswapsi2",
        "__bswapdi3",
        "__ctzsi2",
        "__ctzdi2",
        "__popcountsi2",
        "__popcountdi2",
    }
    missing = expected - set(syms)
    assert not missing, f"missing expected bitop runtime symbols: {missing}"


@pytest.mark.runtime
@pytest.mark.runtime_cross
def test_cross_aeabi_idiv_uidiv(runtime_compiler):
    obj = _cross_compile("aeabi_idiv_uidiv", runtime_compiler)
    syms = _nm_symbols(obj)

    # Direct calls to the 32-bit EABI division helpers resolve from lib/armeabi.c.
    assert "__aeabi_idiv" in syms, "missing __aeabi_idiv reference"
    assert "__aeabi_uidiv" in syms, "missing __aeabi_uidiv reference"


@pytest.mark.runtime
@pytest.mark.runtime_cross
def test_cross_aeabi_memset_memcpy(runtime_compiler):
    obj = _cross_compile("aeabi_memset_memcpy", runtime_compiler)
    syms = _nm_symbols(obj)

    # ARM EABI memory helpers from lib/armeabi.c.
    assert "__aeabi_memcpy" in syms, "missing __aeabi_memcpy reference"
    assert "__aeabi_memmove" in syms, "missing __aeabi_memmove reference"
    assert "__aeabi_memset" in syms, "missing __aeabi_memset reference"


@pytest.mark.runtime
@pytest.mark.runtime_cross
def test_cross_aeabi_llsr_llsl_lasr(runtime_compiler):
    obj = _cross_compile("aeabi_llsr_llsl_lasr", runtime_compiler)
    syms = _nm_symbols(obj)

    # ARM EABI 64-bit shift helpers from lib/armeabi.c.
    expected = {"__aeabi_llsr", "__aeabi_llsl", "__aeabi_lasr"}
    missing = expected - set(syms)
    assert not missing, f"missing expected 64-bit shift symbols: {missing}"


@pytest.mark.runtime
@pytest.mark.runtime_cross
def test_cross_aeabi_lcmp_ulcmp(runtime_compiler):
    obj = _cross_compile("aeabi_lcmp_ulcmp", runtime_compiler)
    syms = _nm_symbols(obj)

    # ARM EABI 64-bit comparison helpers from lib/armeabi.c.
    assert "__aeabi_lcmp" in syms, "missing __aeabi_lcmp reference"
    assert "__aeabi_ulcmp" in syms, "missing __aeabi_ulcmp reference"


@pytest.mark.runtime
@pytest.mark.runtime_cross
def test_cross_memcpy_memset_nobuiltin(runtime_compiler):
    obj = _cross_compile("memcpy_memset", runtime_compiler, extra_cflags=["-fno-builtin"])
    syms = _nm_symbols(obj)

    # With compiler builtins disabled, plain memcpy/memset calls are emitted
    # and resolved from the runtime library.
    assert "memcpy" in syms, "missing memcpy reference"
    assert "memset" in syms, "missing memset reference"


@pytest.mark.runtime
@pytest.mark.runtime_cross
def test_cross_muldi_divsi_notsymbols(runtime_compiler):
    """Document that generic __muldi3 / __divsi3 are not used on ARMv8-M.

    The ARMv8-M target has hardware MUL instructions for 64-bit products and
    SDIV/UDIV for 32-bit division, so the compiler does not reference the
    generic libgcc-style symbols.  The ARM EABI equivalents are tested above.
    """
    src_mul = RUNTIME_DIR / "cross" / "aeabi_divmod.c"  # 64-bit div/mod
    obj = _cross_compile("aeabi_divmod", runtime_compiler)
    syms = _nm_symbols(obj)

    assert "__muldi3" not in syms
    assert "__divsi3" not in syms
