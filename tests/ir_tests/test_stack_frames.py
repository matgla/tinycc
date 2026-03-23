"""
Stack frame analysis for the native TCC binary (armv8m-tcc.elf).

Ensures that individual function stack frames and critical call chains
stay within budget so that the native compiler does not overflow the
limited process stack on the MCU target.

Background (2026-03-22):
  Native TCC crashed with HardFault (STKOF) on RP2350 during compilation.
  GDB showed psp=0x1104a008 with psplim=0x1104a000 (8 bytes remaining).
  Corrupted locals had sequential byte patterns (ts=0x7c7b7a79)
  confirming stack overflow.
"""

import pytest
import re
import subprocess
from pathlib import Path

CURRENT_DIR = Path(__file__).parent
TCC_ELF = CURRENT_DIR / "../../bin/armv8m-tcc.elf"

# Per-function stack frame budgets (bytes).
# Each value is the maximum allowed frame size (PUSH registers + SUB SP).
# These are set slightly above the currently measured values to allow
# minor code changes without immediately breaking, but tight enough to
# catch regressions that could cause stack overflow on target.
FRAME_BUDGETS = {
    "unary":                    2500,  # was 7312, reduced to 2328 by extracting builtins
    "unary_builtin_fp":         1500,  # extracted: signbit/isinf/copysign/isnan etc
    "unary_builtin_fp2":        1500,  # extracted: fabs/fmax/fmin/bswap/fpclassify etc
    "unary_builtin_shuffle":    1100,  # extracted: shuffle/shufflevector
    "unary_builtin_chk":        1100,  # extracted: object_size + __*_chk builtins
    "unary_builtin_alloca":      350,  # extracted: alloca/apply_args/apply/return
    "unary_builtin_overflow":    250,  # extracted: add/sub/mul_overflow
    "decl":                     1200,  # was 1040
    "block":                     800,  # was 632  — recursive (nested blocks)
    "decl_initializer_alloc":    550,
    "tcc_preprocess":            550,
    "expr_cond":                 450,  # recursive (ternary chains)
    "decl_initializer":          450,
    "next_nomacro":              350,  # called O(tokens); low frame matters
    "parse_btype":               350,
    "next":                      100,
    "tcc_compile":               100,
}

# Estimated worst-case call chain stack usage budget.
# This is the sum of frames along a compilation path that is known to
# be deep.  The RP2350 process stack is typically 64 KB, and the kernel +
# C runtime preamble consume some of that.
# A conservative budget for the *compiler call chain* alone.
MAX_CALL_CHAIN_BUDGET = 40_000  # 40 KB

# Functions in a realistic deep call chain during compilation.
DEEP_CALL_CHAIN = [
    "tcc_compile",
    "tcc_preprocess",       # preprocessing phase
    "decl",                 # top-level declaration
    "decl_initializer_alloc",
    "decl_initializer",
    "parse_btype",          # type parsing
    "unary",                # expression evaluation
    "expr_cond",            # ternary
    "unary",                # nested unary (recursive)
    "block",                # statement block
    "block",                # nested block (recursive)
    "next",                 # tokenizer
    "next_nomacro",         # raw tokenizer
]


def _get_objdump():
    """Return arm-none-eabi-objdump path or skip."""
    for name in ("arm-none-eabi-objdump",):
        result = subprocess.run(["which", name], capture_output=True)
        if result.returncode == 0:
            return name
    pytest.skip("arm-none-eabi-objdump not found")


def _parse_frame_sizes(objdump_path, elf_path):
    """Parse function stack frame sizes from disassembly.

    Returns dict mapping function name -> total frame size (PUSH + SUB SP).
    """
    result = subprocess.run(
        [objdump_path, "-d", str(elf_path)],
        capture_output=True,
        text=True,
        timeout=120,
    )
    assert result.returncode == 0, f"objdump failed: {result.stderr}"

    frames = {}
    current_func = None
    push_bytes = 0
    sub_sp = 0
    # Track whether we've seen the prologue (first few instructions)
    insn_count = 0

    for line in result.stdout.splitlines():
        # New function label
        m = re.match(r'^[0-9a-f]+ <(\w+)>:', line)
        if m:
            # Save previous function
            if current_func is not None:
                frames[current_func] = push_bytes + sub_sp
            current_func = m.group(1)
            push_bytes = 0
            sub_sp = 0
            insn_count = 0
            continue

        if current_func is None:
            continue

        insn_count += 1
        # Only look at prologue (first ~10 instructions)
        if insn_count > 15:
            continue

        # PUSH / STMDB SP! — count registers
        # stmdb sp!, {r4, r5, r6, r7, r8, sl, ip, lr}
        # push {r4, r5, r6, r7, lr}
        m_push = re.search(r'(?:stmdb\s+sp!,|push)\s*\{([^}]+)\}', line)
        if m_push:
            regs = m_push.group(1).split(',')
            push_bytes += len(regs) * 4
            continue

        # SUB SP, #imm  or  SUB SP, SP, #imm  (immediate in instruction)
        # Also matches subw (Thumb encoding without dot)
        m_sub = re.search(r'sub(?:\.w|w)?\s+sp,\s*(?:sp,\s*)?#(\d+)', line)
        if m_sub:
            sub_sp += int(m_sub.group(1))
            continue

        # SUB SP via register loaded from literal pool:
        #   ldr.w ip, [pc, #N]  @  addr <func+off>
        #   sub.w sp, sp, ip
        # We detect the pattern: sub.w sp, sp, <reg> preceded by ldr.w <reg>, [pc, #N]
        # and the literal value is in a .word at the referenced address.
        # For simplicity, check if we see "sub.w sp, sp, ip" (or r12)
        # and scan backwards for the corresponding ldr.w ip literal.
        # This is handled by a second pass below if needed.

    # Save last function
    if current_func is not None:
        frames[current_func] = push_bytes + sub_sp

    # Second pass: find indirect SUB SP via literal pool for functions
    # that have suspiciously small sub sp (like unary).
    # Look for: ldr.w ip, [pc, #N] -> sub.w sp, sp, ip -> .word VALUE
    current_func = None
    insn_count = 0
    ldr_target_addr = None
    ldr_reg = None

    for line in result.stdout.splitlines():
        m = re.match(r'^[0-9a-f]+ <(\w+)>:', line)
        if m:
            current_func = m.group(1)
            insn_count = 0
            ldr_target_addr = None
            ldr_reg = None
            continue

        if current_func is None:
            continue

        insn_count += 1
        if insn_count > 15:
            if ldr_target_addr is None:
                current_func = None
            continue

        # ldr.w ip, [pc, #972]  @ 4bd20 <unary+0x3d8>
        m_ldr = re.search(r'ldr(?:\.w)?\s+(ip|r12),\s*\[pc,\s*#\d+\]\s*@\s*([0-9a-f]+)', line)
        if m_ldr:
            ldr_reg = m_ldr.group(1)
            ldr_target_addr = int(m_ldr.group(2), 16)
            continue

        # sub.w / subw sp, sp, ip
        if ldr_target_addr and re.search(rf'sub(?:\.w|w)?\s+sp,\s*sp,\s*(?:{ldr_reg}|ip|r12)', line):
            # Now find the .word at ldr_target_addr
            break

    # Third pass: if we found an indirect sub, read the literal value
    if ldr_target_addr is not None:
        target_hex = f"{ldr_target_addr:x}:"
        for line in result.stdout.splitlines():
            if target_hex in line:
                m_word = re.search(r'\.word\s+0x([0-9a-f]+)', line)
                if m_word:
                    indirect_sub = int(m_word.group(1), 16)
                    if current_func in frames:
                        # Replace the sub_sp portion
                        frames[current_func] = frames.get(current_func, 0) + indirect_sub
                break

    return frames


def _parse_frame_sizes_simple(objdump_path, elf_path, functions):
    """Targeted frame size extraction for specific functions.

    More accurate than the generic parser: examines each function's
    prologue individually and handles indirect sub sp via literal pool.
    """
    frames = {}

    for func in functions:
        result = subprocess.run(
            [objdump_path, "-d", str(elf_path)],
            capture_output=True,
            text=True,
            timeout=120,
        )
        lines = result.stdout.splitlines()

        # Find function start
        func_start = None
        for i, line in enumerate(lines):
            if re.match(rf'^[0-9a-f]+ <{re.escape(func)}>:', line):
                func_start = i
                break

        if func_start is None:
            continue

        push_bytes = 0
        sub_sp = 0
        ldr_literals = {}  # reg -> (target_addr)

        # Scan prologue (first 15 instructions)
        for j in range(func_start + 1, min(func_start + 20, len(lines))):
            line = lines[j]

            # Stop at next function
            if re.match(r'^[0-9a-f]+ <\w+>:', line):
                break

            # PUSH / STMDB
            m_push = re.search(r'(?:stmdb\s+sp!,|push)\s*\{([^}]+)\}', line)
            if m_push:
                regs = m_push.group(1).split(',')
                push_bytes += len(regs) * 4
                continue

            # Direct sub sp, #imm  (matches sub, sub.w, and subw variants)
            m_sub = re.search(r'sub(?:\.w|w)?\s+sp,\s*(?:sp,\s*)?#(\d+)', line)
            if m_sub:
                sub_sp += int(m_sub.group(1))
                continue

            # ldr.w reg, [pc, #N]  @ addr
            m_ldr = re.search(r'ldr(?:\.w)?\s+(\w+),\s*\[pc,\s*#\d+\]\s*@\s*([0-9a-f]+)', line)
            if m_ldr:
                ldr_literals[m_ldr.group(1)] = int(m_ldr.group(2), 16)
                continue

            # sub.w / subw sp, sp, reg  (indirect)
            m_sub_reg = re.search(r'sub(?:\.w|w)?\s+sp,\s*sp,\s*(\w+)', line)
            if m_sub_reg:
                reg = m_sub_reg.group(1)
                if reg in ldr_literals:
                    # Find the literal value
                    target_addr = ldr_literals[reg]
                    target_hex = f"{target_addr:x}:"
                    for k in range(len(lines)):
                        if target_hex in lines[k]:
                            m_word = re.search(r'\.word\s+0x([0-9a-f]+)', lines[k])
                            if m_word:
                                sub_sp += int(m_word.group(1), 16)
                            break

        frames[func] = push_bytes + sub_sp

    return frames


@pytest.fixture(scope="module")
def frame_sizes():
    """Parse stack frame sizes from the native TCC binary."""
    if not TCC_ELF.exists():
        pytest.skip(f"Native TCC binary not found: {TCC_ELF}")

    objdump = _get_objdump()
    functions = list(FRAME_BUDGETS.keys())
    return _parse_frame_sizes_simple(objdump, TCC_ELF, functions)


class TestStackFrameBudgets:
    """Verify per-function stack frame sizes stay within budget."""

    @pytest.mark.parametrize("func,budget", list(FRAME_BUDGETS.items()),
                             ids=list(FRAME_BUDGETS.keys()))
    def test_frame_within_budget(self, frame_sizes, func, budget):
        if func not in frame_sizes:
            pytest.skip(f"Function {func} not found in binary")

        actual = frame_sizes[func]
        assert actual <= budget, (
            f"{func}() stack frame is {actual} bytes, exceeds budget of {budget} bytes. "
            f"This risks stack overflow on target (RP2350). "
            f"Consider reducing local variables or moving large buffers to heap."
        )

    def test_deep_call_chain_total(self, frame_sizes):
        """Estimate worst-case stack consumption along a deep compilation path."""
        total = 0
        breakdown = []
        for func in DEEP_CALL_CHAIN:
            size = frame_sizes.get(func, 0)
            total += size
            breakdown.append(f"  {func}: {size}")

        assert total <= MAX_CALL_CHAIN_BUDGET, (
            f"Estimated deep call chain uses {total} bytes, exceeds budget of "
            f"{MAX_CALL_CHAIN_BUDGET} bytes.\n"
            f"Breakdown:\n" + "\n".join(breakdown) + f"\n"
            f"Total: {total}\n"
            f"The RP2350 process stack is ~64KB. Reduce frame sizes of the "
            f"largest contributors to prevent stack overflow."
        )

    def test_report_all_frames(self, frame_sizes):
        """Informational: print all measured frame sizes."""
        print("\n=== Native TCC Stack Frame Report ===")
        for func in sorted(frame_sizes, key=lambda f: frame_sizes[f], reverse=True):
            size = frame_sizes[func]
            budget = FRAME_BUDGETS.get(func, "N/A")
            status = "OK" if isinstance(budget, int) and size <= budget else "OVER" if isinstance(budget, int) else ""
            print(f"  {func:30s}  {size:6d} bytes  (budget: {budget}) {status}")

        total = sum(frame_sizes.get(f, 0) for f in DEEP_CALL_CHAIN)
        print(f"\n  Deep call chain estimate: {total} bytes / {MAX_CALL_CHAIN_BUDGET} budget")
