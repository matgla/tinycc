"""yasc `unused`: a parameter the body leaves alone on purpose.

`int f(unused ctx *c, int arg)` is `__attribute__((unused))` spelled as a
parameter word: written where a parameter's declaration starts (before or
after in / inout / out), it silences -Wunused-parameter and, since such a
parameter's signature is someone else's (a callback), -Wparam-access's
"drop inout" / "say out".  An `out` parameter must still be written: the
caller relies on it.  Anywhere else `unused` is a name (`int unused`), a
typedef named `unused` is a type, and regular C knows no such word.  The C
headers (-yasc-c-header, -fmodule-header) drop it.

The statement `unused(a, b);` says the same in the body, of parameters or
locals: no code, and only names of the function's own (not a global, not an
expression).  A function named `unused` in scope is called instead.  An
exported inline body has no C form for it.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

LEVELS = ["-O0", "-O2"]

PRELUDE = "#pragma yasc\nstruct S { int a; };\n"
PRELUDE_LINES = PRELUDE.count("\n")


def _run(tmp_path, body, *flags, prelude=PRELUDE):
    src = tmp_path / "t.c"
    src.write_text(prelude + body)
    return subprocess.run(
        [str(TCC), "-Wall", "-Wextra", "-Wno-missing-prototypes", "-c", *flags, str(src), "-o", str(tmp_path / "t.o")],
        capture_output=True, text=True, cwd=tmp_path,
    )


def _diags(proc):
    out = []
    for line in proc.stderr.splitlines():
        parts = line.split(": ", 2)
        if len(parts) == 3 and parts[1] in ("warning", "error") and parts[0].rsplit(":", 1)[0].endswith("t.c"):
            out.append((int(parts[0].rsplit(":", 1)[1]) - PRELUDE_LINES, parts[1], parts[2]))
    return out


QUIET = {
    "scalar": "int f(unused int x) { return 0; }\n",
    "pointer": "int f(unused struct S *s, int k) { return k; }\n",
    "inout_empty_body": "void f(unused inout struct S *s) { }\n",
    "inout_read_only": "int f(unused inout int *p) { return *p; }\n",
    "inout_written_first": "void f(unused inout int *p) { *p = 1; }\n",
    "after_inout": "void f(inout unused struct S *s) { }\n",
    "with_in": "int f(in unused struct S *s) { return 0; }\n",
    "prototype_then_definition": "int f(int x);\nint f(unused int x) { return 0; }\n",
    "callback": ("typedef int (*cb)(inout struct S *s, int arg);\n"
                 "int f(unused inout struct S *s, unused int arg) { return 0; }\n"
                 "cb table[] = { f };\n"),
    "attribute_on_inout": "void f(__attribute__((unused)) inout struct S *s) { }\n",
    "named_unused": "int f(int unused) { return unused; }\n",
}


@pytest.mark.parametrize("level", LEVELS)
@pytest.mark.parametrize("name", sorted(QUIET))
def test_quiet(tmp_path, level, name):
    proc = _run(tmp_path, QUIET[name], level)
    assert proc.returncode == 0, proc.stderr
    assert _diags(proc) == [], proc.stderr


@pytest.mark.parametrize("level", LEVELS)
def test_only_the_marked_parameter(tmp_path, level):
    proc = _run(tmp_path, "int f(int a, unused int b, int c) { return 0; }\n", level)
    assert proc.returncode == 0, proc.stderr
    assert sorted(_diags(proc)) == [(1, "warning", "unused parameter 'a'"), (1, "warning", "unused parameter 'c'")]


@pytest.mark.parametrize("level", LEVELS)
def test_unmarked_inout_still_warns(tmp_path, level):
    proc = _run(tmp_path, "void f(inout struct S *s) { (void)s; }\n", level)
    assert (1, "warning", "'s' is inout, but 'f' writes nothing through it: drop inout") in _diags(proc), proc.stderr


@pytest.mark.parametrize("level", LEVELS)
def test_unused_out_must_still_be_written(tmp_path, level):
    proc = _run(tmp_path, "void f(unused out int *r) { }\n", level)
    assert proc.returncode != 0
    assert any(k == "error" and "'*r' is not written" in m for _, k, m in _diags(proc)), proc.stderr


def test_prototype_word_is_not_the_definitions(tmp_path):
    """like the attribute: the definition says what its body does"""
    proc = _run(tmp_path, "int f(unused int x);\nint f(int x) { return 0; }\n")
    assert _diags(proc) == [(2, "warning", "unused parameter 'x'")], proc.stderr


def test_typedef_named_unused_is_a_type(tmp_path):
    proc = _run(tmp_path, "int f(unused x) { return x; }\n", prelude="typedef int unused;\n#pragma yasc\n")
    assert proc.returncode == 0, proc.stderr
    assert _diags(proc) == [], proc.stderr


def test_regular_c_has_no_unused_word(tmp_path):
    proc = _run(tmp_path, "int f(unused int x) { return x; }\n", "-ccompat", prelude="")
    assert proc.returncode != 0


def test_module_header_drops_it(tmp_path):
    (tmp_path / "m.c").write_text(
        "#pragma yasc\nmodule cbs;\n"
        "export int on_event(unused inout int *q, unused int code);\n"
        "int on_event(unused inout int *q, unused int code) { return 0; }\n"
    )
    proc = subprocess.run(
        [str(TCC), "-ccompat", "-c", str(tmp_path / "m.c"), "-o", str(tmp_path / "m.o"),
         f"-fmodule-header={tmp_path / 'inc'}"],
        capture_output=True, text=True, cwd=tmp_path,
    )
    assert proc.returncode == 0 and not proc.stderr, proc.stderr
    text = (tmp_path / "inc" / "cbs.h").read_text()
    assert "int on_event(int * q, int code);" in text, text
    assert "unused" not in text, text


def test_c_header_drops_it(tmp_path):
    inc = tmp_path / "inc"
    (inc / "yasc").mkdir(parents=True)
    (inc / "yasc" / "cb.h").write_text(
        "#pragma yasc\n#pragma once\n"
        "int on_event(unused inout int *q, unused int code);\n"
        "int on_key(inout unused int *q, int unused);\n"
    )
    proc = subprocess.run([str(TCC), "-yasc-c-header", "yasc/cb.h"], cwd=inc, capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr
    assert "int on_event(int *q, int code);" in proc.stdout, proc.stdout
    assert "int on_key(int *q, int unused);" in proc.stdout, proc.stdout


STMT_QUIET = {
    "params": "int f(char *user, unsigned group) { unused(user, group); return -1; }\n",
    "one_at_a_time": "int f(int a, int b) { unused(a); unused(b); return 0; }\n",
    "inout": "void f(inout struct S *s) { unused(s); }\n",
    "inout_and_scalar": "void f(inout struct S *s, int k) { unused(s, k); }\n",
    "local": "int f(void) { int x = 1; unused(x); return 0; }\n",
    "later_use_compiles": "int f(int a) { unused(a); return a; }\n",
    "function_named_unused": "int unused(int x);\nint f(int a) { return unused(a); }\n",
}


@pytest.mark.parametrize("level", LEVELS)
@pytest.mark.parametrize("name", sorted(STMT_QUIET))
def test_statement_quiet(tmp_path, level, name):
    proc = _run(tmp_path, STMT_QUIET[name], level)
    assert proc.returncode == 0, proc.stderr
    assert _diags(proc) == [], proc.stderr


@pytest.mark.parametrize("level", LEVELS)
def test_statement_only_the_named(tmp_path, level):
    proc = _run(tmp_path, "int f(int a, int b, int c) { unused(b); return 0; }\n", level)
    assert sorted(_diags(proc)) == [(1, "warning", "unused parameter 'a'"), (1, "warning", "unused parameter 'c'")]


def test_statement_inner_local_is_not_the_parameter(tmp_path):
    proc = _run(tmp_path, "int f(int a) { { int a = 2; unused(a); } return 0; }\n", "-Wno-shadow")
    assert _diags(proc) == [(1, "warning", "unused parameter 'a'")], proc.stderr


@pytest.mark.parametrize("body", ["int g;\nint f(int a) { unused(a, g); return 0; }\n",
                                  "int f(int a) { unused(f); return 0; }\n"])
def test_statement_names_only_own_variables(tmp_path, body):
    proc = _run(tmp_path, body)
    assert proc.returncode != 0
    assert any("unused(...) names the function's parameters or locals" in m for _, _, m in _diags(proc)), proc.stderr


def test_statement_takes_names_not_expressions(tmp_path):
    proc = _run(tmp_path, "int f(int a) { unused(a + 1); return 0; }\n")
    assert proc.returncode != 0


def test_statement_regular_c_is_a_call(tmp_path):
    proc = _run(tmp_path, "int f(int a) { unused(a); return 0; }\n", "-ccompat", prelude="")
    assert "implicit declaration of function 'unused'" in proc.stderr, proc.stderr


def test_statement_in_exported_inline_has_no_c_form(tmp_path):
    (tmp_path / "m.c").write_text(
        "#pragma yasc\nmodule cbs;\n"
        "export static inline int on_event(int code) { unused(code); return 0; }\n"
    )
    proc = subprocess.run(
        [str(TCC), "-ccompat", "-c", str(tmp_path / "m.c"), "-o", str(tmp_path / "m.o"),
         f"-fmodule-header={tmp_path / 'inc'}"],
        capture_output=True, text=True, cwd=tmp_path,
    )
    assert proc.returncode != 0
    assert "which C does not have" in proc.stderr, proc.stderr


def test_statement_in_c_header_inline_has_no_c_form(tmp_path):
    inc = tmp_path / "inc"
    (inc / "yasc").mkdir(parents=True)
    (inc / "yasc" / "cb.h").write_text(
        "#pragma yasc\n#pragma once\n"
        "static inline int on_event(int code) { unused(code); return 0; }\n"
    )
    proc = subprocess.run([str(TCC), "-yasc-c-header", "yasc/cb.h"], cwd=inc, capture_output=True, text=True)
    assert proc.returncode != 0
    assert "unused" in proc.stderr, proc.stderr
