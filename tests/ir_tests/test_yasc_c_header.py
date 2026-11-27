"""tcc -yasc-c-header: a regular C header written from a yasc one (docs/yasc_c_headers.md).

The yasc header is the one source: a yasc unit reads it through the C header's
`#ifdef __YASC__` include, everything else reads the C declarations written from it.
Also `#pragma tcc c_begin` / `c_end` across an `#include` (the C section stays C).
"""

import shutil
import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
ROOT = CURRENT_DIR.parent.parent
TCC = ROOT / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

ATTRS = """\
#pragma once
#define __yl_nonnull(...) __attribute__((__nonnull__(__VA_ARGS__)))
#define __yl_dealloc(d, n) __attribute__((__malloc__(d, n)))
#define __yl_cstr(n)
#define __yl_view
#define __yl_view_maybe
#define __yl_access(mode, ref, size) __attribute__((__access__(mode, ref, size)))
"""

SOURCE = """\
/* the license */

#pragma yasc
#pragma once

#include <sys/_attrs.h>

#define PICK(p) ((p) ? (p) : nullptr)

#pragma tcc c_begin
struct legacy { char *name; };
int legacy_use(char *p) __yl_nonnull(1);
#pragma tcc c_end

typedef struct rec rec;
struct rec {
  char *name;
  int *?next;
  char buf[8];
};

typedef struct ctx {
  const char *?path;
  long *result;
} ctx;
typedef char ctx_fits[(sizeof(ctx) <= 16) ? 1 : -1];

void rec_free(int flags, drop rec *?owned r);
rec *?owned(rec_free) rec_new(char *name) __yl_nonnull(1);
char *?borrows(s) rec_find(char *s, int c);
long rec_read(inout rec *r, out char *?dst, in char *src);
int rec_open(int owned(close) fd, char *?owned(free) *out_name);
char *borrows(path) rec_base(inout char path[maybe :0]);
int rec_sort(inout rec *base, int (*view cmp)(rec *, rec *));
long rec_fill(int fd, out void buf[?n], unsigned long n);
void rec_copy(out char dst[n], char src[n], unsigned long n);
int rec_sum(unsigned long n, int v[n]);
void rec_bump(inout int v[k], int k);
int rec_pair(out int ?owned(close) fds[2]);
#ifdef __YASC__
int rec_count([]const char names);
#else
int rec_count(const char *names, int n);
#endif
"""

EXPECTED = """\
/* the license */

/* Written by `tcc -yasc-c-header` from yasc/rec.h: edit that file, not this one.
   A yasc unit reads the yasc header itself. */
#ifdef __YASC__
#include "yasc/rec.h"
#else
#pragma once

#include <sys/_attrs.h>

#define PICK(p) ((p) ? (p) : ((void *)0))

struct legacy { char *name; };
int legacy_use(char *p) __yl_nonnull(1);

typedef struct rec rec;
struct rec {
  char *name __yl_view;
  int *next __yl_view_maybe;
  char buf[8];
};

typedef struct ctx {
  const char *path __yl_view_maybe;
  long *result __yl_view;
} ctx;
typedef char ctx_fits[(sizeof(ctx) <= 16) ? 1 : -1];

void rec_free(int flags, rec *r);
rec *rec_new(const char *name) __yl_nonnull(1) __yl_dealloc(rec_free, 2);
char *rec_find(const char *s, int c);
long rec_read(rec *r, char *dst, const char *src);
int rec_open(int fd, char **out_name);
char *rec_base(char path[]) __yl_cstr(1);
int rec_sort(rec *base, int (*cmp)(const rec *, const rec *));
long rec_fill(int fd, void *buf, unsigned long n) __yl_access(write_only, 2, 3);
void rec_copy(char *dst, const char *src, unsigned long n) __yl_access(write_only, 1, 3) __yl_access(read_only, 2, 3);
int rec_sum(unsigned long n, const int *v) __yl_access(read_only, 2, 1);
void rec_bump(int *v, int k) __yl_access(read_write, 1, 2);
int rec_pair(int fds[2]);
int rec_count(const char *names, int n);
#endif /* __YASC__ */
"""


def _tree(tmp_path, source=SOURCE):
    inc = tmp_path / "inc"
    (inc / "yasc").mkdir(parents=True)
    (inc / "sys").mkdir()
    (inc / "sys" / "_attrs.h").write_text(ATTRS)
    (inc / "yasc" / "rec.h").write_text(source)
    return inc


def _gen(inc, *extra):
    return subprocess.run([str(TCC), "-yasc-c-header", "yasc/rec.h", *extra], cwd=inc, capture_output=True, text=True)


def test_translation(tmp_path):
    inc = _tree(tmp_path)
    proc = _gen(inc)
    assert proc.returncode == 0, proc.stderr
    assert proc.stdout == EXPECTED


@pytest.mark.skipif(shutil.which("gcc") is None, reason="needs gcc")
def test_c_header_is_c99(tmp_path):
    inc = _tree(tmp_path)
    assert _gen(inc, "-o", "rec.h").returncode == 0
    use = tmp_path / "use.c"
    use.write_text("#include <rec.h>\nint main(void) { return rec_find(\"x\", 'x') != 0; }\n")
    proc = subprocess.run(["gcc", "-std=c99", "-Wall", "-Wextra", "-pedantic", "-Werror", "-fsyntax-only", f"-I{inc}",
                           str(use)], capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr


def test_sized_parameter_checked_from_c(tmp_path):
    """`T p[n]` is `T *p` in C with __yl_access: a C caller's call is checked as a yasc one's."""
    inc = _tree(tmp_path)
    assert _gen(inc, "-o", "rec.h").returncode == 0
    use = tmp_path / "use.c"
    use.write_text("#include <rec.h>\nvoid f(const char *s) { char d[4]; rec_copy(d, s, 8); }\n")
    proc = subprocess.run([str(TCC), "-ccompat", "-ffortify", "-Wstringop-overflow", "-c", f"-I{inc}", str(use), "-o", str(tmp_path / "use.o")], capture_output=True,
                          text=True)
    assert proc.returncode == 0, proc.stderr
    assert "'rec_copy' accesses 8 bytes of an object of 4" in proc.stderr, proc.stderr


def test_yasc_unit_reads_the_source(tmp_path):
    inc = _tree(tmp_path)
    assert _gen(inc, "-o", "rec.h").returncode == 0
    use = tmp_path / "use.c"
    use.write_text("#include <rec.h>\n"
                   "int f(void) { return *rec_find(\"x\", 'x'); }\n"
                   "void g(void) { rec *?owned r = rec_new(\"a\"); rec_free(0, r); }\n")
    proc = subprocess.run([str(TCC), "-std=yasc", "-fsyntax-only", "-Wno-missing-prototypes", f"-I{inc}", str(use)],
                          capture_output=True, text=True)
    assert "use.c:2: error: a pointer that may be null is dereferenced" in proc.stderr, proc.stderr
    assert "use.c:3" not in proc.stderr, proc.stderr
    c_use = tmp_path / "c_use.c"
    c_use.write_text("#include <rec.h>\nint f(void) { return legacy_use(0) + rec_count(\"x\", 1); }\n")
    proc = subprocess.run([str(TCC), "-ccompat", "-fsyntax-only", "-Wall", f"-I{inc}", str(c_use)], capture_output=True,
                          text=True)
    assert "c_use.c:2: warning: null passed as argument 1, which is declared nonnull" in proc.stderr, proc.stderr


def test_written_only_when_it_changes(tmp_path):
    inc = _tree(tmp_path)
    assert _gen(inc, "-o", "rec.h").returncode == 0
    out = inc / "rec.h"
    old = out.stat().st_mtime_ns
    out.touch()
    stamp = out.stat().st_mtime_ns
    assert _gen(inc, "-o", "rec.h").returncode == 0
    assert out.stat().st_mtime_ns == stamp and stamp >= old


@pytest.mark.parametrize("source,message", [
    ("#pragma yasc\nint f([]const char s);\n", "a yasc slice has no C form"),
    ("#pragma yasc\nint f(int x)\n", "a declaration without its ';'"),
    ("#pragma yasc\n#pragma tcc c_begin\nint f(char *p);\n", "#pragma tcc c_begin without its c_end"),
    ("#pragma yasc\nstatic inline int f(void) { defer g(); return 0; }\n", "a yasc statement in an inline function"),
    ("#pragma yasc\n#ifdef __YASC__\nint f(void);\n", "a conditional without its #endif"),
])
def test_errors(tmp_path, source, message):
    inc = _tree(tmp_path, source)
    proc = _gen(inc, "-o", "rec.h")
    assert proc.returncode == 1 and message in proc.stderr, proc.stderr
    assert not (inc / "rec.h").exists()


def test_needs_a_yasc_directory(tmp_path):
    (tmp_path / "rec.h").write_text("#pragma yasc\n")
    proc = subprocess.run([str(TCC), "-yasc-c-header", "rec.h"], cwd=tmp_path, capture_output=True, text=True)
    assert proc.returncode == 1 and "not under a directory named yasc" in proc.stderr


@pytest.mark.parametrize("before", ["", "#include \"inline.h\"\n"], ids=["alone", "after_a_saved_body"])
def test_c_section_survives_an_include(tmp_path, before):
    """The pragmas apply where they are: also in the lookahead past a saved (static inline) body."""
    (tmp_path / "other.h").write_text("#define OTHER 1\n")
    (tmp_path / "inline.h").write_text("static inline int up(int c) { return c + 1; }\n")
    (tmp_path / "h.h").write_text("#pragma yasc\n#pragma tcc c_begin\n#include \"other.h\"\n"
                                  "struct s { char *p; };\nint c_fn(char *p);\n#pragma tcc c_end\n"
                                  "char *y_p(void);\n")  # (a function's result says it; a global's is a view)
    (tmp_path / "u.c").write_text(before + "#include \"h.h\"\n")
    proc = subprocess.run([str(TCC), "-std=yasc", "-fsyntax-only", str(tmp_path / "u.c")], capture_output=True,
                          text=True)
    errors = [line for line in proc.stderr.splitlines() if "error:" in line]
    assert len(errors) == 1 and "h.h:7:" in errors[0] and "plain pointer" in errors[0], proc.stderr


def test_c_header_after_a_yasc_one(tmp_path):
    """A yasc header ending in a C section of static inline functions, then a plain C header: the
    dialect markers read in the lookahead past the saved bodies are not applied again in the C header."""
    (tmp_path / "y.h").write_text("#pragma yasc\n#pragma tcc c_begin\nstatic inline int up(int c) { return c + 1; }\n"
                                  "#pragma tcc c_end\n")
    (tmp_path / "c.h").write_text("int c_fn(char *p);\n")
    (tmp_path / "u.c").write_text("#include \"y.h\"\n#include \"c.h\"\n")
    proc = subprocess.run([str(TCC), "-std=yasc", "-fsyntax-only", str(tmp_path / "u.c")], capture_output=True,
                          text=True)
    assert "error" not in proc.stderr, proc.stderr


def test_parameter_words_used_as_names(tmp_path):
    """out / inout / drop are words where a parameter starts (or before an int
    handle's words) and names everywhere else: the names stay in the C header"""
    inc = _tree(tmp_path, "#pragma yasc\n#pragma once\n"
                "struct w { int out; int drop; char *inout; };\n"
                "void name(out char *out, int drop, inout int *inout);\n"
                "int close(int drop ?owned(close) fd);\n"
                "static inline int sum(int out, int in) { int drop = out + in; return drop; }\n")
    proc = _gen(inc)
    assert proc.returncode == 0, proc.stderr
    assert "struct w { int out; int drop; char *inout __yl_view; };" in proc.stdout, proc.stdout
    assert "void name(char *out, int drop, int *inout);" in proc.stdout, proc.stdout
    assert "int close(int fd);" in proc.stdout, proc.stdout
    assert "static inline int sum(int out, int in) { int drop = out + in; return drop; }" in proc.stdout, proc.stdout
