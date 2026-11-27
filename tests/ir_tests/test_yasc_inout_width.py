"""yasc -Wparam-access: an `inout` parameter wider than its body needs.

`inout T *p` says the function reads *p and writes it.  When the body only
writes it -- on every path, before any read -- `out` says what it does (and
the caller need not set it first); when the body never writes through it, the
default `in` (a const target) does.  The flow walk of lifetime.c decides: a
dereference reads or writes the target; the pointer handed to a call reads it
for an `in` (const) parameter, writes it for an `out` one, and may do both for
anything else; stored, returned or reassigned, it says nothing.  Moved along
its own target (`p++`, `p += n`, `p = p - 1`) it still points at the caller's
memory: that writes nothing, for an `out` parameter too.  A struct's target is
never told to be `out` (one member written, another read).  `inout` on a const
target (`inout const char *s`) can write nothing at all.  On in the safe
default, a warning.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

LEVELS = ["-O0", "-O2"]

PRELUDE = """\
void fill(out int *n);
void bump(inout int *n);
int peek(int *n);
struct S { int a, b; };
"""
PRELUDE_LINES = PRELUDE.count("\n")


def _run(tmp_path, body, *flags):
    src = tmp_path / "t.c"
    src.write_text(PRELUDE + body)
    return subprocess.run(
        [str(TCC), "-Wno-missing-prototypes", "-c", *flags, str(src), "-o", str(tmp_path / "t.o")],
        capture_output=True, text=True, cwd=tmp_path,
    )


def _diags(proc):
    out = []
    for line in proc.stderr.splitlines():
        parts = line.split(": ", 2)
        if len(parts) == 3 and parts[1] in ("warning", "error") and parts[0].rsplit(":", 1)[0].endswith("t.c"):
            out.append((int(parts[0].rsplit(":", 1)[1]) - PRELUDE_LINES, parts[1], parts[2]))
    return out


def _out(p):
    return f"'{p}' is inout, but '*{p}' is written before it is read: say out"


def _drop(p, f):
    return f"'{p}' is inout, but '{f}' writes nothing through it: drop inout"


SAY_OUT = {
    "store": "void f(inout int *n) { *n = 1; }\n",
    "store_then_read": "void f(inout int *n) { *n = 1; *n += 1; }\n",
    "to_out_callee": "void f(inout int *n) { fill(n); }\n",
    "elements": "void f(inout char *b) { b[0] = 0; b[1] = 0; }\n",
    "moved_then_store": "void f(inout char *b) { b++; *b = 0; }\n",
}

DROP_INOUT = {
    "read": "int f(inout int *n) { return *n; }\n",
    "to_in_callee": "int f(inout int *n) { return peek(n); }\n",
    "null_test_only": "int f(inout int *n) { return n ? 1 : 0; }\n",
    "struct_read": "int f(inout struct S *s) { return s->a; }\n",
    "empty_body": "void f(inout int *n) { (void)n; }\n",
    # moving the pointer along the caller's target writes nothing
    "moved_inc": "int f(inout int *n) { n++; return *n; }\n",
    "moved_add": "int f(inout int *n) { n += 2; return *n; }\n",
    "moved_assign": "int f(inout int *n) { n = n + 1; return *n; }\n",
    "moved_in_loop": "int f(inout int *n) { while (*n == 1) n++; return peek(n); }\n",
}

CLEAN = {
    "read_modify_write": "void f(inout int *n) { *n += 1; }\n",
    "read_then_write": "void f(inout int *n) { int x = *n; *n = x + 1; }\n",
    "written_on_one_path": "void f(inout int *n, int c) { if (c) *n = 1; }\n",
    "loop_may_not_run": "void f(inout char *b, int k) { for (int i = 0; i < k; i++) b[i] = 0; }\n",
    "to_inout_callee": "void f(inout int *n) { bump(n); }\n",
    "alias_written": "void f(inout int *n) { int *q = n; *q = 1; }\n",
    "through_void": "void f(inout void *p) { int *q = p; *q = 1; }\n",
    "reassigned": "void f(inout int *n, inout int *m) { n = m; *n = 1; }\n",
    "moved_then_read_modify_write": "void f(inout int *n) { n++; *n += 1; }\n",
    "moved_to_inout_callee": "void f(inout int *n) { n++; bump(n); }\n",
    "struct_member_written": "void f(inout struct S *s) { s->a = 1; }\n",
    "out_already": "void f(out int *n) { *n = 1; }\n",
    "in_already": "int f(int *n) { return *n; }\n",
}


@pytest.mark.parametrize("opt", LEVELS)
@pytest.mark.parametrize("name", sorted(SAY_OUT))
def test_say_out(tmp_path, name, opt):
    proc = _run(tmp_path, SAY_OUT[name], opt)
    assert proc.returncode == 0 and _diags(proc) == [(1, "warning", _out("n" if "char" not in SAY_OUT[name] else "b"))], proc.stderr


@pytest.mark.parametrize("opt", LEVELS)
@pytest.mark.parametrize("name", sorted(DROP_INOUT))
def test_drop_inout(tmp_path, name, opt):
    p = "s" if "struct" in DROP_INOUT[name] else "n"
    proc = _run(tmp_path, DROP_INOUT[name], opt)
    assert proc.returncode == 0 and _diags(proc) == [(1, "warning", _drop(p, "f"))], proc.stderr


@pytest.mark.parametrize("opt", LEVELS)
@pytest.mark.parametrize("name", sorted(CLEAN))
def test_clean(tmp_path, name, opt):
    proc = _run(tmp_path, CLEAN[name], opt)
    assert proc.returncode == 0 and _diags(proc) == [], proc.stderr


def test_off_with_no_param_access(tmp_path):
    proc = _run(tmp_path, SAY_OUT["store"] + "int g(inout int *k) { return *k; }\n", "-Wno-param-access")
    assert proc.returncode == 0 and _diags(proc) == [], proc.stderr


def test_regular_c_says_nothing(tmp_path):
    # no inout in regular C: a pointer to non-const that is only written is not one
    src = tmp_path / "c.c"
    src.write_text("void f(int *n) { *n = 1; }\nint g(int *n) { return *n; }\n")
    proc = subprocess.run([str(TCC), "-ccompat", "-Wall", "-c", str(src), "-o", str(tmp_path / "c.o")],
                          capture_output=True, text=True, cwd=tmp_path)
    assert proc.returncode == 0 and proc.stderr == "", proc.stderr


@pytest.mark.parametrize("opt", LEVELS)
def test_out_array_parameter_is_out(tmp_path, opt):
    # `out char a[2]`: out goes on the pointer the array becomes, so the
    # body is held to it (and it is not taken for an inout)
    proc = _run(tmp_path, "void f(out char a[2]) { char x = a[0]; a[0] = x; }\n", opt)
    assert (1, "error", "'*a' is read before it is written") in _diags(proc), proc.stderr
    proc = _run(tmp_path, "void f(out char a[2]) { a[0] = 0; a[1] = 0; }\n", opt)
    assert proc.returncode == 0 and _diags(proc) == [], proc.stderr


# `out T **p`: out sits on the pointer p points at (the first '*' parsed);
# the body is held to it as for `out T *p` -- with borrows(...) too
OUT_TWO_LEVELS = {
    "borrows": "void f(char *s, int c, out const char *borrows(s) *e) { if (c) *e = s; }\n",
    "plain": "void f(int c, out const char **e) { if (c) *e = \"x\"; }\n",
}


@pytest.mark.parametrize("opt", LEVELS)
@pytest.mark.parametrize("name", sorted(OUT_TWO_LEVELS))
def test_out_two_levels_must_be_written(tmp_path, name, opt):
    proc = _run(tmp_path, OUT_TWO_LEVELS[name], opt)
    assert (1, "error", "'*e' is not written when 'f' returns") in _diags(proc), proc.stderr


@pytest.mark.parametrize("opt", LEVELS)
def test_out_two_levels_read_first(tmp_path, opt):
    proc = _run(tmp_path, "int f(out const char **e) { int r = **e == 0; *e = \"x\"; return r; }\n", opt)
    assert (1, "error", "'*e' is read before it is written") in _diags(proc), proc.stderr


@pytest.mark.parametrize("opt", LEVELS)
def test_out_two_levels_maybe_null_path(tmp_path, opt):
    # a null `*?` out pointer has nothing to write
    proc = _run(tmp_path, "void f(char *s, out const char *borrows(s) *?e) { if (e) *e = s; }\n", opt)
    assert proc.returncode == 0 and _diags(proc) == [], proc.stderr


CONST_TARGET = {
    "plain": "int f(inout const int *n) { return *n; }\n",
    "moved": "int f(inout const int *?n) { if (!n) return 0; n++; return *n; }\n",
    "inner_const": "int f(inout int *const *n) { return **n; }\n",
}


@pytest.mark.parametrize("opt", LEVELS)
@pytest.mark.parametrize("name", sorted(CONST_TARGET))
def test_inout_const_target(tmp_path, name, opt):
    # nothing can be written through it: inout says more than the type allows
    proc = _run(tmp_path, CONST_TARGET[name], opt)
    assert proc.returncode == 0 and _diags(proc) == [
        (1, "warning", "'n' is inout, but its target is const: drop inout")], proc.stderr
    proc = _run(tmp_path, CONST_TARGET[name], opt, "-Wno-param-access")
    assert proc.returncode == 0 and _diags(proc) == [], proc.stderr


@pytest.mark.parametrize("opt", LEVELS)
def test_out_moved_is_not_written(tmp_path, opt):
    # `a++` on an out parameter writes nothing through it
    proc = _run(tmp_path, "int f(out char *a) { a++; return *a; }\n", opt)
    assert (1, "error", "'*a' is read before it is written") in _diags(proc), proc.stderr
    proc = _run(tmp_path, "void f(out char *a) { a++; }\n", opt)
    assert (1, "error", "'*a' is not written when 'f' returns") in _diags(proc), proc.stderr
    proc = _run(tmp_path, "void f(out char *a) { a++; *a = 0; }\n", opt)
    assert proc.returncode == 0 and _diags(proc) == [], proc.stderr
