"""-Wimplicit-fallthrough (gcc: -Wextra; here the safe default, not -Wall):
a case or default label reached from a statement above it that can complete.

Not reported: an empty case (labels back to back), a statement that cannot
complete (return, break, continue, goto, a noreturn call, an if/else whose arms
all end so, a for(;;)/while(1) with no break), one that ends in
`[[fallthrough]];` / `[[gnu::fallthrough]];` / `__attribute__((fallthrough));`
(accepted as statements anywhere, also as an if or else arm), or one followed
by a fall-through comment gcc's -Wimplicit-fallthrough=3 accepts (`/* fall
through */`, `// FALLTHRU`, `/* Falls through. */`, `/* fallthrough - why */`,
`/* lint -fallthrough */`, ...) right before the label.
"""

import pytest

from stmt_check_common import TCC, check_levels, check_option, check_system_header, diags, run

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

TEXT = "this statement may fall through"

HIT = """int f(int x)
{
  int r = 0;
  switch (x)
  {
  case 1:
    r = 1;
  case 2:
    r += 2;
    break;
  }
  return r;
}
"""

CLEAN = """void die(void) __attribute__((noreturn));
int f(int x, int y)
{
  int r = 0;
  switch (x)
  {
  case 0:
  case 1:
    r = 1;
    break;
  case 2:
    if (y)
      return 1;
    else
      return 2;
  case 3:
    die();
  case 4:
    r = 4;
    __attribute__((fallthrough));
  case 5:
    r += 5;
    [[fallthrough]];
  case 6:
    r += 6;
    [[gnu::fallthrough]];
  case 7:
    if (r)
      return r;
    else
      __attribute__((fallthrough));
  case 8:
    for (;;)
      r++;
  case 9:
    {
      r = 9;
      goto out;
    }
  case 10:
    while (1)
      r++;
  default:
    r = 0;
  }
out:
  return r;
}
"""


def test_option(tmp_path):
    check_option(tmp_path, "implicit-fallthrough", TEXT, HIT, CLEAN, [7], in_wall=False, in_safe=True)


def test_note_points_at_the_label(tmp_path):
    proc = run(tmp_path, HIT, "-Wimplicit-fallthrough")
    assert "t.c:8: note: here" in proc.stderr, proc.stderr


def test_what_may_fall_through(tmp_path):
    body = """int f(int x, int y)
{
  int r = 0;
  switch (x)
  {
  case 1:
    if (y)
      r = 1;
  case 2:
    if (y)
      return 1;
    else
      r = 2;
  case 3:
    {
      r = 3;
    }
  case 4:
    while (y)
      r++;
  case 5:
    if (y)
      return 5;
    else
      [[fallthrough]];
  case 6:
    return r;
  }
  return r;
}
"""
    proc = run(tmp_path, body, "-Wimplicit-fallthrough")
    assert [l for l, _ in diags(proc, TEXT)] == [7, 10, 16, 19], proc.stderr


@pytest.mark.parametrize(
    "comment",
    [
        "/* fall through */",
        "/* fallthrough */",
        "/* FALLTHRU */",
        "// FALLTHROUGH",
        "/* Falls through. */",
        "/* Fall-through */",
        "// falls through",
        "/* else fall through */",
        "/* fallthrough - to the next */",
        "/* lint -fallthrough */",
        "/* -fallthrough */",
        "/* @fallthrough@ */",
        "/* intentional fallthrough */",
    ],
)
def test_fallthrough_comment(tmp_path, comment):
    body = "int f(int x)\n{\n  int r = 0;\n  switch (x)\n  {\n  case 1:\n    r = 1;\n    %s\n  case 2:\n    r++;\n  }\n  return r;\n}\n" % comment
    assert diags(run(tmp_path, body, "-Wimplicit-fallthrough"), TEXT) == []
    # on the case's line too
    body = "int f(int x)\n{\n  int r = 0;\n  switch (x)\n  {\n  case 1:\n    r = 1; %s case 2:\n    r++;\n  }\n  return r;\n}\n" % comment
    assert diags(run(tmp_path, body, "-Wimplicit-fallthrough"), TEXT) == []


@pytest.mark.parametrize("comment", ["/* not a fall-through comment */", "/* fall through to the end */", "/* FallThrough */"])
def test_other_comments_do_not_count(tmp_path, comment):
    body = "int f(int x)\n{\n  int r = 0;\n  switch (x)\n  {\n  case 1:\n    r = 1;\n    %s\n  default:\n    r++;\n  }\n  return r;\n}\n" % comment
    assert [l for l, _ in diags(run(tmp_path, body, "-Wimplicit-fallthrough"), TEXT)] == [7]


def test_comment_not_right_before_the_label(tmp_path):
    body = "int f(int x)\n{\n  int r = 0;\n  switch (x)\n  {\n  case 1:\n    /* fall through */\n    r = 1;\n  case 2:\n    r++;\n  }\n  return r;\n}\n"
    assert [l for l, _ in diags(run(tmp_path, body, "-Wimplicit-fallthrough"), TEXT)] == [8]


def test_attribute_statement_accepted_everywhere(tmp_path):
    # an if arm takes the attribute statement (it used to be a syntax error)
    body = "int f(int x)\n{\n  if (x)\n    [[fallthrough]];\n  else\n    __attribute__((fallthrough));\n  return x;\n}\n"
    proc = run(tmp_path, body)
    assert proc.returncode == 0, proc.stderr
    proc = run(tmp_path, "#if __has_attribute(fallthrough)\nint ok;\n#else\n#error no\n#endif\n")
    assert proc.returncode == 0, proc.stderr


def test_once_at_every_level(tmp_path):
    body = """static inline int g(int x)
{
  int r = 0;
  switch (x)
  {
  case 1:
    r = 1;
  case 2:
    r++;
  }
  return r;
}
int a(int x) { return g(x); }
int b(int x) { return g(x + 1); }
"""
    check_levels(tmp_path, "implicit-fallthrough", TEXT, body, [7])


def test_system_header(tmp_path):
    header = "static inline int g(int x) { int r = 0; switch (x) { case 1: r = 1; case 2: r++; } return r; }\n"
    check_system_header(tmp_path, "implicit-fallthrough", TEXT, header, "int a(int x) { return g(x); }\n")


@pytest.mark.parametrize("other", ["/* see below */", "// more", "/* a */ /* b */", "// one\n    // two"])
def test_comment_then_other_comments(tmp_path, other):
    """other comments between the fall-through comment and the label, as gcc"""
    body = ("int f(int x)\n{\n  int r = 0;\n  switch (x)\n  {\n  case 1:\n    r = 1;\n    /* fall through */\n"
            "    %s\n  case 2:\n    r++;\n  }\n  return r;\n}\n" % other)
    assert diags(run(tmp_path, body, "-Wimplicit-fallthrough"), TEXT) == []
    body = body.replace("/* fall through */", "/* nothing */")
    assert [l for l, _ in diags(run(tmp_path, body, "-Wimplicit-fallthrough"), TEXT)] == [7]


def _straddling(offset, comment):
    """a source with COMMENT starting at byte OFFSET, then the label"""
    head = "int f(int x)\n{\n  int r = 0;\n  switch (x)\n  {\n  case 1:\n    r++;\n"
    need = offset - len(head) - 4
    pad = ""
    while need - len(pad) > 80:
        pad += "    //" + "x" * 70 + "\n"
    pad += " " * (need - len(pad))
    return head + pad + "    " + comment + "\n  case 2:\n    r += 2;\n  }\n  return r;\n}\n"


@pytest.mark.parametrize("io_buf", [2048, 8192])
def test_comment_across_the_read_buffer(tmp_path, io_buf):
    """a fall-through comment, the blanks after it or the label read across an
    I/O buffer refill (8 KB on the host, 2 KB on the device): still seen"""
    for comment in ("/* fall through */", "// fall through", "/* Falls through. */"):
        for off in range(io_buf - 24, io_buf + 4):
            body = _straddling(off, comment)
            assert diags(run(tmp_path, body, "-Wimplicit-fallthrough"), TEXT) == [], (comment, off)
    for off in range(8192 - 24, 8192 + 4, 3):
        body = _straddling(off, "/* not one */")
        assert len(diags(run(tmp_path, body, "-Wimplicit-fallthrough"), TEXT)) == 1, off


PLACE = "attribute 'fallthrough' not preceding a case label or default label"


def test_attribute_not_before_a_label(tmp_path):
    """gcc's unconditional warning: a fall-through statement followed by a
    statement, or by the end of the switch body (only -w silences it)"""
    body = """int f(int x, int y)
{
  int r = 0;
  switch (x)
  {
  case 1:
    r++;
    __attribute__((fallthrough));
  case 2:
    r++;
    [[fallthrough]];
    r++;
  case 3:
    if (y)
    {
      r++;
      [[fallthrough]];
    }
  case 4:
    if (y)
      [[fallthrough]];
    else
      r++;
  case 5:
    r++;
    [[fallthrough]];
  }
  return r;
}
"""
    for lvl in ("-O0", "-O2"):
        proc = run(tmp_path, body, "-std=c23", lvl)
        assert proc.returncode == 0, proc.stderr
        assert [l for l, _ in diags(proc, PLACE)] == [11, 26], proc.stderr
    assert diags(run(tmp_path, body, "-std=c23", "-w"), PLACE) == []
