"""A regular-C macro's expansion in yasc code is regular C, and so are the
arguments written inside it; a yasc macro used inside a regular-C one reads
as regular C too.  A regular-C macro inside a yasc macro (in its body or an
argument) is regular C.  The '*' is in the dialect it was written in:
`PTR(char) p` with a regular-C PTR is a regular-C (unchecked) pointer though
`p` is yasc again, and `PTR(char) view p` qualifies it.  tcc's va_arg is
function-like, as gcc's, so its type is a regular-C argument:
`va_arg(ap, char *)`.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
REPO = CURRENT_DIR.parent.parent
TCC = REPO / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

C_H = """\
#define DECL(t, n) t n
#define ID(x) x
#define CALL(f, a) f(a)
#define CAST(T, v) ((T)(v))
#define PTR(T) T *
#define CHARP char *
#define ALIAS YID
"""

Y_H = """\
#pragma yasc
#define YID(x) x
#define YPTR(T) T * view
#define YCALL(m, a) m(a)
"""


def _run(tmp_path, body, *flags):
    (tmp_path / "c.h").write_text(C_H)
    (tmp_path / "y.h").write_text(Y_H)
    (tmp_path / "m.c").write_text('#include "c.h"\n#include "y.h"\n' + body)
    return subprocess.run(
        [str(TCC), f"-B{REPO}", "-std=yasc", "-Wno-lifetime", "-I.", *flags, "-c", "m.c", "-o", "m.o"],
        capture_output=True,
        text=True,
        cwd=tmp_path,
    )


NESTED = """\
static inline int h(int a)
{
  DECL(char *, p) = 0;          /* an argument of a C macro */
  ID(int out = a;)              /* a yasc word in one: a name */
  YID(DECL(char *, q) = 0;)     /* a C macro in a yasc macro's argument */
  YCALL(ID, int view = out;)    /* a C macro named by a yasc macro */
  CALL(YID, int maybe = view;)  /* a yasc macro named by a C macro */
  YID(ID)(int drop = maybe;)    /* a C macro after a yasc expansion, its arguments from the file */
  (void)p; (void)q;
  return drop;
}
int g(int a) { return h(a) + h(a + 1); }
"""


@pytest.mark.parametrize("opt", ["-O0", "-O1", "-O2", "-Os"])
def test_arguments_and_nesting(tmp_path, opt):
    proc = _run(tmp_path, NESTED, opt)
    assert proc.returncode == 0, proc.stderr
    assert not proc.stderr, proc.stderr


def test_yasc_macro_inside_c_macro_is_c(tmp_path):
    """YPTR's `view` inside ID(...) is a name: so `char * view u` is a syntax error."""
    proc = _run(tmp_path, "void g(char * view s) { ID(YPTR(char) u = s;) }\n")
    assert proc.returncode != 0
    assert "m.c:3: error: ';' expected" in proc.stderr, proc.stderr


def test_yasc_macro_alone_is_yasc(tmp_path):
    proc = _run(tmp_path, "void g(char * view s) { YPTR(char) u = s; (void)u; }\n")
    assert proc.returncode == 0, proc.stderr


@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_star_from_c_macro(tmp_path, opt):
    body = """\
void use(char * view);
void g(char * view s)
{
  PTR(char) a = s;       /* the '*' is regular C */
  CHARP b = s;
  PTR(char) view c = s;  /* qualified by yasc code after it */
  CHARP * view d = &b;   /* the second '*' is yasc's and says view */
  use(a); use(b); use(c); (void)d;
}
"""
    proc = _run(tmp_path, body, opt)
    assert proc.returncode == 0, proc.stderr
    assert not proc.stderr, proc.stderr


def test_star_after_c_macro_still_checked(tmp_path):
    """A '*' written in yasc after a regular-C macro is yasc's: plain is an error."""
    proc = _run(tmp_path, "void g(void) { CHARP * d = 0; (void)d; }\n")
    assert proc.returncode != 0
    assert "m.c:3: error: plain pointer in yasc: say owned or view" in proc.stderr, proc.stderr


@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_va_arg_type_is_regular_c(tmp_path, opt):
    body = """\
#include <stdarg.h>
int f(int n, ...)
{
  va_list ap;
  va_start(ap, n);
  char * view s = va_arg(ap, char *);
  va_end(ap);
  return s[0];
}
"""
    proc = _run(tmp_path, body, opt)
    assert proc.returncode == 0, proc.stderr


def test_object_alias_of_yasc_macro_reads_c(tmp_path):
    """A regular-C object-like macro naming a yasc function-like one: the
    arguments it takes from the file are inside the C expansion."""
    proc = _run(tmp_path, "void g(void) { ALIAS(char * p = 0;) (void)p; }\n")
    assert proc.returncode == 0, proc.stderr


@pytest.mark.parametrize("opt", ["-O1", "-O2"])
def test_code_unchanged(tmp_path, opt):
    """The same text as plain C and as yasc compiles to the same object."""
    body = "int g(int a)\n{\n  PTR(char) p = CAST(char *, &a);\n  CHARP q = p;\n  return ID(q[0]) + a;\n}\n"
    out = []
    for flags in (["-ccompat"], ["-std=yasc", "-Wno-lifetime"]):
        (tmp_path / "c.h").write_text(C_H)
        (tmp_path / "t.c").write_text('#include "c.h"\n' + body)
        proc = subprocess.run(
            [str(TCC), "-I.", *flags, opt, "-c", "t.c", "-o", "t.o"], capture_output=True, text=True, cwd=tmp_path
        )
        assert proc.returncode == 0, proc.stderr
        out.append((tmp_path / "t.o").read_bytes())
    assert out[0] == out[1]
