"""C23 `auto` and GNU `__auto_type`: a variable's type from its initializer.

`auto` with no type specifier takes the type of the initializer after lvalue
conversion: arrays and functions decay to pointers, top-level qualifiers go
(`const auto` puts const back).  It means that in C23 (-std=c23, -std=yasc,
the safe default) and in a yasc file; before C23 it stays the storage class
(`auto x = 5;` is an int).  `__auto_type` always infers.  One plain name per
declaration, with an initializer that is an expression.

In yasc the qualifiers come with the type -- owned / maybe / view of a call's
result -- so `auto p = malloc(n);` owns its memory and the flow checks follow
it.  `auto q = p;` with p owned is an error: whether q takes p's memory or
borrows it is not written anywhere: `auto q = move(p);` or `auto v =
borrow(p);` (test_yasc_move.py).

The run-time behaviour (one evaluation of the initializer, values at every -O
level) is in c23_auto.c (test_qemu.py).
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")


def _compile(tmp_path, body, *flags):
    src = tmp_path / "t.c"
    src.write_text(body)
    return subprocess.run(
        [str(TCC), "-Wno-missing-prototypes", "-c", *flags, str(src), "-o", str(tmp_path / "t.o")], capture_output=True, text=True
    )


def _ok(proc):
    assert proc.returncode == 0, proc.stderr
    return [line.split("warning: ", 1)[1] for line in proc.stderr.splitlines() if "warning: " in line]


def _error(proc):
    assert proc.returncode != 0, "expected a compile error"
    lines = [line.split("error: ", 1)[1] for line in proc.stderr.splitlines() if "error: " in line]
    assert lines, proc.stderr
    return lines[0]


# _Generic sees the type after lvalue conversion, as auto does
TYPES = """
struct s { int a, b; };
int fn(int x) { return x; }
int arr[4];
const int ci = 3;
#define IS(e, T) _Static_assert(_Generic((e), T: 1, default: 0), #e " is not " #T)
void f(void)
{
  struct s sv = {1, 2};
  AUTO i = 5;          IS(i, int);
  AUTO l = 5L;         IS(l, long);
  AUTO ull = 5ULL;     IS(ull, unsigned long long);
  AUTO u = 5u;         IS(u, unsigned);
  AUTO d = 1.5;        IS(d, double);
  AUTO fl = 1.5f;      IS(fl, float);
  AUTO c = 'a';        IS(c, int);
  AUTO ch = (char)1;   IS(ch, char);
  AUTO p = arr;        IS(p, int *);
  AUTO q = &arr;       IS(q, int (*)[4]);
  AUTO fp = fn;        IS(fp, int (*)(int));
  AUTO str = "abc";    IS(str, char *);
  AUTO t = sv;         IS(t, struct s);
  AUTO k = ci;         IS(k, int);
  AUTO sum = i + d;    IS(sum, double);
  k = 4;               /* const went with the lvalue conversion */
  (void)l; (void)ull; (void)u; (void)fl; (void)c; (void)ch; (void)p; (void)q;
  (void)fp; (void)str; (void)t; (void)sum;
}
"""


@pytest.mark.parametrize(
    "spelling,flags",
    [
        ("auto", ("-ccompat", "-std=c23")),
        ("auto", ("-ccompat", "-std=gnu23")),
        ("__auto_type", ("-ccompat",)),  # C11, GNU spelling
        ("__auto_type", ("-ccompat", "-std=c23")),
    ],
)
def test_inferred_types(tmp_path, spelling, flags):
    assert _ok(_compile(tmp_path, TYPES.replace("AUTO", spelling), *flags)) == []


def test_auto_in_the_safe_default(tmp_path):
    # the safe default is C23 and yasc (no plain `int *` in the type names here)
    body = "void f(void)\n{\n  auto d = 1.5;\n  _Static_assert(_Generic(d, double: 1, default: 0), \"\");\n}\n"
    assert _ok(_compile(tmp_path, body)) == []


def test_auto_in_a_yasc_file_before_c23(tmp_path):
    body = "#pragma yasc\nvoid f(void)\n{\n  auto d = 1.5;\n  _Static_assert(_Generic(d, double: 1, default: 0), \"\");\n}\n"
    assert _ok(_compile(tmp_path, body, "-ccompat")) == []


def test_auto_before_c23_is_the_storage_class(tmp_path):
    body = (
        "void f(void)\n{\n  auto int a = 1;\n  auto d = 1.5;\n"
        "  _Static_assert(_Generic(d, int: 1, default: 0), \"\");\n  (void)a;\n}\n"
    )
    _ok(_compile(tmp_path, body, "-ccompat", "-std=c11"))


def test_auto_with_a_type_is_the_storage_class_in_c23(tmp_path):
    body = "void f(void)\n{\n  auto long a = 1;\n  _Static_assert(_Generic(a, long: 1, default: 0), \"\");\n}\n"
    assert _ok(_compile(tmp_path, body, "-ccompat", "-std=c23")) == []


def test_const_auto_is_const(tmp_path):
    body = "void f(void)\n{\n  const auto k = 5;\n  k = 6;\n}\n"
    assert "read-only" in "\n".join(_ok(_compile(tmp_path, body, "-ccompat", "-std=c23")))


def test_static_and_file_scope(tmp_path):
    body = (
        "auto g = 7;\nstatic auto h = 2.5;\n"
        "_Static_assert(_Generic(g, int: 1, default: 0), \"\");\n"
        "_Static_assert(_Generic(h, double: 1, default: 0), \"\");\n"
        "int f(void)\n{\n  static auto n = 3L;\n"
        "  _Static_assert(_Generic(n, long: 1, default: 0), \"\");\n  return n++;\n}\n"
    )
    assert _ok(_compile(tmp_path, body, "-ccompat", "-std=c23")) == []


def test_for_init(tmp_path):
    body = "int f(int n)\n{\n  int s = 0;\n  for (auto i = 0u; i < n; i++)\n    s += i;\n  return s;\n}\n"
    assert _ok(_compile(tmp_path, body, "-ccompat", "-std=c23", "-Wno-sign-compare")) == []


def test_initializer_with_commas_inside(tmp_path):
    body = (
        "int g(int a, int b);\n"
        "void f(void)\n{\n  auto x = g(1, 2);\n  auto y = (1, 2.5);\n"
        "  _Static_assert(_Generic(y, double: 1, default: 0), \"\");\n  (void)x;\n}\n"
    )
    _ok(_compile(tmp_path, body, "-ccompat", "-std=c23", "-Wno-unused-value"))


ERRORS = {
    "no_initializer": ("auto x;", "'auto' needs an initializer to take the type from"),
    "pointer_declarator": ("int i; auto *p = &i;", "'auto' declares a plain name: the type comes from the initializer"),
    "array_declarator": ("auto a[] = {1, 2};", "'auto' declares a plain name: the type comes from the initializer"),
    "braced": ("auto x = {1};", "'auto' takes the type of an expression, not of a braced initializer"),
    "two_declarators": ("auto a = 1, b = 2;", "'auto' declares one variable at a time"),
    "void": ("auto v = (void)0;", "'auto' cannot take the type void"),
    "typedef": ("typedef auto T = 1;", "'auto' declares a variable, not a typedef"),
}


@pytest.mark.parametrize("name", sorted(ERRORS))
def test_errors(tmp_path, name):
    decl, msg = ERRORS[name]
    assert _error(_compile(tmp_path, "void f(void)\n{\n  " + decl + "\n}\n", "-ccompat", "-std=c23")) == msg


NOT_HERE = {
    "parameter": "int f(auto x) { return x; }\n",
    "member": "struct s { auto x; };\n",
    "cast": "int f(void) { return (auto)1; }\n",
    "sizeof": "int n = sizeof(auto);\n",
}


@pytest.mark.parametrize("name", sorted(NOT_HERE))
def test_auto_where_no_initializer_follows(tmp_path, name):
    proc = _compile(tmp_path, NOT_HERE[name], "-ccompat", "-std=c23")
    assert _error(proc) == "'auto' takes the type of an initializer: it cannot be used here"


def test_auto_type_spelling_in_error(tmp_path):
    proc = _compile(tmp_path, "void f(void)\n{\n  __auto_type x;\n}\n", "-ccompat")
    assert _error(proc) == "'__auto_type' needs an initializer to take the type from"


# --- yasc: the qualifiers come with the type ---------------------------------

PRELUDE = """\
#pragma yasc
void * owned malloc(unsigned n);
void free(drop void * owned maybe p);
struct node { int v; };
struct node * borrows(static) maybe find(int k);
char * view first(char * view s);
int use(char * view p);
"""
PRELUDE_LINES = PRELUDE.count("\n")


def _yasc(tmp_path, body, *flags):
    proc = _compile(tmp_path, PRELUDE + body, "-ccompat", "-Wlifetime", *flags)
    out = []
    for line in proc.stderr.splitlines():
        parts = line.split(": ", 2)
        if len(parts) == 3 and parts[1] in ("warning", "error", "note"):
            out.append((int(parts[0].rsplit(":", 1)[1]) - PRELUDE_LINES, parts[1], parts[2]))
    return proc, out


LEVELS = ["-O0", "-O1", "-O2", "-Os"]


@pytest.mark.parametrize("opt", LEVELS)
def test_auto_owns_a_malloc_result(tmp_path, opt):
    proc, diags = _yasc(tmp_path, "void f(void)\n{\n  auto p = malloc(4);\n  use(p);\n}\n", opt)
    assert proc.returncode == 0, proc.stderr
    assert diags == [(5, "warning", "'p' leaks: it still owns memory at the end of its scope")]


@pytest.mark.parametrize("opt", LEVELS)
def test_auto_owned_freed_is_clean(tmp_path, opt):
    proc, diags = _yasc(tmp_path, "void f(void)\n{\n  auto p = malloc(4);\n  use(p);\n  free(p);\n}\n", opt)
    assert proc.returncode == 0 and diags == [], proc.stderr


def test_auto_maybe(tmp_path):
    proc, diags = _yasc(tmp_path, "int f(int k)\n{\n  auto q = find(k);\n  return q->v;\n}\n")
    assert proc.returncode == 0, proc.stderr
    assert diags == [(4, "warning", "'q' may be null where it is dereferenced")]


def test_auto_maybe_tested_is_clean(tmp_path):
    proc, diags = _yasc(tmp_path, "int f(int k)\n{\n  auto q = find(k);\n  return q ? q->v : 0;\n}\n")
    assert proc.returncode == 0 and diags == [], proc.stderr


def test_auto_view_owns_nothing(tmp_path):
    proc, diags = _yasc(tmp_path, "void f(char * view s)\n{\n  auto v = first(s);\n  free(v);\n}\n")
    assert proc.returncode == 0, proc.stderr
    assert (4, "warning", "a pointer that owns nothing is stored in an owned pointer") in diags


def test_auto_pointer_satisfies_the_plain_pointer_rule(tmp_path):
    proc, diags = _yasc(tmp_path, "int f(void)\n{\n  int i = 1;\n  auto p = &i;\n  return *p;\n}\n")
    assert proc.returncode == 0 and diags == [], proc.stderr


@pytest.mark.parametrize(
    "init",
    ["p", "s->buf"],
)
def test_auto_copy_of_an_owned_pointer_is_an_error(tmp_path, init):
    body = (
        "struct box { char * owned buf; };\n"
        "void f(char * owned p, struct box * view s)\n{\n  auto q = " + init + ";\n  free(q);\n  free(p);\n}\n"
    )
    proc, diags = _yasc(tmp_path, body)
    assert proc.returncode != 0
    msg = (
        "'auto' would copy the owned pointer 'p': write move(p) or borrow(p)"
        if init == "p"
        else "'auto' would copy an owned pointer: write move(...) or borrow(...)"
    )
    assert diags[0][1:] == ("error", msg)
