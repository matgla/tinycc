"""-Wswitch (gcc -Wall, so -Wall and the safe default here): a switch on a
value of an enum type that has no default and misses an enumerator
("enumeration value 'X' not handled in switch", one per value: enumerators of
equal value count once, a GNU case range covers what it spans), and a case
value no enumerator has ("case value 'N' not in enumerated type"), with or
without a default.  The first is reported at the switch, the second at the
case.  A switch on anything else -- `(int)e` too -- is not checked.
"""

import pytest

from stmt_check_common import TCC, check_levels, check_option, check_system_header, diags, run

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

MISSING = "not handled in switch"
NOT_IN = "not in enumerated type"

HIT = """enum color { RED, GREEN, BLUE };
int f(enum color c)
{
  switch (c)
  {
  case RED:
    return 1;
  case GREEN:
    return 2;
  }
  return 0;
}
"""
CLEAN = """enum color { RED, GREEN, BLUE, CRIMSON = 0 };
typedef enum color color_t;
int f(color_t c, int i)
{
  switch (c)
  {
  case RED:
  case GREEN:
  case BLUE:
    break;
  }
  switch (c)
  {
  case RED:
    return 1;
  default:
    return 0;
  }
  switch ((int)c)
  {
  case 7:
    return 7;
  }
  switch (i)
  {
  case 1:
    return 1;
  }
  switch (c)
  {
  case RED ... BLUE:
    return 3;
  }
}
"""


def test_option(tmp_path):
    check_option(tmp_path, "switch", MISSING, HIT, CLEAN, [4], in_wall=True, in_safe=True)


def test_messages(tmp_path):
    proc = run(tmp_path, HIT, "-Wswitch")
    assert "t.c:4: warning: enumeration value 'BLUE' not handled in switch" in proc.stderr, proc.stderr


def test_each_missing_value_once(tmp_path):
    body = """enum e { A, B, C, D, ALIAS_C = 2 };
int f(enum e v)
{
  switch (v)
  {
  case A:
    return 1;
  }
  return 0;
}
"""
    proc = run(tmp_path, body, "-Wswitch")
    names = [line.split("'")[1] for line in proc.stderr.splitlines() if MISSING in line]
    assert names == ["B", "C", "D"], proc.stderr


def test_case_value_not_in_enum(tmp_path):
    body = """enum e { A, B };
int f(enum e v)
{
  switch (v)
  {
  case A:
  case B:
    return 1;
  case 5:
    return 5;
  default:
    return 0;
  }
}
"""
    proc = run(tmp_path, body, "-Wswitch")
    assert [l for l, _ in diags(proc, NOT_IN)] == [9], proc.stderr
    assert "case value '5' not in enumerated type 'enum e'" in proc.stderr


def test_once_at_every_level(tmp_path):
    body = """enum e { A, B };
static inline int g(enum e v)
{
  switch (v)
  {
  case A:
    return 1;
  }
  return 0;
}
int x(void) { return g(A); }
int y(void) { return g(B); }
"""
    check_levels(tmp_path, "switch", MISSING, body, [4])


def test_system_header(tmp_path):
    header = "enum e { A, B };\nstatic inline int g(enum e v) { switch (v) { case A: return 1; } return 0; }\n"
    check_system_header(tmp_path, "switch", MISSING, header, "int x(void) { return g(A); }\n")
