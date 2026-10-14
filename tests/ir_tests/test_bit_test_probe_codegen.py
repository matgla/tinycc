"""Page-pool free-run probe shapes (bug_bit_test_probe.c).

- `(x & (1 << n)) != 0` is tested as `(x >> n) & 1`: no `1` materialised and
  no register `lsl` in the bit tests and the probe loop, while a mask read as a
  value keeps its `lsl`; a negated such test is not an `eor #1`.
- unsigned `x > 0` / `x <= 0` against zero is a CBZ/CBNZ like `!=` / `==`.
- no conditional branch lands on an unconditional `b`, and none just hops over
  one (`b<c> L; b X; L:`), a return's jump to the epilogue included.
"""
import re

import pytest

from test_codegen_asm import ROOT, _compile
from test_licm_field_ptr_codegen import _functions, _innermost_loop

SRC = ROOT / "tests/ir_tests"
NAME = "bug_bit_test_probe"
BIT_TESTS = ("bt_ne", "bt_eq", "bt_if", "bt_sel", "bt_mask_first")

_UNCOND = ("b", "b.n", "b.w")


def _is_cond_branch(mnem):
    return re.fullmatch(r"(?:b(?:eq|ne|cs|cc|hi|ls|ge|lt|gt|le|mi|pl|hs|lo)|cbn?z)(?:\.w|\.n)?", mnem) is not None


def _target(ops):
    m = re.match(r"([0-9a-f]+)\b", ops.split(",")[-1].strip())
    return int(m.group(1), 16) if m else None


def _moves_one(insns):
    return [(m, o) for _, m, o in insns if re.fullmatch(r"movs?(?:\.w|\.n)?", m) and re.search(r",\s*#1$", o)]


def _register_lsl(insns):
    return [(m, o) for _, m, o in insns if re.fullmatch(r"lsls?(?:\.w|\.n)?", m) and "#" not in o]


@pytest.mark.parametrize("opt", ["-O1", "-O2", "-Os"])
def test_single_bit_test_has_no_mask(opt):
    funcs = _functions(_compile(NAME, [opt], SRC))
    for name in BIT_TESTS:
        assert not _register_lsl(funcs[name]), (name, funcs[name])
        assert not _moves_one(funcs[name]), (name, funcs[name])
    # The negated test is the test with the branch swapped.
    assert not any(m.startswith("eor") for _, m, _ in funcs["scheduled_here"]), funcs["scheduled_here"]
    assert not _register_lsl(funcs["scheduled_here"]), funcs["scheduled_here"]
    # The masked word itself is the result here.
    assert _register_lsl(funcs["bt_value"]), funcs["bt_value"]


@pytest.mark.parametrize("opt", ["-O2", "-Os"])
def test_probe_loop_has_no_mask(opt):
    funcs = _functions(_compile(NAME, [opt], SRC))
    for name in ("probe", "find_free_run"):
        loop = _innermost_loop(funcs[name])
        assert not _moves_one(loop), (name, loop)
        assert not _register_lsl(loop), (name, loop)


@pytest.mark.parametrize("opt", ["-O1", "-O2", "-Os"])
def test_unsigned_zero_test_is_cbz(opt):
    funcs = _functions(_compile(NAME, [opt], SRC))
    for name in ("ugt0", "ule0"):
        insns = funcs[name]
        assert any(m.startswith("cbz") or m.startswith("cbnz") for _, m, _ in insns), (name, insns)
        assert not any(m.startswith("cmp") for _, m, _ in insns), (name, insns)


@pytest.mark.parametrize("opt", ["-O1", "-O2", "-Os"])
def test_no_branch_to_or_over_lone_jump(opt):
    funcs = _functions(_compile(NAME, [opt], SRC))
    for name in ("find_free_run", "reserve", "valid_stack_ptr", "probe", "probe_regions"):
        insns = funcs[name]
        at = {a: (m, o) for a, m, o in insns}
        for k, (addr, mnem, ops) in enumerate(insns):
            if not _is_cond_branch(mnem):
                continue
            tgt = _target(ops)
            assert tgt is not None and at.get(tgt, ("", ""))[0] not in _UNCOND, (name, hex(addr), insns)
            if k + 2 < len(insns) and insns[k + 1][1] in _UNCOND:
                assert tgt != insns[k + 2][0], (name, hex(addr), insns)
