"""A by-value struct parameter split between r3 and the stack: its register
words are PARAM vregs (no `push {r0-r3}` copy, no SP reloads in loops), and a
function that only forwards it still becomes a branch."""
import re

import pytest

from test_codegen_asm import ROOT, _compile, _disassemble

CASE = "bug_split_byval_param_word_vregs"


@pytest.mark.parametrize("opt", ["-O1", "-O2", "-Os"])
def test_split_struct_param_reads_registers(opt):
    funcs = _disassemble(_compile(CASE, [opt], ROOT / "tests/ir_tests"))
    for name in ("skip24", "skip24n", "sum24", "mid", "s8_r3", "s20_mix", "l24"):
        insns = funcs[name]
        pushes = [(m, a) for m, a in insns if m.startswith("push") and "r0" in a]
        assert not pushes, f"{name}: argument registers pushed: {insns}"
    # Only the stack words are read from memory, once each.  (skip24's
    # aggregate copy `t11 = *t5` in the loop is the LICM report's half.)
    for name in ("sum24", "mid", "skip24n", "s8_r3"):
        loads = [(m, a) for m, a in funcs[name] if re.fullmatch(r"ldr(d|\.w|\.n)?", m)]
        assert len(loads) == 1 and "sp" in loads[0][1], f"{name}: {funcs[name]}"


@pytest.mark.parametrize("opt", ["-O1", "-O2", "-Os"])
def test_split_struct_param_pure_forward_is_a_branch(opt):
    funcs = _disassemble(_compile(CASE, [opt], ROOT / "tests/ir_tests"))
    for name in ("fwd", "fwd8", "fwd18"):
        insns = funcs[name]
        assert len(insns) == 1 and insns[0][0] in ("b", "b.w", "b.n"), f"{name}: {insns}"


def test_long_split_struct_param_stays_in_place():
    funcs = _disassemble(_compile(CASE, ["-O2"], ROOT / "tests/ir_tests"))
    insns = funcs["big_sum"]
    assert any(m.startswith("push") and "r0" in a for m, a in insns), insns


def _loop_body_loads(obj, name):
    """Loads between a backward branch's target and the branch, in `name`."""
    import subprocess

    from test_codegen_asm import OBJDUMP

    out = subprocess.run([OBJDUMP, "-d", "--no-show-raw-insn", str(obj)], stdout=subprocess.PIPE, text=True,
                         check=True).stdout
    insns, cur = [], None
    for line in out.splitlines():
        h = re.match(r"^\s*[0-9a-f]+\s+<([^>]+)>:$", line)
        if h:
            cur = h.group(1)
            continue
        m = re.match(r"^\s*([0-9a-f]+):\s+(\S+)(?:\s+(.*))?$", line)
        if m and cur == name:
            insns.append((int(m.group(1), 16), m.group(2), m.group(3) or ""))
    for addr, mnem, ops in insns:
        b = re.match(r"([0-9a-f]+) <", ops)
        if mnem.startswith("b") and b and int(b.group(1), 16) < addr:
            lo = int(b.group(1), 16)
            return [(m, o) for a, m, o in insns if lo <= a < addr and m.startswith("ldr")]
    raise AssertionError(f"{name}: no loop: {insns}")


@pytest.mark.parametrize("opt", ["-O1", "-O2", "-Os"])
def test_volatile_struct_param_reads_stay_in_loop(opt):
    # convert_parameter_type drops `volatile` from the parameter's type;
    # params.c must put it back on a struct parameter's symbol as it does for a
    # scalar, or the home copy's field reads are forwarded and hoisted.
    obj = _compile(CASE, [opt], ROOT / "tests/ir_tests")
    for name, reads in (("vol_split12", 2), ("vol_split8", 1), ("vol_reg8", 1)):
        loads = _loop_body_loads(obj, name)
        assert len(loads) >= reads, f"{name}: volatile field reads left the loop: {loads}"
