"""Phase D: codegen size-lever tests via objdump pattern/count assertions.

Each test cross-compiles a tiny C case with the existing libs/tinycc/armv8m-tcc
(cflags mirror the QEMU Makefile's compile step: -nostdlib, -mcpu=cortex-m33,
-mthumb, -mfloat-abi=soft, -ffunction-sections, -c), then inspects the
resulting object file with arm-none-eabi-objdump.  The assertions count
mnemonics per function and check .rodata contents.  They are intentionally
written as *characterizations* of the current codegen; as the size-reduction
levers land, the failing assertions should be flipped to lock in the wins.
"""

import re
import subprocess
from collections import Counter
from pathlib import Path

import pytest

ROOT = Path(__file__).parent.parent.parent  # libs/tinycc
TCC = ROOT / "armv8m-tcc"
ASM_DIR = Path(__file__).parent / "asm"
BUILD_DIR = Path(__file__).parent / "build" / "asm"

OBJDUMP = "arm-none-eabi-objdump"


def _compile(name, extra_cflags=()):
    """Cross-compile a case in asm/<name>.c to an object file."""
    src = ASM_DIR / f"{name}.c"
    obj = BUILD_DIR / f"{name}.o"
    BUILD_DIR.mkdir(parents=True, exist_ok=True)

    cflags = [
        "-O1",
        "-nostdlib",
        "-fvisibility=hidden",
        "-mcpu=cortex-m33",
        "-mthumb",
        "-mfloat-abi=soft",
        "-ffunction-sections",
        "-c",
    ]
    cmd = [str(TCC)] + cflags + list(extra_cflags) + [str(src), "-o", str(obj)]
    result = subprocess.run(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        errors="replace",
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"Compile failed for {name}: {cmd}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return obj


def _disassemble(obj):
    """Return {func_name: [(mnemonic, operands), ...]} from objdump -d."""
    result = subprocess.run(
        [OBJDUMP, "-d", "--no-show-raw-insn", str(obj)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        errors="replace",
    )
    assert result.returncode == 0, f"objdump failed for {obj}: {result.stderr}"

    funcs = {}
    cur = None
    for line in result.stdout.splitlines():
        header = re.match(r"^\s*([0-9a-f]+)\s+<([^>]+)>:$", line)
        if header:
            cur = header.group(2)
            funcs[cur] = []
            continue
        if cur is None:
            continue
        insn = re.match(r"^\s*[0-9a-f]+:\s+(\S+)(?:\s+(.*))?$", line)
        if insn:
            funcs[cur].append((insn.group(1), insn.group(2) or ""))
    return funcs


def _mnem_counts(func_insns):
    """Counter of mnemonics for a single function."""
    return Counter(mnem for mnem, _ in func_insns)


def _count_mnem(func_insns, mnem):
    return _mnem_counts(func_insns).get(mnem, 0)


def _count_mnem_regex(func_insns, pattern):
    rx = re.compile(pattern)
    return sum(1 for mnem, ops in func_insns if rx.search(f"{mnem} {ops}"))


def _rodata_bytes(obj):
    """Return raw bytes of the .rodata section (little-endian word order)."""
    result = subprocess.run(
        [OBJDUMP, "-s", "-j", ".rodata", str(obj)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        errors="replace",
    )
    if result.returncode != 0:
        return b""

    data = bytearray()
    for line in result.stdout.splitlines():
        m = re.match(r"^\s*[0-9a-f]+\s+((?:[0-9a-f]{8}\s+)*)", line)
        if not m:
            continue
        for group in m.group(1).split():
            data.extend(bytes.fromhex(group))  # objdump already prints bytes in file order
    return bytes(data)


def _count_subseq(data, needle):
    """Non-overlapping count of needle in data."""
    count = 0
    i = 0
    while True:
        i = data.find(needle, i)
        if i < 0:
            return count
        count += 1
        i += len(needle)


# -----------------------------------------------------------------------------
# R9 GOT-base save/restore
# -----------------------------------------------------------------------------
def test_r9_spill_around_calls():
    obj = _compile("r9_spill", extra_cflags=["-mpic-data-is-text-relative"])
    funcs = _disassemble(obj)
    caller = funcs["caller"]

    str_r9 = _count_mnem_regex(caller, r"^str\.w.*r9")
    ldr_r9 = _count_mnem_regex(caller, r"^ldr\.w.*r9")
    mov_r9_r10 = _count_mnem_regex(caller, r"^mov\s+.*r9.*r10")

    # Current codegen: R9 is saved before each call and restored after.
    assert str_r9 >= 2, f"expected R9 saves around calls, got {str_r9} str.w r9"
    assert ldr_r9 >= 2, f"expected R9 restores after calls, got {ldr_r9} ldr.w r9"
    # Phase 1b (callee-saved R10 holding the GOT base) is not implemented yet.
    assert mov_r9_r10 == 0, "unexpected mov r9, r10 (Phase 1b not landed)"


# -----------------------------------------------------------------------------
# Forward conditional branch narrowing
# -----------------------------------------------------------------------------
def test_forward_branch_conditional_still_wide():
    obj = _compile("forward_branch_narrow")
    funcs = _disassemble(obj)
    loop = funcs["loop"]

    wide_fwd = _count_mnem(loop, "bge.w")
    narrow_back = _count_mnem(loop, "blt.n")

    # Backward branches are already narrowed.
    assert narrow_back >= 1, f"expected backward blt.n, got {narrow_back}"
    # Forward conditional branches currently stay wide (Phase 2a not landed).
    assert wide_fwd >= 1, f"expected forward bge.w, got {wide_fwd}"


# -----------------------------------------------------------------------------
# CBZ/CBNZ fusion
# -----------------------------------------------------------------------------
def test_cbz_fusion_disabled():
    obj = _compile("cbz_fusion")
    funcs = _disassemble(obj)

    for name in ("iszero", "isnonzero"):
        fn = funcs[name]
        cmp_count = _count_mnem(fn, "cmp")
        cbz_count = _count_mnem(fn, "cbz") + _count_mnem(fn, "cbnz")
        # One of beq.w or bne.w depending on polarity.
        cond_wide = _count_mnem(fn, "beq.w") + _count_mnem(fn, "bne.w")

        assert cmp_count >= 1, f"{name}: expected cmp #0, got {cmp_count}"
        assert cond_wide >= 1, f"{name}: expected wide conditional branch, got {cond_wide}"
        assert cbz_count == 0, f"{name}: cbz/cbnz fusion unexpectedly enabled ({cbz_count})"


# -----------------------------------------------------------------------------
# Struct by-value 9-byte packed operand
# -----------------------------------------------------------------------------
def test_struct_packed_9byte_by_value():
    obj = _compile("struct_packed_9byte")
    funcs = _disassemble(obj)
    caller = funcs["caller"]
    consume = funcs["consume"]

    load_mnems = {"ldr", "ldr.w", "ldrh", "ldrsh", "ldrsh.w", "ldrb", "ldrsb"}
    load_count = sum(_count_mnem(consume, m) for m in load_mnems)

    # Caller currently copies the by-value struct with __aeabi_memmove.
    assert any("__aeabi_memmove" in ops for _, ops in caller), "caller missing __aeabi_memmove copy"
    # Callee loads unaligned packed fields.
    assert load_count >= 2, f"consume expected at least 2 loads, got {load_count}"
    # No undefined/breakpoint instructions (i.e. no obviously broken encoding).
    udf_count = sum(
        _count_mnem(func, "udf") + _count_mnem(func, "bkpt")
        for func in funcs.values()
    )
    assert udf_count == 0, f"unexpected udf/bkpt instructions ({udf_count})"


# -----------------------------------------------------------------------------
# Wide-string-literal merge
# -----------------------------------------------------------------------------
def test_wide_string_literals_not_merged():
    obj = _compile("wide_string_merge")
    rodata = _rodata_bytes(obj)

    # L"abc\0" as 32-bit little-endian chars.
    literal = b"a\x00\x00\x00b\x00\x00\x00c\x00\x00\x00\x00\x00\x00\x00"
    copies = _count_subseq(rodata, literal)

    assert rodata, ".rodata is empty"
    # Current codegen emits two copies; once merging lands this should become 1.
    assert copies == 2, f"expected two unmerged wide-string copies, got {copies}"
