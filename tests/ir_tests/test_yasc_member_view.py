"""A struct member from regular C that says what it holds: yasc_view.

A pointer written in regular C is unchecked in yasc code: dereferencing it
needs `unsafe` (test_yasc_unsafe.py).  A regular-C header can say more without
a yasc copy of its structs: `__attribute__((yasc_view))` on a member makes its
pointer `view`, with `yasc_maybe` `?view` -- believed, as malloc is
(test_yasc_ownership.py).  libc spells them __yl_view / __yl_view_maybe
(sys/_attrs.h), nothing outside tcc.  Regular C (-ccompat) sees no change.
"""

import subprocess
from pathlib import Path

import pytest

from cross_includes import INCLUDE_FLAGS

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

# regular C, as libc's sys/syscall.h
HEADER = """\
#define V __attribute__((__yasc_view__))
#define VM __attribute__((__yasc_view__, __yasc_maybe__))
typedef struct ctx {
  int fd;
  const void *buf V;
  long *result V;
  long *opt VM;
  long *plain;
  long *a V, *b;
} ctx;
struct spec { __attribute__((yasc_view)) int *x, *y; };
"""

UNCHECKED = "a pointer from regular C is dereferenced: write unsafe { } around it"


def _compile(tmp_path, body, *flags):
    (tmp_path / "h.h").write_text(HEADER)
    src = tmp_path / "t.c"
    src.write_text('#include "h.h"\n' + body + "\n")
    return subprocess.run(
        [str(TCC), "-Wno-missing-prototypes", *INCLUDE_FLAGS, "-c", f"-I{tmp_path}", *flags, str(src), "-o", str(tmp_path / "t.o")],
        capture_output=True,
        text=True,
    )


def _diagnostics(proc):
    return [line for line in proc.stderr.splitlines() if "error: " in line or "warning: " in line]


@pytest.mark.parametrize("opt", ["-O0", "-O1"])
def test_view_member_needs_no_unsafe(tmp_path, opt):
    """The consumer dereferences the view and the checked maybe; the producer
    hands it a local for the call."""
    body = """\
void consume(void *args)
{
  const ctx *view c = cast<ptr(const ctx *view)>(args);
  *c->result = cast<as(long)>(c->fd);
  if (c->opt)
    *c->opt = 2;
  *c->a = 3;
}
int spec(struct spec *s) { return *s->x + *s->y; }
long produce(void)
{
  long r = 0, o = 0, a = 0;
  ctx c = {.fd = 1, .buf = nullptr, .result = &r, .opt = &o, .plain = nullptr, .a = &a, .b = nullptr};
  consume(&c);
  return r + o + a;
}"""
    proc = _compile(tmp_path, body, opt)
    assert proc.returncode == 0, proc.stderr
    assert _diagnostics(proc) == []


@pytest.mark.parametrize(
    "body, message",
    [
        ("long f(ctx *c) { return *c->plain; }", UNCHECKED),
        # the attribute is one declarator's: `*a V, *b` leaves b unchecked
        ("long f(ctx *c) { return *c->b; }", UNCHECKED),
        ("void f(ctx *c) { *c->opt = 1; }", "'c->opt' may be null where it is dereferenced"),
        ("static ctx g;\nvoid f(void) { long r; g.result = &r; }", "the address of a local variable is stored in global 'g', which outlives 'f'"),
    ],
    ids=["unannotated", "next_declarator", "maybe_unchecked", "local_escapes"],
)
@pytest.mark.parametrize("opt", ["-O0", "-O1"])
def test_view_member_is_checked(tmp_path, body, message, opt):
    proc = _compile(tmp_path, body, opt)
    assert proc.returncode != 0 and f"error: {message}" in proc.stderr, proc.stderr


def test_misplaced_attributes_warn(tmp_path):
    (tmp_path / "w.h").write_text("struct w { int n __attribute__((yasc_view)); long *m __attribute__((yasc_maybe)); };\n")
    proc = _compile(tmp_path, '#include "w.h"\nint f(struct w *s) { return s->n; }')
    assert proc.returncode == 0, proc.stderr
    assert [d.split(": ", 2)[2] for d in _diagnostics(proc)] == [
        "yasc_view on 'n', which is not a pointer: ignored",
        "yasc_maybe on 'm' without yasc_view: ignored",
    ]


def test_has_attribute(tmp_path):
    """libc's __yl_view is empty where tcc does not know the attribute."""
    body = """\
#if !__has_attribute(__yasc_view__) || !__has_attribute(yasc_maybe)
#error no yasc_view
#endif"""
    proc = _compile(tmp_path, body, "-E")
    assert proc.returncode == 0, proc.stderr


def test_regular_c_sees_no_change(tmp_path):
    body = """\
long g;
long f(ctx *c) { c->result = &g; c->plain = c->result; return *c->b + *c->plain; }"""
    proc = _compile(tmp_path, body, "-ccompat", "-Wall")
    assert proc.returncode == 0, proc.stderr
    assert _diagnostics(proc) == []
