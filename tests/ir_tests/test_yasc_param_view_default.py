"""yasc: a parameter's own pointer is a view unless it says owned, and an int
handle goes only where the type says it is one.

A pointer points at memory, so the most restrictive reading is the default:
`T *p` as a parameter is `T * view p` -- no free, no escape, no return
without borrows(...).  Only what changes hands is written (`owned`,
`drop ... owned`), with `maybe`, `inout` / `out`.  `view` written there anyway
is -Wredundant-view (with a fix-it).  Inner levels, an array's element, a
function's result, a slice's element and every non-parameter pointer still
say it; a pointer to a function stays exempt.

An int is a number: it points at nothing, so it is never a view by default.
A handle (`int owned(close) fd`, `int view fd`) stored in a plain int -- a
parameter, a local, a global, a member, a result -- is an error; `(int)fd`
takes its number on purpose.  A regular-C parameter cannot say it: an owned
handle goes there as borrow(fd) / move(fd), a view as it is (also through a
prototype parked in a header's regular-C section).
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

LEVELS = ["-O0", "-O2"]
PLAIN = "plain pointer in yasc: say owned or view"
REDUNDANT = "a parameter's pointer is a view unless it says owned: drop 'view'"
HANDLE = "a handle is stored in a plain int: where it goes says 'int view' or 'int owned', or (int)fd takes its number"


def _param_owns_nothing(name):
    return (f"the parameter '{name}' owns nothing (a view unless it says owned): write '* owned {name}' when the "
            "function takes it over, 'drop' before the type too when it frees it")


PRELUDE = """\
#pragma yasc
void free(drop void * owned maybe p);
void keep(char * owned p);
int owned(close) maybe open(const char *path, int flags);
int close(int drop owned(close) maybe fd);
int rd(int view fd);
int num(int n);
"""
PRELUDE_LINES = PRELUDE.count("\n")


def _run(tmp_path, body, *flags, name="t.c"):
    src = tmp_path / name
    src.write_text(PRELUDE + body)
    return subprocess.run(
        [str(TCC), "-Wno-missing-prototypes", "-c", *flags, str(src), "-o", str(tmp_path / "t.o")],
        capture_output=True, text=True, cwd=tmp_path,
    )


def _diags(proc, name="t.c"):
    out = []
    for line in proc.stderr.splitlines():
        parts = line.split(": ", 2)
        if len(parts) == 3 and parts[1] in ("warning", "error") and parts[0].rsplit(":", 1)[0].endswith(name):
            out.append((int(parts[0].rsplit(":", 1)[1]) - PRELUDE_LINES, parts[1], parts[2]))
    return out


# ------------------------------------------------------------- the default

CLEAN = {
    "read": "int f(const char *s) { return s[0]; }\n",
    "maybe": "int f(const char * maybe s) { return s ? s[0] : 0; }\n",
    "inout": "void f(inout char *s) { s[0] = 1; }\n",
    "out": "void f(out int *n) { *n = 1; }\n",
    "restrict": "void f(inout char * restrict d, const char * restrict s) { d[0] = s[0]; }\n",
    "abstract_prototype": "int f(const char *, int);\n",
    "inner_level_said": "int f(char * view *argv) { return argv[0][0]; }\n",
    "array_parameter": "int f(char * view a[]) { return a[0][0]; }\n",
    "function_pointer_parameters": "int f(int (*cb)(const char *), const char *s) { return cb(s); }\n",
    "function_type": "typedef int cmp(const void *, const void *);\n",
    "owned_said": "void f(drop char * owned p) { free(p); }\n",
    "owned_kept": "void f(char * owned p) { keep(move(p)); }\n",
    "borrows_result": "const char * borrows(s) f(const char *s) { return s + 1; }\n",
    "elided_result": "const char * view f(const char *s) { return s; }\n",
    "fn_pointer_view_not_redundant": "int f(int (* view cb)(int)) { return cb(1); }\n",
    "slice_element_said": "int f([]char * view a) { return a.len > 0; }\n",
    "int_view_handle": "int f(int view fd) { return rd(fd); }\n",
    "handle_number": "int f(int view fd) { return num((int)fd); }\n",
}


@pytest.mark.parametrize("opt", LEVELS)
@pytest.mark.parametrize("name", sorted(CLEAN))
def test_clean(tmp_path, name, opt):
    proc = _run(tmp_path, CLEAN[name], opt)
    assert proc.returncode == 0 and _diags(proc) == [], proc.stderr


PLAIN_STILL = {
    "inner_level": "int f(char **s) { return s[0][0]; }\n",
    "array_element": "int f(char *a[]) { return a[0][0]; }\n",
    "function_parameter_result": "int f(char *g(int)) { return g(1)[0]; }\n",
    "fn_pointer_parameter_result": "int f(char *(*cb)(int)) { return cb(1)[0]; }\n",
    "slice_element": "int f([]char *a) { return a.len > 0; }\n",
    "result": "char *f(void);\n",
    "member": "struct s { char *p; };\n",
    "global": "char *g;\n",
}


@pytest.mark.parametrize("name", sorted(PLAIN_STILL))
def test_other_pointers_still_say_it(tmp_path, name):
    proc = _run(tmp_path, PLAIN_STILL[name])
    assert proc.returncode != 0 and (1, "error", PLAIN) in _diags(proc), proc.stderr


def test_redundant_view_warns_with_a_fixit(tmp_path):
    proc = _run(tmp_path, "int f(const char * view s);\n", "-fdiagnostics-parseable-fixits")
    assert proc.returncode == 0, proc.stderr
    assert _diags(proc) == [(1, "warning", REDUNDANT)], proc.stderr
    assert 't.c":{%d:20-%d:25}:""' % (PRELUDE_LINES + 1, PRELUDE_LINES + 1) in proc.stderr, proc.stderr
    # in each parameter, the outermost pointer; not borrows(...), not an inner level
    proc = _run(tmp_path, "void g(inout char * restrict view a, char * view maybe b, out char * view * view c);\n")
    assert [d for d in _diags(proc) if d[2] == REDUNDANT] == [(1, "warning", REDUNDANT)] * 3, proc.stderr
    assert _run(tmp_path, "int f(const char * view s);\n", "-Wno-redundant-view").stderr == ""


@pytest.mark.parametrize("opt", LEVELS)
def test_a_view_parameter_that_needs_owning(tmp_path, opt):
    # freed, kept, handed to an owned parameter, returned as owned: one error,
    # naming the parameter, the same at every level (a static body too)
    body = (
        "void a(char *p) { free(p); }\n"
        "struct box { char * owned v; };\n"
        "void b(inout struct box *x, char *p) { x->v = p; }\n"
        "void c(char *p) { keep(p); }\n"
        "char * owned d(char *p) { return p; }\n"
        "static void e(char *p) { free(p); }\n"
        "void g(char *p) { e(p); }\n"
    )
    proc = _run(tmp_path, body, opt, "-fmax-errors=0")
    assert proc.returncode != 0
    errs = sorted(l for l, k, m in _diags(proc) if m == _param_owns_nothing("p"))
    assert errs == [1, 3, 4, 5, 6], proc.stderr


def test_same_at_every_level(tmp_path):
    body = "void a(char *p) { free(p); }\nint b(char **s) { return s[0][0]; }\nint c(const char * view s);\n"
    outs = [_run(tmp_path, body, o).stderr for o in ("-O0", "-O2", "-fsyntax-only")]
    assert outs[0] == outs[1] == outs[2], outs


# ------------------------------------------------------------- int handles

@pytest.mark.parametrize("opt", LEVELS)
@pytest.mark.parametrize("where,body", [
    ("parameter", "  num(fd);\n"),
    ("local", "  int c = fd;\n  (void)c;\n"),
    ("global", "  g_fd = fd;\n"),
    ("member", "  struct m s = {.n = fd};\n  (void)s;\n"),
])
def test_handle_into_a_plain_int(tmp_path, where, body, opt):
    src = ("int g_fd;\nstruct m { int n; };\nvoid f(void)\n{\n  int owned(close) maybe fd = open(\"a\", 0);\n"
           "  if (fd < 0)\n    return;\n" + body + "  close(fd);\n}\n")
    proc = _run(tmp_path, src, opt)
    assert proc.returncode != 0 and (8, "error", HANDLE) in _diags(proc), proc.stderr


def test_handle_returned_as_a_plain_int(tmp_path):
    proc = _run(tmp_path, "int f(int view fd) { return fd; }\n")
    assert proc.returncode != 0 and (1, "error", HANDLE) in _diags(proc), proc.stderr


def test_handle_number_and_view_parameter(tmp_path):
    src = ("void f(void)\n{\n  int owned(close) maybe fd = open(\"a\", 0);\n  if (fd < 0)\n    return;\n"
           "  int n = (int)fd;\n  rd(fd);\n  num(n + (int)fd);\n  close(fd);\n}\n")
    proc = _run(tmp_path, src)
    assert proc.returncode == 0 and _diags(proc) == [], proc.stderr


def test_regular_c_parameter_borrow_or_move(tmp_path):
    # a regular-C int parameter: an owned handle says borrow(fd) or move(fd), a
    # view goes as it is -- also when the prototype is parked (a header's
    # regular-C section, not used before the call)
    (tmp_path / "h.h").write_text("#pragma yasc\n#pragma tcc c_begin\nint c_use(int fd);\nint c_take(int fd);\n"
                                  "#pragma tcc c_end\n")
    head = '#include "h.h"\n'
    bad = head + ("void f(void)\n{\n  int owned(close) maybe fd = open(\"a\", 0);\n  if (fd < 0)\n    return;\n"
                  "  c_use(fd);\n  close(fd);\n}\n")
    proc = _run(tmp_path, bad)
    assert proc.returncode != 0
    assert (7, "error", "'fd' is owned and goes to a regular-C parameter: write borrow(fd) or move(fd)") in _diags(proc), \
        proc.stderr
    good = head + ("void f(int view v)\n{\n  int owned(close) maybe fd = open(\"a\", 0);\n  if (fd < 0)\n    return;\n"
                   "  c_use(borrow(fd));\n  c_use(v);\n  c_take(move(fd));\n}\n")
    proc = _run(tmp_path, good)
    assert proc.returncode == 0 and _diags(proc) == [], proc.stderr


# ------------------------------------------------------------- C headers

def test_module_header_in_parameter_is_const(tmp_path):
    (tmp_path / "m.c").write_text("#pragma yasc\nmodule io;\nexport int get(const int *p, int *q, char * owned o, "
                                  "inout int *r);\n")
    proc = subprocess.run(
        [str(TCC), "-ccompat", "-c", "m.c", "-o", "m.o", f"-fmodule-header={tmp_path / 'inc'}"],
        capture_output=True, text=True, cwd=tmp_path,
    )
    assert proc.returncode == 0 and not proc.stderr, proc.stderr
    text = (tmp_path / "inc" / "io.h").read_text()
    # an `in` view's target is const (once), an owned one and an inout one not
    assert "int get(const int * p, int const * q, char * o, int * r);" in text, text
