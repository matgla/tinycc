"""ssa:licm_field_ptr: never load above a zero-trip guard; do hoist out of guarded loops.

bug_licm_field_ptr_zero_trip.c holds the shapes.  The masks pointer load must
not run before the loop's first exit test, and in the probe loops it must not
run per iteration either (one ldr per probe: the bitmap word).
"""
import re
import subprocess

import pytest

from test_codegen_asm import OBJDUMP, ROOT, _compile

SRC = ROOT / "tests/ir_tests"


def _functions(obj):
    """{name: [(addr, mnemonic, operands), ...]}"""
    out = subprocess.run([OBJDUMP, "-d", "--no-show-raw-insn", str(obj)], capture_output=True, text=True,
                         check=True).stdout
    funcs, cur = {}, None
    for line in out.splitlines():
        m = re.match(r"^\s*([0-9a-f]+)\s+<([^>]+)>:$", line)
        if m:
            cur = funcs.setdefault(m.group(2), [])
            continue
        m = re.match(r"^\s*([0-9a-f]+):\s+(\S+)(?:\s+(.*))?$", line)
        if m and cur is not None:
            cur.append((int(m.group(1), 16), m.group(2), m.group(3) or ""))
    return funcs


def _is_load(mnem):
    return re.fullmatch(r"ldr(?:\.w|\.n)?", mnem) is not None


def _is_branch(mnem):
    return re.fullmatch(r"(?:b(?:eq|ne|cs|cc|hi|ls|ge|lt|gt|le|mi|pl)?|cbn?z)(?:\.w|\.n)?", mnem) is not None


def _loads_before_first_exit_test(insns):
    """Loads issued before the first conditional branch of the function."""
    loads = []
    for _, mnem, ops in insns:
        if _is_branch(mnem) and mnem not in ("b", "b.n", "b.w"):
            return loads
        if _is_load(mnem) and "sp" not in ops and "pc" not in ops:
            loads.append((mnem, ops))
    return loads


def _innermost_loop(insns):
    """Instructions of the shortest backward-branch range."""
    best = None
    for addr, mnem, ops in insns:
        if not _is_branch(mnem):
            continue
        m = re.match(r"([0-9a-f]+)\b", ops.split(",")[-1].strip())
        if not m:
            continue
        target = int(m.group(1), 16)
        if target <= addr and (best is None or addr - target < best[1] - best[0]):
            best = (target, addr)
    assert best, "no loop"
    return [i for i in insns if best[0] <= i[0] <= best[1]]


@pytest.mark.parametrize("opt", ["-O2", "-Os"])
def test_no_field_load_above_zero_trip_guard(opt):
    funcs = _functions(_compile("bug_licm_field_ptr_zero_trip", [opt], SRC))
    for name in ("probe", "probe_byval", "count_set", "count_until"):
        assert not _loads_before_first_exit_test(funcs[name]), (name, funcs[name])


@pytest.mark.parametrize("opt", ["-O2"])
def test_probe_masks_load_hoisted(opt):
    funcs = _functions(_compile("bug_licm_field_ptr_zero_trip", [opt], SRC))
    for name in ("probe", "probe_byval", "count_set"):
        loop = _innermost_loop(funcs[name])
        loads = [(m, o) for _, m, o in loop if _is_load(m)]
        assert len(loads) == 1, (name, loop)
