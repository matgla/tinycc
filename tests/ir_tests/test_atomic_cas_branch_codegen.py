"""An -minline-atomics compare-exchange whose result an `if` tests.

The LDREX/STREX loop already knows which way it left: a mismatch branches
out before the store, a successful store falls out (or branches) after it.
When the CAS's result feeds a branch, those exits are the branch -- the
mismatch goes to the failure code, the success to the success code -- and
nothing compares the loaded value with the expected one again after the
loop (LLVM's shape).  The CLREX stays on the failure path only.
"""
import pytest

import os
import re

from test_atomic_cas_seqcst_codegen import ROOT, _assert_cas_shape, _cas_loops, _compile, _funcs, _retests, _target, _text

FIXTURE = "bug_atomic_cas_result_branch"


def _obj(flags):
    worker = os.environ.get("PYTEST_XDIST_WORKER", "main")
    return _compile(FIXTURE, [*flags, f"-DTEST_XDIST_WORKER_{worker}"], ROOT / "tests/ir_tests")


@pytest.fixture(scope="module", params=["-O1", "-O2", "-Os"])
def v8(request):
    return _funcs(_obj([request.param, "-minline-atomics"]))


# The CAS's result goes straight into a branch, or is a value selected or
# tested again where the failure path joins (ra:branch_thread decides that
# compare on each edge): the loop's exits are it.
BRANCHING = ("br_spin_lock", "br_take", "br_spin", "br_take_far_fail", "br_take5", "br_take_big", "br_take_reg",
             "br_far", "br_far_fail")
# A low status register and the success code in reach: `cbz status, <success>`.
CBZ = ("br_spin_lock", "br_take", "br_spin", "br_take5", "br_trylock")


def _success_branches(insns):
    """For each CAS loop, whether a successful store branches out of it
    (`cmp status; beq <success>; b top`, or `cbz status, <success>; b top`)
    instead of falling out."""
    return [bool(re.match(r"^b(\.n|\.w)?$", insns[back][1])) and insns[back - 1][1].startswith(("beq", "cbz"))
            for _, back in _cas_loops(insns)]


@pytest.mark.parametrize("fn", BRANCHING)
def test_cas_exits_are_the_branch(v8, fn):
    insns = v8[fn]
    _assert_cas_shape(insns, fn)
    assert all(_success_branches(insns)), f"{fn}: the success falls out of the loop:\n{_text(insns)}"
    assert _retests(insns) == 0, f"{fn}: the loaded value tested again after the loop:\n{_text(insns)}"


@pytest.mark.parametrize("fn", CBZ)
def test_cas_store_status_cbz(v8, fn):
    """The store's status tested by the branch to the success code itself:
    `cbz status, <success>; b top`, one instruction fewer than `cmp; beq`."""
    insns = v8[fn]
    _assert_cas_shape(insns, fn)
    for _, back in _cas_loops(insns):
        assert insns[back - 1][1] == "cbz", f"{fn}: the store's status compared:\n{_text(insns)}"


def test_spin_lock_wfe_after_clrex(v8):
    insns = v8["br_spin_lock"]
    mn = [m for _, m, _ in insns]
    assert mn.index("clrex") < mn.index("wfe"), _text(insns)


@pytest.mark.parametrize("fn", ("br_trylock", "br_reg", "br_big", "br_max", "br_take_far"))
def test_cas_shape_everywhere(v8, fn):
    _assert_cas_shape(v8[fn], fn)


@pytest.mark.parametrize("opt", ["-O1", "-O2"])
def test_v7m_keeps_the_trailing_dmb(opt):
    """ARMv7-M orders an acquire CAS with a DMB after the loop, on both of its
    exits: there the loop's exits stay where the DMB is."""
    f = _funcs(_obj([opt, "-minline-atomics", "-march=armv7-m"]))
    for fn in (fn for fn in BRANCHING if fn != "br_take_big"):  # release only: no DMB after
        insns = f[fn]
        text = _text(insns)
        loops = _cas_loops(insns)
        assert loops, text
        for top, back in loops:
            body = insns[top:back + 1]
            out = insns[back + 1]
            assert out[1].startswith("dmb"), f"{fn}:\n{text}"
            for _, m, o in body:
                if m.startswith(("b", "cb")):
                    assert _target(o) in (body[0][0], out[0]), f"{fn}: an exit skips the DMB:\n{text}"
