"""Writes to a local that are dead still perform their volatile accesses."""
import re

import pytest

from test_codegen_asm import ROOT, _compile, _disassemble

# dead_trail_addrvar deleted `x = *r` (r volatile) after x's last read, and the
# trailing store to a volatile x; ptr_local_fwd turned `*(volatile int *)&x`
# into a plain access of x, which SSA then kept in a register.


def _loads_through_reg(insns):
    """ldr from a pointer register: the volatile *r, not a frame slot."""
    return sum(1 for m, a in insns if m.startswith("ldr") and re.search(r"\[r\d+", a))


def _count(insns, prefix):
    return sum(1 for m, _ in insns if m.startswith(prefix))


@pytest.mark.parametrize("opt", ["-O1", "-O2", "-Os"])
def test_dead_trail_keeps_volatile_accesses(opt):
    funcs = _disassemble(_compile("bug_dead_trail_volatile_access", [opt], ROOT / "tests/ir_tests"))
    # Exactly one read of *r each, as at -O0 and gcc -O2.
    for name in ("f_load", "f_add", "f_addr", "f_ptr", "f_ctrl"):
        assert _loads_through_reg(funcs[name]) == 1, (name, funcs[name])
    # A volatile x is stored twice: its initialiser and the trailing write.
    for name in ("f_vlocal", "f_vlocal_ptr"):
        assert _count(funcs[name], "str") == 2, (name, funcs[name])
    # The volatile store and load through &x of a plain local stay.
    assert _count(funcs["f_vcast"], "str") >= 1, funcs["f_vcast"]
    assert _count(funcs["f_vcast_read"], "ldr") >= 1, funcs["f_vcast_read"]
    # The plain neighbour still loses its trailing store and its frame.
    assert _count(funcs["f_plain"], "str") == 0, funcs["f_plain"]
    assert _count(funcs["f_plain"], "ldr") == 0, funcs["f_plain"]
