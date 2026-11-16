"""gdb on a running tcc -g program: locals one frame up, and the backtrace.

bug_dwarf_frame_base.c runs on the mps2-an505 QEMU with its gdb stub; gdb
stops in `leaf`, unwinds to `work` and prints its locals.  That needs two
things from tcc's DWARF: call-frame information (.debug_frame) to unwind
through a frame that pushed registers, and a frame base that the locals'
DW_OP_fbreg offsets are measured from.  tcc had neither -- the frame base was
DW_OP_call_frame_cfa with no CFI, and the offsets are from the frame top
(sp + locals), not the CFA -- so gdb read garbage or nothing.  A parameter
was also described as its register for the whole function, long after the
register had moved on; and a local the optimizer dissolved kept its old
frame offset.
"""

import shutil
import socket
import subprocess
import time
from pathlib import Path

import pytest

from qemu_run import CompileConfig, build_qemu_command, compile_testcase

HERE = Path(__file__).parent
SOURCE = HERE / "bug_dwarf_frame_base.c"


def _gdb_has_arm():
    gdb = shutil.which("gdb-multiarch") or shutil.which("arm-none-eabi-gdb") or shutil.which("gdb")
    if not gdb:
        return None
    probe = subprocess.run([gdb, "-nx", "-batch", "-ex", "set architecture arm"], capture_output=True, text=True)
    return gdb if probe.returncode == 0 and "arm" in probe.stdout + probe.stderr else None


GDB = _gdb_has_arm()


def _free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def _gdb_session(elf, commands):
    port = _free_port()
    qemu = subprocess.Popen(
        build_qemu_command("mps2-an505", elf) + f" -gdb tcp::{port} -S",
        shell=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    try:
        time.sleep(0.3)
        args = [GDB, "-nx", "-batch", "-ex", "set pagination off", "-ex", "set confirm off",
                "-ex", f"target remote :{port}"]
        for c in commands:
            args += ["-ex", c]
        result = subprocess.run(args + [str(elf)], capture_output=True, text=True, timeout=60)
        return result.stdout + result.stderr
    finally:
        qemu.kill()
        qemu.wait()


@pytest.mark.skipif(GDB is None, reason="needs a gdb that knows ARM")
@pytest.mark.parametrize("level", ["-O0", "-O1", "-O2", "-Os"])
def test_gdb_reads_locals_one_frame_up(level, tmp_path):
    result = compile_testcase([SOURCE], "mps2-an505", config=CompileConfig(output_dir=tmp_path, extra_cflags=f"-g {level}"))
    assert result.success, result.error
    out = _gdb_session(
        result.elf_file,
        ["break leaf", "continue", "bt", "frame 1", "print k", "print p", "print a", "kill"],
    )
    # The backtrace reaches main through work: the CFI unwinds both frames.
    assert "#1" in out and "work" in out and "main" in out, out
    values = [line.split("=", 1)[1].strip() for line in out.splitlines() if line.startswith("$")]
    assert len(values) == 3, out
    k, p, a = values
    # work's array stays in its frame (its address escapes to sum3): read
    # through the frame base, one frame up, at every level.
    assert k == "{6, 7, 8}", out
    # p is on the stack at -O0; optimised it may be in registers and gone --
    # but never wrong.
    if level == "-O0":
        assert p == "{x = 5, y = 10}", out
    else:
        assert p in ("{x = 5, y = 10}", "<optimized out>"), out
    # The parameter arrived in r0, which holds leaf's argument by now; it
    # lives across the call in its allocated register, and its location
    # list says where it is when.
    assert a == "5", out
