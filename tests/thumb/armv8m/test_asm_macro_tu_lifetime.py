"""Inline-asm macros must not survive into the next translation unit."""

import os
import signal
import subprocess
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[3]
TCC = os.getenv("TEST_CC") or str(ROOT / "armv8m-tcc")
if not os.path.exists(TCC):
    TCC = str(ROOT / "bin" / "armv8m-tcc")


@pytest.mark.skipif(not Path(TCC).exists(), reason="needs armv8m-tcc")
def test_inline_asm_macro_is_cleared_between_translation_units(tmp_path):
    defines_macro = tmp_path / "defines_macro.c"
    uses_unknown_opcode = tmp_path / "uses_unknown_opcode.c"
    defines_macro.write_text('__asm__(".macro mymac\\n nop\\n .endm\\n");\n')
    uses_unknown_opcode.write_text('__asm__("xymac");\n')

    result = subprocess.run(
        [
            TCC,
            "-B",
            str(ROOT),
            "-c",
            str(defines_macro),
            str(uses_unknown_opcode),
        ],
        capture_output=True,
        text=True,
    )

    assert result.returncode not in {
        -signal.SIGSEGV,
        -signal.SIGABRT,
        128 + signal.SIGSEGV,
        128 + signal.SIGABRT,
    }
    assert "known instruction expected" in result.stderr
