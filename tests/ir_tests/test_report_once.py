"""Report once: what the frontend says about a place is said once per unit.

The frontend parses a body more than once -- the -O1+ inliner replays a
`static inline` at every call site, a static is compiled again or on its
own after its calls were inlined, a body every call inlined gets a
check-only parse.  Every frontend warning goes through one set
(tcc_diag_once in libtcc.c: option, file, line, text), so a warning is
given once whatever the -O level, two of a line are still two (the k-th of
a parse is the k-th of every other), and the #pragma diagnostic state is
the one at its line.  The older warnings (-Wint-conversion,
-Wincompatible-pointer-types, -Wdiscarded-qualifiers,
-Wimplicit-function-declaration, -Wreturn-type, -Wnonnull, the yasc type
rules, ...) are on it too.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

LEVELS = ["-O0", "-O1", "-O2", "-Os"]


def _run(tmp_path, body, *flags, compat=True, name="t.c"):
    src = tmp_path / name
    src.write_text(body)
    cmd = [str(TCC)] + (["-ccompat"] if compat else []) + ["-c", *flags, str(src), "-o", str(tmp_path / "t.o")]
    return subprocess.run(cmd, capture_output=True, text=True)


def _diags(proc, kind="warning"):
    out = []
    for line in proc.stderr.splitlines():
        parts = line.split(": ", 2)
        if len(parts) == 3 and parts[1] == kind:
            out.append((int(parts[0].rsplit(":", 1)[1]), parts[2]))
    return sorted(out)


INT_PTR = "assignment makes pointer from integer without a cast"
PTR_INT = "assignment makes integer from pointer without a cast"
INCOMPAT = "assignment from incompatible pointer type"
QUAL = "assignment discards qualifiers from pointer target type"
NONNULL = "null passed as argument 1, which is declared nonnull"
IMPLICIT = "implicit declaration of function 'undeclared'"
VOID_RET = "void function returns a value"

OLD = """\
void use(void *p) __attribute__((nonnull));
static inline int f(int a)
{
  int *p = a;
  long *l = p;
  char *s = "x";
  int n = p;
  use(0);
  return undeclared(a) + *l + *s + n;
}
static inline void g(int a) { if (a) return 1; }
int main(int argc, char **argv) { g(argc); g(argc); return f(argc) + f(argc + 1); }
"""

OLD_EXPECTED = sorted([
    (4, INT_PTR),
    (5, INCOMPAT),
    (6, QUAL),
    (7, PTR_INT),
    (8, NONNULL),
    (9, IMPLICIT),
    (11, VOID_RET),
])


@pytest.mark.parametrize("level", LEVELS)
def test_old_warnings_once_at_every_level(tmp_path, level):
    proc = _run(tmp_path, OLD, level, "-Wall", "-Wwrite-strings")
    assert proc.returncode == 0, proc.stderr
    assert _diags(proc) == OLD_EXPECTED, proc.stderr


def test_nonnull_once_at_o2(tmp_path):
    # three copies before: the out-of-line compile and the two expansions
    body = ("void use(void *p) __attribute__((nonnull));\n"
            "static inline void f(void) { use(0); }\n"
            "void g(void) { f(); f(); }\n"
            "void h(void (*q)(void)) { q = f; q(); }\n")
    proc = _run(tmp_path, body, "-O2", "-Wall")
    assert _diags(proc) == [(2, NONNULL)], proc.stderr


@pytest.mark.parametrize("level", LEVELS)
def test_two_of_a_line_are_two(tmp_path, level):
    # gcc tells them apart by column: both are reported, once each
    body = ("void use(void *p) __attribute__((nonnull));\n"
            "static inline int f(int a) { int *p = a, *q = a; use(0); use(0); return p == q; }\n"
            "int main(int argc, char **argv) { return f(argc) + f(argc + 1); }\n")
    proc = _run(tmp_path, body, level, "-Wall")
    assert _diags(proc) == [(2, INT_PTR), (2, INT_PTR), (2, NONNULL), (2, NONNULL)], proc.stderr


def test_no_and_error_forms(tmp_path):
    proc = _run(tmp_path, OLD, "-O2", "-Wall", "-Wno-int-conversion")
    assert [d for d in _diags(proc) if d[1] in (INT_PTR, PTR_INT)] == [], proc.stderr
    proc = _run(tmp_path, OLD, "-O2", "-Wall", "-Werror=nonnull")
    assert proc.returncode != 0
    assert _diags(proc, "error") == [(8, NONNULL)], proc.stderr
    proc = _run(tmp_path, OLD, "-O2", "-Wall", "-Werror", "-Wno-error=nonnull", "-Wno-int-conversion",
                "-Wno-incompatible-pointer-types", "-Wno-implicit-function-declaration", "-Wno-return-type")
    assert proc.returncode == 0, proc.stderr
    assert _diags(proc) == [(8, NONNULL)], proc.stderr


@pytest.mark.parametrize("level", LEVELS)
def test_safe_default_error_once(tmp_path, level):
    # the safe default: int-conversion is an error -- reported at its line
    body = ("static inline int f(int a) { int * view p = a; return p != 0; }\n"
            "int main(int argc, char * view * view argv) { (void)argv; return f(argc) + f(argc + 1); }\n")
    proc = _run(tmp_path, body, level, "-Wno-lifetime", compat=False)
    assert proc.returncode != 0
    assert _diags(proc, "error") == [(1, INT_PTR)], proc.stderr


@pytest.mark.parametrize("level", LEVELS)
def test_pragma_at_the_body_line(tmp_path, level):
    # the expansions come after the pop: the state at the body's line counts
    body = ("#pragma GCC diagnostic push\n"
            "#pragma GCC diagnostic ignored \"-Wint-conversion\"\n"
            "static inline int f(int a) { int *p = a; return p != 0; }\n"
            "#pragma GCC diagnostic pop\n"
            "static inline int g(int a) { int *p = a; return p != 0; }\n"
            "int main(int argc, char **argv) { (void)argv; return f(argc) + f(argc + 1) + g(argc) + g(1); }\n")
    proc = _run(tmp_path, body, level, "-Wall")
    assert _diags(proc) == [(5, INT_PTR)], proc.stderr


@pytest.mark.parametrize("level", LEVELS)
def test_quiet_in_system_header(tmp_path, level):
    inc = tmp_path / "sys"
    inc.mkdir()
    (inc / "h.h").write_text("static inline int f(int a) { int *p = a; return p != 0; }\n")
    body = "#include <h.h>\nint main(int argc, char **argv) { (void)argv; return f(argc) + f(1); }\n"
    proc = _run(tmp_path, body, level, "-Wall", "-isystem", str(inc))
    assert proc.returncode == 0, proc.stderr
    assert _diags(proc) == [], proc.stderr


@pytest.mark.parametrize("level", LEVELS)
def test_uncalled_static_inline(tmp_path, level):
    # one of the main file nobody calls: its check-only parse speaks, at -O0 as at -O2
    body = "static inline int f(int a) { int *p = a; return p != 0; }\nint main(void) { return 0; }\n"
    proc = _run(tmp_path, body, level, "-Wall")
    assert _diags(proc) == [(1, INT_PTR)], proc.stderr


YASC = """\
char * owned make(void);
static inline void f(void) { make(); }
static inline void g(void) { char * view v = make(); (void)v; }
void h(void) { f(); f(); g(); g(); }
"""


@pytest.mark.parametrize("level", LEVELS)
def test_yasc_type_rules_once(tmp_path, level):
    proc = _run(tmp_path, YASC, level, "-std=yasc", "-Wno-lifetime", compat=False)
    assert proc.returncode == 0, proc.stderr
    assert _diags(proc) == [
        (2, "an owned result is discarded: it is never freed"),
        (3, "an owned result is stored in a pointer that owns nothing: it is never freed"),
    ], proc.stderr


@pytest.mark.parametrize("level", LEVELS)
def test_ccompat_without_wall(tmp_path, level):
    # -ccompat without -Wall: gcc's default-on ones, once each; -Wnonnull and
    # -Wwrite-strings are off
    proc = _run(tmp_path, OLD, level)
    assert proc.returncode == 0, proc.stderr
    assert _diags(proc) == sorted([(4, INT_PTR), (5, INCOMPAT), (7, PTR_INT), (9, IMPLICIT), (11, VOID_RET)]), \
        proc.stderr


def test_objects_identical_with_and_without_warnings(tmp_path):
    body = OLD
    for level in LEVELS:
        a = _run(tmp_path, body, level, "-Wall", "-Wwrite-strings")
        oa = (tmp_path / "t.o").read_bytes()
        b = _run(tmp_path, body, level, "-w", "-Wwrite-strings")
        ob = (tmp_path / "t.o").read_bytes()
        assert a.returncode == 0 and b.returncode == 0, a.stderr + b.stderr
        assert oa == ob, level


ASM_GOTO_INLINE = """\
extern void fn1(void), fn2(void);
static inline __attribute__((always_inline)) int foo(int *x, long y)
{
  asm goto("" : : "r"(x), "r"(y) : "memory" : lab);
  return 0;
lab:
  return 1;
}
static inline int back(int *x)
{
top:
  asm goto("" : : "r"(x) : "memory" : top, out);
  return 0;
out:
  return 2;
}
void bar(int *x)
{
  if (foo(x, 23))
    fn1();
  else
    fn2();
  foo(x, 2);
  back(x);
}
"""


@pytest.mark.parametrize("level", LEVELS)
def test_check_only_asm_goto_labels_stay_out(tmp_path, level):
    """gcc.c-torture/compile/pr51767.c: the check-only parse of a body every
    call inlined registered its asm goto label as an ELF symbol, an
    undefined `lab` in the object only when a check was on."""
    objs = []
    for flags in ([], ["-Wuninitialized"], ["-Wall"], ["-Warray-bounds=2"]):
        proc = _run(tmp_path, ASM_GOTO_INLINE, level, *flags)
        assert proc.returncode == 0, proc.stderr
        objs.append((tmp_path / "t.o").read_bytes())
        nm = subprocess.run(["arm-none-eabi-nm", str(tmp_path / "t.o")], capture_output=True, text=True).stdout
        assert not [l for l in nm.splitlines() if l.split()[-2:] in (["U", "lab"], ["U", "top"], ["U", "out"])], nm
    assert all(o == objs[0] for o in objs), level


@pytest.mark.parametrize("level", LEVELS)
def test_speculative_fold_still_reports(tmp_path, level):
    # -O1+ folds h(0) by a speculative parse with constant arguments, which
    # says nothing; the body is then parsed once more for what it met
    body = "static inline int h(int *p) { return p == 1; }\nint main(void) { return h(0) + h(0); }\n"
    proc = _run(tmp_path, body, level)
    assert _diags(proc) == [(1, "pointer/integer mismatch in comparison")], proc.stderr


@pytest.mark.parametrize("level", LEVELS)
def test_wsystem_headers_reaches_every_entry_point(tmp_path, level):
    # error1 decides the system header for the whole layer: -Wsystem-headers
    # gives tcc_warning_once's and tcc_fe_warning's alike, once each
    inc = tmp_path / "sys"
    inc.mkdir()
    (inc / "h.h").write_text("static inline int f(int a) { int *p = a; return (p != 0) + (1 << 40); }\n")
    body = "#include <h.h>\nint main(int argc, char **argv) { (void)argv; return f(argc) + f(1); }\n"
    proc = _run(tmp_path, body, level, "-Wall", "-isystem", str(inc))
    assert _diags(proc) == [], proc.stderr
    proc = _run(tmp_path, body, level, "-Wall", "-Wsystem-headers", "-isystem", str(inc))
    assert _diags(proc) == [(1, INT_PTR), (1, "left shift count >= width of type")], proc.stderr
