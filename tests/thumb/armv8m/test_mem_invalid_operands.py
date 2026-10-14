"""Invalid load/store operands must be rejected, never re-encoded as another insn.

Regression lock for the fixed thumb-* bug report (removed 2026-10-07; see git history)
"""
import os
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[3]
TCC = os.getenv("TEST_CC") or str(ROOT / "armv8m-tcc")
if not os.path.exists(TCC):
    TCC = str(ROOT / "bin" / "armv8m-tcc")


def _asm(tmp_path, body):
    src = tmp_path / "t.S"
    src.write_text(".syntax unified\n.thumb\nf:\n" + body + "\n bx lr\n")
    out = tmp_path / "t.o"
    return subprocess.run([TCC, "-B", str(ROOT), "-I", str(ROOT / "include"), "-c", str(src), "-o", str(out)],
                          capture_output=True, text=True)


@pytest.mark.parametrize("insn", [
    "str r0, [pc, #8]",
    "str r0, [pc, #-8]",
    "strb r0, [pc, #4]",
    "strh r0, [pc, #4]",
    "ldrd r0, r1, [r2, #6]",
    "strd r0, r1, [r2, #6]",
    "ldrd r0, r0, [r2]",
])
def test_invalid_mem_operand_rejected(tmp_path, insn):
    r = _asm(tmp_path, " " + insn)
    assert r.returncode != 0, "%r silently assembled" % insn


@pytest.mark.parametrize("insn", [
    "str r0, [r1, #8]",
    "strb r0, [r1, #4]",
    "strh r0, [r1, #4]",
    "ldr r0, [pc, #8]",
    "ldrd r0, r1, [r2, #8]",
    "strd r0, r1, [r2, #-8]",
    "ldrd r0, r1, [r2, #8]!",
])
def test_valid_mem_operand_accepted(tmp_path, insn):
    r = _asm(tmp_path, " " + insn)
    assert r.returncode == 0, r.stderr
