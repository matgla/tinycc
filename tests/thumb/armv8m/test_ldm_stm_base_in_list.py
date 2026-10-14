"""ldm/stm must keep the base register in the list; illegal forms are rejected.

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
    "ldm r0!, {r0, r1}",
    "ldm.w r0!, {r0, r1}",
    "stm.w r0!, {r0, r1}",
    "stm r1!, {r0, r1}",
    "stm r8!, {r0, r8}",
])
def test_unpredictable_base_in_list_rejected(tmp_path, insn):
    r = _asm(tmp_path, " " + insn)
    assert r.returncode != 0, "%r silently assembled" % insn
