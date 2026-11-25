"""-Wdangling-pointer, and -Wreturn-local-addr through a variable.

A pointer variable holding an address in this call's frame (`p = &x`,
`p = buf`) returned -- "may return the address of a local variable" under
-Wreturn-local-addr -- or a frame address stored through a parameter, or
left in a global when the function returns -- -Wdangling-pointer.  A global
cleared again before every return is fine (`g = &x; use(); g = 0;`), and so
is anything main stores (its frame lasts as long as the program).  `if (p ==
&x)` tells the paths apart.  Plain C; the safe default has both, -Wall has
-Wreturn-local-addr.  Same at every -O level, and no change to the code.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

LEVELS = ["-O0", "-O1", "-O2", "-Os"]
FLAGS = ["-ccompat", "-Wdangling-pointer", "-Wreturn-local-addr"]
PRELUDE = "struct S { char *buf; int n; };\nint *g;\nstatic char *sg;\nint use(int *p);\n"


def _run(tmp_path, body, *flags, obj="t.o"):
    src = tmp_path / "t.c"
    src.write_text(PRELUDE + body)
    return subprocess.run(
        [str(TCC), "-c", *flags, str(src), "-o", str(tmp_path / obj)], capture_output=True, text=True
    )


def _diags(proc):
    out = []
    for line in proc.stderr.splitlines():
        parts = line.split(": ", 2)
        if len(parts) == 3 and parts[1] in ("warning", "error"):
            out.append((int(parts[0].rsplit(":", 1)[1]) - PRELUDE.count("\n"), parts[2]))
    return out


BAD = {
    "return_through_variable": (
        "int *f(int c)\n{\n  int x = c;\n  int *p = &x;\n  return p;\n}\n",
        5,
        "'f' may return the address of a local variable (held in 'p')",
    ),
    "return_array_through_variable": (
        "char *f(void)\n{\n  char b[8];\n  char *p = b;\n  p++;\n  return p;\n}\n",
        6,
        "'f' may return the address of a local variable (held in 'p')",
    ),
    "global": ("void f(void)\n{\n  int x = 1;\n  g = &x;\n}\n", 4,
               "the address of a local variable is stored in global 'g', which outlives 'f'"),
    "global_cleared_on_one_path": (
        "int f(int c)\n{\n  int x = c;\n  g = &x;\n  if (c)\n    g = 0;\n  return 1;\n}\n",
        4,
        "the address of a local variable is stored in global 'g', which outlives 'f'",
    ),
    "static_through_variable": (
        "void f(void)\n{\n  char b[4];\n  char *p = b;\n  sg = p;\n}\n",
        5,
        "the address of a local variable is stored in global 'sg', which outlives 'f'",
    ),
    "member_through_parameter": (
        "void f(struct S *s)\n{\n  char b[8];\n  s->buf = b;\n}\n",
        4,
        "the address of a local variable is stored through 's', which outlives 'f'",
    ),
    "out_parameter": ("void f(int **pp)\n{\n  int x = 0;\n  *pp = &x;\n}\n", 4,
                      "the address of a local variable is stored through 'pp', which outlives 'f'"),
}

CLEAN = {
    "parameter_value": "int *f(int *a)\n{\n  int *p = a;\n  return p;\n}\n",
    "reassigned": "int *f(int c)\n{\n  int x = c;\n  int *p = &x;\n  p = g;\n  return p;\n}\n",
    "static_local": "void f(void)\n{\n  static int st;\n  g = &st;\n}\n",
    "heap_through_parameter": "void f(struct S *s, char *heap)\n{\n  s->buf = heap;\n}\n",
    "dereferenced": "int f(void)\n{\n  int x = 1;\n  int *p = &x;\n  return *p;\n}\n",
    "global_cleared": "int f(void)\n{\n  int x = 2;\n  g = &x;\n  int r = use(g);\n  g = 0;\n  return r;\n}\n",
    "cleared_inside_branch": "int f(int c)\n{\n  int x = c;\n  if (c)\n  {\n    g = &x;\n    c = use(g);\n"
    "    g = 0;\n  }\n  return c;\n}\n",
    "compared_away": "struct S *mk(void);\nstruct S *f(void)\n{\n  struct S *p, q;\n  p = mk();\n  if (!p)\n"
    "    p = &q;\n  if (p == &q)\n    p = 0;\n  return p;\n}\n",
    "main": "int main(void)\n{\n  int x = 1;\n  g = &x;\n  return *g;\n}\n",
}


@pytest.mark.parametrize("name", sorted(BAD))
@pytest.mark.parametrize("opt", LEVELS)
def test_reported(tmp_path, name, opt):
    body, line, msg = BAD[name]
    assert _diags(_run(tmp_path, body, *FLAGS, opt)) == [(line, msg)]


@pytest.mark.parametrize("name", sorted(CLEAN))
@pytest.mark.parametrize("opt", LEVELS)
def test_clean(tmp_path, name, opt):
    proc = _run(tmp_path, CLEAN[name], *FLAGS, opt)
    assert proc.returncode == 0 and not proc.stderr, proc.stderr


def test_switches(tmp_path):
    body = BAD["global"][0] + BAD["return_through_variable"][0].replace("f(", "h(", 1)
    assert not _run(tmp_path, body, "-ccompat").stderr
    wall = _diags(_run(tmp_path, body, "-ccompat", "-Wall", "-Wno-unused"))
    assert [m for _, m in wall] == ["'h' may return the address of a local variable (held in 'p')"]
    assert len(_diags(_run(tmp_path, body, "-ccompat", "-Wdangling-pointer"))) == 1
    assert not _run(tmp_path, body, *FLAGS, "-Wno-dangling-pointer", "-Wno-return-local-addr").stderr


def test_safe_default(tmp_path):
    src = tmp_path / "t.c"
    src.write_text("int * view g;\nvoid f(void)\n{\n  int x = 1;\n  g = &x;\n}\n")
    proc = subprocess.run([str(TCC), "-c", str(src), "-o", str(tmp_path / "t.o")], capture_output=True, text=True)
    # an error in safe mode (strict lifetimes)
    assert proc.returncode != 0 and "error:" in proc.stderr and "stored in global 'g'" in proc.stderr, proc.stderr


@pytest.mark.parametrize("opt", LEVELS[1:])
def test_code_unchanged(tmp_path, opt):
    corpus = "".join(body.replace("f(", f"f_{name}(", 1) for name, (body, _, _) in BAD.items())
    _run(tmp_path, corpus, "-ccompat", opt, obj="plain.o")
    _run(tmp_path, corpus, *FLAGS, opt, obj="checked.o")
    assert (tmp_path / "plain.o").read_bytes() == (tmp_path / "checked.o").read_bytes()
