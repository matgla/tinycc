"""-Wnull-dereference: a dereference the compiler proves to be of null.

The second kind of check: it runs after optimisation, on what the optimizer
proved, so -O2 may find what -O0 does not.  A memory access at address 0, or
through a value known to be null plus a constant offset (a member through
null), is reported once per line.  A literal non-zero address is the MMIO
idiom (`*(volatile T *)0x40`) and is left alone; a pointer null on only some
paths is not proven.  Plain C, the safe default; not in -Wall.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

PRELUDE = "struct S { int a, b; };\n"
MSG = "a null pointer is dereferenced"


def _run(tmp_path, body, *flags, obj="t.o"):
    src = tmp_path / "t.c"
    src.write_text(PRELUDE + body)
    return subprocess.run(
        [str(TCC), "-ccompat", "-c", *flags, str(src), "-o", str(tmp_path / obj)], capture_output=True, text=True
    )


def _lines(proc):
    return [int(l.split(":")[1]) - 1 for l in proc.stderr.splitlines() if MSG in l]


@pytest.mark.parametrize("opt", ["-O0", "-O1", "-O2", "-Os"])
def test_everywhere(tmp_path, opt):
    body = "int f(void)\n{\n  int *p = 0;\n  return *p;\n}\nint g(void)\n{\n  return *(volatile int *)0;\n}\n"
    assert _lines(_run(tmp_path, body, "-Wnull-dereference", opt)) == [4, 8]


def test_member_through_null_at_o0(tmp_path):
    body = "int f(void)\n{\n  struct S *s = 0;\n  return s->b;\n}\n"
    assert _lines(_run(tmp_path, body, "-Wnull-dereference", "-O0")) == [4]


@pytest.mark.parametrize("opt", ["-O1", "-O2"])
def test_found_by_optimisation(tmp_path, opt):
    body = "static int *getp(void)\n{\n  return 0;\n}\nint f(void)\n{\n  return *getp();\n}\n"
    assert _lines(_run(tmp_path, body, "-Wnull-dereference", "-O0")) == []
    assert _lines(_run(tmp_path, body, "-Wnull-dereference", opt)) == [7]


CLEAN = {
    "parameter": "int f(int *p)\n{\n  return *p;\n}\n",
    "local": "int f(int c)\n{\n  int x = c;\n  int *p = &x;\n  return *p;\n}\n",
    "one_path": "void f(int c)\n{\n  int *p = 0;\n  if (c)\n    p = &c;\n  *p = 1;\n}\n",
    "mmio": "int f(void)\n{\n  return *(volatile short *)126;\n}\n",
    "set_through_address": "void set(int **pp);\nint f(void)\n{\n  int *p = 0;\n  set(&p);\n  return *p;\n}\n",
}


@pytest.mark.parametrize("name", sorted(CLEAN))
@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_clean(tmp_path, name, opt):
    assert _lines(_run(tmp_path, CLEAN[name], "-Wnull-dereference", opt)) == []


def test_switches(tmp_path):
    body = "int f(void)\n{\n  int *p = 0;\n  return *p;\n}\n"
    assert _lines(_run(tmp_path, body)) == []
    assert _lines(_run(tmp_path, body, "-Wall")) == []
    src = tmp_path / "s.c"
    src.write_text("int f(void)\n{\n  int * view p = 0;\n  return *p;\n}\n")
    proc = subprocess.run([str(TCC), "-c", str(src), "-o", str(tmp_path / "s.o")], capture_output=True, text=True)
    assert MSG in proc.stderr, proc.stderr


@pytest.mark.parametrize("opt", ["-O1", "-O2", "-Os"])
def test_code_unchanged(tmp_path, opt):
    body = "int f(void)\n{\n  int *p = 0;\n  return *p;\n}\n"
    _run(tmp_path, body, opt, obj="plain.o")
    _run(tmp_path, body, "-Wnull-dereference", opt, obj="checked.o")
    assert (tmp_path / "plain.o").read_bytes() == (tmp_path / "checked.o").read_bytes()
