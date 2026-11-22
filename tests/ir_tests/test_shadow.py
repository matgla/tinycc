"""-Wshadow (in neither gcc -Wall nor -Wextra; here only when asked for, it is
noisy): a local, a static local or a parameter that hides another local, a
parameter, or a file-scope object or enumerator ("declaration of 'x' shadows
a previous local" / "a parameter" / "a global declaration"), with a note at
the hidden declaration.  A function or a typedef hidden is not reported (as
gcc), nor a prototype's parameters.  The parameters are bound, and checked,
where the function's body starts; their warnings and notes give each one's
own line, as gcc.
"""

import pytest

from stmt_check_common import TCC, check_levels, check_option, check_system_header, diags, run

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

TEXT = "shadows"

HIT = """int f(int x)
{
  int r = x;
  {
    int r = 2;
    x += r;
  }
  return r + x;
}
"""
CLEAN = """typedef int T;
int g(void);
int h(int count);
int f(int x)
{
  int T = x;
  int g = 1;
  {
    int y = 2;
    T += y;
  }
  {
    int y = 3;
    T += y + g;
  }
  return T;
}
"""


def test_option(tmp_path):
    check_option(tmp_path, "shadow", TEXT, HIT, CLEAN, [5], in_wall=False, in_safe=False)


def test_kinds_and_notes(tmp_path):
    body = """int count;
enum { RED };
int f(int count, int x)
{
  int y = x;
  {
    int x = 1;
    int y = 2;
    static int RED;
    for (int y = 0; y < 2; y++)
      x += y;
    return x + y + RED;
  }
}
"""
    proc = run(tmp_path, body, "-Wshadow")
    lines = [l for l in proc.stderr.splitlines() if "t.c:" in l]
    want = [
        ("3", "declaration of 'count' shadows a global declaration"),
        ("1", "note: shadowed declaration is here"),
        ("7", "declaration of 'x' shadows a parameter"),
        ("3", "note: shadowed declaration is here"),
        ("8", "declaration of 'y' shadows a previous local"),
        ("5", "note: shadowed declaration is here"),
        ("9", "declaration of 'RED' shadows a global declaration"),
        ("2", "note: shadowed declaration is here"),
        ("10", "declaration of 'y' shadows a previous local"),
        ("8", "note: shadowed declaration is here"),
    ]
    assert len(lines) == len(want), proc.stderr
    for got, (line, msg) in zip(lines, want):
        assert got.split(":")[1] == line and msg in got, proc.stderr


def test_once_at_every_level(tmp_path):
    body = """static inline int g(int a)
{
  int r = a;
  {
    int a = 2;
    r += a;
  }
  return r;
}
int x(int b) { return g(b); }
int y(int b) { return g(b + 1); }
"""
    check_levels(tmp_path, "shadow", TEXT, body, [5])


def test_system_header(tmp_path):
    header = "static inline int g(int a) { int r = a; { int a = 2; r += a; } return r; }\n"
    check_system_header(tmp_path, "shadow", TEXT, header, "int x(int b) { return g(b); }\n")


def test_parameter_at_its_own_line(tmp_path):
    """a parameter's warning (and a note pointing at it) gives its own line,
    not the body's '{', at every level, with or without -Wunused"""
    body = """int a, b;
static inline int f(int a,
                    int b,
                    int x)
{
  {
    int x = 1;
    return a + b + x;
  }
}
int g(void) { return f(1, 2, 3) + f(4, 5, 6); }
"""
    for flags in ((), ("-Wall", "-Wunused-parameter")):
        check_levels(tmp_path, "shadow", TEXT, body, [2, 3, 7])
        proc = run(tmp_path, body, "-Wshadow", *flags)
        notes = [l.split(":")[1] for l in proc.stderr.splitlines() if "note: shadowed" in l]
        assert notes == ["1", "1", "4"], proc.stderr
