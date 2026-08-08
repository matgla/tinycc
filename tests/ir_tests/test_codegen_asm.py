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
import hashlib
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
    # The object name must include the flags, not just the source: two tests
    # compiling the same case with different -mfloat-abi/-mfpu otherwise share
    # one .o and race under pytest-xdist.  That was invisible while both FP
    # tests expected the same lowering; it surfaced the moment one of them
    # started expecting VFP and the other __aeabi_ calls.
    tag = hashlib.sha1(" ".join(extra_cflags).encode()).hexdigest()[:8] if extra_cflags else "base"
    obj = BUILD_DIR / f"{name}.{tag}.o"
    BUILD_DIR.mkdir(parents=True, exist_ok=True)

    cflags = [
        "-O2",
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

    # R9 holds the PIC GOT base and is function-invariant, so 173819f4 hoisted
    # the save out of the call sites: one store in the prologue, a reload after
    # each call.  Exactly one — a second store means the per-call re-save is
    # back (42,356 of the 45,329 stores were dead), zero means the reloads have
    # nothing to read.
    assert str_r9 == 1, f"expected a single hoisted R9 save, got {str_r9} str.w r9"
    assert ldr_r9 >= 2, f"expected R9 restores after calls, got {ldr_r9} ldr.w r9"
    # Phase 1b (callee-saved R10 holding the GOT base) is not implemented yet.
    assert mov_r9_r10 == 0, "unexpected mov r9, r10 (Phase 1b not landed)"


# -----------------------------------------------------------------------------
# Forward conditional branch narrowing
# -----------------------------------------------------------------------------
def test_forward_branch_conditional_narrows():
    obj = _compile("forward_branch_narrow")
    funcs = _disassemble(obj)
    loop = funcs["loop"]

    wide = sum(_count_mnem(loop, m)
               for m in ("bgt.w", "bge.w", "blt.w", "ble.w", "beq.w", "bne.w", "b.w"))
    narrow_back = _count_mnem(loop, "blt.n")

    # Loop rotation is enabled, so the loop is bottom-tested: the back-edge is a
    # tight conditional narrow `blt.n` rather than an unconditional `b.n`.
    assert narrow_back >= 1, f"expected narrow backward blt.n, got {narrow_back}"
    # Every branch here is short-range, and the rehearsal pass can prove it for
    # the forward ones too, so nothing should be left in a wide encoding.
    assert wide == 0, f"expected all branches narrow, got {wide} wide"


# -----------------------------------------------------------------------------
# CBZ/CBNZ fusion
# -----------------------------------------------------------------------------
def test_cbz_fusion_fires():
    obj = _compile("cbz_fusion")
    funcs = _disassemble(obj)

    for name in ("iszero", "isnonzero"):
        fn = funcs[name]
        cmp_count = _count_mnem(fn, "cmp")
        cbz_count = _count_mnem(fn, "cbz") + _count_mnem(fn, "cbnz")
        cond = sum(_count_mnem(fn, m) for m in ("beq.w", "bne.w", "beq.n", "bne.n"))

        # `cmp rN,#0` + `b<eq|ne>` collapses into one 16-bit CBZ/CBNZ now that
        # the rehearsal pass can bound the forward distance from both sides.
        assert cbz_count >= 1, f"{name}: expected cbz/cbnz fusion, got {cbz_count}"
        assert cmp_count == 0, f"{name}: cmp #0 should have been folded away, got {cmp_count}"
        assert cond == 0, f"{name}: conditional branch should have been folded away, got {cond}"


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
def test_wide_string_literals_merged():
    obj = _compile("wide_string_merge")
    rodata = _rodata_bytes(obj)

    # L"abc\0" as 32-bit little-endian chars.
    literal = b"a\x00\x00\x00b\x00\x00\x00c\x00\x00\x00\x00\x00\x00\x00"
    copies = _count_subseq(rodata, literal)

    assert rodata, ".rodata is empty"
    # The string-literal pool dedupes identical read-only literals to one copy.
    assert copies == 1, f"expected merged wide-string literal (1 copy), got {copies}"


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
    # Hardware multiply; the narrow T16 MULS form is used when the destination
    # is also a source and NZCV is dead.
    assert _count_mnem_regex(funcs["mul_reg"], r"^muls?(\.w)?\b") >= 1, "mul_reg missing hardware multiply"


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
        assert _count_mnem_regex(fn, r"^muls?(\.w)?\b") >= 1, f"{name} missing hardware multiply"
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
    assert _count_mnem_regex(count, r"^bge\.[wn]") >= 1, "count missing forward conditional branch"
    assert _count_mnem(count, "b.n") >= 1, "count missing narrow back-edge"

    ifte = funcs["if_then_else"]
    # Chained if/else should use conditional execution or branches, not UDF.
    assert _count_mnem(ifte, "cmp") >= 1, "if_then_else missing comparison"
    cond_branches = sum(_count_mnem(ifte, m) for m in ("bgt.w", "bge.w", "blt.w", "ble.w", "beq.w", "bne.w", "b.w", "b.n", "ite"))
    assert cond_branches >= 1, "if_then_else missing any branch/conditional execution"
    udf_count = _count_mnem(ifte, "udf") + _count_mnem(ifte, "bkpt")
    assert udf_count == 0, f"if_then_else has unexpected undefined/breakpoint instructions ({udf_count})"


def test_cmp_common_base_offset_fold_fires():
    """Common-base CMP constant-offset fold: A = X+K1, B = X+K2 => K1 cond K2.

    Each function must collapse to a constant return (no cmp), proving the fold
    fired.  HEAD emits adds/adds/cmp + branch for these; a disabled or broken
    fold would reintroduce the cmp and fail here.  This is the firing-level
    regression that the QEMU test (361_cmp_offset_common_base.c), being
    correctness-only, cannot provide.
    """
    obj = _compile("cmp_offset_common_base")
    funcs = _disassemble(obj)
    expect = {"lt_if": 111, "gt_if": 222, "le_sel": 222, "ne_sel": 111, "sub_if": 111}
    for name, val in expect.items():
        fn = funcs[name]
        assert _count_mnem(fn, "cmp") == 0, f"{name}: common-base fold did not fire (cmp present)"
        assert _count_mnem_regex(fn, rf"^movs\s+r0, #{val}\b") >= 1, \
            f"{name}: expected folded constant return {val}"


def test_float_narrow_fires():
    """Soft-FP double->float demotion fold: f2d -> floor -> [d2f] narrows to floorf.

    Case 1 (`q`, result narrowed back to float) must collapse to a tail call to
    floorf with no f2d/d2f conversion calls and no double `floor` call.
    Case 2 (`q1`, result stays double) must swap to floorf + f2d (again no
    double `floor` and no d2f).  With the fold disabled or broken, the
    __aeabi_f2d/__aeabi_d2f helpers and the double `floor` call reappear.
    """
    obj = _compile("float_narrow")
    funcs = _disassemble(obj)

    q = funcs["q"]
    assert _count_mnem_regex(q, r"^b\.w\s+.*<floorf>") >= 1, \
        "q: demotion fold did not fire (no tail call to floorf)"
    assert not any("__aeabi_f2d" in ops or "__aeabi_d2f" in ops for _, ops in q), \
        "q: f2d/d2f conversion calls survived the demotion fold"
    assert not any(re.search(r"<floor>", ops) for _, ops in q), \
        "q: double floor() call survived the demotion fold"

    q1 = funcs["q1"]
    assert _count_mnem_regex(q1, r"^bl\s+.*<floorf>") >= 1, \
        "q1: demotion fold did not fire (no floorf call)"
    assert not any("__aeabi_d2f" in ops for _, ops in q1), \
        "q1: unexpected d2f in double-result shape"
    assert not any(re.search(r"<floor>", ops) for _, ops in q1), \
        "q1: double floor() call survived the demotion fold"


def test_return_const_reuse_fires():
    """Return-constant register reuse: `return C` on the equality edge of
    TEST_ZERO V / CMP V,#C returns V (provably == C there, already in a
    register) instead of rematerializing C.

    SSA home: ssa:branch (return-const reuse); the retired flat return_reuse
    pass did the same rewrite pre-SSA.  A disabled or broken fold
    reintroduces the `movs r0, #C` and fails here.  Covers the gcc-torture
    shapes pr106433::bar (TEST_ZERO, C == 0) and 920812-1::f (CMP, C == 1).
    """
    obj = _compile("return_const_reuse")
    funcs = _disassemble(obj)

    bar = funcs["bar_inf"]
    assert _count_mnem_regex(bar, r"^movs\s+r0, #0\b") == 0, \
        "bar_inf: return-const reuse did not fire (movs r0, #0 present)"

    fsw = funcs["f_switch"]
    assert _count_mnem_regex(fsw, r"^movs\s+r0, #1\b") == 0, \
        "f_switch: return-const reuse did not fire (movs r0, #1 present)"


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
    # 64-bit args arrive in r0:r1 and r2:r3; result leaves in r0:r1.  The
    # return-pair preference computes it there directly, so there is no copy
    # out of a callee-saved pair and no prolog at all.
    assert _count_mnem_regex(callee, r"^adds\s+r0, r0, r2") >= 1, "callee_long missing low-word add into r0"
    assert _count_mnem_regex(callee, r"^adcs?(\.w)?\s+r1,\s*(r1,\s*)?r3") >= 1, "callee_long missing high-word adc into r1"
    assert _count_mnem(callee, "push") == 0, "callee_long should need no callee-saved pair"

    caller = funcs["caller_long"]
    # Caller loads 64-bit args into r0:r1 and r2:r3 before the branch.
    assert _count_mnem(caller, "b.w") >= 1, "caller_long missing branch to callee"


def test_call_aapcs_stack_arg():
    obj = _compile("call_args")
    funcs = _disassemble(obj)

    callee = funcs["callee_stack"]
    # Fifth arg is passed on the stack and loaded from caller's frame.
    # Offset 8 = the two pushed regs; the old #24 additionally covered a
    # speculative 16-byte scratch reservation that dead-frame elimination
    # (dry-run-sized scratch areas) no longer allocates.
    assert _count_mnem_regex(callee, r"^ldr\s+r2, \[sp, #8\]") >= 1, "callee_stack missing stack-arg load"

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


def test_fp_hard_float_uses_vfp():
    """Anything but -mfloat-abi=soft should compute floats on the FPU.

    Was a strict xfail documenting the gap; single-precision arithmetic now
    lowers to vadd.f32 / vmul.f32 inline (thumb_emit_vfp_arith_mop) instead of
    __aeabi_fadd / __aeabi_fmul.  Only the *single*-precision ops are asserted:
    on fpv5-sp-d16 there is no double-precision unit, so addd legitimately
    stays a call.
    """
    obj = _compile("fp_select", extra_cflags=["-mfloat-abi=hard", "-mfpu=fpv5-sp-d16"])
    funcs = _disassemble(obj)

    for name, mnem in (("addf", "vadd.f32"), ("mulf", "vmul.f32")):
        assert _count_mnem(funcs[name], mnem) >= 1, f"{name} did not emit {mnem}"
        assert not any("__aeabi_fadd" in ops or "__aeabi_fmul" in ops for _, ops in funcs[name]), \
            f"{name} still calls a single-precision runtime helper"


# -----------------------------------------------------------------------------
# 64-bit register-deref LDRD/STRD pairing vs packed-access safety
# -----------------------------------------------------------------------------
def test_ldrd_deref_pairing_and_packed_safety():
    obj = _compile("ldrd_deref_pair")
    funcs = _disassemble(obj)

    # Aligned typed derefs: the align4_ok bit unlocks the paired encodings.
    assert _count_mnem(funcs["ll_load"], "ldrd") == 1, "ll_load: expected LDRD for *p (long long)"
    assert _count_mnem(funcs["ll_store"], "strd") == 1, "ll_store: expected STRD for *p = v"
    assert _count_mnem(funcs["d_load"], "ldrd") == 1, "d_load: expected LDRD for *p (double)"

    # Packed-derived accesses may be < 4-byte aligned: LDRD/STRD would fault
    # (UsageFault regardless of UNALIGN_TRP), so they must stay on the
    # unaligned-tolerant LDR/STR pair.  pk_arr/pk_arr_store also cover the
    # LOAD_INDEXED/STORE_INDEXED lowering via the underalign_hint transfer.
    for name in ("pk_load", "pk_store", "pk_arr", "pk_arr_store"):
        fn = funcs[name]
        paired = _count_mnem(fn, "ldrd") + _count_mnem(fn, "strd")
        assert paired == 0, f"{name}: LDRD/STRD on a packed access would fault, got {paired}"


def test_mem_inline_expansion_policy():
    obj = _compile("mem_inline_expand")
    funcs = _disassemble(obj)

    # n == 4: single ldr/str pair, no call, and never LDRD/STRD (char* base).
    cp4 = funcs["cp4"]
    assert _count_mnem(cp4, "bl") == 0, "cp4: 4-byte memcpy must inline"
    assert _count_mnem(cp4, "ldrd") + _count_mnem(cp4, "strd") == 0, \
        "cp4: LDRD/STRD would fault on an unaligned char*"

    # n == 8 memcpy: strict policy keeps the call (2-piece expansion loses
    # statically vs `movs #8; bl`).
    assert _count_mnem_regex(funcs["cp8"], r"^(bl|b\.w)\s") >= 1, \
        "cp8: 8-byte memcpy must stay a runtime call"

    # n == 8 memset: movs + two word STRs, no call, no STRD.
    st8 = funcs["st8"]
    assert _count_mnem(st8, "bl") == 0, "st8: 8-byte memset must inline"
    assert _count_mnem(st8, "strd") == 0, \
        "st8: STRD would fault on an unaligned char*"

    # n == 0: no call, no memory access at all.
    cp0 = funcs["cp0"]
    assert _count_mnem(cp0, "bl") == 0, "cp0: zero-size memcpy must vanish"

    # The float-bits reinterpret expands and store-load forwards: no call and
    # no stack traffic once the copy is forwarded.
    fb = funcs["fbits"]
    assert _count_mnem(fb, "bl") == 0, "fbits: 4-byte memcpy must inline"


def test_switch_pair_merge_no_spill():
    obj = _compile("sw_pair_merge")
    funcs = _disassemble(obj)

    # Every case must compute directly into the coalesced merge pair: no
    # per-case spill slots, no str/ldr round-trips through the stack.  The
    # broken shape was 4 sp accesses PER CASE (32 here); the only tolerated
    # stack traffic is the dispatch index itself (it crosses the R12-using
    # dispatch while every callee-saved register is occupied): 1 str + 2 ldr.
    fn = funcs["sw8"]
    sp_traffic = _count_mnem_regex(fn, r"^(str|ldr)(\.w)?\s.*\[sp")
    assert sp_traffic <= 3, \
        f"sw8: case temps must not spill (got {sp_traffic} sp accesses)"


def test_sym_base_reuse():
    obj = _compile("sym_base_reuse")
    funcs = _disassemble(obj)

    # Repeated accesses through the same global base must materialize the
    # symbol address exactly once; later accesses reuse the parked register
    # (get_scratch_reg_for_sym_addr + the imm_cache elide in load_full_const).
    for name in ("lookup6", "scatter"):
        fn = funcs[name]
        pool_loads = _count_mnem_regex(fn, r"^ldr(\.w)?\s+\S+,\s*\[pc")
        assert pool_loads == 1, \
            f"{name}: expected 1 literal-pool base load, got {pool_loads}"


def test_const_pool_hoist():
    obj = _compile("const_pool_hoist")
    funcs = _disassemble(obj)

    # A MOV/MVN/MOVW-unencodable constant (0x9e3779b1) must be loaded from
    # the literal pool exactly once per function and stay register-resident:
    # across calls via the entry hoist, around a call-free loop via the
    # preheader hoist (const classes in ssa:global_addr_hoist /
    # loop_addr_hoist).
    for name in ("hash_calls", "hash_loop"):
        fn = funcs[name]
        pool_loads = _count_mnem_regex(fn, r"^ldr(\.w)?\s+\S+,\s*\[pc")
        assert pool_loads == 1, \
            f"{name}: expected 1 literal-pool constant load, got {pool_loads}"

    # Straight-line regions deliberately have NO const hoist: local_addr_cse
    # regions are call/branch-free, where the machine-level imm_cache already
    # reuses the register when scratch churn permits, and a hoisted temp costs
    # a callee-saved push/pop (andok::foo went 6 -> 8 that way).  This bound
    # characterizes the residual churn misses; tighten it if that improves.
    straight = _count_mnem_regex(funcs["hash_straight"], r"^ldr(\.w)?\s+\S+,\s*\[pc")
    assert straight <= 3, \
        f"hash_straight: expected <=3 literal-pool loads, got {straight}"


def test_bool_chain_fuse():
    obj = _compile("bool_chain_fuse")
    funcs = _disassemble(obj)

    # Both bool checks must fold to `bl f; cmp; b<cond>`: no SETIF
    # materialization (ite/movne/moveq) and exactly one comparison.
    for name in ("chk_direct", "chk_inverted"):
        fn = funcs[name]
        assert _count_mnem(fn, "ite") == 0, f"{name}: SETIF chain not fused"
        assert _count_mnem(fn, "cmp") == 1, f"{name}: expected a single cmp"


# -----------------------------------------------------------------------------
# Inline asm operand marshalling
# -----------------------------------------------------------------------------
def _symbolic_gpr_eval(func_insns, params):
    """Interpret a mov/add-only function symbolically over its parameters.

    Returns the Counter of parameter terms left in r0 (the return register).
    Registers start holding the AAPCS core arguments, so r0="a", r1="b", ...
    Anything other than a register mov/add or the final `bx lr` fails the test:
    the point is to notice unexpected codegen rather than to skip past it.
    """
    state = {i: Counter([name]) for i, name in enumerate(params)}
    reg = re.compile(r"^r(\d+)$|^(ip)$")

    def rnum(tok):
        m = reg.match(tok.strip())
        assert m, f"unexpected operand {tok!r} in {func_insns}"
        return 12 if m.group(2) else int(m.group(1))

    for mnem, ops in func_insns:
        parts = [p.strip() for p in ops.split(",")] if ops else []
        if mnem in ("mov", "mov.w", "movs"):
            d, s = rnum(parts[0]), rnum(parts[1])
            state[d] = Counter(state.get(s, Counter()))
        elif mnem in ("add", "add.w", "adds"):
            if len(parts) == 2:  # T1: add rd, rm
                d, s = rnum(parts[0]), rnum(parts[1])
                state[d] = state.get(d, Counter()) + state.get(s, Counter())
            else:
                d, a, b = rnum(parts[0]), rnum(parts[1]), rnum(parts[2])
                state[d] = state.get(a, Counter()) + state.get(b, Counter())
        elif mnem == "bx":
            break
        else:
            raise AssertionError(f"unexpected instruction {mnem} {ops!r}")
    return state.get(0, Counter())


def test_asm_operand_loads_do_not_clobber_each_other():
    """Each "r" operand must reach the asm body holding its own value.

    Regression test for asm_gen_code emitting the operand loads in declaration
    order: with register-allocated values that is a parallel move, and loading
    %1 into a register that still held %2's source destroyed it.  Float
    parameters trigger the permutation (a in r2, b in r0, constraints wanting r0
    and r1), so `"r"(a), "r"(b)` loaded `a` into both -- which is why
    libvfpv4sp's hardware __aeabi_fadd computed a+a.
    """
    obj = _compile("asm_operand_shuffle")
    funcs = _disassemble(obj)

    cases = {
        "two_floats": ["a", "b"],
        "two_ints": ["a", "b"],
        "three_floats": ["a", "b", "c"],
    }
    for name, params in cases.items():
        got = _symbolic_gpr_eval(funcs[name], params)
        want = Counter(params)
        assert got == want, (
            f"{name}: asm body summed {sorted(got.elements())}, expected "
            f"{sorted(want.elements())} -- an operand load clobbered another "
            f"operand's source"
        )
