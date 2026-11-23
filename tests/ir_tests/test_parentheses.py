"""-Wparentheses and -Wdangling-else (gcc -Wall, so -Wall and the safe default
here), -Wempty-body (gcc -Wextra; the safe default here, not -Wall).

-Wparentheses: an assignment used as the condition of if/while/do/for without
parentheses of its own (`=` only, as gcc), and gcc's operator-precedence
cases: `&&` within `||`, `+`/`-` inside `<<`/`>>`, arithmetic or `&`/`^` in an
operand of `|`/`^`, `+`/`-` or a comparison in an operand of `&`, a comparison
in an operand of `==`/`!=`, and `X<=Y<=Z`.  Never in an #if.
-Wdangling-else: `if (a) if (b) x; else y;` (also through a loop body).
-Wempty-body: `if (x);` with no else, `else;`, `do ; while (x);`.
"""

import pytest

from stmt_check_common import TCC, check_levels, check_option, check_system_header, diags, run

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

P_ASSIGN = "suggest parentheses around assignment used as truth value"
P_ANY = "suggest parentheses"
P_CMP = "do not have their mathematical meaning"

HIT = """int f(int a, int b)
{
  if (a = b)
    return 1;
  return 0;
}
"""
CLEAN = """int f(int a, int b)
{
  if ((a = b))
    return 1;
  while ((a = b - 1) != 0)
    b--;
  if (a == b)
    return 2;
  return (a && b) || (a + b) << 1 || (a & b) == 3;
}
"""


def test_option(tmp_path):
    check_option(tmp_path, "parentheses", P_ASSIGN, HIT, CLEAN, [3], in_wall=True, in_safe=True)


def test_assignment_conditions(tmp_path):
    body = """int f(int a, int b, int c)
{
  int r = 0;
  while (a = c)
    r++;
  do
    r++;
  while (a = b);
  for (; a = b;)
    r++;
  if (a += b)
    r++;
  if (a = b, c)
    r++;
  if (c, a = b)
    r++;
  return r;
}
"""
    proc = run(tmp_path, body, "-Wparentheses")
    assert [l for l, _ in diags(proc, P_ASSIGN)] == [4, 8, 9, 15], proc.stderr


PRECEDENCE = [
    ("a && b || c", "suggest parentheses around '&&' within '||'"),
    ("a || b && c", "suggest parentheses around '&&' within '||'"),
    ("a + b << c", "suggest parentheses around '+' inside '<<'"),
    ("a >> b - c", "suggest parentheses around '-' inside '>>'"),
    ("a | b & c", "suggest parentheses around arithmetic in operand of '|'"),
    ("a ^ b | c", "suggest parentheses around arithmetic in operand of '|'"),
    ("a + b ^ c", "suggest parentheses around arithmetic in operand of '^'"),
    ("a | b == c", "suggest parentheses around comparison in operand of '|'"),
    ("a & b + c", "suggest parentheses around '+' in operand of '&'"),
    ("a & b == c", "suggest parentheses around comparison in operand of '&'"),
    ("a == b == c", "suggest parentheses around comparison in operand of '=='"),
    ("a < b != c", "suggest parentheses around comparison in operand of '!='"),
    ("a < b < c", "comparisons like 'X<=Y<=Z' do not have their mathematical meaning"),
]


@pytest.mark.parametrize("expr,msg", PRECEDENCE, ids=[e for e, _ in PRECEDENCE])
def test_precedence(tmp_path, expr, msg):
    body = "int f(int a, int b, int c)\n{\n  return %s;\n}\n" % expr
    proc = run(tmp_path, body, "-Wparentheses")
    assert [l for l, _ in diags(proc, msg)] == [3], proc.stderr


@pytest.mark.parametrize(
    "expr",
    ["(a && b) || c", "a || (b && c)", "(a + b) << c", "a | (b & c)", "(a & b) == c", "a * b + c",
     "a + b * c", "a && b && c", "a | b | c", "a == (b < c)", "a & b & c", "a - b - c"],
)
def test_precedence_clean(tmp_path, expr):
    body = "int f(int a, int b, int c)\n{\n  return %s;\n}\n" % expr
    assert diags(run(tmp_path, body, "-Wparentheses"), P_ANY) == []
    assert diags(run(tmp_path, body, "-Wparentheses"), P_CMP) == []


def test_not_in_preprocessor_conditions(tmp_path):
    body = "#if 1 && 0 || 1\nint ok;\n#endif\n#if 1 & 2 == 2\nint ok2;\n#endif\n"
    proc = run(tmp_path, body, "-Wparentheses", "-Werror")
    assert proc.returncode == 0, proc.stderr


def test_parentheses_once_at_every_level(tmp_path):
    body = """static inline int g(int a, int b)
{
  if (a = b)
    a++;
  return a && b || a;
}
int x(int a) { return g(a, 1); }
int y(int a) { return g(a, 2); }
"""
    check_levels(tmp_path, "parentheses", P_ANY, body, [3, 5])


def test_parentheses_system_header(tmp_path):
    header = "static inline int g(int a, int b) { if (a = b) a++; return a && b || a; }\n"
    check_system_header(tmp_path, "parentheses", P_ANY, header, "int x(int a) { return g(a, 1); }\n")


# ---------------------------------------------------------------------------
# -Wdangling-else

D_TEXT = "suggest explicit braces to avoid ambiguous 'else'"
D_HIT = """int f(int a, int b)
{
  int r = 0;
  if (a)
    if (b)
      r = 1;
    else
      r = 2;
  return r;
}
"""
D_CLEAN = """int f(int a, int b)
{
  int r = 0;
  if (a)
  {
    if (b)
      r = 1;
    else
      r = 2;
  }
  if (a)
    if (b)
      r = 3;
    else
      r = 4;
  else
    r = 5;
  if (a)
    r = 6;
  else if (b)
    r = 7;
  else
    r = 8;
  return r;
}
"""


def test_dangling_else_option(tmp_path):
    check_option(tmp_path, "dangling-else", D_TEXT, D_HIT, D_CLEAN, [4], in_wall=True, in_safe=True)


def test_dangling_else_through_a_loop(tmp_path):
    body = "int f(int a, int b)\n{\n  if (a)\n    while (b--)\n      if (b)\n        a++;\n      else\n        a--;\n  return a;\n}\n"
    assert [l for l, _ in diags(run(tmp_path, body, "-Wdangling-else"), D_TEXT)] == [3]


def test_dangling_else_once_at_every_level(tmp_path):
    body = """static inline int g(int a, int b)
{
  if (a)
    if (b)
      a++;
    else
      a--;
  return a;
}
int x(int a) { return g(a, 1); }
int y(int a) { return g(a, 2); }
"""
    check_levels(tmp_path, "dangling-else", D_TEXT, body, [3])


# ---------------------------------------------------------------------------
# -Wempty-body

E_TEXT = "suggest braces around empty body"
E_HIT = """int f(int a)
{
  if (a);
  return a;
}
"""
E_CLEAN = """int f(int a)
{
  if (a)
  {
  }
  if (a);
  else
    a++;
  while (a--);
  for (; a < 3; a++);
  return a;
}
"""


def test_empty_body_option(tmp_path):
    check_option(tmp_path, "empty-body", E_TEXT, E_HIT, E_CLEAN, [3], in_wall=False, in_safe=True)


def test_empty_body_else_and_do(tmp_path):
    body = "int f(int a)\n{\n  if (a)\n    a++;\n  else;\n  do ; while (a--);\n  return a;\n}\n"
    proc = run(tmp_path, body, "-Wempty-body")
    assert [l for l, _ in diags(proc, E_TEXT)] == [5, 6], proc.stderr
    assert "'else' statement" in proc.stderr and "'do' statement" in proc.stderr


def test_empty_body_once_at_every_level(tmp_path):
    body = "static inline int g(int a)\n{\n  if (a);\n  return a;\n}\nint x(int a) { return g(a); }\nint y(int a) { return g(a + 1); }\n"
    check_levels(tmp_path, "empty-body", E_TEXT, body, [3])
