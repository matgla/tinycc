"""A 64-bit value's known high word is a constant before allocation.

docs/bugs/kernel-u64-zero-high-shift.md: `((uint64_t)u16 ^ C) >> 32` is C.hi,
but ssa:fold had no 64-bit known-bits, so the XOR stayed and the multiply's
constant-word partial products (C.hi * M) kept two of mum's four UMULL/UMAALs.
"""
import pytest

from test_codegen_asm import ROOT, _compile, _disassemble

_ALU = ("eor", "orr", "and", "bic", "lsl", "lsr", "asr", "uxt", "sxt", "ubfx")
_MUL = ("umull", "umaal", "umlal", "mul", "mla")


def _alu(insns):
    return [(m, a) for m, a in insns if m.startswith(_ALU)]


def _body(insns):
    return [(m, a) for m, a in insns if not m.startswith(("bx", "nop", ".word"))]


def _muls(insns):
    return [(m, a) for m, a in insns if m.startswith(_MUL)]


@pytest.mark.parametrize("opt", ["-O1", "-O2", "-Os"])
def test_known_high_word_codegen(opt):
    funcs = _disassemble(_compile("bug_u64_known_high_word", [opt], ROOT / "tests/ir_tests"))

    # The result is a literal: nothing computes the dead low word.
    for name in ("hi_of_xor", "hi32_of_or", "hi_of_and", "shr40_of_xor", "sar40_of_xor", "hi_of_expand"):
        insns = funcs[name]
        assert not _alu(insns), (name, insns)

    # a.hi is the literal C.hi: both of its partial products are constants,
    # and the low one rides in the middle UMAAL's accumulator (umaal_fusion's
    # const-carry rewrite) instead of an ADDS/ADC pair of literals.
    live = len(_body(funcs["mum_live_hi"]))
    for name in ("mum_const_hi", "u16_key_hash"):
        insns = funcs[name]
        assert len(_muls(insns)) <= 2, (name, insns)
        assert sum(m.startswith(("adds", "adc")) for m, _ in insns) <= 2, (name, insns)
        # Nothing reads a.hi's register any more: zero_half64 lets the XOR
        # skip it, so C.hi is not even in the literal pool.
        assert not [a for m, a in insns if m == ".word" and "e7037ed1" in a], (name, insns)
    assert len(_body(funcs["mum_const_hi"])) <= live - 5, funcs["mum_const_hi"]

    # A live high word keeps every partial product.
    assert len(_muls(funcs["mum_live_hi"])) == 4, funcs["mum_live_hi"]
