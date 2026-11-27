"""A prototype's `T p[n]` says nothing of the functions defined after it.

A parameter's size expression is kept (vla_param_exprs) for its function's
entry, and a function with one is neither inlined nor deferred: the body's
tokens do not hold it.  The list also held every prototype's since the last
definition -- one `void *memcpy(out void dst[n], void src[n], size_t n)`
in <string.h> turned inlining and -fdrop-unused-statics off for the whole
unit.  Only the function's own parameters count.
"""

import shutil
import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"
OBJDUMP = shutil.which("arm-none-eabi-objdump")

pytestmark = pytest.mark.skipif(OBJDUMP is None or not TCC.exists(), reason="needs armv8m-tcc and arm-none-eabi-objdump")

BODY = """
static unsigned long len2(unsigned long n) { unsigned long i = n; while (i > 0 && (i & 3)) i--; return i; }
static int dead(int x) { return x + 1; }
unsigned long f(unsigned long n) { return len2(n) + 1; }
"""

CASES = {
    "c_bound_before": ("", "void zz(unsigned long n, char d[n]);\n"),
    "yasc_bound_after": ("#pragma yasc\n", "void zz(char d[n], unsigned long n);\n"),
    "yasc_void_bytes": ("#pragma yasc\n", "void zz(out void d[?n], unsigned long n);\n"),
}


def _dis(tmp_path, src, *flags):
    c = tmp_path / "t.c"
    c.write_text(src)
    o = tmp_path / "t.o"
    proc = subprocess.run([str(TCC), "-O1", "-Wno-missing-prototypes", *flags, "-c", str(c), "-o", str(o)],
                          capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr
    return subprocess.run([OBJDUMP, "-d", str(o)], capture_output=True, text=True).stdout


@pytest.mark.parametrize("name", sorted(CASES))
def test_prototype_keeps_inlining(tmp_path, name):
    head, proto = CASES[name]
    flags = ["-ccompat"] if not head else []
    dis = _dis(tmp_path, head + proto + BODY, *flags)
    assert "<len2>" not in dis, dis  # inlined, and its standalone copy dropped
    assert "<dead>" not in dis, dis  # -fdrop-unused-statics still drops it


def test_own_bound_still_blocks(tmp_path):
    """`a[n++]`: the bound's side effect is the entry's, which a replayed body would lose."""
    dis = _dis(tmp_path, "static int g(int n, int a[n++]) { return n + a[0]; }\n"
                         "int h(int n, int *a) { return g(n, a); }\n", "-ccompat")
    assert "<g>:" in dis, dis
