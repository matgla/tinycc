"""-Warray-bounds=2: accesses through pointers (ir/bounds.c).

The value-range analysis of the raw IR also follows pointers: a pointer
into a known object is its offset from the object's start and the bytes
left to its end, each an interval.  Objects: locals the frame holds (an
array, a struct, a scalar whose address is taken), globals of a known size
(not extern scalars: a linker's symbols), string literals, and allocations
by a function with alloc_size or one of the C library's allocators, whose
size is the interval its arguments have.  `*p`, `p[k]`, `p->m` are checked
once p went through a variable or got a variable index (the frontend's
level-1 checks say the constant ones), on a path that certainly runs: a
branch on a value a loop's widening over-approximated may never be taken.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

PRELUDE = """\
typedef unsigned long size_t;
void *malloc(size_t);
void *calloc(size_t, size_t);
void *realloc(void *, size_t);
void free(void *);
void *my_alloc(int n) __attribute__((alloc_size(1)));
int g(int);
int ga[8];
struct S { int a[4]; int tail; } gs;
extern unsigned _sdata, _edata;
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
int h1(void) { int a[4] = {0}; int *p = a + 2; return p[2]; }
int h2(void) { int a[4] = {0}, *p = a, s = 0; for (int i = 0; i <= 4; i++) s += *p++; return s; }
int h3(void) { int *p = malloc(4 * sizeof *p); p[4] = 1; return p[0]; }
int h4(void) { int *p = calloc(4, sizeof *p); return p[5]; }
int h5(int c) { int *p = malloc(c ? 16 : 32); return p[8]; }
int h6(void) { int *p = ga; return p[8]; }
int h7(void) { struct S s = {0}; struct S *q = &s; int *p = &q->tail; return p[1]; }
int h8(void) { int a[4] = {0}; int *p = a; return p[-1]; }
int h9(void) { int a[4] = {0}; char *p = (char *)a; return p[16]; }
int h10(void) { char *s = "abc"; return s[4]; }
int h11(void) { int *p = my_alloc(8); return p[2]; }
int h12(void) { int x = 0; int *p = &x; return p[1]; }
int h13(void) { int *p = ga; int i = 0; while (i < 9) { p[i] = 0; i++; } return 0; }
int h14(void) { int a[4] = {0}; char *p = (char *)a + 1; return *(int *)(p + 12); }
int h15(void) { int *p = malloc(8); p = realloc(p, 12); return p[3]; }
"""
HITS_EXPECT = [
    (1, "array subscript 4 is outside array bounds of 'int[4]'"),
    (2, "array subscript 4 is outside array bounds of 'int[4]'"),
    (3, "array subscript 4 is outside the 16 bytes allocated"),
    (4, "array subscript 5 is outside the 16 bytes allocated"),
    (5, "array subscript 8 is outside the 16 to 32 bytes allocated"),
    (6, "array subscript 8 is outside array bounds of 'int[8]'"),
    (7, "array subscript 5 is outside array bounds of 'struct S[1]'"),
    (8, "array subscript -1 is outside array bounds of 'int[4]'"),
    (9, "array subscript 16 is outside array bounds of 'int[4]'"),
    (10, "array subscript 4 is outside array bounds of 'char[4]'"),
    (11, "array subscript 2 is outside the 8 bytes allocated"),
    (12, "array subscript 1 is outside array bounds of 'int[1]'"),
    (13, "array subscript 8 is outside array bounds of 'int[8]'"),
    (14, "access of 4 bytes at offset 13 is outside 'a' (16 bytes)"),
    (15, "array subscript 3 is outside the 12 bytes allocated"),
]

MISSES = """\
int m1(void) { int a[4] = {0}, *p = a, s = 0; for (int i = 0; i < 4; i++) s += *p++; return s; }
int m2(void) { int a[4] = {0}, s = 0; for (int *p = a; p < a + 4; p++) s += *p; return s; }
int m3(int n) { int *p = malloc(n * sizeof *p); int r = p[n - 1]; free(p); return r; }
int m4(int *p) { return p[100]; }
int m5(void) { char buf[8] = "abc", *p = buf; while (*p) p++; return p - buf; }
int m6(void) { unsigned *src = &_sdata; unsigned *dst = &_edata; return dst[1] + src[3]; }
int m7(int k) { int a[4] = {0}, b[8] = {0}; int *p = k ? a : b; return p[3]; }
int m8(int n) { int a[4] = {0}; int *p = a; if (n >= 0 && n < 4) p += n; return *p; }
int m9(int k, int p0) {
  int j = 0, pdest = 0, *p = ga - 1;
  for (;;) {
    if (pdest > 2) do j--, pdest++; while (j > 2);
    if (j == 1) break;
    while (pdest > p0) if (j == p0) pdest++;
    if (j == 0) p = ga;
    k = p[0];
    j = 1;
  }
  return k + pdest;
}
int m10(void) { int *p = malloc(16); if (!p) return 0; int r = p[3]; free(p); return r; }
int m11(void) { int a[4] = {0}; return a[3] + *(a + 3); }
struct F { int n; int d[]; };
struct F gf = { 1, { 2, 3, 4 } };
int m12(void) { int *p = gf.d; return p[2]; }
int m13(void) { int a[4] = {0}; int *p = &a[4]; return p[-1]; }
int m14(int n) { int a[8] = {0}; int *p = a; for (int i = 0; i < n; i++) p++; return *p; }
"""


@pytest.mark.parametrize("opt", ["-O0", "-O1", "-O2", "-Os"])
def test_hits(tmp_path, opt):
    proc = _run(tmp_path, HITS, "-Warray-bounds=2", opt)
    assert _diags(proc) == HITS_EXPECT, proc.stderr


@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_misses(tmp_path, opt):
    proc = _run(tmp_path, MISSES, "-Wall", "-Warray-bounds=2", opt)
    assert "array subscript" not in proc.stderr and "access of" not in proc.stderr, proc.stderr


def test_notes(tmp_path):
    body = (
        "int f(void) {\n"
        "  int a[4] = {0};\n"
        "  int *p = a + 2;\n"
        "  return p[2];\n"
        "}\n"
        "int h(int c) {\n"
        "  int *p = malloc(c ? 16 : 32);\n"
        "  return p[8];\n"
        "}\n"
    )
    proc = _run(tmp_path, body, "-Warray-bounds=2")
    assert _diags(proc, notes=True) == [
        (4, "array subscript 4 is outside array bounds of 'int[4]'"),
        (2, "'a' declared here"),
        (4, "the subscript is 4 on a path that reaches it"),
        (8, "array subscript 8 is outside the 16 to 32 bytes allocated"),
        (7, "allocated here"),
        (8, "the subscript is 8 on a path that reaches it"),
    ], proc.stderr


def test_level_1_said_it(tmp_path):
    """A constant subscript of the array itself: the frontend says it, once."""
    body = "int f(void) { int c[2] = {0}; return c[-1] + ga[9]; }\n"
    proc = _run(tmp_path, body, "-Warray-bounds=2")
    assert _diags(proc) == [
        (1, "array subscript -1 is below array bounds of 'int[2]'"),
        (1, "array subscript 9 is above array bounds of 'int[8]'"),
    ], proc.stderr


def test_only_level_2(tmp_path):
    body = "int f(void) { int a[4] = {0}; int *p = a + 2; return p[2]; }\n"
    for flags in ([], ["-Wall"], ["-Warray-bounds"], ["-Warray-bounds=2", "-Wno-array-bounds"]):
        assert "array subscript" not in _run(tmp_path, body, *flags).stderr, flags
    # the safe default
    proc = _run(tmp_path, "int f(void) { int a[4] = {0}; int *view p = a + 2; return p[2]; }\n", "-Warray-bounds=2",
                ccompat=False, prelude="")
    assert "warning: array subscript 4 is outside array bounds of 'int[4]'" in proc.stderr, proc.stderr


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
