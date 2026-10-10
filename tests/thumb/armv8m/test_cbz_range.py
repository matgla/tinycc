"""CBZ/CBNZ must reject what they cannot encode (never a silent wrong branch).

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


def _build(tmp_path, body, link):
    src = tmp_path / "t.S"
    src.write_text(".syntax unified\n.thumb\n.global _start\n_start:\n" + body)
    out = tmp_path / ("t.elf" if link else "t.o")
    cmd = [TCC, "-B", str(ROOT), "-I", str(ROOT / "include")]
    if link:
        cmd += ["-nostdlib", "-nodefaultlibs", "-static", "-Wl,-oformat=elf32-littlearm"]
    else:
        cmd += ["-c"]
    cmd += [str(src), "-o", str(out)]
    return subprocess.run(cmd, capture_output=True, text=True)


def test_cbz_backward_label_rejected(tmp_path):
    r = _build(tmp_path, "back: nop\n cbz r0, back\n bx lr\n", False)
    assert r.returncode != 0, "backward cbz silently assembled as offset 0"


def test_cbz_high_register_rejected(tmp_path):
    r = _build(tmp_path, " cbz r8, fwd\n nop\nfwd: bx lr\n", False)
    assert r.returncode != 0, "cbz r8 silently assembled as cbz r0"


def test_cbz_forward_too_far_rejected_by_linker(tmp_path):
    r = _build(tmp_path, " cbz r0, fwd\n .space 200\nfwd: bx lr\n", True)
    assert r.returncode != 0, "cbz 200 bytes ahead linked with wrapped offset"
    assert "not found" not in r.stderr, r.stderr


def test_cbz_forward_in_range_ok(tmp_path):
    r = _build(tmp_path, " cbz r0, fwd\n .space 128\nfwd: bx lr\n", True)
    assert r.returncode == 0, r.stderr
    data = (tmp_path / "t.elf").read_bytes()
    # cbz r0 with offset 126 = imm5 31 (0x1f<<3), i=1 -> 0xb100|0x200|0xf8
    assert b"\xf8\xb3" in data
