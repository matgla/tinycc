"""-Warray-bounds=2: value ranges of subscripts on the raw IR (ir/bounds.c).

Level 2 only (not -Wall, not plain -Warray-bounds, which are level 1: the
frontend's constant and `for`-pattern checks).  An interval analysis of the
raw IR before any pass, so the same at every -O: while and do loops, other
strides, loops counting down, conditions in the body (`if (i < N) a[i]`),
indexes computed from i (`a[2 * i]`, `a[i - 1]`), and a subscript after a
loop.  Loops are unrolled while each trip is decided by the header; a trip
that may leave on an unknown condition ends it, and the widened rest is not
checked.  Reported: an index whose whole range on a reached path is out of
bounds (gcc's text, a note with the range), once per subscript.  Values an
inlined body computes are unknown, as the call they replace.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

PRELUDE = """\
int a[10];
char buf[8];
struct T { int x[4]; int y; } gt;
int g(int);
void abort(void);
static inline int plus10(int i) { return i + 10; }
"""
PL = PRELUDE.count("\n")


def _run(tmp_path, body, *flags, ccompat=True, prelude=PRELUDE, name="t"):
    src = tmp_path / (name + ".c")
    src.write_text(prelude + body)
    cmd = [str(TCC)] + (["-ccompat"] if ccompat else []) + ["-c", *flags, str(src), "-o", str(tmp_path / (name + ".o"))]
    return subprocess.run(cmd, capture_output=True, text=True)


def _diags(proc, notes=False):
    out = []
    for line in proc.stderr.splitlines():
        parts = line.split(":", 3)
        if len(parts) == 4 and parts[1].isdigit() and (notes or parts[2].strip() != "note"):
            out.append((int(parts[1]) - PL, parts[3].strip()))
    return out


HITS = """\
int w1(void) { int i = 0, s = 0; while (i <= 10) { s += a[i]; i++; } return s; }
int w2(void) { int s = 0; for (int j = 0; j < 30; j += 3) s += a[j]; return s; }
int w3(void) { int s = 0; for (int j = 0; j < 20; j++) if (j < 12) s += a[j]; return s; }
int w4(void) { int s = 0; for (int k = 9; k >= -1; k--) s += a[k]; return s; }
int w5(void) { int s = 0; for (int i = 0; i < 6; i++) s += a[2 * i]; return s; }
int w6(void) { int s = 0; for (int i = 0; i < 10; i++) s += a[i - 1]; return s; }
int w7(void) { int i; for (i = 0; i < 10; i++) g(i); return a[i]; }
int w8(void) { int i = 0, s = 0; do { s += a[i]; } while (++i <= 10); return s; }
int w9(int i) { if (i > 10 && i < 20) return a[i]; return 0; }
int w10(void) { int s = 0; for (unsigned char c = 0; c < 9; c++) s += buf[c]; return s; }
int w11(void) { int s = 0; for (int i = 0; i < 4; i++) s += gt.x[i + 1]; return s; }
int w12(int n) { int s = 0; for (int i = 0; i < n; i++) s += a[i + 10]; return s; }
int w13(void) { int *p = 0; for (int i = 0; i <= 11; i++) p = &a[i]; return *p; }
int w14(void) { for (int i = 0; i <= 12; i++) a[i] = 0; return 0; }
"""
HITS_EXPECT = [
    (1, "array subscript 10 is above array bounds of 'int[10]'"),
    (2, "array subscript 12 is above array bounds of 'int[10]'"),
    (3, "array subscript 10 is above array bounds of 'int[10]'"),
    (4, "array subscript -1 is below array bounds of 'int[10]'"),
    (5, "array subscript 10 is above array bounds of 'int[10]'"),
    (6, "array subscript -1 is below array bounds of 'int[10]'"),
    (7, "array subscript 10 is above array bounds of 'int[10]'"),
    (8, "array subscript 10 is above array bounds of 'int[10]'"),
    (9, "array subscript [11, 19] is outside array bounds of 'int[10]'"),
    (10, "array subscript 8 is above array bounds of 'char[8]'"),
    (11, "array subscript 4 is above array bounds of 'int[4]'"),
    (12, "array subscript 10 is above array bounds of 'int[10]'"),
    # the level-1 loop pattern says it (once)
    (13, "array subscript 11 is above array bounds of 'int[10]'"),
    (14, "array subscript 12 is above array bounds of 'int[10]'"),
]

MISSES = """\
int m1(void) { int s = 0; for (int i = 0; i <= 10; i++) { if (g(i)) break; s += a[i]; } return s; }
int m2(int n) { int s = 0; for (int i = 0; i < n; i++) s += a[i]; return s; }
int m3(int i) { if (i < 0 || i >= 10) return 0; return a[i]; }
int m4(int i) { return a[(unsigned)i % 10] + a[i & 7]; }
int m5(void) { int s = 0; for (int i = 0; i <= 10; i++) { if (i == 10) abort(); s += a[i]; } return s; }
int m6(void) { int s = 0; for (int i = 0; ; i++) { if (i == 10) return s; s += a[i]; } }
int m7(void) { int i = 0; while (a[i] != 0) i++; return i; }
int m8(void) { int s = 0; for (int i = 0; i < 10; i++) for (int j = i; j < 10; j++) s += a[j] + a[i]; return s; }
int m9(void) { int s = 0; for (unsigned i = 9; i < 10; i--) s += a[i]; return s; }
int m10(void) { int s = 0; for (int i = 1; i < 20; i++) s += i - 1 < 18 ? a[(i - 1) / 2] : 0; return s; }
int m11(void) { int s = 0; for (int i = 0; i < 10; i++) s += a[plus10(i) - 10]; return s; }
int m12(int n) { int s = 0; for (int i = 0; i < n; i++) for (int j = 0; j < n; j++) s += gt.x[i] + gt.x[j]; return s; }
int m13(void) { int s = 0; for (int i = 0; i <= 10; i++) s += a[plus10(i) - 11]; return s; }
int m14(int i) { int b[4] = {0}; long long k = 12; return b[k & 3] + b[i < 4 && i >= 0 ? i : 0]; }
"""


@pytest.mark.parametrize("opt", ["-O0", "-O1", "-O2", "-Os"])
def test_hits(tmp_path, opt):
    proc = _run(tmp_path, HITS, "-Warray-bounds=2", opt)
    assert _diags(proc) == HITS_EXPECT, proc.stderr


@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_misses(tmp_path, opt):
    proc = _run(tmp_path, MISSES, "-Wall", "-Warray-bounds=2", opt)
    assert "array subscript" not in proc.stderr, proc.stderr


def test_notes(tmp_path):
    body = (
        "int f(void) { int i = 0, s = 0; while (i <= 10) { s += a[i]; i++; } return s; }\n"
        "int h(void) { int s = 0; for (int j = 0; j < 20; j++) if (j < 12) s += a[j]; return s; }\n"
    )
    proc = _run(tmp_path, body, "-Warray-bounds=2")
    assert _diags(proc, notes=True) == [
        (1, "array subscript 10 is above array bounds of 'int[10]'"),
        (1, "the subscript is 10 on a path that reaches it"),
        (2, "array subscript 10 is above array bounds of 'int[10]'"),
        (2, "the subscript is in [10, 11] on the paths that reach it"),
    ], proc.stderr


def test_levels_and_switches(tmp_path):
    body = "int f(void) { int i = 0, s = 0; while (i <= 10) { s += a[i]; i++; } return s; }\n"
    msg = "array subscript 10 is above array bounds of 'int[10]'"
    for flags in ([], ["-Wall"], ["-Warray-bounds"], ["-Warray-bounds=1"], ["-Warray-bounds=2", "-Warray-bounds"],
                  ["-Warray-bounds=0"], ["-Warray-bounds=2", "-Wno-array-bounds"], ["-Warray-bounds=2", "-w"]):
        assert msg not in _run(tmp_path, body, *flags).stderr, flags
    for flags in (["-Warray-bounds=2"], ["-Wall", "-Warray-bounds=2"], ["-Warray-bounds=2", "-Wall"]):
        assert "warning: " + msg in _run(tmp_path, body, *flags).stderr, flags
    proc = _run(tmp_path, body, "-Werror=array-bounds=2")
    assert proc.returncode != 0 and "error: " + msg in proc.stderr, proc.stderr
    proc = _run(tmp_path, body, "-Warray-bounds=2", "-Werror=array-bounds")
    assert proc.returncode != 0 and "error: " + msg in proc.stderr, proc.stderr
    proc = _run(tmp_path, body, "-Warray-bounds=2", "-Werror", "-Wno-error=array-bounds")
    assert proc.returncode == 0 and "warning: " + msg in proc.stderr, proc.stderr
    proc = _run(tmp_path, body, "-Warray-bounds=3")
    assert proc.returncode != 0 and "invalid argument in '-Warray-bounds=3'" in proc.stderr, proc.stderr
    # the safe default (no -ccompat)
    proc = _run(tmp_path, body, "-Warray-bounds=2", ccompat=False, prelude="int a[10];\n")
    assert proc.returncode == 0 and "warning: " + msg in proc.stderr, proc.stderr


def test_pragma(tmp_path):
    body = (
        "int f(void)\n{\n  int s = 0, i = 0, j = 0;\n"
        "#pragma GCC diagnostic push\n"
        "#pragma GCC diagnostic ignored \"-Warray-bounds\"\n"
        "  while (i <= 10) { s += a[i]; i++; }\n"
        "#pragma GCC diagnostic pop\n"
        "  while (j <= 10) { s += a[j]; j++; }\n"
        "  return s;\n}\n"
    )
    proc = _run(tmp_path, body, "-Warray-bounds=2")
    assert [d[0] for d in _diags(proc)] == [8], proc.stderr


def test_once_in_an_inline_function(tmp_path):
    body = (
        "static inline int sum(void) { int i = 0, s = 0; while (i <= 10) s += a[i++]; return s; }\n"
        "int f(void) { return sum() + sum(); }\n"
    )
    for opt in ("-O0", "-O2"):
        proc = _run(tmp_path, body, "-Warray-bounds=2", opt)
        assert proc.stderr.count("array subscript 10") == 1, (opt, proc.stderr)


def test_objects_unchanged(tmp_path):
    for opt in ("-O0", "-O1", "-O2", "-Os"):
        objs = []
        for flags in (["-w"], ["-Wall", "-Warray-bounds=2"]):
            out = tmp_path / ("o%d.o" % len(objs))
            src = tmp_path / "b.c"
            src.write_text(PRELUDE + HITS + MISSES)
            proc = subprocess.run([str(TCC), "-ccompat", opt, *flags, "-c", str(src), "-o", str(out)],
                                  capture_output=True, text=True)
            assert proc.returncode == 0, proc.stderr
            objs.append(out.read_bytes())
        assert objs[0] == objs[1], opt
