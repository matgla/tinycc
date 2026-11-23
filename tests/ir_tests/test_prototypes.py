"""-Wstrict-prototypes and -Wmissing-prototypes, and `()` in C23 and yasc.

-Wstrict-prototypes: a function declarator that does not give the parameter
types -- `int f()` before C23, anywhere a function type is written
(declarations, typedefs, function pointers, casts), and an old-style (K&R)
definition; a definition is not reported when a prototype came before it.
The safe default has it (not -Wall, as gcc): C23 reads `()` as `(void)`, so
there only an old-style definition is left.

-Wmissing-prototypes: a global function defined with no prototype before it
(not main, not static, not inline, not a yasc module's export) -- how a
header and its source drift apart.  Neither -Wall nor the safe default: a
single-file program and every test file defines functions it never declares
(815 reports over our C, most of them in test files).

C23 (-std=c23, the safe default) and yasc files read `()` as `(void)`:
calling `int f();` with arguments is an error there; C17 and before keep the
old-style meaning.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

LEVELS = ["-O0", "-O1", "-O2", "-Os"]
FLAGS = ["-Wstrict-prototypes", "-Wmissing-prototypes"]


def _run(tmp_path, body, *flags, compat=True, name="t.c", out="t.o"):
    src = tmp_path / name
    src.write_text(body)
    cmd = [str(TCC)] + (["-ccompat"] if compat else []) + ["-c", *flags, str(src), "-o", str(tmp_path / out)]
    return subprocess.run(cmd, capture_output=True, text=True)


def _diags(proc, kind="warning"):
    out = []
    for line in proc.stderr.splitlines():
        parts = line.split(": ", 2)
        if len(parts) == 3 and parts[1] == kind:
            out.append((int(parts[0].rsplit(":", 1)[1]), parts[2]))
    return out


NOPROTO = "function declaration isn't a prototype"

BODY = """\
int ext(int);
int ext(int a) { return a; }
int noproto(int a) { return a + 1; }
static int st(int a) { return a; }
int old();
int old() { return 1; }
int knr(a) int a; { return a + st(a); }
int pk(int);
int pk(a) int a; { return a; }
void cb(int (*f)());
typedef int fn();
inline int il(int a) { return a; }
int main(void) { return old() + ((int (*)())0 != 0); }
"""

EXPECTED = [
    (3, "no previous prototype for 'noproto'"),
    (5, NOPROTO),
    (6, NOPROTO),
    (6, "no previous prototype for 'old'"),
    (7, NOPROTO),
    (7, "no previous prototype for 'knr'"),
    (10, NOPROTO),
    (11, NOPROTO),
    (13, NOPROTO),
]


@pytest.mark.parametrize("opt", LEVELS)
def test_hits_at_every_level(tmp_path, opt):
    proc = _run(tmp_path, BODY, *FLAGS, opt)
    assert proc.returncode == 0, proc.stderr
    assert _diags(proc) == EXPECTED, proc.stderr


def test_c23_reads_empty_parens_as_void(tmp_path):
    proc = _run(tmp_path, BODY, *FLAGS, "-std=c23")
    assert _diags(proc) == [
        (3, "no previous prototype for 'noproto'"),
        (7, NOPROTO),
        (7, "no previous prototype for 'knr'"),
    ], proc.stderr


def test_c23_call_with_arguments_is_an_error(tmp_path):
    body = "int f();\nint g(void) { return f(1); }\n"
    proc = _run(tmp_path, body, "-std=c23")
    assert proc.returncode != 0 and "too many arguments" in proc.stderr, proc.stderr
    assert _run(tmp_path, body, "-std=c17").returncode == 0
    assert _run(tmp_path, body).returncode == 0  # -ccompat: gnu C before C23


def test_c23_definition_against_old_declaration(tmp_path):
    # f() is f(void) in C23: a definition with a parameter conflicts
    body = "int f();\nint f(int a) { return a; }\n"
    assert _run(tmp_path, body, "-std=c23").returncode != 0
    assert _run(tmp_path, body, "-std=c17").returncode == 0


def test_yasc_file_reads_empty_parens_as_void(tmp_path):
    body = "#pragma yasc\nint f();\nint g(void) { return f(1); }\n"
    proc = _run(tmp_path, body, "-std=c17")
    assert proc.returncode != 0 and "too many arguments" in proc.stderr, proc.stderr


def test_switches(tmp_path):
    assert _diags(_run(tmp_path, BODY)) == []
    assert _diags(_run(tmp_path, BODY, "-Wall")) == []
    assert _diags(_run(tmp_path, BODY, *FLAGS, "-Wno-strict-prototypes", "-Wno-missing-prototypes")) == []
    only_missing = _diags(_run(tmp_path, BODY, "-Wmissing-prototypes"))
    assert only_missing and all("no previous prototype" in d[1] for d in only_missing)
    only_strict = _diags(_run(tmp_path, BODY, "-Wstrict-prototypes"))
    assert only_strict and all(d[1] == NOPROTO for d in only_strict)


def test_werror(tmp_path):
    proc = _run(tmp_path, BODY, *FLAGS, "-Werror=missing-prototypes")
    assert proc.returncode != 0 and "error: no previous prototype for 'noproto'" in proc.stderr, proc.stderr
    proc = _run(tmp_path, BODY, *FLAGS, "-Werror", "-Wno-error=missing-prototypes", "-Wno-error=strict-prototypes")
    assert proc.returncode == 0 and len(_diags(proc)) == len(EXPECTED), proc.stderr


def test_pragma_ignored(tmp_path):
    body = (
        "int a(int x) { return x; }\n"
        "#pragma GCC diagnostic push\n"
        '#pragma GCC diagnostic ignored "-Wmissing-prototypes"\n'
        "int b(int x) { return x; }\n"
        "#pragma GCC diagnostic pop\n"
        "int c(int x) { return x; }\n"
    )
    for opt in ("-O0", "-O2"):
        assert [d[0] for d in _diags(_run(tmp_path, body, "-Wmissing-prototypes", opt))] == [1, 6]
    # a pragma turns a check on that the command line left off
    body2 = '#pragma GCC diagnostic warning "-Wmissing-prototypes"\nint d(int x) { return x; }\n'
    assert _diags(_run(tmp_path, body2)) == [(2, "no previous prototype for 'd'")]


def test_not_in_a_system_header(tmp_path):
    inc = tmp_path / "sys"
    inc.mkdir()
    (inc / "s.h").write_text("int old();\nint hdef(int a) { return a; }\n")
    proc = _run(tmp_path, "#include <s.h>\nint x;\n", *FLAGS, "-isystem", str(inc))
    assert _diags(proc) == [], proc.stderr
    (tmp_path / "u.h").write_text("int old();\n")
    proc = _run(tmp_path, '#include "u.h"\nint x;\n', *FLAGS)
    assert _diags(proc) == [(1, NOPROTO)], proc.stderr


def test_safe_default(tmp_path):
    # -Wstrict-prototypes is on, `()` is a prototype, -Wmissing-prototypes off
    body = "int f();\nint f() { return 0; }\nint knr(a) int a; { return a; }\nint g(int a) { return a; }\n"
    proc = _run(tmp_path, body, compat=False)
    assert proc.returncode == 0, proc.stderr
    assert _diags(proc) == [(3, NOPROTO)], proc.stderr


def test_module_export_is_its_declaration(tmp_path):
    body = "module m;\nexport int get(void) { return 1; }\nint helper(void) { return 2; }\n"
    proc = _run(tmp_path, body, "-Wmissing-prototypes", "-Wno-unused-function", compat=False, name="m.c")
    assert proc.returncode == 0, proc.stderr
    assert _diags(proc) == [], proc.stderr


def test_once_in_an_inlined_function(tmp_path):
    body = (
        "static inline int f(int x) { int (*fp)() = 0; return x + (fp != 0); }\n"
        "int g(int a);\n"
        "int g(int a) { return f(a) + f(a + 1); }\n"
    )
    for opt in LEVELS:
        assert _diags(_run(tmp_path, body, *FLAGS, opt)) == [(1, NOPROTO)]


@pytest.mark.parametrize("opt", LEVELS)
def test_code_unchanged(tmp_path, opt):
    a = _run(tmp_path, BODY, opt, out="a.o")
    b = _run(tmp_path, BODY, opt, *FLAGS, out="b.o")
    assert a.returncode == 0 and b.returncode == 0
    assert (tmp_path / "a.o").read_bytes() == (tmp_path / "b.o").read_bytes()
