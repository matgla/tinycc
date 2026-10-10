"""`return a[i];` from a VLA scope must load the value before the scope-exit
SP restore releases the VLA (an exception taken in between would overwrite it).

Regression lock for the fixed bug report vla-sp-restore-before-return-value-load
(removed 2026-10-07; see git history).
"""
import os
import re
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
TCC = os.getenv("TEST_CC") or str(ROOT / "armv8m-tcc")
if not os.path.exists(TCC):
    TCC = str(ROOT / "bin" / "armv8m-tcc")
OBJDUMP = os.getenv("TEST_OBJDUMP") or "arm-none-eabi-objdump"

SRC = """
void use(int *);
int f32(int n) { int a[n]; use(a); return a[n - 1]; }
double f64(int n) { double a[n]; use((int *)a); return a[n - 1]; }
long long fll(int n) { long long a[n]; use((int *)a); { int k = 1; return a[k]; } }
char fch(int n) { char a[n]; use((int *)a); return a[n - 1]; }
"""


@pytest.mark.parametrize("opt", ["-O0", "-O1", "-O2"])
@pytest.mark.parametrize("fn", ["f32", "f64", "fll", "fch"])
def test_return_load_precedes_vla_release(tmp_path, opt, fn):
    if shutil.which(OBJDUMP) is None:
        pytest.skip("objdump missing")
    src = tmp_path / "t.c"
    src.write_text(SRC)
    obj = tmp_path / "t.o"
    r = subprocess.run([TCC, "-B", str(ROOT), "-I", str(ROOT / "include"), opt, "-c", str(src), "-o", str(obj)],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    dis = subprocess.run([OBJDUMP, "-d", "-marm", "-Mforce-thumb", str(obj)],
                         capture_output=True, text=True, check=True).stdout
    body = dis.split("<%s>:" % fn, 1)[1].split("\n\n", 1)[0].splitlines()[1:]
    insns = [l.split("\t", 2)[-1].strip() for l in body]
    restore = [i for i, t in enumerate(insns) if re.match(r"mov\s+sp, r(?!7\b)\d+", t)]
    # the first non-prologue "mov sp, rN" after the VLA alloc is the release
    assert restore, "\n".join(insns)
    release = restore[-1]
    for t in insns[release + 1:]:
        m = re.match(r"ldr\w*\s+[^\[]*\[(\w+)", t)
        assert not (m and m.group(1) not in ("sp", "r7")), \
            "load through VLA pointer after SP release:\n" + "\n".join(insns)
