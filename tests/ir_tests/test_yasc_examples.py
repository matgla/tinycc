"""examples/yasc, the yasc tutorial: what the run-time test cannot see.

- 03_mistakes.c is meant to warn: tcc -O1 must say exactly what
  03_mistakes.diag (quoted in README.md) says, one diagnostic per mistake
  (-O1 for the last one, which the optimizer finds).
- The other chapters must compile without a warning in safe mode (no
  -ccompat, -Wall), the way a reader builds them.
The programs themselves run in test_qemu.py::test_yasc_tutorial.
"""

import shutil
import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TOP = CURRENT_DIR.parent.parent
TCC = TOP / "armv8m-tcc"
EXAMPLES = TOP / "examples" / "yasc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")


def test_mistakes_say_what_the_tutorial_says(tmp_path):
    proc = subprocess.run(
        [str(TCC), "-nostdinc", f"-I{TOP / 'include'}", "-O1", "-c", str(EXAMPLES / "03_mistakes.c"), "-o",
         str(tmp_path / "m.o")],
        capture_output=True, text=True,
    )
    assert proc.returncode == 0, proc.stderr
    got = [line.split("/03_mistakes.c:", 1)[-1] for line in proc.stderr.splitlines()]
    want = [line.split("03_mistakes.c:", 1)[-1] for line in (EXAMPLES / "03_mistakes.diag").read_text().splitlines()]
    assert got == want, proc.stderr


def _newlib_flags():
    gcc = shutil.which("arm-none-eabi-gcc")
    if not gcc:
        return None
    sysroot = subprocess.run([gcc, "-mcpu=cortex-m33", "-mthumb", "-mfloat-abi=soft", "--print-sysroot"],
                             capture_output=True, text=True).stdout.strip()
    inc = CURRENT_DIR / "libc_includes"
    # system headers, as in a real sysroot (cross_includes.py)
    dirs = [TOP / "include", inc, CURRENT_DIR / "libc_imports", inc / "newlib", f"{sysroot}/include"]
    return ["-nostdinc"] + [f"-isystem{d}" for d in dirs]


@pytest.mark.parametrize("example", ["01_hello", "02_ownership", "04_defer", "05_errors", "06_c_library", "07_methods", "09_config"])
def test_chapter_is_clean_in_safe_mode(tmp_path, example):
    flags = _newlib_flags()
    if flags is None:
        pytest.skip("needs arm-none-eabi-gcc for the newlib headers")
    proc = subprocess.run([str(TCC), *flags, "-Wall", "-c", str(EXAMPLES / f"{example}.c"), "-o",
                           str(tmp_path / "x.o")], capture_output=True, text=True)
    assert proc.returncode == 0 and not proc.stderr, proc.stderr


@pytest.mark.parametrize("c_file", ["mem.c", "counter.c"])
def test_c_files_are_c(tmp_path, c_file):
    """The C halves build as regular C (-ccompat), and would not as yasc."""
    flags = _newlib_flags()
    if flags is None:
        pytest.skip("needs arm-none-eabi-gcc for the newlib headers")
    src = EXAMPLES / c_file
    proc = subprocess.run([str(TCC), *flags, "-ccompat", "-Wall", "-c", str(src), "-o", str(tmp_path / "c.o")],
                          capture_output=True, text=True)
    assert proc.returncode == 0 and not proc.stderr, proc.stderr
    proc = subprocess.run([str(TCC), *flags, "-c", str(src), "-o", str(tmp_path / "y.o")], capture_output=True, text=True)
    assert "plain pointer in yasc" in proc.stderr
