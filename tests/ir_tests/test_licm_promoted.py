"""Pin late arithmetic hoists and their call-crossing profitability guard."""
import os
import re
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
SOURCE = Path(__file__).with_name("896_licm_promoted.c")


def _dump(tmp_path, function, flags=(), disabled=None):
    env = dict(os.environ, TCC_DUMP_FUNC=function)
    env.pop("TCC_DISABLE_PASS", None)
    if disabled:
        env["TCC_DISABLE_PASS"] = disabled
    result = subprocess.run(
        [str(ROOT / "armv8m-tcc"), "-O2", *flags,
         "-dump-ir-passes=ra:umaal", "-c", str(SOURCE),
         "-o", str(tmp_path / "promoted.o")],
        env=env, capture_output=True, text=True, check=True,
    )
    block = re.search(r"=== AFTER ra:umaal ===\n(.*?)=== END AFTER ra:umaal ===",
                      result.stdout + result.stderr, re.S)
    assert block, result.stdout + result.stderr
    return block[1]


def _loop_start(dump):
    targets = []
    for line in dump.splitlines():
        match = re.match(r"(\d+): JMP to (\d+)", line)
        if match and int(match[2]) < int(match[1]):
            targets.append(int(match[2]))
    assert targets, dump
    return min(targets)


def _shift_positions(dump):
    return [int(m[1]) for m in re.finditer(r"(?m)^(\d+): .* SHR #2", dump)]


@pytest.mark.parametrize("function", ["promoted", "promoted_call"])
def test_promoted_arithmetic_hoisted(tmp_path, function):
    dump = _dump(tmp_path, function)
    shifts = _shift_positions(dump)
    assert len(shifts) == 1, dump
    assert shifts[0] < _loop_start(dump), dump


@pytest.mark.parametrize("flags,disabled", [
    (("-fno-licm",), None), ((), "ra:licm"), ((), "ssa:licm"),
])
def test_promoted_licm_disable(tmp_path, flags, disabled):
    dump = _dump(tmp_path, "promoted", flags, disabled)
    shifts = _shift_positions(dump)
    assert len(shifts) == 1, dump
    assert shifts[0] >= _loop_start(dump), dump


def test_call_crossing_hoist_retains_shared_source(tmp_path):
    dump = _dump(tmp_path, "retained_source")
    shifts = _shift_positions(dump)
    assert len(shifts) == 1, dump
    assert shifts[0] >= _loop_start(dump), dump


def _asm(tmp_path, function, disabled=None):
    _dump(tmp_path, function, disabled=disabled)
    result = subprocess.run(
        ["arm-none-eabi-objdump", "-d", "--no-show-raw-insn",
         f"--disassemble={function}", str(tmp_path / "promoted.o")],
        capture_output=True, text=True, check=True,
    )
    return [(int(m[1], 16), m[2], m[3]) for m in re.finditer(
        r"(?m)^[ \t]*([0-9a-f]+):[ \t]+(\S+)[ \t]*(.*)$", result.stdout)]


def _hot_loop(instructions):
    for address, mnemonic, operands in reversed(instructions):
        target = re.match(r"([0-9a-f]+)\s+<", operands)
        if (mnemonic.startswith("b") and mnemonic not in ("bl", "blx", "bx")
                and target and int(target[1], 16) < address):
            start = int(target[1], 16)
            return [(m, o) for a, m, o in instructions
                    if start <= a <= address and m != "nop"]
    raise AssertionError(instructions)


@pytest.mark.parametrize("function", ["promoted", "promoted_call"])
def test_promoted_hoist_saves_work_without_memory_traffic(tmp_path, function):
    on = _asm(tmp_path, function)
    off = _asm(tmp_path, function, disabled="ra:licm")
    assert len(_hot_loop(on)) == len(_hot_loop(off)) - 2, (on, off)
    for prefix in ("ldr", "str", "ldm", "stm"):
        assert sum(m.startswith(prefix) for _, m, _ in on) <= sum(
            m.startswith(prefix) for _, m, _ in off), (on, off)
