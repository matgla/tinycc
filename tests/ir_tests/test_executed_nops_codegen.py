"""No executed NOPs: a jump over code-free ops is dropped, not patched to a NOP,
and a word-alignment pad on a fall-through path widens the instruction before
it instead (the pad remains free when nothing falls into it)."""
import re
import subprocess

import pytest

from test_codegen_asm import OBJDUMP, ROOT, _compile, _disassemble


def _transfers(mnem, ops):
    """An instruction after which execution never falls through."""
    if re.fullmatch(r"b(?:\.n|\.w)?", mnem) or mnem.startswith("bx"):
        return True
    return mnem.startswith(("pop", "ldm")) and re.search(r"\bpc\b", ops) is not None


def _executed_nops(insns):
    return [k for k in range(1, len(insns)) if insns[k][0].startswith("nop") and not _transfers(*insns[k - 1])]


FUNCS = ("sel", "sel2", "sel3", "count_le", "sum_bytes", "copy_bytes", "fill_words", "next_bit", "mixed", "trim")


@pytest.mark.parametrize("opt", ["-O1", "-O2"])
def test_no_executed_nops(opt):
    funcs = _disassemble(_compile("bug_executed_nops", [opt], ROOT / "tests/ir_tests"))
    bad = {}
    for name in FUNCS:
        insns = funcs[name]
        hits = _executed_nops(insns)
        if hits:
            bad[name] = [insns[max(0, k - 2):k + 2] for k in hits]
    assert not bad, f"executed nops: {bad}"


def test_jump_over_coalesced_copies_dropped():
    funcs = _disassemble(_compile("bug_executed_nops", ["-O2"], ROOT / "tests/ir_tests"))
    # cbz; orr.w; b.n join; uxtb; [join] bx lr -- the else arm falls through.
    insns = funcs["sel2"]
    assert not any(m.startswith("nop") for m, _ in insns), insns
    assert sum(1 for m, _ in insns if re.fullmatch(r"b(?:\.n|\.w)?", m)) == 1, insns


_INSN = re.compile(r"^\s*([0-9a-f]+):\s+(?:[0-9a-f]{4}\s?)+\s+(\S+)\s*(.*)$")


def _mid_instruction_branches(obj):
    """Branches whose target is not the start of an instruction in its section."""
    out = subprocess.run([OBJDUMP, "-d", str(obj)], stdout=subprocess.PIPE, text=True, errors="replace").stdout
    bad, starts, branches, sect = [], set(), [], None

    def flush():
        bad.extend((sect, line) for target, line in branches if target not in starts)

    for line in out.splitlines():
        if line.startswith("Disassembly of section"):
            flush()
            sect, starts, branches = line, set(), []
            continue
        m = _INSN.match(line)
        if not m:
            continue
        addr, mnem, ops = int(m.group(1), 16), m.group(2), m.group(3)
        starts.add(addr)
        if re.fullmatch(r"b(?:eq|ne|cs|hs|cc|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le)?(?:\.[nw])?|cbn?z", mnem):
            t = re.search(r"\b([0-9a-f]+) <", ops)
            if t:
                branches.append((int(t.group(1), 16), line.strip()))
    flush()
    return bad


# A pad widened into the instruction before it must not be a branch target:
# an `if` join right before a loop head (bug_executed_nops_pad_target) and the
# shapes the suite already had (426 walk, 846 f2c) branched into the middle of
# the widened `movs.w`/`mov.w`.
@pytest.mark.parametrize("opt", ["-O1", "-O2"])
@pytest.mark.parametrize(
    "name",
    ["bug_executed_nops", "bug_executed_nops_pad_target", "426_entry_store_phi_alias", "846_loop_iv_init_reaching_def"],
)
def test_no_branch_into_widened_pad(name, opt):
    bad = _mid_instruction_branches(_compile(name, [opt], ROOT / "tests/ir_tests"))
    assert not bad, f"branches into the middle of an instruction: {bad}"
