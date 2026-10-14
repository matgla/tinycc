"""-fpic 64-bit global read shapes (bug_pic_ll_global_load_base_alias.c).

A 64-bit lvalue is read as [base] then [base+4].  The low word must not land in
the address register before the high word is read through it, and the address
scratch must not be a destination register saved around its use (its pop
would discard the loaded word).  -fpic is what reserves r9 and leaves the
picker no free register in the nested function.
"""
import re

import pytest

from test_codegen_asm import ROOT, _compile
from test_licm_field_ptr_codegen import _functions

SRC = ROOT / "tests/ir_tests"
NAME = "bug_pic_ll_global_load_base_alias"

_LDR = re.compile(r"ldr(?:\.w|\.n)?")
_MEM = re.compile(r"^(\w+),\s*\[(\w+)(?:,\s*#(-?\d+))?\]")


def _base_clobbers(insns):
    """`ldr rX,[rX,#k]` directly followed by a load of [rX,#k+4]."""
    bad = []
    for (a, m, o), (_, m2, o2) in zip(insns, insns[1:]):
        x, y = _MEM.match(o), _MEM.match(o2)
        if not (_LDR.fullmatch(m) and _LDR.fullmatch(m2) and x and y):
            continue
        if x.group(1) == x.group(2) and y.group(2) == x.group(2) and \
                int(y.group(3) or 0) == int(x.group(3) or 0) + 4:
            bad.append((hex(a), m, o, m2, o2))
    return bad


@pytest.mark.parametrize("opt", ["-O0", "-O1", "-O2", "-Os"])
def test_pic_ll_global_load_keeps_base(opt):
    funcs = _functions(_compile(NAME, [opt, "-fpic"], SRC))
    bad = {n: b for n, i in funcs.items() if (b := _base_clobbers(i))}
    assert not bad, bad
