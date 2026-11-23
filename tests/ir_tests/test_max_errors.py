"""Error recovery and -fmax-errors=N (frontend/gen/core/recover.c).

An error no longer ends the compile: the parser goes on at the next external
declaration (file scope: to the declaration's ';', or past a function body)
or the next statement (in a body: to the statement's ';' or closing brace;
the rest of the body is checked without code), up to -fmax-errors=N errors
(8 by default, 0 = no limit), then "compilation terminated due to
-fmax-errors=N." as gcc.  The compile still fails: exit status 1, no object,
no link.  A warning made an error (-Werror, the safe default's errors) and
"plain pointer in yasc" do not stop the parse at all; #error does not either
(gcc).  An undeclared name is reported once per function.  Errors inside a
preprocessor directive, a macro's substitution or inline asm still stop.
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
    # of the file, and g was never seen)
    p = _run(tmp_path, INIT_ERRORS, opt)
    assert _error_lines(p) == [3, 4, 7], p.stderr


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
int use(int * view p, int *q)
{
  char *loc = 0;
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
    # a definition's parameter (q, line 3) and a local's pointer (loc, line
    # 5) are said at the end of their body, in line order, whose uses decide
    # owned or view (decl/plain_param.c)
    assert plain == [1, 2, 3, 5], p.stderr
    assert (9, "warning", "'x' is used uninitialized") in d
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
