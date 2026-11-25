"""-Warray-bounds, the frontend part (-Wall, and so the safe default; gcc
needs -O2 for it, here it does not depend on -O): a constant subscript of an
array of known size that is negative or past the end -- the end itself is a
valid address, `&a[n]`, not an element: `a[n]`, `&a[n][0]` and `&a[n+1]`
warn.  A trailing array member is taken as flexible (as gcc does) unless it
has two or more elements and its struct is a named object; an array in a
union, a flexible array member and a pointer are never checked.  Not under
sizeof nor in dead code (`if (0)`).  Pointer arithmetic on such an array with
constants counts as its subscript: `*(a + 4)`, `(a + 2)[3]`, `(a + 4)->m`
(`&*(a + 4)` is the end, fine).
"""

import pytest

from fe_check_cases import ALL_CHECKS, TCC, Case

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

# no plain pointers in yasc (the safe default): V is `view` there, and IO
# `inout` (a view parameter's target is read-only without it)
PRE = "#ifdef __YASC__\n#define V view\n#define IO inout\n#else\n#define V\n#define IO\n#endif\n"  # 7 lines

HIT = PRE + (
    "int a[4];\nint m[3][5];\nint * V gp;\n"
    "struct T { int n; int d[4]; } gt;\n"
    "struct N { int d[2]; int n; };\n"
    "int f1(IO struct N * V pn)\n{\n"
    "  int r = 0;\n"
    "  int la[3] = {};\n"
    "  la[0] = 1;\n"
    "  r += a[4]; /* W: array subscript 4 is above array bounds of 'int[4]' */\n"
    "  a[5] = 1; /* W: array subscript 5 is above array bounds of 'int[4]' */\n"
    "  r += a[-1]; /* W: array subscript -1 is below array bounds of 'int[4]' */\n"
    "  r += la[3]; /* W: array subscript 3 is above array bounds of 'int[3]' */\n"
    "  r += m[1][5]; /* W: array subscript 5 is above array bounds of 'int[5]' */\n"
    "  gp = &m[3][0]; /* W: array subscript 3 is above array bounds of 'int[3][5]' */\n"
    "  gp = &a[5]; /* W: array subscript 5 is above */\n"
    "  r += 4[a]; /* W: array subscript 4 is above */\n"
    "  r += gt.d[4]; /* W: array subscript 4 is above */\n"
    "  r += pn->d[2]; /* W: array subscript 2 is above array bounds of 'int[2]' */\n"
    '  r += "abc"[5]; /* W: array subscript 5 is above array bounds of */\n'  # 'const char[4]' in the safe default (-Wwrite-strings)
    "  r += *(a + 4); /* W: array subscript 4 is above array bounds of 'int[4]' */\n"
    "  r += *(4 + a); /* W: array subscript 4 is above */\n"
    "  r += *(a - 1); /* W: array subscript -1 is below */\n"
    "  r += *(a + 2 + 2); /* W: array subscript 4 is above */\n"
    "  r += (a + 2)[3]; /* W: array subscript 5 is above */\n"
    "  r += *(gt.d + 5); /* W: array subscript 5 is above */\n"
    "  r += *(la + 3); /* W: array subscript 3 is above array bounds of 'int[3]' */\n"
    "  r += (pn->d + 2)[0]; /* W: array subscript 2 is above array bounds of 'int[2]' */\n"
    "  return r;\n}\n"
    "struct P { int x; } ps[3];\n"
    "int f2(void)\n{\n"
    "  return (ps + 3)->x; /* W: array subscript 3 is above array bounds of 'struct P[3]' */\n}\n"
)

CLEAN = PRE + (
    "int a[4];\nint m[3][5];\nint * V gp;\nint (* V gq)[5];\n"
    "struct F { int n; int d[]; };\n"
    "struct Z { int n; int d[0]; };\n"
    "struct O { int n; int d[1]; };\n"
    "struct T { int n; int d[4]; };\n"
    "union U { int d[1]; char c[16]; };\n"
    "int f1(struct F * V pf, struct Z * V pz, struct O * V po, struct T * V pt, union U * V pu, int * V p)\n{\n"
    "  int r = 0;\n"
    "  gp = &a[4];\n  gq = &m[3];\n  gp = a + 4;\n  r += a[3];\n  r += pf->d[10];\n  r += pz->d[10];\n"
    "  r += po->d[10];\n  r += pt->d[10];\n  r += pu->d[3];\n  r += p[100];\n  r += sizeof(a[10]);\n"
    "  if (0)\n    r += a[7];\n"
    "  gp = &*(a + 4);\n  r += *(a + 3);\n  r += (a + 2)[1];\n  r += *(pf->d + 10);\n  r += *(po->d + 10);\n"
    "  r += *(pu->d + 3);\n  r += *(p + 100);\n  r += sizeof *(a + 9);\n  r += *(a + r);\n"
    "  if (0)\n    r += *(a + 7);\n"
    "  return r;\n}\n"
)

CASE = Case("array-bounds", True, HIT, CLEAN)


@pytest.mark.parametrize("check", ALL_CHECKS, ids=lambda f: f.__name__)
def test_array_bounds(tmp_path, check):
    check(tmp_path, CASE)
