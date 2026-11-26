"""yasc spells the null pointer one way: nullptr.

In yasc code (the safe default) NULL and an integer constant zero used as a
pointer -- stored, passed, returned, compared, chosen by ?:, cast, or in an
initializer -- draw -Wzero-as-null-pointer-constant ("write nullptr").  What
the code spelled says so wherever it stands -- a macro's argument (`assert(p
!= NULL)`) or a macro of a header of its own -- but a system header's macro
(SIG_DFL, one comparing with NULL in its body) says nothing, nor does
-ccompat code.  `!p`, `if (p)` and pointer arithmetic write no null pointer.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
REPO = CURRENT_DIR.parent.parent
TCC = REPO / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

C_H = """\
#include <stddef.h>
#define ISNULL(p) ((p) == NULL)
#define CALL(f, a) f(a)
"""

# a system header (-isystem sys): its macros are quiet
SYS_H = """\
#include <stddef.h>
#define SYS_ISNULL(p) ((p) == NULL)
#define SYS_NONE NULL
#define SYS_ZERO ((char *)0)
#define SYS_CHECK(e) ((void)(e))
"""

PRE = """\
#include <stddef.h>
#include "c.h"
#include <s.h>
struct S { char * view maybe p; int n; };
void take(char * maybe p);
"""

NULL_MSG = "NULL is regular C: write nullptr"
ZERO_MSG = "0 as a null pointer: write nullptr"

WARNS = [
    ("void f(void) { char * view maybe p = NULL; take(p); }", NULL_MSG),
    ("int f(char * maybe p) { return p == NULL; }", NULL_MSG),
    ("void f(void) { take(NULL); }", NULL_MSG),
    ("void f(void) { char * view maybe p = 0; take(p); }", ZERO_MSG),
    ("void f(char * maybe p) { p = 0; take(p); }", ZERO_MSG),
    ("void f(void) { take(0); }", ZERO_MSG),
    ("char * view maybe f(char * maybe q) { take(q); return 0; }", ZERO_MSG),
    ("int f(char * maybe p) { return p != 0; }", ZERO_MSG),
    ("int f(char * maybe p) { return 0 == p; }", ZERO_MSG),
    ("char * view maybe f(inout char * maybe p, int c) { return c ? p : 0; }", ZERO_MSG),
    ("char * view maybe f(inout char * maybe p, int c) { return c ? 0 : p; }", ZERO_MSG),
    ("void f(void) { char * view maybe q = 0; take(q); }", ZERO_MSG),
    ("void f(char * maybe p) { p = '\\0'; take(p); }", ZERO_MSG),
    ("void f(char * maybe p) { p = 1 - 1; take(p); }", ZERO_MSG),
    ("int f(void) { struct S s = {0}; return s.n; }", ZERO_MSG),
    ("int f(int k) { struct S s = { .p = 0, .n = k }; return s.n; }", ZERO_MSG),
    ("int f(void) { char * view maybe a[2] = {0}; return a[1] != nullptr; }", ZERO_MSG),
    ("static struct S g = {0};\nint f(void) { return g.n; }", ZERO_MSG),
    # spelled by this code: in a macro's argument, a header macro of its own
    ("int f(char * maybe p) { SYS_CHECK(p != NULL); return 0; }", NULL_MSG),
    ("int f(char * maybe p) { SYS_CHECK(p != 0); return 0; }", ZERO_MSG),
    ("void f(void) { CALL(take, NULL); }", NULL_MSG),
    ("void f(void) { CALL(take, 0); }", ZERO_MSG),
    ("int f(char * maybe p) { return ISNULL(p); }", NULL_MSG),
]

QUIET = [
    "void f(void) { char * view maybe p = nullptr; take(p); }",
    "int f(char * maybe p) { return p == nullptr || p != nullptr; }",
    "int f(char * maybe p) { return !p; }",
    "int f(char * maybe p) { if (p) return 1; return 0; }",
    "int f(char * maybe p) { return p && *p; }",
    "char * view maybe f(inout char * maybe p) { return p + 0; }",
    "long f(char *p, char *q) { return p - q; }",
    "int f(void) { int x = 0; long y = 0; return x + cast<int(int)>(y); }",
    "int f(void) { struct S s = {}; return s.n; }",
    "struct T { int n; char * view maybe p; };\nint f(void) { struct T t = {0}; return t.n; }",
    # a system header's macros
    "int f(char * maybe p) { return SYS_ISNULL(p); }",
    "void f(void) { take(SYS_NONE); take(SYS_ZERO); }",
]


def _compile(tmp_path, body, *flags):
    (tmp_path / "c.h").write_text(C_H)
    (tmp_path / "sys").mkdir(exist_ok=True)
    (tmp_path / "sys" / "s.h").write_text(SYS_H)
    (tmp_path / "t.c").write_text(PRE + body + "\n")
    return subprocess.run(
        [str(TCC), f"-B{REPO}", "-I.", "-isystem", "sys", "-c", *flags, "t.c", "-o", "t.o"],
        capture_output=True, text=True, cwd=tmp_path,
    )


@pytest.mark.parametrize("body, message", WARNS, ids=[str(i) for i in range(len(WARNS))])
def test_null_spelled_otherwise_warns(tmp_path, body, message):
    proc = _compile(tmp_path, body)
    assert proc.returncode == 0 and f"warning: {message}" in proc.stderr, proc.stderr


@pytest.mark.parametrize("body", QUIET, ids=[str(i) for i in range(len(QUIET))])
def test_nullptr_and_regular_c_are_quiet(tmp_path, body):
    # the snippets' f has no prototype: -Wmissing-prototypes is not this test's
    proc = _compile(tmp_path, body, "-Werror", "-Wno-missing-prototypes")
    assert proc.returncode == 0 and proc.stderr == "", proc.stderr


@pytest.mark.parametrize("body, message", WARNS[:4], ids=[str(i) for i in range(4)])
def test_option_controls_it(tmp_path, body, message):
    proc = _compile(tmp_path, body, "-Wno-zero-as-null-pointer-constant")
    assert proc.returncode == 0 and message not in proc.stderr, proc.stderr
    proc = _compile(tmp_path, body, "-Werror=zero-as-null-pointer-constant")
    assert proc.returncode != 0 and f"error: {message}" in proc.stderr, proc.stderr


def test_regular_c_says_nothing(tmp_path):
    body = """\
#include <stddef.h>
struct S { char *p; int n; };
void take(char *p);
char *f(char *p, int c)
{
  struct S s = {0};
  char *q = NULL;
  take(0);
  if (p == NULL || p == 0)
    return c ? q : 0;
  return s.p;
}
"""
    (tmp_path / "r.c").write_text(body)
    for flags in (("-ccompat",), ("-std=c11",), ("-std=c23",)):
        proc = subprocess.run([str(TCC), f"-B{REPO}", *flags, "-Werror", "-c", "r.c", "-o", "r.o"],
                              capture_output=True, text=True, cwd=tmp_path)
        assert proc.returncode == 0 and proc.stderr == "", (flags, proc.stderr)


def test_once_per_place(tmp_path):
    proc = _compile(tmp_path, "int f(char * maybe p)\n{\n  if (p == NULL)\n    return 1;\n  return p == 0;\n}")
    lines = [l for l in proc.stderr.splitlines() if "write nullptr" in l]
    assert len(lines) == 2 and "t.c:8:" in lines[0] and "t.c:10:" in lines[1], proc.stderr
