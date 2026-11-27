"""yasc forbids plain pointers: every '*' says owned or view.

But a parameter's own pointer (the last '*' before its name) is a view unless
it says owned or borrows(...); its inner levels still say it.  A local
pointer variable likewise is a `?view` unless its function owns it
(gives it an owned value, frees it, stores or returns it as owned): then the
error says owned.  A struct or union member's own pointer and its inner levels
are views too (test_yasc_member_view_default.py).  An array of pointers, a
local's inner level and a static local still say it.  `maybe`, `out`
and `drop` combine with either.  The rule covers every place
a pointer type is written -- parameters, returns, members, globals, locals,
typedefs, casts, compound literals, sizeof -- in yasc code only: a regular-C
header, the expansion of a macro defined in regular C (NULL), and the body of
a function written in regular C are not yasc.  A pointer to a function is
exempt: it can only borrow.  The error names the pointer's type, and the
caret is under it when that spelling is on the line once.  A typedef that already says it, or a pointer
declared in regular C, needs nothing more; the latter reads as `?view`
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
    "parameter_array_element": "int f(char *a[]);",
    "parameter_function": "int f(char *g(int));",
    "parameter_function_result": "int f(char *(*cb)(int));",
    "return": "char *f(void);",
    "cast": "void f(void) { void *view v = (void *)0; (void)v; }",
    "sizeof": "int f(void) { return sizeof(char *); }",
    "typedef": "typedef char *str;",
    "array_pointer": "int (*ap)[3];",
    "function_pointer_pointer": "int (**fpp)(int);",
    "function_returning_plain": "int *(*pf)(void);",
}

ANNOTATED = {
    "owned": "char *owned make(void);",
    "view": "int *view p;",
    "view_maybe": "int *?view p;",
    # a global's or static's pointers are views, an array's elements and inner levels too
    "global": "int *p;",
    "global_array": "char *names[2];",
    "global_second_level": "char ** argv;",
    # ... and a local's and a member's (test_yasc_nested_view_default.py)
    "local_array": "void f(void) { int x = 0; int *a[1] = {&x}; (void)a; }",
    "local_inner_level": "void f(void) { int y = 0; int *x = &y; int **pp = &x; (void)pp; }",
    "member_array": "struct s { char *names[2]; };",
    "local_static": "void f(void) { static int *p; (void)p; }",
    "owned_maybe_param": "void release(void *?owned p);",
    "out": "int fill(out int *n);",
    # a parameter's inner levels are views like its own pointer
    "parameter_inner_level": "int f(char **s);",
    "parameter_third_level": "int f(char ***s);",
    "drop": "void take(drop char *owned p);",
    "levels": "char *view *owned argv_copy;",
    "abstract": "int f(char *, int);",
    "parameter": "int f(char *s);",
    "parameter_maybe": "int f(char *?s);",
    "parameter_restrict": "int f(char *restrict s, char *restrict t);",
    "parameter_inner_said": "int f(char *view *s);",
    "function_type_parameter": "typedef int cmp(void *, void *);",
    "cast": "int f(char *?view p) { return cast<as(const char *?view)>(p) == nullptr; }",
    "sizeof": "int f(void) { return sizeof(char *owned); }",
    "typedef_use": "typedef char *view str;\nstr s;",
    "function_pointer": "void (*fp)(int);",
    "function_pointer_param": "void h(void (*cb)(char *));",
    "function_typedef": "typedef int fn(int);\nfn *f2;",
    "array_decay_and_string": "int f(void) { char b[4] = undefined; char * p = b; const char * s = \"x\"; return p != s; }",
    "address_of": "int f(void) { int x = 1; int * p = &x; return *p; }",
    "local": "int f(char *s) { const char *p = s; char b[2] = undefined; char *q = b; q[0] = *p; return q[0]; }",
    "local_maybe": "int f(void) { int *p = nullptr; return p != nullptr; }",
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
    proc = _compile(tmp_path, PLAIN[name].replace("*view", "*"), "-ccompat")
    assert proc.returncode == 0, proc.stderr


OWNED_LOCAL = """\
void free(drop void *?owned p);
void *?owned(free) malloc(unsigned long n);
void f(void) { char *p = malloc(4); free(p); }"""


def test_owned_local_says_owned(tmp_path):
    """a local its function owns is never a view: the error says owned"""
    proc = _compile(tmp_path, OWNED_LOCAL, "-std=yasc")
    assert proc.returncode != 0, proc.stderr
    assert "t.c:3: error: plain pointer in yasc: 'p' owns what it points at, say owned" in proc.stderr, proc.stderr
    assert "t.c:3: note: the function gives 'p' an owned value that may be null (line 3): write '*?owned'" in proc.stderr, proc.stderr


UNSAFE_LOCAL = """\
typedef struct ctx { void *?view *result; } ctx;
int fill(ctx * c);
int dlclose(drop void * h);
void *?owned(dlclose) f(void) {
  void *?result = nullptr;
  const ctx c = {.result = &result};
  if (fill(&c) != 0) return nullptr;
  return unsafe(result);
}
void *?owned(dlclose) g(void) {
  void *p = nullptr;
  const ctx c = {.result = &p};
  fill(&c);
  return p;
}"""


def test_unsafe_keeps_a_local_a_view(tmp_path):
    """a local returned as owned through unsafe(...) stays the view it is by
    default (libc dl.c dlopen): only the plain return says it owns"""
    proc = _compile(tmp_path, UNSAFE_LOCAL, "-std=yasc")
    assert proc.returncode != 0, proc.stderr
    assert "'result' owns" not in proc.stderr, proc.stderr
    assert "t.c:11: error: plain pointer in yasc: 'p' owns what it points at, say owned" in proc.stderr, proc.stderr


FOR_INIT = """\
char *?borrows(s) next(char *?s);
void free(drop void *?owned p);
void *?owned(free) malloc(unsigned long n);
int f(char *?s) { int n = 0; for (char *t = next(s); t; t = next(nullptr)) n += *t; return n; }
int g(void) { for (char *p = malloc(4); p; ) { free(p); break; } return 0; }
int h(char *?s) { for (char *?view t = s; t; t = nullptr) return *t; return 0; }"""


def test_for_init_local_is_a_local(tmp_path):
    """a pointer declared in a for-init is a local like any other: a view
    maybe by default, owned when its function owns it, `?view` redundant"""
    proc = _compile(tmp_path, FOR_INIT, "-std=yasc")
    assert proc.returncode != 0, proc.stderr
    assert "t.c:4:" not in proc.stderr, proc.stderr
    assert "t.c:5: error: plain pointer in yasc: 'p' owns what it points at, say owned" in proc.stderr, proc.stderr
    assert "t.c:6: warning: a local's pointer is a view that may be null unless the function owns it: drop 'view'" in proc.stderr, proc.stderr


def test_local_view_is_redundant(tmp_path):
    """`?view` on a local pointer's own '*' says what it is anyway
    (-Wredundant-view, fix-it: `*?`); `view` alone says more (never
    null), and an array of them, an inner level or a static say it"""
    body = """\
int f(char *s) {
  const char *view p = s;
  const char *?view q = nullptr;
  const char *view a[1] = {s};
  const char *view *view pp = &p;
  static const char *view st;
  return *p + (q != nullptr) + (a[0] != nullptr) + (*pp != nullptr) + (st != nullptr);
}"""
    proc = _compile(tmp_path, body, "-std=yasc", "-fdiagnostics-parseable-fixits")
    assert proc.returncode == 0, proc.stderr
    msg = "warning: a local's pointer is a view that may be null unless the function owns it: drop 'view'"
    lines = [l for l in proc.stderr.splitlines() if msg in l]
    assert lines == [f"{tmp_path / 't.c'}:3: {msg}"], proc.stderr
    assert 'fix-it:"' + str(tmp_path / "t.c") + '":{3:16-3:21}:""' in proc.stderr, proc.stderr
    # never null: a maybe value stored there is an error
    proc = _compile(tmp_path, "int f(const char *?s) { const char *view p = s; return *p; }", "-std=yasc",
                    "-Wlifetime")
    assert proc.returncode != 0 and "may be null" in proc.stderr, proc.stderr
    proc = _compile(tmp_path, "int f(const char *?s) { const char *p = s; return p ? *p : 0; }", "-std=yasc",
                    "-Wlifetime")
    assert proc.returncode == 0 and proc.stderr == "", proc.stderr
    proc = _compile(tmp_path, body, "-std=yasc", "-Wno-redundant-view")
    assert proc.returncode == 0 and "drop 'view'" not in proc.stderr, proc.stderr


def test_the_safe_default_is_yasc(tmp_path):
    proc = _compile(tmp_path, "int *f(void);")
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
int f(char *s)
{
  void *?p = NULL;
  void *?q = MY_NULL;
  cstr c = find(s, 'x');
  char *?d = unsafe(first(c));
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
@pytest.mark.parametrize("perm", ["as", "trunc"])
def test_cast_before_a_function_macro(tmp_path, opt, perm):
    """cast<as(int)>(x) in a body is no call to an undeclared `as`: the scan for
    unprototyped calls took it for one and generated the body at once, after
    the next expansion's "yasc off" had been read ahead -- and that replay left
    the dialect yasc, so the macro's regular-C functions were checked as yasc
    (libc's atoi_tests.c, -O1 and above)."""
    body = f"""\
#include "h.h"
static long v = 3;
DEFINE(one) {{ int x = cast<{perm}(int)>(v); (void)x; (void)result; reg_one(); }}
DEFINE(two) {{ (void)result; reg_two(); }}"""
    proc = _compile_strict(tmp_path, body, FN_MACRO_HEADER, opt)
    assert proc.returncode == 0, proc.stderr
    assert [line for line in proc.stderr.splitlines() if "error" in line or "warning" in line] == []


@pytest.mark.parametrize("opt", ["-O0", "-O1", "-O2", "-Os"])
def test_yasc_after_a_function_macro_is_checked(tmp_path, opt):
    """The yasc body after the expansion, and yasc code around a regular-C
    macro in a function, stay yasc."""
    body = """\
#include "h.h"
DEFINE(one) { typedef int *ip; reg_one(); }
int first(const char *s) { typedef int *iq; return FIRST(s); }"""
    proc = _compile_strict(tmp_path, body, FN_MACRO_HEADER, opt)
    assert proc.returncode != 0
    assert f"t.c:2: error: {ERROR}" in proc.stderr and f"t.c:3: error: {ERROR}" in proc.stderr, proc.stderr


def test_yasc_header_is_checked(tmp_path):
    proc = _compile(tmp_path, '#include "h.h"', "-ccompat", header="#pragma yasc\nint *bad(void);\n")
    assert proc.returncode != 0 and f"h.h:2: error: {ERROR}" in proc.stderr, proc.stderr


def test_regular_c_pointer_is_unchecked(tmp_path):
    """-Wlifetime: a pointer from regular C is unchecked (docs/yasc_modules.md,
    "Legacy C"; this replaced the earlier "reads as `?view`" rule): a
    regular-C function's pointer result and a variable typed through a
    regular-C typedef are neither maybe nor null.  A header that says more --
    __attribute__((malloc)) -- is believed (test_yasc_ownership.py)."""
    body = """\
#include "h.h"
int f(char *s)
{
  char *view a = unsafe(find(s, 'a'));
  char *?b = unsafe(find(s, 'b'));
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


# the error says which pointer: its type, with the caret under it (a
# function's result: the function's name too, the caret under that)
@pytest.mark.parametrize(
    "body,ptr",
    [
        ("int *f(void);", "the result of 'f' ('int *')"),
        ("char *view *f(void);", "the result of 'f' ('char *view *')"),
        ("int (*ap)[3];", "int (*)[3]"),
        ("int f(void) { return sizeof(char *); }", "char *"),
    ],
)
def test_error_names_the_pointer(tmp_path, body, ptr):
    proc = _compile(tmp_path, body, "-fdiagnostics-show-caret")
    assert proc.returncode != 0
    want = ptr if ptr.startswith("the result") else f"'{ptr}'"
    assert f"t.c:1: error: {ERROR} for {want}" in proc.stderr, proc.stderr


def test_result_named_with_caret_and_note(tmp_path):
    # `const char *` twice on the line: the function's name says which
    proc = _compile(tmp_path, "const char *name(char *s);", "-fdiagnostics-show-caret")
    lines = proc.stderr.splitlines()
    i = next(k for k, l in enumerate(lines) if "for the result of 'name' ('const char *')" in l)
    assert lines[i + 2].index("^") == lines[i + 1].index("name("), proc.stderr
    assert "note: '*owned' if the caller frees it" in proc.stderr, proc.stderr


def test_caret_under_the_pointer(tmp_path):
    proc = _compile(tmp_path, "int x; typedef char **s;", "-fdiagnostics-show-caret")
    lines = proc.stderr.splitlines()
    i = next(k for k, l in enumerate(lines) if f"{ERROR} for 'char *'" in l)
    src, caret = lines[i + 1], lines[i + 2]
    col = src.index("char **s")
    assert caret[col:col + 6] == "^~~~~~", proc.stderr
