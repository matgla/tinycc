"""yasc `restrict` (S3, docs/static_analysis_todo.md): an exclusive borrow,
checked.

`T * restrict view p` (also with borrows(...)) keeps restrict in the type;
regular C drops it as before.  What becomes a restrict pointer must be
exclusive: an owned pointer, another restrict pointer, a local's address or
a local array -- a view may be shared (Rust's &T never becomes &mut T), and
a global's or static's address is reachable by name (unsafe).  At a call,
no other argument may point into the object a restrict argument points
into: the same variable (a pointer that owns nothing counts as its owner),
the same local object, through copies and offsets.  -Wlifetime, so an error
in the safe default and lowered only by unsafe.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

LEVELS = ["-O0", "-O1", "-O2", "-Os"]

PRELUDE = """\
void * owned maybe malloc(unsigned n);
void free(drop void * owned maybe p);
void cpy(char * restrict view d, const char * view s, int n);
void two(char * restrict view a, char * restrict view b);
char gbuf[8];
"""
PRELUDE_LINES = PRELUDE.count("\n")

SHARED = "a view may be shared: a restrict pointer needs an owned pointer, a local's address or another restrict pointer"
GLOBAL = "a restrict pointer to a global or static: the code can reach it by name too: write unsafe(...)"


def _compile(tmp_path, body, *flags):
    src = tmp_path / "t.c"
    src.write_text(PRELUDE + body)
    return subprocess.run(
        [str(TCC), "-Wno-missing-prototypes", "-Wno-cross-tu", "-Wno-tainted-format", "-c", "-fmax-errors=0", *flags, str(src), "-o", str(tmp_path / "t.o")],
        capture_output=True,
        text=True,
    )


def _diags(proc):
    out = []
    for line in proc.stderr.splitlines():
        parts = line.split(": ", 2)
        if len(parts) == 3 and parts[1] in ("warning", "error"):
            where = parts[0].rsplit(":", 1)
            ln = int(where[1]) - PRELUDE_LINES if len(where) == 2 and where[1].isdigit() else 0
            out.append((ln, parts[1], parts[2]))
    return out


BAD = {
    "view_to_restrict": ("void f(char * view v, const char * view s)\n{\n  cpy(v, s, 1);\n}\n", 3, SHARED),
    # p + n keeps what p is: a view stays one
    "derived_from_view": ("void f(char * view v, const char * view s)\n{\n  cpy(v + 1, s, 1);\n}\n", 3, SHARED),
    "global": ("void f(const char * view s)\n{\n  cpy(gbuf, s, 4);\n}\n", 3, GLOBAL),
    "static_local": ("void f(const char * view s)\n{\n  static char b[4];\n  cpy(b, s, 4);\n}\n", 4, GLOBAL),
    "same_array": (
        "void f(void)\n{\n  char buf[8] = {0};\n  cpy(buf, buf, 4);\n}\n",
        4,
        "argument 1 of 'cpy' is restrict, but argument 2 points into 'buf' too",
    ),
    "offset_into_array": (
        "void f(void)\n{\n  char buf[8] = {0};\n  cpy(buf + 4, buf, 4);\n}\n",
        4,
        "argument 1 of 'cpy' is restrict, but argument 2 points into 'buf' too",
    ),
    "same_owned": (
        "void f(void)\n{\n  char * owned maybe p = malloc(8);\n  if (!p)\n    return;\n  cpy(p, p + 1, 4);\n"
        "  free(p);\n}\n",
        6,
        "argument 1 of 'cpy' is restrict, but argument 2 points into 'p' too",
    ),
    "same_restrict_param": (
        "void f(char * restrict view r)\n{\n  cpy(r, r, 1);\n}\n",
        3,
        "argument 1 of 'cpy' is restrict, but argument 2 points into 'r' too",
    ),
    "two_restricts": (
        "void f(void)\n{\n  char buf[8] = {0};\n  two(buf, buf);\n}\n",
        4,
        "argument 1 of 'two' is restrict, but argument 2 points into 'buf' too",
    ),
}

CLEAN = {
    "two_arrays": "void f(void)\n{\n  char x[8] = {0};\n  char y[8] = {0};\n  cpy(x, y, 8);\n}\n",
    "local_scalar": "void f(const char * view s)\n{\n  char c = 0;\n  cpy(&c, s, 1);\n}\n",
    "owned": "void f(const char * view s)\n{\n  char * owned maybe p = malloc(8);\n  if (!p)\n    return;\n"
    "  cpy(p, s, 8);\n  free(p);\n}\n",
    "two_owned": "void f(void)\n{\n  char * owned maybe p = malloc(8);\n  char * owned maybe q = malloc(8);\n"
    "  if (p && q)\n    cpy(p, q, 8);\n  free(p);\n  free(q);\n}\n",
    "restrict_param_on": "void f(char * restrict view r, const char * view s)\n{\n  cpy(r, s, 1);\n"
    "  cpy(r + 1, s, 1);\n}\n",
    "global_in_unsafe": "void f(const char * view s)\n{\n  cpy(unsafe(gbuf), s, 4);\n}\n",
}


@pytest.mark.parametrize("opt", LEVELS)
@pytest.mark.parametrize("name", sorted(BAD))
def test_reported(tmp_path, name, opt):
    body, line, msg = BAD[name]
    proc = _compile(tmp_path, body, opt)
    assert proc.returncode != 0
    assert (line, "error", msg) in _diags(proc), proc.stderr


@pytest.mark.parametrize("opt", LEVELS)
@pytest.mark.parametrize("name", sorted(CLEAN))
def test_clean(tmp_path, name, opt):
    proc = _compile(tmp_path, CLEAN[name], opt)
    assert proc.returncode == 0 and _diags(proc) == [], proc.stderr


@pytest.mark.parametrize("name", sorted(BAD))
def test_unsafe_lets_it_pass(tmp_path, name):
    body, line, _ = BAD[name]
    lines = body.splitlines()
    lines[line - 1] = "  unsafe {\n" + lines[line - 1] + "\n  }"
    proc = _compile(tmp_path, "\n".join(lines) + "\n")
    assert proc.returncode == 0, proc.stderr


def test_regular_c_drops_restrict(tmp_path):
    src = tmp_path / "t.c"
    src.write_text("void cpy(char * restrict d, const char * restrict s, int n);\n"
                   "void f(char *b) { cpy(b, b, 1); }\n")
    proc = subprocess.run([str(TCC), "-Wno-missing-prototypes", "-Wno-cross-tu", "-Wno-tainted-format", "-ccompat", "-c", str(src), "-o", str(tmp_path / "t.o")], capture_output=True,
                          text=True)
    assert proc.returncode == 0 and proc.stderr == "", proc.stderr


def test_restrict_is_part_of_the_type(tmp_path):
    """A restrict view and a plain view are different pointer types to the
    checks (a restrict view converts to a view freely)."""
    body = (
        "char * restrict view pick(inout char * restrict view a)\n{\n  return a;\n}\n"
        "int g(inout char * restrict view r)\n{\n  char * view v = r;\n  return *v + *pick(r);\n}\n"
    )
    proc = _compile(tmp_path, body)
    assert proc.returncode == 0 and _diags(proc) == [], proc.stderr


@pytest.mark.parametrize("opt", LEVELS[1:])
def test_code_unchanged(tmp_path, opt):
    """restrict changes the checks, not the code (S4 is what uses it)."""
    body = "void f(char * restrict view r, const char * view s)\n{\n  cpy(r, s, 1);\n}\n"
    a = _compile(tmp_path, body, opt)
    oa = (tmp_path / "t.o").read_bytes()
    b = _compile(tmp_path, body.replace("restrict ", ""), opt, "-ccompat", "-std=yasc")
    ob = (tmp_path / "t.o").read_bytes()
    assert a.returncode == 0 and b.returncode == 0, a.stderr + b.stderr
    assert oa == ob


# ---- a restrict local's borrow lasts while it is still in use ----

LOCAL_BAD = {
    "array": (
        "int f(void)\n{\n  int buf[4] = {0};\n  int * restrict view r = buf;\n  buf[1] = 2;\n  return r[0];\n}\n",
        5,
        "'buf' is reached directly while 'r', a restrict borrow of it, is still in use",
    ),
    "struct_member": (
        "struct S { int a[4]; };\nint f(void)\n{\n  struct S s = {{0}};\n  int * restrict view r = s.a;\n"
        "  s.a[1] = 3;\n  return r[1];\n}\n",
        6,
        "'s' is reached directly while 'r', a restrict borrow of it, is still in use",
    ),
    "owned": (
        "int f(void)\n{\n  char * owned maybe p = malloc(4);\n  if (!p)\n    return 0;\n"
        "  char * restrict view r = p;\n  p[0] = 1;\n  int v = r[1];\n  free(p);\n  return v;\n}\n",
        7,
        "'p' is reached directly while 'r', a restrict borrow of it, is still in use",
    ),
}

LOCAL_CLEAN = {
    "after_last_use": "int f(void)\n{\n  int buf[4] = {0};\n  int * restrict view r = buf;\n  r[0] = 1;\n"
    "  return buf[0];\n}\n",
    "through_r_only": "int f(void)\n{\n  int buf[4] = {0};\n  int * restrict view r = buf;\n  r[0] = 1;\n"
    "  r[1] = r[0];\n  return r[1];\n}\n",
    "other_object": "int f(void)\n{\n  int a[4] = {0};\n  int b[4] = {0};\n  int * restrict view r = a;\n"
    "  b[0] = 1;\n  return r[0] + b[0];\n}\n",
    # r borrows what s->buf points to, not s: s->len is another object
    "loaded_member": "struct S { char * owned buf; int len; };\nint f(inout struct S * view s)\n{\n"
    "  char * restrict view r = s->buf;\n  r[0] = 1;\n  s->len = 3;\n  return r[1];\n}\n",
}


@pytest.mark.parametrize("opt", LEVELS)
@pytest.mark.parametrize("name", sorted(LOCAL_BAD))
def test_local_borrow_reported(tmp_path, name, opt):
    body, line, msg = LOCAL_BAD[name]
    proc = _compile(tmp_path, body, opt)
    assert proc.returncode != 0
    assert (line, "error", msg) in _diags(proc), proc.stderr


@pytest.mark.parametrize("opt", LEVELS)
@pytest.mark.parametrize("name", sorted(LOCAL_CLEAN))
def test_local_borrow_clean(tmp_path, name, opt):
    proc = _compile(tmp_path, LOCAL_CLEAN[name], opt)
    assert proc.returncode == 0 and _diags(proc) == [], proc.stderr
