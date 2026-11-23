"""-Waddress-of-packed-member (-Wall, so the safe default too; gcc has it on
by default): the address of a member a packed struct (or #pragma pack)
places below its type's alignment, converted -- assigned, passed, returned,
cast -- to a pointer whose target needs more alignment than the place
guarantees.  `&p->u32` in a `uint32_t *` faults on Cortex-M23 at the first
access, and on M33 once tcc uses LDRD/LDM on it.

A pointer to char or void takes the address as it is (memcpy(&v, &p->u, 4)),
except an explicit (void *) cast, which hands it to the conversion after it,
as gcc's check sees through it.  An array member decaying counts, and so
does a member of a packed member (`&n->inner.u`).  A pointer to a packed
struct converted to an over-aligned pointer is -Wcast-align's (gcc 15 does
not report it here either).
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

LEVELS = ["-O0", "-O1", "-O2", "-Os"]


def _run(tmp_path, body, *flags, compat=True, out="t.o"):
    src = tmp_path / "t.c"
    src.write_text(body)
    cmd = [str(TCC)] + (["-ccompat"] if compat else []) + ["-c", *flags, str(src), "-o", str(tmp_path / out)]
    return subprocess.run(cmd, capture_output=True, text=True)


def _diags(proc, kind="warning"):
    out = []
    for line in proc.stderr.splitlines():
        parts = line.split(": ", 2)
        if len(parts) == 3 and parts[1] == kind:
            out.append((int(parts[0].rsplit(":", 1)[1]), parts[2]))
    return out


MSG = "taking address of packed member of 'struct P' may result in an unaligned pointer value"

BODY = """\
typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32;
struct __attribute__((packed)) P { u8 c; u32 u; u16 h; u8 b[4]; u32 arr[2]; };
struct N { u8 c; u8 d; u16 h; struct P in; };
struct C { char a; char b; };
void take(u32 *p);
void *memcpy(void *, const void *, unsigned);
u32 *ret(struct P *p) { return &p->u; }
void f(struct P *p, struct N *n, struct C *c, u32 v) {
  u32 *a = &p->u;
  void *ok1 = &p->u;
  u8 *ok2 = (u8 *)&p->u;
  u8 *ok3 = &p->c;
  char *ok4 = &c->b;
  u16 *ok5 = &n->h;
  take(&p->u);
  u32 *b = (u32 *)&p->u;
  u32 *d = p->arr;
  u32 *e = &n->in.u;
  u32 *g = (u32 *)p;
  u8 *ok6 = (u8 *)p;
  memcpy(&v, &p->u, 4);
  u32 *h = (void *)&p->u;
  u8 *ok7 = p->b;
  u32 ok8 = *&p->u + p->u;
  (void)a; (void)ok1; (void)ok2; (void)ok3; (void)ok4; (void)ok5; (void)b; (void)d; (void)e; (void)g;
  (void)ok6; (void)h; (void)ok7; (void)ok8;
}
"""

# the same lines as arm-none-eabi-gcc 15 -Wall
EXPECTED = [(n, MSG) for n in (7, 9, 15, 16, 17, 18, 22)]


@pytest.mark.parametrize("opt", LEVELS)
def test_hits(tmp_path, opt):
    proc = _run(tmp_path, BODY, "-Wall", opt)
    assert proc.returncode == 0, proc.stderr
    assert _diags(proc) == EXPECTED, proc.stderr


def test_pragma_pack(tmp_path):
    body = (
        "#pragma pack(push, 1)\n"
        "struct P { char c; int i; short s; };\n"
        "#pragma pack(pop)\n"
        "int *f(struct P *p) { return &p->i; }\n"
        "short *g(struct P *p) { return &p->s; }\n"
    )
    proc = _run(tmp_path, body, "-Waddress-of-packed-member")
    assert [d[0] for d in _diags(proc)] == [4, 5], proc.stderr


def test_aligned_place_in_a_packed_struct_is_fine(tmp_path):
    # u at offset 4 of a packed struct still has alignment 1: the struct does
    body = (
        "struct __attribute__((packed)) Q { int a; int u; };\n"
        "struct __attribute__((packed, aligned(4))) R { int a; int u; char c; };\n"
        "int *f(struct Q *q) { return &q->u; }\n"
        "int *g(struct R *r) { return &r->u; }\n"
    )
    proc = _run(tmp_path, body, "-Waddress-of-packed-member")
    assert [d[0] for d in _diags(proc)] == [3], proc.stderr


def test_switches(tmp_path):
    assert _diags(_run(tmp_path, BODY)) == []
    assert _diags(_run(tmp_path, BODY, "-Waddress-of-packed-member")) == EXPECTED
    assert _diags(_run(tmp_path, BODY, "-Wall", "-Wno-address-of-packed-member")) == []


def test_werror(tmp_path):
    proc = _run(tmp_path, BODY, "-Werror=address-of-packed-member")
    assert proc.returncode != 0 and f"error: {MSG}" in proc.stderr, proc.stderr
    proc = _run(tmp_path, BODY, "-Wall", "-Werror", "-Wno-error=address-of-packed-member")
    assert proc.returncode == 0 and _diags(proc) == EXPECTED, proc.stderr


def test_pragma(tmp_path):
    body = (
        "struct __attribute__((packed)) P { char c; int u; };\n"
        "int *a(struct P *p) { return &p->u; }\n"
        "#pragma GCC diagnostic push\n"
        '#pragma GCC diagnostic ignored "-Waddress-of-packed-member"\n'
        "int *b(struct P *p) { return &p->u; }\n"
        "#pragma GCC diagnostic pop\n"
        "int *c(struct P *p) { return &p->u; }\n"
    )
    for opt in ("-O0", "-O2"):
        assert [d[0] for d in _diags(_run(tmp_path, body, "-Wall", opt))] == [2, 7]


def test_safe_default(tmp_path):
    body = (
        "struct __attribute__((packed)) P { char c; int u; };\n"
        "int * view f(struct P * view p) { return &p->u; }\n"
        "int * view g(struct P * view p) { int * view q = (int * view)&p->u; return q; }\n"
        "void * view h(struct P * view p) { return &p->u; }\n"
    )
    proc = _run(tmp_path, body, compat=False)
    assert proc.returncode == 0, proc.stderr
    assert _diags(proc) == [(2, MSG), (3, MSG)], proc.stderr


def test_not_in_a_system_header(tmp_path):
    inc = tmp_path / "sys"
    inc.mkdir()
    (inc / "s.h").write_text(
        "struct __attribute__((packed)) P { char c; int u; };\n"
        "static inline int *pu(struct P *p) { return &p->u; }\n"
    )
    body = "#include <s.h>\nint g(struct P *p) { return *pu(p); }\n"
    for opt in ("-O0", "-O2"):
        proc = _run(tmp_path, body, "-Wall", opt, "-isystem", str(inc))
        assert _diags(proc) == [], proc.stderr


def test_once_in_an_inlined_function(tmp_path):
    body = (
        "struct __attribute__((packed)) P { char c; int u; };\n"
        "static inline int rd(struct P *p) { int *q = &p->u; return *q; }\n"
        "int g(struct P *p) { return rd(p) + rd(p + 1); }\n"
    )
    for opt in LEVELS:
        assert _diags(_run(tmp_path, body, "-Wall", opt)) == [(2, MSG)]


@pytest.mark.parametrize("opt", LEVELS)
def test_code_unchanged(tmp_path, opt):
    a = _run(tmp_path, BODY, opt, out="a.o")
    b = _run(tmp_path, BODY, opt, "-Waddress-of-packed-member", out="b.o")
    assert a.returncode == 0 and b.returncode == 0
    assert (tmp_path / "a.o").read_bytes() == (tmp_path / "b.o").read_bytes()


ELEMENTS = """\
struct I { int x; char c; };
struct __attribute__((packed)) P { char c; int arr[4]; struct I items[2]; char s[4]; short sh[2]; };
struct __attribute__((packed, aligned(4))) Q { int a[2]; };
int *g1(struct P *p, int i) { return &p->arr[i]; }
int *g2(struct P *p) { return &p->arr[1]; }
int *g3(struct P *p, int i) { return &p->items[i].x; }
char *g4(struct P *p, int i) { return &p->s[i]; }
short *g5(struct P *p, int i) { return &p->sh[i]; }
int *g6(struct Q *q, int i) { return &q->a[i]; }
struct I *g7(struct P *p, int i) { return &p->items[i]; }
int *g8(struct P *p) { return p->arr; }
void *g9(struct P *p, int i) { return &p->arr[i]; }
int g10(struct P *p, int i) { return p->arr[i] + p->items[i].x; }
"""


@pytest.mark.parametrize("opt", LEVELS)
def test_elements_of_packed_array_members(tmp_path, opt):
    """`&p->arr[i]` and a member of an element of a packed array member, as
    arm-none-eabi-gcc 15 (which names the packed struct)"""
    proc = _run(tmp_path, ELEMENTS, "-Wall", opt)
    assert proc.returncode == 0, proc.stderr
    assert _diags(proc) == [(n, MSG) for n in (4, 5, 6, 8, 10, 11)], proc.stderr
