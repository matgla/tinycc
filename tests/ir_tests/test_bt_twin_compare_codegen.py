"""A join whose compare repeats the one each incoming branch made.

`if (a < 7) ...; return a < 7 ? 10 : 20;`: every edge into the second compare
comes through the first one's branch with a unchanged, so it already knows
the answer (opt/ra/branch_thread.c, bt_twin_fact).  A JUMPIF there is
threaded; a SELECT becomes a move of the value it picks on each edge.  An
-minline-atomics compare-exchange kept as a value is the frontend's shape of
it: the failure path's test of old == expected, then the result selected.
"""
import os
import re

import pytest

from test_atomic_cas_seqcst_codegen import ROOT, _assert_cas_shape, _compile, _count, _funcs, _retests, _text

FIXTURE = "bug_bt_twin_compare_select"


def _obj(flags):
    worker = os.environ.get("PYTEST_XDIST_WORKER", "main")
    return _compile(FIXTURE, [*flags, f"-DTEST_XDIST_WORKER_{worker}"], ROOT / "tests/ir_tests")


@pytest.fixture(scope="module", params=["-O1", "-O2", "-Os"])
def v8(request):
    return _funcs(_obj([request.param, "-minline-atomics"]))


@pytest.mark.parametrize("fn", ("cs_take5", "cs_sel_store", "cs_branch_vreg"))
def test_cas_value_not_retested(v8, fn):
    """The CAS's result selected (or the write-back tested) after the join:
    each edge already knows it, so old is compared inside the loop only."""
    insns = v8[fn]
    _assert_cas_shape(insns, fn)
    assert _retests(insns) == 0, f"{fn}: old compared again after the loop:\n{_text(insns)}"
    assert _count(insns, r"^it") == 0, f"{fn}: an IT block selects the result:\n{_text(insns)}"


# function -> compares left: one per branch that decides something
DECIDED = {"tw_select": 1, "tw_vreg": 1, "tw_three": 2, "tw_select_then_test": 2}


@pytest.mark.parametrize("fn", sorted(DECIDED))
def test_twin_compare_decided(v8, fn):
    insns = v8[fn]
    assert _count(insns, r"^it") == 0, f"{fn}: the join still selects:\n{_text(insns)}"
    assert _count(insns, r"^cmp") == DECIDED[fn], f"{fn}: the join's compare stays:\n{_text(insns)}"


@pytest.mark.parametrize("fn", ("tw_changed", "tw_weaker", "tw_signedness"))
def test_twin_compare_undecided(v8, fn):
    """The operand changed on one path, a weaker condition, the other
    signedness: the join compares again and selects."""
    insns = v8[fn]
    assert _count(insns, r"^cmp") == 2, f"{fn}:\n{_text(insns)}"
    assert _count(insns, r"^it") == 1, f"{fn}:\n{_text(insns)}"
