"""yasc forbids plain pointers: every '*' says owned or view.

But a parameter's own pointer (the last '*' before its name) is a view unless
it says owned or borrows(...); its inner levels still say it.  `maybe`, `out`
and `drop` combine with either.  The rule covers every place
a pointer type is written -- parameters, returns, members, globals, locals,
typedefs, casts, compound literals, sizeof -- in yasc code only: a regular-C
header, the expansion of a macro defined in regular C (NULL), and the body of
a function written in regular C are not yasc.  A pointer to a function is
exempt: it can only borrow.  A typedef that already says it, or a pointer
declared in regular C, needs nothing more; the latter reads as `view maybe`
from yasc code (-Wlifetime).
"""

import subprocess
from pathlib import Path

import pytest

from cross_includes import INCLUDE_FLAGS

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

ERROR = "plain pointer in yasc: say owned or view"

PLAIN = {
    "global": "int *p;",
    "member": "struct S { int *m; };",
    "parameter_inner_level": "int f(char **s);",
    "parameter_array_element": "int f(char *a[]);",
    "parameter_function": "int f(char *g(int));",
    "parameter_function_result": "int f(char *(*cb)(int));",
    "return": "char *f(void);",
    "local": "void f(void) { int *p = 0; (void)p; }",
    "cast": "void f(void) { void * view v = (void *)0; (void)v; }",
    "sizeof": "int f(void) { return sizeof(char *); }",
    "typedef": "typedef char *str;",
    "second_level": "char * view * argv;",
    "array_pointer": "int (*ap)[3];",
    "function_pointer_pointer": "int (**fpp)(int);",
    "function_returning_plain": "int *(*pf)(void);",
    "compound_literal": "void f(void) { int * view * view p = (int *[]){0}; (void)p; }",
}

ANNOTATED = {
    "owned": "char * owned make(void);",
    "view": "int * view p;",
    "view_maybe": "int * view maybe p;",
    "owned_maybe_param": "void release(void * owned maybe p);",
    "out": "int fill(out int *n);",
    "drop": "void take(drop char * owned p);",
    "levels": "char * view * owned argv_copy;",
    "abstract": "int f(char *, int);",
    "parameter": "int f(char *s);",
    "parameter_maybe": "int f(char * maybe s);",
    "parameter_restrict": "int f(char * restrict s, const char * restrict t);",
    "parameter_inner_said": "int f(char * view *s);",
    "function_type_parameter": "typedef int cmp(const void *, const void *);",
    "cast": "int f(void) { return (char * view)0 == 0; }",
    "sizeof": "int f(void) { return sizeof(char * owned); }",
    "typedef_use": "typedef char * view str;\nstr s;",
    "function_pointer": "void (*fp)(int);",
    "function_pointer_param": "void h(void (*cb)(char *));",
    "function_typedef": "typedef int fn(int);\nfn *f2;",
    "array_decay_and_string": "int f(void) { char b[4] = undefined; char * view p = b; const char * view s = \"x\"; return p != s; }",
    "address_of": "int f(void) { int x = 1; int * view p = &x; return *p; }",
}


def _compile(tmp_path, body, *flags, header=None):
    if header is not None:
        (tmp_path / "h.h").write_text(header)
    src = tmp_path / "t.c"
    src.write_text(body + "\n")
    return subprocess.run(
        [str(TCC), "-Wno-missing-prototypes", *INCLUDE_FLAGS, "-c", "-Wno-lifetime", f"-I{tmp_path}", *flags, str(src), "-o", str(tmp_path / "t.o")],
        capture_output=True,
        text=True,
    )


@pytest.mark.parametrize("name", sorted(PLAIN))
def test_plain_pointer_is_an_error(tmp_path, name):
    proc = _compile(tmp_path, PLAIN[name], "-std=yasc")
    assert proc.returncode != 0 and f"t.c:1: error: {ERROR}" in proc.stderr, proc.stderr


@pytest.mark.parametrize("name", sorted(ANNOTATED))
def test_annotated_pointer_compiles(tmp_path, name):
    proc = _compile(tmp_path, ANNOTATED[name], "-std=yasc")
    assert proc.returncode == 0, proc.stderr


@pytest.mark.parametrize("name", sorted(PLAIN))
def test_regular_c_keeps_plain_pointers(tmp_path, name):
    proc = _compile(tmp_path, PLAIN[name].replace(" view", ""), "-ccompat")
    assert proc.returncode == 0, proc.stderr


def test_the_safe_default_is_yasc(tmp_path):
    proc = _compile(tmp_path, "int *p;")
    assert proc.returncode != 0 and ERROR in proc.stderr, proc.stderr


def test_line_of_a_nested_declarator(tmp_path):
    proc = _compile(tmp_path, "int x;\nint\n(*ap)\n[3];", "-std=yasc")
    assert proc.returncode != 0 and f"t.c:3: error: {ERROR}" in proc.stderr, proc.stderr


HEADER = """\
/* regular C */
#define MY_NULL ((void *)0)
typedef char *cstr;
char *find(const char *s, int c);
static inline char *first(char *s) { return s; }
"""


@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_regular_c_header_and_its_macros(tmp_path, opt):
    body = """\
#include "h.h"
#include <stddef.h>
int f(const char *s)
{
  void * view maybe p = NULL;
  void * view maybe q = MY_NULL;
  cstr c = find(s, 'x');
  char * view maybe d = unsafe(first(c));
  return p == q && d == c;
}"""
    proc = _compile(tmp_path, body, "-std=yasc", opt, header=HEADER)
    assert proc.returncode == 0, proc.stderr


# regular C defining functions in a macro, as utest.h's UTEST does
FN_MACRO_HEADER = """\
#include <stdlib.h>
struct reg { char **names; int n; };
extern struct reg registry;
#define DEFINE(NAME)                                   \\
  static void reg_##NAME(void) {                       \\
    char *name = malloc(8);                            \\
    if (name && registry.names)                        \\
      registry.names[registry.n++] = name;             \\
    else if (name)                                     \\
      free(name);                                      \\
  }                                                    \\
  void run_##NAME(int *result)
#define FIRST(s) (*(const char *)(s))
"""


def _compile_strict(tmp_path, body, header, *flags):
    (tmp_path / "h.h").write_text(header)
    src = tmp_path / "t.c"
    src.write_text(body + "\n")
    return subprocess.run(
        [str(TCC), "-Wno-missing-prototypes", *INCLUDE_FLAGS, "-c", f"-I{tmp_path}", *flags, str(src), "-o", str(tmp_path / "t.o")],
        capture_output=True,
        text=True,
    )


@pytest.mark.parametrize("opt", ["-O0", "-O1", "-O2", "-Os"])
def test_regular_c_macro_defines_functions(tmp_path, opt):
    """A function a regular-C macro defines is regular C at every -O: from -O1
    its body is saved and parsed later, in its file's dialect unless the saved
    tokens say it began in a regular-C expansion."""
    body = """\
#include "h.h"
DEFINE(one) { (void)result; reg_one(); }
DEFINE(two) { (void)result; reg_two(); }
int first(const char *s) { return FIRST(s); }"""
    proc = _compile_strict(tmp_path, body, FN_MACRO_HEADER, opt)
    assert proc.returncode == 0, proc.stderr
    assert [line for line in proc.stderr.splitlines() if "error" in line or "warning" in line] == []


@pytest.mark.parametrize("opt", ["-O0", "-O1", "-O2", "-Os"])
def test_yasc_after_a_function_macro_is_checked(tmp_path, opt):
    """The yasc body after the expansion, and yasc code around a regular-C
    macro in a function, stay yasc."""
    body = """\
#include "h.h"
DEFINE(one) { int *p = result; (void)p; reg_one(); }
int first(const char *s) { int *q = 0; (void)q; return FIRST(s); }"""
    proc = _compile_strict(tmp_path, body, FN_MACRO_HEADER, opt)
    assert proc.returncode != 0
    assert f"t.c:2: error: {ERROR}" in proc.stderr and f"t.c:3: error: {ERROR}" in proc.stderr, proc.stderr


def test_yasc_header_is_checked(tmp_path):
    proc = _compile(tmp_path, '#include "h.h"', "-ccompat", header="#pragma yasc\nint *bad(void);\n")
    assert proc.returncode != 0 and f"h.h:2: error: {ERROR}" in proc.stderr, proc.stderr


def test_regular_c_pointer_is_unchecked(tmp_path):
    """-Wlifetime: a pointer from regular C is unchecked (docs/yasc_modules.md,
    "Legacy C"; this replaced the earlier "reads as `view maybe`" rule): a
    regular-C function's pointer result and a variable typed through a
    regular-C typedef are neither maybe nor null.  A header that says more --
    __attribute__((malloc)) -- is believed (test_yasc_ownership.py)."""
    body = """\
#include "h.h"
int f(const char *s)
{
  char * view a = unsafe(find(s, 'a'));
  char * view maybe b = unsafe(find(s, 'b'));
  cstr c = find(s, 'c');
  return *a + (b != nullptr) + unsafe(*c);
}"""
    src = tmp_path / "t.c"
    (tmp_path / "h.h").write_text(HEADER)
    src.write_text(body + "\n")
    proc = subprocess.run(
        [str(TCC), "-Wno-missing-prototypes", *INCLUDE_FLAGS, "-c", "-std=yasc", f"-I{tmp_path}", str(src), "-o", str(tmp_path / "t.o")],
        capture_output=True,
        text=True,
    )
    assert proc.returncode == 0, proc.stderr
    warnings = [line.split("warning: ", 1)[1] for line in proc.stderr.splitlines() if "warning: " in line]
    assert warnings == []
