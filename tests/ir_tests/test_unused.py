"""-Wunused-* (frontend/gen/core/unused.c): what the source declares and never
uses.

In -Wall (gcc's C -Wall has them) and so in the safe default:
-Wunused-variable (a local, or a file-scope static), -Wunused-const-variable
(a const file-scope static of the main file: gcc's level 1),
-Wunused-function (a static, non-inline function), -Wunused-but-set-variable
(a local only assigned to), -Wunused-value (a statement whose value goes
nowhere), -Wunused-label, -Wunused-local-typedefs (a typedef in a block
nothing names).  Outside it, as gcc keeps them in -Wextra:
-Wunused-parameter and -Wunused-but-set-parameter.  -Wunused names the -Wall
ones.  unused / [[maybe_unused]] / used, (void)x, sizeof, a defer body, a
nested function's capture, a cleanup or alias attribute count as uses.

Decided from the source: the same diagnostics, once each, at every -O --
where static inline bodies are replayed at their call sites, statics are
deferred, dropped or never generated on their own.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")


def _run(tmp_path, body, *flags, ccompat=True, name="t.c"):
    src = tmp_path / name
    src.write_text(body)
    cmd = [str(TCC)] + (["-ccompat"] if ccompat else []) + ["-c", *flags, str(src), "-o", str(tmp_path / "t.o")]
    return subprocess.run(cmd, capture_output=True, text=True, cwd=tmp_path)


def _diags(proc):
    """(line, message) of every warning and error, sorted."""
    out = []
    for line in proc.stderr.splitlines():
        parts = line.split(":", 3)
        if len(parts) == 4 and parts[1].isdigit() and parts[2].strip() in ("warning", "error"):
            out.append((int(parts[1]), parts[3].strip()))
    return sorted(out)


def _msgs(proc):
    return sorted(m for _, m in _diags(proc))


HITS = {
    "unused-variable": ("int f(void)\n{\n  int a;\n  return 0;\n}\n", (3, "unused variable 'a'")),
    "unused-variable-static": ("static int sv;\nint f(void) { return 0; }\n", (1, "'sv' defined but not used")),
    "unused-const-variable": ("static const int sc = 3;\nint f(void) { return 0; }\n", (1, "'sc' defined but not used")),
    "unused-function": ("static int sf(void)\n{\n  return 1;\n}\nint f(void) { return 0; }\n",
                        (1, "'sf' defined but not used")),
    "unused-but-set-variable": ("int f(void)\n{\n  int c;\n  c = 1;\n  return 0;\n}\n",
                                (3, "variable 'c' set but not used")),
    "unused-value": ("int f(int y)\n{\n  y == 1;\n  return y;\n}\n", (3, "statement with no effect")),
    "unused-label": ("int f(void)\n{\nlbl:\n  return 0;\n}\n", (3, "label 'lbl' defined but not used")),
    "unused-local-typedefs": ("int f(void)\n{\n  typedef int T;\n  return 0;\n}\n",
                              (3, "typedef 'T' locally defined but not used")),
}

OPTION = {k: k.replace("-static", "") for k in HITS}


@pytest.mark.parametrize("case", sorted(HITS))
def test_hit(tmp_path, case):
    body, want = HITS[case]
    assert _diags(_run(tmp_path, body, "-Wall")) == [want]
    assert _diags(_run(tmp_path, body, "-W" + OPTION[case])) == [want]
    assert _diags(_run(tmp_path, body, "-Wunused")) == [want]


@pytest.mark.parametrize("case", sorted(HITS))
def test_options(tmp_path, case):
    body, want = HITS[case]
    opt = OPTION[case]
    assert not _run(tmp_path, body).stderr  # -ccompat: off
    assert not _run(tmp_path, body, "-Wall", "-Wno-" + opt).stderr
    assert not _run(tmp_path, body, "-Wall", "-Wno-unused").stderr
    err = _run(tmp_path, body, "-Wall", "-Werror=" + opt)
    assert err.returncode != 0 and _diags(err) == [(want[0], want[1])] and "error:" in err.stderr
    noerr = _run(tmp_path, body, "-Wall", "-Werror", "-Wno-error=" + opt)
    assert noerr.returncode == 0 and _diags(noerr) == [want]
    # the safe default has them
    assert _diags(_run(tmp_path, body, ccompat=False)) == [want]


def test_parameters_are_outside_wall(tmp_path):
    body = "int f(int a,\n      int b)\n{\n  a = 2;\n  return 0;\n}\n"
    assert not _run(tmp_path, body, "-Wall").stderr
    assert not _run(tmp_path, body, ccompat=False).stderr
    assert _diags(_run(tmp_path, body, "-Wunused-parameter")) == [(2, "unused parameter 'b'")]
    assert _diags(_run(tmp_path, body, "-Wunused-but-set-parameter")) == [(1, "parameter 'a' set but not used")]


VALUE = """int f(void);
int g;
volatile int vg;
void t(int a, int *p, volatile int *vp)
{
  *p++;
  a + f();
  (char)f();
  *vp;
  vg;
  a = 1, a;
  (void)0;
  0;
  a ? f() : 0;
  a && f();
  a && (g = 1);
  a || (f(), 0);
  &a;
  a++;
  f;
  a, f();
  for (a = 0; a < 1; a + 1)
    ;
  (int)a;
  ({ f(); 1; });
  __builtin_expect(a, 1);
}
"""


def test_value_forms_follow_gcc(tmp_path):
    assert _diags(_run(tmp_path, VALUE, "-Wunused-value")) == [
        (6, "value computed is not used"),
        (7, "value computed is not used"),
        (8, "value computed is not used"),
        (11, "right-hand operand of comma expression has no effect"),
        (13, "statement with no effect"),
        (15, "value computed is not used"),
        (18, "statement with no effect"),
        (20, "statement with no effect"),
        (21, "left-hand operand of comma expression has no effect"),
        (22, "statement with no effect"),
        (24, "statement with no effect"),
    ]


SET = """int use(int);
int f(void)
{
  int e = 0; e++;
  int g = 0; g += 2;
  int h = 0; h = h + 1;
  int x, y; y = x = 1;
  int t = 0; t++, t++;
  int q; q = 1, use(q);
  int w; (void)(w = 1);
  static int s; s = 1;
  int k; if ((k = use(0))) return 1;
  return y + h;
}
"""


def test_set_but_not_used_follows_gcc(tmp_path):
    assert _diags(_run(tmp_path, SET, "-Wunused-but-set-variable")) == [
        (4, "variable 'e' set but not used"),
        (5, "variable 'g' set but not used"),
        (8, "variable 't' set but not used"),
        (11, "variable 's' set but not used"),
    ]


CLEAN = """#include <stddef.h>
int ext(int);
static int only_init_ref;
int *gp = &only_init_ref;
static void used_by_unused_inline(void) {}
static inline void si(void) { used_by_unused_inline(); }
static int with_attr(void) __attribute__((unused));
static int with_attr(void) { return 0; }
[[maybe_unused]] static int mu;
static int keep __attribute__((used));
static volatile int vol;
static void cl(int *p) { (void)p; }
static int base(void) { return 1; }
int alias(void) __attribute__((alias("base")));
static int fwd(void);
int user(void) { return fwd(); }
static int fwd(void) { return 2; }
int f(int n)
{
  int v = 0; (void)v;
  int w __attribute__((unused));
  [[maybe_unused]] int x;
  int sz; size_t z = sizeof sz;
  int ad; int *pa = &ad; *pa = 1;
  int c __attribute__((cleanup(cl))) = 0;
  ext(1), ext(2);
  return n + (int)z;
}
"""


def test_clean(tmp_path):
    proc = _run(tmp_path, CLEAN, "-Wall", "-Wunused-parameter")
    assert not proc.stderr, proc.stderr


def test_nested_function_asm_goto_computed_goto(tmp_path):
    body = (
        "static int helper(int x) { return x; }\n"
        "int f(int a)\n{\n  int cap = a;\n  int inner(void) { return helper(cap); }\n"
        "  asm goto (\"b %l[out]\" : : : : out);\n  void *tgt = &&lbl;\n  goto *tgt;\nlbl:\n"
        "  return inner();\nout:\n  return 0;\n}\n"
    )
    assert not _run(tmp_path, body, "-Wall").stderr


def test_yasc_defer_and_move_are_uses(tmp_path):
    body = (
        "#include <stdlib.h>\n"
        "void take(char * owned p);\n"
        "int f(int n)\n{\n  char * owned p = malloc(4);\n  char * owned q = malloc(4);\n"
        "  int seen = n;\n  defer { (void)seen; }\n  take(move(q));\n  free(move(p));\n  return 0;\n}\n"
    )
    proc = _run(tmp_path, body, "-Wunused", ccompat=False)
    assert not [m for m in _msgs(proc) if "unused" in m or "not used" in m or "no effect" in m], proc.stderr


STATIC_INLINE = """int use(int);
static int helper(int x) { return x; }
static inline int twice(int a)
{
  int dead;
  int setonly;
  setonly = a;
  a == 3;
  return helper(a);
lbl:
  return 0;
}
static int g(int a) { int z; z = 1; return a * 2; }
static int never(int a) { int q; return a; }
static int st;
int h(int x) { return twice(x) + twice(x + 1) + g(x) + g(2); }
"""

STATIC_INLINE_WANT = [
    (5, "unused variable 'dead'"),
    (6, "variable 'setonly' set but not used"),
    (8, "statement with no effect"),
    (10, "label 'lbl' defined but not used"),
    (13, "variable 'z' set but not used"),
    (14, "'never' defined but not used"),
    (14, "unused variable 'q'"),
    (15, "'st' defined but not used"),
]


@pytest.mark.parametrize("opt", ["-O0", "-O1", "-O2", "-Os"])
def test_once_at_every_level(tmp_path, opt):
    assert _diags(_run(tmp_path, STATIC_INLINE, "-Wall", opt)) == STATIC_INLINE_WANT
    # -fsyntax-only parses every body once, for its diagnostics
    assert _diags(_run(tmp_path, STATIC_INLINE, "-Wall", "-fsyntax-only", opt)) == STATIC_INLINE_WANT


def test_pragma(tmp_path):
    body = (
        "int f(void)\n{\n"
        "#pragma GCC diagnostic push\n#pragma GCC diagnostic ignored \"-Wunused\"\n"
        "  int quiet;\n  quiet == 1;\n"
        "#pragma GCC diagnostic pop\n"
        "  int loud;\n  return 0;\n}\n"
        "#pragma GCC diagnostic ignored \"-Wunused-function\"\n"
        "static void s(void) {}\n"
    )
    assert _diags(_run(tmp_path, body, "-Wall")) == [(8, "unused variable 'loud'")]
    # a pragma may turn them on in a -ccompat unit
    on = "#pragma GCC diagnostic warning \"-Wunused-variable\"\nint f(void)\n{\n  int a;\n  return 0;\n}\n"
    assert _diags(_run(tmp_path, on)) == [(4, "unused variable 'a'")]


def test_system_header_is_quiet(tmp_path):
    inc = tmp_path / "sys"
    inc.mkdir()
    (inc / "h.h").write_text(
        "static int hv;\nstatic void hf(void) {}\n"
        "static inline int hi(int a) { int dead; a == 1; return a; }\n"
    )
    body = "#include <h.h>\nint f(int x) { return hi(x); }\n"
    for opt in ("-O0", "-O2"):
        assert not _run(tmp_path, body, "-Wall", "-isystem", str(inc), opt).stderr
    # the same header from a user include path is not
    assert _msgs(_run(tmp_path, body, "-Wall", "-I", str(inc))) == [
        "'hf' defined but not used",
        "'hv' defined but not used",
        "statement with no effect",
        "unused variable 'dead'",
    ]


@pytest.mark.parametrize("opt", ["-O0", "-O1", "-O2", "-Os"])
def test_objects_unchanged(tmp_path, opt):
    for src in (STATIC_INLINE, VALUE, SET, CLEAN):
        a = _run(tmp_path, src, opt, "-Wno-unused")
        objs = (tmp_path / "t.o").read_bytes()
        b = _run(tmp_path, src, opt, "-Wunused", "-Wunused-parameter", "-Wunused-but-set-parameter")
        assert a.returncode == 0 and b.returncode == 0, a.stderr + b.stderr
        assert (tmp_path / "t.o").read_bytes() == objs, src


@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_set_but_not_used_through_parts(tmp_path, opt):
    """`a[i] = 0` and `s.f = 1` where the address is not folded: the local is
    set, as gcc says -- not when the index reads it, nor through a pointer,
    nor for ++ / +=, which read the element (gcc's lines)"""
    body = """struct S { int f, g; int arr[4]; struct { int x; } in; };
int f1(int i) { int a[4]; a[i] = 0; return 0; }
int f2(int i) { int a[4]; a[1] = 0; return 0; }
int f3(int i) { struct S s; s.f = 1; return 0; }
int f4(int i) { struct S s; s.arr[i] = 1; return 0; }
int f5(int i) { struct S s; s.in.x = 1; return 0; }
int f6(int i) { int a[4][4]; a[i][1] = 0; return 0; }
int f7(int i) { int a[4]; a[i] = 0; return a[0]; }
int f8(int i) { int a[4]; int *p = a; p[i] = 0; return 0; }
int f9(int i) { int a[4]; a[i]++; return 0; }
int f10(int i) { int a[4]; a[a[0]] = 0; return 0; }
int f11(int i) { struct S s[2]; s[i].f = 0; return 0; }
int f12(int i) { int a[4]; a[i] += 1; return 0; }
"""
    proc = _run(tmp_path, body, "-Wall", "-Wno-uninitialized", opt)
    assert proc.returncode == 0, proc.stderr
    got = [(l, m) for l, m in _diags(proc) if "set but not used" in m]
    assert got == [(2, "variable 'a' set but not used"), (3, "variable 'a' set but not used"),
                   (4, "variable 's' set but not used"), (5, "variable 's' set but not used"),
                   (6, "variable 's' set but not used"), (7, "variable 'a' set but not used"),
                   (12, "variable 's' set but not used")], proc.stderr


@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_local_typedefs(tmp_path, opt):
    body = """typedef int G;
int f(int x)
{
  typedef int T;
  typedef long U;
  typedef char V __attribute__((unused));
  typedef struct { int a; } W;
  U u = x;
  {
    typedef int X;
    typedef int Y;
    x += sizeof(Y);
  }
  return (int)u + (W){1}.a;
}
static inline int g(void) { typedef int Z; return 1; }
int h(void) { return g() + g(); }
"""
    proc = _run(tmp_path, body, "-Wall", opt)
    assert _diags(proc) == [(4, "typedef 'T' locally defined but not used"),
                            (10, "typedef 'X' locally defined but not used"),
                            (16, "typedef 'Z' locally defined but not used")], proc.stderr


@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_statement_expression_statements(tmp_path, opt):
    """a statement of `({ ... })` other than the last is a discarded value"""
    body = """int g(int);
int f(int a, int b)
{
  int r = ({ a; b; });
  r += ({ a + 1; g(a); b; });
  r += ({ g(a); a == 1; int t = g(b); t; });
  r += ({ int y; y = 1; 2; });
  r += ({ (void)a; g(b); 4; });
  return r;
}
"""
    proc = _run(tmp_path, body, "-Wall", opt)
    assert _diags(proc) == [(4, "statement with no effect"), (5, "statement with no effect"),
                            (6, "statement with no effect"), (7, "variable 'y' set but not used")], proc.stderr
