"""-Wuninitialized / -Wmaybe-uninitialized (lifetime.c, un_check).

A scalar local declared without a value and read where no path stored one
is "used uninitialized"; read where only some paths did, "may be used
uninitialized".  A declaration inside a loop makes the variable uninitialised
again on every iteration.  A local whose address is taken, or that a nested
function captures, is not followed.  Off under -ccompat unless asked for;
the safe default is yasc, which enforces initialization (errors, and an
address-taken local followed: test_yasc_init.py).  Checked on the IR before
any pass: the same at every -O level, and the code does not change.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

LEVELS = ["-O0", "-O1", "-O2", "-Os"]
FLAGS = ["-ccompat", "-Wuninitialized", "-Wmaybe-uninitialized"]
# VIEW: `view` where the file is yasc (the safe default), nothing in plain C
PRELUDE = (
    "#ifdef __YASC__\n#define VIEW view\n#else\n#define VIEW\n#endif\n"
    "int g(int);\nvoid set(int * VIEW p);\n"
)


def _run(tmp_path, body, *flags, obj="t.o"):
    src = tmp_path / "t.c"
    src.write_text(PRELUDE + body)
    return subprocess.run(
        [str(TCC), "-Wno-missing-prototypes", "-c", *flags, str(src), "-o", str(tmp_path / obj)], capture_output=True, text=True
    )


def _diags(proc):
    out = []
    for line in proc.stderr.splitlines():
        parts = line.split(": ", 2)
        if len(parts) == 3 and parts[1] in ("warning", "error"):
            out.append((int(parts[0].rsplit(":", 1)[1]) - PRELUDE.count("\n"), parts[1], parts[2]))
    return out


def _check(tmp_path, body, *flags):
    proc = _run(tmp_path, body, *FLAGS, *flags)
    assert proc.returncode == 0, proc.stderr
    return _diags(proc)


BAD = {
    "never_set": ("int f(void)\n{\n  int x;\n  return x;\n}\n", 4, "'x' is used uninitialized"),
    "one_branch": ("int f(int a)\n{\n  int x;\n  if (a)\n    x = 1;\n  return x;\n}\n", 6, "'x' may be used uninitialized"),
    "increment": ("int f(void)\n{\n  int x;\n  x++;\n  return x;\n}\n", 4, "'x' is used uninitialized"),
    "loop_redeclared": (
        "int f(int n)\n{\n  int s = 0;\n  for (int i = 0; i < n; i++)\n  {\n    int t;\n    if (i)\n      t = i;\n"
        "    s += t;\n  }\n  return s;\n}\n",
        9,
        "'t' may be used uninitialized",
    ),
    "set_in_loop": (
        "int f(int n)\n{\n  int b;\n  while (n < 1)\n  {\n    b = n;\n    n++;\n  }\n  return b;\n}\n",
        9,
        "'b' may be used uninitialized",
    ),
    "long_long": (
        "long long f(int a)\n{\n  long long v;\n  if (a)\n    v = 2;\n  return v;\n}\n",
        6,
        "'v' may be used uninitialized",
    ),
    "pointer": ("char *f(void)\n{\n  char *p;\n  return p;\n}\n", 4, "'p' is used uninitialized"),
    "read_on_one_path": (
        "int f(int b, int c)\n{\n  int x;\n  if (b)\n    return x & c;\n  return 1;\n}\n",
        5,
        "'x' is used uninitialized",
    ),
}


@pytest.mark.parametrize("name", sorted(BAD))
def test_reported(tmp_path, name):
    body, line, msg = BAD[name]
    assert _check(tmp_path, body) == [(line, "warning", msg)]


CLEAN = {
    "both_branches": "int f(int a)\n{\n  int x;\n  if (a)\n    x = 1;\n  else\n    x = 2;\n  return x;\n}\n",
    "address_taken": "int f(void)\n{\n  int x;\n  set(&x);\n  return x;\n}\n",
    "loop_until_set": "int f(int n)\n{\n  int x;\n  for (;;)\n  {\n    x = g(n);\n    if (x)\n      break;\n  }\n"
    "  return x;\n}\n",
    "initialised": "int f(int a)\n{\n  int x = a;\n  return x;\n}\n",
    "switch_default": "int f(int a)\n{\n  int x;\n  switch (a)\n  {\n  case 1:\n    x = 1;\n    break;\n"
    "  default:\n    x = 0;\n  }\n  return x;\n}\n",
    "set_in_condition": "int f(int a)\n{\n  int x;\n  while (!(x = g(a)))\n    ;\n  return x;\n}\n",
    "nested_capture": "int f(int n)\n{\n  int x;\n  void put(int v) { x = v; }\n  put(n);\n  return x;\n}\n",
}


@pytest.mark.parametrize("name", sorted(CLEAN))
@pytest.mark.parametrize("opt", LEVELS)
def test_clean(tmp_path, name, opt):
    assert _check(tmp_path, CLEAN[name], opt) == []


@pytest.mark.parametrize("opt", LEVELS[1:])
def test_same_at_every_level(tmp_path, opt):
    corpus = "".join(body.replace("f(", f"f_{name}(", 1) for name, (body, _, _) in BAD.items())
    assert _check(tmp_path, corpus, opt) == _check(tmp_path, corpus, "-O0")


BOTH = "int f(int a)\n{\n  int x, y;\n  if (a)\n    y = 1;\n  return x + y;\n}\n"


def test_flags_apart(tmp_path):
    proc = _run(tmp_path, BOTH, "-ccompat", "-Wuninitialized")
    assert [d[2] for d in _diags(proc)] == ["'x' is used uninitialized"]
    proc = _run(tmp_path, BOTH, "-ccompat", "-Wmaybe-uninitialized")
    assert [d[2] for d in _diags(proc)] == ["'y' may be used uninitialized"]


def test_off_under_ccompat(tmp_path):
    proc = _run(tmp_path, BOTH, "-ccompat", "-Wall")
    assert proc.returncode == 0 and not proc.stderr, proc.stderr


def test_safe_default(tmp_path):
    """The safe default is yasc, which enforces initialization: errors, the
    "may" case too (test_yasc_init.py)"""
    proc = _run(tmp_path, BOTH)
    assert proc.returncode != 0, proc.stderr
    assert sorted((d[1], d[2]) for d in _diags(proc)) == [
        ("error", "'x' is used uninitialized"),
        ("error", "'y' may be used uninitialized: yasc wants a value on every path"),
    ]


def test_wno(tmp_path):
    """-Wno-uninitialized turns regular C's warnings off, not yasc's errors"""
    proc = _run(tmp_path, BOTH, "-ccompat", "-Wall", "-Wno-uninitialized", "-Wno-maybe-uninitialized")
    assert proc.returncode == 0 and not proc.stderr, proc.stderr
    proc = _run(tmp_path, BOTH, "-Wno-uninitialized", "-Wno-maybe-uninitialized")
    assert proc.returncode != 0 and len(_diags(proc)) == 2, proc.stderr


def test_werror_reports_all(tmp_path):
    proc = _run(tmp_path, BOTH, *FLAGS, "-Werror=uninitialized", "-Werror=maybe-uninitialized")
    assert proc.returncode != 0
    assert [d[:2] for d in _diags(proc)] == [(6, "error"), (6, "error")]


def test_wno_error(tmp_path):
    proc = _run(tmp_path, BOTH, *FLAGS, "-Werror", "-Wno-error=uninitialized", "-Wno-error=maybe-uninitialized")
    assert proc.returncode == 0, proc.stderr
    assert len(_diags(proc)) == 2


@pytest.mark.parametrize("opt", LEVELS[1:])
def test_code_unchanged(tmp_path, opt):
    corpus = BOTH + "".join(body.replace("f(", f"f_{name}(", 1) for name, (body, _, _) in BAD.items())
    _run(tmp_path, corpus, "-ccompat", opt, obj="plain.o")
    _run(tmp_path, corpus, *FLAGS, opt, obj="checked.o")
    assert (tmp_path / "plain.o").read_bytes() == (tmp_path / "checked.o").read_bytes()
