"""-Wlogical-not-parentheses (gcc -Wall, so -Wall and the safe default here):
`!a == b` -- a `!` not in parentheses on the left of a comparison whose right
side is not a truth value (a comparison, `&&`, `||`, `!`, a _Bool) nor, for
`==`/`!=`, the constant 0 -- with gcc's note.  -Wparentheses gives gcc's
`!a & b` / `!a | b` (the right side not a truth value nor 0 or 1).  Both
match gcc 16 line for line on the cases below.
"""

import pytest

from stmt_check_common import TCC, check_levels, check_option, check_system_header, diags, run

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

TEXT = "logical not is only applied to the left hand side of comparison"
BITS = "suggest parentheses around operand of '!'"

HIT = """int f(int a, int b)
{
  return !a == b;
}
"""
CLEAN = """int f(int a, int b)
{
  return (!a) == b || !(a == b) || !a == 0;
}
"""


def test_option(tmp_path):
    check_option(tmp_path, "logical-not-parentheses", TEXT, HIT, CLEAN, [3], in_wall=True, in_safe=True)


def test_cases_as_gcc(tmp_path):
    body = """int f(int a, int b, _Bool c, unsigned m)
{
  int r = 0;
  if (!a == b) r++;
  if (!a != b) r++;
  if (!a == 0) r++;
  if (!a == 1) r++;
  if (!a == c) r++;
  if (!a == !b) r++;
  if (!a < b) r++;
  if ((!a) == b) r++;
  if (!a == (b > 1)) r++;
  if (!(a) == b) r++;
  if (!a & m) r++;
  if (!a | m) r++;
  r += !a & 1;
  r += !a & (m > 1);
  r += !a ^ m;
  r += a == !b;
  return r;
}
"""
    proc = run(tmp_path, body, "-Wall")
    assert [l for l, _ in diags(proc, TEXT)] == [4, 5, 7, 10, 13], proc.stderr
    assert [l for l, _ in diags(proc, BITS)] == [14, 15], proc.stderr
    assert proc.stderr.count("note: add parentheses around left hand side expression") == 5, proc.stderr


def test_bitwise_is_parentheses(tmp_path):
    body = "int f(int a, unsigned m)\n{\n  return !a & m;\n}\n"
    assert [l for l, _ in diags(run(tmp_path, body, "-Wparentheses"), BITS)] == [3]
    assert diags(run(tmp_path, body, "-Wall", "-Wno-parentheses"), BITS) == []
    assert diags(run(tmp_path, body, "-Wall", "-Wno-logical-not-parentheses"), BITS) != []


def test_not_in_preprocessor_conditions(tmp_path):
    body = "#if !1 == 2\nint x;\n#endif\nint y;\n"
    proc = run(tmp_path, body, "-Wall", "-Werror")
    assert proc.returncode == 0, proc.stderr


def test_once_at_every_level(tmp_path):
    body = """static inline int g(int a, int b)
{
  return !a == b;
}
int x(int a) { return g(a, 1); }
int y(int a) { return g(a, 2); }
"""
    check_levels(tmp_path, "logical-not-parentheses", TEXT, body, [3])


def test_system_header(tmp_path):
    header = "static inline int g(int a, int b) { return !a == b; }\n"
    check_system_header(tmp_path, "logical-not-parentheses", TEXT, header, "int x(int a) { return g(a, 1); }\n")
