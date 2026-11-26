"""-Wcast-qual and -Wcast-align: what an explicit pointer cast hides.

-Wcast-qual: the cast drops const or volatile from what the pointer points to
(at any level), or adds const below a level that is not const (`char **` to
`const char **` -- gcc's "to be safe all intermediate pointers ...").

-Wcast-align: the cast raises the alignment the target needs -- a char
buffer or a packed struct cast to a pointer to a word.  ARMv8-M Baseline
(Cortex-M23) faults on every unaligned access, Mainline (M33) on LDRD/STRD
and LDM/STM, which tcc emits for 64-bit values and block copies.  As gcc's
-Wcast-align=strict (plain -Wcast-align is a no-op on targets with unaligned
access), except that a `void *` source is never reported (gcc's rule: what it
points to is unknown -- malloc's result, most of the time), nor a cast to or
from an incomplete type.

Both stay outside -Wall and the safe default, as in gcc (not even -Wextra):
over our C, 33 and 103 reports, most of them deliberate -- strchr-like
functions, register blocks, buffers known to be aligned.
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
    cmd = [str(TCC), "-Wno-missing-prototypes"] + (["-ccompat"] if compat else []) + ["-c", *flags, str(src), "-o", str(tmp_path / out)]
    return subprocess.run(cmd, capture_output=True, text=True)


def _diags(proc, kind="warning"):
    out = []
    for line in proc.stderr.splitlines():
        parts = line.split(": ", 2)
        if len(parts) == 3 and parts[1] == kind:
            out.append((int(parts[0].rsplit(":", 1)[1]), parts[2]))
    return out


DISCARD = "cast discards '%s' qualifier from pointer target type"
ALIGN = "cast to '%s' from '%s' increases required alignment from %d to %d"

BODY = """\
typedef unsigned char u8; typedef unsigned int u32; typedef unsigned long long u64;
struct opaque;
struct __attribute__((packed)) P { char c; u32 u; };
void f(const char *s, volatile int *v, char **pp, const char *const *cc, void *vp, u8 *b,
       struct P *pk, struct opaque *o, short *sh, const volatile int *cv) {
  char *a = (char *)s;
  int *b2 = (int *)v;
  const char **c = (const char **)pp;
  const char *const *d = (const char *const *)pp;
  char **e = (char **)cc;
  void *g = (void *)s;
  u32 *h = (u32 *)b;
  u32 *i = (u32 *)vp;
  u32 *j = (u32 *)pk;
  u32 *k = (u32 *)o;
  int *l = (int *)sh;
  char *m = (char *)sh;
  u64 *n = (u64 *)sh;
  u8 buf[8];
  u32 *q = (u32 *)buf;
  int *r = (int *)cv;
  const char *t = (const char *)a;
  (void)a; (void)b2; (void)c; (void)d; (void)e; (void)g; (void)h; (void)i; (void)j; (void)k;
  (void)l; (void)m; (void)n; (void)q; (void)r; (void)t;
}
"""

QUAL = [
    (6, DISCARD % "const"),
    (7, DISCARD % "volatile"),
    (8, "to be safe all intermediate pointers in cast from 'char **' to 'const char **' must be 'const' qualified"),
    (10, DISCARD % "const"),
    (11, DISCARD % "const"),
    (21, DISCARD % "const volatile"),
]
ALIGNS = [
    (12, ALIGN % ("unsigned int *", "unsigned char *", 1, 4)),
    (14, ALIGN % ("unsigned int *", "struct P *", 1, 4)),
    (16, ALIGN % ("int *", "short *", 2, 4)),
    (18, ALIGN % ("unsigned long long *", "short *", 2, 8)),
    (20, ALIGN % ("unsigned int *", "unsigned char *", 1, 4)),
]


@pytest.mark.parametrize("opt", LEVELS)
def test_hits(tmp_path, opt):
    proc = _run(tmp_path, BODY, "-Wcast-qual", "-Wcast-align", opt)
    assert proc.returncode == 0, proc.stderr
    assert _diags(proc) == sorted(QUAL + ALIGNS), proc.stderr


def test_each_alone(tmp_path):
    assert _diags(_run(tmp_path, BODY, "-Wcast-qual")) == QUAL
    assert _diags(_run(tmp_path, BODY, "-Wcast-align")) == ALIGNS


def test_switches(tmp_path):
    assert _diags(_run(tmp_path, BODY)) == []
    assert _diags(_run(tmp_path, BODY, "-Wall")) == []
    assert _diags(_run(tmp_path, BODY, "-Wcast-qual", "-Wcast-align", "-Wno-cast-qual", "-Wno-cast-align")) == []


def test_werror(tmp_path):
    proc = _run(tmp_path, BODY, "-Werror=cast-align")
    assert proc.returncode != 0 and f"error: {ALIGNS[0][1]}" in proc.stderr, proc.stderr
    proc = _run(tmp_path, BODY, "-Wcast-qual", "-Werror", "-Wno-error=cast-qual")
    assert proc.returncode == 0 and _diags(proc) == QUAL, proc.stderr


def test_implicit_conversions_are_not_casts(tmp_path):
    # -Wdiscarded-qualifiers' business, not -Wcast-qual's
    body = "char *f(const char *s) { char *p = s; return p; }\n"
    proc = _run(tmp_path, body, "-Wcast-qual", "-Wcast-align")
    assert all("cast" not in d[1] for d in _diags(proc)), proc.stderr


def test_pragma(tmp_path):
    body = (
        "unsigned *a(unsigned char *p) { return (unsigned *)p; }\n"
        "#pragma GCC diagnostic push\n"
        '#pragma GCC diagnostic ignored "-Wcast-align"\n'
        "unsigned *b(unsigned char *p) { return (unsigned *)p; }\n"
        "#pragma GCC diagnostic pop\n"
        "unsigned *c(unsigned char *p) { return (unsigned *)p; }\n"
    )
    for opt in ("-O0", "-O2"):
        assert [d[0] for d in _diags(_run(tmp_path, body, "-Wcast-align", opt))] == [1, 6]


def test_safe_default(tmp_path):
    # on in the safe default; in yasc the cast that drops const is
    # unsafe_cast<qual(...)> (test_yasc_casts.py), which says it was meant:
    # no -Wcast-qual for it
    body = "char * view f(const char *s)\n{\n  return unsafe_cast<qual(char * view)>(s);\n}\n"
    proc = _run(tmp_path, body, compat=False)
    assert proc.returncode == 0 and _diags(proc) == [], proc.stderr
    proc = _run(tmp_path, body, "-Wno-cast-qual", compat=False)
    assert proc.returncode == 0 and _diags(proc) == [], proc.stderr


def test_not_in_a_system_header(tmp_path):
    inc = tmp_path / "sys"
    inc.mkdir()
    (inc / "s.h").write_text("static inline char *unc(const char *s) { return (char *)s; }\n")
    body = "#include <s.h>\nchar *g(const char *s) { return unc(s); }\n"
    for opt in ("-O0", "-O2"):
        proc = _run(tmp_path, body, "-Wcast-qual", opt, "-isystem", str(inc))
        assert _diags(proc) == [], proc.stderr


def test_once_in_an_inlined_function(tmp_path):
    body = (
        "static inline unsigned w(unsigned char *p) { return *(unsigned *)p; }\n"
        "unsigned g(unsigned char *p) { return w(p) + w(p + 4); }\n"
    )
    for opt in LEVELS:
        assert _diags(_run(tmp_path, body, "-Wcast-align", opt)) == [
            (1, ALIGN % ("unsigned int *", "unsigned char *", 1, 4))]


@pytest.mark.parametrize("opt", LEVELS)
def test_code_unchanged(tmp_path, opt):
    a = _run(tmp_path, BODY, opt, out="a.o")
    b = _run(tmp_path, BODY, opt, "-Wcast-qual", "-Wcast-align", out="b.o")
    assert a.returncode == 0 and b.returncode == 0
    assert (tmp_path / "a.o").read_bytes() == (tmp_path / "b.o").read_bytes()


def test_aligned_places_are_not_reported(tmp_path):
    """an array whose place is aligned enough: its _Alignas, or a member at an
    aligned offset of an aligned struct (as clang); `buf + n` is not known"""
    body = (
        "struct d { int fd; int pos; char buf[16]; char odd; char tail[8]; };\n"
        "struct __attribute__((packed)) pk { char c; char buf[8]; };\n"
        "static unsigned char gbuf[8];\n"
        "static _Alignas(4) unsigned char gabuf[8];\n"
        "void use(void *);\n"
        "void f(struct d *d, struct pk *p) {\n"
        "  _Alignas(8) char abuf[16]; char pbuf[16];\n"
        "  use((unsigned *)d->buf); use((unsigned *)gabuf); use((unsigned *)abuf);\n"  # 8: none
        "  use((unsigned *)(d->buf + d->pos));\n"                                     # 9
        "  use((unsigned *)d->tail);\n"                                               # 10: offset 25
        "  use((unsigned *)p->buf);\n"                                                # 11: packed
        "  use((unsigned *)gbuf); use((unsigned *)pbuf);\n"                           # 12
        "}\n"
    )
    for opt in ("-O0", "-O2"):
        assert [d[0] for d in _diags(_run(tmp_path, body, "-Wcast-align", opt))] == [9, 10, 11, 12, 12]


def test_two_casts_on_one_line_are_two_reports(tmp_path):
    body = "void use(void *, void *);\nvoid f(char *a, short *b) { use((int *)a, (int *)b); }\n"
    assert _diags(_run(tmp_path, body, "-Wcast-align")) == [
        (2, ALIGN % ("int *", "char *", 1, 4)), (2, ALIGN % ("int *", "short *", 2, 4))]
