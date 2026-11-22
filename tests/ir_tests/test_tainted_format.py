"""-Wtainted-format: a printf-style format that comes from untrusted input
(ir/taint.c, raw IR before any pass).

Sources: main's argv, getenv/secure_getenv results, the buffers fgets, gets,
fread, read, pread, recv, recvfrom, getline fill, and functions declared
__attribute__((tainted_result)) / tainted_arg(n).  Sinks: the format argument
of a format(printf, n, m) function, or of the printf family by name.  Off by
default, not in -Wall.  Followed through globals, struct members and the
unit's functions (summaries computed at the end of the unit, callees defined
later included; a format reached through a call is reported at the sink with
a note at the call).
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


def test_objects_unchanged(tmp_path):
    for opt in ("-O0", "-O2"):
        _run(tmp_path, HITS, opt)
        a = (tmp_path / "t.o").read_bytes()
        _run(tmp_path, HITS, opt, "-Wtainted-format")
        assert (tmp_path / "t.o").read_bytes() == a


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
    for opt in ("-O0", "-O2"):
        _run(tmp_path, ACROSS, opt)
        a = (tmp_path / "t.o").read_bytes()
        _run(tmp_path, ACROSS, opt, "-Wtainted-format")
        assert (tmp_path / "t.o").read_bytes() == a
