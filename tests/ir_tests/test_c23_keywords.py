"""C23 keywords: alignas, alignof, thread_local, constexpr, true, false, nullptr.

tcc's safe default is C23 (so is a yasc file in it); before C23 these are
identifiers, as static_assert and bool are.  thread_local maps to _Thread_local, which tcc does
not implement on this target (no TLS), as before.  constexpr objects are const
(not yet usable in constant expressions).
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

C23 = """\
alignas(16) static int a;
alignas(double) static char buf[8];
static_assert(alignof(double) == 8, "alignof");
static_assert(sizeof(true) == 1 && sizeof(false) == 1, "true/false are bool");
static_assert(_Generic(true, _Bool: 1, default: 0), "bool type");
static_assert(true == 1 && false == 0, "values");
constexpr int k = 5;
int use(void) { int *p = nullptr; bool b = true; return (p == 0) + b + k + a + buf[0]; }
"""

YASC = "#pragma yasc\n" + C23.replace("int *p = nullptr", "int * view maybe p = nullptr")

OLD = """\
int true = 1, false = 0, nullptr = 2, alignas = 3, alignof = 4, constexpr = 5;
int use(void) { return true + false + nullptr + alignas + alignof + constexpr; }
"""


def _cc(tmp_path, src, *flags):
    f = tmp_path / "t.c"
    f.write_text(src)
    return subprocess.run([str(TCC), "-c", *flags, str(f), "-o", str(tmp_path / "t.o")],
                          capture_output=True, text=True)


@pytest.mark.parametrize("flags", [("-std=c23", "-ccompat"), ("-std=gnu23",)], ids=["c23", "gnu23"])
@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_c23_keywords(tmp_path, flags, opt):
    p = _cc(tmp_path, C23, *flags, opt)
    assert p.returncode == 0, p.stderr
    assert "error" not in p.stderr and "warning" not in p.stderr, p.stderr


def test_safe_default_and_yasc(tmp_path):
    p = _cc(tmp_path, YASC)
    assert p.returncode == 0, p.stderr


@pytest.mark.parametrize("std", ["-std=c11", "-std=c17", "-std=gnu99"])
def test_identifiers_before_c23(tmp_path, std):
    p = _cc(tmp_path, OLD, std)
    assert p.returncode == 0, p.stderr


def test_preprocess_keeps_spelling(tmp_path):
    f = tmp_path / "t.c"
    f.write_text("alignas(8) int x; bool b = true;\n")
    p = subprocess.run([str(TCC), "-E", "-std=c23", str(f)], capture_output=True, text=True)
    assert "alignas(8) int x; bool b = true;" in p.stdout, p.stdout


def test_stdalign_macro_still_wins(tmp_path):
    p = _cc(tmp_path, "#define alignas(x) _Alignas(x)\nalignas(4) int v;\n", "-std=c23")
    assert p.returncode == 0, p.stderr
