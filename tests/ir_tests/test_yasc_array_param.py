"""yasc: a parameter declared as an array is a pointer, and that pointer is a view.

`char * view args[]` writes one '*': the array the parameter becomes a
pointer to is not written as a pointer, so it says nothing -- it is a `view`
(not maybe), exactly `char * view * view args`.  As in C99's `args[const]`,
the brackets of the parameter's outermost array qualify that pointer:
`[owned]`, `[maybe]`, `[view]`, `[restrict]`.  A parameter array declared in
regular C is unchecked, like any pointer written there.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

PRELUDE = """\
#pragma yasc
void free(drop void * owned maybe p);
char * view * view g;
char * view * view maybe gm;
"""
PRELUDE_LINES = PRELUDE.count("\n")
LEVELS = ["-O0", "-O1", "-O2", "-Os"]


def _compile(tmp_path, body, *flags, header=None):
    if header is not None:
        (tmp_path / "h.h").write_text(header)
    src = tmp_path / "t.c"
    src.write_text(PRELUDE + body)
    return subprocess.run(
        [str(TCC), "-ccompat", "-Wlifetime", f"-I{tmp_path}", "-c", *flags, str(src), "-o", str(tmp_path / "t.o")],
        capture_output=True,
        text=True,
    )


def _diags(tmp_path, body, *flags, header=None):
    proc = _compile(tmp_path, body, *flags, header=header)
    assert proc.returncode == 0, proc.stderr
    out = []
    for line in proc.stderr.splitlines():
        parts = line.split(": ", 2)
        if len(parts) == 3 and parts[1] in ("warning", "error", "note"):
            out.append((int(parts[0].rsplit(":", 1)[1]) - PRELUDE_LINES, parts[1], parts[2]))
    return out


def _error(tmp_path, body):
    proc = _compile(tmp_path, body)
    assert proc.returncode != 0, "expected a compile error"
    return [line.split("error: ", 1)[1] for line in proc.stderr.splitlines() if "error: " in line][0]


ESCAPE_GLOBAL = "'a' is a view: it cannot leave the function (it is stored in global 'g')"
MAYBE_INTO = "a pointer that may be null is stored in 'q', which is not maybe"
LEAKS = "'a' leaks: it still owns memory at the end of the function"

# body (one line after the prelude) -> the diagnostics it gives, and the same
# body with `char * view * view a` must give exactly those too
CASES = {
    "escapes_to_global": ("void f(inout char * view a[])\n{\n  g = a;\n}\n", [(3, "warning", ESCAPE_GLOBAL)]),
    "escapes_sized": ("void f(inout char * view a[4])\n{\n  g = a + 1;\n}\n", [(3, "warning", ESCAPE_GLOBAL)]),
    "result_elided": ("char * view * view f(inout char * view a[])\n{\n  return a + 1;\n}\n", []),
    "result_borrows": ("char * view * borrows(a) f(inout char * view a[], int n)\n{\n  return a + n;\n}\n", []),
    "not_maybe": ("void f(inout char * view a[])\n{\n  char * view * view q = a;\n  (void)q;\n}\n", []),
    "not_owned": ("void f(inout char * view a[])\n{\n  free(a);\n}\n", None),
    "read": ("char f(char * view a[], int i)\n{\n  return a[i][0];\n}\n", []),
}


@pytest.mark.parametrize("name", sorted(CASES))
@pytest.mark.parametrize("opt", LEVELS)
def test_array_param_is_a_view(tmp_path, name, opt):
    body, want = CASES[name]
    got = _diags(tmp_path, body, opt)
    if want is not None:
        assert got == want
    as_pointer = body.replace("char * view a[4]", "char * view * view a").replace("char * view a[]", "char * view * view a")
    assert _diags(tmp_path, as_pointer, opt) == got


def test_typedef_array(tmp_path):
    body = "typedef char * view vec_t[4];\nvoid f(inout vec_t a)\n{\n  g = a;\n}\n"
    assert _diags(tmp_path, body) == [(4, "warning", ESCAPE_GLOBAL)]


def test_abstract_and_prototype(tmp_path):
    body = (
        "void p(char * view []);\n"
        "void f(char * view a[]);\n"
        "void f(char * view * view a)\n{\n  p(a);\n}\n"
    )
    assert _diags(tmp_path, body) == []


BRACKETS = {
    "maybe": ("void f(inout char * view a[maybe])\n{\n  char * view * view q = a;\n  (void)q;\n}\n", [(3, "warning", MAYBE_INTO)]),
    "maybe_tested": (
        "void f(inout char * view a[maybe])\n{\n  if (a)\n  {\n    char * view * view q = a;\n    (void)q;\n  }\n}\n",
        [],
    ),
    "maybe_into_maybe": ("void f(char * view a[maybe])\n{\n  gm = 0;\n  (void)a;\n}\n", []),
    "view": ("void f(inout char * view a[view])\n{\n  g = a;\n}\n", [(3, "warning", ESCAPE_GLOBAL)]),
    "view_maybe": ("void f(inout char * view a[view maybe])\n{\n  char * view * view q = a;\n  (void)q;\n}\n", [(3, "warning", MAYBE_INTO)]),
    "owned_leaks": ("void f(char * view a[owned])\n{\n  (void)a;\n}\n", [(4, "warning", LEAKS)]),
    "owned_freed": ("void f(char * view a[owned])\n{\n  free(a);\n}\n", []),
    "owned_sized": ("void f(int a[owned 4])\n{\n  free(a);\n}\n", []),
    "with_const_static": ("void f(inout char * view a[const static 2])\n{\n  g = a;\n}\n", [(3, "warning", ESCAPE_GLOBAL)]),
    "restrict": ("int f(int a[restrict], int b[restrict])\n{\n  return a[0] + b[0];\n}\n", []),
}


@pytest.mark.parametrize("name", sorted(BRACKETS))
@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_bracket_qualifiers(tmp_path, name, opt):
    body, want = BRACKETS[name]
    assert _diags(tmp_path, body, opt) == want


BRACKET_ERRORS = {
    "owned_and_view": ("void f(int a[owned view]);", "a pointer is owned, a view or borrows(...): one of them"),
    "drop": ("void f(int a[drop]);", "'drop' in [ ]: only owned, view and maybe qualify the pointer a parameter array is"),
    "out": ("void f(int a[out]);", "'out' in [ ]: only owned, view and maybe qualify the pointer a parameter array is"),
    "dealloc": ("void f(int a[owned(fclose)]);", "owned(...) in [ ]: write the parameter as a pointer"),
    "inner_dimension": ("void f(int a[3][view]);", "'view' in [ ] qualifies a parameter's outermost array only"),
    "array_pointed_to": ("void f(int (* view a)[maybe]);", "'maybe' in [ ] qualifies a parameter's outermost array only"),
}


@pytest.mark.parametrize("name", sorted(BRACKET_ERRORS))
def test_bracket_errors(tmp_path, name):
    decl, msg = BRACKET_ERRORS[name]
    assert _error(tmp_path, decl + "\n") == msg


def test_regular_c_array_param_is_unchecked(tmp_path):
    # a maybe value into a parameter array declared in regular C: unchecked,
    # not a non-maybe view
    header = "void legacy(char *a[]);\n"
    body = '#include "h.h"\nvoid f(inout char * view * view maybe m)\n{\n  legacy(m);\n}\n'
    assert _diags(tmp_path, body, header=header) == []
    # and in yasc a non-maybe view parameter does not take one
    body = "void yasc_fn(char * view a[]);\nvoid f(char * view * view maybe m)\n{\n  yasc_fn(m);\n}\n"
    assert _diags(tmp_path, body) == [
        (4, "warning", "a pointer that may be null is passed as argument 1 of 'yasc_fn', which is not maybe")
    ]


def test_regular_c_unit_unchanged(tmp_path):
    src = tmp_path / "c.c"
    src.write_text("int view;\nint f(int a[view], char *b[]) { return a[0] + b[0][0]; }\n")
    proc = subprocess.run(
        [str(TCC), "-ccompat", "-Wall", "-c", str(src), "-o", str(tmp_path / "c.o")], capture_output=True, text=True
    )
    assert proc.returncode == 0 and proc.stderr == "", proc.stderr


def test_sizeof_array_argument_still_warns(tmp_path):
    body = "int f(char * view a[])\n{\n  return sizeof(a);\n}\n"
    assert any("sizeof" in d[2] for d in _diags(tmp_path, body, "-Wall"))
