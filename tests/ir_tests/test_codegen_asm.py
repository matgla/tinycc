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

    wide_fwd = sum(_count_mnem(loop, m) for m in ("bgt.w", "bge.w", "blt.w", "ble.w", "beq.w", "bne.w"))
    narrow_back = _count_mnem(loop, "blt.n")

    # Loop rotation is enabled, so the loop is bottom-tested: the back-edge is a
    # tight conditional narrow `blt.n` rather than an unconditional `b.n`.
    assert narrow_back >= 1, f"expected narrow backward blt.n, got {narrow_back}"
    # Forward conditional branches still stay wide (Phase 2a not landed).
    assert wide_fwd >= 1, f"expected forward wide conditional branch, got {wide_fwd}"


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


# -----------------------------------------------------------------------------
# Phase 4: backend per-instruction-family correctness
# -----------------------------------------------------------------------------

# -----------------------------------------------------------------------------
# Arithmetic: immediate and register operand shapes
# -----------------------------------------------------------------------------
def test_arith_imm_reg_shapes():
    obj = _compile("arith_imm_reg")
    funcs = _disassemble(obj)

    # ADD/SUB immediate should use narrow ALU-immediate forms.
    assert _count_mnem(funcs["add_imm"], "adds") >= 1, "add_imm missing adds"
    assert _count_mnem(funcs["sub_imm"], "subs") >= 1, "sub_imm missing subs"
    # MUL by constant 7 should lower to shift/sub, not a helper call.
    assert _count_mnem(funcs["mul_imm"], "lsls") >= 1, "mul_imm missing shift"
    assert _count_mnem(funcs["mul_imm"], "subs") >= 1, "mul_imm missing subtract"
    assert not any("__aeabi" in ops for _, ops in funcs["mul_imm"]), "mul_imm unexpectedly calls runtime helper"

    # Register forms.
    assert _count_mnem(funcs["add_reg"], "adds") >= 1, "add_reg missing adds"
    assert _count_mnem(funcs["sub_reg"], "subs") >= 1, "sub_reg missing subs"
    assert _count_mnem(funcs["mul_reg"], "mul.w") >= 1, "mul_reg missing mul.w"


# -----------------------------------------------------------------------------
# Arithmetic: DIV/IMOD lowering
# -----------------------------------------------------------------------------
def test_arith_div_mod_lowering():
    obj = _compile("arith_div_mod")
    funcs = _disassemble(obj)

    # Signed/unsigned division should use SDIV/UDIV on Cortex-M33.
    assert _count_mnem(funcs["div_signed"], "sdiv") >= 1, "signed division missing sdiv"
    assert _count_mnem(funcs["div_unsigned"], "udiv") >= 1, "unsigned division missing udiv"

    # Modulo should lower to div + mul + sub, no runtime helper.
    for name in ("mod_signed", "mod_unsigned"):
        fn = funcs[name]
        div_mnem = "sdiv" if name == "mod_signed" else "udiv"
        assert _count_mnem(fn, div_mnem) >= 1, f"{name} missing {div_mnem}"
        assert _count_mnem(fn, "mul.w") >= 1, f"{name} missing mul.w"
        assert _count_mnem(fn, "subs") >= 1, f"{name} missing subs"
        assert not any("__aeabi" in ops for _, ops in fn), f"{name} unexpectedly calls runtime helper"


# -----------------------------------------------------------------------------
# Memory: LOAD/STORE/LEA addressing modes
# -----------------------------------------------------------------------------
def test_mem_load_store_addressing():
    obj = _compile("mem_load_store")
    funcs = _disassemble(obj)

    # PC-relative literal load for globals.
    assert _count_mnem_regex(funcs["load_global"], r"^ldr.*\[pc,") >= 1, "load_global missing pc-relative load"
    assert _count_mnem_regex(funcs["store_global"], r"^ldr.*\[pc,") >= 1, "store_global missing pc-relative base load"
    assert _count_mnem(funcs["store_global"], "str") >= 1, "store_global missing store"

    # Indexed array access: ldr.w/str.w [rn, rm, lsl #2].
    assert _count_mnem_regex(funcs["load_array"], r"ldr\.w.*lsl #2") >= 1, "load_array missing scaled indexed load"
    assert _count_mnem_regex(funcs["store_array"], r"str\.w.*lsl #2") >= 1, "store_array missing scaled indexed store"

    # Struct offset uses immediate offset.
    assert _count_mnem_regex(funcs["load_struct"], r"ldr.*#12") >= 1, "load_struct missing offset load"
    assert _count_mnem_regex(funcs["store_struct"], r"str.*#12") >= 1, "store_struct missing offset store"

    # LEA of a local is an SP-based add.
    lea = funcs["lea_local"]
    assert _count_mnem_regex(lea, r"^add\s+r0, sp") >= 1, "lea_local missing add r0, sp"


def test_rmw_partial_bitfield_clears_collapse_to_byte_store():
    obj = _compile("rmw_byte_clear_run")
    funcs = _disassemble(obj)
    fn = funcs["clear_display"]

    assert _count_mnem(fn, "strb") == 1, "multi-mask byte clear should lower to one strb"
    assert _count_mnem(fn, "and") + _count_mnem(fn, "and.w") == 0, "byte clear left word RMW masks"


def test_vector_xor_eq_self_avoids_long_cmp_forwarding():
    obj = _compile("vector_xor_eq_self", extra_cflags=["-O2"])
    funcs = _disassemble(obj)
    fn = funcs["vector_xor_eq_self"]

    assert len(fn) <= 100, f"vector_xor_eq_self regressed to {len(fn)} instructions"


# -----------------------------------------------------------------------------
# Control: switch table and branch narrowing
# -----------------------------------------------------------------------------
def test_control_switch_uses_table():
    obj = _compile("control_switch")
    funcs = _disassemble(obj)
    fn = funcs["switch_small"]

    # A dense switch should emit a jump table (ADD PC) and a bounds check.
    assert _count_mnem_regex(fn, r"^add.*pc") >= 1, "switch_small missing pc-indexed table lookup"
    assert _count_mnem(fn, "cmp") >= 1, "switch_small missing bounds comparison"
    assert _count_mnem_regex(fn, r"^ldr\.w.*\[ip,") >= 1, "switch_small missing table entry load"


def test_control_branch_conditional_and_loop():
    obj = _compile("control_branch")
    funcs = _disassemble(obj)

    count = funcs["count"]
    # Loop should have a conditional forward test and a narrow back-edge.
    assert _count_mnem(count, "cmp") >= 1, "count missing comparison"
    assert _count_mnem_regex(count, r"^bge\.w") >= 1, "count missing forward conditional branch"
    assert _count_mnem(count, "b.n") >= 1, "count missing narrow back-edge"

    ifte = funcs["if_then_else"]
    # Chained if/else should use conditional execution or branches, not UDF.
    assert _count_mnem(ifte, "cmp") >= 1, "if_then_else missing comparison"
    cond_branches = sum(_count_mnem(ifte, m) for m in ("bgt.w", "bge.w", "blt.w", "ble.w", "beq.w", "bne.w", "b.w", "b.n", "ite"))
    assert cond_branches >= 1, "if_then_else missing any branch/conditional execution"
    udf_count = _count_mnem(ifte, "udf") + _count_mnem(ifte, "bkpt")
    assert udf_count == 0, f"if_then_else has unexpected undefined/breakpoint instructions ({udf_count})"


# -----------------------------------------------------------------------------
# Calls: AAPCS parameter marshalling and return values
# -----------------------------------------------------------------------------
def test_call_aapcs_register_args():
    obj = _compile("call_args")
    funcs = _disassemble(obj)

    caller = funcs["caller_int"]
    # First four int args go in r0-r3; the caller loads them from globals.
    # Tail-call optimized to b.w (still a correct call transfer).
    assert _count_mnem(caller, "b.w") >= 1, "caller_int missing branch to callee"
    # Callee uses r0-r3 as its parameters.
    callee = funcs["callee_int"]
    # narrow-preference regalloc keeps the a+b temp in a low reg (r4), so all
    # three adds use the 16-bit ADDS encoding
    assert _count_mnem_regex(callee, r"^adds\s+r4, r0, r1") >= 1, "callee_int missing r0+r1 add"
    assert _count_mnem_regex(callee, r"^adds\s+r0, r4, r2") >= 1, "callee_int missing r4+r2 add"
    assert _count_mnem_regex(callee, r"^adds\s+r0, r0, r3") >= 1, "callee_int missing r0+r3 add"


def test_call_aapcs_long_long():
    obj = _compile("call_args")
    funcs = _disassemble(obj)

    callee = funcs["callee_long"]
    # 64-bit args arrive in r0:r1 and r2:r3; result leaves in r0:r1.
    assert _count_mnem_regex(callee, r"^adds\s+r4, r0, r2") >= 1, "callee_long missing low-word add"
    assert _count_mnem_regex(callee, r"^adc\.w\s+r5, r1, r3") >= 1, "callee_long missing high-word adc"

    caller = funcs["caller_long"]
    # Caller loads 64-bit args into r0:r1 and r2:r3 before the branch.
    assert _count_mnem(caller, "b.w") >= 1, "caller_long missing branch to callee"


def test_call_aapcs_stack_arg():
    obj = _compile("call_args")
    funcs = _disassemble(obj)

    callee = funcs["callee_stack"]
    # Fifth arg is passed on the stack and loaded from caller's frame.
    assert _count_mnem_regex(callee, r"^ldr\s+r2, \[sp, #24\]") >= 1, "callee_stack missing stack-arg load"

    caller = funcs["caller_stack"]
    # Caller must store the fifth arg to its own stack before calling.
    assert _count_mnem(caller, "push") >= 1, "caller_stack missing prolog"
    assert _count_mnem(caller, "str") >= 1, "caller_stack missing stack-arg store"
    assert _count_mnem(caller, "bl") >= 1, "caller_stack missing bl"


# -----------------------------------------------------------------------------
# Floating point: soft-float vs hard-float selection
# -----------------------------------------------------------------------------
def test_fp_soft_float_uses_runtime_helpers():
    obj = _compile("fp_select", extra_cflags=["-mfloat-abi=soft"])
    funcs = _disassemble(obj)

    for name in ("addf", "addd", "mulf"):
        fn = funcs[name]
        assert any("__aeabi_fadd" in ops or "__aeabi_dadd" in ops or "__aeabi_fmul" in ops for _, ops in fn), \
            f"{name} missing expected soft-float runtime helper"
        vfp_count = sum(_count_mnem(fn, m) for m in ("vadd.f32", "vadd.f64", "vmul.f32", "vmul.f64"))
        assert vfp_count == 0, f"{name} unexpectedly emitted VFP instruction under soft float"


@pytest.mark.xfail(
    reason="hard-float VFP lowering not implemented yet (Phase 4 gap): fp_select "
    "still emits __aeabi_fadd/__aeabi_dadd/__aeabi_fmul under -mfloat-abi=hard. "
    "Remove this marker once the hard-float codegen work lands.",
    strict=True,
)
def test_fp_hard_float_uses_vfp():
    """Hard-float ABI with VFP should select VFP instructions, not __aeabi_* helpers.

    This test documents the current codegen gap: even with -mfloat-abi=hard
    -mfpu=fpv5-sp-d16, fp_select lowers to __aeabi_fadd/__aeabi_dadd/__aeabi_fmul.
    See Phase 4 findings in docs/plan_whole_tinycc_coverage.md.
    """
    obj = _compile("fp_select", extra_cflags=["-mfloat-abi=hard", "-mfpu=fpv5-sp-d16"])
    funcs = _disassemble(obj)

    vfp_count = sum(
        _count_mnem(funcs[name], m)
        for name in ("addf", "addd", "mulf")
        for m in ("vadd.f32", "vadd.f64", "vmul.f32", "vmul.f64")
    )
    assert vfp_count >= 1, "hard-float ABI did not emit any VFP instructions (Phase 4 gap)"
