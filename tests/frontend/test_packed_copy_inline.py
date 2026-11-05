"""Packed struct copies stay out of __aeabi_memmove where the core can do unaligned words.

A packed struct has alignment 1, so copying one through a word-sized chunk is an
unaligned access.  Mainline ARMv8-M does those in hardware; a call per copy made
tinycc's own 9-byte IROperand the largest single cost of a compile on the board.
Baseline cores fault on them, but tinycc cannot yet emit code for one at all (BL
has no baseline encoding here), so that side of the gate has no test.
"""

import subprocess

import pytest

from conftest import _find_compiler


@pytest.fixture
def tcc(request):
    return str(_find_compiler(request.config.getoption("--compiler")).resolve())


def undefined_symbols(tcc, tmp_path, source, *flags):
    src, obj = tmp_path / "t.c", tmp_path / "t.o"
    src.write_text(source)
    r = subprocess.run([tcc, "-nostdlib", *flags, "-c", str(src), "-o", str(obj)], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    nm = subprocess.run(["arm-none-eabi-nm", "-u", str(obj)], capture_output=True, text=True, check=True)
    return nm.stdout


P9 = "typedef struct __attribute__((packed)) { int a; int b; unsigned char c; } P;\n"


@pytest.mark.parametrize("opt", ["-O0", "-O1", "-O2"])
@pytest.mark.parametrize(
    "body",
    ["void f(P *d, const P *s) { *d = *s; }", "P f(const P *s) { return *s; }", "int f(const P *s) { P t = *s; return t.a + t.c; }"],
    ids=["assign", "return", "local"],
)
def test_mainline_inlines_a_packed_copy(tcc, tmp_path, opt, body):
    assert "memmove" not in undefined_symbols(tcc, tmp_path, P9 + body, opt, "-mcpu=cortex-m33")


def test_volatile_keeps_the_call(tcc, tmp_path):
    source = P9 + "void f(volatile P *d, const P *s) { *d = *s; }"
    assert "memmove" in undefined_symbols(tcc, tmp_path, source, "-O2", "-mcpu=cortex-m33")


def test_three_byte_tail_keeps_the_call(tcc, tmp_path):
    """A word for the tail would read past the object; 2 + 1 is not one chunk width."""
    source = "typedef struct __attribute__((packed)) { int a; short b; char c; } P;\nvoid f(P *d, const P *s) { *d = *s; }"
    assert "memmove" in undefined_symbols(tcc, tmp_path, source, "-O2", "-mcpu=cortex-m33")


def test_field_across_a_word_keeps_the_call(tcc, tmp_path):
    source = "typedef struct __attribute__((packed)) { char c; int a; char d[3]; } P;\nvoid f(P *d, const P *s) { *d = *s; }"
    assert "memmove" in undefined_symbols(tcc, tmp_path, source, "-O2", "-mcpu=cortex-m33")
