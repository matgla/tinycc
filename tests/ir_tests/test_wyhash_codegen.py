"""Keep UMAAL's implicit zero out of allocated pairs and stack slots."""
import re

import pytest

from test_codegen_asm import ROOT, _compile, _disassemble


@pytest.mark.parametrize("opt", ["-O1", "-O2", "-Os"])
def test_umaal_word_accumulator_codegen(opt):
    funcs = _disassemble(_compile("bug_wyhash_pair_spills", [opt], ROOT / "tests/ir_tests"))
    insns = funcs["one_addend"]
    assert sum(m.startswith("umaal") for m, _ in insns) == 1
    assert not [(m, a) for m, a in insns if m.startswith(("push", "pop")) or re.search(r"\bsp\b", a)]
    assert sum(m.startswith("umaal") for m, _ in funcs["two_addends"]) == 1

    insns = funcs["wyhash_mum"]
    zeros = [(m, a) for m, a in insns if m.startswith("mov") and re.search(r",\s*#0\b", a)]
    assert len(zeros) == 2, zeros
    assert sum(m.startswith("umaal") for m, _ in insns) == 3
    assert sum(m.startswith("umull") for m, _ in insns) == 1
    assert not [(m, a) for m, a in insns if m.startswith(("add", "adc"))], insns
    assert len(insns) <= 18, insns
    saved = [a for m, a in insns if m.startswith(("push", "stmdb"))]
    assert len(saved) == 1, saved
    assert len(re.search(r"\{([^}]+)\}", saved[0]).group(1).split(",")) <= 4, saved

    for name in ("high_addend", "high_addends"):
        insns = funcs[name]
        assert sum(m.startswith("umaal") for m, _ in insns) == 1, insns
        assert not any(m.startswith("umull") for m, _ in insns), insns
    assert not any(m.startswith(("add", "adc")) for m, _ in funcs["high_addend"])
    assert not [(m, a) for m, a in funcs["high_addend"] if re.search(r"\bsp\b", a)
                or m.startswith(("push", "pop"))], funcs["high_addend"]
    assert any(m.startswith("adc") for m, _ in funcs["late_high_addend"])
    for name in ("signed_high_addend", "shift31_addend"):
        assert not any(m.startswith("umaal") for m, _ in funcs[name]), funcs[name]
