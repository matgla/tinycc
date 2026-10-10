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


def _compile(name, extra_cflags=(), src_dir=None):
    """Cross-compile a case in asm/<name>.c (or <src_dir>/<name>.c) to an object file."""
    src = (src_dir or ASM_DIR) / f"{name}.c"
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
        # Mirror the QEMU Makefile's compile step: the armv8m-tcc at the tree
        # root may be a YasOS-flavour cross, and without these the fixtures
        # come out R9/SB-relative -- bare metal never initializes R9, and
        # arm-none-eabi-objdump cannot even disassemble the relocations
        # ("unsupported relocation type 0x8b").  A fixture that wants the
        # separation passes -mpic-data-is-text-relative as an extra flag,
        # which sits later on the command line and wins.
        "-fno-pic",
        "-mno-sb-relative-got",
        # GNU objdump does not understand the YasOS module-call relocation.
        "-fno-module-local-calls",
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


def test_small_struct_copy_padding_keeps_narrow_tail():
    funcs = _disassemble(_compile("bug_small_struct_copy_padding",
                                 extra_cflags=["-minline-atomics"], src_dir=Path(__file__).parent))
    for name in ("acquire", "half_after_copy"):
        insns = funcs[name]
        assert not any(m.startswith(("ldr", "str")) and "[sp" in o for m, o in insns), insns


# -----------------------------------------------------------------------------
# R9 GOT-base save/restore
# -----------------------------------------------------------------------------
def test_r9_spill_around_calls():
    # Reproduce what an ARM-Linux-flavour cross + TDS used to produce here:
    # a YasOS-flavour cross additionally defaults module-local-call marking
    # (R_ARM_YASOS_LOCAL_CALL = 0x8b, which arm-none-eabi-objdump cannot
    # disassemble) and the r9-reload elision it licenses -- the reload-after-
    # each-call pattern this test characterizes needs it off.
    obj = _compile("r9_spill", extra_cflags=["-mpic-data-is-text-relative", "-fno-module-local-calls"])
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

    # `struct S s = make();` returns into s's own slot; the store of s onto
    # itself used to be an __aeabi_memmove and is now skipped.
    assert not any("__aeabi_memmove" in ops for _, ops in caller), "caller copies s onto itself"
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
    # `while (i < n) i++` is bottom-tested (ssa:loop_header_dup): a forward
    # zero-trip guard and a narrow conditional back-edge, no unconditional `b`.
    assert _count_mnem(count, "cmp") >= 1, "count missing comparison"
    assert _count_mnem_regex(count, r"^ble\.[wn]") >= 1, "count missing forward zero-trip guard"
    assert _count_mnem(count, "blt.n") >= 1, "count missing narrow conditional back-edge"
    assert _count_mnem(count, "b.n") == 0, "count kept an unconditional back-edge"

    ifte = funcs["if_then_else"]
    # Chained if/else should use conditional execution or branches, not UDF.
    assert _count_mnem(ifte, "cmp") >= 1, "if_then_else missing comparison"
    cond_branches = sum(_count_mnem(ifte, m) for m in ("bgt.w", "bge.w", "blt.w", "ble.w", "beq.w", "bne.w", "b.w", "b.n", "ite"))
    assert cond_branches >= 1, "if_then_else missing any branch/conditional execution"
    udf_count = _count_mnem(ifte, "udf") + _count_mnem(ifte, "bkpt")
    assert udf_count == 0, f"if_then_else has unexpected undefined/breakpoint instructions ({udf_count})"


def test_size_flags_alias_o2():
    """-Os / -Oz are a real tier, not a silent -O0.

    `s->optimize = atoi(optarg)` turned both into 0, so -Os meant "no
    optimization" -- and since -O is a plain option scan, `-O2 ... -Os` (which
    is what toybox passes) threw the -O2 away.  They run the -O2 pipeline
    (test_size_tier covers where they part from it), and -Oz is -Os.
    """
    o0 = _disassemble(_compile("dead_loop_rotated", extra_cflags=["-O0"]))
    os_ = _disassemble(_compile("dead_loop_rotated", extra_cflags=["-Os"]))
    oz = _disassemble(_compile("dead_loop_rotated", extra_cflags=["-Oz"]))
    o2_os = _disassemble(_compile("dead_loop_rotated", extra_cflags=["-O2", "-Os"]))

    assert oz == os_, "-Oz should select the same tier as -Os"
    assert o2_os == os_, "a later -Os should win over an earlier -O2"
    assert os_ != o0, "-Os is still a silent -O0"


def test_size_tier():
    """-Os: where a choice trades code size for speed, the smaller code.

    - no word-alignment NOPs before loop heads and in-loop joins (-O2 pads
      them for the M33's fetch; 72 KB of zig.c),
    - an aligned block copy longer than the largest copy stub stays a
      memmove call instead of an inline LDM/STM loop,
    - __OPTIMIZE_SIZE__ is defined, as gcc does.
    """
    o2 = _disassemble(_compile("size_tier", extra_cflags=["-O2"]))
    os_ = _disassemble(_compile("size_tier", extra_cflags=["-Os"]))

    pads_o2 = sum(_count_mnem_regex(fn, r"^nop") for fn in o2.values())
    pads_os = sum(_count_mnem_regex(fn, r"^nop") for fn in os_.values())
    assert pads_o2 > 0, "the -O2 build should pad some loop head (case no longer exercises alignment)"
    assert pads_os == 0, f"-Os padded {pads_os} branch targets"

    assert _count_mnem_regex(o2["copy_big"], r"^ldmia") > 0, "-O2 should copy 160 bytes inline"
    assert _count_mnem_regex(os_["copy_big"], r"^bl\b") == 1, f"-Os should call memmove: {os_['copy_big']}"
    assert _count_mnem_regex(os_["copy_big"], r"^ldm") == 0, "-Os still copies inline"

    assert os_["size_macro"][0] == ("movs", "r0, #1"), "__OPTIMIZE_SIZE__ not defined at -Os"
    assert o2["size_macro"][0] == ("movs", "r0, #0"), "__OPTIMIZE_SIZE__ defined at -O2"


def test_branch_over_pool_window_narrows():
    """A forward B over more than a literal-pool window is 16-bit if it fits.

    The then-arm's jump over the 1 KB else-arm used to stay B.W because a pool
    dump could land inside the range -- and one does (the arm's second B.W is
    that dump's skip branch).  A 16-bit B is safe whenever its offset fits with
    every dump the range can take; 7,900 branches in zig.c.
    """
    fn = _disassemble(_compile("branch_over_pool_window"))["over_pool_window"]
    branches = [(m, o) for m, o in fn if re.match(r"^b(\.[nw])?$", m)]
    assert branches and branches[0][0] == "b.n", f"jump over the else-arm is wide: {branches[:2]}"
    assert any(m == "b.w" for m, _ in branches[1:]), "no pool dump inside the range any more"


def test_cmp_negative_small_is_adds():
    """CMP Rn, #-k (k = 1..7, Rn low) is ADDS Rt, Rn, #k into a free low register.

    Rn - (-k) and Rn + k are one sum with one carry and one overflow, so the
    flags match, in 16 bits where the compare needs CMP.W (Zig compares
    against maxInt(u32), its `none` index, 1,800 times in zig.c).
    """
    funcs = _disassemble(_compile("cmp_neg_small"))
    for name, k in (("is_none", 1), ("below_minus3", 3)):
        fn = funcs[name]
        assert _count_mnem_regex(fn, r"^(cmp|cmn)") == 0, f"{name} still compares: {fn}"
        assert _count_mnem_regex(fn, rf"^adds r[0-7], r0, #{k}$") == 1, f"{name}: {fn}"


def test_literal_body_word_aligned():
    """A body that reads its literal pool starts word-aligned.

    A literal load's offset and the pad before a pool dump depend on the
    address mod 4, so the same function at a 2 mod 4 start is different
    machine code, and identical code folding compares machine code: lit_b
    only folds onto lit_a when both start aligned.
    """
    obj = _compile("func_align_literal", extra_cflags=["-fno-function-sections"])
    syms = {}
    out = subprocess.run(["arm-none-eabi-nm", str(obj)], stdout=subprocess.PIPE, text=True).stdout
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3:
            syms[parts[2]] = int(parts[0], 16)
    assert syms["pad_before"] == 0 and syms["lit_a"] % 4 == 0, f"lit_a not word-aligned: {syms}"
    assert syms["lit_b"] == syms["lit_a"], f"identical lit_b was not folded onto lit_a: {syms}"


def test_hi_callee_pref():
    """More call-crossing values than r4-r7: the compared one keeps a low register.

    Five pointers are only passed from call to call; `k` is compared with a
    constant after each.  First come first served gave the pointers r4-r7 and
    `k` a high register, so every compare was CMP.W; ra:hi_callee_pref gives
    values at most one 16-bit-capable op touches r8-r11 first.
    """
    fn = _disassemble(_compile("hi_callee_pref"))["keep_low"]
    cmps = [(m, o) for m, o in fn if m.startswith("cmp")]
    assert len(cmps) == 4, f"expected four compares: {fn}"
    assert all(m == "cmp" and re.match(r"r[0-7], #", o) for m, o in cmps), f"a compare is wide: {cmps}"


def test_has_builtin():
    """__has_builtin answers for the builtins tcc implements.

    It was `#define __has_builtin(x) 0`, so zig.h built its byte swaps from
    shifts and ORs.  It must also expand outside #if: zig.h pastes the 0/1
    answer into a name to choose a libm import.
    """
    funcs = _disassemble(_compile("has_builtin", extra_cflags=["-nostdinc", f"-I{ROOT / 'include'}"]))
    assert _count_mnem_regex(funcs["swap16"], r"^rev16") == 1, f"swap16: {funcs['swap16']}"
    assert _count_mnem_regex(funcs["swap32"], r"^rev ") == 1, f"swap32: {funcs['swap32']}"
    assert ("movs", "r0, #10") in funcs["answers"], f"answers: {funcs['answers']}"


def test_shift64_var_os_calls_helper():
    """-Os: a 64-bit shift by a variable count is the __aeabi_* helper call.

    Inline it is eight instructions at every site (440 of them in zig.c);
    -O2 keeps that, -Os takes the call.  A constant count stays inline.
    """
    o2 = _disassemble(_compile("shift64_var", extra_cflags=["-O2"]))
    os_ = _disassemble(_compile("shift64_var", extra_cflags=["-Os"]))
    for name, helper in (("shl", "__aeabi_llsl"), ("shr", "__aeabi_llsr"), ("sar", "__aeabi_lasr")):
        assert _count_mnem_regex(o2[name], helper) == 0, f"-O2 {name} calls {helper}"
        assert _count_mnem_regex(os_[name], helper) == 1, f"-Os {name}: {os_[name]}"
    assert _count_mnem_regex(os_["shl_const"], r"__aeabi") == 0, "a constant shift became a call"


def test_cmp_type_extreme_folds():
    """A compare against an end of its operand's own range is constant.

    `x > 0xFFFFFFFFu` and `x < INT32_MIN` hold for no x: VRP folds the
    CMP whether written directly or made so by inlining -- zig.h's overflow
    wrappers check every result against zig_minInt/zig_maxInt of its full
    width that way.  (A frontend gen_opic fold of the same compares broke
    the self-hosted tcc on the device; see the commit.)
    """
    funcs = _disassemble(_compile("cmp_type_extreme",
                                  extra_cflags=["-nostdinc", f"-I{ROOT / 'include'}",
                                                f"-I{Path(__file__).parent / 'libc_includes' / 'newlib'}"]))
    for name in ("u_above_max", "s_below_min", "inlined"):
        assert _count_mnem_regex(funcs[name], r"^cmp") == 0, f"{name} still compares: {funcs[name]}"


def test_it_mov_narrow():
    """A small constant moved inside an IT block uses the 16-bit encoding.

    Inside an IT block MOV #imm8 sets no flags, so SELECT arms and the
    BOOL_AND/BOOL_OR result need not be the flag-preserving MOV.W.
    """
    funcs = _disassemble(_compile("it_mov_narrow"))
    for name, fn in funcs.items():
        wide = [(m, o) for m, o in fn if re.match(r"^mov(eq|ne|lt|ge|gt|le|cc|cs|hi|ls)\.w$", m) and re.match(r"r[0-7], #", o)]
        assert not wide, f"{name} has a wide conditional move: {fn}"


def test_switch_const_selector_folds():
    """ssa:switch_fold: `switch (7)` picks its arm at compile time.

    Before, the selector was never looked at: SCCP propagated the constant into
    the dispatch and stopped, so const_seven became a .rodata value table read
    with a constant index, and the in-loop shape kept a full PC-relative
    indirect jump on every iteration.  A runtime selector must still dispatch.
    """
    obj = _compile("switch_const_selector")
    funcs = _disassemble(obj)

    for name, want in (("const_seven", "1000"), ("const_out_of_range", "-1")):
        fn = funcs[name]
        assert len(fn) == 2, f"{name} should be one move plus bx lr, got {fn}"
        assert _count_mnem_regex(fn, r"^ldr") == 0, f"{name} still reads a table"

    # The dispatch is gone, so ssa:dead_loop can take the loop with it: what
    # is left is at most the zero-trip guard, a forward branch.  (With the
    # dispatch ahead of the bodies, switch_head.c, the flat passes fold the
    # switch before the loop passes and the guard stays a branch instead of
    # an ITE -- the same 14 bytes.)
    loop = funcs["const_switch_in_loop"]
    assert _count_mnem_regex(loop, r"^b[a-z]*\.[wn] ") <= 1 and len(loop) <= 6, (
        f"const_switch_in_loop still loops: {loop}"
    )
    assert _count_mnem_regex(loop, r"^ldr") == 0, "const_switch_in_loop still reads a table"

    var = funcs["variable_selector"]
    assert _count_mnem_regex(var, r"^bx (ip|r)") >= 1, (
        "variable_selector lost its indirect dispatch"
    )


def test_dead_loop_rotated_kills_back_edge():
    """ssa:dead_loop, rotated arm: a pure counting loop whose exit value is a
    constant must lose its back-edge entirely, leaving only the entry guard.

    Every loop tcc rotates is bottom-tested, and the pass used to require a CMP
    as the first instruction of the header -- so it matched nothing real and
    const_result kept a 2-cmp / 2-branch counting loop.  The negatives each
    trip one of the conditions and must still count.
    """
    obj = _compile("dead_loop_rotated")
    funcs = _disassemble(obj)

    branch = r"^b[a-z]*\.[wn] "

    kept = funcs["const_result"]
    assert _count_mnem(kept, "cmp") == 1, (
        f"const_result should keep only the entry guard's cmp, got "
        f"{_count_mnem(kept, 'cmp')}"
    )
    assert _count_mnem_regex(kept, branch) == 1, (
        f"const_result still has a back-edge: "
        f"{_count_mnem_regex(kept, branch)} branches"
    )

    for name, why in (
        ("mangle", "exit value depends on the trip count"),
        ("counter_escapes", "the counter is read after the loop"),
        ("store_each", "the body stores"),
        ("volatile_poll", "the body reads volatile memory"),
    ):
        fn = funcs[name]
        assert _count_mnem_regex(fn, branch) >= 2, (
            f"{name} lost its back-edge but {why}"
        )


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


def test_nonneg_cmp_zero_fold_keeps_nan_directions_dcp():
    """ssa:branch must not fold `fabs(x) >= 0.0` on the inline DCP compare.

    A NaN makes that false, so only the `< 0.0` half may be decided at compile
    time (gcc.c-torture/execute/20020720-1.c pins that half).  The DCP spells
    every relation with an *unsigned* condition token, and reading those as if
    the U meant "unordered-true" folded the wrong half -- silently, because no
    host or QEMU target has a DCP to run the result on.  ir_tests/441 catches
    it on real RP2350 silicon; this catches it here.
    """
    obj = _compile("nonneg_cmp_zero_fold", extra_cflags=["-mfloat-abi=hard", "-mfpu=rp2350"])
    funcs = _disassemble(obj)

    # `mrc 4, 0, APSR_nzcv, ...` is the DCP compare handing its result to the
    # flags; a folded compare has no flag read at all.  Objects carry their
    # .ARM.attributes now, and objdump given an FPv5 Tag_FP_arch prints
    # coprocessor-4 instructions as "<UNDEFINED> instruction: 0xee1?f4??" --
    # an MRC (bit 20 set) to cp4 with Rt = 15, i.e. the same APSR_nzcv read.
    def reads_dcp_flags(fn):
        return _count_mnem_regex(funcs[fn], r"\bAPSR_nzcv\b|0xee[13579bdf][0-9a-f]f4[0-9a-f]{2}\b") >= 1

    for name in ("ge_must_not_fold", "le0_must_not_fold"):
        assert reads_dcp_flags(name), (
            f"{name}: fabs() can be a NaN, so the compare must survive to run time"
        )
    for name in ("lt_folds", "gt0_folds"):
        assert not reads_dcp_flags(name), (
            f"{name}: false for every value including NaN, so it should fold away"
        )


# -----------------------------------------------------------------------------
# 64-bit register-deref LDRD/STRD pairing vs packed-access safety
# -----------------------------------------------------------------------------
def test_aggregate_copy_preserves_alignment():
    funcs = _disassemble(_compile("bug_aggregate_copy_alignment", src_dir=ASM_DIR.parent))
    for name, forbidden in (("copy_packed", r"^(ldrd|strd|ldm|stm)"),
                            ("load_packed", r"^(ldrd|ldm)"),
                            ("store_packed", r"^(strd|stm)")):
        unsafe = [(mnem, ops) for mnem, ops in funcs[name]
                  if re.match(forbidden, mnem) and not re.search(r"\bsp\b", ops)]
        assert not unsafe, f"{name}: unaligned aggregate access: {unsafe}"


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


def test_memcpy_local_copy8_expands():
    """An 8-byte copy through a local's address expands to plain accesses.

    The Zig C backend spells an align(1) 8-byte load as `memcpy(&tmp, p, 8)`
    into a local that is only read back (and the store side as `memcpy(p,
    &local, 8)`).  mem_inline's one-piece policy leaves both a `bl memcpy`
    (~25 instructions run) and a stack slot; with a single-def `T <- &slot`
    temp on either end the two-word expansion wins: the call and its argument
    marshaling go, and the slot is filled by plain stores instead of the
    runtime.  The control pins that a copy whose ends are both plain pointers
    keeps the call.
    """
    funcs = _disassemble(_compile("bug_memcpy_local_copy8", src_dir=ASM_DIR.parent))

    sum_at = funcs["sum_at"]
    assert _count_mnem_regex(sum_at, r"^bl\b") == 0, \
        f"sum_at still calls memcpy: {sum_at}"
    # What must go is the call and its argument marshaling: the pieces are
    # plain accesses, so the slot itself is no longer filled by the runtime.
    put_at = funcs["put_at"]
    assert _count_mnem_regex(put_at, r"^bl\b") == 0, \
        f"put_at still calls memcpy: {put_at}"

    cp8 = funcs["cp8_plain"]
    assert _count_mnem_regex(cp8, r"^(bl|b\.w)\s") >= 1, \
        "cp8_plain: a plain-pointer 8-byte copy must keep the runtime call"


def test_eqlbytes_local_byte_roundtrip():
    """A by-value align(1) struct copy read back byte-wise stays register-resident.

    The Zig C backend spells an align(1) 4-byte load as a by-value struct copy
    into a local whose only uses are scalar byte reads (mem.eqlBytes).  The
    frontend lowers the copy as four byte STOREs into stack slots and reads
    them back through a constant-trip countdown; ssa:loop_unroll makes the
    readback offsets constant in the RA pipeline, long after sl_forward ran,
    so each group paid 4 strb + 4 reload ldrb through [sp].  The fix
    forwards the stored byte values into their readbacks; the shape lock is
    that neither compare-side function touches [sp].
    """
    funcs = _disassemble(_compile("bug_eqlbytes_stack_roundtrip", src_dir=ASM_DIR.parent))

    for name in ("eql4", "be_sum"):
        fn = funcs[name]
        sp_traffic = _count_mnem_regex(fn, r"^(ldr|str)[a-z.]*\s.*\[sp")
        assert sp_traffic == 0, \
            f"{name}: byte groups must not round-trip through stack slots (got {sp_traffic} sp accesses)"


def test_eqlbytes_kernel_byte_roundtrip():
    """The full kernel shape must forward every byte copied into its local buffer."""
    funcs = _disassemble(_compile("bug_eqlbytes_kernel_roundtrip", src_dir=ASM_DIR.parent))
    fn = funcs["mem_eqlBytes__1647"]
    for name in ("mem_eqlBytes__1647", "word_with_flag"):
        byte_stores = _count_mnem_regex(funcs[name], r"^strb")
        assert byte_stores == 0, f"{name}: {byte_stores} byte stores remain"
    byte_loads = _count_mnem_regex(fn, r"^ldrb")
    assert byte_loads <= 6, f"kernel eqlBytes: word assembly left {byte_loads} byte loads"


def test_wyhash_readint_copy8():
    """A by-value align(1) 8-byte copy read back byte-wise becomes one wide load.

    The Zig C backend spells mem.readInt(u64, .little) as `t29 = (*t27)` on a
    u8[8] local plus a constant-trip countdown assembling the word
    (streaming Wyhash.update, docs/bugs/kernel-streaming-wyhash-byte-assembly.md).
    The copy tiled at byte width hit the four-chunk ceiling and left an opaque
    __aeabi_memmove call plus a stack round-trip per input word inside the
    block loop; the fix lets byte tiles run to eight chunks so the pieces
    store-forward and load_combine fuses the readback into a wide load of the
    source.  Shape lock: no copy helper call, no byte round-trip.
    """
    funcs = _disassemble(_compile("bug_wyhash_readint_copy8", src_dir=ASM_DIR.parent))

    fn = funcs["read_u64_le"]
    calls = _count_mnem_regex(fn, r"^bl")
    assert calls == 0, f"read_u64_le: 8-byte copy became {calls} helper call(s)"
    byte_ops = _count_mnem_regex(fn, r"^(strb|ldrb)")
    assert byte_ops == 0, f"read_u64_le: {byte_ops} byte round-trip ops remain"
    assert _count_mnem_regex(fn, r"^(ldrd|strd|ldm|ldmia)(\.w)?\s") == 0, \
        f"read_u64_le: paired access to an align(1) source: {fn}"


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


def test_auto_inline_accepts_shared_backward_return_tail():
    obj = _compile("bug_inline_shared_return", src_dir=ASM_DIR.parent)
    funcs = _disassemble(obj)
    assert "select_word" in funcs and "select_word_again" in funcs
    relocations = subprocess.check_output([OBJDUMP, "-r", str(obj)], text=True)
    assert not re.search(r"\bR_ARM_\S+\s+word_at\b", relocations), relocations


def test_inlined_bool_predicate_branches_like_the_written_test():
    """`if (pred(a, b))` on a static inline must not materialize the 0/1.

    The inlined callee returns a value, so the short-circuit arrives as a
    diamond feeding a TEST_ZERO.  ssa:bool_diamond_branch sends the constant
    arm's edge straight to the side of that test its constant picks, which
    kills the arm and lets setif fusion reach the survivor.  The check is a
    comparison against the hand-written form rather than an absolute count, so
    it keeps its teeth if the branch lowering itself changes.
    """
    obj = _compile("inline_bool_predicate")
    funcs = _disassemble(obj)

    for inlined, written in (("inlined_and", "written_and"), ("inlined_or", "written_or")):
        fn = funcs[inlined]
        ref = funcs[written]
        assert _count_mnem(fn, "ite") == 0, f"{inlined}: boolean still materialized"
        for mnem in ("cmp", "orrs", "movs", "b", "bne", "beq"):
            assert _count_mnem(fn, mnem) <= _count_mnem(ref, mnem), (
                f"{inlined}: more {mnem!r} than the hand-written {written}"
            )
        # Not a length comparison: the inlined form still carries one extra
        # register copy from parameter marshalling, which is a different gap.
        # What must match is the branch skeleton.
        branches = lambda insns: sum(
            1 for m, _ in insns if m.split(".")[0] in ("b", "bne", "beq", "cbz", "cbnz")
        )
        assert branches(fn) == branches(ref), (
            f"{inlined}: branch count {branches(fn)} != hand-written {branches(ref)}"
        )


def test_pointer_chase_keeps_every_dereference():
    """`***ppp` must emit three loads.

    The frame-slot reload cache records that a load leaves its destination
    holding the memory it read.  For `ldr r0,[r0]` that is false -- the
    register is the loaded VALUE afterwards, not the address -- and recording
    it elides the next link of the chain.
    """
    obj = _compile("spill_reload_cache")
    fn = _disassemble(obj)["chase"]
    loads = [a for m, a in fn if m.split(".")[0] == "ldr"]
    assert len(loads) >= 3, f"pointer chase lost a dereference: {fn}"


def test_frame_slot_reloads_collapse_across_ir_ops():
    """A spilled value materialized twice in a row is loaded once.

    Every frame slot `reload_pair` reads is read again by the next IR op with
    a conditional branch in between, so this fails both if the cache is reset
    at every IR boundary and if a plain branch invalidates it.
    """
    obj = _compile("spill_reload_cache")
    fn = _disassemble(obj)["reload_pair"]

    seen = set()
    repeats = 0
    for mnem, args in fn:
        base = mnem.split(".")[0]
        m = re.match(r"^(r\d+|ip|lr|fp|sl), \[(sp|r7|fp)(?:, #(-?\d+))?\]$", args)
        if base == "ldr" and m:
            key = (m.group(1), m.group(2), m.group(3) or "0")
            if key in seen:
                repeats += 1
            seen.add(key)
        elif base in ("bl", "blx") or base.startswith("push") or base.startswith("pop"):
            seen.clear()   # a call clobbers registers and memory alike
    assert repeats == 0, (
        f"{repeats} frame slot(s) reloaded into a register that already held them: {fn}"
    )


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


# -----------------------------------------------------------------------------
# Volatile accesses survive CSE / DSE / store-forwarding
# -----------------------------------------------------------------------------
def _mem_counts(func_insns):
    """(loads, stores) of real memory accesses, excluding the literal pool.

    `ldr rX, [pc, #N]` materializes a constant (a global's address, an MMIO
    address); it is not an access to the object under test.
    """
    loads = stores = 0
    for mnem, ops in func_insns:
        if "[pc," in ops or ", [pc" in ops:
            continue
        if mnem.startswith("ldr"):
            loads += 1
        elif mnem.startswith("str"):
            stores += 1
    return loads, stores


@pytest.mark.parametrize("opt", ["-O0", "-O1", "-O2"])
def test_volatile_accesses_all_survive(opt):
    """Every volatile read and write must reach the machine code.

    The case file names each function `v<N>_<loads>_<stores>` with the minimum
    number of real ldr/str it must contain, and `c<N>_...` for the same shape
    without volatile — the controls prove the optimization being blocked for
    the volatile cases still fires when it is allowed to.

    This locks in a sweep that found seven distinct volatile bugs at once:
    LOAD_INDEXED CSE (`p[1] + p[1]`), the sym-keyed global CSE missing
    `volatile int a[4]` and volatile struct members, dead-store elimination of
    volatile global and local writes, store-to-load forwarding into a volatile
    read, whole-body elision of a function whose only effect is a volatile
    write, the discarded read (`*p;`, `(void)REG;`), and — the worst — every
    `*(volatile T *)0x40000000` MMIO read compiling to the ADDRESS instead of a
    load.
    """
    obj = _compile("volatile_access", extra_cflags=[opt])
    funcs = _disassemble(obj)

    cases = [(n, f) for n, f in funcs.items() if re.match(r"^[vc]\d+_\d+_\d+$", n)]
    assert len(cases) >= 40, f"case file did not compile fully: {sorted(funcs)}"

    failures = []
    for name, insns in sorted(cases):
        want_loads, want_stores = (int(x) for x in name.rsplit("_", 2)[1:])
        loads, stores = _mem_counts(insns)
        if loads < want_loads or stores < want_stores:
            failures.append(
                f"{name}: want >={want_loads} ldr / >={want_stores} str, got {loads}/{stores}"
            )
    assert not failures, f"volatile accesses dropped at {opt}:\n  " + "\n  ".join(failures)

@pytest.mark.parametrize("opt", ["-O0", "-O1", "-O2"])
def test_volatile_access_not_duplicated(opt):
    """A volatile access must happen EXACTLY as often as the source says.

    The sweep above checks minimums, which cannot see an access emitted twice.
    The `x` cases are compiled as written and must hold their name's count
    exactly.  The `r` cases are self-recursive, and at -O1/-O2 one level of the
    recursion is expanded into the body, so each holds two copies of the count
    while each surviving call covers two levels -- see asm/volatile_once.c.
    That multiplier is asserted, not tolerated: a third copy would mean the
    expansion ran deeper than intended, and a single one that it stopped
    happening.

    -O0 is the only arm with no expansion.  (_compile passes -O2 ahead of
    `opt`, and the driver's -O2 feature flags are sticky, so the -O1 arm is
    really -O2's inliner at -O1's optimize level.)
    """
    obj = _compile("volatile_once", extra_cflags=[opt])
    funcs = _disassemble(obj)

    cases = [(n, f) for n, f in funcs.items() if re.match(r"^[xr]\d+_\d+_\d+$", n)]
    assert len(cases) >= 9, f"case file did not compile fully: {sorted(funcs)}"

    copies = 1 if opt == "-O0" else 2
    failures = []
    for name, insns in sorted(cases):
        mult = copies if name[0] == "r" else 1
        want_loads, want_stores = (int(x) * mult for x in name.rsplit("_", 2)[1:])
        loads, stores = _mem_counts(insns)
        if loads != want_loads or stores != want_stores:
            failures.append(f"{name}: want exactly {want_loads} ldr / {want_stores} str, got {loads}/{stores}")
    assert not failures, f"wrong number of volatile accesses at {opt}:\n  " + "\n  ".join(failures)


# One QEMU-runnable program (tests/ir_tests/bug_ra_reload_elim_volatile_frame_load.c)
# that is also the compile case here: nothing a run can do writes a frame slot
# behind the compiler's back, so only the instruction counts can tell a kept
# volatile read from a deleted one.  (function) -> (min ldr, min str).
RELOAD_ELIM_VOLATILE_WANT = {
    "vol_array_elem": (1, 1),
    "vol_struct_obj": (1, 1),
    "vol_struct_member": (1, 1),
    "vol_array_elem64": (1, 1),
    "vol_double": (1, 1),
    "plain_store_volatile_load": (1, 1),
    "vol_read_twice": (2, 1),
}


@pytest.mark.parametrize("opt", ["-O1", "-O2", "-Os"])
def test_bug_ra_reload_elim_volatile_frame_load(opt):
    """ra:reload_elim must keep the volatile load of a frame slot it just stored.

    `volatile int v[2]; v[1] = x; return v[1];` once compiled to a lone `str`:
    the pass dropped a load into the register the slot was stored from without
    asking whether the access was volatile.
    """
    obj = _compile("bug_ra_reload_elim_volatile_frame_load", extra_cflags=[opt],
                   src_dir=Path(__file__).parent)
    funcs = _disassemble(obj)
    failures = []
    for name, (want_loads, want_stores) in RELOAD_ELIM_VOLATILE_WANT.items():
        assert name in funcs, f"{name} missing from the object at {opt}: {sorted(funcs)}"
        loads, stores = _mem_counts(funcs[name])
        if loads < want_loads or stores < want_stores:
            failures.append(f"{name}: want >={want_loads} ldr / >={want_stores} str, got {loads}/{stores}")
    assert not failures, f"volatile frame load dropped at {opt}:\n  " + "\n  ".join(failures)


def test_nested_inline_bool_return():
    """A bool-returning helper inlines inside an inlined body.

    The nested-inline gate whitelisted void/struct/pointer and integer
    returns up to VT_LLONG; VT_BOOL (11) fell outside it, so zig.h's
    zig_addo_u32 & co. stayed a real `bl` in every inlined Zig CBE wrapper.
    """
    obj = _compile("nested_inline_bool_return")
    fn = _disassemble(obj)["sum"]
    assert _count_mnem_regex(fn, r"^bl\b") == 0, f"add_ovf not inlined into sum: {fn}"


def test_bitfield_offset_past_256mb():
    """struct_layout computed c * 8 + bit_pos in int: a bitfield after a 0x32100000-byte
    array got a wrapped (0xf2100000) access offset instead of 0x32100004."""
    funcs = _disassemble(_compile("bitfield_past_256mb"))
    insns = funcs["gety"]
    assert _count_mnem_regex(insns, r"\.word\s+0x32100004") == 1, insns


@pytest.mark.parametrize("fn", ["clr_word", "clr_bf", "clr_bf2"])
def test_rmw_byte_clear_keeps_volatile_word_access(fn):
    """A volatile word read-modify-write must not become a bare byte store."""
    funcs = _disassemble(_compile("rmw_byte_clear_volatile"))
    insns = funcs[fn]
    assert _count_mnem_regex(insns, r"^strb") == 0, f"{fn}: byte store: {insns}"
    assert _count_mnem_regex(insns, r"^ldr(?!b)") >= 1, f"{fn}: word read dropped: {insns}"
    assert _count_mnem_regex(insns, r"^str\b") >= 1, f"{fn}: word write dropped: {insns}"


TAIL_TARGET_ARG_FUNCS = ["dispatch_slot1", "dispatch_slot0", "dispatch_three"] + [f"perm{i}" for i in range(1, 9)]


@pytest.mark.parametrize("opt", ["-O1", "-O2", "-Os"])
def test_bug_ra_tail_call_target_in_arg_reg(opt):
    """An indirect call whose target is also a register argument branches
    through that argument register.

    `return v(n, v);` kept v in a callee-saved register (push/pop around a tail
    call that needs no frame) and moved it to ip to branch, where gcc emits
    `ldr r1, [r0]; bx r1`.
    """
    obj = _compile("bug_ra_tail_call_target_in_arg_reg", extra_cflags=[opt],
                   src_dir=Path(__file__).parent)
    funcs = _disassemble(obj)
    failures = []
    for name in TAIL_TARGET_ARG_FUNCS:
        insns = funcs[name]
        if _count_mnem_regex(insns, r"^(push|pop|stmdb|ldmia|ldr\.w\s+lr)"):
            failures.append(f"{name}: saves a register: {insns}")
        if not re.fullmatch(r"r[0-3]", insns[-1][1].strip()) or insns[-1][0] != "bx":
            failures.append(f"{name}: does not branch through an argument register: {insns}")
    for name in ["dispatch_slot1", "dispatch_slot0"]:
        if len(funcs[name]) != 2:
            failures.append(f"{name}: want `ldr; bx`, got {funcs[name]}")
    nontail = funcs["dispatch_nontail"]
    if _count_mnem_regex(nontail, r"^blx\s+r[0-3]$") != 1 or _count_mnem_regex(nontail, r"^mov\s+(lr|ip)"):
        failures.append(f"dispatch_nontail: target moved out of its argument register: {nontail}")
    assert not failures, f"at {opt}:\n  " + "\n  ".join(failures)


# -----------------------------------------------------------------------------
# Zig readInt: constant-trip mid-exit byte loop -> unrolled -> one LDR/LDRH
# (ssa:loop_unroll_midexit + ssa:load_combine)
# -----------------------------------------------------------------------------
def test_readint64_load_combine():
    funcs = _disassemble(_compile("readint64_combine"))
    for name in ("le64", "le64_off", "zig_rd8"):
        insns = funcs[name]
        assert _count_mnem_regex(insns, r"^ldr(?:\.w)?\s") == 2, f"{name}: {insns}"
        assert _count_mnem_regex(insns, r"^(ldrb|ldrsb|ldrh|ldrd|ldm|bl|b\.|cb)") == 0, f"{name}: {insns}"
    assert _count_mnem_regex(funcs["vol_le64"], r"^ldrb(?:\.w)?\s") == 8


def test_readint_unroll_and_load_combine():
    obj = _compile("readint_combine")
    funcs = _disassemble(obj)

    def cnt(fn, *mnems):
        return sum(_count_mnem(fn, m) for m in mnems)

    def word(fn):
        return cnt(fn, "ldr", "ldr.w")

    def half(fn):
        return cnt(fn, "ldrh", "ldrh.w")

    def byte(fn):
        return cnt(fn, "ldrb", "ldrb.w", "ldrsb", "ldrsb.w")

    def branches(fn):
        return _count_mnem_regex(fn, r"^(b|b\.n|b\.w|b[a-z]{2}|b[a-z]{2}\.w|b[a-z]{2}\.n|cbz|cbnz)\s")

    # The Zig shape: no loop, no byte loads, exactly one word / halfword load.
    rd4 = funcs["zig_rd4"]
    assert word(rd4) == 1 and byte(rd4) == 0, f"zig_rd4: expected one ldr and no byte loads, got {rd4}"
    assert branches(rd4) == 0, f"zig_rd4: loop not unrolled: {rd4}"
    rd2 = funcs["zig_rd2"]
    assert half(rd2) == 1 and byte(rd2) == 0 and branches(rd2) == 0, f"zig_rd2: {rd2}"
    # Through the inlined narrow-parameter helper, two reads: two loads, no loop.
    x2 = funcs["zig_rd4x2_cast"]
    assert word(x2) == 2 and byte(x2) == 0 and branches(x2) == 0, f"zig_rd4x2_cast: {x2}"

    # Plain idioms.
    assert word(funcs["le32"]) == 1 and byte(funcs["le32"]) == 0, f"le32: {funcs['le32']}"
    assert half(funcs["le16"]) == 1 and byte(funcs["le16"]) == 0, f"le16: {funcs['le16']}"

    # A mid-exit loop with no memory: unrolled, no branch.
    assert branches(funcs["mid_sum"]) == 0, f"mid_sum: loop not unrolled: {funcs['mid_sum']}"

    # Declined shapes keep their byte loads (and never become a word load).
    # (le24's low two bytes are a legitimate ldrh; the third byte stays a byte load.)
    l24 = funcs["le24"]
    assert word(l24) == 0 and half(l24) + byte(l24) >= 2, f"le24: {l24}"
    for name, nbytes in (("be32", 4), ("vol_le32", 4), ("le16_signed", 2)):
        fn = funcs[name]
        assert byte(fn) == nbytes, f"{name}: expected {nbytes} byte loads, got {fn}"
        assert word(fn) == 0 and half(fn) == 0, f"{name}: must not become a wide load: {fn}"
    lw = funcs["zig_load_words"]
    assert word(lw) == 1 and byte(lw) == 0, f"zig_load_words: inner readInt loop not one ldr: {lw}"
    adj = funcs["adj_words"]
    assert word(adj) == 2 and byte(adj) == 0, f"adj_words: expected two ldr, got {adj}"
    assert cnt(adj, "ldrd", "ldm", "ldmia", "ldmia.w", "ldm.w") == 0, f"adj_words: unaligned pair fused: {adj}"
    sb = funcs["store_between"]
    assert byte(sb) >= 3 and word(sb) == 0, f"store_between: byte loads fused across a store: {sb}"
    for name in ("le16_off16", "le16_off6"):
        fn = funcs[name]
        assert word(fn) == 0, f"{name}: 2-byte read widened to a word load: {fn}"
        assert half(fn) + byte(fn) >= 1, f"{name}: {fn}"


# -----------------------------------------------------------------------------
# ra:struct_arg_split: by-value struct arguments built in a frame slot
# -----------------------------------------------------------------------------
STRUCT_ARG_HARD = ["-fno-pic", "-mfloat-abi=hard", "-mfpu=fpv5-sp-d16"]
STRUCT_ARG_TAIL_FUNCS = ["call_tuple", "call_sl", "call_w1", "call_w3", "call_after"]
STRUCT_ARG_STACK_FUNCS = ["call_w4", "call_r3"]  # a word of the struct goes on the stack


def _compile_struct_arg(opt, disable=None):
    """struct_arg_split.c under hard float; `disable` names a TCC_DISABLE_PASS knob."""
    import os

    tag = hashlib.sha1(f"{opt}|{disable}".encode()).hexdigest()[:8]
    obj = BUILD_DIR / f"struct_arg_split.{tag}.o"
    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    cmd = [str(TCC), opt, "-nostdlib", "-fvisibility=hidden", "-mcpu=cortex-m33", "-mthumb",
           "-ffunction-sections", "-c", *STRUCT_ARG_HARD, str(ASM_DIR / "struct_arg_split.c"), "-o", str(obj)]
    env = dict(os.environ)
    if disable:
        env["TCC_DISABLE_PASS"] = disable
    result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, errors="replace", env=env)
    assert result.returncode == 0, f"compile failed: {cmd}\n{result.stderr}"
    return obj


@pytest.mark.parametrize("opt", ["-O1", "-O2", "-Os"])
def test_struct_arg_split_words_reach_the_call(opt):
    """Struct words stored into a slot right before the call go to the argument
    registers directly: no slot (no stores, no `sub sp`, no ldm), and a call
    that needs no stack argument is a plain tail call."""
    funcs = _disassemble(_compile_struct_arg(opt))
    failures = []
    for name in STRUCT_ARG_TAIL_FUNCS:
        insns = funcs[name]
        if [m for m, _ in insns] != ["b.w"]:
            failures.append(f"{name}: want a lone tail branch, got {insns}")
    for name in STRUCT_ARG_STACK_FUNCS:
        insns = funcs[name]
        if _count_mnem_regex(insns, r"^(strd|ldm|stm)"):
            failures.append(f"{name}: slot traffic left: {insns}")
        if _count_mnem_regex(insns, r"^add\s+r\d+, sp"):
            failures.append(f"{name}: struct still read through a slot address: {insns}")
        if _count_mnem_regex(insns, r"^sub\s+sp, #(1[2-9]|[2-9]\d)"):
            failures.append(f"{name}: frame larger than the outgoing word: {insns}")
    assert not failures, f"at {opt}:\n  " + "\n  ".join(failures)


def test_struct_arg_split_knob_restores_slot_shape():
    """The same source with the pass off keeps the slot: proves the shape test
    above is sensitive to the pass (and that TCC_DISABLE_PASS reaches it)."""
    funcs = _disassemble(_compile_struct_arg("-O2", disable="ra:struct_arg_split"))
    for name in ["call_tuple", "call_sl", "call_w3", "call_after"]:
        insns = funcs[name]
        assert _count_mnem_regex(insns, r"^(strd|str)\s") >= 1, f"{name}: no slot store with the pass off: {insns}"
        assert _count_mnem_regex(insns, r"^sub\s+sp"), f"{name}: no frame with the pass off: {insns}"


# -----------------------------------------------------------------------------
# Overwritten stores through an incoming pointer (error-return zero fill)
# -----------------------------------------------------------------------------
def test_sret_zero_fill_is_dead():
    """Count bytes written so alignment-safe split stores retain the DSE lock."""
    funcs = _disassemble(_compile("errtail_sret_zero_fill"))
    fn = funcs["first_error"]
    widths = {"strd": 8, "str": 4, "strh": 2, "strb": 1}
    written = sum(widths.get(mnem.removesuffix(".w"), 0) for mnem, _ in fn)
    assert not _count_mnem_regex(fn, r"^(stm|stmia)(\.w)?\s"), fn
    assert written == 42, f"first_error: {written} bytes written; expected 24 on error and 18 on success"


def _saved_regs(insns):
    """Registers named by every mid-function push/stmdb of r0-r3/ip/lr (the
    prologue push is the first instruction)."""
    out = []
    for i, (mnem, ops) in enumerate(insns):
        if i and mnem in ("push", "stmdb") and re.search(r"\br[0-3]\b|\bip\b", ops):
            out.append(set(re.findall(r"\b(?:r[0-3]|ip|lr)\b", ops)))
    return out


@pytest.mark.parametrize("opt", ["-O1", "-O2", "-Os"])
def test_struct_arg_copy_saves_only_live_registers(opt):
    """A big by-value stack struct is copied through r0-r3/ip/lr; it saves
    around the copy only what the rest of the call setup still reads (it used
    to push all six every time)."""
    funcs = _disassemble(_compile("struct_arg_copy_regs", extra_cflags=[opt]))
    fresh = _saved_regs(funcs["fresh_local"])
    assert fresh == [], f"fresh_local saves registers around the copy: {funcs['fresh_local']}"
    for name in ("fresh_local", "pass_through", "split_src"):
        insns = funcs[name]
        for k in range(len(insns) - 1):
            assert not (insns[k][0] == "movs" and re.fullmatch(r"r[0-7], sp", insns[k + 1][1]) and insns[k + 1][0] == "add"), \
                f"{name}: `movs rN,#off; add rN,sp` instead of `add rN, sp, #off`: {insns}"
    ident = _saved_regs(funcs["pass_through"])
    assert ident and {"r0", "r1", "r2", "r3"} <= ident[0], \
        f"pass_through must keep its identity arguments: {funcs['pass_through']}"
    split = _saved_regs(funcs["split_src"])
    assert all(not (r & {"ip", "lr"}) for r in split) and all(len(r) <= 1 for r in split), \
        f"split_src saves more than its live register: {funcs['split_src']}"
    assert not (set().union(*split, set()) & {"r1", "r2", "r3"}), f"split_src: {funcs['split_src']}"
def _backward_unconditional_branches(obj):
    """{function: [unconditional `b` lines whose target is not after them]} for
    an object built with -ffunction-sections (each function at offset 0)."""
    out = subprocess.run([OBJDUMP, "-d", "--no-show-raw-insn", str(obj)], stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE, text=True, errors="replace").stdout
    cur = None
    back = {}
    for line in out.splitlines():
        h = re.match(r"^\s*[0-9a-f]+\s+<([^>]+)>:$", line)
        if h:
            cur = h.group(1)
            back[cur] = []
            continue
        m = re.match(r"^\s*([0-9a-f]+):\s+b(?:\.[nw])?\s+([0-9a-f]+)\s", line)
        if m and cur is not None and int(m.group(2), 16) <= int(m.group(1), 16):
            back[cur].append(line.strip())
    return back


HEADER_DUP_FUNCS = ["hd_move_bwd", "hd_find", "hd_fill", "hd_outer"]


@pytest.mark.parametrize("opt", ["-O1", "-O2", "-Os"])
def test_loop_header_dup_no_unconditional_back_edge(opt):
    """Loops whose exit test follows header instructions (`n--`, a bound read
    from memory), carry a call, or hold an inner loop are bottom-tested: no
    unconditional branch jumps backwards."""
    back = _backward_unconditional_branches(_compile("loop_header_dup", extra_cflags=[opt]))
    failures = [f"{name}: {back.get(name)}" for name in HEADER_DUP_FUNCS if back.get(name) is None or back.get(name)]
    assert not failures, f"unconditional back-edges remain at {opt}:\n  " + "\n  ".join(failures)


@pytest.mark.parametrize("opt", ["-O1", "-O2", "-Os"])
def test_post_ra_backward_jumpif_threading(opt):
    """`continue` arms reached the loop head through a `b head` trampoline;
    post-regalloc threading (ra:jt_backward) aims the conditional branches at
    the head and inverts the last one over the trampoline."""
    back = _backward_unconditional_branches(_compile("loop_backedge_thread", extra_cflags=[opt]))
    assert "bt_ident" in back, back
    assert not back["bt_ident"], f"bt_ident at {opt}: {back['bt_ident']}"


def _sp_loads_in(opt, name, disable=None):
    """SP-relative loads in `name` of asm/ra_caller_save.c; `disable` names a TCC_DISABLE_PASS knob."""
    import os

    tag = hashlib.sha1(f"{opt}|{disable}".encode()).hexdigest()[:8]
    obj = BUILD_DIR / f"ra_caller_save.{tag}.o"
    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    cmd = [str(TCC), opt, "-nostdlib", "-fvisibility=hidden", "-mcpu=cortex-m33", "-mthumb", "-mfloat-abi=soft",
           "-ffunction-sections", "-fno-pic", "-mno-sb-relative-got",
           "-c", str(ASM_DIR / "ra_caller_save.c"), "-o", str(obj)]
    env = dict(os.environ)
    env.pop("TCC_DISABLE_PASS", None)
    if disable:
        env["TCC_DISABLE_PASS"] = disable
    result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, errors="replace", env=env)
    assert result.returncode == 0, f"compile failed: {cmd}\n{result.stderr}"
    insns = _disassemble(obj)[name]
    return [i for i in insns if i[0].startswith("ldr") and re.search(r"\[sp\b", i[1] or "")], insns


@pytest.mark.parametrize("opt", ["-O1", "-O2", "-Os"])
def test_ra_caller_save_keeps_hot_index_out_of_the_frame(opt):
    """With every callee-saved register taken until the loop ends, the hot
    loop index crosses a call only some iterations make: ra:caller_save keeps
    it in a caller-saved register stored and reloaded around that call, so the
    function reloads fewer values from the stack than with the pass off.

    Both arms disable ra:loop_split: that pass gives the eight invariants
    entry-copy temps of their own, which changes how many values spill here
    independently of caller_save (with it on, this fixture's static SP loads
    go 6 -> 10) and would mask the comparison this test characterizes."""
    on, on_insns = _sp_loads_in(opt, "walk", disable="ra:loop_split")
    off, _ = _sp_loads_in(opt, "walk", disable="ra:loop_split,ra:caller_save")
    assert len(on) < len(off), f"{opt}: {len(on)} SP loads with ra:caller_save, {len(off)} without:\n{on_insns}"


def _call_targets(opt_flags, disable=None, case="inline_accessor_classes"):
    """{function: [called symbols]} for asm/inline_accessor_classes.c; `disable` names TCC_DISABLE_PASS knobs."""
    import os

    tag = hashlib.sha1(f"{case}|{opt_flags}|{disable}".encode()).hexdigest()[:8]
    obj = BUILD_DIR / f"{case}.{tag}.o"
    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    cmd = [str(TCC), *opt_flags.split(), "-nostdlib", "-fvisibility=hidden", "-mcpu=cortex-m33", "-mthumb",
           "-mfloat-abi=soft", "-ffunction-sections", "-c", str(ASM_DIR / f"{case}.c"), "-o", str(obj)]
    env = dict(os.environ)
    env.pop("TCC_DISABLE_PASS", None)
    if disable:
        env["TCC_DISABLE_PASS"] = disable
    result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, errors="replace", env=env)
    assert result.returncode == 0, f"compile failed: {cmd}\n{result.stderr}"
    out = subprocess.run([OBJDUMP, "-dr", "--no-show-raw-insn", str(obj)], stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE, text=True, errors="replace").stdout
    calls, cur = {}, None
    for line in out.splitlines():
        h = re.match(r"^[0-9a-f]+ <([^>]+)>:$", line)
        if h:
            cur = h.group(1)
            calls.setdefault(cur, [])
            continue
        r = re.search(r"R_ARM_THM_(?:CALL|JUMP24)\s+(\S+)", line)
        if r and cur:
            calls[cur].append(r.group(1))
    return calls


@pytest.mark.parametrize("opt", ["-O1", "-O2"])
def test_inline_wrapper_class_inlines_accessor_chains(opt):
    """Zig-style accessors around one call (mem.span, Symbol.name) are inlined
    (inline:wrappers): only the looping helper they wrap stays a call."""
    on = _call_targets(opt)
    for fn in ("one_name", "two_names"):
        assert set(on[fn]) <= {"count_to_zero"}, f"{opt} {fn}: {on[fn]}"
    off = _call_targets(opt, "inline:wrappers")
    assert "name_of" in off["one_name"], f"{opt}: the knob must matter: {off['one_name']}"


def test_inline_wrapper_class_off_at_os():
    """-Os keeps wrappers out of line: one copy of the call setup, not one per site."""
    calls = _call_targets("-Os")
    assert "name_of" in calls["one_name"], calls["one_name"]


@pytest.mark.parametrize("opt", ["-O2", "-Os"])
def test_inline_barrier_is_not_a_call(opt):
    """An acquire load's barrier is one DMB, not a call: the accessor using it
    (Zig's itemPtr over Entry.acquire) is a leaf and inlined (inline:barrier_leaf)."""
    on = _call_targets(opt)
    assert not on["use_item"] and not on["use_item2"], f"{opt}: {on['use_item']} {on['use_item2']}"
    if opt == "-Os":
        off = _call_targets(opt, "inline:barrier_leaf")
        assert "item_at" in off["use_item"], f"the knob must matter at -Os: {off['use_item']}"


def test_inline_trivial_leaf_at_non_constant_sites():
    """A trivial static body over the registration token limit is inlined at a
    non-constant call site (inline:trivial_leaf), not only evaluated for constants."""
    on = _call_targets("-O2")
    assert not on["use_low_bits"] and not on["use_low_bits2"], f"{on['use_low_bits']} {on['use_low_bits2']}"
    off = _call_targets("-O2", "inline:trivial_leaf")
    assert "low_bits" in off["use_low_bits"], off["use_low_bits"]


def test_inline_leaf_rescue_keeps_limit_monotonic():
    """Raising -finline-limit must not UN-inline a straight leaf: registered on
    its token length, it is kept a small_leaf over the IR-size revoke
    (inline:leaf_rescue).  math.rotr went out of line at 200: +165M on zig.c.
    The off case needs the revoke to actually bite, so asm/mix is sized for
    the post-opt economics: over 24 IR slots (which the late barrel-shift
    fusion shrinks by folding shifts into consumers) yet <= 24 real ops, so
    only leaf_rescue's small_leaf class keeps it inlined."""
    on = _call_targets("-O2 -finline-limit=200")
    assert not on["use_mix"] and not on["use_mix2"], f"{on['use_mix']} {on['use_mix2']}"
    off = _call_targets("-O2 -finline-limit=200", "inline:leaf_rescue")
    assert "mix" in off["use_mix"], off["use_mix"]


@pytest.mark.parametrize("opt", ["-O1", "-O2", "-Os"])
def test_inline_multi3_widened_product(opt):
    """zig.h's u64*u64->u128 (Wyhash's mum) through __multi3: the call is
    expanded (inline:multi3) and the widened operands' zero high halves fold
    away (arm_mla_zero_product): two UMULL and two UMAAL, no MLA, no call."""
    obj = _compile("inline_multi3", extra_cflags=[opt])
    out = subprocess.run([OBJDUMP, "-dr", "--no-show-raw-insn", str(obj)], stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE, text=True, errors="replace").stdout
    assert "__multi3" not in out, f"{opt}: still calls __multi3:\n{out}"
    insns = _disassemble(obj)["mum"]
    ops = [m for m, _ in insns]
    muls = [m for m in ops if m.startswith(("umull", "umaal", "umlal", "mul", "mla"))]
    assert not [m for m in ops if m.startswith("mla")], f"{opt}: zero cross term left as MLA: {insns}"
    assert len(muls) == 4, f"{opt}: expected 4 word multiplies, got {muls}"


@pytest.mark.parametrize("opt", ["-O1", "-O2", "-Os"])
def test_ra_branch_cost_keeps_latch_path_values_in_registers(opt, monkeypatch):
    monkeypatch.delenv("TCC_DISABLE_PASS", raising=False)
    on = _disassemble(_compile("ra_branch_cost", extra_cflags=[opt]))
    monkeypatch.setenv("TCC_DISABLE_PASS", "ra:branch_cost")
    off = _disassemble(_compile("ra_branch_cost", extra_cflags=[opt]))
    def sp_loads(insns):
        return sum(mnem.startswith("ldr") and re.search(r"\[sp\b", operands) is not None
                   for mnem, operands in insns)
    assert sp_loads(on["replay"]) < sp_loads(off["replay"])
    assert on["hot_walk"] == off["hot_walk"]


@pytest.mark.parametrize("opt", ["-O1", "-O2", "-Os"])
def test_addreg_indexed_access(opt):
    """`base + index` of two registers feeding only memory accesses becomes the
    [Rn, Rm] operand (arm_addreg_indexed): the string scan loads `[r0, r1]`
    instead of `adds; ldrb [rX]`.  A 64-bit access keeps the ADD (no LDRD [Rn, Rm])."""
    funcs = _disassemble(_compile("addreg_indexed", extra_cflags=[opt, "-mfloat-abi=hard", "-mfpu=fpv5-sp-d16"]))
    scan = funcs["find_sentinel"]
    assert any(m.startswith("ldrb") and re.search(r"\[r\d+, r\d+\]", o or "") for m, o in scan), \
        f"{opt}: find_sentinel has no [Rn, Rm] byte load: {scan}"
    assert not any(m.startswith("add") and re.fullmatch(r"r\d+, r\d+, r\d+", o or "") for m, o in scan), \
        f"{opt}: find_sentinel still forms the address with ADD: {scan}"
    assert not any(re.search(r"\[r\d+, r\d+\]", o or "") for _, o in funcs["get64"]), \
        f"{opt}: get64 must not use a register offset: {funcs['get64']}"


@pytest.mark.parametrize("opt", ["-O1", "-O2", "-Os"])
def test_indexed_zero_offset_fuse(opt):
    """The `[addr, #0]` STORE_INDEXED/LOAD_INDEXED mem_inline emits for an
    align(1) word memcpy through a pointer fuses into the shl+add address like
    a plain LOAD/STORE does: the access is `str.w/ldr.w r?, [r?, r?, lsl #2]`,
    not `lsls; adds; str/ldr [rX]`."""
    funcs = _disassemble(_compile("indexed_zero_offset_fuse", extra_cflags=[opt]))
    for name, mnem in (("put", "str"), ("get", "ldr")):
        fn = funcs[name]
        assert any(m.startswith(mnem) and re.search(r"\[r\d+, r\d+, lsl #2\]", o or "")
                   for m, o in fn), \
            f"{opt}: {name} has no [Rn, Rm, lsl #2] {mnem}: {fn}"
        assert not any(m == "lsls" for m, _ in fn), \
        f"{opt}: {name} still shifts the index separately: {fn}"


@pytest.mark.parametrize("opt", ["-O1", "-O2", "-Os"])
def test_shifted_address_shared_use(opt):
    """Fuse shared addresses into ADD while retaining single-use indexed loads."""
    funcs = _disassemble(_compile("shifted_address_shared_use", extra_cflags=[opt]))
    assert any(m.startswith("add") and re.search(r", lsl #3\b", o or "")
               for m, o in funcs["shared"]), \
        f"{opt}: shared forms the address with a separate shift: {funcs['shared']}"
    assert not any(m == "lsls" for m, _ in funcs["shared"]), \
        f"{opt}: shared still shifts the index separately: {funcs['shared']}"
    assert any(m.startswith("ldr") and re.search(r"\[r\d+, r\d+, lsl #3\]", o or "")
               for m, o in funcs["single"]), \
        f"{opt}: single has no [Rn, Rm, lsl #3] ldr: {funcs['single']}"
    assert not any(m == "lsls" for m, _ in funcs["single"]), \
        f"{opt}: single still shifts the index separately: {funcs['single']}"


@pytest.mark.parametrize("opt", ["-O1", "-O2", "-Os"])
def test_guarded_field_load_reused(opt):
    """Reuse the field tested by an early-exit branch in its dominated body."""
    fn = _disassemble(_compile("shifted_address_shared_use", extra_cflags=[opt]))["shared"]
    loads = [(m, o) for m, o in fn if m.startswith("ldr")]
    assert len(loads) == 2, f"{opt}: expected one load per field, got {loads}"


@pytest.mark.parametrize("opt", ["-O2", "-Os"])
def test_pure_call_cse_symbol_walk(opt, monkeypatch):
    """ssa:pure_call_cse: `size(p)` then `next(p) = p + size(p)` makes one call
    per record.  size() is PURE only because find_sentinel's verdict is cached
    (purity:transitive); a store between two scans keeps both."""
    monkeypatch.delenv("TCC_DISABLE_PASS", raising=False)
    on = _disassemble(_compile("pure_call_cse", extra_cflags=[opt]))
    assert _count_mnem(on["walk"], "bl") == 1, f"{opt}: walk: {on['walk']}"
    assert _count_mnem(on["store_between"], "bl") == 2, f"{opt}: store_between: {on['store_between']}"
    for knob in ("ssa:pure_call_cse", "purity:transitive"):
        monkeypatch.setenv("TCC_DISABLE_PASS", knob)
        off = _disassemble(_compile("pure_call_cse", extra_cflags=[opt, "-D_KNOB_" + knob.replace(":", "_")]))
        assert _count_mnem(off["walk"], "bl") == 2, f"{opt} {knob} off: walk: {off['walk']}"


def test_machine_call_locks_inline():
    """System-instruction asm becomes machine calls and the spin loop a small
    loop leaf, so with -minline-atomics the kernel-shaped lock inlines fully."""
    obj = _compile("machine_call_locks", ("-minline-atomics",))
    funcs = _disassemble(obj)
    for fn in ("heap_add", "pages_add"):
        insns = funcs[fn]
        text = "\n".join(f"{m} {o}" for m, o in insns)
        assert _count_mnem_regex(insns, r"^bl\b") == 0, f"{fn} still calls:\n{text}"
        for mnem in ("mrs", "cpsid", "ldaex", "strex", "wfe", "sev", "msr"):
            assert _count_mnem_regex(insns, rf"^{mnem}") >= 1, f"{fn} lacks {mnem}:\n{text}"
        # A failed compare-exchange closes its reservation before the spin
        # lock's WFE: left open, the WFE does not sleep and the waiter spins.
        mn = [m for m, _ in insns]
        assert "clrex" in mn and mn.index("clrex") < mn.index("wfe"), f"{fn}: no CLREX before WFE:\n{text}"
        # The unlock's release store is one STL, not DMB; STR.
        assert "stl" in mn and "dmb" not in mn, f"{fn}: release store not an STL:\n{text}"
        # The machine calls clobber nothing: no call setup, LR never saved.
        assert not any(m in ("push", "stmdb", "stmdb.w") and "lr" in o for m, o in insns), text
    # The two the lowering must leave as asm statements still assemble.
    assert _count_mnem(funcs["clobbers"], "cpsid") == 1
    assert _count_mnem(funcs["immediate"], "bkpt") == 1


def test_machine_call_off_at_o0_and_without_flag():
    """Without -minline-atomics the compare-exchange stays a runtime call."""
    obj = _compile("machine_call_locks")
    funcs = _disassemble(obj)
    calls = [o for m, o in funcs["heap_add"] if m == "bl"]
    assert any("__atomic_compare_exchange_4" in o for o in calls) or any(
        "__atomic_compare_exchange_4" in o for f in funcs.values() for m, o in f if m == "bl"
    )


def test_zig_optional_cas_branches_directly():
    """A compare-exchange's success stored in a Zig optional, copied and
    tested reaches codegen as one compare and branch: no ITE/MOV/MOV."""
    obj = _compile("zig_optional_cas", ("-minline-atomics",))
    insns = _disassemble(obj)["zig_lock"]
    text = "\n".join(f"{m} {o}" for m, o in insns)
    assert _count_mnem_regex(insns, r"^(ite|moveq|movne)") == 0, text
    assert _count_mnem_regex(insns, r"^(str|ldr)\S*\s.*\[sp") == 0, text
    assert _count_mnem(insns, "wfe") == 1 and _count_mnem_regex(insns, r"^bl\b") == 0, text


def test_atomic_ptr_arg_flows_direct():
    """The generic atomic forms' pointer operands -- a compare-exchange's
    desired, an exchange's or a store's value -- with the value in a
    parameter load straight from that register: no LEA of the local, no
    round trip through its slot (parse_atomic defers the & marking when the
    expansion is inline)."""
    obj = _compile("atomic_ptr_arg_direct", ("-minline-atomics",))
    funcs = _disassemble(obj)

    cas = funcs["cas_desired_direct"]
    text = "\n".join(f"{m} {o}" for m, o in cas)
    assert _count_mnem_regex(cas, r"^(str|ldr)\S*\s.*\[sp") == 0, text
    assert _count_mnem_regex(cas, r"^add\S*\s+\w+,\s*sp\b") == 0, text
    assert _count_mnem_regex(cas, r"^strex\S*\s+\w+,\s+r1,") == 1, text

    store = funcs["store_value_direct"]
    text = "\n".join(f"{m} {o}" for m, o in store)
    assert _count_mnem_regex(store, r"^(str|ldr)\S*\s.*\[sp") == 0, text
    assert _count_mnem(store, "stl") == 1, text

    # The exchange's value still feeds STREX from its parameter register;
    # the only stack traffic left is the result pointer's own slot.
    xchg = funcs["xchg_value_direct"]
    text = "\n".join(f"{m} {o}" for m, o in xchg)
    assert _count_mnem_regex(xchg, r"^strex\S*\s+\w+,\s+r1,") == 1, text
    assert _count_mnem_regex(xchg, r"^(str|ldr)\S*\s.*\[sp") <= 2, text


def test_spare_scratch_no_loop_spill():
    """A scratch with every allocatable register taken comes from an unused
    callee-saved register, not a save/restore of a live one in the loop."""
    obj = _compile("spare_scratch_loop")
    insns = _disassemble(obj)["walk"]
    text = "\n".join(f"{m} {o}" for m, o in insns)
    sp_traffic = [m for m, o in insns if m.startswith(("str", "ldr")) and "[sp" in o]
    # Only the two stack-passed parameters are read from the stack.
    assert len(sp_traffic) <= 2, text


def test_bump_sink_post_indexed_walks():
    """ra:bump_sink + ra:store_postinc: pointer walks written `*p++ = v` use
    post-indexed accesses and never copy the pointer inside the loop."""
    obj = _compile("bump_sink_walk")
    funcs = _disassemble(obj)
    # (name, post-indexed accesses, the argument registers holding pointers)
    for name, accesses, ptrs in (("fill", ("strb",), ("r0",)), ("copy", ("ldrb", "strb"), ("r0", "r1")),
                                 ("fill_words", ("str",), ("r0",))):
        insns = funcs[name]
        text = "\n".join(f"{m} {o}" for m, o in insns)
        for acc in accesses:
            assert any(m.split(".")[0] == acc and "], #" in o for m, o in insns), f"{name}: no post-indexed {acc}\n{text}"
        copies = [o for m, o in insns if m.startswith("mov") and o.split(",")[-1].strip() in ptrs]
        assert not copies, f"{name}: pointer copied\n{text}"


def test_bump_sink_off_keeps_copy():
    """The same walk with ra:bump_sink disabled keeps its pointer copy: the
    test above measures the pass, not some other change."""
    import os

    tag = "nosink"
    obj = BUILD_DIR / f"bump_sink_walk.{tag}.o"
    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    cmd = [str(TCC), "-O2", "-nostdlib", "-fvisibility=hidden", "-mcpu=cortex-m33", "-mthumb", "-mfloat-abi=soft",
           "-ffunction-sections", "-c", str(ASM_DIR / "bump_sink_walk.c"), "-o", str(obj)]
    env = dict(os.environ)
    env["TCC_DISABLE_PASS"] = "ra:bump_sink"
    r = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    assert r.returncode == 0, r.stderr
    insns = _disassemble(obj)["fill"]
    assert not any(m.startswith("strb") and "], #" in o for m, o in insns), insns


def test_licm_entry_header_hoists_narrow_param():
    """A loop opening the function (header = entry block) still gets a
    preheader -- the function entry -- and the u8 parameter's extension,
    a LOAD off its register, leaves the loop instead of running per byte."""
    obj = _compile("bump_sink_walk")
    insns = _disassemble(obj)["fill"]
    text = "\n".join(f"{m} {o}" for m, o in insns)
    first_branch = next(i for i, (m, o) in enumerate(insns) if m.startswith(("b", "cb")) and not m.startswith("bic"))
    ext = [i for i, (m, o) in enumerate(insns) if m.startswith("uxtb")]
    assert ext and all(i < first_branch for i in ext), text


def test_inline_readonly_wrapper_removes_scan_wrapper_calls():
    calls = _call_targets("-O2", case="inline_readonly_wrapper")
    for fn in ("one_size", "two_sizes"):
        assert "record_size" not in calls[fn], calls[fn]
    off = _call_targets("-O2", "inline:readonly_wrappers", case="inline_readonly_wrapper")
    assert "record_size" in off["one_size"], off["one_size"]


def test_inline_readonly_wrapper_keeps_per_caller_cap():
    calls = _call_targets("-O2", case="inline_readonly_wrapper")
    assert calls["three_sizes"].count("record_size") == 1, calls["three_sizes"]


def test_inline_readonly_wrapper_does_not_expand_side_effecting_scan():
    calls = _call_targets("-O2", case="inline_readonly_wrapper")
    assert "observed_size" in calls["observed_one"], calls["observed_one"]
    assert calls["observed_two"].count("observed_size") == 2, calls["observed_two"]


def test_inline_readonly_wrapper_keeps_os_size_policy():
    calls = _call_targets("-Os", case="inline_readonly_wrapper")
    assert "record_size" in calls["one_size"], calls["one_size"]


@pytest.mark.parametrize("opt", ["-O1", "-O2"])
def test_inline_wrapper_budget_is_local_to_caller(opt):
    on = _call_targets(opt, case="inline_readonly_wrapper")
    for i in range(10):
        assert set(on[f"name_caller_{i}"]) <= {"scan"}, on[f"name_caller_{i}"]
    off = _call_targets(opt, "inline:wrapper_local_budget", case="inline_readonly_wrapper")
    assert "name_of" in off["name_caller_9"], off["name_caller_9"]


def test_inline_wrapper_local_budget_retains_growth_cap():
    on = _call_targets("-O2", case="inline_readonly_wrapper")
    assert on["three_names"].count("name_of") == 1, on["three_names"]


def test_inline_readonly_wrapper_preserves_repeated_scan_reuse():
    on = _call_targets("-O2", case="inline_readonly_wrapper")
    assert on["walk_records"] == ["scan"], on["walk_records"]


@pytest.mark.parametrize("opt", ["-O1", "-O2", "-O2 -fno-licm"])
def test_inline_readonly_loop_retains_call_boundary(opt):
    on = _call_targets(opt, case="inline_readonly_wrapper")
    assert on["name_caller_0"] == ["scan"], on["name_caller_0"]
    off = _call_targets(opt, "inline:readonly_loops", case="inline_readonly_wrapper")
    assert off["name_caller_0"] == [], off["name_caller_0"]


@pytest.mark.parametrize("opt", ["-O1", "-O2"])
def test_inline_readonly_loop_preserves_explicit_and_register_loops(opt):
    on = _call_targets(opt, case="inline_readonly_wrapper")
    for fn in ("forced_name", "sum_name"):
        assert on[fn] == [], on[fn]


def test_pure_call_forward_diamond_and_address_copies():
    on = _call_targets("-O2", case="pure_call_forward")
    assert on["clean_diamond"].count("scan_forward") == 1, on["clean_diamond"]
    off = _call_targets("-O2", "ssa:pure_call_cse:forward", case="pure_call_forward")
    assert off["clean_diamond"].count("scan_forward") == 2, off["clean_diamond"]


@pytest.mark.parametrize("fn", ["conditional_store", "conditional_call", "conditional_volatile",
                                 "changed_pointer", "skipped_first", "external_entry", "loop_store",
                                 "changed_base", "asm_barrier"])
def test_pure_call_forward_preserves_barriers_and_changed_values(fn):
    on = _call_targets("-O2", case="pure_call_forward")
    assert on[fn].count("scan_forward") == 2, on[fn]


# -----------------------------------------------------------------------------
# u64 division runtime: the AAPCS helper computes without a helper chain
# -----------------------------------------------------------------------------
@pytest.mark.parametrize("opt", ["-O0", "-O1"])
def test_bug_u64_divmod_layer_cake(opt):
    """lib/armeabi.c's __tcc_aeabi_uldivmod_helper computes the whole divide
    in its own frame: a leaf, with the two-digit step's hardware UDIVs in its
    body and the chain's statics gone.

    The chain (helper -> udivmod_u64 -> divlu -> armeabi_clz32) put three
    stack frames and an out-of-line clz around every 64-bit divide: core-sort's
    4,560 timestamp divides cost 569k instructions in the tcc kernel against
    the llvm kernel's 260k through compiler_rt's single __udivmoddi4.  The
    kernel build compiles this source at -O1 (build/kernel_tcc.zig runtime()
    flags), so the shape is checked with exactly one -O flag, as that build
    passes it — _compile's leading -O2 would re-enable the -O2 inliner at the
    -O1 arm and inline the chain on its own.
    """
    obj = BUILD_DIR / f"armeabi.{opt}.o"
    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    cmd = [str(TCC), opt, "-nostdlib", "-fvisibility=hidden", "-mcpu=cortex-m33",
           "-mthumb", "-mfloat-abi=soft", "-ffunction-sections", "-fno-pic",
           "-mno-sb-relative-got", "-c", str(ROOT / "lib" / "armeabi.c"),
           "-o", str(obj)]
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       text=True, errors="replace")
    assert r.returncode == 0, f"compile failed: {cmd}\n{r.stderr}"
    funcs = _disassemble(obj)
    fn = funcs["__tcc_aeabi_uldivmod_helper"]
    assert _count_mnem(fn, "bl") + _count_mnem(fn, "blx") == 0, \
        f"{opt}: helper still calls the divide chain: {fn}"
    assert "udivmod_u64" not in funcs and "divlu" not in funcs, \
        f"{opt}: the divide chain is still out of line: {sorted(funcs)}"
    assert _count_mnem_regex(fn, r"^udiv") >= 2, \
        f"{opt}: the two-digit step lost its hardware divides: {fn}"


# -----------------------------------------------------------------------------
# ssa:reroll cost model: a copy/fill body must not be re-rolled
# -----------------------------------------------------------------------------
def test_reroll_copy_pair_body_stays_straight_line():
    """An explicitly unrolled copy body (a load/store pair plus pointer steps)
    must stay straight-line: the roll pays ADD+CMP+JUMPIF per element and
    gives up the post-indexed/folded addressing the straight-line body gets
    for free, so re-rolling a body that only moves data is a plain loss.
    A body carrying real per-element work (add_copies) is long enough to pay
    for the counter and keeps its counted inner loop."""
    funcs = _disassemble(_compile("reroll_copy_pair_body"))

    def counter_loop(fn):
        init = _count_mnem_regex(fn, r"^movs\s+r\d+, #0$")
        back = _count_mnem_regex(fn, r"^blt\.n?\s")
        return init, back

    for name, load, store in (("postinc_bytes", "ldrb", "strb"),
                              ("postinc_words", "ldr", "str"),
                              ("fill_bytes", None, "strb"),
                              ("idx_words", "ldr", "str")):
        fn = funcs[name]
        if load:
            assert _count_mnem_regex(fn, rf"^{load}(?:\.w)?\s") == 4, \
                f"{name}: copy body not kept inline: {fn}"
        assert _count_mnem_regex(fn, rf"^{store}(?:\.w)?\s") == 4, \
            f"{name}: expected 4 inline stores: {fn}"
        init, back = counter_loop(fn)
        assert init == 0 and back == 0, \
            f"{name}: re-rolled into a counted inner loop: {fn}"

    init, back = counter_loop(funcs["add_copies"])
    assert init >= 1 and back >= 1, \
        f"add_copies: a body with real work lost its roll: {funcs['add_copies']}"


def test_kernel_eql_slice_prologue():
    funcs = _disassemble(_compile("bug_kernel_eql_slice", src_dir=ASM_DIR.parent))
    fn = funcs["kernel_eql"]
    first_byte = next(i for i, (m, _) in enumerate(fn) if m.startswith("ldrb"))
    prologue = fn[:first_byte]
    assert not any(m.startswith("it") for m, _ in prologue), prologue
    assert not any(m.startswith(("ldr", "str")) and "[sp" in o for m, o in prologue), prologue


def test_metadata_byte_local_stays_in_register():
    funcs = _disassemble(_compile("906_narrow_local_probe", src_dir=ASM_DIR.parent))
    for name in ("metadata_used", "signed_byte", "signed_half"):
        fn = funcs[name]
        assert not any(m.startswith(("ldr", "str")) and "[sp" in o for m, o in fn), (name, fn)
    assert sum(m.startswith("ldrb") for m, _ in funcs["volatile_byte"]) == 2


def test_borrowed_header_initialized_in_return_buffer():
    fn = _disassemble(_compile("907_borrowed_header_nrvo", src_dir=ASM_DIR.parent))["make_header"]
    assert not any(m.startswith(("ldm", "stm")) and "sp!" not in o for m, o in fn), fn
