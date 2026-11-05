"""An indexed load or store must not depend on how full the operand pool is.

Folding `*(base + index)` takes four new pool slots.  The fold used to give up when
they were not free rather than grow the pool, as every other pass does, so a run of
array accesses kept its separate address computations from the moment the pool
filled up: with the pool compacted to its real size, after about ten of them.
"""

import re
import subprocess

import pytest

from conftest import _find_compiler


@pytest.fixture
def tcc(request):
    return str(_find_compiler(request.config.getoption("--compiler")).resolve())


def disassemble(tcc, tmp_path, source):
    src, obj = tmp_path / "t.c", tmp_path / "t.o"
    src.write_text(source)
    r = subprocess.run([tcc, "-O2", "-nostdlib", "-c", str(src), "-o", str(obj)], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    return subprocess.run(["arm-none-eabi-objdump", "-d", str(obj)], capture_output=True, text=True, check=True).stdout


SIZES = [4, 10, 16, 32, 64, 128]


@pytest.mark.parametrize("accesses", SIZES)
def test_every_byte_load_is_indexed(tcc, tmp_path, accesses):
    body = "".join(f"  s += p[s & {k % 7 + 1}];\n" for k in range(accesses))
    dis = disassemble(tcc, tmp_path, f"int f(const char *p, int s) {{\n{body}  return s;\n}}\n")
    assert len(re.findall(r"ldrb(?:\.w)?\s+\w+, \[\w+, \w+\]", dis)) == accesses


@pytest.mark.parametrize("accesses", SIZES)
def test_every_word_store_is_indexed(tcc, tmp_path, accesses):
    body = "".join(f"  p[s & {k % 7 + 1}] = s + {k};\n" for k in range(accesses))
    dis = disassemble(tcc, tmp_path, f"void f(int *p, int s) {{\n{body}}}\n")
    assert len(re.findall(r"str(?:\.w)?\s+\w+, \[\w+, \w+, lsl #2\]", dis)) == accesses
