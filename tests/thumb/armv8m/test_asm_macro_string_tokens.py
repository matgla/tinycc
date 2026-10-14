"""Assembler macro expansion must preserve string-token payload words."""

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


@pytest.mark.skipif(not Path(TCC).exists(), reason="needs armv8m-tcc")
def test_macro_expansion_preserves_string_token_payload(tmp_path):
    if shutil.which(OBJCOPY) is None:
        pytest.skip("needs arm-none-eabi-objcopy")

    src = tmp_path / "macro_string.S"
    src.write_text(
        ".data\n"
        ".macro m a\n"
        '.ascii "\\377\\377\\377\\377"\n'
        ".byte \\a\n"
        ".byte 7\n"
        ".endm\n"
        "m 5\n"
        "\n"
        ".byte 9\n"
    )
    obj = tmp_path / "macro_string.o"
    result = subprocess.run(
        [TCC, "-B", str(ROOT), "-c", str(src), "-o", str(obj)],
        capture_output=True,
        text=True,
    )
    assert result.returncode == 0, result.stderr

    data = tmp_path / "macro_string.data"
    subprocess.run(
        [OBJCOPY, "-O", "binary", "--only-section=.data", str(obj), str(data)],
        check=True,
    )
    assert data.read_bytes() == b"\xff\xff\xff\xff\x05\x07\x09"
