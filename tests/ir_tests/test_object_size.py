"""__builtin_object_size / __builtin_dynamic_object_size (builtin/objsize.c).

gcc's semantics: type 0/2 the whole object, 1/3 the closest member
subobject; 0/1 an upper bound ((size_t)-1 when unknown), 2/3 a lower bound
(0 when unknown).  Decided in the frontend as the argument is parsed, so the
answer is the same at every -O level (gcc's improves with optimisation):
declared objects, members (`.`, `->`, `[]`, `&`, casts, pointer +/- an
integer), string literals, and allocations -- alloc_size functions and
malloc/calloc/realloc/... by name -- with constant sizes, directly or through
a local pointer variable every store so far gave one (or null).  A variable is
not followed in a loop that began after its declaration (unless it is const),
after a goto label, once its address is taken or something else is stored.
A parameter is never looked at, not even in an inline expansion.  Side
effects in the argument (an assignment, ++, a call) make it unknown, as gcc
says (they are not evaluated).  A variable also follows the addresses of
declared objects, alloca and conditionals of known arms stored to it.
__builtin_dynamic_object_size also answers at run time for a VLA and for
malloc(n) through a variable while n is not stored to.  __has_builtin says 1.

The run-time values (and that every -O level prints the same) are in
objsize_builtin.c (test_qemu.py).
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
void *calloc(size_t, size_t);
void *realloc(void *, size_t);
void *xalloc(size_t a, size_t b) __attribute__((alloc_size(1, 2)));
void *yalloc(int tag, size_t n) __attribute__((__alloc_size__(2)));
void sink(void *);
struct S { int a; char buf[10]; int b; };
struct F { int n; char d[]; };
struct T { struct S s; char tail[4]; };
char g[20];
struct S gs;
#define BOS(p, t, v) _Static_assert(__builtin_object_size(p, t) == (size_t)(v), #p " type " #t)
#define BOSE(p, t, v) (void)sizeof(char[__builtin_object_size(p, t) == (size_t)(v) ? 1 : -1])
"""


def _run(tmp_path, body, *flags, ccompat=True):
    src = tmp_path / "t.c"
    src.write_text(PRELUDE + body)
    cmd = [str(TCC)] + (["-ccompat"] if ccompat else []) + ["-c", *flags, str(src), "-o", str(tmp_path / "t.o")]
    return subprocess.run(cmd, capture_output=True, text=True)


DECLARED = """\
void f(struct S *ps, struct F *pf, char *q, struct T *pt)
{
  char a[10];
  struct S s;
  char m[3][10];
  int x;
  sink(&x);
  BOS(a, 0, 10); BOS(a, 1, 10); BOS(a, 2, 10); BOS(a, 3, 10);
  BOS(&a[3], 0, 7); BOS(a + 3, 1, 7); BOS(a + 10, 0, 0); BOS(a + 12, 0, 0);
  BOS(g, 0, 20); BOS(g + 5, 0, 15); BOS(&g[5], 1, 15);
  BOS(s.buf, 0, 16); BOS(s.buf, 1, 10); BOS(&s.buf[2], 1, 8); BOS(&s.buf[2], 3, 8);
  BOS(&s.b, 0, 4); BOS(&s.b, 1, 4); BOS(&s, 0, 20); BOS((char *)&s + 4, 1, 16);
  BOS(gs.buf, 0, 16); BOS(gs.buf, 1, 10);
  BOS(m[1], 1, 10); BOS(m[1], 0, 20);
  BOS(&x, 0, 4);
  BOS("abc", 0, 4); BOS("abc" + 1, 0, 3);
  BOS(ps->buf, 0, -1); BOS(ps->buf, 1, 10); BOS(ps->buf, 2, 0); BOS(ps->buf, 3, 10);
  BOS(ps->buf + 4, 1, 6); BOS(&ps->buf[4], 3, 6);
  BOS(pt->s.buf, 1, 10); BOS(pt->tail, 1, -1);
  BOS(pf->d, 1, -1); BOS(pf->d, 3, 0);
  BOS(q, 0, -1); BOS(q, 1, -1); BOS(q, 2, 0); BOS(q, 3, 0);
}
"""


@pytest.mark.parametrize("opt", ["-O0", "-O1", "-O2", "-Os"])
def test_declared_objects_and_members(tmp_path, opt):
    proc = _run(tmp_path, DECLARED, opt)
    assert proc.returncode == 0, proc.stderr


ALLOCS = """\
void f(int c, char *q, size_t n)
{
  char *p = malloc(10);
  char *p2 = calloc(3, 4);
  char *p3 = xalloc(5, 6);
  char *p4 = yalloc(1, 7);
  char *p5 = malloc(100);
  char *p6 = q;
  char *p7 = malloc(n);
  struct S *ps = malloc(sizeof *ps);
  char *p8 = 0;
  char *p9;
  char *const k = malloc(12);
  if (c) p5 = malloc(20);
  if (c) p8 = malloc(30);
  p9 = realloc(q, 40);
  BOS(p, 0, 10); BOS(p + 3, 0, 7); BOS(p, 2, 10); BOS(p, 1, 10);
  BOS(p2, 0, 12); BOS(p3, 0, 30); BOS(p4, 1, 7);
  BOS(p5, 0, 100); BOS(p5, 2, 20);
  BOS(p6, 0, -1); BOS(p7, 0, -1); BOS(p7, 2, 0);
  BOS(ps->buf, 0, 16); BOS(ps->buf, 1, 10);
  BOS(p8, 0, -1); BOS(p8, 2, 0);
  BOS(p9, 0, 40);
  BOS(malloc(5), 0, -1); /* side effects in the argument: unknown, as gcc */
  BOS(p + c++, 0, -1); BOS(p + (c = 1), 2, 0);
  while (c--)
  {
    char *in = malloc(3);
    BOS(in, 0, 3);    /* declared in the loop: each trip stores it again */
    BOS(p, 0, -1);    /* declared before the loop: a later store comes back */
    BOS(k, 0, 12);    /* const: only its initializer stores to it */
    p = q;
  }
}
"""


@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_allocations(tmp_path, opt):
    proc = _run(tmp_path, ALLOCS, opt)
    assert proc.returncode == 0, proc.stderr


def test_what_stops_the_following(tmp_path):
    body = """\
void f(int c, char *q)
{
  char *a = malloc(8);
  char *b = malloc(9);
  char *d = malloc(4);
  char *e = malloc(5);
  char *h = malloc(6);
  sink(&*d);
  sink(&e);
  BOS(d, 0, 4);    /* &*d is d's object, not d */
  BOS(e, 0, -1);   /* &e: anything may store to e */
  for (int i = 0; i < c; i++)
  {
    BOS(a, 0, -1); /* in a loop header or body begun after a's declaration */
  }
  while (BOSE(h, 0, -1), c--)
    h = q;
L:
  BOS(b, 0, -1);   /* a goto label: a jump back carries what is stored later */
  if (c)
    goto L;
}
"""
    assert _run(tmp_path, body).returncode == 0
    assert _run(tmp_path, body, "-O2").returncode == 0


def test_parameters_are_not_looked_at(tmp_path):
    """gcc folds these at -O2 through inlining; tcc gives (size_t)-1 at every
    level (inl(a), inl("hello"), inl(p) in objsize_builtin.c run it)."""
    body = """\
static inline size_t inl(char *p) { return __builtin_object_size(p, 0); }
size_t b(void) { return inl(malloc(10)); }
"""
    for opt in ("-O0", "-O1", "-O2"):
        proc = _run(tmp_path, body, opt)
        assert proc.returncode == 0, proc.stderr
        dis = subprocess.run(["arm-none-eabi-objdump", "-d", str(tmp_path / "t.o")], capture_output=True,
                             text=True).stdout
        assert "mvn.w\tr0, #0" in dis, (opt, dis)


def test_dynamic_object_size_compiles(tmp_path):
    body = """\
size_t f(size_t n, int i)
{
  char vla[n];
  char *p = malloc(n);
  char *r = calloc(n, 4);
  char a[5];
  BOS(vla, 0, -1);
  _Static_assert(__builtin_dynamic_object_size(a, 0) == 5, "static when known");
  _Static_assert(__builtin_dynamic_object_size(malloc(3), 2) == 0, "a call: side effects");
  return __builtin_dynamic_object_size(vla, 0) + __builtin_dynamic_object_size(p, 0) +
         __builtin_dynamic_object_size(r, 1) + i;
}
"""
    for opt in ("-O0", "-O2"):
        proc = _run(tmp_path, body, opt)
        assert proc.returncode == 0, proc.stderr


def test_has_builtin(tmp_path):
    body = """\
#if !__has_builtin(__builtin_object_size) || !__has_builtin(__builtin_dynamic_object_size)
#error no
#endif
"""
    proc = _run(tmp_path, body)
    assert proc.returncode == 0, proc.stderr


def test_safe_default(tmp_path):
    body = "void f(void) { char a[10]; _Static_assert(__builtin_object_size(&a[2], 1) == 8, \"\"); }\n"
    src = tmp_path / "s.c"
    src.write_text("typedef unsigned int size_t;\n" + body)
    proc = subprocess.run([str(TCC), "-c", str(src), "-o", str(tmp_path / "s.o")], capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr


def test_objects_byte_identical_with_and_without_checks(tmp_path):
    """The checks never change code: the tracker runs whatever the options."""
    body = """\
int memcmp(const void *, const void *, size_t);
void *memcpy(void *, const void *, size_t);
void free(void *);
int f(const char *s, size_t n)
{
  char *p = malloc(16);
  char buf[8];
  memcpy(buf, s, 8);
  memcpy(p, s, n);
  free(p);
  return memcmp(buf, s, 8) + (int)__builtin_object_size(p, 0);
}
"""
    for opt in ("-O0", "-O1", "-O2", "-Os"):
        objs = []
        for flags in (["-w"], ["-Wall", "-Walloc-mul-overflow", "-Wformat-truncation=2"]):
            src = tmp_path / "b.c"
            src.write_text(PRELUDE + body)
            out = tmp_path / ("b%d.o" % len(objs))
            proc = subprocess.run([str(TCC), "-ccompat", opt, *flags, "-c", str(src), "-o", str(out)],
                                  capture_output=True, text=True)
            assert proc.returncode == 0, proc.stderr
            objs.append(out.read_bytes())
        assert objs[0] == objs[1], opt


def test_variables_holding_addresses(tmp_path):
    """gcc's builtins/*-chk.c test3: a pointer variable given addresses of
    declared objects, alloca blocks and conditionals of them (merged: the
    largest for 0, the smallest for 2)"""
    body = """\
volatile size_t l1 = 1;
void f(void)
{
  struct A { char buf1[10]; char buf2[10]; } a;
  char *r = l1 == 1 ? &a.buf1[5] : &a.buf2[4];
  char buf3[20];
  BOS(r, 0, 15); BOS(r, 2, 6); BOS(r, 1, 15); /* no member through a variable */
  char *s = buf3 + 4;
  BOS(s, 0, 16);
  char *t = l1 ? __builtin_alloca(4) : &a.buf2[7];
  BOS(t, 0, 4); BOS(t, 2, 3); BOS(t + 2, 0, 2);
  char *u = buf3;
  for (int i = 0; i < 4; ++i)
    if (i == l1)
      u = &a.buf1[9];
  BOS(u, 0, 20); BOS(u, 2, 11);
  char *v = buf3;
  v = v + 2;
  BOS(v, 0, 20); BOS(v, 2, 18); /* stores merge: no kill */
}
"""
    for opt in ("-O0", "-O2"):
        proc = _run(tmp_path, body, opt)
        assert proc.returncode == 0, proc.stderr
