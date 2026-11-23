"""-Warray-bounds in loops (builtin/objsize.c; -Wall, as gcc's -Warray-bounds).

`for (i = C0; i < N; i++)` (also <=, N > i, ++i, i += 1, `for (int i = ...`)
with i a local integer variable and C0 at most the last value: a[i] or
a[i + k] in the body, where a's size is known, is checked with i's last value.
Only an access that no branch precedes in the body (if, switch, a loop,
break, continue, goto, return, ?:, &&, || -- an inline expansion's own
branches do not count, so -O2 says what -O0 does), only when the body stores
nothing to i and its address is not taken; &a[n] is fine.  The pattern is
read by the parser, so the result is the same at every -O level; while loops,
other strides and conditions in the body (a value-range analysis on a check
SSA) are not done.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

PRELUDE = """\
int a[10];
struct S { int x[4]; int y; };
struct F { int n; int d[1]; };
void use(int);
static inline void zz(int *p) { if (*p) use(1); }
"""
PL = PRELUDE.count("\n")


def _run(tmp_path, body, *flags, ccompat=True, prelude=PRELUDE):
    src = tmp_path / "t.c"
    src.write_text(prelude + body)
    cmd = [str(TCC)] + (["-ccompat"] if ccompat else []) + ["-c", *flags, str(src), "-o", str(tmp_path / "t.o")]
    return subprocess.run(cmd, capture_output=True, text=True)


def _diags(proc):
    out = []
    for line in proc.stderr.splitlines():
        parts = line.split(":", 3)
        if len(parts) == 4 and parts[1].isdigit():
            out.append((int(parts[1]) - PL, parts[3].strip()))
    return out


BODY = """\
void f(int c, struct S *ps, struct F *pf)
{
  int i, b[5];
  for (i = 0; i <= 10; i++) a[i] = 0;
  for (int j = 0; j < 10; j++) a[j] = 1;
  for (int j = 0; j < 5; j++) b[j + 1] = 2;
  for (i = 0; i <= 10; i++) { if (i == 10) break; a[i] = 3; }
  for (i = 0; i <= 10; i++) { a[i] = 3; i++; }
  for (i = 0; 10 >= i; ++i) use(a[i]);
  for (i = 0; i <= 4; i += 1) use(ps->x[i]);
  for (i = 0; i <= 10; i++) use(c ? a[i] : 0);
  for (i = 0; i <= 10; i++) { zz(&c); a[i] = 4; }
  for (i = 20; i <= 10; i++) a[i] = 0;
  for (i = 0; i <= 10; i++) use(&a[i] - a);
  for (int k = 0; k < 3; k++) for (i = 0; i <= 5; i++) b[i] = k;
  for (i = 0; i <= 3; i++) pf->d[i] = 0;
  for (i = 0; i <= 10; i++) { use(i); a[i] = 0; }
  for (i = 0; i <= 10; i += 2) a[i] = 0;
  for (i = 0; i <= 10; i++) { int *p = &i; a[i] = *p; }
  use(b[0]);
}
"""
EXPECT = [
    (4, "array subscript 10 is above array bounds of 'int[10]'"),
    (6, "array subscript 5 is above array bounds of 'int[5]'"),
    (9, "array subscript 10 is above array bounds of 'int[10]'"),
    (10, "array subscript 4 is above array bounds of 'int[4]'"),
    (12, "array subscript 10 is above array bounds of 'int[10]'"),
    (15, "array subscript 5 is above array bounds of 'int[5]'"),
    (17, "array subscript 10 is above array bounds of 'int[10]'"),
]


@pytest.mark.parametrize("opt", ["-O0", "-O1", "-O2", "-Os"])
def test_hits_and_misses(tmp_path, opt):
    proc = _run(tmp_path, BODY, "-Wall", opt)
    assert _diags(proc) == EXPECT, proc.stderr


def test_switches(tmp_path):
    body = "void f(void) { for (int i = 0; i <= 10; i++) a[i] = 0; }\n"
    msg = "array subscript 10 is above array bounds of 'int[10]'"
    assert msg not in _run(tmp_path, body).stderr
    assert msg in _run(tmp_path, body, "-Warray-bounds").stderr
    assert msg not in _run(tmp_path, body, "-Wall", "-Wno-array-bounds").stderr
    proc = _run(tmp_path, body, "-Wall", "-Werror=array-bounds")
    assert proc.returncode != 0 and "error: " + msg in proc.stderr, proc.stderr
    proc = _run(tmp_path, body, "-Wall", "-Werror", "-Wno-error=array-bounds")
    assert proc.returncode == 0 and "warning: " + msg in proc.stderr, proc.stderr
    proc = _run(tmp_path, body, ccompat=False, prelude="int a[10];\n")
    assert proc.returncode == 0 and "warning: " + msg in proc.stderr, proc.stderr


def test_pragma(tmp_path):
    body = (
        "void f(void)\n{\n"
        "#pragma GCC diagnostic push\n"
        "#pragma GCC diagnostic ignored \"-Warray-bounds\"\n"
        "  for (int i = 0; i <= 10; i++) a[i] = 0;\n"
        "#pragma GCC diagnostic pop\n"
        "  for (int i = 0; i <= 10; i++) a[i] = 0;\n}\n"
    )
    proc = _run(tmp_path, body, "-Wall")
    assert [d[0] for d in _diags(proc)] == [7], proc.stderr


def test_once_in_an_inline_function(tmp_path):
    body = (
        "static inline void clr(void) { for (int i = 0; i <= 10; i++) a[i] = 0; }\n"
        "void f(void) { clr(); clr(); }\n"
    )
    for opt in ("-O0", "-O2"):
        proc = _run(tmp_path, body, "-Wall", opt)
        assert proc.stderr.count("array subscript 10") == 1, (opt, proc.stderr)


def test_objects_unchanged(tmp_path):
    objs = []
    for opt in ("-O0", "-O2"):
        for flags in (["-w"], ["-Wall"]):
            out = tmp_path / ("o%d.o" % len(objs))
            src = tmp_path / "b.c"
            src.write_text(PRELUDE + BODY)
            proc = subprocess.run([str(TCC), "-ccompat", opt, *flags, "-c", str(src), "-o", str(out)],
                                  capture_output=True, text=True)
            assert proc.returncode == 0, proc.stderr
            objs.append(out.read_bytes())
    assert objs[0] == objs[1] and objs[2] == objs[3]
