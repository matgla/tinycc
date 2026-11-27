"""yasc: nested pointers are views by default -- a local's and a member's
array elements and every inner level, as a global's are
(test_yasc_global_view_default.py).

`const char *dirs[2] = {...}` in a function is `const char *view dirs[2]`,
`char **pp` is `char *view *pp` (its own pointer still waits for the body,
which may own it: test_plain_pointer*), and a struct member `char *names[4]`
is `char *view names[4]`.  Never null: `*?` says an element may be.  `view`
written on an element anyway is -Wredundant-view.

Never null is checked where a value is stored: in the initializer, and by
`a[i] = v` / `*p = v` (the lifetime check, lifetime.c) -- for an element of
any array of such pointers (local, global, member) and through any pointer
to one.  A value tested first is not null.  A typedef's pointer, a function
pointer and a `(type)` keep their own rules.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

LEVELS = ["-O0", "-O2"]
PLAIN = "plain pointer in yasc: say owned or view"
REDUNDANT = "an element's pointer is a view unless it says owned: drop 'view'"

PRELUDE = """\
#pragma yasc
char *?borrows(static) getenv(char *name);
int puts(char *s);
"""
PRELUDE_LINES = PRELUDE.count("\n")


def _run(tmp_path, source, *flags):
    src = tmp_path / "t.c"
    src.write_text(PRELUDE + source)
    return subprocess.run(
        [str(TCC), "-Wno-missing-prototypes", "-Wno-unused-variable", "-fmax-errors=50", "-c", *flags, str(src),
         "-o", str(tmp_path / "t.o")],
        capture_output=True, text=True, cwd=tmp_path,
    )


def _diags(proc):
    """(line in the body, kind, message) of every diagnostic"""
    out = []
    for line in proc.stderr.splitlines():
        parts = line.split(":", 3)
        if len(parts) == 4 and parts[1].isdigit() and parts[2].strip() in ("error", "warning", "note"):
            out.append((int(parts[1]) - PRELUDE_LINES, parts[2].strip(), parts[3].strip()))
    return out


CLEAN = {
    "local_array": "int f(void)\n{\n  const char *dirs[2] = {\"a\", \"b\"};\n  return puts(dirs[1]);\n}\n",
    "local_array_maybe": "int f(void)\n{\n  const char *?dirs[2] = {getenv(\"TZDIR\"), \"/usr\"};\n"
                         "  for (int i = 0; i < 2; i++)\n    if (dirs[i])\n      return puts(dirs[i]);\n"
                         "  return 0;\n}\n",
    "inner_level": "int f(void)\n{\n  const char *list[2] = {\"a\", \"b\"};\n  const char **pp = list;\n"
                   "  return puts(pp[1]);\n}\n",
    "two_inner_levels": "int f(void)\n{\n  const char *list[1] = {\"a\"};\n  const char **pp = list;\n  const char ***ppp = &pp;\n"
                        "  return puts(**ppp);\n}\n",
    "member_array": "struct s { const char *names[2]; };\nint f(struct s *p) { return puts(p->names[0]); }\n",
    "tested_then_stored": "int f(void)\n{\n  const char *c[2] = {\"a\", \"b\"};\n  const char *?q = getenv(\"Q\");\n"
                          "  if (q)\n    c[0] = q;\n  return puts(c[0]);\n}\n",
    "maybe_element_takes_null": "int f(void)\n{\n  const char *?c[2] = {\"a\", \"b\"};\n  c[1] = nullptr;\n"
                                "  return c[1] ? 1 : 0;\n}\n",
    "function_pointers": "int g(int x) { return x; }\nint f(void)\n{\n  int (*fns[2])(int) = {g, g};\n"
                         "  return fns[1](1);\n}\n",
    "owned_elements_said": "void free(drop void *?owned p);\nchar *?owned dup(char *s);\n"
                           "void f(void)\n{\n  char *?owned v[1] = {dup(\"x\")};\n  free(move(v[0]));\n}\n",
}


@pytest.mark.parametrize("level", LEVELS)
@pytest.mark.parametrize("name", sorted(CLEAN))
def test_clean(tmp_path, name, level):
    proc = _run(tmp_path, CLEAN[name], level)
    assert proc.returncode == 0, proc.stderr
    assert not [d for d in _diags(proc) if d[1] != "note"], proc.stderr


NULL_STORE = "int g(int i);\nconst char *gtab[2] = {\"a\", \"b\"};\nstruct s { const char *arr[2]; };\n" \
    "int f(inout struct s *p, int i)\n{\n" \
    "  const char *c[2] = {\"a\", \"b\"};\n" \
    "  c[1] = nullptr;\n" \
    "  c[i] = getenv(\"Q\");\n" \
    "  const char **pp = c;\n" \
    "  *pp = nullptr;\n" \
    "  pp[1] = nullptr;\n" \
    "  p->arr[i] = nullptr;\n" \
    "  gtab[i] = nullptr;\n" \
    "  return puts(c[0]);\n}\n"


@pytest.mark.parametrize("level", LEVELS)
def test_null_stores(tmp_path, level):
    proc = _run(tmp_path, NULL_STORE, level)
    errs = [(n, m) for n, k, m in _diags(proc) if k == "error"]
    assert errs == [
        (7, "a null pointer is stored in an element of 'c', which is not '?'"),
        (8, "a pointer that may be null is stored in an element of 'c', which is not '?'"),
        (10, "a null pointer is stored where 'pp' points, which is not '?'"),
        (11, "a null pointer is stored in an element of 'pp', which is not '?'"),
        (12, "a null pointer is stored in an element of 'arr', which is not '?'"),
        (13, "a null pointer is stored in an element of 'gtab', which is not '?'"),
    ], proc.stderr
    notes = [m for _, k, m in _diags(proc) if k == "note"]
    assert "an element's pointer is never null unless it says '?': write '*?' in the element type" in notes
    assert any(m.startswith("a pointer's target pointer is never null") for m in notes), proc.stderr


def test_initializer_maybe(tmp_path):
    """the localtime.c case: getenv() may be null"""
    proc = _run(tmp_path, "int f(void)\n{\n  const char *dirs[2] = {getenv(\"TZDIR\"), \"/usr\"};\n"
                          "  return puts(dirs[1]);\n}\n")
    assert (3, "error", "a pointer that may be null initializes an element that is not '?'") in _diags(proc), \
        proc.stderr
    assert not [m for _, _, m in _diags(proc) if PLAIN in m], proc.stderr


def test_no_plain_errors(tmp_path):
    """arrays of pointers and inner levels no longer ask for owned or view"""
    proc = _run(tmp_path, "struct s { const char *names[2]; const char **list; };\n"
                          "int f(void)\n{\n  const char *a[2] = {\"a\", \"b\"};\n  const char **pp = a;\n"
                          "  const char *b[2][2] = {{\"a\", \"b\"}, {\"c\", \"d\"}};\n  return puts(pp[0]) + puts(b[1][1]);\n}\n")
    assert proc.returncode == 0, proc.stderr
    assert not [m for _, _, m in _diags(proc) if PLAIN in m], proc.stderr


def test_redundant_view(tmp_path):
    proc = _run(tmp_path, "int f(void)\n{\n  const char *view a[2] = {\"a\", \"b\"};\n  const char *?view b[1] = {nullptr};\n"
                          "  return puts(a[0]) + (b[0] ? 1 : 0);\n}\n", "-Wredundant-view")
    warns = [(n, m) for n, k, m in _diags(proc) if k == "warning"]
    assert warns == [(3, REDUNDANT), (4, REDUNDANT)], proc.stderr


def test_typedef_still_said(tmp_path):
    proc = _run(tmp_path, "int f(void)\n{\n  typedef const char *str;\n  return 0;\n}\n")
    assert [m for _, _, m in _diags(proc) if PLAIN in m], proc.stderr
