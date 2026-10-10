"""`.previous` must not see `last_text_section` left over from the previous TU."""

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
def test_previous_section_is_cleared_between_translation_units(tmp_path):
    switches_section = tmp_path / "switches_section.c"
    uses_previous = tmp_path / "uses_previous.c"
    switches_section.write_text('__asm__(".section .foo,\\"a\\"\\n.text\\n");\n')
    uses_previous.write_text('__asm__(".previous\\n.byte 1\\n.text\\n");\n')

    result = subprocess.run(
        [
            TCC,
            "-B",
            str(ROOT),
            "-c",
            str(switches_section),
            str(uses_previous),
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
    assert "no previous section referenced" in result.stderr
