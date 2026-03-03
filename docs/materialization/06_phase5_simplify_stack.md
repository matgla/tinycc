# Phase 5: Simplify Stack and Spill Management

> **Status: ✅ Done** — All sub-phases 5b–5q complete. All operations fully on MOP path. **Phase 5l** ✅: `pr0_spilled`/`pr1_spilled` removed from `IROperand`. **Phase 5m** ✅: `fill_registers_ir` deleted entirely (~256 lines). **Phase 5n** ✅: 10 dead `_op` function bodies + declarations removed (~700 lines). **Phase 5o** ✅: last 3 `_op` handlers converted to `_mop` — dispatch loop is 100% MOP. **Phase 5p** ✅: `pr0_reg`/`pr1_reg` fields removed from `IROperand` (10→9 bytes). Added `irop_phys_r0()`/`irop_phys_r1()` helpers that read interval table. `load_to_dest_ir` takes explicit `(int dest_r0, int dest_r1, IROperand src)`. All legacy `_ir` functions + `arm-thumb-asm.c` converted. `irop_init_phys_regs()` deleted. `tccir_operand.c` conversion functions updated. **Phase 5q** ✅: all legacy `_ir` wrappers deleted (~560 lines); `tcc_gen_mach_load_to_reg` rewritten for direct-dest loading; inline asm operand clobber regression (pr49390) fixed.

## Goal

With backend-driven materialization complete, clean up data structures that were only needed to support the old materialization layer.

## Changes

### 5.1: Simplify `IROperand`

**Remove fields that are only used for materialization state encoding:**

| Field | Current Use | Replacement |
|---|---|---|
| `pr0_spilled` | Set by `fill_registers_ir()` | `MachineOperand.kind == MACH_OP_SPILL` |
| `pr1_spilled` | Set by `fill_registers_ir()` | `MachineOperand.is_64bit && MACH_OP_SPILL` |
| `is_local` | Set by `fill_registers_ir()` | `MachineOperand.kind == MACH_OP_FRAME_ADDR` |
| `is_llocal` | Set by `fill_registers_ir()` | `MachineOperand.kind == MACH_OP_SPILL + needs_deref` |
| `is_param` | Set by `fill_registers_ir()` | `MachineOperand.kind == MACH_OP_PARAM_STACK` |

**Note:** These fields are set by `tcc_ir_fill_registers_ir()` which is deleted in Phase 4. After Phase 4, nothing writes to these fields. Removing them shrinks `IROperand` and eliminates the possibility of stale/incorrect flag state.

**Caution:** Verify that no IR-level pass (optimization, liveness) reads these fields. They should only be read during codegen.

### 5.2: Remove materialization result structs

Delete from `tccir.h` or `ir/mat.h`:

```c
// REMOVE:
typedef struct TCCMaterializedValue { ... };
typedef struct TCCMaterializedAddr { ... };
typedef struct TCCMaterializedDest { ... };
typedef struct TCCMatValue { ... };
typedef struct TCCMatAddr { ... };
typedef struct TCCMatDest { ... };
```

### 5.3: Simplify `TCCStackSlot`

**Remove fields that only existed for materialization decisions:**

| Field | Purpose | Needed? |
|---|---|---|
| `addressable` | Told materialization layer not to spill this | **Remove** — backend decides |
| `live_across_calls` | Told materialization to use callee-saved reg | **Remove** — allocator handles this |

Keep: `kind`, `vreg`, `offset`, `size`, `alignment` — these are fundamental to stack layout.

### 5.4: Remove VT_LLOCAL handling from backend

**Action:** Search `arm-thumb-gen.c` for `is_llocal` or `VT_LLOCAL` references. With `MachineOperand`, the double-indirection case is expressed as `MACH_OP_SPILL` with `needs_deref=true` — there's no separate code path.

### 5.5: Consolidate operand headers

**Current state:** There are two near-duplicate operand headers:
- `tccir_operand.h` (567 lines, 17-bit position)
- `ir/operand.h` (539 lines, 18-bit position)

**Action:** Eliminate the older `tccir_operand.h` and keep only `ir/operand.h`. Update all `#include "tccir_operand.h"` to `#include "ir/operand.h"`.

This is a maintenance hazard flagged during review — fixing it here prevents future bugs from edits to the wrong copy.

## Implementation Steps

### Step 5.1: Audit field usage

```bash
# Verify these fields are only read during codegen (now deleted):
grep -rn 'pr0_spilled\|pr1_spilled' --include='*.c' --include='*.h' | grep -v 'ir/mat.c\|ir/codegen.c'
grep -rn 'is_llocal' --include='*.c' --include='*.h' | grep -v 'ir/mat.c\|ir/codegen.c'
grep -rn 'is_local' --include='*.c' --include='*.h' | grep -v 'ir/mat.c\|ir/codegen.c'
```

Any unexpected callers need investigation before removal.

### Step 5.2: Remove fields from `IROperand`

Edit `ir/operand.h` to remove `pr0_spilled`, `pr1_spilled`, `is_local`, `is_llocal`, `is_param` bitfields.

**Note:** This changes `IROperand` layout. Since it's `__attribute__((packed))` at 10 bytes, removing 5 bits saves space and may improve cache behavior during IR passes.

### Step 5.3: Remove `TCCMaterializedValue`/`Addr`/`Dest` structs

Edit `tccir.h` to delete these struct definitions and any function declarations that reference them.

### Step 5.4: Simplify `TCCStackSlot`

Edit `tccir.h` or `ir/stack.h` to remove `addressable` and `live_across_calls` fields.

### Step 5.5: Consolidate operand headers

1. Diff `tccir_operand.h` vs `ir/operand.h` to identify differences
2. Ensure `ir/operand.h` is the superset
3. Replace all `#include "tccir_operand.h"` with `#include "ir/operand.h"`
4. Delete `tccir_operand.h`

### Step 5.6: Compile and test

```bash
make clean && make cross -j16
make test -j16
make test-gcc-torture-compile
```

## Expected Impact

| Metric | Change |
|---|---|
| `IROperand` size | 10 bytes → ~9 bytes (5 bits freed) |
| Struct types deleted | 6 (3 legacy + 3 new wrapper) |
| `TCCStackSlot` fields | 2 removed |
| Duplicate headers | Consolidated (`tccir_operand.h` deleted) |
| Dead code | All VT_LLOCAL-specific code paths removed |

## Current State (After `0e772abb`)

### Done
- Dead `TCCStackSlot` fields removed (`addressable`, `live_across_calls` — these were never set meaningfully after Phase 0)
- `ir/operand.c`, `ir/operand.h`, `ir/mat.c` deleted (Phase 4)

### Remaining: IROperand codegen-time flags

The `fill_registers_ir` function is now deleted from the production path (behind `#ifdef TCC_REGALLOC_DEBUG`). `machine_op_from_ir` reads the interval table directly. However, the `pr0_reg`/`pr1_reg` fields remain in `IROperand` because legacy `_ir` functions still read/write them:

| Field | Who sets it | Who reads it | Status |
|-------|------------|--------------|--------|
| `pr0_reg` / `pr1_reg` (5 bits each) | `svalue_to_iroperand()`, `irop_copy_svalue_info()`, `asm_gen_code()` | `load_to_dest_ir()` (~38 reads), `store_ex_ir()` (~10 reads), `th_store_resolve_base_ir()` (2 reads) | **Blocked:** legacy `_ir` functions + inline asm |
| `_reserved0` / `_reserved1` (1 bit each) | (unused) | (unused) | **Free** — formerly `pr0_spilled`/`pr1_spilled` (Phase 5l) |
| `is_llocal` | IR construction (`tccgen.c`) | `machine_op_from_ir()` for `needs_deref`; `tccopt.c` | **IR-semantic** — stays |
| `is_local` | IR construction (`tccgen.c`) | `machine_op_from_ir()`; `tccopt.c`; backend helpers | **IR-semantic** — stays |
| `is_param` | IR construction (`tccgen.c`) | `machine_op_from_ir()` | **IR-semantic** — stays |

**Key insight:** `is_local`, `is_llocal`, and `is_param` are IR-semantic — set during IR construction, read during codegen. They do NOT need to be removed. Only `pr0_reg`/`pr1_reg` are pure codegen-time state that should be eliminated.

**Remaining steps for full `pr0_reg`/`pr1_reg` removal:**
1. Convert `asm_gen_code` in `arm-thumb-asm.c` (6 writes) to use `MachineOperand` or read intervals directly
2. Convert `load_to_dest_ir`, `store_ex_ir`, `th_store_resolve_base_ir` in `arm-thumb-gen.c` (~50 reads, 3 writes) to use `MachineOperand` equivalents
3. Remove `pr0_reg : 5` and `pr1_reg : 5` from `IROperand` struct in `tccir_operand.h`
4. Also remove `_reserved0 : 1` and `_reserved1 : 1` (freed from Phase 5l)
5. Update `IROP_NONE` macro and `irop_init_phys_regs()` in `tccir_operand.h`
6. Update `svalue_to_iroperand()`, `iroperand_to_svalue()`, `irop_copy_svalue_info()` in `tccir_operand.c`
7. Verify `sizeof(IROperand)` — expected: 8 bytes, down from 10

### Remaining: `tccir_operand.h` deduplication

Two near-identical operand headers still exist:
- `tccir_operand.h` (root, 17-bit position encoding)
- `tccir_operand.c` (root, companion)

The `ir/` subdirectory no longer has `ir/operand.h` (deleted in Phase 4). The deduplication goal was to eliminate one copy, but since only `tccir_operand.h` remains, this is now moot — the duplication is gone. No further action needed on this item.

## Verification Checklist

- [x] Dead `TCCStackSlot` fields removed (`addressable`, `live_across_calls`)
- [x] `ir/mat.c`, `ir/operand.c`, `ir/operand.h` deleted
- [x] Unconditional dispatch-loop fills removed (Phase 5b)
- [x] `machine_op_from_ir` fills `IROperand *op` in-place (Phase 5b)
- [x] `ir_fill_op` at all old-path `_op` sites, dry-run and real-run (Phase 5b)
- [x] Debug trace blocks use pre-filled local copies (Phase 5b)
- [x] `ir_fill_op` removed from JUMP/JUMPIF dispatch (Phase 5c) — those ops only
      read `irop_get_imm32(dest)` / `src.u.imm32` (raw immediates, never written
      by `fill_registers_ir`); removing the fills is a pure elimination
- [x] SWITCH_TABLE converted to MOP via `tcc_gen_machine_switch_table_mop` (Phase 5c)
      — reads only one register (`mach_ensure_in_reg`), no pr0_reg direct access
- [x] SETIF 64-bit pair dest supported in `tcc_gen_machine_setif_mop` (Phase 5c)
      — `!irop_needs_pair(dest_ir)` guard removed; handler splits dest via
      `mach_make_lo/hi_half`, emits `MOV lo, #0; IT cond; MOV lo, #1; MOV hi, #0`
- [x] MLA converted to MOP via `tcc_gen_machine_mla_mop` (Phase 5c)
      — 4-operand MOP: src1, src2, dest, accum all via `mach_ensure_in_reg`;
      accumulator read from `ir->iroperand_pool[operand_base+3]` converted with
      `machine_op_from_ir`; single `th_mla` instruction; no fallback path needed
- [x] UMULL converted to MOP via `tcc_gen_machine_umull_mop` (Phase 5c)
      — 64-bit dest split via `mach_make_lo/hi_half`; src1/src2 loaded via
      `mach_ensure_in_reg`; single `th_umull` instruction
- [x] `!irop_needs_pair` guard removed for BOOL (Phase 5c) — 64-bit pair sources
      handled via lo/hi ORR reduction to single nonzero test value
- [x] `!irop_needs_pair` guard removed for LOAD (Phase 5c) — 64-bit pair sources/dests
      handled; bug fix: MACH_OP_REG non-deref case now copies hi-half (`src.u.reg.r1 → dest_r1`)
- [x] `!irop_needs_pair` guard removed for FUNC_CALL dest (Phase 5c) — 64-bit pair return
      values handled via `handle_return_value_mop` (R0+R1 writeback); `is_complex` guard retained
- [x] Bug fix: dest/scratch register overlap in `thumb_emit_data_processing_mop64` and
      `thumb_emit_shift64_mop` — dest pair determined BEFORE `mach_resolve_deref_64`;
      src register operands pre-excluded from scratch pool
- [x] Bug fix: PARAM_STACK double-indirection in `mach_resolve_deref_64` — added early return
      for `MACH_OP_PARAM_STACK` with `needs_deref=false`
- [x] `!irop_needs_pair` guard removed for MUL (Phase 5c) — 64-bit pair supported via
      `thumb_emit_mul64_mop`: UMULL for lo 64-bit product, MLA for cross-product hi bits;
      32-bit result from 64-bit source falls back to plain MUL of lo halves
- [x] `!irop_needs_pair` + `!irop_is_64bit` guards removed for TEST_ZERO (Phase 5c) —
      64-bit src handled via `mach_resolve_deref_64` + `CMP lo,#0 / IT EQ / CMP hi,#0`
- [x] `!irop_needs_pair` guard removed for DIV/UDIV/IMOD/UMOD (Phase 5c) — these are
      dead guards: `tccgen.c` lowers 64-bit integer division to `__divdi3` / `__udivdi3` /
      `__moddi3` / `__umoddi3` FUNCCALL IR before the backend; no 64-bit TCCIR_OP_DIV ever
      reaches `tcc_gen_machine_muldiv_mop` in practice
- [x] `make test -j16` passes — 3310 passed, 0 failed (all tests)
- [x] FP double-precision `!irop_needs_pair` guards removed (Phase 5c) — `tcc_gen_machine_fp_mop`
      extended with `fp_mop_load_double_arg`, `fp_mop_do_bl`, `fp_mop_writeback_result` helpers;
      all FADD/FSUB/FMUL/FDIV/FNEG/FCMP/CVT_* opcodes handle `is_double=true` via
      `__aeabi_dadd`, `__aeabi_dsub`, etc.; `!irop_needs_pair` guards removed from both
      dispatch loops
- [x] `!ir->has_static_chain` guards removed from MOP dispatch (44 occurrences, Phase 5c) —
      new `MACH_OP_CHAIN_REL` operand kind added (`ir/machine_op.h`, `ir/machine_op.c`);
      captured variables detected in `machine_op_from_ir` via `captured_offsets_list` scan;
      handled in `mach_ensure_in_reg`, `mach_writeback_dest`, `fp_mop_load_arg`,
      `mach_make_hi_half`, `load_mop`, `store_mop` (32-bit and 64-bit branches)
- [x] LEA converted to MOP path (was already on MOP path in both dispatch loops)
- [x] Dead old-path `else` branches removed (Phase 5d) — 14 unreachable fallbacks
      deleted from both dry-run and real-run dispatch loops; 17 unconditionally-true
      `use_mop_*` flag variables eliminated; only `use_mop_fp` and `use_mop_func_call`
      remain (conditional on `is_complex`); `ir/codegen.c` reduced by 440 lines
      (3149 → 2709); LOAD/ASSIGN/LOAD_INDEXED `*_before_ret` peephole conditions
      simplified to just the `before_ret` guard
- [x] `*_before_ret` peephole converted to MOP path (Phase 5e) — LOAD, LOAD_INDEXED,
      ASSIGN `before_ret` branches now construct synthetic `MACH_OP_REG(R0/R1)` dest
      and patch interval allocation instead of falling back to old `_op` path;
      6 old-path call sites eliminated from both dispatch loops; `ir/codegen.c`
      2711 lines (net +2 from new peephole logic, −730 from old-path removal)
- [x] `machine_op_from_ir` decoupled from `fill_registers_ir` (Phase 5f) — function
      reads interval table directly, `const IROperand *` signature (no mutation);
      `mop_fixup_subcomponent()` helper for LOAD/STORE sub-component access;
      LOAD/STORE dispatch guards `mop_src.kind != MACH_OP_NONE` to fall back to
      old `_op` path for operands with tag=VREG, vreg=-1 (unfilled)
- [x] FUNCCALL `func_target` converted to MachineOperand (Phase 5g) —
      `tcc_gen_machine_func_call_mop` signature changed from `IROperand func_target`
      to `MachineOperand func_mop`; pre-save logic rewritten to use `func_mop.kind`,
      `func_mop.u.reg.r0`, `func_mop.needs_deref` instead of `pr0_reg`/`is_lval`;
      new `gcall_or_jump_mop()` function handles MACH_OP_SYMBOL (direct BL),
      MACH_OP_IMM (relative), and indirect calls via `mach_ensure_in_reg`;
      `ir/codegen.c` call sites use `machine_op_from_ir(ir, &src1_ir)` for func_target,
      eliminating `ir_fill_op` for both `src1_ir` and `src2_ir` on MOP path;
      all 3310 tests pass
- [x] LOAD spilled-dest support (Phase 5h) — `tcc_gen_machine_load_mop` rewritten
      to accept any dest kind (MACH_OP_REG, MACH_OP_SPILL, MACH_OP_PARAM_STACK)
      using `mach_get_dest_reg` + `mach_writeback_dest` pattern; 64-bit spilled dest
      handled via `mach_make_hi_half` + separate writeback; LOAD dispatch condition
      widened from `mop_dest.kind == MACH_OP_REG` to `mop_dest.kind != MACH_OP_NONE`
      in both dry-run and real-run loops; eliminates all LOAD fallbacks observed in
      test suite (8 test files previously triggered spilled-dest fallback);
      all 3310 tests pass
- [x] LOAD/STORE `MACH_OP_NONE` fallback converted to `tcc_error` (Phase 5i) — zero tests
      triggered the fallback; converting to a compiler error proves the old `_op` path is
      dead for LOAD/STORE; `ir/codegen.c` simplified by removing 4 fallback branches
- [x] Dead `_op` backend functions removed (Phase 5j) — ~2400 lines deleted from
      `arm-thumb-gen.c`: `tcc_gen_machine_data_processing_op`, `tcc_gen_machine_assign_op`,
      `tcc_gen_machine_load_op`, `tcc_gen_machine_fp_op`, `tcc_gen_machine_func_call_op`,
      `tcc_gen_machine_return_value_op`, and supporting helpers (`fill_register_arg`,
      `tcc_gen_machine_func_start_op`, `tcc_gen_machine_func_jump_op`); VREG/-1 edge case
      handled in `machine_op_from_ir` (pre-assigned physical reg); FPU_NONE compile guard
      added for `tcc_gen_machine_fp_mop`
- [x] Callsite arg-handling converted to MOP (Phase 5k) — `fill_arg_from_machine_op` bridge
      function deleted (~90 lines); `thumb_build_call_layout_from_ir` updated with
      `MachineOperand **out_mops` 7th parameter; `build_reg_move_64bit/32bit` and
      `place_stack_arg_64bit/32bit` rewritten to take `MachineOperand *mop` instead of
      `IROperand *arg`; `THUMB_ARG_MOVE_LVAL` enum variant removed (replaced by
      `THUMB_ARG_MOVE_MOP` with needs_deref); `tcc_gen_machine_fp_mop` signature extended
      with `int is_complex` param; `is_complex` guards removed from FP/FUNCCALL dispatch
      in `ir/codegen.c` (both dry-run and real-run); `tcc_ir_fill_registers_ir` and
      `ir_fill_op` wrapped in `#ifdef TCC_REGALLOC_DEBUG` (no longer called in production)
- [x] Bug fix: ARM_R12 base clobber in `place_stack_arg_64bit` (Phase 5k) — when placing
      a 64-bit needs_deref operand on stack, `mach_ensure_in_reg` returned ARM_R12 as base,
      then `load_from_base_ir(ARM_R12, ..., ARM_R12)` clobbered the pointer before hi-half
      load; fixed by excluding `(1u << ARM_R12)` from base allocation
- [x] Bug fix: PARAM_STACK double-indirection (Phase 5k) — `needs_deref=true` on
      PARAM_STACK operands (from `interval->is_lvalue`) was incorrectly treated as
      pointer-to-follow; PARAM_STACK always contains the value directly in the caller's
      argument area; fixed by excluding `MACH_OP_PARAM_STACK` from the `needs_deref`
      path in both `place_stack_arg_64bit` and `THUMB_ARG_MOVE_MOP` handler
- [x] `pr0_spilled`/`pr1_spilled` removed from `IROperand` (Phase 5l) — replaced with
      `_reserved0`/`_reserved1` to maintain 10-byte packed layout; all `.pr0_spilled` /
      `.pr1_spilled` reads/writes removed from `arm-thumb-gen.c`, `ir/codegen.c`,
      `tccir_operand.c`, `arm-thumb-asm.c`; 2 bits freed in packed struct
- [x] `fill_registers_ir` + `ir_fill_op` deleted from production (Phase 5m) — ~256 lines
      removed from `ir/codegen.c`: function body, wrapper, `_dbg_trace_all` variable +
      matching block, main debug trace block; declaration removed from `tccir.h`;
      `#ifdef TCC_REGALLOC_DEBUG` vreg stats + `[RA-PEEPHOLE]` trace kept (independent)
- [x] 10 dead `_op` declarations + bodies removed (Phase 5n) — ~700 lines from
      `arm-thumb-gen.c`: `load_indexed_op`, `store_indexed_op`, `load_postinc_op`,
      `store_postinc_op`, `indirect_jump_op`, `switch_table_op`, `setif_op`, `bool_op`,
      `func_parameter_op`, `vla_op`; 10 declarations from `tcc.h`; 2 dead static helpers
      (`thumb_irop_has_immediate_value`, `thumb_irop_needs_value_load`) also removed
- [x] Last 3 `_op` handlers converted to `_mop` (Phase 5o) — `jump_op` → `jump_mop`,
      `conditional_jump_op` → `conditional_jump_mop`, `trap_op` → `trap_mop`; dispatch
      loop now 100% MOP; 5 call sites updated in dry-run + real-run loops
- [x] `machine_op_from_ir` vreg=-1 path decoupled from `pr0_reg` (Phase 5p partial) —
      `IROP_VREG_PHYS_VALID` (0x100) + `IROP_VREG_PHYS_MASK` (0x1F) encoding in `u.imm32`
      for IROP_TAG_VREG operands with vreg=-1; `svalue_to_iroperand()` Case 1b encodes
      pinned physical register; `machine_op_from_ir()` reads `u.imm32` instead of `pr0_reg`;
      Case 1 (vr >= 0) must NOT set `u.imm32` (breaks complex imaginary part access);
      GCC torture test 20030222-1 fixed (inline asm 64→32 constraint load)
- [x] `pr0_reg`/`pr1_reg` removed from `IROperand` — blocked by ~50 reads in `arm-thumb-gen.c`
      legacy `_ir` functions and 6 writes in `arm-thumb-asm.c` — **RESOLVED (Phase 5q):** all legacy
      `_ir` functions deleted; inline asm path converted to `tcc_gen_mach_load_to_reg`/`tcc_gen_mach_store_from_reg`
- [x] `_reserved0`/`_reserved1` removed from `IROperand` — removed along with `pr0_reg`/`pr1_reg` in Phase 5p

## Phase 5a: Failed Attempt — Internalize Fill in `machine_op_from_ir`

### What was tried

Added `fill_registers_ir` call inside `machine_op_from_ir` so it would be self-contained:

```c
MachineOperand machine_op_from_ir(TCCIRState *ir, const IROperand *op)
{
    IROperand filled = *op;
    tcc_ir_fill_registers_ir(ir, &filled);
    op = &filled;
    // ... rest of conversion
}
```

### Why it failed (30 test failures)

`fill_registers_ir` is **NOT idempotent**. For `IROP_TAG_STACKOFF` operands, it applies:
```c
delta = old_stackoff - interval->original_offset;
op->u.imm32 += delta;
```

The dispatch loop already calls `fill_registers_ir` unconditionally at lines 1382–1386 (dry-run) and 2091–2095 (real-run) **before** `machine_op_from_ir` is called. Adding fill inside `machine_op_from_ir` = double-fill → delta applied twice → corrupted stack offsets → 30 GCC torture test failures.

The sub-component access logic (pr1_reg remap for `__imag__`) was also moved into `machine_op_from_ir` during this attempt but had to be reverted — old-path 64-bit pair operands can also have `pr1_reg != NONE && u.imm32 != 0` from fill's delta calculation, which is not an `__imag__` sub-component.

### Lesson

Cannot add fill inside `machine_op_from_ir` without simultaneously removing all dispatch-level fills.

## Phase 5b: Correct Approach — Coordinated Fill Removal

Must be done as a **single coordinated change**:

### Step 1: Remove dispatch-level fills

Remove the 6 unconditional `tcc_ir_fill_registers_ir()` calls from the dispatch loop:
- Dry-run: lines 1382–1386 (src1, src2, dest)
- Real-run: lines 2091–2095 (src1, src2, dest)

### Step 2: Add fill inside `machine_op_from_ir`

Now safe because it’s the only fill — no double-application.

### Step 3: Add targeted fills at old-path `_op` call sites

For all ops that bypass the MOP path and still need filled IROperands:
- `tcc_gen_machine_data_processing_op` (64-bit pair fallback)
- `tcc_gen_machine_assign_op` (64-bit pair fallback)
- `tcc_gen_machine_func_call_op` (64-bit/complex/static-chain fallback)
- `tcc_gen_machine_load_op` / `store_op` (64-bit pair fallback)
- `tcc_gen_machine_return_value_op` (64-bit fallback)
- `tcc_gen_machine_fp_op` (double/complex fallback)
- `tcc_gen_machine_lea_op`, `jump_op`, `conditional_jump_op` (always old-path)
- All remaining old-path ops

### Step 4: Handle LOAD/STORE sub-component fixup

The `__imag__` pr1_reg remap (lines 1535–1555 in codegen.c) must either:
- Be computed from the raw (unfilled) operand before fill, or
- Be passed as a flag to `machine_op_from_ir` (e.g., `machine_op_from_ir_for_load()`)

### Step 5: Handle debug traces

The `_dbg_trace_all` and `TCC_MACH_DBG` blocks read filled operand fields (`pr0_reg`, `is_lval`, etc.). These need fill before trace, or the trace format needs updating.

### Risk

This is a wide-reaching change touching every old-path dispatch site. Must be done with extreme care and tested against the full GCC torture suite (3310 tests).

## Phase 5d: Dead Old-Path Fallback Removal (COMPLETED)

### What was done

Removed 14 dead (unreachable) `else` branches from both the dry-run and real-run
dispatch loops in `ir/codegen.c`. These branches unconditionally used the MOP path
(their `use_mop_*` flag was always `true`) but still carried dead fallback code for
the old `_op` path.

### Ops cleaned up (14 dead sites × 2 loops = 28 branches removed)

| Op | Old flag (always true) |
|----|----------------------|
| STORE | `use_mop_store` |
| STORE_INDEXED | `use_mop_store_indexed` |
| LOAD_POSTINC | `use_mop_load_postinc` |
| STORE_POSTINC | `use_mop_store_postinc` |
| RETURNVALUE | `use_mop_ret` |
| MUL, DIV, TEST_ZERO | `use_mop_mul` |
| MLA | `use_mop_mla` |
| UMULL | `use_mop_umull` |
| DP (data processing) | `use_mop_dp` |
| IJUMP | `use_mop_ijump` |
| SETIF | `use_mop_setif` |
| BOOL | `use_mop_bool` |
| FUNCPARAM | `use_mop_func_param` |
| VLA | `use_mop_vla` |

### Additional simplifications

- **LOAD/ASSIGN/LOAD_INDEXED**: Removed always-true `use_mop_*` part of conditions,
  kept the `*_before_ret` peephole guards (these are runtime-variable).
- **17 `use_mop_*` flag variables deleted** along with their corresponding
  `switch` case assignments in both loops.
- Only **`use_mop_fp`** and **`use_mop_func_call`** remain — both are conditional
  on `!is_complex` and guard the FP/FUNCCALL old-path fallbacks needed for
  `_Complex` type support.

### Results

- `ir/codegen.c`: 3149 → 2709 lines (**−440 lines**, −14%)
- All IR tests pass
- Build clean with `-Werror`

## Phase 5e: Convert `before_ret` Peephole to MOP Path (COMPLETED)

### What was done

The LOAD, LOAD_INDEXED, and ASSIGN ops each had a `*_before_ret` peephole:
when the instruction immediately precedes RETURNVALUE on the same vreg, the
old-path `_op` handler was called so it could write directly to R0. This was
the last non-complex reason these three ops fell back to the old dispatch path.

Phase 5e converts these peephole branches to use the MOP path instead:

1. **Patch interval allocation** — when `before_ret` is detected, the dest
   vreg's `IRLiveInterval` allocation is patched to `R0` (and `R1` for 64-bit),
   so subsequent MOP handlers see the return register as the physical allocation.

2. **Synthetic MOP dest** — instead of calling `machine_op_from_ir(dest)`,
   construct `(MachineOperand){.kind = MACH_OP_REG, .u.reg.r0 = REG_IRET, ...}`
   directly. This ensures the load/assign writes straight to R0 without a
   later MOV in RETURNVALUE.

### Sites converted (6 old-path call sites × 2 loops = 12 removed)

| Op | Dry-run | Real-run |
|----|---------|----------|
| LOAD | `tcc_gen_machine_load_op` → MOP with R0 dest | same |
| LOAD_INDEXED | `tcc_gen_machine_load_op` → MOP with R0 dest | same |
| ASSIGN | `tcc_gen_machine_assign_op` → MOP with R0 dest | same |

### Results

- `ir/codegen.c`: 2711 lines (net +2 from new peephole logic, −730 lines from old-path removal)
- Only `is_complex` FP/FUNCCALL guards remain as old-path dispatch
- All IR tests pass
- Build clean with `-Werror`

## Phase 5f: Decouple `machine_op_from_ir` from `fill_registers_ir` (COMPLETED)

### What was done

Rewrote `machine_op_from_ir` in `ir/machine_op.c` to read the register-allocation
interval table directly instead of calling `tcc_ir_fill_registers_ir()`. The function
no longer mutates the `IROperand` — its signature changed to `const IROperand *op`.

### Key changes

1. **`ir/machine_op.c`**: Complete rewrite of `machine_op_from_ir`:
   - Reads `IRLiveInterval` directly for register/spill/offset info
   - 5 sections: (1) IMM constants, (2) SYMREF symbols, (3) concrete stack slots
     (vreg < 0, is_local/is_llocal/tag=STACKOFF), (4) allocated operands via interval,
     (5) MACH_OP_NONE fallback
   - Handles unallocated vregs (`PREG_NONE, offset=0`) as spills
   - Sub-component offset delta computed inline (replaces fill's `old_stackoff - original_offset`)

2. **`ir/machine_op.h`**: Signature updated to `const IROperand *op`

3. **`ir/codegen.c`**: New `mop_fixup_subcomponent()` helper for LOAD/STORE
   sub-component access (e.g., `__imag__` on `_Complex float`). Previously this
   was done by reading `pr1_reg`/`u.imm32` from the filled operand.

4. **LOAD/STORE dispatch guards**: Both dry-run and real-run LOAD/STORE checks
   now verify `mop_src.kind != MACH_OP_NONE` (LOAD) or both operands (STORE)
   before entering the MOP path. Operands with tag=VREG, vreg=-1 (unfilled
   temporaries) produce MACH_OP_NONE and fall back to the old `_op` path with
   explicit `ir_fill_op` calls.

### Bug found and fixed

Operands with `tag=IROP_TAG_VREG, vreg=-1` (negative vreg sentinel encoding, not
same as `IROP_NONE`) are not tracked by the interval table. The old code handled
them via `fill_registers_ir` which left them unchanged, and the old `machine_op_from_ir`
would produce a valid result via tag-based dispatch. The new code returns
`MACH_OP_NONE` for these, and the dispatch loop falls back to old `_op` path.

Section 3 also broadened to catch `tag=IROP_TAG_STACKOFF` operands with vreg < 0
even without `is_local`/`is_llocal` flags (raw stack offset references from struct
temporaries).

### Results

- `ir/machine_op.c`: `machine_op_from_ir` is now a pure query (no mutation)
- `fill_registers_ir` only called at old-path fallback sites (FP complex,
  FUNCCALL complex, and MACH_OP_NONE fallback for LOAD/STORE)
- `ir/codegen.c`: ~2732 lines
- All 3310 IR tests pass, 156 asm tests pass
- Build clean with `-Werror`

## Phase 5i: LOAD/STORE MACH_OP_NONE Fallback → tcc_error (COMPLETED)

### What was done

Converted the LOAD/STORE `MACH_OP_NONE` fallback branches from old `_op` path
calls to `tcc_error("compiler_error: ...")`. Zero tests in the full suite (3310 IR +
GCC torture + ASM) ever triggered these fallbacks, proving the old `_op` path is
dead for LOAD and STORE operations.

### Impact

- 4 fallback branches removed from `ir/codegen.c` (2 dry-run + 2 real-run)
- Simplifies future cleanup: any regression that hits these paths will be caught
  at compile time with a clear error message instead of silently using stale code

## Phase 5j: Dead `_op` Backend Function Removal (COMPLETED)

### What was done

Removed ~2400 lines of dead `_op` backend functions from `arm-thumb-gen.c`. These
functions were the old IROperand-based handlers that have been fully replaced by
MOP-based handlers. With Phase 5i proving the fallbacks are unreachable, these
functions are dead code.

### Functions deleted

| Function | Lines | Role |
|----------|-------|------|
| `tcc_gen_machine_data_processing_op` | ~350 | Old DP handler (ADD/SUB/CMP/etc.) |
| `tcc_gen_machine_assign_op` | ~200 | Old ASSIGN handler |
| `tcc_gen_machine_load_op` | ~400 | Old LOAD handler |
| `tcc_gen_machine_fp_op` | ~300 | Old FP handler |
| `tcc_gen_machine_func_call_op` | ~500 | Old FUNCCALL handler |
| `tcc_gen_machine_return_value_op` | ~150 | Old RETURNVALUE handler |
| `fill_register_arg` | ~100 | Old fill helper |
| `tcc_gen_machine_func_start_op` | ~80 | Old func_start helper |
| `tcc_gen_machine_func_jump_op` | ~80 | Old func_jump helper |
| Various supporting helpers | ~240 | Old-path-only utilities |

### Additional fixes

- `machine_op_from_ir`: VREG/-1 with pre-assigned `pr0_reg` now correctly produces
  `MACH_OP_REG` (previously fell through to `MACH_OP_NONE`)
- `tcc_gen_machine_fp_mop`: Added `#ifndef FPU_NONE` compile guard for builds
  without FPU support

### Results

- `arm-thumb-gen.c`: reduced from ~11700 → ~9300 lines
- All `_op` function declarations removed from `tcc.h`
- All 3310 tests pass

## Phase 5k: Callsite Arg-Handling MOP Conversion (COMPLETED)

### What was done

Converted the entire callsite argument placement pipeline from IROperand to
MachineOperand, eliminating the last bridge between the two representations.

### Key changes

1. **`fill_arg_from_machine_op` bridge deleted** (~90 lines): This function
   reverse-engineered IROperand fields from MachineOperand to pass to the old
   arg-handling functions. With native MOP support, it's no longer needed.

2. **`thumb_build_call_layout_from_ir` updated**: New 7th parameter
   `MachineOperand **out_mops` — returns the MOP array alongside the existing
   IROperand pool for struct and complex args still on the old path.

3. **Arg placement functions rewritten**:
   - `build_reg_move_64bit(ThumbArgMove*, int, MachineOperand*, IROperand*, int, ...)`
   - `build_reg_move_32bit(ThumbArgMove*, int, MachineOperand*, IROperand*, int, ...)`
   - `place_stack_arg_64bit(MachineOperand*, int, TCCIRState*)`
   - `place_stack_arg_32bit(MachineOperand*, int, CallGenContext*)`

4. **`THUMB_ARG_MOVE_LVAL` removed**: Was a special enum variant for lval args.
   `THUMB_ARG_MOVE_MOP` with `needs_deref=true` handles all dereference cases.

5. **`tcc_gen_machine_fp_mop` signature extended**: Added `int is_complex` param
   so the FP handler can dispatch to complex float operations (add/sub/mul/div)
   directly.

6. **`is_complex` guards removed from ir/codegen.c**: FP and FUNCCALL dispatch
   in both dry-run and real-run loops now unconditionally use the MOP path.
   Complex type handling is inside the MOP handlers themselves.

7. **`fill_registers_ir` / `ir_fill_op` wrapped in `#ifdef TCC_REGALLOC_DEBUG`**:
   No longer called in production builds. Only used for debug trace output.

### Bug fixes

**ARM_R12 base clobber in `place_stack_arg_64bit`:** When placing a 64-bit
`needs_deref` operand on the stack, `mach_ensure_in_reg` could return ARM_R12
as the base register. The code then did:
```
ldr ip, [base]      ; ip = lo half VALUE (base clobbered if base==ip)
str ip, [sp, #0]
ldr ip, [base, #4]  ; BUG: base was clobbered → HardFault
str ip, [sp, #4]
```
Fixed by excluding `(1u << ARM_R12)` from the base register allocation mask.

**PARAM_STACK double-indirection:** `needs_deref=true` on PARAM_STACK operands
(from `interval->is_lvalue`) was incorrectly interpreted as "dereference this
pointer". For PARAM_STACK, the 64-bit value IS directly in the caller's argument
area — `needs_deref` just means the param is addressable, not that it's a pointer.
The `needs_deref` path did double indirection: load value from stack, then use
that value as a pointer → HardFault or garbage data. Fixed by excluding
`MACH_OP_PARAM_STACK` from the `needs_deref` path in both `place_stack_arg_64bit`
and the `THUMB_ARG_MOVE_MOP` handler.

### Results

- `arm-thumb-callsite.c`: 322 lines (−29 from bridge deletion)
- `ir/codegen.c`: 2630 lines (−100 from guard removal)
- `arm-thumb-gen.c`: 9332 lines (net change from rewrite)
- `fill_registers_ir` no longer called in production code
- All 3310 tests pass, 79 skipped, 582 xfailed, 0 failures
## Phase 5l: Remove `pr0_spilled` / `pr1_spilled` from `IROperand` (COMPLETED)

### What was done

Replaced `pr0_spilled : 1` and `pr1_spilled : 1` with `_reserved0 : 1` and
`_reserved1 : 1` in `IROperand` struct (`tccir_operand.h`) to maintain 10-byte
packed layout. Removed all `.pr0_spilled` / `.pr1_spilled` writes/reads.

### Files modified

- `tccir_operand.h`: struct fields, `IROP_NONE` macro, `irop_init_phys_regs`
- `tccir_operand.c`: `irop_copy_svalue_info` (removed copy), `irop_to_svalue`
  (set SValue fields to 0), removed spill comparisons from validation function
- `arm-thumb-gen.c`: `load_to_dest_ir`, `load_to_reg_ir` — simplified conditional
  logic that checked spill flags (all live callers already passed 0)
- `ir/codegen.c`: removed writes in `fill_registers_ir` (debug-only), removed
  `spill=%d` from debug trace format
- `arm-thumb-asm.c`: removed 6 spill-flag assignments in `asm_gen_code`

### Results

- 2 bits freed in packed struct (currently `_reserved0`/`_reserved1`)
- All 3310 tests pass, 79 skipped, 582 xfailed — no regressions

## Phase 5m: Delete `fill_registers_ir` Entirely (COMPLETED)

### What was deleted (~256 lines)

- `tcc_ir_fill_registers_ir()` body (~157 lines) + header comment
- `ir_fill_op()` wrapper (~8 lines)
- `_dbg_trace_all` variable + function name matching block (~25 lines)
- Main debug trace block calling `ir_fill_op` for `trc_s1/s2/d` (~60 lines)
- Declaration + comment (6 lines) from `tccir.h`
- Stale comments referencing `fill_registers_ir` / `ir_fill_op`

### Files modified

- `ir/codegen.c`, `tccir.h`

**Note:** The `#ifdef TCC_REGALLOC_DEBUG` vreg statistics block and `[RA-PEEPHOLE]`
trace were kept — they don't depend on `fill_registers_ir`.

### Results

- All 3310 tests pass, 79 skipped, 582 xfailed — no regressions
- Clean build with `CFLAGS+='-DTCC_REGALLOC_DEBUG'`

## Phase 5n: Delete Dead `_op` Declarations and Bodies (COMPLETED)

### What was deleted (~700 lines)

10 dead `_op` function bodies from `arm-thumb-gen.c` + 10 declarations from `tcc.h`:

| Function | File |
|----------|------|
| `tcc_gen_machine_load_indexed_op` | tcc.h + arm-thumb-gen.c |
| `tcc_gen_machine_store_indexed_op` | tcc.h + arm-thumb-gen.c |
| `tcc_gen_machine_load_postinc_op` | tcc.h + arm-thumb-gen.c |
| `tcc_gen_machine_store_postinc_op` | tcc.h + arm-thumb-gen.c |
| `tcc_gen_machine_indirect_jump_op` | tcc.h + arm-thumb-gen.c |
| `tcc_gen_machine_switch_table_op` | tcc.h + arm-thumb-gen.c |
| `tcc_gen_machine_setif_op` | tcc.h + arm-thumb-gen.c |
| `tcc_gen_machine_bool_op` | tcc.h + arm-thumb-gen.c |
| `tcc_gen_machine_func_parameter_op` | tcc.h + arm-thumb-gen.c |
| `tcc_gen_machine_vla_op` | tcc.h + arm-thumb-gen.c |

Also deleted 2 now-unused static helpers: `thumb_irop_has_immediate_value`,
`thumb_irop_needs_value_load`.

### Results

- `arm-thumb-gen.c`: −700 lines
- All 3310 tests pass — no regressions

## Phase 5o: Convert Control-Flow `_op` Handlers to `_mop` (COMPLETED)

### What was done

Converted the last 3 `_op` handlers to `_mop` so the dispatch loop is 100% MOP:

| Old | New | Change |
|---|---|---|
| `tcc_gen_machine_jump_op(TccIrOp, IROperand, int)` | `tcc_gen_machine_jump_mop(TccIrOp, int32_t, int)` | Extract `irop_get_imm32(dest)` at call site |
| `tcc_gen_machine_conditional_jump_op(IROperand, TccIrOp, IROperand, int)` | `tcc_gen_machine_conditional_jump_mop(int32_t, TccIrOp, int32_t, int)` | Extract raw scalars at call site |
| `tcc_gen_machine_trap_op(void)` | `tcc_gen_machine_trap_mop(void)` | Rename only |

### Files changed

- `tcc.h` (declarations), `arm-thumb-gen.c` (bodies), `ir/codegen.c` (5 call sites)

### Results

- All backend dispatch now uses `_mop` variants or extracted scalars
- No `IROperand` passed to any backend handler
- All 3310 tests pass — no regressions

## Phase 5p: Decouple `machine_op_from_ir` from `pr0_reg` (COMPLETED)

### What was done

The `machine_op_from_ir()` dispatch path for vreg=-1 operands was reading
`op->pr0_reg` to determine which physical register to use. This was decoupled
via an encoding in `u.imm32`:

1. Defined `IROP_VREG_PHYS_VALID` (0x100) and `IROP_VREG_PHYS_MASK` (0x1F)
   in `tccir_operand.h`

2. `svalue_to_iroperand()` Case 1b (vreg=-1): now sets
   `result.u.imm32 = IROP_VREG_PHYS_VALID | (val_kind & IROP_VREG_PHYS_MASK)`

3. `machine_op_from_ir()` vreg=-1 path: reads `op->u.imm32` instead of `op->pr0_reg`

### Important constraint

Case 1 (vr >= 0) must **NOT** set `u.imm32` — the legacy `load_to_dest_ir()` (now deleted in Phase 5q)
used `u.imm32 != 0` on VREG operands for sub-component access (complex imaginary part).
This constraint was validated during Phase 5p: setting it caused GCC torture test 20030222-1 to fail.

### What remains

**✅ All resolved (Phase 5q).** The following functions that read `pr0_reg`/`pr1_reg` have all been deleted:

| Function | File | Status |
|---|---|---|
| `load_to_dest_ir` | `arm-thumb-gen.c` | ✅ Deleted (Phase 5q) |
| `store_ex_ir` | `arm-thumb-gen.c` | ✅ Deleted (Phase 5q) |
| `th_store_resolve_base_ir` | `arm-thumb-gen.c` | ✅ Deleted (Phase 5q) |
| `load_to_reg_ir` | `arm-thumb-gen.c` | ✅ Deleted (Phase 5q) |
| `irop_phys_r0` / `irop_phys_r1` | `arm-thumb-gen.c` | ✅ Deleted (Phase 5q) |
| `asm_gen_code` | `arm-thumb-asm.c` | ✅ Converted to `tcc_gen_mach_load_to_reg`/`tcc_gen_mach_store_from_reg` (Phase 5q) |
| `svalue_to_iroperand` | `tccir_operand.c` | ✅ Updated (Phase 5p — no pr0/pr1) |
| `iroperand_to_svalue` | `tccir_operand.c` | ✅ Updated (Phase 5p) |
| `irop_copy_svalue_info` | `tccir_operand.c` | ✅ Updated (Phase 5p) |
| `tcc_ir_fill_registers` (SValue) | `ir/codegen.c` | ✅ Updated (Phase 5p) |
| Validation function | `tccir_operand.c` | ✅ Updated (Phase 5p) |

The inline asm path now uses `tcc_gen_mach_load_to_reg` (rewritten in Phase 5q to load directly into dest register without scratch intermediary) and `tcc_gen_mach_store_from_reg` (delegates to `mach_writeback_dest`). No `pr0_reg`/`pr1_reg` references remain in the codebase.

### Results

- `machine_op_from_ir` fully decoupled from `pr0_reg`
- 3 GCC torture tests confirmed working (pr41239, pr46309, pr58831)
- All 3310 tests pass — no regressions