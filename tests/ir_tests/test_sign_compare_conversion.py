"""-Wsign-compare, -Wconversion, -Wsign-conversion: in neither -Wall nor the
safe default (gcc keeps -Wsign-compare in -Wextra for C, the other two in
neither; all three are noisy).

-Wsign-compare: a comparison done unsigned that converts a signed operand,
unless that operand is known non-negative: a constant >= 0, an unsigned
char / short / bit-field promoted, a comparison result; `x == 3u` with a
constant that fits as signed is quiet too.
-Wconversion: an implicit conversion (assignment, initializer, argument,
return) that may change a value: to a narrower integer, float <-> integer,
double to float; for a constant only when the value does change.
-Wsign-conversion (implied by -Wconversion unless given): the sign may
change (`unsigned u = x;`, `int x = u;`, `u = -1`).  A compound assignment
`c += y` checks y against c, so `c += 1` is quiet, as in gcc.  tcc has no
value ranges, so `c = x & 0x7f` or `c = uc + 1` still warn where gcc's range
analysis is quiet.
"""

import subprocess

import pytest

from fe_check_cases import ALL_CHECKS, TCC, Case, matches, run

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

DECLS = (
    "int g;\nunsigned u;\nlong long ll;\nunsigned long long ull;\nunsigned char uc;\nsigned char sc;\n"
    "short sh;\nchar c;\nfloat fl;\ndouble d;\n"
)  # 10 lines

CASES = {
    "sign-compare": Case(
        "sign-compare",
        False,
        DECLS + "int f1(int x)\n{\n  int r = 0;\n"
        "  r += x < u; /* W: comparison of integer expressions of different signedness: 'int' and 'unsigned int' */\n"
        "  r += u < -1; /* W: 'unsigned int' and 'int' */\n"
        "  r += sc < u; /* W: 'signed char' and 'unsigned int' */\n"
        "  r += x < ull; /* W: 'int' and 'unsigned long long' */\n"
        "  r += x == 4294967295u; /* W: different signedness */\n"
        "  return r;\n}\n",
        DECLS + "int f1(int x)\n{\n  int r = 0;\n  r += u < 5;\n  r += uc < u;\n  r += x < ll;\n  r += u < ll;\n"
        "  r += x == 3u;\n  r += x < g;\n  return r;\n}\n",
    ),
    "conversion": Case(
        "conversion",
        False,
        DECLS + "void take(char);\nchar f1(int x)\n{\n"
        "  c = x; /* W: conversion from 'int' to 'char' may change value */\n"
        "  g = ll; /* W: conversion from 'long long' to 'int' may change value */\n"
        "  g = fl; /* W: conversion from 'float' to 'int' may change value */\n"
        "  fl = g; /* W: conversion from 'int' to 'float' may change value */\n"
        "  fl = d; /* W: conversion from 'double' to 'float' may change value */\n"
        "  sc = 200; /* W: changes value from '200' to '-56' */\n"
        "  g = 3.5; /* W: changes value from '3.5' to '3' */\n"
        "  c += x; /* W: conversion from 'int' to 'char' may change value */\n"
        "  take(x); /* W: may change value */\n"
        "  return x; /* W: may change value */\n}\n",
        DECLS + "char f1(int x)\n{\n  g = c;\n  sh = sc;\n  d = g;\n  fl = sh;\n  c = 100;\n  fl = 1.5;\n  g = 3.0;\n"
        "  c += 1;\n  c <<= 1;\n  sh += sh;\n  g = (char)x;\n  ll = g;\n  return 'a';\n}\n",
        extra=("-Wno-sign-conversion",),
    ),
    "sign-conversion": Case(
        "sign-conversion",
        False,
        DECLS + "void take(unsigned);\nint f1(int x)\n{\n"
        "  u = x; /* W: conversion to 'unsigned int' from 'int' may change the sign of the result */\n"
        "  g = u; /* W: conversion to 'int' from 'unsigned int' may change the sign of the result */\n"
        "  ull = x; /* W: may change the sign */\n"
        "  u = -1; /* W: unsigned conversion from 'int' to 'unsigned int' changes value from '-1' to '4294967295' */\n"
        "  g = 0xFFFFFFFFu; /* W: signed conversion from 'unsigned int' to 'int' changes value */\n"
        "  u += x; /* W: may change the sign */\n"
        "  take(x); /* W: may change the sign */\n"
        "  return x;\n}\n",
        DECLS + "int f1(int x)\n{\n  ll = u;\n  u = uc;\n  u = 5;\n  g = 7u;\n  u = (unsigned)x;\n  u = x < g;\n"
        "  return x;\n}\n",
    ),
}


@pytest.mark.parametrize("check", ALL_CHECKS, ids=lambda f: f.__name__)
@pytest.mark.parametrize("name", sorted(CASES))
def test_case(tmp_path, name, check):
    check(tmp_path, CASES[name])


def test_conversion_implies_sign_conversion(tmp_path):
    body = DECLS + "int f1(int x)\n{\n  u = x;\n  c = x;\n  return x;\n}\n"
    proc = run(tmp_path, body, "-Wconversion")
    matches(proc, [(13, "may change the sign"), (14, "may change value")])
    proc = run(tmp_path, body, "-Wconversion", "-Wno-sign-conversion")
    matches(proc, [(14, "may change value")])
    proc = run(tmp_path, body, "-Wsign-conversion")
    matches(proc, [(13, "may change the sign")])


def test_not_in_wall(tmp_path):
    for c in CASES.values():
        matches(run(tmp_path, c.hit, "-Wall", "-Wno-overflow"), [])


RANGES = """\
unsigned char uc, uc2; signed char sc; char c; short s; unsigned short us; int x, y; unsigned u;
_Bool b;
unsigned char n8(int);
signed char s8(int);
void f(int n)
{
  c = x & 0x7f;
  uc = x & 0xff;
  uc = x & 0x1ff;
  c = x ? 'a' : 'b';
  uc = uc + 1;
  uc = uc2 | 128;
  uc = uc ^ uc2;
  uc = uc * 2;
  c = c + 2;
  uc = x % 10;
  uc = u % 10;
  uc = u >> 24;
  uc = u >> 23;
  uc = (x < y);
  us = uc + uc2;
  s = sc * sc;
  uc = b + 1;
  sc = uc - 1;
  c = x ? 1 : 300;
  u = (x & 0xff) < u;
  x = u & 0xff;
  u = x & 0xff;
  uc = -uc;
  us = s & 0x7fff;
  uc = x >> 31;
  c = (char)x + 1;
  uc = n8(x);
  sc = s8(x) + 1;
  u = x < y;
  uc++;
  us = uc * 1000;
  int vla[n];
  vla[0] = 0;
  x = vla[0];
}
"""


@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_value_ranges_as_gcc(tmp_path, opt):
    """the ranges gcc's conversion_warning sees: `x & C`, `+ - * | ^` of narrow
    operands (constants fitting them), comparisons, `?:` arms, unary minus, a
    call's narrow return type; the compiler's own stores (a VLA's size, a
    K&R parameter, ++ on a char, a bit-field's unit) are not checked.  The
    lines arm-none-eabi-gcc 15 reports, but `us = uc * 1000` (gcc: the
    constant does not fit the narrow operand, so no shortening, and a sign
    conversion it words differently)"""
    src = tmp_path / "t.c"
    src.write_text(RANGES)
    proc = subprocess.run([str(TCC), "-Wno-missing-prototypes", "-ccompat", "-c", "-Wconversion", "-Wsign-compare", opt, str(src), "-o",
                           str(tmp_path / "t.o")], capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr
    lines = sorted({int(l.split(":")[1]) for l in proc.stderr.splitlines() if ": warning: " in l})
    assert lines == [8, 9, 16, 17, 18, 19, 24, 25, 31, 37], proc.stderr
