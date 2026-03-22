# Fix: 20001009-2.c — Missing sign extension + inline asm register clobber

## Bug

Test: `gcc.c-torture/execute/20001009-2.c`

```c
int a = 0xff;
int c = (signed char)a;       // Expected: c = -1, Actual: c = 255
asm volatile ("" : : "r"(c)); // Clobbers register holding 'a'
if (c != -1) abort();
```

Two independent bugs caused this test to fail:

1. **Missing sign extension**: The `(signed char)` cast was silently dropped.
2. **Inline asm register clobber**: The asm constraint solver picked the
   register already holding `a`, clobbering it.

## Root Cause

### Bug 1: ALLOW_SUBTYPE_ACCESS skips sign extension (tccgen.c)

When casting from `int` to `signed char`, `gen_cast()` enters the
`ALLOW_SUBTYPE_ACCESS` path because:
- `vtop->r & VT_LVAL` is true (local variable `a` is on the stack)
- `ds <= ss` (1 byte ≤ 4 bytes)

This optimization assumes the value is still in memory and a future
byte-sized load will naturally give sign extension. It just changes
`vtop->type.t` and skips code generation.

This is correct for the legacy backend where values stay on the stack,
but the IR backend's register allocator promotes stack slots to registers —
the byte-load never happens.

### Bug 2: Asm constraint solver ignores IR register allocation (arm-thumb-asm.c)

The IR linear-scan allocator (tccls.c) and the inline asm constraint solver
(arm-thumb-asm.c) are two disconnected register-allocation worlds. The asm
solver scans r0 upward for "r" constraints and picks the first free register —
with no knowledge of which registers the IR allocator assigned to live
variables. This can pick a register already holding a live value, and the
operand load in `asm_gen_code` clobbers it.

### Pre-existing bug: Thumb-2 push/pop encoding (arm-thumb-asm.c)

`asm_gen_code()` used `gen_le32(0xe92d0000|regset)` for push and
`gen_le32(0xe8bd0000|regset)` for pop. For Thumb-2, 32-bit instructions
must be emitted as two 16-bit halfwords, not one 32-bit word. The
`gen_le32()` approach wrote bytes in the wrong order.

## Fixes Applied

### Fix 1: Disable ALLOW_SUBTYPE_ACCESS for IR mode (tccgen.c)

```c
if (ALLOW_SUBTYPE_ACCESS && (vtop->r & VT_LVAL) && !tcc_state->ir) {
```

When `tcc_state->ir` is set, the ALLOW_SUBTYPE_ACCESS optimization is
skipped. The fallback SHL+SAR path generates explicit sign extension.

### Fix 2: reserved_regs for asm constraint solver (multiple files)

Added a `reserved_regs[NB_ASM_REGS]` mechanism:

- **ir/codegen.c** (`tcc_ir_codegen_inline_asm_by_id`): Before calling
  `tcc_asm_emit_inline`, iterates over all live interval arrays
  (variables, temporaries, parameters) and marks physical registers of
  intervals live at the current instruction index. These go into a
  `reserved_regs` array.

- **arm-thumb-asm.c** (`asm_compute_constraints`): New `reserved_regs`
  parameter. After initializing `regs_allocated[]` from `clobber_regs`,
  also marks reserved registers as `REG_IN_MASK | REG_OUT_MASK`. This
  prevents the "r" constraint scanner from picking them.

- **Key design**: `reserved_regs` only affects constraint allocation, NOT
  `asm_gen_code` save/restore. This avoids spurious push/pop of callee-saved
  registers that would corrupt output operands.

- **tcc.h**, **tccasm.c**: Updated function signatures to thread
  `reserved_regs` through `tcc_asm_emit_inline` → `asm_compute_constraints`.
  Non-IR call sites pass `NULL`.

### Fix 3: Thumb-2 push/pop encoding (arm-thumb-asm.c)

```c
// Before (broken):
gen_le32(0xe92d0000 | regset);  // push
gen_le32(0xe8bd0000 | regset);  // pop

// After (correct):
gen_le16(0xe92d); gen_le16(regset);  // push: hw1, hw2
gen_le16(0xe8bd); gen_le16(regset);  // pop: hw1, hw2
```

### Fix 4: parse_asm_operands initialization (tccasm.c)

Added `op->reg = -1;` initialization in `parse_asm_operands()` so the
constraint solver correctly detects unassigned operands.

## Files Modified

| File | Change |
|------|--------|
| `tccgen.c` | Guard ALLOW_SUBTYPE_ACCESS with `!tcc_state->ir` |
| `tcc.h` | Updated signatures for `asm_compute_constraints`, `tcc_asm_emit_inline` |
| `arm-thumb-asm.c` | reserved_regs in constraint solver; Thumb-2 push/pop encoding |
| `tccasm.c` | Thread reserved_regs; `op->reg = -1` init |
| `ir/codegen.c` | Compute reserved_regs from live intervals |

## Test Results

- **3154 passed**, 768 xfailed, 0 failed (was 3148 passed before fix — 6 newly passing)
- All previously-regressing tests pass: pr41239, pr43560, pr45695, loop-6
- The target test 20001009-2 passes
