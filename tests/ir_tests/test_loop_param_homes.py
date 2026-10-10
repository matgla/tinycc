"""Loop-invariant by-value parameters should survive calls in registers."""
import os
import re
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
SOURCE = Path(__file__).with_name("900_loop_param_homes.c")


@pytest.mark.parametrize("opt", ["-O1", "-O2"])
@pytest.mark.parametrize("flags", [(), ("-mpic-data-is-text-relative",)])
@pytest.mark.parametrize("disabled", [None, "param_home_fwd", "ra:stack_param_promote"])
def test_parameter_homes_not_reloaded_in_loop(tmp_path, opt, flags, disabled):
    obj = tmp_path / "params.o"
    env = dict(os.environ)
    env.pop("TCC_DISABLE_PASS", None)
    if disabled:
        env["TCC_DISABLE_PASS"] = disabled
    subprocess.run([str(ROOT / "armv8m-tcc"), opt, "-fno-pic", "-fno-module-local-calls", *flags, "-c", str(SOURCE),
                    "-o", str(obj)], env=env, check=True, capture_output=True, text=True)
    asm = subprocess.run(
        ["arm-none-eabi-objdump", "-d", "--no-show-raw-insn",
         "--disassemble=thunk_replay", str(obj)],
        check=True, capture_output=True, text=True,
    ).stdout
    instructions = [(int(m[1], 16), m[2], m[3]) for m in re.finditer(
        r"(?m)^\s*([0-9a-f]+):\s+(\S+)\s*(.*)$", asm)]
    backedges = []
    for address, mnemonic, operands in instructions:
        target = re.match(r"([0-9a-f]+)\s+<", operands)
        if (mnemonic.startswith("b") and mnemonic not in ("bl", "blx", "bx")
                and target and int(target[1], 16) < address):
            backedges.append((int(target[1], 16), address))
    assert backedges, asm
    start, end = max(backedges, key=lambda edge: edge[1] - edge[0])
    loop = [(m, o) for a, m, o in instructions if start <= a <= end]
    assert any(m == "udiv" for m, _ in loop), asm
    assert any(m == "bl" for m, _ in loop), asm
    reloads = [(m, o) for m, o in loop if m.startswith("ldr") and "[sp" in o]
    assert bool(reloads) == bool(disabled), asm


def test_crowded_loop_keeps_parameter_homes(tmp_path):
    result = subprocess.run(
        [str(ROOT / "armv8m-tcc"), "-O2", "-dump-ir-passes=param_home_fwd", "-c",
         str(SOURCE), "-o", str(tmp_path / "pressure.o")],
        env=dict(os.environ, TCC_DUMP_FUNC="pressured_home"),
        check=True, capture_output=True, text=True,
    )
    dump = result.stdout + result.stderr
    assert "=== AFTER param_home_fwd ===" in dump, dump
    assert re.search(r"<-- StackLoc\[-\d+\] \[LOAD\]", dump), dump


NEEDLE_SOURCE = Path(__file__).with_name("905_setif_window_fuse.c")


@pytest.mark.parametrize("opt", ["-O1", "-O2"])
@pytest.mark.parametrize("disabled", [None, "ra:stack_param_promote"])
def test_narrow_stack_param_needle_not_reloaded(tmp_path, opt, disabled):
    """A u8 5th argument (mem.findScalarPos's needle) is cached in a register
    for the whole scan loop instead of re-read from the incoming argument
    area every byte (kernel-ls-bin-scan-family-gap)."""
    obj = tmp_path / "needle.o"
    env = dict(os.environ)
    env.pop("TCC_DISABLE_PASS", None)
    if disabled:
        env["TCC_DISABLE_PASS"] = disabled
    subprocess.run([str(ROOT / "armv8m-tcc"), opt, "-fno-pic", "-c", str(NEEDLE_SOURCE),
                    "-o", str(obj)], env=env, check=True, capture_output=True, text=True)
    asm = subprocess.run(
        ["arm-none-eabi-objdump", "-d", "--no-show-raw-insn",
         "--disassemble=find_scalar", str(obj)],
        check=True, capture_output=True, text=True,
    ).stdout
    instructions = [(int(m[1], 16), m[2], m[3]) for m in re.finditer(
        r"(?m)^\s*([0-9a-f]+):\s+(\S+)\s*(.*)$", asm)]
    backedges = []
    for address, mnemonic, operands in instructions:
        target = re.match(r"([0-9a-f]+)\s+<", operands)
        if (mnemonic.startswith("b") and mnemonic not in ("bl", "blx", "bx")
                and target and int(target[1], 16) < address):
            backedges.append((int(target[1], 16), address))
    assert backedges, asm
    start, end = max(backedges, key=lambda edge: edge[1] - edge[0])
    loop = [(m, o) for a, m, o in instructions if start <= a <= end]
    assert any(m.startswith("ldrb") for m, _ in loop), asm  # buf[i] stays a load
    arg_reloads = [(m, o) for m, o in loop if m.startswith("ldr") and "[sp" in o]
    assert bool(arg_reloads) == bool(disabled), asm
