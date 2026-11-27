"""-Wstringop-overflow, -Wformat-overflow, -Wformat-truncation (builtin/objsize.c).

A call whose destination's size is known (__builtin_object_size's frontend
answer) with a length that is known and too big:

- -Wstringop-overflow (gcc: on by default; -Wall here): memcpy, memmove,
  mempcpy, memset, bzero against the whole object; strcpy, stpcpy, strcat (a
  constant source string), strncpy, stpncpy, strncat (the bound), snprintf's
  bound against the closest subobject (=1 uses the whole object everywhere);
  also __builtin___memcpy_chk and kin with a constant length over their
  object size (what a _FORTIFY_SOURCE header expands to);
- -Wformat-overflow (gcc -Wall): sprintf/vsprintf whose literal format's
  shortest output -- constants formatted, everything else at its shortest --
  and its nul do not fit;
- -Wformat-truncation (gcc -Wall, level 1): snprintf/vsnprintf whose shortest
  output does not fit the constant bound, when nothing reads the result;
  =2 also when something does.

Once per place at every -O level, quiet in system headers.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

PRELUDE = """\
typedef unsigned int size_t;
void *malloc(size_t);
void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
void *memset(void *, int, size_t);
void bzero(void *, size_t);
char *strcpy(char *, const char *);
char *strcat(char *, const char *);
char *strncpy(char *, const char *, size_t);
char *strncat(char *, const char *, size_t);
int sprintf(char *, const char *, ...);
int snprintf(char *, size_t, const char *, ...);
int vsnprintf(char *, size_t, const char *, __builtin_va_list);
struct S { char name[8]; int x; };
"""
PL = PRELUDE.count("\n")

PRELUDE_YASC = """\
typedef unsigned int size_t;
void *view memcpy(void *d, const void *s, size_t n);
int sprintf(char *d, const char *f, ...);
"""


def _run(tmp_path, body, *flags, prelude=PRELUDE, ccompat=True):
    src = tmp_path / "t.c"
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


HITS = """\
void f(const char *src, int c, struct S *ps)
{
  char buf[10];
  struct S s;
  char *p = malloc(16);
  memcpy(buf, src, 20);
  memset(p, 0, 17);
  bzero(buf + 4, 7);
  strcpy(buf, "0123456789");
  strcpy(s.name, "too long name");
  strcpy(ps->name, "too long name");
  strncpy(buf, src, 11);
  strncat(buf, src, 10);
  strcat(buf + 8, "ab");
  snprintf(buf, 20, "%s", src);
  sprintf(buf, "%d-%s", 12345, "abcde");
  sprintf(buf, "%-10d", c);
  sprintf(buf, "%5.3f%%", 3.25);
  snprintf(buf, 5, "%s", "hello");
  __builtin___memcpy_chk(buf, src, 12, __builtin_object_size(buf, 0));
}
"""
HIT_LINES = [
    (6, "'memcpy' writing 20 bytes into a region of size 10 overflows the destination"),
    (7, "'memset' writing 17 bytes into a region of size 16 overflows the destination"),
    (8, "'bzero' writing 7 bytes into a region of size 6 overflows the destination"),
    (9, "'strcpy' writing 11 bytes into a region of size 10 overflows the destination"),
    (10, "'strcpy' writing 14 bytes into a region of size 8 overflows the destination"),
    (11, "'strcpy' writing 14 bytes into a region of size 8 overflows the destination"),
    (12, "'strncpy' writing 11 bytes into a region of size 10 overflows the destination"),
    (13, "'strncat' specified bound 10 equals destination size 10"),
    (14, "'strcat' writing 3 bytes into a region of size 2 overflows the destination"),
    (15, "'snprintf' specified bound 20 exceeds destination size 10"),
    (16, "'sprintf' writing at least 12 bytes into a region of size 10 overflows the destination"),
    (17, "'sprintf' writing at least 11 bytes into a region of size 10 overflows the destination"),
    (18, "'sprintf' writing at least 7 bytes into a region of size 10 overflows the destination"),
    (19, "'snprintf' output truncated: at least 6 bytes into a destination of size 5"),
    (20, "'memcpy' writing 12 bytes into a region of size 10 overflows the destination"),
]


@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_hits(tmp_path, opt):
    proc = _run(tmp_path, HITS, "-Wall", opt)
    got = [(line, msg) for line, kind, msg in _diags(proc) if kind == "warning"]
    expect = [h for h in HIT_LINES if h[0] != 18]  # 3.250% is 6 + nul: fits
    assert got == expect, proc.stderr


CLEAN = """\
int f(const char *src, int c, unsigned n, struct S *ps)
{
  char buf[10];
  struct S s;
  char *p = malloc(16);
  memcpy(buf, src, 10);
  memcpy(&s, src, sizeof s);        /* across members: the whole object */
  memset(s.name, 0, sizeof s);      /* likewise */
  memcpy(buf, src, n);
  memcpy(src[0] ? buf : p, src, 12); /* not one object */
  memset(p, 0, 16);
  strcpy(buf, "012345678");
  strcpy(buf, src);
  strncpy(buf, src, sizeof buf);
  strncat(buf, src, sizeof buf - 1);
  snprintf(buf, sizeof buf, "%s", src);
  sprintf(buf, "%d", c);
  sprintf(buf, "%s", src);
  sprintf(buf, "%.3s-%d", "abcdef", 99999);
  c = snprintf(buf, 5, "%s", "hello"); /* the result says it was cut */
  for (int i = 0; i < 2; i++)
    memset(p, 0, 32);               /* p declared before the loop: not known there */
  memcpy(ps->name + 1, src, 7);
  return c;
}
"""


def test_clean(tmp_path):
    proc = _run(tmp_path, CLEAN, "-Wall")
    assert proc.returncode == 0 and not proc.stderr, proc.stderr


def test_switches(tmp_path):
    body = "void f(char *s) { char b[4]; memcpy(b, s, 5); sprintf(b, \"%s\", \"abcd\"); }\n"
    sop = "'memcpy' writing 5 bytes"
    fov = "'sprintf' writing at least 5 bytes"
    proc = _run(tmp_path, body)
    assert sop not in proc.stderr and fov not in proc.stderr  # -ccompat alone: neither
    proc = _run(tmp_path, body, "-Wall")
    assert sop in proc.stderr and fov in proc.stderr, proc.stderr
    proc = _run(tmp_path, body, "-Wall", "-Wno-stringop-overflow")
    assert sop not in proc.stderr and fov in proc.stderr, proc.stderr
    proc = _run(tmp_path, body, "-Wall", "-Wno-format-overflow")
    assert sop in proc.stderr and fov not in proc.stderr, proc.stderr
    proc = _run(tmp_path, body, "-Wstringop-overflow")
    assert sop in proc.stderr and fov not in proc.stderr, proc.stderr
    proc = _run(tmp_path, body, "-Wall", "-Werror=stringop-overflow")
    assert proc.returncode != 0 and "error: " + sop in proc.stderr, proc.stderr
    proc = _run(tmp_path, body, "-Wall", "-Werror", "-Wno-error=stringop-overflow", "-Wno-format-overflow")
    assert proc.returncode == 0 and "warning: " + sop in proc.stderr, proc.stderr
    proc = _run(tmp_path, body, "-Wall", "-Wstringop-overflow=0", "-Wformat-overflow=0")
    assert sop not in proc.stderr and fov not in proc.stderr, proc.stderr


def test_levels(tmp_path):
    body = (
        "int f(char *s) { char b[4]; struct S t; strcpy(t.name, \"0123456789\");\n"
        " return snprintf(b, 4, \"%s\", \"abcd\"); }\n"
    )
    tr = "'snprintf' output truncated"
    assert tr not in _run(tmp_path, body, "-Wall").stderr
    assert tr in _run(tmp_path, body, "-Wall", "-Wformat-truncation=2").stderr
    # level 1: the whole object (12 bytes), not the member (8)
    st = "'strcpy' writing 11 bytes"
    assert st in _run(tmp_path, body, "-Wall").stderr
    assert st not in _run(tmp_path, body, "-Wall", "-Wstringop-overflow=1").stderr
    proc = _run(tmp_path, body, "-Wformat-truncation=9")
    assert proc.returncode != 0 and "invalid argument" in proc.stderr, proc.stderr


def test_once_per_place(tmp_path):
    body = (
        "static inline void put(char *d) { memcpy(d, \"0123456789\", 11); }\n"
        "void f(void) { char a[4], b[4]; put(a); put(b); }\n"
    )
    for opt in ("-O0", "-O2"):
        proc = _run(tmp_path, body, "-Wall", opt)
        assert proc.stderr.count("writing 11 bytes") == 0, proc.stderr  # d is a parameter
    body = "static inline void z(void) { char a[4]; memset(a, 0, 8); }\nvoid f(void) { z(); z(); }\n"
    for opt in ("-O0", "-O2"):
        proc = _run(tmp_path, body, "-Wall", opt)
        assert proc.stderr.count("'memset' writing 8 bytes into a region of size 4") == 1, (opt, proc.stderr)


def test_pragma(tmp_path):
    body = (
        "void f(char *s)\n{\n  char b[4];\n"
        "#pragma GCC diagnostic push\n"
        "#pragma GCC diagnostic ignored \"-Wstringop-overflow\"\n"
        "  memcpy(b, s, 5);\n"
        "#pragma GCC diagnostic pop\n"
        "  memcpy(b, s, 6);\n}\n"
    )
    proc = _run(tmp_path, body, "-Wall")
    assert [d[0] for d in _diags(proc)] == [8], proc.stderr


def test_system_header_quiet(tmp_path):
    inc = tmp_path / "sys"
    inc.mkdir()
    (inc / "h.h").write_text(PRELUDE + "static inline void z(void) { char a[4]; memset(a, 0, 8); }\n")
    src = tmp_path / "t.c"
    src.write_text("#include <h.h>\nvoid f(void) { z(); }\n")
    proc = subprocess.run([str(TCC), "-ccompat", "-Wall", "-isystem", str(inc), "-c", str(src), "-o",
                           str(tmp_path / "t.o")], capture_output=True, text=True)
    assert proc.returncode == 0 and not proc.stderr, proc.stderr


def test_safe_default(tmp_path):
    body = "void f(char *s) { char b[4] = undefined; memcpy(b, s, 5); sprintf(b, \"%d\", 12345); }\n"
    proc = _run(tmp_path, body, prelude=PRELUDE_YASC, ccompat=False)
    assert proc.returncode == 0, proc.stderr
    assert "warning: 'memcpy' writing 5 bytes into a region of size 4" in proc.stderr, proc.stderr
    assert "warning: 'sprintf' writing at least 6 bytes into a region of size 4" in proc.stderr, proc.stderr


def test_no_builtin_names(tmp_path):
    """-fno-builtin: memcpy is any function"""
    body = "void f(char *s) { char b[4]; memcpy(b, s, 5); }\n"
    assert "memcpy" not in _run(tmp_path, body, "-Wall", "-fno-builtin").stderr
