"""A 64-bit shift / literal op of a zero-high-word value reads only the low word.

docs/bugs/kernel-u64-zero-high-shift.md: zero_half64 knew the widened key's
high word was zero, but thumb_emit_shift64_mop never asked, so every shift by
k < 32 materialized `mov hi, #0` and shifted/ORed it in; XOR/OR with a literal
did the same.  IntegerContext(u16).hash's key expansion cost 18 instructions
against LLVM's 4.
"""
import re

import pytest

from test_codegen_asm import ROOT, _compile, _disassemble


def _zero_movs(insns):
    return [(m, a) for m, a in insns if m.startswith("mov") and re.search(r",\s*#0$", a)]


def _body(insns):
    return [(m, a) for m, a in insns if not m.startswith(("bx", "nop", ".word"))]


@pytest.mark.parametrize("opt", ["-O1", "-O2", "-Os"])
def test_zero_high_shift_codegen(opt):
    funcs = _disassemble(_compile("bug_u64_zero_high_shift", [opt], ROOT / "tests/ir_tests"))

    # u16 << 16: lo = x << 16, hi = x >> 16 -- no zero, no ORR.
    for name in ("shl_u16_1", "shl_u16_8", "shl_u16_16", "shl_u16_31", "shl_u8_8", "shl_u32_16"):
        insns = funcs[name]
        assert not _zero_movs(insns), (name, insns)
        assert not any(m.startswith("orr") for m, _ in insns), (name, insns)
        assert len(_body(insns)) <= 4, (name, insns)

    # u16 >> 8: lo = x >> 8, the returned high word is the only zero.
    for name in ("shr_u16_1", "shr_u16_8", "shr_u32_16", "sar_u16_8", "sar_u32_16"):
        insns = funcs[name]
        assert len(_zero_movs(insns)) == 1, (name, insns)
        assert not any(m.startswith("orr") for m, _ in insns), (name, insns)
        assert len(_body(insns)) <= 4, (name, insns)

    # Literal against a zero high word: the high result is the literal's half.
    insns = funcs["xor_lit"]
    assert sum(m.startswith("eor") for m, _ in insns) == 1, insns
    assert not _zero_movs(insns), insns
    insns = funcs["or_lit"]
    assert sum(m.startswith("orr") for m, _ in insns) <= 1, insns
    assert not _zero_movs(insns), insns

    # The key expansion: every intermediate high word is provably zero, so no
    # ORR folds a shifted zero in.  (The allocator still reserves the dead high
    # registers, so a push/pop may remain; that is not counted.)
    insns = [(m, a) for m, a in funcs["expand"] if not m.startswith(("push", "pop", "stmdb", "ldmia"))]
    assert len(_zero_movs(insns)) == 1, insns
    assert not [(m, a) for m, a in insns if m.startswith("orr") and re.search(r"ls[lr] #", a)], insns
    assert len(_body(insns)) <= 9, insns


@pytest.mark.parametrize("opt", ["-O1", "-O2", "-Os"])
def test_volatile_u64_local_keeps_both_words(opt):
    """A volatile u64 local owes both words of every store, read or not: its
    zero high word is no license to drop the `str #0` into the slot."""
    funcs = _disassemble(_compile("bug_u64_zero_high_shift", [opt], ROOT / "tests/ir_tests"))
    for name in ("vol_write_only", "vol_shl", "vol_xor_lit", "vol_or_reg"):
        insns = funcs[name]
        words = sum(2 if m.startswith("strd") else 1 for m, a in insns
                    if m.startswith("str") and "sp" in a)
        assert words >= 2, (name, insns)
