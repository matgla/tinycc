"""zig.h's full-width signed truncate `(x & SIGN) ? x | MIN : x & MAX` is x."""
import re

import pytest

from test_codegen_asm import ROOT, _compile, _disassemble


def _mask_work(insns):
    return [(m, a) for m, a in insns
            if re.match(r"(and|orr|orrs|ubfx|sbfx|sxth|sxtb|bic|tst|cbz|cbnz|b|beq|bne|bmi|bpl|it)(\.[nw])?$", m)]


@pytest.mark.parametrize("opt", ["-O1", "-O2", "-Os"])
def test_full_width_signed_truncate_folds(opt):
    funcs = _disassemble(_compile("bug_signed_truncate_identity", [opt], ROOT / "tests/ir_tests"))
    # The identity at every width: no sign test, no arm masks, no re-extension.
    for name in ("id32", "id64", "ld16_id", "ld8_id"):
        assert not _mask_work(funcs[name]), (name, funcs[name])
    insns = funcs["sext16_id"]
    assert sum(m.startswith("sxth") for m, _ in insns) == 1, insns
    assert not [(m, a) for m, a in _mask_work(insns) if not m.startswith("sxth")], insns
    # write_result stores and returns the payload: no sign test, no arm masks.
    insns = funcs["write_result"]
    assert not any(m.startswith(("ubfx", "orr")) for m, _ in insns), insns
    # Narrower truncates and non-identity masks keep their work.
    for name in ("t32_31", "t32_16", "t64_32", "t64_63", "wide_or", "narrow_and", "sext16_bit14"):
        assert any(m.startswith(("orr", "ubfx", "and")) for m, _ in funcs[name]), (name, funcs[name])
