"""push/pop of exactly one register that the 16-bit form cannot hold must use
STR/LDR with writeback (T3), never STMDB/LDMIA.W with a one-register list
(CONSTRAINED UNPREDICTABLE).

Regression lock for the fixed thumb-* bug report (removed 2026-10-07; see git history)
"""
import os
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[3]
TCC = os.getenv("TEST_CC") or str(ROOT / "armv8m-tcc")
if not os.path.exists(TCC):
    TCC = str(ROOT / "bin" / "armv8m-tcc")
OBJDUMP = os.getenv("TEST_OBJDUMP") or "arm-none-eabi-objdump"
OBJCOPY = os.getenv("TEST_OBJCOPY") or "arm-none-eabi-objcopy"
AS = "arm-none-eabi-as"

CASES = [
    "push {r8}", "pop {r8}", "push {r12}", "pop {r12}", "push {ip}", "pop {ip}",
    "pop {lr}", "push {lr}", "push {r0}", "pop {r0}", "pop {pc}",
    "push.w {r0}", "pop.w {r0}", "push.w {r8}", "pop.w {lr}", "push.w {lr}", "push.w {r0, r1}", "pop.w {r0, pc}",
    "push {r8, lr}", "pop {r8, pc}", "push {r0, r8}", "pop {r0, r8}",
]


def _text(obj, out):
    subprocess.run([OBJCOPY, "-O", "binary", "--only-section=.text", str(obj), str(out)], check=True)
    return Path(out).read_bytes()


@pytest.mark.parametrize("insn", CASES)
def test_push_pop_matches_gas(tmp_path, insn):
    if shutil.which(AS) is None:
        pytest.skip("arm-none-eabi-as missing")
    src = tmp_path / "t.S"
    src.write_text(".syntax unified\n.thumb\nf:\n " + insn + "\n bx lr\n")
    ours = tmp_path / "ours.o"
    gas = tmp_path / "gas.o"
    r = subprocess.run([TCC, "-B", str(ROOT), "-I", str(ROOT / "include"), "-c", str(src), "-o", str(ours)],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    subprocess.run([AS, "-march=armv8-m.main", str(src), "-o", str(gas)], check=True)
    assert _text(ours, tmp_path / "o.bin") == _text(gas, tmp_path / "g.bin"), insn


@pytest.mark.parametrize("insn", ["push {r8}", "pop {r8}", "pop {lr}", "push {ip}"])
def test_no_one_register_stmdb_ldmia(tmp_path, insn):
    src = tmp_path / "t.S"
    src.write_text(".syntax unified\n.thumb\nf:\n " + insn + "\n bx lr\n")
    obj = tmp_path / "t.o"
    r = subprocess.run([TCC, "-B", str(ROOT), "-I", str(ROOT / "include"), "-c", str(src), "-o", str(obj)],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    dis = subprocess.run([OBJDUMP, "-d", "-marm", "-Mforce-thumb", str(obj)],
                         capture_output=True, text=True, check=True).stdout
    body = dis.split("<f>:", 1)[1]
    assert "stm" not in body and "ldm" not in body, dis
