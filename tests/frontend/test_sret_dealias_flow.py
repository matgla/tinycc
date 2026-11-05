"""A struct-return buffer stays the named destination only if the callee cannot reach it.

tcc_ir_sret_dealias gives the call a fresh buffer and a memmove afterwards when the
destination's address has escaped on a way to the call.  An address can get there
only by a back edge, in two ways its analysis has to follow: a value copied before
the assignment later in the loop that gives it the address, so one walk in index
order does not know it yet; and an escape after the call, which reaches it only
round the loop.
"""

import re
import subprocess

import pytest

from conftest import _find_compiler

SOURCE = """
/* Big enough for -Os to keep the copy as a call, which shows the decision. */
struct S { int a; int rest[255]; };
extern struct S make(int);
extern struct S *seen;
extern void use(int);

/* q = p reads p before p = &x, so the copy learns the address on a later round. */
int copied_before_assigned(int n)
{
  struct S x;
  struct S *p = 0, *q;
  for (int i = 0; i < n; i++)
  {
    q = p;
    seen = q;
    x = make(i);
    use(x.a);
    p = &x;
  }
  return 0;
}

/* Two copies behind, so one more round is not enough either. */
int copied_twice_before_assigned(int n)
{
  struct S x;
  struct S *a = 0, *b = 0, *c = 0;
  for (int i = 0; i < n; i++)
  {
    seen = c;
    x = make(i);
    use(x.a);
    c = b;
    b = a;
    a = &x;
  }
  return 0;
}

/* Escapes after the call: reaches it only round the loop. */
int escapes_after_the_call(int n)
{
  struct S x;
  for (int i = 0; i < n; i++)
  {
    x = make(i);
    use(x.a);
    seen = &x;
  }
  return 0;
}

/* Escapes after the last call on every path: the buffer can stay. */
int escapes_after_the_loop(int n)
{
  struct S x;
  for (int i = 0; i < n; i++)
  {
    x = make(i);
    use(x.a);
  }
  seen = &x;
  return 0;
}

/* Never escapes. */
int never_escapes(int n)
{
  struct S x;
  x = make(n);
  return x.a;
}
"""

COPIES = {
    "copied_before_assigned": True,
    "copied_twice_before_assigned": True,
    "escapes_after_the_call": True,
    "escapes_after_the_loop": False,
    "never_escapes": False,
}


@pytest.fixture
def tcc(request):
    return str(_find_compiler(request.config.getoption("--compiler")).resolve())


def test_fresh_buffer_exactly_where_the_address_can_reach_the_call(tcc, tmp_path):
    """At -Os, which keeps a copy this size as a call.  The pass runs first at every level."""
    src, obj = tmp_path / "t.c", tmp_path / "t.o"
    src.write_text(SOURCE)
    r = subprocess.run([tcc, "-Os", "-fno-pic", "-nostdlib", "-c", str(src), "-o", str(obj)],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    dump = subprocess.run(["arm-none-eabi-objdump", "-dr", str(obj)], capture_output=True, text=True, check=True).stdout
    calls, current = {}, None
    for line in dump.splitlines():
        header = re.match(r"^[0-9a-f]+ <([^>]+)>:", line)
        if header:
            current = header.group(1)
            calls[current] = []
        target = re.search(r"R_ARM_THM_(?:CALL|JUMP24|PC22)\s+(\S+)", line)
        if target and current:
            calls[current].append(target.group(1))
    got = {name: any("memmove" in c for c in calls[name]) for name in COPIES}
    assert got == COPIES, got
