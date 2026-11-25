"""-Wtainted-format: a printf-style format that comes from untrusted input
(ir/taint.c, raw IR before any pass).

Sources: main's argv, getenv/secure_getenv results, the buffers fgets, gets,
fread, read, pread, recv, recvfrom, getline fill, and functions declared
__attribute__((tainted_result)) / tainted_arg(n).  Sinks: the format argument
of a format(printf, n, m) function, or of the printf family by name.  Off by
default, not in -Wall.  Followed through globals, struct members and the
unit's functions (summaries computed at the end of the unit, callees defined
later included; a format reached through a call is reported at the sink with
a note at the call), calls through pointers (the address-taken functions of
their type), scanf's %s / %[ / %c buffers, and across units: the units'
records (".tcc.taint", only with the flag) solved at the link.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

PRELUDE = """\
typedef unsigned size_t;
int printf(const char *, ...);
int snprintf(char *, size_t, const char *, ...);
char *getenv(const char *);
char *fgets(char *, int, void *);
int read(int, void *, size_t);
void logf(int lvl, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
const char *user_name(void) __attribute__((tainted_result));
void get_line(int fd, char *buf, size_t n) __attribute__((tainted_arg(2)));
"""
PL = PRELUDE.count("\n")

HITS = """\
int main(int argc, char **argv)
{
  char buf[64], out[64];
  char *e = getenv("X");
  printf(argv[1]);
  printf(e);
  fgets(buf, sizeof buf, 0);
  printf(buf);
  char *p = buf + 2;
  logf(1, p);
  snprintf(out, sizeof out, user_name());
  char line[32];
  get_line(0, line, sizeof line);
  printf(line);
  char *q = argc > 1 ? argv[1] : "x";
  printf(q);
  return 0;
}
"""

CLEAN = """\
int helper(char **argv) { return printf("%s", argv[1]); }
int main(int argc, char **argv)
{
  char buf[64];
  char *e = getenv("X");
  printf("%s %s\\n", argv[1], e);
  printf("%d\\n", argc);
  e = "literal";
  printf(e);
  read(0, buf, sizeof buf);
  printf("%s", buf);
  logf(1, "%s", argv[0]);
  return helper(argv);
}
"""


# a link's runtime libraries (libtcc1, fp/lib*.a) are in the tree, not where
# the cross is configured to install them (test_cross_tu.py)
LINK_RT = [f"-B{TCC.parent}", f"-L{TCC.parent}/lib", f"-L{TCC.parent}/lib/fp"]


def _run(tmp_path, body, *flags, ccompat=True):
    src = tmp_path / "t.c"
    src.write_text(PRELUDE + body)
    cmd = [str(TCC)] + (["-ccompat"] if ccompat else []) + ["-c", *flags, str(src), "-o", str(tmp_path / "t.o")]
    return subprocess.run(cmd, capture_output=True, text=True)


def _diags(proc):
    out = []
    for line in proc.stderr.splitlines():
        parts = line.split(":", 3)
        if len(parts) == 4 and parts[1].isdigit():
            out.append((int(parts[1]) - PL, parts[2].strip(), parts[3].strip()))
    return sorted(out)


def _lines(proc):
    return [l for l, k, m in _diags(proc) if "untrusted input" in m and k != "note"]


def _notes(proc):
    return [l for l, k, m in _diags(proc) if "untrusted input" in m and k == "note"]


@pytest.mark.parametrize("opt", ["-O0", "-O1", "-O2", "-Os"])
def test_hits(tmp_path, opt):
    proc = _run(tmp_path, HITS, "-Wtainted-format", opt)
    assert _lines(proc) == [5, 6, 8, 10, 11, 14, 16]
    msgs = {m for _, _, m in _diags(proc)}
    assert "the format of 'printf' comes from untrusted input: write printf(\"%s\", ...)" in msgs
    assert "the format of 'logf' comes from untrusted input: write logf(\"%s\", ...)" in msgs
    assert "the format of 'snprintf' comes from untrusted input: write snprintf(\"%s\", ...)" in msgs


@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_clean(tmp_path, opt):
    proc = _run(tmp_path, CLEAN, "-Wtainted-format", opt)
    assert proc.returncode == 0
    assert _lines(proc) == []


def test_off_by_default(tmp_path):
    assert _lines(_run(tmp_path, HITS)) == []
    assert _lines(_run(tmp_path, HITS, "-Wall")) == []
    proc = _run(tmp_path, HITS, "-Wtainted-format", "-Wno-tainted-format")
    assert _lines(proc) == []


def test_werror(tmp_path):
    proc = _run(tmp_path, HITS, "-Werror=tainted-format")
    assert proc.returncode == 1
    assert all(k == "error" for l, k, m in _diags(proc) if "untrusted" in m)
    proc = _run(tmp_path, HITS, "-Werror", "-Wtainted-format", "-Wno-error=tainted-format")
    assert proc.returncode == 0 and _lines(proc)


def test_pragma(tmp_path):
    body = HITS.replace(
        "  printf(argv[1]);",
        '#pragma GCC diagnostic push\n#pragma GCC diagnostic ignored "-Wtainted-format"\n'
        "  printf(argv[1]);\n#pragma GCC diagnostic pop",
    )
    proc = _run(tmp_path, body, "-Wtainted-format")
    lines = _lines(proc)
    assert 5 not in lines and 7 not in lines and len(lines) == 6


def test_inline_helper_same_at_every_level(tmp_path):
    body = """\
static inline void say(const char *s) { printf(s); }
static inline const char *env(void) { return getenv("X"); }
int main(int argc, char **argv)
{
  say(argv[1]);
  say(argv[1]);
  printf(env());
  return 0;
}
"""
    got = {opt: _diags(_run(tmp_path, body, "-Wtainted-format", opt)) for opt in ("-O0", "-O1", "-O2")}
    assert got["-O0"] == got["-O1"] == got["-O2"]
    proc = _run(tmp_path, body, "-Wtainted-format", "-O2")
    # say's printf (once, with a note at the first call), printf(env())
    assert _lines(proc) == [1, 7] and _notes(proc) == [5]


# the functions PRELUDE declares, for a link
STUBS = """\
typedef unsigned size_t;
void _start(void) {}
int printf(const char *f, ...) { return 0; }
int snprintf(char *b, size_t n, const char *f, ...) { return 0; }
char *getenv(const char *n) { return 0; }
char *fgets(char *b, int n, void *f) { return b; }
int read(int fd, void *b, size_t n) { return 0; }
void logf(int lvl, const char *fmt, ...) {}
const char *user_name(void) { return 0; }
void get_line(int fd, char *buf, size_t n) {}
int scanf(const char *f, ...) { return 0; }
int fscanf(void *s, const char *f, ...) { return 0; }
int sscanf(const char *s, const char *f, ...) { return 0; }
"""


def _code_unchanged(tmp_path, body):
    """the records (.tcc.taint, only with the flag) never change the code:
    -Wno-tainted-format gives the same object, and the linked outputs of the
    objects with and without them are the same"""
    (tmp_path / "stubs.c").write_text(STUBS)
    assert subprocess.run([str(TCC), "-ccompat", "-c", str(tmp_path / "stubs.c"), "-o", str(tmp_path / "s.o")]).returncode == 0
    for opt in ("-O0", "-O2"):
        _run(tmp_path, body, opt)
        a = (tmp_path / "t.o").read_bytes()
        assert b".tcc.taint" not in a
        _run(tmp_path, body, opt, "-Wtainted-format", "-Wno-tainted-format")
        assert (tmp_path / "t.o").read_bytes() == a
        outs = []
        for flags in ((), ("-Wtainted-format",)):
            _run(tmp_path, body, opt, *flags)
            assert (b".tcc.taint" in (tmp_path / "t.o").read_bytes()) == bool(flags)
            link = [str(TCC), "-ccompat", *LINK_RT, *flags, str(tmp_path / "t.o"), str(tmp_path / "s.o"), "-nostdlib", "-o",
                    str(tmp_path / "out")]
            assert subprocess.run(link, capture_output=True).returncode == 0
            outs.append((tmp_path / "out").read_bytes())
        assert outs[0] == outs[1]


def test_objects_unchanged(tmp_path):
    _code_unchanged(tmp_path, HITS)


def test_ccompat_still_reports(tmp_path):
    proc = _run(tmp_path, HITS, "-Wtainted-format", ccompat=True)
    assert _lines(proc)


# Globals, struct members, and the unit's functions: a helper returning
# getenv(), one filling a buffer with read(), both defined after main; a global
# set by a helper; a parameter passed through to the result; a format reached
# inside a helper (reported there, with a note at the call)
ACROSS = """\
static char *gfmt;
char gbuf[64];
struct cfg { int n; const char *name; const char *fmt; char line[32]; };
struct cfg gcfg;
const char *env_fmt(void);
void fill(char *b, size_t n);
static void setup(void) { gfmt = getenv("FMT"); }
const char *pass(const char *s) { return s; }
static void say(const char *s) { printf(s); }
int main(int argc, char **argv)
{
  struct cfg c;
  char buf[64], other[64];
  c.name = "name";
  c.fmt = getenv("F");
  printf(c.fmt);
  printf(c.name);
  gcfg.fmt = argv[1];
  printf(gcfg.fmt);
  printf(gcfg.name);
  setup();
  printf(gfmt);
  printf(env_fmt());
  fill(buf, sizeof buf);
  printf(buf);
  printf(pass("lit"));
  printf(pass(argv[2]));
  read(0, gbuf, sizeof gbuf);
  printf(gbuf + 1);
  say(argv[0]);
  c.fmt = "ok";
  printf(c.fmt);
  char *p = other;
  fgets(p, 64, 0);
  printf(other);
  fgets(c.line, sizeof c.line, 0);
  printf(c.line);
  printf(c.name);
  return 0;
}
const char *env_fmt(void) { return getenv("G"); }
void fill(char *b, size_t n) { read(0, b, n); }
"""

ACROSS_HITS = [9, 16, 19, 22, 23, 25, 27, 29, 35, 37]


@pytest.mark.parametrize("opt", ["-O0", "-O1", "-O2", "-Os"])
def test_globals_members_summaries(tmp_path, opt):
    proc = _run(tmp_path, ACROSS, "-Wtainted-format", opt)
    assert proc.returncode == 0
    # clean: c.name (17, 38: another member), gcfg.name (20), pass("lit")
    # (26), c.fmt stored again with a literal (32)
    assert _lines(proc) == ACROSS_HITS
    assert _notes(proc) == [30]
    msgs = {m for l, k, m in _diags(proc) if k == "note"}
    assert msgs == {"untrusted input reaches it through the call to 'say' in 'main'"}


# What a helper writes through a struct pointer, member by member; a format
# two calls down; recursion; a result picked from the parameters
DEEP = """\
struct cfg { int n; const char *name; const char *fmt; };
static void init(struct cfg *c) { c->fmt = getenv("X"); c->name = "n"; }
static void inner(const char *f, int n) { if (n) printf(f); }
static void outer(int n, const char *f) { inner(f, n); }
static int count(const char *s, int n) { return n ? count(s, n - 1) : printf(s); }
static const char *pick(const char *a, const char *b, int k) { return k ? a : b; }
int main(int argc, char **argv)
{
  struct cfg c;
  init(&c);
  printf(c.name);
  printf(c.fmt);
  outer(argc, argv[1]);
  outer(argc, "fixed");
  count(argv[1], 3);
  printf(pick("a", "b", argc));
  printf(pick("a", getenv("Y"), argc));
  return 0;
}
"""


@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_summaries_deep(tmp_path, opt):
    proc = _run(tmp_path, DEEP, "-Wtainted-format", opt)
    assert _lines(proc) == [3, 5, 12, 17]
    assert _notes(proc) == [13, 15]


def test_pragma_at_the_sink_in_a_helper(tmp_path):
    body = ACROSS.replace(
        "static void say(const char *s) { printf(s); }",
        '#pragma GCC diagnostic push\n#pragma GCC diagnostic ignored "-Wtainted-format"\n'
        "static void say(const char *s) { printf(s); }\n#pragma GCC diagnostic pop",
    )
    proc = _run(tmp_path, body, "-Wtainted-format")
    assert 11 not in _lines(proc) and _notes(proc) == []
    assert len(_lines(proc)) == len(ACROSS_HITS) - 1


def test_across_objects_unchanged(tmp_path):
    _code_unchanged(tmp_path, ACROSS)


# scanf & co: %s, %[...] and %c fill their buffers with untrusted bytes
# (sscanf: with its input string's); a format that is not a literal fills
# every argument after it
SCANF = """\
int scanf(const char *, ...);
int fscanf(void *, const char *, ...);
int sscanf(const char *, const char *, ...);
int main(int argc, char **argv)
{
  char a[32], b[32], c[32], d[32], e[8], f[32], g[32];
  int n;
  scanf("%d %31s", &n, a);
  printf(a);
  scanf("%d", &n);
  printf(b);
  fscanf(0, "%*s %[a-z]", c);
  printf(c);
  sscanf("fixed", "%s", d);
  printf(d);
  sscanf(getenv("X"), "%5c", e);
  printf(e);
  scanf(argc ? "%s" : "%d", f);
  printf(f);
  scanf("%*s %d", g);
  printf(g);
  return 0;
}
"""


@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_scanf_buffers(tmp_path, opt):
    proc = _run(tmp_path, SCANF, "-Wtainted-format", opt)
    # clean: b (no %s), d (sscanf of a literal), g (%*s is not stored)
    assert _lines(proc) == [9, 13, 17, 19]


# A call through a pointer: the join of the summaries of the functions of
# its type whose address is taken (in code or in an initializer)
INDIRECT = """\
static void say(const char *s) { printf(s); }
static void quiet(const char *s) { printf("%s", s); }
static void shout(const char *s) { printf(s); }
static const char *env(int k) { return getenv("X"); }
static int twice(int x) { return 2 * x; }
struct ops { void (*out)(const char *); };
static const struct ops table = { shout };
int main(int argc, char **argv)
{
  void (*fp)(const char *) = argc ? say : quiet;
  fp("fixed");
  fp(argv[1]);
  const char *(*g)(int) = env;
  printf(g(1));
  int (*h)(int) = twice;
  printf((const char *)h(1));
  void (*any)() = 0;
  any(argv[2]);
  table.out("fixed");
  return 0;
}
"""


@pytest.mark.parametrize("opt", ["-O0", "-O1", "-O2"])
def test_indirect_calls(tmp_path, opt):
    proc = _run(tmp_path, INDIRECT, "-Wtainted-format", opt)
    # say and shout (fp, and 'any' without a prototype), g's env; not h:
    # 'int (int)' is not env's type
    assert _lines(proc) == [1, 3, 14]
    notes = sorted((l, m) for l, k, m in _diags(proc) if k == "note")
    assert notes == [
        (12, "untrusted input reaches it through a call through a pointer in 'main', which may call 'say'"),
        (12, "untrusted input reaches it through a call through a pointer in 'main', which may call 'shout'"),
    ]


# ---------------------------------------------------------------------------
# Across units: with -c each unit's functions go in ".tcc.taint" (only with
# the flag); the link with -Wtainted-format solves the summaries over the
# program and reports the flows that cross units, at the sink, with a note at
# the call that crosses (or, for a source another unit's function gives, at
# that call).  What a unit said by itself is not said again.

XA = """\
int printf(const char *, ...);
void say(const char *s);
const char *env_fmt(void);
void fill(char *b, int n);
void wrap(int k, const char *s);
void get_word(char *b);
void run_a(const char *s);
extern void (*hook)(const char *);
int main(int argc, char **argv)
{
  char buf[64], w[32];
  say(argv[1]);
  say("fixed");
  printf(env_fmt());
  fill(buf, sizeof buf);
  printf(buf);
  wrap(1, argv[2]);
  get_word(w);
  printf(w);
  run_a(argv[3]);
  hook(argv[4]);
  printf(argv[0]);
  return 0;
}
"""

XB = """\
int printf(const char *, ...);
char *getenv(const char *);
int read(int, void *, unsigned);
int scanf(const char *, ...);
void say(const char *s)
{
  printf(s);
}
const char *env_fmt(void) { return getenv("F"); }
void fill(char *b, int n) { read(0, b, n); }
static void inner(const char *s) { printf(s); }
void wrap(int k, const char *s) { if (k) inner(s); }
void get_word(char *b) { scanf("%31s", b); }
static void emit(const char *s) { printf(s); }
void run_b(const char *s) { emit(s); }
static void loud(const char *s) { printf(s); }
void (*hook)(const char *) = loud;
"""

XC = """\
int printf(const char *, ...);
static void emit(const char *s) { printf("%s", s); }
void run_a(const char *s) { emit(s); }
"""

XSTUBS = """\
void _start(void) {}
int printf(const char *f, ...) { return 0; }
char *getenv(const char *n) { return 0; }
int read(int f, void *b, unsigned n) { return 0; }
int scanf(const char *f, ...) { return 0; }
"""

X_WARN = [
    "b.c:7: warning: the format of 'printf' comes from untrusted input: write printf(\"%s\", ...)",
    "a.c:12: note: untrusted input reaches it through the call to 'say' in 'main'",
    "a.c:14: warning: the format of 'printf' comes from untrusted input: write printf(\"%s\", ...)",
    "a.c:14: note: untrusted input comes from the call to 'env_fmt' in 'main'",
    "a.c:16: warning: the format of 'printf' comes from untrusted input: write printf(\"%s\", ...)",
    "a.c:15: note: untrusted input comes from the call to 'fill' in 'main'",
    "b.c:11: warning: the format of 'printf' comes from untrusted input: write printf(\"%s\", ...)",
    "a.c:17: note: untrusted input reaches it through the call to 'wrap' in 'main'",
    "a.c:19: warning: the format of 'printf' comes from untrusted input: write printf(\"%s\", ...)",
    "a.c:18: note: untrusted input comes from the call to 'get_word' in 'main'",
    "b.c:16: warning: the format of 'printf' comes from untrusted input: write printf(\"%s\", ...)",
    "a.c:21: note: untrusted input reaches it through a call through a pointer in 'main', which may call 'loud'",
]
# a.c's own (argv[0], line 22) is said by its compile, not by the link
A_OWN = ["a.c:22: warning: the format of 'printf' comes from untrusted input: write printf(\"%s\", ...)"]


def _xsetup(tmp_path, b=XB):
    for name, src in (("a", XA), ("b", b), ("c", XC), ("st", XSTUBS)):
        (tmp_path / f"{name}.c").write_text(src)


def _xtcc(tmp_path, *args):
    rt = [] if "-c" in args else LINK_RT
    proc = subprocess.run([str(TCC), "-ccompat", *rt, *map(str, args)], capture_output=True, text=True, cwd=tmp_path)
    return proc, [l for l in proc.stderr.splitlines() if l.strip()]


def _xobjects(tmp_path, *flags, opts=("-O0", "-O2", "-O0")):
    for name, opt in zip(("a", "b", "c"), opts):
        proc, out = _xtcc(tmp_path, "-Wtainted-format", opt, *flags, "-c", f"{name}.c", "-o", f"{name}.o")
        assert proc.returncode == 0, proc.stderr
        assert out == (A_OWN if name == "a" and "-Wno-tainted-format" not in flags else []), out
    assert _xtcc(tmp_path, "-c", "st.c", "-o", "st.o")[0].returncode == 0


@pytest.mark.parametrize("opts", [("-O0", "-O0", "-O0"), ("-O2", "-O1", "-Os"), ("-O0", "-O2", "-O1")])
def test_across_units(tmp_path, opts):
    _xsetup(tmp_path)
    _xobjects(tmp_path, opts=opts)
    proc, out = _xtcc(tmp_path, "-Wtainted-format", "a.o", "b.o", "c.o", "st.o", "-nostdlib", "-o", "out")
    assert proc.returncode == 0, proc.stderr
    # run_a reaches c.c's emit, not b.c's emit of the same name: quiet
    assert out == X_WARN


def test_across_units_one_command_archive_r(tmp_path):
    _xsetup(tmp_path)
    proc, out = _xtcc(tmp_path, "-Wtainted-format", "a.c", "b.c", "c.c", "st.c", "-nostdlib", "-o", "out")
    assert proc.returncode == 0 and out == A_OWN + X_WARN
    _xobjects(tmp_path)
    ar = [str(TCC), "-ar", "rcs", "libbc.a", "b.o", "c.o"]
    assert subprocess.run(ar, capture_output=True, cwd=tmp_path).returncode == 0
    proc, out = _xtcc(tmp_path, "-Wtainted-format", "a.o", "st.o", "libbc.a", "-nostdlib", "-o", "out")
    assert out == X_WARN
    proc, out = _xtcc(tmp_path, "-Wtainted-format", "-r", "a.o", "b.o", "c.o", "-o", "r.o")
    assert proc.returncode == 0 and out == []
    proc, out = _xtcc(tmp_path, "-Wtainted-format", "r.o", "st.o", "-nostdlib", "-o", "out")
    assert out == X_WARN


def test_across_units_options(tmp_path):
    _xsetup(tmp_path)
    _xobjects(tmp_path)
    link = ("a.o", "b.o", "c.o", "st.o", "-nostdlib", "-o", "out")
    # the link decides
    proc, out = _xtcc(tmp_path, *link)
    assert proc.returncode == 0 and out == []
    proc, out = _xtcc(tmp_path, "-Wtainted-format", "-Wno-tainted-format", *link)
    assert out == []
    proc, out = _xtcc(tmp_path, "-Werror=tainted-format", *link)
    assert proc.returncode == 1 and out == [l.replace(": warning:", ": error:") for l in X_WARN]
    proc, out = _xtcc(tmp_path, "-Werror", "-Wtainted-format", "-Wno-error=tainted-format", *link)
    assert proc.returncode == 0 and out == X_WARN
    # objects compiled without the flag carry nothing
    _xobjects(tmp_path, "-Wno-tainted-format")
    proc, out = _xtcc(tmp_path, "-Wtainted-format", *link)
    assert out == []


def test_across_units_pragma_at_the_sink(tmp_path):
    b = XB.replace(
        "void say(const char *s)\n",
        '#pragma GCC diagnostic push\n#pragma GCC diagnostic ignored "-Wtainted-format"\nvoid say(const char *s)\n',
    ).replace("  printf(s);\n}\n", "  printf(s);\n}\n#pragma GCC diagnostic pop\n", 1)
    _xsetup(tmp_path, b=b)
    _xobjects(tmp_path)
    proc, out = _xtcc(tmp_path, "-Wtainted-format", "a.o", "b.o", "c.o", "st.o", "-nostdlib", "-o", "out")
    assert out == [l.replace("b.c:11:", "b.c:14:").replace("b.c:16:", "b.c:19:") for l in X_WARN[2:]]
