"""A compare's flag tested again where that compare's branch joins.

`ok = a == b; if (a != b) {...}; if (ok)`: each edge into the second test
comes from a branch that already decided ok, so it goes straight to the code
ok selects (opt/ra/branch_thread.c), and the flag, read nowhere else, is never
materialized.  An inline compare-exchange is the frontend's form of it: the
success flag, then the failure path's own test of old == expected.
"""
import os
import re

import pytest

from test_atomic_cas_seqcst_codegen import ROOT, _assert_cas_shape, _cas_loops, _compile, _count, _funcs, _text

FIXTURE = "bug_cas_flag_join_thread"


def _obj(flags):
    worker = os.environ.get("PYTEST_XDIST_WORKER", "main")
    return _compile(FIXTURE, [*flags, f"-DTEST_XDIST_WORKER_{worker}"], ROOT / "tests/ir_tests")


@pytest.fixture(scope="module", params=["-O1", "-O2", "-Os"])
def v8(request):
    return _funcs(_obj([request.param, "-minline-atomics"]))


def _flag_tests(insns):
    """Tests, after it, of a register a SETIF (`ite; mov<c> rX, #1; mov<!c>
    rX, #0`) wrote."""
    n = 0
    for k, (_, m, o) in enumerate(insns):
        t = re.match(r"^\s*(r\d+|ip|lr),\s*#1$", o)
        if not (re.match(r"^mov(eq|ne|lt|ge|gt|le|cc|cs|hi|ls|mi|pl)", m) and t):
            continue
        for _, m2, o2 in insns[k + 1:]:
            r = re.match(r"^\s*(r\d+|ip|lr)\b", o2)
            if r and r.group(1) == t.group(1) and (re.match(r"^cbn?z", m2) or (m2.startswith("cmp") and "#0" in o2)):
                n += 1
    return n


@pytest.mark.parametrize("fn", ("cas_flag_ne", "cas_flag_keep"))
def test_cas_flag_not_retested(v8, fn):
    """The flag is still stored (it is read after the join), but the join's
    test of it is gone: both edges already know it."""
    insns = v8[fn]
    _assert_cas_shape(insns, fn)
    assert _flag_tests(insns) == 0, f"{fn}: the CAS flag tested after the join:\n{_text(insns)}"


def test_cas_flag_spin_fused(v8):
    """Zig's cmpxchgWeak spin loop in a function that touches volatile memory:
    the flag is never materialized and the loop's exits are the branch."""
    insns = v8["cas_flag_spin"]
    _assert_cas_shape(insns, "cas_flag_spin")
    assert _count(insns, r"^it") == 0, _text(insns)
    for top, back in _cas_loops(insns):
        tail = [m for _, m, _ in insns[top:back + 1]]
        assert any(m.startswith(("beq", "cbz")) for m in tail), f"success falls out of the loop:\n{_text(insns)}"


@pytest.mark.parametrize("opt", ["-O1", "-O2", "-Os"])
def test_plain_twin_flag_gone(opt):
    """No atomics: `ok = a < 7; if (a >= 7) ...; if (ok)` on a TEMP compares
    once and keeps no flag."""
    insns = _funcs(_obj([opt]))["plain_twin_tmp"]
    assert _count(insns, r"^it") == 0, _text(insns)
    assert _count(insns, r"^cmp") == 1, _text(insns)
