"""-Wformat, -Wformat-security, -Wformat-n: format(printf|scanf, n, m).

A call to a function declared `__attribute__((format(printf, n, m)))` (also
`__format__`, `gnu_printf`, `[[gnu::format(...)]]`) with a literal format has
its arguments checked against the conversions, as they are parsed: the type
(after the default promotions for printf; signedness is not checked, as with
gcc), too few and too many arguments, unknown conversions, `*` widths.  YasOS
libc's length modifiers are known: hh, h, l, ll, q, L, j, z, t.  -Wformat is in
-Wall (gcc's).

-Wformat-security (not in -Wall, as in gcc; the safe default): the format is
not a literal and nothing follows it.  -Wformat-n (neither -Wall nor gcc): a
`%n` in a literal printf format, an error in the safe default.  Each warning
appears once, whatever the -O level (the inliner replays a body per call).
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

PRELUDE = """\
typedef unsigned int size_t;
typedef int ptrdiff_t;
typedef long long intmax_t;
int pf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int sf(const char *fmt, ...) __attribute__((__format__(__scanf__, 1, 2)));
int vpf(const char *fmt, __builtin_va_list ap) __attribute__((format(printf, 1, 0)));
[[gnu::format(gnu_printf, 2, 3)]] int fpf(void *f, const char *fmt, ...);
"""
PL = PRELUDE.count("\n")

# yasc spelling for the safe default
PRELUDE_YASC = """\
int pf(const char * view fmt, ...) __attribute__((format(printf, 1, 2)));
"""


def _run(tmp_path, body, *flags, prelude=PRELUDE, ccompat=True, name="t.c"):
    src = tmp_path / name
    src.write_text(prelude + body)
    cmd = [str(TCC)] + (["-ccompat"] if ccompat else []) + ["-c", *flags, str(src), "-o", str(tmp_path / "t.o")]
    return subprocess.run(cmd, capture_output=True, text=True)


def _diags(proc, prelude_lines=PL):
    out = []
    for line in proc.stderr.splitlines():
        parts = line.split(":", 3)
        if len(parts) == 4 and parts[1].isdigit():
            out.append((int(parts[1]) - prelude_lines, parts[2].strip(), parts[3].strip()))
    return out


CLEAN = """\
void t(int i, unsigned u, long l, unsigned long ul, long long ll, size_t z, ptrdiff_t pd, intmax_t im,
       double d, float f, long double ld, char *s, const char *cs, unsigned char *us, void *p, short sh,
       char c, _Bool b, int *ip, long *lp, short *sp, signed char *scp, float *fp, double *dp, char **spp)
{
  pf("%d %i %u %x %X %o %c %hd %hhu %ld %lu %lld %llx %qd %zu %zd %td %jd %ju\\n",
     i, u, i, u, i, u, c, sh, c, l, ul, ll, ll, ll, z, z, pd, im, im);
  pf("%f %e %g %a %F %E %G %A %Lf %s %s %s %p %p %% %-+ #05.3d %*d %.*s %c %d\\n",
     d, f, d, d, d, d, d, d, ld, s, cs, us, p, s, i, i, i, i, s, b, b);
  pf("no conversions\\n");
  pf("%m and %5.2f\\n", d);
  sf("%d %u %ld %hd %hhd %f %lf %s %c %[a-z] %[^]x] %*d %p %n %ms", ip, (unsigned *)ip, lp, sp, scp, fp, dp, s, s,
     s, s, (void **)spp, ip, spp);
  fpf(p, "%s %d", s, i);
  pf("%1$d %2$s\\n", i, s); /* operands by number: not checked */
}
"""


def test_clean(tmp_path):
    proc = _run(tmp_path, CLEAN, "-Wall", "-Wformat-security")
    assert proc.returncode == 0 and not proc.stderr, proc.stderr


HITS = """\
void t(int i, long l, long long ll, size_t z, double d, char *s, void *p, int *ip, double *dp, const char *nf)
{
  pf("%d\\n", l);
  pf("%ld\\n", i);
  pf("%s\\n", i);
  pf("%d %d\\n", i);
  pf("%d\\n", i, i);
  pf("%f\\n", i);
  pf("%lu\\n", sizeof(int));
  pf("%*d\\n", l, i);
  pf("%y\\n", i);
  pf("%s\\n", p);
  pf("%p\\n", i);
  pf("%Lf\\n", d);
  sf("%d", i);
  sf("%f", dp);
  fpf(p, "%lld", i);
  pf("%d%", i);
  pf("%lld %d", ll, ll);
  pf("%c", s);
}
"""
HIT_LINES = [
    (3, "format '%d' expects argument of type 'int', but argument 2 has type 'long'"),
    (4, "format '%ld' expects argument of type 'long int', but argument 2 has type 'int'"),
    (5, "format '%s' expects argument of type 'char *', but argument 2 has type 'int'"),
    (6, "format '%d' expects a matching 'int' argument"),
    (7, "too many arguments for format"),
    (8, "format '%f' expects argument of type 'double', but argument 2 has type 'int'"),
    (9, "format '%lu' expects argument of type 'long unsigned int', but argument 2 has type 'unsigned int'"),
    (10, "field width or precision in '%*d' expects argument of type 'int', but argument 2 has type 'long'"),
    (11, "unknown conversion type character 'y' in format"),
    (12, "format '%s' expects argument of type 'char *', but argument 2 has type 'void *'"),
    (13, "format '%p' expects argument of type 'void *', but argument 2 has type 'int'"),
    (14, "format '%Lf' expects argument of type 'long double', but argument 2 has type 'double'"),
    (15, "format '%d' expects argument of type 'int *', but argument 2 has type 'int'"),
    (16, "format '%f' expects argument of type 'float *', but argument 2 has type 'double *'"),
    (17, "format '%lld' expects argument of type 'long long int', but argument 3 has type 'int'"),
    (18, "spurious trailing '%' in format"),
    (19, "format '%d' expects argument of type 'int', but argument 3 has type 'long long'"),
    (20, "format '%c' expects argument of type 'int', but argument 2 has type 'char *'"),
]


def test_hits(tmp_path):
    proc = _run(tmp_path, HITS, "-Wformat")
    got = [(line, msg) for line, kind, msg in _diags(proc) if kind == "warning"]
    assert got == HIT_LINES, proc.stderr


def test_switches(tmp_path):
    body = "void t(long l) { pf(\"%d\", l); }\n"
    msg = "format '%d' expects argument"
    assert msg not in _run(tmp_path, body).stderr  # -ccompat: off
    assert msg in _run(tmp_path, body, "-Wall").stderr
    assert msg in _run(tmp_path, body, "-Wformat").stderr
    assert msg not in _run(tmp_path, body, "-Wall", "-Wno-format").stderr
    proc = _run(tmp_path, body, "-Wall", "-Werror=format")
    assert proc.returncode != 0 and "error: " + msg in proc.stderr, proc.stderr
    proc = _run(tmp_path, body, "-Wall", "-Werror", "-Wno-error=format")
    assert proc.returncode == 0 and "warning: " + msg in proc.stderr, proc.stderr
    assert not _run(tmp_path, body, "-Wall", "-w").stderr


def test_safe_default(tmp_path):
    body = "void t(long l) { pf(\"%d\", l); }\n"
    proc = _run(tmp_path, body, prelude=PRELUDE_YASC, ccompat=False)
    assert proc.returncode == 0 and "warning: format '%d' expects argument" in proc.stderr, proc.stderr


def test_pragma(tmp_path):
    body = (
        "void t(long l)\n{\n"
        "#pragma GCC diagnostic push\n"
        "#pragma GCC diagnostic ignored \"-Wformat\"\n"
        "  pf(\"%d\", l);\n"
        "#pragma GCC diagnostic pop\n"
        "  pf(\"%d\", l);\n}\n"
    )
    got = [line for line, _, msg in _diags(_run(tmp_path, body, "-Wall")) if "expects" in msg]
    assert got == [7]


INLINE = """\
static inline int f(long x) { return pf("%d\\n", x); }
static int g(const char *s) { return pf(s); }
int a(int y) { return f(y) + f(y + 1) + g("x") + g("y"); }
"""


@pytest.mark.parametrize("opt", ["-O0", "-O1", "-O2", "-Os"])
def test_once_at_every_level(tmp_path, opt):
    proc = _run(tmp_path, INLINE, opt, "-Wall", "-Wformat-security")
    got = sorted((line, msg) for line, _, msg in _diags(proc))
    assert got == [
        (1, "format '%d' expects argument of type 'int', but argument 2 has type 'long'"),
        (2, "format not a string literal and no format arguments"),
    ], proc.stderr


TWO_ON_A_LINE = """\
static inline void twice(void) { pf("%d", "x"); pf("%d", "x"); }
int main(void) { pf("%d", "x"); pf("%d", "x");
  twice(); twice(); return 0; }
"""


@pytest.mark.parametrize("opt", ["-O0", "-O1", "-O2", "-Os"])
def test_two_identical_on_a_line_are_two(tmp_path, opt):
    """Two identical warnings of one line are both reported, once each: the
    once layer counts the k-th of a parse apart (a static inline's body is
    parsed again per call site at -O1+)."""
    proc = _run(tmp_path, TWO_ON_A_LINE, opt, "-Wall")
    msg = "format '%d' expects argument of type 'int', but argument 2 has type 'char *'"
    assert sorted((line, m) for line, _, m in _diags(proc)) == [(1, msg), (1, msg), (2, msg), (2, msg)], proc.stderr


def test_syntax_only(tmp_path):
    proc = _run(tmp_path, INLINE, "-fsyntax-only", "-Wall", "-Wformat-security")
    got = sorted((line, msg) for line, _, msg in _diags(proc))
    assert got == [
        (1, "format '%d' expects argument of type 'int', but argument 2 has type 'long'"),
        (2, "format not a string literal and no format arguments"),
    ], proc.stderr


SECURITY = """\
void t(const char *nf, int i)
{
  pf(nf);
  pf(nf, i);
  pf("lit");
  pf(i ? "a" : "b");
  vpf(nf, 0);
}
"""


def test_format_security(tmp_path):
    msg = "format not a string literal and no format arguments"
    assert msg not in _run(tmp_path, SECURITY, "-Wall").stderr  # not in -Wall, as with gcc
    got = [line for line, _, m in _diags(_run(tmp_path, SECURITY, "-Wformat-security")) if m == msg]
    assert got == [3]
    assert msg not in _run(tmp_path, SECURITY, "-Wformat-security", "-Wno-format-security").stderr
    proc = _run(tmp_path, "void t(const char * view nf) { pf(nf); }\n", prelude=PRELUDE_YASC, ccompat=False)
    assert proc.returncode == 0 and "warning: " + msg in proc.stderr, proc.stderr


NBODY = "void t(int * view ip) { pf(\"%d%n\\n\", 1, ip); }\n"
NMSG = "'%n' in format writes through its argument"


def test_percent_n(tmp_path):
    cbody = NBODY.replace(" view", "")
    assert NMSG not in _run(tmp_path, cbody).stderr
    assert NMSG not in _run(tmp_path, cbody, "-Wall").stderr  # gcc has no such warning
    assert "warning: " + NMSG in _run(tmp_path, cbody, "-Wformat-n").stderr
    # the safe default refuses it
    proc = _run(tmp_path, NBODY, prelude=PRELUDE_YASC, ccompat=False)
    assert proc.returncode != 0 and "error: " + NMSG in proc.stderr, proc.stderr
    proc = _run(tmp_path, NBODY, "-Wno-error=format-n", prelude=PRELUDE_YASC, ccompat=False)
    assert proc.returncode == 0 and "warning: " + NMSG in proc.stderr, proc.stderr
    proc = _run(tmp_path, NBODY, "-Wno-format-n", prelude=PRELUDE_YASC, ccompat=False)
    assert proc.returncode == 0 and NMSG not in proc.stderr, proc.stderr
    # a va_list format is checked alone; scanf's %n is fine
    assert NMSG in _run(tmp_path, "void t(void) { vpf(\"%n\", 0); }\n", "-Wformat-n").stderr
    assert NMSG not in _run(tmp_path, "void t(int *ip) { sf(\"%n\", ip); }\n", "-Wformat-n").stderr


def test_system_header_is_quiet(tmp_path):
    inc = tmp_path / "sys"
    inc.mkdir()
    (inc / "s.h").write_text(PRELUDE + "static inline int sh(long l) { return pf(\"%d\", l); }\n")
    proc = _run(tmp_path, "#include <s.h>\nint u(long l) { return sh(l) + pf(\"%d\", l); }\n",
                "-Wall", "-isystem", str(inc), prelude="")
    got = [(line, msg) for line, _, msg in _diags(proc, 0)]
    assert got == [(2, "format '%d' expects argument of type 'int', but argument 2 has type 'long'")], proc.stderr


def test_bad_attribute(tmp_path):
    proc = _run(tmp_path, "int bad(const char *f, ...) __attribute__((format(printf, 2, 1)));\n", "-Wall", prelude="")
    assert "'format' attribute: argument numbers 2, 1 are out of range: ignored" in proc.stderr, proc.stderr


@pytest.mark.parametrize("opt", ["-O0", "-O1", "-O2", "-Os"])
def test_objects_identical(tmp_path, opt):
    """A check never changes code: with or without the options, the same bytes."""
    body = CLEAN + HITS.replace("void t(", "void t2(") + INLINE
    a = _run(tmp_path, body, opt, "-w", "-Wno-format", "-Wno-format-security", "-Wno-format-n")
    assert a.returncode == 0, a.stderr
    obj_a = (tmp_path / "t.o").read_bytes()
    b = _run(tmp_path, body, opt, "-Wformat", "-Wformat-security", "-Wformat-n")
    assert b.returncode == 0, b.stderr
    assert (tmp_path / "t.o").read_bytes() == obj_a


@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_header_inline_named_by_its_file(tmp_path, opt):
    """At -O2 a call site expands the body: the warning still names the header."""
    (tmp_path / "u.h").write_text(PRELUDE + "\nstatic inline int sh(long l) { return pf(\"%d\", l); }\n")
    proc = _run(tmp_path, '#include "u.h"\nint u(long l) { return sh(l) + sh(l + 1); }\n', opt, "-Wall", prelude="")
    lines = [l for l in proc.stderr.splitlines() if "expects argument" in l]
    assert len(lines) == 1 and f"u.h:{PL + 2}:" in lines[0], proc.stderr


TIME_PRELUDE = """\
typedef unsigned int size_t;
struct tm;
size_t my_strftime(char *, size_t, const char *, const struct tm *) __attribute__((format(strftime, 3, 0)));
long my_strfmon(char *, size_t, const char *, ...) __attribute__((format(strfmon, 3, 4)));
char *gettext(const char *) __attribute__((format_arg(1)));
char *dgettext(const char *, const char *) __attribute__((__format_arg__(2)));
int pf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define _(s) gettext(s)
"""
TIME_BODY = """\
void f(char *b, const struct tm *t, double d, int i, long double ld, const char *s)
{
  my_strftime(b, 10, "%Y-%m-%d %H:%M:%S %q", t);
  my_strftime(b, 10, "%Ey %Od %EQ %Oq %%", t);
  my_strftime(b, 10, "%-d %_H %05Y %^a %#b %c %x %y %k %l %s %P", t);
  my_strftime(b, 10, "%", t);
  my_strftime(b, 10, "%Ea %Ed %-a %^d %5D %EOy", t);
  my_strfmon(b, 10, "%n %i", d, i);
  my_strfmon(b, 10, "%=*#5.2n %Ln %!^(10i %q", d, ld, d);
  my_strfmon(b, 10, "%n");
  pf(_("%d"), s);
  pf(gettext("%s"), i);
  pf(dgettext("dom", "%s %d"), s, s);
  pf(_("%d"), i);
  pf(_(s), i);
  pf(_(_("%d")), s);
}
"""


@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_strftime_strfmon_format_arg(tmp_path, opt):
    """strftime: the conversions, their flags, width and E/O modifier (gcc's
    table and wording); strfmon: %i / %n take a double, L a long double;
    format_arg(n) (gettext): the result is argument n's literal.  As gcc 16
    (which also says "gnu_strftime")"""
    pl = TIME_PRELUDE.count("\n")
    proc = _run(tmp_path, TIME_BODY, opt, "-Wall", prelude=TIME_PRELUDE)
    assert proc.returncode == 0, proc.stderr
    assert [(l, m) for l, _, m in _diags(proc, pl)] == [
        (3, "unknown conversion type character 'q' in format"),
        (4, "unknown conversion type character 'Q' in format"),
        (4, "unknown conversion type character 'q' in format"),
        (6, "spurious trailing '%' in format"),
        (7, "'E' modifier used with '%a' strftime format"),
        (7, "'E' modifier used with '%d' strftime format"),
        (7, "'-' flag used with '%a' strftime format"),
        (7, "'^' flag used with '%d' strftime format"),
        (7, "field width used with '%D' strftime format"),
        (7, "use of 'E' modifier and 'O' modifier together in strftime format"),
        (8, "format '%i' expects argument of type 'double', but argument 5 has type 'int'"),
        (9, "unknown conversion type character 'q' in format"),
        (10, "format '%n' expects a matching 'double' argument"),
        (11, "format '%d' expects argument of type 'int', but argument 2 has type 'const char *'"),
        (12, "format '%s' expects argument of type 'char *', but argument 2 has type 'int'"),
        (13, "format '%d' expects argument of type 'int', but argument 3 has type 'const char *'"),
        (16, "format '%d' expects argument of type 'int', but argument 2 has type 'const char *'"),
    ], proc.stderr
    assert _run(tmp_path, TIME_BODY, opt, "-Wall", "-Wno-format", prelude=TIME_PRELUDE).stderr == ""


def test_format_arg_and_format_security(tmp_path):
    """a gettext of a literal is a literal format; of a variable it is not"""
    body = "void f(const char *s)\n{\n  pf(_(\"x\"));\n  pf(_(s));\n  pf(gettext(\"%d\"));\n}\n"
    pl = TIME_PRELUDE.count("\n")
    proc = _run(tmp_path, body, "-Wformat", "-Wformat-security", prelude=TIME_PRELUDE)
    assert [(l, m) for l, _, m in _diags(proc, pl)] == [
        (4, "format not a string literal and no format arguments"),
        (5, "format '%d' expects a matching 'int' argument"),
    ], proc.stderr
