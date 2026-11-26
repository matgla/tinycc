"""Allocation checks (builtin/objsize.c).

- -Walloc-size-larger-than=N (gcc: on by default, PTRDIFF_MAX; -Wall here):
  a constant size, or product of calloc-style sizes, over N -- for alloc_size
  functions and malloc/calloc/realloc/aligned_alloc/memalign/alloca by name.
  -Walloc-size-larger-than=4294967295 (SIZE_MAX) or more turns it off.
- -Walloc-mul-overflow (tcc's own; not in -Wall: `malloc(n * sizeof *p)` is
  everywhere, and an earlier test of n is not seen): the size argument of a
  one-size allocator is a multiplication with an operand that is not a
  constant and no known bound (constants, the local facts, a narrow type)
  keeping the product in range.  Suggests calloc / reallocarray.
- -Wfree-nonheap-object (gcc: on by default; -Wall): free, realloc or a
  deallocator named by malloc(dealloc, i) given a local or global object, a
  parameter's address, a string literal, or a followed allocation plus a
  nonzero offset.
- -Wmismatched-dealloc (gcc -Wall): a followed allocation (a variable every
  store so far gave one, or the call itself) given to a deallocator that is
  not its allocator's: malloc's result to fclose, fopen's (malloc(fclose, 1))
  to free.  free and realloc release the free family's.

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
void free(void *);
void *malloc(size_t) __attribute__((malloc, malloc(free, 1)));
void *calloc(size_t, size_t);
void *realloc(void *, size_t);
void *xalloc(int tag, size_t n) __attribute__((alloc_size(2)));
typedef struct FILE FILE;
int fclose(FILE *);
FILE *fopen(const char *, const char *) __attribute__((malloc(fclose, 1)));
void release(void *);
void *pool_get(size_t) __attribute__((malloc(release)));
"""
PL = PRELUDE.count("\n")


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
int g;
void f(size_t n, int c, char *arg)
{
  char buf[10];
  char *p = malloc(16);
  malloc(-1);
  calloc(65536, 65536);
  xalloc(1, 3000000000u);
  free(buf);
  free("lit");
  free(&g);
  free(&arg);
  free(p + 1);
  realloc(buf, 4);
  FILE *fp = fopen("a", "r");
  free(fp);
  fclose((FILE *)p);
  release(malloc(4));
  free(pool_get(8));
  free(p);
}
"""
HIT_LINES = [
    (6, "warning", "argument 1 value '4294967295' exceeds maximum object size 2147483647"),
    (7, "warning", "product '4294967296' of arguments 1 and 2 exceeds maximum object size 2147483647"),
    (8, "warning", "argument 2 value '3000000000' exceeds maximum object size 2147483647"),
    (9, "warning", "'free' called on unallocated object 'buf'"),
    (10, "warning", "'free' called on a pointer to an unallocated object"),
    (11, "warning", "'free' called on unallocated object 'g'"),
    (12, "warning", "'free' called on unallocated object 'arg'"),
    (13, "warning", "'free' called on a pointer to an allocated object with nonzero offset 1"),
    (14, "warning", "'realloc' called on unallocated object 'buf'"),
    (16, "warning", "'free' called on pointer returned from a mismatched allocation function"),
    (15, "note", "returned from 'fopen'"),
    (17, "warning", "'fclose' called on pointer returned from a mismatched allocation function"),
    (5, "note", "returned from 'malloc'"),
    (18, "warning", "'release' called on pointer returned from a mismatched allocation function"),
    (18, "note", "returned from 'malloc'"),
    (19, "warning", "'free' called on pointer returned from a mismatched allocation function"),
    (19, "note", "returned from 'pool_get'"),
    # -Wuse-after-free (ir/bounds.c): fclose released p at line 17
    (20, "warning", "'p' is freed twice"),
]


@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_hits(tmp_path, opt):
    proc = _run(tmp_path, HITS, "-Wall", opt)
    assert _diags(proc) == HIT_LINES, proc.stderr


CLEAN = """\
void f(size_t n, int c, char *arg)
{
  char *p = malloc(16);
  char *q = 0;
  char *r = c ? malloc(4) : 0;
  FILE *fp = fopen("a", "r");
  char *s = pool_get(4);
  if (c)
    q = malloc(8);
  p = realloc(p, 32);
  free(p);
  free(q);
  free(r);
  free(arg);
  free(0);
  fclose(fp);
  release(s);
  malloc(2147483647);
  calloc(2, 1000);
  char *t = malloc(4);
  t = (char *)fopen("b", "r"); /* two deallocators: not known */
  free(t);
}
"""


def test_clean(tmp_path):
    proc = _run(tmp_path, CLEAN, "-Wall")
    assert proc.returncode == 0 and not proc.stderr, proc.stderr


def test_alloc_size_limit(tmp_path):
    body = "void f(void) { malloc(1000); calloc(10, 200); }\n"
    assert not _run(tmp_path, body, "-Wall").stderr
    proc = _run(tmp_path, body, "-Walloc-size-larger-than=999")
    assert [d[2] for d in _diags(proc)] == [
        "argument 1 value '1000' exceeds maximum object size 999",
        "product '2000' of arguments 1 and 2 exceeds maximum object size 999",
    ], proc.stderr
    big = "void f(void) { malloc(-1); }\n"
    assert not _run(tmp_path, big, "-Wall", "-Walloc-size-larger-than=4294967295").stderr
    assert not _run(tmp_path, big, "-Wall", "-Wno-alloc-size-larger-than").stderr
    proc = _run(tmp_path, big, "-Werror=alloc-size-larger-than=100")
    assert proc.returncode != 0 and "error: argument 1 value" in proc.stderr, proc.stderr


MUL = """\
void *reallocarray(void *, size_t, size_t);
void f(size_t n, unsigned char k, unsigned short h, void *v)
{
  malloc(n * 4);
  malloc(n * sizeof(int));
  realloc(v, n * 8);
  malloc(k * 16);
  malloc(h * 4);
  malloc(16 * 4);
  calloc(n, 4);
  malloc(n + 1);
  xalloc(0, n * n);
}
"""


def test_alloc_mul_overflow(tmp_path):
    assert "multiplication" not in _run(tmp_path, MUL, "-Wall").stderr  # not in -Wall
    proc = _run(tmp_path, MUL, "-Walloc-mul-overflow")
    assert _diags(proc) == [
        (4, "warning", "the size argument of 'malloc' is a multiplication that may overflow: use 'calloc' or "
                       "'reallocarray'"),
        (5, "warning", "the size argument of 'malloc' is a multiplication that may overflow: use 'calloc' or "
                       "'reallocarray'"),
        (6, "warning", "the size argument of 'realloc' is a multiplication that may overflow: use 'reallocarray'"),
        (12, "warning", "the size argument of 'xalloc' is a multiplication that may overflow: use 'calloc' or "
                        "'reallocarray'"),
    ], proc.stderr


def test_switches(tmp_path):
    body = "void f(void) { char b[4]; free(b); FILE *fp = fopen(\"x\", \"r\"); free(fp); }\n"
    nh = "'free' called on unallocated object 'b'"
    md = "'free' called on pointer returned from a mismatched"
    proc = _run(tmp_path, body)
    assert nh not in proc.stderr and md not in proc.stderr
    proc = _run(tmp_path, body, "-Wall", "-Wno-free-nonheap-object")
    assert nh not in proc.stderr and md in proc.stderr, proc.stderr
    proc = _run(tmp_path, body, "-Wall", "-Wno-mismatched-dealloc")
    assert nh in proc.stderr and md not in proc.stderr, proc.stderr
    proc = _run(tmp_path, body, "-Wall", "-Werror=free-nonheap-object")
    assert proc.returncode != 0 and "error: " + nh in proc.stderr, proc.stderr
    proc = _run(tmp_path, body, "-Wall", "-Werror", "-Wno-error=free-nonheap-object", "-Wno-error=mismatched-dealloc")
    assert proc.returncode == 0 and "warning: " + nh in proc.stderr and "warning: " + md in proc.stderr


def test_pragma(tmp_path):
    body = (
        "void f(void)\n{\n  char b[4];\n"
        "#pragma GCC diagnostic push\n"
        "#pragma GCC diagnostic ignored \"-Wfree-nonheap-object\"\n"
        "  free(b);\n"
        "#pragma GCC diagnostic pop\n"
        "  free(b + 1);\n}\n"
    )
    proc = _run(tmp_path, body, "-Wall")
    assert [d[0] for d in _diags(proc)] == [8], proc.stderr


def test_once_per_place(tmp_path):
    body = "static inline void z(void) { char a[4]; free(a); }\nvoid f(void) { z(); z(); }\n"
    for opt in ("-O0", "-O2"):
        proc = _run(tmp_path, body, "-Wall", opt)
        assert proc.stderr.count("'free' called on unallocated object 'a'") == 1, (opt, proc.stderr)
    # a parameter is not looked at: the same at -O0 and -O2
    body = "static inline void d(void *p) { free(p); }\nvoid f(void) { char a[4]; d(a); d(\"x\"); }\n"
    for opt in ("-O0", "-O2"):
        assert "called on" not in _run(tmp_path, body, "-Wall", opt).stderr


def test_safe_default(tmp_path):
    prelude = "typedef unsigned int size_t;\nvoid free(drop void * owned maybe p);\n"
    # the cast needs unsafe in safe mode (strict lifetimes); -Wfree-nonheap-object still speaks
    body = "int g;\nvoid f(void)\n{\n  free(unsafe(cast<as(void * owned)>(&g)));\n}\n"
    proc = _run(tmp_path, body, prelude=prelude, ccompat=False)
    assert proc.returncode == 0 and "warning: 'free' called on unallocated object 'g'" in proc.stderr, proc.stderr
