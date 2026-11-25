"""examples/yasc, the yasc tutorial: what the run-time test cannot see.

- 03_mistakes.c is meant to warn: tcc -O1 (and -O2) must say exactly what
  03_mistakes.diag (quoted in README.md) says, one diagnostic per mistake;
  -O0 too, but for the last one, which the optimizer finds at -O1 and the
  flow check words differently at -O0.
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


def _mistakes(tmp_path, opt):
    proc = subprocess.run(
        [str(TCC), "-Wno-missing-prototypes", "-nostdinc", f"-I{TOP / 'include'}", opt, "-fmax-errors=0", "-c", str(EXAMPLES / "03_mistakes.c"),
         "-o", str(tmp_path / "m.o")],
        capture_output=True, text=True,
    )
    # the lifetime mistakes are errors in yasc: the chapter does not compile
    assert proc.returncode != 0, proc.stderr
    return [line.split("/03_mistakes.c:", 1)[-1] for line in proc.stderr.splitlines()]


MISTAKES = [line.split("03_mistakes.c:", 1)[-1] for line in (EXAMPLES / "03_mistakes.diag").read_text().splitlines()]


@pytest.mark.parametrize("opt", ["-O1", "-O2"])
def test_mistakes_say_what_the_tutorial_says(tmp_path, opt):
    assert _mistakes(tmp_path, opt) == MISTAKES


def test_mistakes_at_O0(tmp_path):
    """-O0: the same, but for null_dereference: nothing() is not inlined, so
    the flow check says its result may be null (an error, strict lifetimes)
    instead of the optimizer finding a load from address 0 (-Wnull-dereference,
    a warning; README.md, chapter 3)"""
    want = [line.replace("warning: a null pointer is dereferenced", "error: a pointer that may be null is dereferenced")
            for line in MISTAKES]
    assert want != MISTAKES
    assert _mistakes(tmp_path, "-O0") == want


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


@pytest.mark.parametrize("example", ["01_hello", "02_ownership", "04_defer", "05_errors", "06_c_library", "07_methods", "09_config",
                                     "10_arrays"])
def test_chapter_is_clean_in_safe_mode(tmp_path, example):
    flags = _newlib_flags()
    if flags is None:
        pytest.skip("needs arm-none-eabi-gcc for the newlib headers")
    proc = subprocess.run([str(TCC), "-Wno-missing-prototypes", *flags, "-Wall", "-c", str(EXAMPLES / f"{example}.c"), "-o",
                           str(tmp_path / "x.o")], capture_output=True, text=True)
    assert proc.returncode == 0 and not proc.stderr, proc.stderr


@pytest.mark.parametrize("c_file", ["mem.c", "counter.c"])
def test_c_files_are_c(tmp_path, c_file):
    """The C halves build as regular C (-ccompat), and would not as yasc."""
    flags = _newlib_flags()
    if flags is None:
        pytest.skip("needs arm-none-eabi-gcc for the newlib headers")
    src = EXAMPLES / c_file
    proc = subprocess.run([str(TCC), "-Wno-missing-prototypes", *flags, "-ccompat", "-Wall", "-c", str(src), "-o", str(tmp_path / "c.o")],
                          capture_output=True, text=True)
    assert proc.returncode == 0 and not proc.stderr, proc.stderr
    proc = subprocess.run([str(TCC), "-Wno-missing-prototypes", *flags, "-c", str(src), "-o", str(tmp_path / "y.o")], capture_output=True, text=True)
    assert "plain pointer in yasc" in proc.stderr
