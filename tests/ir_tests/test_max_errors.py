"""Error recovery and -fmax-errors=N (frontend/gen/core/recover.c).

An error no longer ends the compile: the parser goes on at the next external
declaration (file scope: to the declaration's ';', or past a function body)
or the next statement (in a body: to the statement's ';' or closing brace;
the rest of the body is checked without code), up to -fmax-errors=N errors
(8 by default, 0 = no limit), then "compilation terminated due to
-fmax-errors=N." as gcc.  The compile still fails: exit status 1, no object,
no link.  A warning made an error (-Werror, the safe default's errors) and
"plain pointer in yasc" do not stop the parse at all; #error does not either
(gcc).  An undeclared name is reported once per function.  A preprocessor
directive's error still stops (gcc too).  Since 2026-11-23 the diagnostics
are -O0's, in -O0's order, at every level and with -fsyntax-only.
"""

import re
import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

TERMINATED = "compilation terminated due to -fmax-errors="


def _run(tmp_path, body, *flags, ccompat=True, name="t.c", out="t.o", compile_only=True, timeout=30):
    src = tmp_path / name
    src.write_text(body)
    cmd = [str(TCC)] + (["-ccompat"] if ccompat else []) + (["-c"] if compile_only else [])
    cmd += [*flags, str(src), "-o", str(tmp_path / out)]
    return subprocess.run(cmd, capture_output=True, text=True, cwd=tmp_path, timeout=timeout)


def _diags(proc, kind=None):
    """(line, kind, message) of every warning and error, in order."""
    out = []
    for line in proc.stderr.splitlines():
        parts = line.split(":", 3)
        if len(parts) == 4 and parts[1].isdigit() and parts[2].strip() in ("warning", "error"):
            if kind is None or parts[2].strip() == kind:
                out.append((int(parts[1]), parts[2].strip(), parts[3].strip()))
    return out


def _error_lines(proc):
    return [l for l, _, _ in _diags(proc, "error")]


SEVERAL = """\
int g1 = undeclared_a;
int ok1(void) { return 1; }
struct S { int a; };
int f(int x)
{
  int y = x;
  y = bogus + 1;
  if (x) {
    y = 3 +;
    y++;
  } else {
    y = nothere;
  }
  for (int i = 0; i < x; i++) {
    struct S s;
    s.b = i;
  }
  return y;
}
int h(void) { return f(2) + ; }
int k(void) { return zz; }
"""


def test_several_errors_all_reported(tmp_path):
    p = _run(tmp_path, SEVERAL)
    assert p.returncode == 1
    assert _error_lines(p) == [1, 7, 9, 12, 16, 20, 21], p.stderr
    assert not (tmp_path / "t.o").exists()
    assert TERMINATED not in p.stderr


@pytest.mark.parametrize("opt", ["-O1", "-O2", "-Os"])
def test_same_at_every_level(tmp_path, opt):
    p0 = _run(tmp_path, SEVERAL, "-O0", "-Wall")
    p = _run(tmp_path, SEVERAL, opt, "-Wall")
    assert p.stderr == p0.stderr
    assert p.returncode == p0.returncode == 1


def test_syntax_only(tmp_path):
    p0 = _run(tmp_path, SEVERAL, "-Wall")
    p = _run(tmp_path, SEVERAL, "-Wall", "-fsyntax-only")
    assert p.stderr == p0.stderr
    assert p.returncode == 1


def _many(n):
    return "".join(f"int f{i}(void) {{ return a{i}; }}\n" for i in range(1, n + 1))


def test_cap_default_8(tmp_path):
    p = _run(tmp_path, _many(12))
    assert _error_lines(p) == list(range(1, 9))
    assert p.stderr.rstrip().endswith(TERMINATED + "8.")
    assert p.returncode == 1


@pytest.mark.parametrize("n,expect", [("1", 1), ("3", 3), ("0", 12), ("20", 12)])
def test_cap(tmp_path, n, expect):
    p = _run(tmp_path, _many(12), f"-fmax-errors={n}")
    assert len(_error_lines(p)) == expect
    assert (TERMINATED + n + ".") in p.stderr if expect < 12 else TERMINATED not in p.stderr
    assert p.returncode == 1


def test_cap_bad_argument(tmp_path):
    p = _run(tmp_path, "int x;\n", "-fmax-errors=x")
    assert p.returncode == 1 and "-fmax-errors=" in p.stderr


def test_cap_counts_errors_that_do_not_stop(tmp_path):
    # -Werror makes warnings errors: they count against the cap too
    body = "".join(f"int f{i}(void) {{ int u{i}; return 0; }}\n" for i in range(1, 6))
    p = _run(tmp_path, body, "-Wall", "-Werror", "-fmax-errors=3")
    assert len(_error_lines(p)) == 3 and TERMINATED + "3." in p.stderr


def test_no_link_with_errors(tmp_path):
    p = _run(tmp_path, "int main(void) { return x; }\nint g(void) { return y; }\n", compile_only=False, out="t.elf")
    assert p.returncode == 1
    assert _error_lines(p) == [1, 2]
    assert not (tmp_path / "t.elf").exists()


def test_werror_goes_on(tmp_path):
    # a warning made an error does not stop: each one is reported, the
    # compile fails
    body = "int f(void)\n{\n  int a;\n  return 0;\n}\nint g(void)\n{\n  int b;\n  return 0;\n}\n"
    p = _run(tmp_path, body, "-Wall", "-Werror")
    assert [(l, k) for l, k, _ in _diags(p)] == [(3, "error"), (8, "error")]
    assert p.returncode == 1 and not (tmp_path / "t.o").exists()


def test_hash_error_goes_on(tmp_path):
    p = _run(tmp_path, "#error first\nint f(void) { return x; }\n#error second\n")
    assert [(l, m) for l, _, m in _diags(p)] == [(1, "#error first"), (2, "'x' undeclared"), (3, "#error second")]
    assert p.returncode == 1


def test_missing_include_stops(tmp_path):
    p = _run(tmp_path, "int f(void) { return x; }\n#include \"nope_not_here.h\"\nint g(void) { return y; }\n")
    assert _error_lines(p) == [1, 2]
    assert p.returncode == 1


def test_undeclared_once_per_function(tmp_path):
    body = (
        "int f(void)\n{\n  int r = q;\n  r += q;\n  r += q * 2;\n  return r + q;\n}\n"
        "int g(void)\n{\n  return q;\n}\n"
    )
    p = _run(tmp_path, body)
    assert [(l, m) for l, _, m in _diags(p)] == [(3, "'q' undeclared"), (10, "'q' undeclared")]


def test_statement_recovery_keeps_locals_and_checks(tmp_path):
    # the statements after an error are still checked, the locals declared
    # before it still known, and a label, break, continue or case after it
    # is no new error
    body = """\
int f(int x)
{
  int y = 0;
  switch (x) {
  case 1:
    y = 1 +;
    break;
  case 2:
    y = 2;
    break;
  default:
    goto out;
  }
  while (x--) {
    y = );
    if (y)
      continue;
    break;
  }
out:
  return y + undef2;
}
"""
    p = _run(tmp_path, body, "-Wall")
    assert _error_lines(p) == [6, 15, 21], p.stderr


def test_error_in_if_then_else(tmp_path):
    body = "int f(int x)\n{\n  int y;\n  if (x) {\n    y = ;\n  } else {\n    y = bad;\n  }\n  return y;\n}\n"
    p = _run(tmp_path, body)
    assert _error_lines(p) == [5, 7], p.stderr


def test_error_in_statement_expression_and_struct(tmp_path):
    body = """\
int f(int x)
{
  int y = ({ int t = x +; t; }) + 1;
  struct L { int a; int b c; int d; } l;
  l.a = y;
  return l.a + nope;
}
struct G { int a; unknown_t b; int c; } g;
int h(void) { return g.c + nope2; }
"""
    p = _run(tmp_path, body)
    # a struct body goes on with the next member after an error: 'l' and 'g'
    # are declared with the members that were right (gcc), so only 'nope'
    # and 'nope2' are undeclared
    assert _error_lines(p) == [3, 4, 6, 8, 9], p.stderr


DECLARATOR = """\
int g[ ;
typedef int T[ ;
int p(unk_t a);
int f(int x)
{
  int y[ ;
  y[0] = x;
  int q(unk_t a);
  T t;
  {
    int w[ ;
    w[0] = 1;
  }
  return y[1] + g[0] + q(1) + t[0] + w + p(2);
}
int h(void) { return g[2]; }
"""


@pytest.mark.parametrize("opt", ["-O0", "-O2", "-fsyntax-only"])
def test_error_in_declarator_declares_the_name(tmp_path, opt):
    # `int y[ ;` still declares y (gcc declares what it can): an array when
    # '[' followed the name, an old-style function when '(' did, a typedef
    # stays a typedef -- at file scope and in a body, at the scope it was
    # in (w is undeclared after its block)
    p = _run(tmp_path, DECLARATOR, opt, "-fmax-errors=0", "-Wall")
    d = _diags(p)
    assert [l for l, k, _ in d if k == "error"] == [1, 2, 3, 6, 8, 11, 14], p.stderr
    assert (14, "error", "'w' undeclared") in d, p.stderr
    assert not [m for _, _, m in d if "undeclared" in m and "'w'" not in m], p.stderr
    assert not [m for _, k, m in d if k == "warning"], p.stderr  # no unused y, t, w
    assert p.returncode == 1


def test_error_in_declarator_same_at_every_level(tmp_path):
    outs = {_run(tmp_path, DECLARATOR, o, "-fmax-errors=0").stderr for o in ("-O0", "-O1", "-O2", "-Os")}
    assert len(outs) == 1, outs


def test_error_in_declarator_keeps_an_earlier_declaration(tmp_path):
    # a name declared before keeps its type: the declarator with the error
    # declares nothing new
    body = "extern int g[4];\nint g[ ;\nint f(void) { return g[3]; }\n"
    p = _run(tmp_path, body)
    assert _error_lines(p) == [2], p.stderr


INLINED = """\
static int a(int x) { return x + nope1; }
int b(void) { return a(1) + nope2; }
static int c(int x) { return x * nope3; }
int d(void) { return nope4 + c(2); }
static int unused(void) { return nope5; }
int e(void) { return nope6; }
"""


@pytest.mark.parametrize("opt", ["-O1", "-O2", "-Os"])
def test_error_in_inline_expansion_goes_on_after_the_call(tmp_path, opt):
    # an error in an inlined body is the callee's: the caller goes on after
    # the call (nope2), and a static whose calls generated nothing after the
    # first error is still parsed (nope3) -- the errors -O0 gives
    p0 = _run(tmp_path, INLINED, "-O0")
    p = _run(tmp_path, INLINED, opt)
    assert _error_lines(p0) == [1, 2, 3, 4, 5, 6], p0.stderr
    assert sorted(_error_lines(p)) == [1, 2, 3, 4, 5, 6], p.stderr


STATIC_INIT = """\
static int tab[] = { 1, 2 + , 3 };
static int tab2[3] = { 1, nope, 3 };
int f(void) { return tab[0] + tab2[1] + bad; }
static int unused_tab[3] = { 1, +, 3 };
int g(void) { return zz; }
"""


@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_error_in_static_initializer_goes_on(tmp_path, opt):
    # an unsized array's initializer is parsed twice from a copy, a static's
    # is saved and replayed at the end of the unit (-O1+): an error in
    # either goes on with what follows
    p = _run(tmp_path, STATIC_INIT, opt)
    assert sorted(_error_lines(p)) == [1, 2, 3, 4, 5], p.stderr


DEFER = """\
int g;
int f(int x)
{
  int a = 1;
  defer { g = a + nope; }
  int a2 = 2;
  return a2 + bad;
}
int k(int x)
{
  int b = 1;
  defer { g = b + nope3; }
  int b2 = 2;
  if (x) return b2;
  g = b2 + bad2;
  return b2;
}
int h(void) { return zz; }
"""


@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_error_in_defer_body(tmp_path, opt):
    # f: the defer never ran before the error (its exits come after it): it
    # is checked once all the same.  k: an error in a copy ends that copy;
    # the names it hid (b2) are back after it
    p = _run(tmp_path, DEFER, opt, ccompat=False)
    d = _diags(p, "error")
    assert [l for l, _, _ in d] == [7, 5, 12, 15, 18], p.stderr
    assert not [m for _, _, m in d if "'b2'" in m or "'a2'" in m], p.stderr


# a body saved as tokens (-O1+) stops where the lexer fails or the input
# ends: the saved part is parsed at once, so the errors are -O0's, in order
CUT_BODIES = {
    "no_brace_after_error": (
        "static int a(int x)\n{\n  if (x) {\n    x = nope1;\n  return x;\n}\n\nint b(void)\n{\n  return bad;\n}\n",
        [(4, "'nope1' undeclared"), (12, "expected declaration or statement at end of input")],
    ),
    "no_brace": (
        "static int a(int x)\n{\n  if (x) {\n    x = 1;\n  return x;\n}\nint b(void) { return 2; }\n",
        [(8, "expression expected before '<eof>'")],
    ),
    "string_after_error": (
        "static int a(int x)\n{\n  int y = nope1;\n  const char *s = \"abc;\n  return x + y + nope2;\n}\n"
        "int b(void) { return bad; }\n",
        [(3, "'nope1' undeclared"), (4, "missing terminating \" character")],
    ),
    "string": (
        "int a(int x)\n{\n  int y = 1;\n  const char *s = \"abc;\n  return x + y;\n}\n",
        [(4, "missing terminating \" character")],
    ),
    "comment": ("int a(int x)\n{\n  x++; /* open\n  return x;\n}\n", [(6, "unexpected end of file in comment")]),
}


@pytest.mark.parametrize("case", sorted(CUT_BODIES))
def test_cut_body_same_at_every_level(tmp_path, case):
    body, expect = CUT_BODIES[case]
    outs = {}
    for opt in ("-O0", "-O1", "-O2", "-Os", "-fsyntax-only"):
        p = _run(tmp_path, body, opt)
        assert p.returncode == 1
        # an ASan build reports what the cut leaks: the deferred body's entry
        # (defer_function_body) and the lexer's message (save_body_raise)
        assert "Sanitizer" not in p.stderr, (opt, p.stderr)
        outs[opt] = p.stderr
        assert [(l, m) for l, _, m in _diags(p)] == expect, (opt, p.stderr)
    assert len(set(outs.values())) == 1, outs


@pytest.mark.parametrize("opt", ["-O0", "-O2", "-fsyntax-only"])
def test_recovery_past_a_helper_symbol(tmp_path, opt):
    # __builtin___memcpy_chk names memcpy as a helper (VT_ASM_FUNC, which has
    # an enum constant's bits and no ref): the file point's walk over the
    # prototype's parameters reached it and crashed in sym_scope
    body = "void f(char *d, const char *s) { __builtin___memcpy_chk(d, s, 4, 8); }\nint g(int a, bad_t b);\nint h(void) { return nope; }\n"
    p = _run(tmp_path, body, opt)
    assert p.returncode == 1, p.stderr
    assert _error_lines(p) == [2, 3], p.stderr


PROTOS = """\
void *cp(char *d, const char *s) { return __builtin___memcpy_chk(d, s, 4, 8); }
extern int vs1(char *dst, const char *fmt, va_list ap);
extern int vs2(char *dst, unsigned n, const char *fmt, va_list ap);
extern int vs3(va_list ap, int n);
int use(void) { return vs1(0, 0, 0) + nope; }
"""


@pytest.mark.parametrize("opt", ["-O0", "-O2", "-fsyntax-only"])
def test_prototypes_with_invalid_parameter_types(tmp_path, opt):
    # scratch_stratchk.c's shape: prototypes naming an undefined va_list
    # after a helper symbol is on the global stack: the walk over the named
    # parameters reached it and crashed (fd6bb2a565 at -O0 and -fsyntax-only)
    p = _run(tmp_path, PROTOS, opt)
    assert p.returncode == 1, p.stderr
    assert _error_lines(p) == [2, 3, 4, 5], p.stderr


INIT_ERRORS = """\
int f(void)
{
  int a[2] = {1, 2, 3}, b[1] = {{1, 2, 3}};
  int c[2][2] = {{1, 2, 3}, {4}};
  return a[0] + c[0][0];
}
int g(void) { return nope; }
"""


@pytest.mark.parametrize("opt", ["-O0", "-O2", "-fsyntax-only"])
def test_error_in_initializer_braces(tmp_path, opt):
    # the skip leaves the initializer's braces the error was in without
    # taking their '}' for the end of a block (the body went on to the end
    # of the file, and g was never seen); b, after a's error, is still read
    p = _run(tmp_path, INIT_ERRORS, opt)
    assert _error_lines(p) == [3, 3, 4, 7], p.stderr


def test_unused_after_skip(tmp_path):
    # names in the skipped part of a statement count as used: no
    # "unused variable" for them
    body = "static int helper(void) { return 1; }\nint f(void)\n{\n  int a = 1;\n  return bad + a + helper();\n}\n"
    p = _run(tmp_path, body, "-Wall")
    assert [(l, k) for l, k, _ in _diags(p)] == [(5, "error")], p.stderr


def test_labels_in_skipped_part(tmp_path):
    body = "int f(int x)\n{\n  if (x)\n    goto done;\n  x = bad; done: x++;\n  return x;\n}\n"
    p = _run(tmp_path, body)
    assert _error_lines(p) == [5], p.stderr


def test_static_inline_error_once(tmp_path):
    # at -O2 the body is replayed at its call before it is compiled: the
    # error is said once, as at -O0
    body = "static inline int sq(int v) { return v * nope; }\nint f(void) { return sq(2); }\nint g(void) { return sq(3) + other; }\n"
    for opt in ("-O0", "-O2"):
        p = _run(tmp_path, body, opt)
        assert sorted(_error_lines(p)) == [1, 3], (opt, p.stderr)


# yasc (the safe default): a plain pointer is an error that does not stop the
# parse -- the checks after it still speak
YASC = """\
int proto(char *s);
struct node { int *next; int v; };
int use(int *p, int *q)
{
  char *loc[1] = {nullptr};
  int x;
  (void)loc;
  (void)q;
  return x + *p;
}
static int unused_fn(void) { return 0; }
int main(void) { int v = 1; return use(&v, &v) + undecl; }
"""


def test_yasc_plain_pointer_goes_on(tmp_path):
    p = _run(tmp_path, YASC, ccompat=False)
    d = [(l, k, m) for l, k, m in _diags(p)]
    plain = [l for l, k, m in d if "plain pointer in yasc" in m]
    # a parameter's own pointer is a view (lines 1, 3), and so is a local
    # pointer its function does not own; a member (line 2) and an array of
    # pointers (loc, line 5, said at the end of its body: decl/plain_param.c)
    # are plain
    assert plain == [2, 5], p.stderr
    assert (9, "error", "'x' is used uninitialized") in d
    assert (11, "warning", "'unused_fn' defined but not used") in d
    assert (12, "error", "'undecl' undeclared") in d
    # nothing more about the plain pointers themselves
    assert not [m for _, _, m in d if "maybe" in m or "null" in m], p.stderr
    assert p.returncode == 1


def test_yasc_same_at_every_level(tmp_path):
    p0 = _run(tmp_path, YASC, "-O0", ccompat=False)
    p2 = _run(tmp_path, YASC, "-O2", ccompat=False)
    ps = _run(tmp_path, YASC, "-fsyntax-only", ccompat=False)
    assert p0.stderr == p2.stderr == ps.stderr


@pytest.mark.parametrize(
    "body",
    [
        "int f(void) {\n  int x = 1;\n  x = ;\n",
        "int f(void) {\n  { { int x = ",
        "struct S { int a;\n",
        "int f(void) { return 1 +",
        "int a = {1, 2,",
        "int f(void) { switch (1) { case 1: x",
        "int f(int a) { if (a) { a = ; } else",
        "}}}}\n",
        ")))\n;;;\n",
    ],
)
def test_no_hang_at_eof(tmp_path, body):
    p = _run(tmp_path, body, timeout=10)
    assert p.returncode == 1
    assert 1 <= len(_error_lines(p)) <= 8


def test_local_label_after_error(tmp_path):
    # `out:` is no label in yasc (`out` is a keyword): the __label__ the
    # nested function jumps to is then never defined -- one error, and the
    # compile ends (it went round closing the label's block)
    body = (
        "int g[4];\nint f(void)\n{\n  __label__ out;\n  g[0] = 1;\n  void jump(void) { goto out; }\n"
        "  jump();\n  return 0;\nout:\n  return 1;\n}\nint h(void) { return 2; }\n"
    )
    for opt in ("-O0", "-O2"):
        p = _run(tmp_path, body, opt, ccompat=False, timeout=20)
        assert p.returncode == 1
        assert _error_lines(p) == [9], (opt, p.stderr)


SWEEP = """\
struct P { int x, y; };
static int sq(int v) { return v * v; }
int f(int n, struct P *p)
{
  int s = 0;
  for (int i = 0; i < n; i++) {
    if (i & 1)
      s += sq(i);
    else {
      switch (i) { case 2: s--; break; default: s++; }
    }
  }
  while (n--) s += p->x;
  return ({ int t = s; t + p->y; });
}
int g[3] = { 1, 2, 3 };
"""


def test_token_deletion_sweep(tmp_path):
    # every token removed in turn: the compile ends (no hang, no crash) with
    # at most the cap of errors
    toks = re.findall(r"[A-Za-z_]\w*|\d+|->|\+\+|--|\+=|&&|[^\s\w]", SWEEP)
    spans = []
    pos = 0
    for t in toks:
        i = SWEEP.index(t, pos)
        spans.append((i, i + len(t)))
        pos = i + len(t)
    for k, (a, b) in enumerate(spans):
        body = SWEEP[:a] + " " + SWEEP[b:]
        p = _run(tmp_path, body, "-O2" if k & 1 else "-O0", timeout=20)
        assert p.returncode in (0, 1), (k, body, p.returncode, p.stderr)
        assert "compiler_error" not in p.stderr, (k, body, p.stderr)
        assert len(_error_lines(p)) <= 8


# --- the recovery limits of 2026-11-23 --------------------------------------

LEVELS = ("-O0", "-O1", "-O2", "-Os", "-fsyntax-only")


def _same_at_every_level(tmp_path, body, *flags, levels=LEVELS, ccompat=True):
    """The diagnostics (stderr) of BODY at every level: all the same; the -O0 run."""
    outs = {}
    for opt in levels:
        p = _run(tmp_path, body, opt, *flags, ccompat=ccompat, timeout=30)
        assert p.returncode in (0, 1), (opt, p.returncode, p.stderr)
        outs[opt] = p
    first = outs[levels[0]]
    for opt in levels[1:]:
        assert outs[opt].stderr == first.stderr, (opt, first.stderr, outs[opt].stderr)
        assert outs[opt].returncode == first.returncode
    return first


DECL_LIST = """\
int f(int x)
{
  int a[2] = {1, 2, 3}, b = 4;
  int c = f(1, +, 3), d = 5;
  int e = x +, g[1] = {{1, 2}}, h = 6;
  return b + d + h + nope;
}
int g1 = 1, h1[1] = {1, 2}, k1 = 3;
int p(unk_t a), q(void);
int m(void) { return k1 + q() + nope2; }
"""


def test_declarator_list_goes_on(tmp_path):
    # an error in a declarator or its initializer goes on with the list's
    # next declarator after its ',' (gcc): b, d, h, k1 and q are declared
    p = _same_at_every_level(tmp_path, DECL_LIST, "-fmax-errors=0", "-Wall")
    d = _diags(p)
    assert [l for l, k, _ in d if k == "error"] == [3, 4, 5, 5, 6, 8, 9, 10], p.stderr
    undeclared = [m for _, _, m in d if "undeclared" in m]
    assert undeclared == ["'nope' undeclared", "'nope2' undeclared"], p.stderr
    assert not [m for _, k, m in d if k == "warning"], p.stderr


MACRO_ARGS = """\
#define ADD(a, b) ((a) + (b))
int f(void) { return ADD(1); }
int g(void) { return nope1; }
int h(void) { return ADD(1, (2, 3), 4); }
int k(void) { int r = ADD(1, 2); return r + nope2; }
static inline int s(void) { return ADD(1, 2, 3); }
int u(void) { return s() + nope3; }
"""


def test_macro_wrong_argument_count_goes_on(tmp_path):
    # report, skip the invocation, go on (gcc): the parse goes on after the
    # statement, in a body saved as tokens too
    p = _same_at_every_level(tmp_path, MACRO_ARGS)
    assert [(l, m) for l, _, m in _diags(p)] == [
        (2, "macro 'ADD' used with too few args"),
        (3, "'nope1' undeclared"),
        (4, "macro 'ADD' used with too many args"),
        (5, "'nope2' undeclared"),
        (6, "macro 'ADD' used with too many args"),
        (7, "'nope3' undeclared"),
    ], p.stderr


def test_macro_error_in_unused_static_inline(tmp_path):
    # a body nothing parses still says the error of its saved tokens
    body = "#define A(x) x\nstatic inline int w(void) { return A(1, 2); }\nint f(void) { return 1; }\n"
    p = _same_at_every_level(tmp_path, body)
    assert _error_lines(p) == [2], p.stderr
    assert p.returncode == 1 and not (tmp_path / "t.o").exists()


LEXER_ERRORS = """\
int f(int x)
{
  int y = nope1;
  y += 12abc;
  y += '';
  y += \x01;
  return y + nope2;
}
int g(void) { return nope3; }
static int h(int x)
{
  x = nope4;
  x += 0x1.0p;
#error in a body
#warning also in a body
  return x + nope5;
}
int k(void) { return h(1); }
"""


def test_lexer_errors_in_saved_bodies(tmp_path):
    # a number's error, an empty character constant, an unrecognized
    # character, #error and #warning: said where the parse reaches them when
    # the body is saved as tokens (-O1+, a static inline), as at -O0
    p = _same_at_every_level(tmp_path, LEXER_ERRORS, "-fmax-errors=0")
    assert [(l, k) for l, k, _ in _diags(p)] == [
        (3, "error"), (4, "error"), (5, "error"), (6, "error"), (7, "error"), (9, "error"),
        (12, "error"), (13, "error"), (14, "error"), (15, "warning"), (16, "error"),
    ], p.stderr


ASM_OPERANDS = """\
int f(int x) { __asm__("mov %0, %1" : "=r"(x) : "q"(x)); return x + nope1; }
int g(void) { return nope2; }
int h(int x) { __asm__("mov %0, %5" : "=r"(x) : "r"(x)); return x; }
int k(int x) { __asm__("add %0, %1" : "=r"(x) : "+r"(x)); return x; }
int m(void) { return nope3; }
"""


def test_asm_operand_errors_drop_the_statement(tmp_path):
    # the constraints and the operand references are checked where the asm
    # statement is parsed: an error drops it, the parse goes on
    p = _same_at_every_level(tmp_path, ASM_OPERANDS, "-fmax-errors=0")
    assert [(l, m) for l, _, m in _diags(p)] == [
        (1, "unknown constraint 'q'"),
        (1, "'nope1' undeclared"),
        (2, "'nope2' undeclared"),
        (3, "invalid operand reference after %"),
        (4, "'+' modifier can only be applied to outputs"),
        (5, "'nope3' undeclared"),
    ], p.stderr


def test_asm_text_error_in_code_generation_goes_on(tmp_path):
    # an error while a function's code is generated (its asm text) is said;
    # the unit goes on as -fsyntax-only (-fsyntax-only generates nothing,
    # so says nothing about the text)
    body = "int m(int x) { __asm__(\"bogus_insn r0\"); return x; }\nint n(void) { return nope; }\n"
    p = _same_at_every_level(tmp_path, body, levels=("-O0", "-O1", "-O2", "-Os"))
    assert _error_lines(p) == [1, 2], p.stderr
    assert not (tmp_path / "t.o").exists()
    for flags in (["-g"], ["-O2", "-g"], ["-ffunction-sections"]):
        q = _run(tmp_path, body, *flags)
        assert q.stderr == p.stderr, (flags, q.stderr)


SETUP_ERROR = """\
struct T { int x; };
int f(int n, int a[n + sizeof(struct T { int y; })]) { return nope1 + a[0]; }
int g(void) { return nope2; }
"""


def test_error_while_parameters_are_set_up(tmp_path):
    # the VLA parameter's size, replayed when the function is set up, says
    # an error there: the body is then parsed check-only (nope1)
    p = _same_at_every_level(tmp_path, SETUP_ERROR)
    assert [(l, m) for l, _, m in _diags(p)] == [
        (2, "struct/union/enum already defined"),
        (2, "'nope1' undeclared"),
        (3, "'nope2' undeclared"),
    ], p.stderr


DEFERRED_ORDER = """\
int f(void)
{
  int a[2] = {1, 2, 3};
  return a[0] + x1;
}
int g = 1, h[1] = {1, 2}, k = 3;
static inline int sq(int v) { return v * nope; }
int m(void) { return k + sq(2) + other; }
static int unused_fn(void) { return 1; }
int gg = 1.5 + "";
static int a(int x) { return x + nope1; }
int b(void) { return a(1) + nope2; }
static int c(int x) { return x * nope3; }
int d(void) { return nope4 + c(2); }
static int unused(void) { return nope5; }
static int tab[3] = { 1, nope6, 3 };
int e(void) { return tab[0] + nope7; }
"""


def test_deferred_bodies_in_source_order(tmp_path):
    # the bodies deferred to the end of the unit (-O1+: a function called
    # once, a static's initializer, an unused static; a static inline at
    # every -O) say their errors at their place: -O0's order, gcc's
    p = _same_at_every_level(tmp_path, DEFERRED_ORDER, "-fmax-errors=0", "-Wall")
    lines = _error_lines(p)
    assert lines == sorted(lines), p.stderr
    assert lines == [3, 4, 6, 7, 8, 10, 11, 12, 13, 14, 15, 16, 17], p.stderr


@pytest.mark.parametrize("n", ["1", "2", "3", "5", "8", "0"])
def test_deferred_bodies_and_the_cap(tmp_path, n):
    # -fmax-errors=N counts in that order: the same N errors at every level
    p = _same_at_every_level(tmp_path, DEFERRED_ORDER, f"-fmax-errors={n}")
    lines = _error_lines(p)
    if n != "0":
        assert len(lines) == min(int(n), 13), p.stderr
        assert p.stderr.rstrip().endswith(TERMINATED + n + "."), p.stderr
    assert lines == sorted(lines), p.stderr


@pytest.mark.parametrize("fmt", ["json", "sarif"])
def test_deferred_order_machine_readable(tmp_path, fmt):
    p = _same_at_every_level(tmp_path, DEFERRED_ORDER, f"-fdiagnostics-format={fmt}", "-fmax-errors=4")
    assert TERMINATED + "4." in p.stderr


def test_warnings_in_source_order(tmp_path):
    body = (
        "int f(void) { int u; return 0; }\nint g = 1;\nstatic int s(void) { int w; return 3; }\n"
        "int *p = 3;\nint z(void) { return s(); }\n"
    )
    p = _same_at_every_level(tmp_path, body, "-Wall")
    assert [l for l, _, _ in _diags(p, "warning")] == [1, 3, 4], p.stderr
    assert p.returncode == 0


def test_fatal_error_after_deferred_body(tmp_path):
    # a missing include stops the compile; a body deferred before it still
    # says its error first, as at -O0
    body = "int f(void) { return x; }\n#include \"nope_not_here.h\"\nint g(void) { return y; }\n"
    p = _same_at_every_level(tmp_path, body)
    assert _error_lines(p) == [1, 2], p.stderr
    body = "int f(void)\n{\n  int a = x;\n#include \"nope_not_here.h\"\n  return a;\n}\nint g(void) { return y; }\n"
    p = _same_at_every_level(tmp_path, body)
    assert _error_lines(p) == [3, 4], p.stderr


RECOVER_SWEEP = """\
#define ADD(a, b) ((a) + (b))
struct P { int x, y; };
static inline int sq(int v) { return v * v; }
static int tab[3] = { 1, 2, 3 };
int g1 = 1, g2[2] = { 3, 4 }, g3 = 5;
int f(int n, struct P *p)
{
  int s = ADD(n, 1), t[2] = { 1, 2 }, u = 3;
  __asm__("mov %0, %1" : "=r"(s) : "r"(s));
  for (int i = 0; i < n; i++)
    s += sq(i) + tab[i % 3];
  return s + t[0] + u + p->x + g2[1];
}
int k(void) { return 12 + 'a'; }
"""


def test_token_deletion_sweep_limits(tmp_path):
    # every token of a source using what the recovery now goes on after
    # (declaration lists, macros, asm, saved bodies) removed in turn: no
    # hang, no crash, every level says the same -- but for a brace: a body
    # saved as tokens (-O1+) ends at its braces' balance, which a stray one
    # moves, where -O0's parse may read it as an initializer's
    toks = re.findall(r"[A-Za-z_]\w*|\d+|'[^']*'|\"[^\"]*\"|->|\+\+|--|\+=|&&|[^\s\w]", RECOVER_SWEEP)
    spans = []
    pos = 0
    for t in toks:
        i = RECOVER_SWEEP.index(t, pos)
        spans.append((i, i + len(t)))
        pos = i + len(t)
    for k, (a, b) in enumerate(spans):
        body = RECOVER_SWEEP[:a] + " " + RECOVER_SWEEP[b:]
        outs = []
        for opt in LEVELS:
            p = _run(tmp_path, body, opt, "-fmax-errors=0", timeout=20)
            assert p.returncode in (0, 1), (k, opt, body, p.returncode, p.stderr)
            assert "compiler_error" not in p.stderr, (k, opt, body, p.stderr)
            outs.append(p.stderr)
        if RECOVER_SWEEP[a:b] not in ("{", "}"):
            assert len(set(outs)) == 1, (k, body, outs)


def test_valid_code_the_new_checks_accept(tmp_path):
    # a struct body in a static's initializer (its ';' is no missing '}'),
    # gcc's '#' / '*' constraint hints, an asm statement in dead code (never
    # generated, never checked): no error at any level (gcc torture
    # 920928-4, 20000922-1)
    body = (
        "static unsigned long off[] = { 0, sizeof(struct { long n; char s[4]; }), 2 };\n"
        "void doit(int);\n"
        "void q(int x) { if (0) __asm__ volatile(\"%0\" : : \"i#*X\"(x)); else doit(x); }\n"
        "int r(int x) { __asm__(\"mov %0, %1\" : \"=r\"(x) : \"r\"(x)); return x + (int)off[1]; }\n"
    )
    p = _same_at_every_level(tmp_path, body)
    assert p.returncode == 0 and not p.stderr, p.stderr


CALL_ARGS = """\
typedef unsigned int size_t;
void *memcpy(void *d, const void *s, size_t n);
int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void f(char *d, const char *s)
{
  memcpy(d, s + nope1, sizeof d);
  printf("%d %s\\n", nope2, s);
  memcpy(d, s, sizeof(printf("%d", nope3)));
  int a = 1, b = nope4, c = 3;
  printf("%d\\n", a + b + c);
}
int g = 1, h = nope5, k = 2;
"""

CALL_ARGS_INLINE = """\
typedef unsigned int size_t;
void *memcpy(void *d, const void *s, size_t n);
int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static inline int bad(int x) { return x + nope1; }
void f(char *d, const char *s)
{
  memcpy(d, s, bad(4));
  printf("%d %s\\n", bad(2), s);
  int a = bad(1), b = 2;
  printf("%d\\n", a + b);
}
"""


@pytest.mark.parametrize(
    "case,lines", [("args", [6, 7, 8, 9, 12]), ("inline", [4])], ids=["args", "inline"]
)
def test_errors_in_call_arguments_and_declaration_lists(tmp_path, case, lines):
    # an error in a call's arguments while its -Wformat / -Wsizeof-pointer-
    # memaccess checks are on (builtin/call.c CallArgChecks, on the heap), or
    # in a declaration list (decl.c's RecoverDeclList, from a free list), and
    # at -O1+ in an inline expansion replayed inside the arguments: the
    # recovery gives back what the frames it left held -- an ASan build
    # reports a leak otherwise
    body = CALL_ARGS if case == "args" else CALL_ARGS_INLINE
    outs = set()
    for opt in ("-O0", "-O1", "-O2", "-fsyntax-only"):
        p = _run(tmp_path, body, "-Wall", opt)
        assert p.returncode == 1, (opt, p.stderr)
        assert "Sanitizer" not in p.stderr, (opt, p.stderr)
        assert _error_lines(p) == lines, (opt, p.stderr)
        outs.add(p.stderr)
    assert len(outs) == 1, outs
