"""yasc enforces initialization (lifetime.c un_check, init/alloc.c).

In yasc, under -Wlifetime (the safe default: errors only unsafe { } lowers),
every local has a value before anything can see it:

- a scalar read on a path where nothing stored it is an error, "is used" or
  "may be used" alike -- the -W[maybe-]uninitialized flow check, made strict;
- a scalar's address may only be taken where it has a value, unless it goes
  straight to an `out T *` parameter (a call's, or an inlined callee's), or
  is only stored through (__builtin_add_overflow's result), which sets it;
- an array, struct, union or complex local, whose later stores the flow
  check does not follow, needs an initializer -- or `= undefined`;
- `T x = undefined;` declares a local with no value on purpose: no code, and
  its address may be taken before it has one.  A scalar's reads are still
  checked.  `undefined` is a name like any other where one is in scope.

Checked on the IR before any pass: the same at every -O level.  Regular C
(-ccompat) is test_uninitialized.py's: warnings when asked for, and a local
whose address is taken is not followed.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

LEVELS = ["-O0", "-O1", "-O2", "-Os"]
PRELUDE = (
    "#pragma yasc\n"
    "int g(int);\n"
    "void fill(out int * view p);\n"
    "void plain(int * view p);\n"
    "void see(const int * view p);\n"
    "static inline void ifill(out int * view p) { *p = 7; }\n"
    "void pfill(out const char * view maybe * view p);\n"
    "static inline int ipfill(const char * view s, out const char * borrows(s) maybe * view p)\n"
    "{\n  *p = nullptr;\n  if (s[0] == '0')\n    *p = s;\n  return 8;\n}\n"
    "struct pt { int x, y; };\n"
    "void sfill(out struct pt * view p);\n"
)

READ_MAY = "may be used uninitialized: yasc wants a value on every path"
READ_IS = "is used uninitialized"
ADDR = "is taken where it may have no value: set it first, pass it to an 'out' parameter, or declare it '= undefined'"


def _run(tmp_path, body, *flags):
    src = tmp_path / "t.c"
    src.write_text(PRELUDE + body)
    return subprocess.run(
        [str(TCC), "-Wno-missing-prototypes", "-Wno-vla", "-fmax-errors=0", "-c", *flags, str(src), "-o", str(tmp_path / "t.o")],
        capture_output=True,
        text=True,
    )


def _diags(proc):
    out = []
    for line in proc.stderr.splitlines():
        parts = line.split(": ", 2)
        if len(parts) == 3 and parts[1] in ("warning", "error"):
            out.append((int(parts[0].rsplit(":", 1)[1]) - PRELUDE.count("\n"), parts[1], parts[2]))
    return out


BAD = {
    "never_set": ("int f(void)\n{\n  int x;\n  return x;\n}\n", 4, f"'x' {READ_IS}"),
    "one_branch": ("int f(int a)\n{\n  int x;\n  if (a)\n    x = 1;\n  return x;\n}\n", 6, f"'x' {READ_MAY}"),
    "loop_only": (
        "int f(int n)\n{\n  int s;\n  for (int i = 0; i < n; i++)\n    s = i;\n  return s;\n}\n",
        6,
        f"'s' {READ_MAY}",
    ),
    "pointer": ("int f(void)\n{\n  int * view p;\n  return *p;\n}\n", 4, f"'p' {READ_IS}"),
    "address_unset": ("int f(void)\n{\n  int m;\n  plain(&m);\n  return m;\n}\n", 4, f"the address of 'm' {ADDR}"),
    "address_to_const": ("int f(void)\n{\n  int m;\n  see(&m);\n  return m;\n}\n", 4, f"the address of 'm' {ADDR}"),
    "address_in_pointer": (
        "int f(void)\n{\n  int m;\n  int * view p = &m;\n  *p = 1;\n  return m;\n}\n",
        4,
        f"the address of 'm' {ADDR}",
    ),
    "address_one_branch": (
        "int f(int c)\n{\n  int m;\n  if (c)\n    m = 1;\n  plain(&m);\n  return m;\n}\n",
        6,
        f"the address of 'm' {ADDR}",
    ),
    "overflow_reads_result": (
        "int f(int a)\n{\n  int r;\n  if (__builtin_add_overflow(a, r, &r))\n    return -1;\n  return r;\n}\n",
        4,
        f"'r' {READ_IS}",
    ),
    "undefined_read": ("int f(void)\n{\n  int m = undefined;\n  return m;\n}\n", 4, f"'m' {READ_IS}"),
    "array": (
        "int f(void)\n{\n  char b[4];\n  b[0] = 0;\n  return b[0];\n}\n",
        3,
        "'b' has no value: yasc does not follow what is stored in an array later: initialize it, or write '= undefined'",
    ),
    "struct": (
        "int f(void)\n{\n  struct pt p;\n  p.x = 1;\n  return p.x;\n}\n",
        3,
        "'p' has no value: yasc does not follow what is stored in a struct later: initialize it, or write '= undefined'",
    ),
    "struct_out": (
        "int f(void)\n{\n  struct pt p;\n  sfill(&p);\n  return p.x;\n}\n",
        3,
        "'p' has no value: yasc does not follow what is stored in a struct later: initialize it, or write '= undefined'",
    ),
    "union": (
        "int f(void)\n{\n  union { int i; float f; } u;\n  u.i = 1;\n  return u.i;\n}\n",
        3,
        "'u' has no value: yasc does not follow what is stored in a union later: initialize it, or write '= undefined'",
    ),
    "vla": (
        "int f(int n)\n{\n  int a[n];\n  a[0] = 1;\n  return a[0];\n}\n",
        3,
        "'a' has no value: yasc does not follow what is stored in a variable length array later: write '= undefined'",
    ),
}


@pytest.mark.parametrize("level", LEVELS)
@pytest.mark.parametrize("name", sorted(BAD))
def test_error(tmp_path, name, level):
    body, line, msg = BAD[name]
    proc = _run(tmp_path, body, level)
    assert proc.returncode != 0, proc.stderr
    assert _diags(proc) == [(line, "error", msg)], proc.stderr


GOOD = {
    "both_branches": "int f(int c)\n{\n  int x;\n  if (c)\n    x = 1;\n  else\n    x = 2;\n  return x;\n}\n",
    "switch_default": (
        "int f(int c)\n{\n  int x;\n  switch (c)\n  {\n  case 0:\n    x = 1;\n    break;\n  default:\n    x = 2;\n  }\n  return x;\n}\n"
    ),
    "out_call": "int f(void)\n{\n  int n;\n  fill(&n);\n  return n;\n}\n",
    "out_inlined": "int f(void)\n{\n  int n;\n  ifill(&n);\n  return n;\n}\n",
    # `out` on the pointer p points at (the level below the parameter's own)
    "out_pointer_call": "int f(void)\n{\n  const char * view maybe q;\n  pfill(&q);\n  return q != nullptr;\n}\n",
    "out_pointer_inlined": (
        "int f(const char * view s)\n{\n  const char * view maybe q;\n  int b = ipfill(s, &q);\n  return q ? b : 0;\n}\n"
    ),
    "overflow_result": (
        "int f(int a, int b)\n{\n  int r;\n  if (__builtin_add_overflow(a, b, &r) || __builtin_mul_overflow(r, 2, &r))\n"
        "    return -1;\n  return r;\n}\n"
    ),
    "set_then_address": "int f(void)\n{\n  int m;\n  m = 3;\n  plain(&m);\n  return m;\n}\n",
    "undefined_address": "int f(void)\n{\n  int m = undefined;\n  plain(&m);\n  return m;\n}\n",
    "undefined_array": "int f(void)\n{\n  char b[4] = undefined, c[2] = {1, 2};\n  b[0] = 1;\n  return b[0] + c[1];\n}\n",
    "undefined_struct": "int f(void)\n{\n  struct pt p = undefined;\n  sfill(&p);\n  return p.x;\n}\n",
    "undefined_vla": "int f(int n)\n{\n  int a[n] = undefined;\n  a[0] = 1;\n  return a[0];\n}\n",
    "initialized_aggregates": (
        "int f(struct pt q)\n{\n  char b[4] = {};\n  struct pt p = q;\n  return b[0] + p.x;\n}\n"
    ),
    "undefined_is_a_name_in_scope": "int f(void)\n{\n  int undefined = 2;\n  {\n    int m = undefined;\n    return m;\n  }\n}\n",
    "static_local": "int f(void)\n{\n  static char b[4];\n  static int n;\n  return b[0] + n;\n}\n",
    "unsafe_lowers": "int f(int c)\n{\n  int x;\n  if (c)\n    x = 1;\n  unsafe {\n    return x;\n  }\n}\n",
}


@pytest.mark.parametrize("level", LEVELS)
@pytest.mark.parametrize("name", sorted(GOOD))
def test_clean(tmp_path, name, level):
    proc = _run(tmp_path, GOOD[name], level)
    assert proc.returncode == 0, proc.stderr
    assert _diags(proc) == [], proc.stderr


def test_undefined_needs_end_of_declarator(tmp_path):
    proc = _run(tmp_path, "int f(void)\n{\n  int m = undefined + 1;\n  return m;\n}\n")
    assert proc.returncode != 0
    assert any("after 'undefined'" in d[2] for d in _diags(proc)), proc.stderr


def test_undefined_emits_no_code(tmp_path):
    """`= undefined` stores nothing: the same code as no initializer (both
    under -ccompat, where a yasc file still parses it and nothing is enforced)."""
    objs = []
    for init in ("", " = undefined"):
        body = f"int f(void)\n{{\n  char b[64]{init};\n  fill((int * view)(void * view)b);\n  return b[1];\n}}\n"
        src = tmp_path / "t.c"
        src.write_text(PRELUDE + body)
        out = tmp_path / f"t{len(objs)}.o"
        proc = subprocess.run(
            [str(TCC), "-ccompat", "-O1", "-Wno-missing-prototypes", "-c", str(src), "-o", str(out)],
            capture_output=True,
            text=True,
        )
        assert proc.returncode == 0, proc.stderr
        objs.append(subprocess.run(["arm-none-eabi-objdump", "-d", str(out)], capture_output=True, text=True).stdout)
    strip = lambda s: [l.split(":", 1)[1] for l in s.splitlines() if l.startswith("  ") and ":" in l]
    assert strip(objs[0]) == strip(objs[1])


def test_ccompat_keeps_regular_c(tmp_path):
    """-ccompat: no enforcement, `undefined` still parses in a yasc file, and an
    address-taken local is not followed (test_uninitialized.py)."""
    body = (
        "int f(int c)\n{\n  int m;\n  plain(&m);\n  char b[4];\n  b[0] = 0;\n  int k = undefined;\n  if (c)\n    k = 1;\n"
        "  return m + b[0] + k;\n}\n"
    )
    proc = _run(tmp_path, body, "-ccompat")
    assert proc.returncode == 0, proc.stderr
    assert _diags(proc) == [], proc.stderr
    proc = _run(tmp_path, body, "-ccompat", "-Wmaybe-uninitialized")
    assert proc.returncode == 0, proc.stderr
    assert _diags(proc) == [(10, "warning", "'k' may be used uninitialized")], proc.stderr
