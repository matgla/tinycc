"""Constant-expression checks of the frontend (gcc has the first four on by
default: -Wall here, and so the safe default):

-Wshift-count-overflow / -Wshift-count-negative: a constant shift count >= the
width of the promoted left operand, or < 0.
-Wdiv-by-zero: / or % by the integer constant 0.
-Woverflow: two constants folded in a signed type that overflow ("integer
overflow in expression"), unary minus of the most negative value, and a
constant converted (assignment, initializer, argument, return) to a type whose
range [-2^(n-1), 2^n - 1] it is outside, as gcc words it.
-Wtautological-compare: the same expression compared with itself -- the same
variable or member, or two operands that read the same tokens with no side
effect between them (`p->a == p->a`, `(x+1) == (x+1)`) -- no float (x != x is
the NaN test), no volatile, not two constants or two truth values; gcc's
bitwise forms `(x & 1) == 2`, `(x | 4) != 1`.
-Wshift-overflow (gcc's level 1, on by default there): a positive constant
shifted past the sign bit of a signed type (`2 << 31`; into it is allowed).
-Wtype-limits (not -Wall, gcc's -Wextra): unsigned < 0, unsigned >= 0, and a
char or short compared with a constant outside its range.

Not in code that is never evaluated (sizeof, the dead arm of a constant ?:,
&& and ||) nor in #if; dead statements are still checked, as gcc.
"""

import pytest

from fe_check_cases import ALL_CHECKS, TCC, Case, matches, run

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

CASES = {
    "shift-count-overflow": Case(
        "shift-count-overflow",
        True,
        "int g;\nlong long gl;\n"
        "int f1(int x)\n{\n"
        "  g = x << 32; /* W: left shift count >= width of type */\n"
        "  g = x >> 40; /* W: right shift count >= width of type */\n"
        "  gl = gl << 64; /* W: left shift count >= width of type */\n"
        "  return g;\n}\n",
        "int g;\nlong long gl;\nint f1(int x)\n{\n  g = x << 31;\n  gl = gl << 63;\n  g = sizeof(x << 40);\n"
        "  g = 0 && (1 << 40);\n  g = 1 ? 0 : 1 << 40;\n  return g;\n}\n",
    ),
    "shift-count-negative": Case(
        "shift-count-negative",
        True,
        "int g;\nint f1(int x)\n{\n  g = x << -1; /* W: left shift count is negative */\n"
        "  g = x >> -3; /* W: right shift count is negative */\n  return g;\n}\n",
        "int g;\nint f1(int x)\n{\n  g = x << 0;\n  g = x >> 3u;\n  return g;\n}\n",
    ),
    "div-by-zero": Case(
        "div-by-zero",
        True,
        "int g;\ndouble d;\nint f1(int x)\n{\n  g = x / 0; /* W: division by zero */\n"
        "  g = x % 0; /* W: division by zero */\n  g /= 0; /* W: division by zero */\n"
        "  d = 1.0 / 0; /* W: division by zero */\n"
        "  if (0)\n    g = x / 0; /* W: division by zero */\n  return g;\n}\n",
        "int g;\ndouble d;\nint f1(int x)\n{\n  g = 0 && (x / 0);\n  g = sizeof(x / 0);\n  g = 1 ? 1 : x % 0;\n"
        "  d = 1.0 / 0.0;\n  return g;\n}\n",
    ),
    "overflow": Case(
        "overflow",
        True,
        "char c;\nunsigned char uc;\nint g;\nlong long gl;\n"
        "struct B { unsigned a : 3; } gb;\n"
        "void take(char);\n"
        "int f1(int x)\n{\n"
        "  g = 2147483647 + 1; /* W: integer overflow in expression of type 'int' results in '-2147483648' */\n"
        "  g = 65536 * 65536; /* W: integer overflow in expression of type 'int' results in '0' */\n"
        "  g = -(-2147483647 - 1); /* W: integer overflow in expression '-2147483648' */\n"
        "  gl = 9223372036854775807LL + 1; /* W: integer overflow in expression of type 'long long' */\n"
        "  c = 300; /* W: unsigned conversion from 'int' to 'char' changes value from '300' to '44' */\n"
        "  uc = 1 << 8; /* W: changes value from '256' to '0' */\n"
        "  g = 4294967296LL; /* W: overflow in conversion from 'long long' to 'int' */\n"
        "  gb.a = 9; /* W: changes value from '9' to '1' */\n"
        "  take(300); /* W: changes value from '300' to '44' */\n"
        "  return x;\n}\n"
        "char f2(int x)\n{\n  if (x)\n    return 256; /* W: changes value from '256' to '0' */\n  return 0;\n}\n",
        "char c;\nunsigned char uc;\nsigned char sc;\nint g;\nunsigned u;\n"
        "int f1(int x)\n{\n  u = 4294967295u + 1;\n  uc = -1;\n  sc = 200;\n  c = 255;\n  g = 0xFFFFFFFFu;\n"
        "  g = 0 && (2147483647 + 1);\n  g = (char)300;\n  c = 'a' + 1;\n  c += 300;\n  return x;\n}\n",
    ),
    "tautological-compare": Case(
        "tautological-compare",
        True,
        "int g;\nstruct S { int a; } gs;\n"
        "int f1(int x)\n{\n"
        "  int r = 0;\n"
        "  r += x == x; /* W: self-comparison always evaluates to true */\n"
        "  r += g != g; /* W: self-comparison always evaluates to false */\n"
        "  r += gs.a <= gs.a; /* W: self-comparison always evaluates to true */\n"
        "  r += x < x; /* W: self-comparison always evaluates to false */\n"
        "  return r;\n}\n"
        "struct T { int a, b; int arr[4]; } gt;\n"
        "int f2(int x, unsigned u)\n{\n"
        "  int r = 0;\n"
        "  r += (x + 1) == (x + 1); /* W: self-comparison always evaluates to true */\n"
        "  r += gt.arr[x] < gt.arr[x]; /* W: self-comparison always evaluates to false */\n"
        "  r += (gt.a * 3) >= (gt.a * 3); /* W: self-comparison always evaluates to true */\n"
        "  r += (x & 1) == 2; /* W: bitwise comparison always evaluates to false */\n"
        "  r += (x | 4) == 1; /* W: bitwise comparison always evaluates to false */\n"
        "  r += (x & 1) != 2; /* W: bitwise comparison always evaluates to true */\n"
        "  r += (x | 4) != 1; /* W: bitwise comparison always evaluates to true */\n"
        "  r += (u & 0xf0) == 0x0f; /* W: bitwise comparison always evaluates to false */\n"
        "  r += 2 == (x & 1); /* W: bitwise comparison always evaluates to false */\n"
        "  return r;\n}\n",
        "int g, h;\nvolatile int vg;\nfloat fl;\nint k(int);\n"
        "struct T { int a, b; volatile int v; } gt;\n"
        "int f1(int x, int y)\n{\n  int r = 0;\n  r += x == g;\n  r += vg == vg;\n  r += fl != fl;\n"
        "  r += g == h;\n  r += gt.a == gt.b;\n  r += k(x) == k(x);\n  r += x++ == x++;\n  r += gt.v == gt.v;\n"
        "  r += (x + 1) == (x + 2);\n  r += (x < y) == (x < y);\n  r += 1 == 1;\n  r += (x & 3) == 3;\n"
        "  r += (x & 1) == 0;\n  r += (x & 8) > 9;\n  r += (x | 4) == 5;\n  r += (fl + 1) == (fl + 1);\n"
        "  r += (x & 1) + 1 == 2;\n  return r;\n}\n",
    ),
    "shift-overflow": Case(
        "shift-overflow",
        True,
        "int g;\nlong long gl;\n"
        "int f1(int x)\n{\n"
        "  g = 2 << 31; /* W: result of '2 << 31' requires 34 bits to represent, but 'int' only has 32 bits */\n"
        "  gl = 3LL << 63; /* W: result of '3 << 63' requires 66 bits to represent, but 'long long' only has 64 bits */\n"
        "  g = 0x10000 << 16; /* W: requires 34 bits */\n"
        "  return g + x;\n}\n",
        "int g;\nlong long gl;\nint f1(int x)\n{\n  g = 1 << 31;\n  g = 3 << 30;\n  g = 2u << 31;\n"
        "  gl = 1LL << 63;\n  gl = 2LL << 62;\n  g = -1 << 3;\n  g = x << 31;\n  g = sizeof(2 << 31);\n"
        "  g = 0 && (2 << 31);\n  return g;\n}\n",
    ),
    "type-limits": Case(
        "type-limits",
        False,
        "unsigned u;\nunsigned char uc;\nsigned char sc;\nunsigned long long ull;\n"
        "int f1(int x)\n{\n"
        "  int r = 0;\n"
        "  r += u < 0; /* W: comparison of unsigned expression in '< 0' is always false */\n"
        "  r += u >= 0; /* W: comparison of unsigned expression in '>= 0' is always true */\n"
        "  r += 0 > ull; /* W: comparison of unsigned expression in '< 0' is always false */\n"
        "  r += uc > 255; /* W: comparison is always false due to limited range of data type */\n"
        "  r += sc < 128; /* W: comparison is always true due to limited range of data type */\n"
        "  r += uc == -1; /* W: comparison is always false due to limited range of data type */\n"
        "  return r + x;\n}\n",
        "unsigned u;\nunsigned char uc;\nint g;\n"
        "int f1(int x)\n{\n  int r = 0;\n  r += u > 0;\n  r += u <= 0;\n  r += uc < 255;\n  r += x < 0;\n"
        "  r += g >= 0;\n  return r;\n}\n",
    ),
}


@pytest.mark.parametrize("check", ALL_CHECKS, ids=lambda f: f.__name__)
@pytest.mark.parametrize("name", sorted(CASES))
def test_case(tmp_path, name, check):
    check(tmp_path, CASES[name])


def test_file_scope(tmp_path):
    """static initializers, enumerators and case labels, also after a function
    (a no-eval count was left behind there: DATA_ONLY_WANTED was 2 in
    function.c)"""
    body = (
        "void first(void) {}\n"
        "int g1 = 1 << 40;\n"
        "enum { E = 2147483647 + 1 };\n"
        "static char sc = 300;\n"
        "char *use(void) { return &sc; }\n"
        "int sw(int n) { switch (n) { case 2147483647 + 1: return 1; } return 0; }\n"
    )
    proc = run(tmp_path, body, "-Wall")
    matches(
        proc,
        [(2, "left shift count"), (3, "integer overflow"), (4, "unsigned conversion"), (6, "integer overflow")],
    )


def test_not_in_preprocessor(tmp_path):
    proc = run(tmp_path, "#if 1 << 40\nint x;\n#endif\n#if (2147483647 + 1) < 0\nint y;\n#endif\n", "-Wall")
    assert "shift count" not in proc.stderr and "integer overflow in expression" not in proc.stderr, proc.stderr


def test_system_header_is_quiet(tmp_path):
    inc = tmp_path / "sys"
    inc.mkdir()
    (inc / "s.h").write_text("static inline int sh(int x) { char c = 300; return (x << 40) + c + (x == x); }\n")
    body = "#include <s.h>\nint a(int y) { return sh(y) + sh(y + 1); }\n"
    for lvl in ("-O0", "-O2"):
        proc = run(tmp_path, body, "-Wall", "-isystem", str(inc), lvl)
        assert proc.returncode == 0 and not proc.stderr, proc.stderr


def test_user_header_inline_names_the_header(tmp_path):
    """an inlined body's warning names its own file at every level"""
    (tmp_path / "u.h").write_text("static inline int uh(int x)\n{\n  return x << 40;\n}\n")
    body = '#include "u.h"\nint a(int y) { return uh(y) + uh(y + 1); }\n'
    for lvl in ("-O0", "-O2"):
        proc = run(tmp_path, body, "-Wall", lvl)
        lines = [l for l in proc.stderr.splitlines() if "shift count" in l]
        assert len(lines) == 1 and "u.h:3:" in lines[0], proc.stderr


def test_tautological_through_pointers(tmp_path):
    """members reached through pointers, compared as the tokens they read"""
    body = (
        "struct T { int a, b; int arr[4]; volatile int v; struct T *n; };\n"
        "int f(struct T *p, int x)\n{\n"
        "  int r = p->a == p->a;\n"
        "  r += p->arr[x] < p->arr[x];\n"
        "  r += p->n->b >= p->n->b;\n"
        "  r += p->n->a == p->n->b;\n"
        "  r += p->v == p->v;\n"
        "  r += p->n++ == p->n++;\n"
        "  return r;\n}\n"
    )
    for lvl in ("-O0", "-O2"):
        proc = run(tmp_path, body, "-Wall", lvl)
        matches(proc, [(4, "always evaluates to true"), (5, "always evaluates to false"), (6, "always evaluates to true")])


def test_static_folded_at_compile_time_is_checked(tmp_path):
    """a static function every call of which is evaluated at compile time
    (constant arguments, -O1+) is never generated: its checks still come,
    once, at every level -- also for one from a header"""
    (tmp_path / "ceh.h").write_text(
        "static inline int sq(int x)\n{\n  int r = x << 40;\n  return r + x;\n}\n"
        "static int k(int x) { return (x << 50) + (x == x); }\n"
    )
    body = '#include "ceh.h"\nint f(void) { return sq(3) + k(2) + sq(4) + k(5); }\n'
    for lvl in ("-O0", "-O1", "-O2", "-Os"):
        proc = run(tmp_path, body, "-Wall", lvl)
        lines = sorted(l.split(": warning: ")[0].rsplit("/", 1)[-1] + ": " + l.split(": warning: ")[1]
                       for l in proc.stderr.splitlines() if ": warning: " in l)
        assert lines == ["ceh.h:3: left shift count >= width of type", "ceh.h:6: left shift count >= width of type",
                         "ceh.h:6: self-comparison always evaluates to true"], (lvl, proc.stderr)
        run(tmp_path, body, "-w", lvl, obj="w.o")
        assert (tmp_path / "w.o").read_bytes() == (tmp_path / "t.o").read_bytes(), lvl
