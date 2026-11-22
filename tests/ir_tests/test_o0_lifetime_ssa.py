"""-Wlifetime leaves -O0 its fast path.

-O0 skips SSA to compile fast (ra_o0_direct: what crosses a block stays in
memory).  The flow checks run on the raw IR before any pass (ir/lifetime.c),
so they need no SSA: -Wlifetime -- also through the safe default -- keeps the
fast path, and never changes the code at any level.
Needs a debug cross (the default `make cross`) for -dump-ir-passes.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

LOOP = "int sum(int n)\n{\n  int s = 0;\n  for (int i = 0; i < n; i++)\n    s += i;\n  return s;\n}\n"


def _compile(tmp_path, *flags, obj="t.o"):
    src = tmp_path / "t.c"
    src.write_text(LOOP)
    proc = subprocess.run(
        [str(TCC), "-c", *flags, str(src), "-o", str(tmp_path / obj)],
        capture_output=True, text=True,
    )
    assert proc.returncode == 0, proc.stderr
    assert "unsupported" not in proc.stderr, proc.stderr
    return proc


def _phis(tmp_path, *flags):
    proc = _compile(tmp_path, *flags, "-dump-ir-passes=ssa_phi")
    if "=== AFTER ssa_phi ===" not in proc.stdout:
        pytest.skip("cross built without CONFIG_TCC_DEBUG")
    return proc.stdout.count("<-- PHI")


@pytest.mark.parametrize("flags", [("-ccompat",), ("-ccompat", "-Wall"), ("-ccompat", "-Wlifetime"), ()],
                         ids=["ccompat", "wall", "wlifetime", "safe_default"])
def test_o0_has_no_ssa(tmp_path, flags):
    assert _phis(tmp_path, *flags, "-O0") == 0


def test_o1_still_builds_ssa(tmp_path):
    assert _phis(tmp_path, "-ccompat", "-O1") > 0


@pytest.mark.parametrize("level", ["-O0", "-O1", "-O2", "-Os"])
def test_wlifetime_keeps_output(tmp_path, level):
    _compile(tmp_path, "-ccompat", level, obj="plain.o")
    _compile(tmp_path, "-ccompat", level, "-Wlifetime", obj="checked.o")
    assert (tmp_path / "plain.o").read_bytes() == (tmp_path / "checked.o").read_bytes()
