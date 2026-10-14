"""Branches over a return's jump and to the epilogue (bug_return_hop.c), and
over a jump that threading orphaned (bug_orphan_jump_hop.c).

- `if (!c) return v;` with v already in the return registers is one
  CBZ/CBNZ to the epilogue, not `cbnz c, L; b <epilogue>; L:`.
- a return that moves its value keeps its jump; a tail-call-only body still
  has an epilogue for the early return to land on.
- a conditional branch to the epilogue is narrowed / fused like any forward
  branch.
- `((1 << n) & *p) != 0` shifts the loaded word: no `1` and no register `lsl`.
- a JUMP every branch to which was threaded past it is not emitted, and the
  `b<c> L; b X; L:` it sat in is one inverted branch.
"""
import re

import pytest

from test_codegen_asm import ROOT, _compile
from test_licm_field_ptr_codegen import _functions
from test_bit_test_probe_codegen import _UNCOND, _is_cond_branch, _moves_one, _register_lsl, _target

SRC = ROOT / "tests/ir_tests"
NAME = "bug_return_hop"


def _funcs(opt, test):
    # One object per test: pytest-xdist workers compiling the same object race.
    return _functions(_compile(NAME, [opt, f"-DHOP_TEST_{test}"], SRC))


def _epilogue_start(insns):
    """Address of the epilogue: the first of the trailing pop / add sp / bx lr run."""
    k = len(insns)
    while k > 0:
        _, m, o = insns[k - 1]
        if m.startswith(("pop", "ldmia")) or (m.startswith("add") and o.startswith("sp")) or (m == "bx" and o == "lr"):
            k -= 1
            continue
        break
    assert k < len(insns), insns
    return insns[k][0]


def _hops(insns):
    """Conditional branches that only hop over an unconditional `b` within the
    function (a `b` out of it is a tail call)."""
    out = []
    for k, (addr, mnem, ops) in enumerate(insns):
        if _is_cond_branch(mnem) and k + 2 < len(insns) and insns[k + 1][1] in _UNCOND:
            dest = _target(insns[k + 1][2])
            inner = dest is not None and insns[0][0] < dest <= insns[-1][0]  # the start: an unrelocated call
            if inner and _target(ops) == insns[k + 2][0]:
                out.append(hex(addr))
    return out


@pytest.mark.parametrize("opt", ["-O1", "-O2", "-Os"])
def test_return_in_r0_is_one_branch_to_epilogue(opt):
    funcs = _funcs(opt, "r0")
    for name in ("first_hit", "keep_a", "keep64", "tail"):
        insns = funcs[name]
        assert not _hops(insns), (name, insns)
        epi = _epilogue_start(insns)
        assert any(m.startswith("cb") and _target(o) == epi for _, m, o in insns), (name, insns)


@pytest.mark.parametrize("opt", ["-O1", "-O2", "-Os"])
def test_return_that_moves_keeps_its_jump(opt):
    funcs = _funcs(opt, "moves")
    for name in ("move_b", "find_neg"):
        insns = funcs[name]
        epi = _epilogue_start(insns)
        assert any(m in _UNCOND and _target(o) == epi for _, m, o in insns), (name, insns)


@pytest.mark.parametrize("opt", ["-O1", "-O2", "-Os"])
def test_branch_to_epilogue_is_narrow(opt):
    funcs = _funcs(opt, "narrow")
    insns = funcs["scan"]
    epi = _epilogue_start(insns)
    to_epi = [(m, o) for _, m, o in insns if _is_cond_branch(m) and _target(o) == epi]
    assert to_epi, insns
    assert all(not m.endswith(".w") for m, _ in to_epi), insns


@pytest.mark.parametrize("opt", ["-O1", "-O2", "-Os"])
def test_mask_before_load_shifts_the_word(opt):
    funcs = _funcs(opt, "mask")
    for name in ("mask_first", "mask_first_if"):
        assert not _register_lsl(funcs[name]), (name, funcs[name])
        assert not _moves_one(funcs[name]), (name, funcs[name])
    # A store between the mask and the load keeps the mask.
    assert _register_lsl(funcs["mask_store"]), funcs["mask_store"]


def _unreachable_jumps(insns):
    """Unconditional `b`s no branch names, right after another `b` or a return
    (NOP pads skipped): code nothing can reach."""
    named = {_target(o) for _, m, o in insns if _is_cond_branch(m) or m in _UNCOND}
    out = []
    prev = None
    for addr, mnem, ops in insns:
        if mnem == "nop":
            continue
        ends = prev is not None and (prev[0] in _UNCOND or prev[0].startswith(("pop", "ldmia")) or prev == ("bx", "lr"))
        if mnem in _UNCOND and ends and addr not in named:
            out.append(hex(addr))
        prev = (mnem, ops)
    return out


def _hops_over_dead(insns):
    """`b<c> L; b X; L:` with only NOPs and unreachable `b`s before L."""
    dead = set(_unreachable_jumps(insns))
    out = []
    for k, (addr, mnem, ops) in enumerate(insns):
        if not (_is_cond_branch(mnem) and k + 1 < len(insns) and insns[k + 1][1] in _UNCOND):
            continue
        m = k + 2
        while m < len(insns) and (insns[m][1] == "nop" or hex(insns[m][0]) in dead):
            m += 1
        if m < len(insns) and _target(ops) == insns[m][0]:
            out.append(hex(addr))
    return out


@pytest.mark.parametrize("opt", ["-O1", "-O2", "-Os"])
def test_orphaned_jump_dropped_and_exit_inverted(opt):
    funcs = _functions(_compile("bug_orphan_jump_hop", [opt], SRC))
    insns = funcs["reserve"]
    assert not _hops_over_dead(insns), insns
    assert not _unreachable_jumps(insns), insns
