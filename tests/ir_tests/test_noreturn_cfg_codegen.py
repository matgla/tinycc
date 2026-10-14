"""A noreturn call ends its block: no reload after a panic guard, SUB/CMP fusion across a store."""
import re

import pytest

from test_codegen_asm import ROOT, _compile, _disassemble


def _depth_loads(insns):
    return [(m, a) for m, a in insns if m.startswith("ldr") and re.search(r"\[r\d+, #4\]", a)]


@pytest.mark.parametrize("opt", ["-O1", "-O2", "-Os"])
def test_noreturn_guard_no_reload(opt):
    funcs = _disassemble(_compile("bug_noreturn_call_cfg_edge", [opt], ROOT / "tests/ir_tests"))
    insns = funcs["guard_noreturn"]
    first_store = next(k for k, (m, _) in enumerate(insns) if m.startswith("str"))
    assert len(_depth_loads(insns[:first_store])) == 1, insns
    # Nothing has to survive the noreturn call: no callee-saved register for it.
    pushes = [a for m, a in insns if m.startswith("push")]
    assert not [a for a in pushes if re.search(r"\br([4-9]|1[01])\b", a)], insns


@pytest.mark.parametrize("opt", ["-O1", "-O2", "-Os"])
def test_sub_cmp_fusion_across_store(opt):
    funcs = _disassemble(_compile("bug_noreturn_call_cfg_edge", [opt], ROOT / "tests/ir_tests"))
    insns = funcs["dec_store_test"]
    assert any(m.startswith("subs") for m, _ in insns), insns
    assert not [(m, a) for m, a in insns if m.startswith("cmp")], insns


def _saves_lr(m, a):
    return (m.startswith("push") or m.startswith("stmdb")) and re.search(r"\blr\b", a)


@pytest.mark.parametrize("opt", ["-O1", "-O2"])
@pytest.mark.parametrize("fn", ["guard_unlock", "guard_last", "guard_slots", "two_guards"])
def test_noreturn_only_calls_leaf_lr_push(opt, fn):
    """Only noreturn calls: LR is pushed in the cold block right before each
    `bl`, never in the prologue, and the hot path returns through `bx lr`."""
    insns = _disassemble(_compile("bug_noreturn_leaf_lr_push", [opt], ROOT / "tests/ir_tests"))[fn]
    bls = [k for k, (m, _) in enumerate(insns) if m in ("bl", "blx")]
    assert bls, insns
    for k in bls:
        assert k > 0 and _saves_lr(*insns[k - 1]), insns
    assert not [(m, a) for k, (m, a) in enumerate(insns) if _saves_lr(m, a) and k + 1 not in bls], insns
    assert any(m.startswith("bx") and a.strip() == "lr" for m, a in insns), insns


@pytest.mark.parametrize("opt", ["-O1", "-O2", "-Os"])
@pytest.mark.parametrize("fn", ["guard_stack_args", "guard_and_call", "guard_pressure"])
def test_noreturn_with_stack_args_or_call_saves_lr_in_prologue(opt, fn):
    """Stack arguments, an ordinary call, or callee-saved registers (LR is free
    in that push, `pop {..., pc}` beats `pop {...}; bx lr`): LR in the prologue."""
    insns = _disassemble(_compile("bug_noreturn_leaf_lr_push", [opt], ROOT / "tests/ir_tests"))[fn]
    assert _saves_lr(*insns[0]), insns


@pytest.mark.parametrize("fn", ["guard_unlock", "guard_slots"])
def test_noreturn_leaf_lr_push_not_at_os(fn):
    """-Os: a push per noreturn call is more bytes than the prologue's, and a
    leaf cannot use the machine outliner; LR stays in the prologue."""
    insns = _disassemble(_compile("bug_noreturn_leaf_lr_push", ["-Os"], ROOT / "tests/ir_tests"))[fn]
    assert _saves_lr(*insns[0]), insns
