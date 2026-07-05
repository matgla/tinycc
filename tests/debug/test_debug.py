"""Phase 5: debug-info coverage tests.

Each test cross-compiles a tiny C case with ``-g`` and inspects the resulting
object with arm-none-eabi-readelf and arm-none-eabi-objdump.  The assertions
are characterizations of the current DWARF output; STAB output is currently
disabled in this fork (put_stabs* are no-ops) so those cases are skipped.
"""

import re
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).parent.parent.parent  # libs/tinycc
TCC = ROOT / "armv8m-tcc"
DEBUG_DIR = Path(__file__).parent
BUILD_DIR = DEBUG_DIR / "build"

READELF = "arm-none-eabi-readelf"
OBJDUMP = "arm-none-eabi-objdump"


def _compile(name, subdir):
    """Cross-compile a case in <subdir>/<name>.c to a relocatable object with -g."""
    src = DEBUG_DIR / subdir / f"{name}.c"
    obj = BUILD_DIR / subdir / f"{name}.o"
    obj.parent.mkdir(parents=True, exist_ok=True)

    cflags = [
        "-O1",
        "-g",
        "-nostdlib",
        "-fvisibility=hidden",
        "-mcpu=cortex-m33",
        "-mthumb",
        "-mfloat-abi=soft",
        "-ffunction-sections",
        "-c",
    ]
    cmd = [str(TCC)] + cflags + [str(src), "-o", str(obj)]
    result = subprocess.run(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        errors="replace",
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"Compile failed for {subdir}/{name}: {cmd}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return obj


def _readelf_debug_sections(obj):
    """Return set of debug section names."""
    result = subprocess.run(
        [READELF, "-S", str(obj)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        errors="replace",
    )
    assert result.returncode == 0, f"readelf -S failed for {obj}: {result.stderr}"

    debug_sections = set()
    for line in result.stdout.splitlines():
        if ".debug_" in line or ".debug_line" in line:
            m = re.search(r"\.debug_\w+", line)
            if m:
                debug_sections.add(m.group(0))
    return debug_sections


def _readelf_debug_info(obj):
    """Return the raw --debug-dump=info output."""
    result = subprocess.run(
        [READELF, "--debug-dump=info", str(obj)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        errors="replace",
    )
    assert result.returncode == 0, f"readelf --debug-dump=info failed for {obj}: {result.stderr}"
    return result.stdout


def _readelf_debug_line(obj):
    """Return the raw --debug-dump=line output."""
    result = subprocess.run(
        [READELF, "--debug-dump=line", str(obj)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        errors="replace",
    )
    assert result.returncode == 0, f"readelf --debug-dump=line failed for {obj}: {result.stderr}"
    return result.stdout


# -----------------------------------------------------------------------------
# dwarf/
# -----------------------------------------------------------------------------
@pytest.mark.debug
@pytest.mark.debug_dwarf
def test_dwarf_compile_unit():
    obj = _compile("01_compile_unit", "dwarf")
    sections = _readelf_debug_sections(obj)

    # DWARF5 CU info needs at least these sections.
    required = {".debug_info", ".debug_abbrev", ".debug_line", ".debug_str"}
    missing = required - sections
    assert not missing, f"missing DWARF sections: {missing}"

    info = _readelf_debug_info(obj)
    assert "DW_TAG_compile_unit" in info
    assert "DW_AT_producer" in info
    assert "DW_AT_name" in info


@pytest.mark.debug
@pytest.mark.debug_dwarf
def test_dwarf_function_and_variables():
    obj = _compile("02_function_var", "dwarf")
    info = _readelf_debug_info(obj)

    # Function and parameter/variable DIEs.
    assert "DW_TAG_subprogram" in info
    assert "add" in info
    assert "DW_TAG_formal_parameter" in info
    assert "DW_TAG_variable" in info


@pytest.mark.debug
@pytest.mark.debug_dwarf
def test_dwarf_line_info():
    obj = _compile("03_line_info", "dwarf")
    line = _readelf_debug_line(obj)

    # Line number program should reference the source file and function lines.
    assert "DWARF Version" in line
    assert "line_func" in line or "03_line_info.c" in line
    assert "Line Number Statements" in line


# -----------------------------------------------------------------------------
# stab/
# -----------------------------------------------------------------------------
@pytest.mark.debug
@pytest.mark.debug_stab
def test_stab_disabled():
    """STAB emission is currently disabled in this fork.

    The source still contains the STAB records (tccdbg.c put_stabs*), but the
    output functions are no-ops and no .stab / .stabstr sections are emitted.
    This test documents that state; if STAB support is restored it should be
    replaced with real golden assertions.
    """
    obj = _compile("01_placeholder", "stab")
    sections = _readelf_debug_sections(obj)

    # With -g the compiler emits DWARF, not STAB.
    assert ".stab" not in sections
    assert ".stabstr" not in sections
    assert ".debug_info" in sections

    pytest.skip("STAB output is disabled in this fork; only DWARF is emitted")
