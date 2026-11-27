"""-Wreturn-local-addr (in -Wall): returning an address in this call's frame.

`return &x;`, `return buf;`, `return &s.m;`, `return &a[2];`, the address of
a parameter, a compound literal: all gone once the function returns.  Read
from the raw IR (the frontend has already put &x into a temp), so the same
at every -O level.  In yasc, also through a call whose result borrows from
the argument the address went to (`return f(buf);`).
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

LEVELS = ["-O0", "-O1", "-O2", "-Os"]
PRELUDE = "struct S { int a, b; };\nint G;\n"

BAD = {
    "scalar": "int *f(void)\n{\n  int x = 1;\n  return &x;\n}\n",
    "array": "char *f(void)\n{\n  char buf[8];\n  buf[0] = 0;\n  return buf;\n}\n",
    "member": "int *f(void)\n{\n  struct S s;\n  s.b = 1;\n  return &s.b;\n}\n",
    "parameter": "int *f(int p)\n{\n  return &p;\n}\n",
    "compound_literal": "int *f(void)\n{\n  return (int[]){1, 2};\n}\n",
    "element": "int *f(void)\n{\n  int a[4];\n  return &a[2];\n}\n",
}

CLEAN = {
    "parameter_value": "int *f(int *p)\n{\n  return p;\n}\n",
    "static": "int *f(void)\n{\n  static int s;\n  return &s;\n}\n",
    "global": "int *f(void)\n{\n  return &G;\n}\n",
    "string": "const char *f(void)\n{\n  return \"x\";\n}\n",
    "into_parameter": "char *f(char *buf, int i)\n{\n  return &buf[i];\n}\n",
    "null": "int *f(void)\n{\n  int *q = 0;\n  return q;\n}\n",
}


def _run(tmp_path, body, *flags, obj="t.o"):
    src = tmp_path / "t.c"
    src.write_text(PRELUDE + body)
    return subprocess.run(
        [str(TCC), "-Wno-missing-prototypes", "-Wno-write-strings", "-ccompat", "-c", *flags, str(src), "-o", str(tmp_path / obj)], capture_output=True, text=True
    )


def _warnings(proc):
    out = []
    for line in proc.stderr.splitlines():
        parts = line.split(": ", 2)
        if len(parts) == 3 and parts[1] in ("warning", "error"):
            out.append((int(parts[0].rsplit(":", 1)[1]) - PRELUDE.count("\n"), parts[2]))
    return out


@pytest.mark.parametrize("name", sorted(BAD))
@pytest.mark.parametrize("opt", LEVELS)
def test_reported(tmp_path, name, opt):
    body = BAD[name]
    line = body.count("\n") - 1  # the return is the last line before '}'
    assert _warnings(_run(tmp_path, body, "-Wall", opt)) == [(line, "'f' returns the address of a local variable")]


@pytest.mark.parametrize("name", sorted(CLEAN))
@pytest.mark.parametrize("opt", LEVELS)
def test_clean(tmp_path, name, opt):
    proc = _run(tmp_path, CLEAN[name], "-Wall", opt)
    assert proc.returncode == 0 and not proc.stderr, proc.stderr


def test_not_without_wall(tmp_path):
    proc = _run(tmp_path, BAD["scalar"])
    assert proc.returncode == 0 and not proc.stderr, proc.stderr


def test_wno(tmp_path):
    proc = _run(tmp_path, BAD["scalar"], "-Wall", "-Wno-return-local-addr")
    assert proc.returncode == 0 and not proc.stderr, proc.stderr


def test_werror(tmp_path):
    proc = _run(tmp_path, BAD["scalar"], "-Werror=return-local-addr")
    assert proc.returncode != 0 and "error: 'f' returns the address" in proc.stderr


@pytest.mark.parametrize("opt", LEVELS[1:])
def test_code_unchanged(tmp_path, opt):
    corpus = "".join(body.replace("f(", f"f_{name}(", 1) for name, body in BAD.items())
    _run(tmp_path, corpus, opt, obj="plain.o")
    _run(tmp_path, corpus, "-Wall", opt, obj="checked.o")
    assert (tmp_path / "plain.o").read_bytes() == (tmp_path / "checked.o").read_bytes()


# yasc: a call's result that borrows from an argument (borrows(...), or the
# elision) points into that argument, a frame address too
YASC_PRELUDE = (
    "char *borrows(p) f(char *?p);\n"
    "char *borrows(static, p) fs(char *?p);\n"
    "char *view v(char *p);\n"
    "char *borrows(a) two(char *a, char *b);\n"
)

YASC_BAD = {
    "call": ("char *borrows(static) g(void)\n{\n  char buf[4] = \"ab\";\n  return f(buf);\n}\n", "f"),
    "static_or_param": ("char *borrows(static) g(void)\n{\n  char buf[4] = \"ab\";\n  return fs(buf);\n}\n", "fs"),
    "elision": ("char *borrows(static) g(void)\n{\n  char buf[4] = \"ab\";\n  return v(buf);\n}\n", "v"),
    "nested": ("char *borrows(static) g(void)\n{\n  char buf[4] = \"ab\";\n  return f(f(buf));\n}\n", "f"),
    "offset": ("char *borrows(static) g(void)\n{\n  char buf[4] = \"ab\";\n  return f(buf) + 1;\n}\n", "f"),
    "element": ("char *borrows(static) g(void)\n{\n  char buf[4] = \"ab\";\n  return f(&buf[1]);\n}\n", "f"),
    "borrowed_one": (
        "char *borrows(p) g(char *p)\n{\n  char buf[4] = \"ab\";\n  return two(buf, p);\n}\n",
        "two",
    ),
}

YASC_CLEAN = {
    "static": "char *borrows(static) g(void)\n{\n  static char sb[4];\n  return f(sb);\n}\n",
    "literal": "char *borrows(static) g(void)\n{\n  return f(\"lit\");\n}\n",
    "parameter": "char *borrows(p) g(char *p)\n{\n  return f(p);\n}\n",
    "not_borrowed": "char *borrows(p) g(char *p)\n{\n  char buf[4] = \"ab\";\n  return two(p, buf);\n}\n",
    "used_inside": "int g(void)\n{\n  char buf[4] = \"ab\";\n  char *view r = f(buf);\n  return *r;\n}\n",
}


def _run_yasc(tmp_path, body, *flags):
    src = tmp_path / "t.c"
    src.write_text(YASC_PRELUDE + body)
    return subprocess.run(
        [str(TCC), "-Wno-missing-prototypes", "-Wno-write-strings", "-c", *flags, str(src), "-o", str(tmp_path / "t.o")], capture_output=True, text=True
    )


def _yasc_diags(proc):
    out = []
    for line in proc.stderr.splitlines():
        parts = line.split(": ", 2)
        if len(parts) == 3 and parts[1] in ("warning", "error"):
            out.append((int(parts[0].rsplit(":", 1)[1]) - YASC_PRELUDE.count("\n"), parts[2]))
    return out


@pytest.mark.parametrize("name", sorted(YASC_BAD))
@pytest.mark.parametrize("opt", LEVELS)
def test_yasc_through_call(tmp_path, name, opt):
    body, callee = YASC_BAD[name]
    line = body.count("\n") - 1
    assert _yasc_diags(_run_yasc(tmp_path, body, "-Wall", opt)) == [
        (line, f"'g' returns the address of a local variable (through the result of '{callee}')")
    ]


@pytest.mark.parametrize("opt", LEVELS)
def test_yasc_through_call_held(tmp_path, opt):
    body = "char *borrows(static) g(void)\n{\n  char buf[4] = \"ab\";\n  char *view r = f(buf);\n  return r;\n}\n"
    assert _yasc_diags(_run_yasc(tmp_path, body, "-Wall", opt)) == [
        (5, "'g' may return the address of a local variable (held in 'r')")
    ]


@pytest.mark.parametrize("name", sorted(YASC_CLEAN))
@pytest.mark.parametrize("opt", LEVELS)
def test_yasc_clean(tmp_path, name, opt):
    proc = _run_yasc(tmp_path, YASC_CLEAN[name], "-Wall", opt)
    assert proc.returncode == 0 and not proc.stderr, proc.stderr


def test_yasc_through_call_unsafe(tmp_path):
    body = "char *borrows(static) g(void)\n{\n  char buf[4] = \"ab\";\n  unsafe { return f(buf); }\n}\n"
    proc = _run_yasc(tmp_path, body, "-Wall")
    assert proc.returncode == 0 and not proc.stderr, proc.stderr
