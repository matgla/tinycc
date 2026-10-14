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
    "unary_builtin_shuffle":    1500,  # extracted: shuffle/shufflevector (increased: auto-inline inlines helpers)
    "unary_builtin_chk":        1300,  # extracted: object_size + __*_chk builtins (increased: auto-inline)
    "unary_builtin_alloca":      350,  # extracted: alloca/apply_args/apply/return
    "unary_builtin_overflow":    950,  # extracted: add/sub/mul_overflow (increased: auto-inline inlines helpers)
    "decl":                     1400,  # was 1040, increased by auto-inline candidate tracking locals
    "block":                    1500,  # was 632  — recursive (nested blocks), increased by auto-inline
    "decl_initializer_alloc":    780,  # was 550; +RELRO type_contains_pointer path + auto-inline (non-recursive, one-shot frame)
    "tcc_preprocess":            550,
    "expr_cond":                 450,  # recursive (ternary chains)
    "decl_initializer":          510,  # was 450; self-host codegen drift (body unchanged), recursive — kept tight
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


_FUNC_LABEL = re.compile(r'^[0-9a-f]+ <(\w+)>:')
_ADDR = re.compile(r'^\s*([0-9a-f]+):')


def _parse_frame_sizes(objdump_path, elf_path, functions):
    """Frame size (PUSH registers + SUB SP) of each named function.

    Disassembles the ELF once and examines each function's prologue,
    including an indirect sub sp whose amount comes from the literal pool.
    Functions missing from the symbol table are left out of the result.
    """
    result = subprocess.run(
        [objdump_path, "-d", str(elf_path)],
        capture_output=True,
        text=True,
        timeout=120,
    )
    assert result.returncode == 0, f"objdump failed: {result.stderr}"
    lines = result.stdout.splitlines()

    wanted = set(functions)
    starts = {}
    for i, line in enumerate(lines):
        if line[:1] in "0123456789abcdef":
            m = _FUNC_LABEL.match(line)
            if m and m.group(1) in wanted and m.group(1) not in starts:
                starts[m.group(1)] = i

    addr_lines = None  # address -> first line at it, built on first use

    def literal_word(addr):
        nonlocal addr_lines
        if addr_lines is None:
            addr_lines = {}
            for line in lines:
                m = _ADDR.match(line)
                if m:
                    addr_lines.setdefault(int(m.group(1), 16), line)
        m_word = re.search(r'\.word\s+0x([0-9a-f]+)', addr_lines.get(addr, ""))
        return int(m_word.group(1), 16) if m_word else 0

    frames = {}
    for func, func_start in starts.items():
        push_bytes = 0
        sub_sp = 0
        ldr_literals = {}  # reg -> literal pool address

        # Scan the prologue
        for line in lines[func_start + 1:func_start + 20]:
            # Stop at next function
            if _FUNC_LABEL.match(line):
                break

            # PUSH / STMDB
            m_push = re.search(r'(?:stmdb\s+sp!,|push)\s*\{([^}]+)\}', line)
            if m_push:
                push_bytes += len(m_push.group(1).split(',')) * 4
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
            if m_sub_reg and m_sub_reg.group(1) in ldr_literals:
                sub_sp += literal_word(ldr_literals[m_sub_reg.group(1)])

        frames[func] = push_bytes + sub_sp

    return frames


@pytest.fixture(scope="module")
def frame_sizes():
    """Parse stack frame sizes from the native TCC binary."""
    if not TCC_ELF.exists():
        pytest.skip(f"Native TCC binary not found: {TCC_ELF}")

    objdump = _get_objdump()
    frames = _parse_frame_sizes(objdump, TCC_ELF, FRAME_BUDGETS)
    if not frames:
        # tcc only writes .symtab when linking with -g, so a release build
        # has none of these functions (build_rootfs.sh --debug keeps them).
        pytest.skip(f"{TCC_ELF.name} has no symbols for the budgeted functions")
    return frames


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
