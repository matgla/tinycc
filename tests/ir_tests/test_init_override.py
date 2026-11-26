"""Initializers that lose a value (init/initializer.c).

-Woverride-init (gcc's name, in -Wextra; on in the safe default): a
designator naming bits an earlier element of the same initializer already
set.  The earlier value is gone -- in a union, the whole other member: a
union holds only the member initialized last, so `{ .value = v, .parts =
{ .imag = 1 } }` is `.parts` alone (its `.real` zero), and `{ .i = v,
.p.hi = 3 }` drops `.i` although the two share no byte.  Bit-fields count
bits: two in one storage unit are two elements.

-Wuninitialized (yasc: -Wlifetime, an error in the safe default): a local
read in its own initializer -- `int x = x + 1;`, `u = { .a = v, .b = -u.b }`
(not set yet, and the order the elements are evaluated in is unspecified).
Its address (`void *p = &p;`), `sizeof`, and a call whose result the callee
writes straight into it (sret) read nothing.

Both are frontend checks: the same at every -O level, and the code does not
change.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

LEVELS = ["-O0", "-O1", "-O2", "-Os"]
FLAGS = ["-ccompat", "-Woverride-init", "-Wuninitialized"]
PRELUDE = (
    "int g(int);\n"
    "union U { int i; struct { int lo, hi; } p; };\n"
    "typedef union { float _Complex value; struct { float real, imag; } parts; } FC;\n"
    "struct B { int a : 4, b : 4; unsigned c; };\n"
    "struct N { struct { int a, b; } s; int c; };\n"
    "struct P { int x, y; };\n"
    "struct P mk(int);\n"
)
UNION = ": a union keeps only the member initialized last"
SELF = "is used uninitialized: read in its own initializer"


def _run(tmp_path, body, *flags, obj="t.o"):
    src = tmp_path / "t.c"
    src.write_text(PRELUDE + body)
    return subprocess.run(
        [str(TCC), "-Wno-missing-prototypes", "-fmax-errors=0", "-c", *flags, str(src), "-o", str(tmp_path / obj)],
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


def _check(tmp_path, body, *flags):
    proc = _run(tmp_path, body, *FLAGS, *flags)
    assert proc.returncode == 0, proc.stderr
    return _diags(proc)


BAD = {
    # libc's conjf, the bug that asked for the check: both messages
    "union_member_overlap": (
        "float _Complex f(float _Complex v)\n{\n  FC r = { .value = v, .parts = { .imag = -r.parts.imag } };\n"
        "  return r.value;\n}\n",
        [(3, "initializer for 'parts' overrides an earlier one" + UNION), (3, "'r' " + SELF)],
    ),
    "union_member_disjoint": (
        "int f(int v)\n{\n  union U u = { .i = v, .p.hi = 3 };\n  return u.i;\n}\n",
        [(3, "initializer for 'p' overrides an earlier one" + UNION)],
    ),
    "union_positional_then_other": (
        "int f(void)\n{\n  union U u = { 1, .p.hi = 3 };\n  return u.i;\n}\n",
        [(3, "initializer for 'p' overrides an earlier one" + UNION)],
    ),
    "field_twice": (
        "int f(void)\n{\n  struct P p = { .x = 1, .y = 2, .x = 3 };\n  return p.x;\n}\n",
        [(3, "initializer for 'x' overrides an earlier one")],
    ),
    "bitfield_twice": (
        "int f(void)\n{\n  struct B b = { .b = 1, .a = 2, .b = 3 };\n  return b.a;\n}\n",
        [(3, "initializer for 'b' overrides an earlier one")],
    ),
    "array_positional_runs_into_designated": (
        "int f(void)\n{\n  int t[4] = { [2] = 1, [1] = 2, 3 };\n  return t[0];\n}\n",
        [(3, "initializer for element [2] overrides an earlier one")],
    ),
    "member_of_whole": (
        "int f(void)\n{\n  struct N n = { .s = { 1, 2 }, .s.b = 3 };\n  return n.c;\n}\n",
        [(3, "initializer for 'b' overrides an earlier one")],
    ),
    "element_twice": (
        "int f(void)\n{\n  union U a[2] = { [0] = { .i = 1 }, [0] = { .p.hi = 2 } };\n  return a[0].i;\n}\n",
        [(3, "initializer for element [0] overrides an earlier one")],
    ),
    "whole_after_field": (
        "int f(void)\n{\n  struct P a[2] = { [0].y = 1, [0] = { 2 } };\n  return a[0].y;\n}\n",
        [(3, "initializer for element [0] overrides an earlier one")],
    ),
    "static": ("static struct P sp = { .y = 1, .y = 2 };\nint f(void)\n{\n  return sp.y;\n}\n", [(1, "initializer for 'y' overrides an earlier one")]),
    # a large all-constant local array is probed into a .rodata template first: one warning
    "template_array": (
        "int f(void)\n{\n  int t[100] = { [0 ... 99] = 5, [7] = 1 };\n  return t[g(0)];\n}\n",
        [(3, "initializer for element [7] overrides an earlier one")],
    ),
    # ... and when the probe fails (a runtime value), the per-element pass parses it again: said once
    "template_fallback": (
        "int f(int v)\n{\n  int t[100] = { [0 ... 99] = 5, [7] = 1, [8] = v };\n  return t[g(0)];\n}\n",
        [(3, "initializer for element [7] overrides an earlier one"), (3, "initializer for element [8] overrides an earlier one")],
    ),
    "self_scalar": ("int f(void)\n{\n  int x = x + 1;\n  return x;\n}\n", [(3, "'x' " + SELF)]),
    "self_whole": ("int f(void)\n{\n  int x = x;\n  return x;\n}\n", [(3, "'x' " + SELF)]),
    "self_struct_whole": ("int f(void)\n{\n  struct P p = p;\n  return p.x;\n}\n", [(3, "'p' " + SELF)]),
    "self_member": ("int f(void)\n{\n  struct P p = { .x = 1, .y = p.x };\n  return p.y;\n}\n", [(3, "'p' " + SELF)]),
    "self_through_address": ("int f(void)\n{\n  struct P p = { .y = (&p)->x };\n  return p.y;\n}\n", [(3, "'p' " + SELF)]),
    "self_array_element": ("int f(void)\n{\n  int t[3] = { 1, t[0] };\n  return t[1];\n}\n", [(3, "'t' " + SELF)]),
}


@pytest.mark.parametrize("name", sorted(BAD))
def test_reported(tmp_path, name):
    body, want = BAD[name]
    assert _check(tmp_path, body) == [(line, "warning", msg) for line, msg in want]


CLEAN = {
    "bitfields_apart": "int f(void)\n{\n  struct B b = { .a = 1, .b = 2, .c = 3 };\n  return b.a;\n}\n",
    "members_of_one": "int f(void)\n{\n  struct N n = { .s.a = 1, .s.b = 3, .c = 1 };\n  return n.c;\n}\n",
    "union_one_member": "int f(void)\n{\n  union U u = { .p.lo = 1, .p.hi = 2 };\n  return u.i;\n}\n",
    "unions_apart": "int f(void)\n{\n  union U a[2] = { [0].i = 1, [1].p.hi = 2 };\n  return a[0].i;\n}\n",
    "designated_then_on": "int f(void)\n{\n  int t[6] = { [3] = 1, 2, 3, [0] = 4, 5 };\n  return t[0];\n}\n",
    "brace_elision": "int f(void)\n{\n  int t[2][2] = { 1, 2, 3, 4 };\n  return t[1][1];\n}\n",
    "string": "int f(void)\n{\n  char s[8] = \"ab\";\n  return s[0];\n}\n",
    "address_of_self": "int f(void)\n{\n  void *p = &p;\n  return p != 0;\n}\n",
    "sizeof_self": "int f(void)\n{\n  int z = sizeof z;\n  return z;\n}\n",
    "array_decays": "int f(void)\n{\n  void *t[2] = { t, 0 };\n  return t[0] != 0;\n}\n",
    "sret_into_self": "int f(int v)\n{\n  struct P p = mk(v);\n  return p.x;\n}\n",
    "other_locals": "int f(int v)\n{\n  int a = v;\n  int b = a + 1;\n  struct P p = { .x = a, .y = b };\n  struct P q = p;\n  return q.y;\n}\n",
    "statement_expression": "int f(int v)\n{\n  int a = ({ int b = v; b + 1; });\n  return a;\n}\n",
    # `[1].x`: the element is the field, not the array element from there on
    "array_then_field": "int f(void)\n{\n  struct P a[2] = { [0].x = 1, [0].y = 2, [1].y = 3 };\n  return a[0].y;\n}\n",
    "field_then_array_then_field": "int f(void)\n{\n  struct { struct P p[2]; int z; } q = { .p[0].x = 1, .p[0].y = 2, .z = 3 };\n"
    "  return q.z;\n}\n",
    "range_then_positional": "int f(void)\n{\n  int t[8] = { [0 ... 3] = 1, 2, 3 };\n  return t[5];\n}\n",
}


@pytest.mark.parametrize("name", sorted(CLEAN))
@pytest.mark.parametrize("opt", LEVELS)
def test_clean(tmp_path, name, opt):
    assert _check(tmp_path, CLEAN[name], opt) == []


@pytest.mark.parametrize("opt", LEVELS[1:])
def test_same_at_every_level(tmp_path, opt):
    corpus = "".join(body.replace("f(", f"f_{name}(", 1) for name, (body, _) in BAD.items() if body.startswith("int f("))
    assert _check(tmp_path, corpus, opt) == _check(tmp_path, corpus, "-O0")


BOTH = "int f(int v)\n{\n  struct P p = { .x = v, .x = p.y };\n  return p.x;\n}\n"


def test_off_under_ccompat(tmp_path):
    proc = _run(tmp_path, BOTH, "-ccompat", "-Wall")
    assert proc.returncode == 0 and not proc.stderr, proc.stderr


def test_wextra(tmp_path):
    proc = _run(tmp_path, BOTH, "-ccompat", "-Wextra")
    assert [d[2] for d in _diags(proc)] == ["initializer for 'x' overrides an earlier one"]


def test_safe_default(tmp_path):
    """yasc enforces initialization: the read is an error; the override a warning"""
    proc = _run(tmp_path, BOTH)
    assert proc.returncode != 0
    assert _diags(proc) == [(3, "warning", "initializer for 'x' overrides an earlier one"), (3, "error", "'p' " + SELF)]


def test_wno(tmp_path):
    proc = _run(tmp_path, BOTH, *FLAGS, "-Wno-override-init", "-Wno-uninitialized")
    assert proc.returncode == 0 and not proc.stderr, proc.stderr


def test_error_in_initializer_recovers(tmp_path):
    """An error inside an initializer (a yasc plain pointer) leaves the parse
    of it by a longjmp: what follows is not read in that initializer."""
    body = (
        "char *f(char *out, const char *s)\n{\n  char *q = out;\n  while (*s)\n    *q++ = *s++;\n"
        "  *q = 0;\n  return out;\n}\n"
    )
    proc = _run(tmp_path, body)
    assert proc.returncode != 0
    assert not [d for d in _diags(proc) if SELF in d[2]], proc.stderr


@pytest.mark.parametrize("opt", LEVELS)
def test_code_unchanged(tmp_path, opt):
    corpus = "".join(body.replace("f(", f"f_{name}(", 1) for name, (body, _) in BAD.items() if body.startswith("int f("))
    corpus += "".join(body.replace("f(", f"c_{name}(", 1) for name, body in CLEAN.items())
    _run(tmp_path, corpus, "-ccompat", opt, obj="plain.o")
    _run(tmp_path, corpus, *FLAGS, opt, obj="checked.o")
    assert (tmp_path / "plain.o").read_bytes() == (tmp_path / "checked.o").read_bytes()
