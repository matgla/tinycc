"""`.quad` must emit the full 64-bit value, not a `strtoll`-saturated one."""

import os
import shutil
import subprocess
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[3]
TCC = os.getenv("TEST_CC") or str(ROOT / "armv8m-tcc")
if not os.path.exists(TCC):
    TCC = str(ROOT / "bin" / "armv8m-tcc")
OBJCOPY = os.getenv("TEST_OBJCOPY") or "arm-none-eabi-objcopy"


def test_quad_emits_full_64bit_constants(tmp_path):
    if shutil.which(OBJCOPY) is None:
        pytest.skip("needs arm-none-eabi-objcopy")

    src = tmp_path / "quad.S"
    src.write_text(
        ".data\n"
        ".quad 0xffffffffffffffff\n"
        ".quad 0x8000000000000000\n"
        ".quad 18446744073709551615\n"
        ".quad -1\n"
        ".quad 0x123456789abcdef\n"
        ".quad 3*1024+7\n"
    )
    obj = tmp_path / "quad.o"
    result = subprocess.run(
        [TCC, "-B", str(ROOT), "-c", str(src), "-o", str(obj)],
        capture_output=True,
        text=True,
    )
    assert result.returncode == 0, result.stderr

    data = tmp_path / "quad.data"
    subprocess.run(
        [OBJCOPY, "-O", "binary", "--only-section=.data", str(obj), str(data)],
        check=True,
    )
    assert data.read_bytes() == (
        b"\xff" * 8
        + b"\x00\x00\x00\x00\x00\x00\x00\x80"
        + b"\xff" * 8
        + b"\xff" * 8
        + bytes.fromhex("ef cd ab 89 67 45 23 01".replace(" ", ""))
        + b"\x07\x0c\x00\x00" + b"\x00" * 4
    )


def test_quad_rejects_a_symbol_like_gas(tmp_path):
    src = tmp_path / "quad_sym.S"
    src.write_text(".data\nsym:\n.quad sym\n")
    result = subprocess.run(
        [TCC, "-B", str(ROOT), "-c", str(src), "-o", str(src.with_suffix(".o"))],
        capture_output=True,
        text=True,
    )
    assert result.returncode != 0
    assert "constant expected" in result.stderr

    gas = subprocess.run(
        ["arm-none-eabi-gcc", "-c", str(src), "-o", os.devnull],
        capture_output=True,
        text=True,
    )
    assert gas.returncode != 0, "GNU as now accepts .quad <symbol>"
