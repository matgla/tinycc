# Materialization Refactor: Move from IR to Machine Backend

## Current Status (as of 2026-03-06)

| Phase | Status | Commit |
|-------|--------|--------|
| 0: SValue Elimination | ✅ Done | `e19755e6` |
| 1: MachineOperand type | ✅ Done — type + `machine_op_from_ir()` reads interval table directly; no `fill_registers_ir` dependency | unstaged (`ir/machine_op.c`) |
| 2: Backend materialization | ✅ Done — all ops on MOP path; `!irop_needs_pair` guards removed; 64-bit pair sources handled via `mach_resolve_deref_64`; RETURNVALUE supports 64-bit; 3 backend bugs fixed | unstaged |
| 3: Dry-run integration | ✅ Done — scratch conflict fixup + R_FP exclusion | `c2569883` |
| 4: Eliminate `ir/mat.c` | ✅ Done — `ir/mat.c`, `ir/operand.c`, `ir/operand.h` deleted | `bc43b639` |
| 5 | Simplify Stack/Spill | ✅ Done — Phases 5b–5q ✅; all ops fully on MOP path; `fill_registers_ir` deleted; ~3100 lines dead `_op` functions+helpers deleted; callsite arg-handling on MOP; `is_complex` guards removed from FP/FUNCCALL dispatch; `pr0_spilled`/`pr1_spilled` removed from `IROperand`; 10 dead `_op` bodies removed; jump/cond_jump/trap converted to `_mop`; `pr0_reg`/`pr1_reg` fields removed from `IROperand` (10→9 bytes); all legacy `_ir` wrappers deleted (~560 lines); `tcc_gen_mach_load_to_reg` rewritten for direct-to-dest loading; inline asm path fully on MOP | unstaged |
| 6: Consolidate dispatch | ✅ Done — merged dry-run and real-run loops into single `for (pass = 0; pass < 2; pass++)` loop; extracted `ir_codegen_before_ret_peephole()`, `ir_codegen_record_scratch()`, `ir_codegen_check_scratch()`, `ir_codegen_track_scratch()` helpers; `ir/codegen.c` reduced from 2106→1767 lines (−339 lines, ~16%) | unstaged |

**Next:** All phases complete. Legacy `_ir` wrapper functions deleted (Phase 5q). All codegen paths use MachineOperand exclusively. Ready for new feature work.

## Problem Statement

The current materialization layer (`ir/mat.c`, `ir/codegen.c`) sits between the IR and the backend (`arm-thumb-gen.c`), creating a tangled intermediate abstraction:

1. **Materialization duplicates backend logic.** `ir/mat.c` decides when to load spills, how to handle constants, when addresses are encodable, etc. But the backend *also* makes these decisions (via `load_to_reg_ir`, `get_scratch_reg_with_save`, `tcc_machine_can_encode_stack_offset`). The two layers constantly second-guess each other.

2. **Register fill is fragile.** `ir/codegen.c:tcc_ir_fill_registers()` translates allocation results back into `SValue`/`IROperand` flags (`VT_LOCAL`, `VT_LLOCAL`, `VT_LVAL`, `VT_PARAM`, `pr0_spilled`). This encoding is the source of most materialization bugs — a misset flag causes double-dereferences, missing loads, or wrong offsets.

3. **Scratch register allocation happens too late.** Materialization acquires scratch registers *during* code emission. This means the backend can't plan register usage across an instruction — it discovers conflicts as it emits.

4. **Two operand representations.** `SValue` (legacy) and `IROperand` (compact IR) both need parallel materialization paths. Every fix must be applied twice.

5. **VT_LLOCAL (double indirection) is a symptom.** The entire VT_LLOCAL mechanism exists because materialization can't express "this value is a spilled pointer that needs dereferencing" cleanly. With backend-driven materialization, the backend simply loads what it needs.

## Proposed Architecture

### Core Idea

**Operate on virtual registers throughout IR and codegen. Let the backend decide how and when to materialize physical values.**

```
Current:
  IR → fill_registers() → materialize_*() → emit instructions
       [ir/codegen.c]      [ir/mat.c]        [arm-thumb-gen.c]

Proposed:
  IR → backend dry run → backend real run
       [arm-thumb-gen.c]   [arm-thumb-gen.c]
       (plan allocations)  (emit with known allocations)
```

### Key Principles

1. **IR operands stay virtual.** No `fill_registers()` pass. Operands carry vreg IDs and allocation metadata (physical reg or spill offset) but no VT_LOCAL/VT_LVAL rewriting.

2. **Backend owns materialization.** Each instruction handler in `arm-thumb-gen.c` knows exactly what it needs: "src1 in register", "src2 as immediate or register", "dest in register, store back if spilled". No generic IR-level guessing.

3. **Dry run determines scratch needs.** A first pass over instructions (without emitting) records what physical registers and scratch regs each instruction needs. This feeds register allocation constraints back to the allocator.

4. **Single operand format.** Eliminate the `SValue` path entirely from codegen. All codegen works with `IROperand` + allocation metadata.

## Detailed Design

### Phase 0: Prerequisite — Eliminate SValue from Codegen Path

**Goal:** Remove the `SValue`-based materialization and register fill paths. All backend codegen uses `IROperand` exclusively.

**Files affected:** `ir/codegen.c`, `ir/mat.c`, `arm-thumb-gen.c`

**Steps:**
- Audit all `arm-thumb-gen.c` instruction handlers that still consume `SValue`
- Convert remaining SValue consumers to IROperand
- Remove `tcc_ir_fill_registers()` (SValue version) from `ir/codegen.c`
- Remove `tcc_ir_materialize_value()`, `_const_to_reg()`, `_addr()`, `_dest()` (SValue versions) from `ir/mat.c`

**Risk:** Medium. SValue is deeply embedded in the parser (`tccgen.c`). The boundary is at IR emission — the parser produces SValues, `ir/core.c` converts them to IR instructions with IROperands. We only need to eliminate SValue *after* IR construction.

**Test:** All existing IR tests must pass. This is a pure refactor with no behavior change.

### Phase 1: New Operand Representation — `MachineOperand`

**Goal:** Replace the overloaded `IROperand` flags with a clear machine-level operand type that the backend can interpret without ambiguity.

```c
typedef enum {
    MACH_OP_REG,          /* Value in physical register(s) */
    MACH_OP_SPILL,        /* Value in spill slot, needs load */
    MACH_OP_IMM,          /* Immediate constant */
    MACH_OP_FRAME_ADDR,   /* Address = FP + offset (address-of local) */
    MACH_OP_SYMBOL,       /* Symbol reference (global/extern) */
    MACH_OP_PARAM_STACK,  /* Stack-passed parameter in caller frame */
} MachineOperandKind;

typedef struct {
    MachineOperandKind kind;
    CType type;
    union {
        struct { int r0, r1; }          reg;    /* MACH_OP_REG */
        struct { int offset; int size; } spill;  /* MACH_OP_SPILL */
        struct { int64_t val; }         imm;    /* MACH_OP_IMM */
        struct { int offset; }          frame;  /* MACH_OP_FRAME_ADDR */
        struct { Sym *sym; int addend; } sym;    /* MACH_OP_SYMBOL */
        struct { int offset; int size; } param;  /* MACH_OP_PARAM_STACK */
    } u;
    int vreg;              /* Original vreg (for debug/liveness queries) */
    bool needs_deref;      /* Load through this address (replaces VT_LVAL) */
    bool is_64bit;
} MachineOperand;
```

**Why:** This eliminates the VT_LOCAL/VT_LLOCAL/VT_LVAL/VT_PARAM/pr0_spilled encoding nightmare. Each case is a distinct enum variant. The backend switches on `kind` rather than testing combinations of bit flags.

**Steps:**
- Define `MachineOperand` in a new header (e.g., `ir/machine_op.h`)
- Write `machine_op_from_ir(IROperand *op, IRLiveInterval *interval)` conversion
- This replaces `tcc_ir_fill_registers_ir()` — instead of rewriting IROperand in place, produce a clean MachineOperand

**Test:** Add unit tests that verify MachineOperand construction matches the old fill_registers behavior for all operand categories.

### Phase 2: Backend-Driven Materialization

**Goal:** Move all materialization decisions into `arm-thumb-gen.c` instruction handlers.

**Current pattern in backend (pseudo):**
```c
case TCCIR_OP_ADD: {
    IROperand src1 = inst->src1;
    IROperand src2 = inst->src2;
    IROperand dest = inst->dest;
    tcc_ir_fill_registers_ir(ir, &src1);   // rewrite flags
    tcc_ir_fill_registers_ir(ir, &src2);
    tcc_ir_fill_registers_ir(ir, &dest);
    tcc_ir_materialize_value_ir(ir, &src1, &mat1);  // load if spilled
    tcc_ir_materialize_value_ir(ir, &src2, &mat2);
    tcc_ir_materialize_dest_ir(ir, &dest, &matd);    // get dest reg
    emit_add(dest_reg, src1_reg, src2_reg);
    tcc_ir_storeback_materialized_dest_ir(&dest, &matd);
    tcc_ir_release_materialized_value_ir(&mat1);
    tcc_ir_release_materialized_value_ir(&mat2);
}
```

**Proposed pattern:**
```c
case TCCIR_OP_ADD: {
    MachineOperand src1 = machine_op_from_ir(&inst->src1, ...);
    MachineOperand src2 = machine_op_from_ir(&inst->src2, ...);
    MachineOperand dest = machine_op_from_ir(&inst->dest, ...);

    int r_src1 = mach_ensure_in_reg(ctx, &src1);  // backend loads if needed
    int r_src2 = mach_ensure_in_reg(ctx, &src2);
    int r_dest = mach_get_dest_reg(ctx, &dest);

    emit_add(r_dest, r_src1, r_src2);

    mach_writeback_dest(ctx, &dest, r_dest);       // store if spilled
    mach_release_scratch(ctx);
}
```

**Key `mach_*` helper functions (in arm-thumb-gen.c):**

| Function | Role |
|---|---|
| `mach_ensure_in_reg(ctx, op)` | If `op` is REG: return reg. If SPILL: load to scratch, return scratch. If IMM: mov to scratch. If FRAME_ADDR: compute address. |
| `mach_ensure_in_reg_or_imm(ctx, op)` | For instructions with flexible operand 2 (ADD, SUB, CMP): return reg or encodable immediate |
| `mach_get_dest_reg(ctx, op)` | If dest is REG: return reg. If SPILL: allocate scratch for output. |
| `mach_writeback_dest(ctx, op, reg)` | If dest was SPILL: STR reg to spill slot. |
| `mach_ensure_addr(ctx, op)` | For LOAD/STORE: compute base register + offset. Handles FRAME_ADDR, SPILL (of pointer), PARAM_STACK. |
| `mach_release_scratch(ctx)` | Free scratch registers used in this instruction. |

**Why this is better:**
- Each instruction knows its own addressing modes. ADD can accept an immediate operand2; LOAD needs a base+offset; MUL needs both in registers. The backend expresses this directly.
- No generic "materialize everything to registers before emitting" — only materialize what's needed.
- Scratch register lifetime is explicit and scoped to one instruction.

**Steps:**
1. Implement `MachineCodegenContext` struct holding current instruction index, scratch pool, etc.
2. Implement `mach_ensure_in_reg()` and friends in `arm-thumb-gen.c` (initially wrapping existing `load_to_reg_ir` / `get_scratch_reg_with_save`)
3. Convert instruction handlers one-by-one from old materialize pattern to new pattern
4. After all handlers converted, remove `ir/mat.c` IROperand functions

**Test:** Convert one instruction at a time, run full test suite after each.

### Phase 3: Dry-Run Register Allocation

**Goal:** Run the backend twice — first to discover register/scratch needs, then to emit code with perfect information.

**Why:** Currently, scratch registers are allocated on-the-fly during emission. This can cause conflicts (scratch stomps a live value) that are hard to debug. A dry run lets us:
1. Know exactly which scratch registers each instruction needs
2. Feed scratch constraints back to the linear scan allocator (avoid allocating a vreg to a register that will be needed as scratch)
3. Detect register pressure issues *before* emission

**Design:**

```c
typedef struct {
    int instruction_index;
    int scratch_regs_needed;      /* how many scratch regs this instruction needs */
    int scratch_reg_hints[4];     /* preferred scratch registers (if any) */
    bool needs_pair;              /* needs an even-aligned register pair */
    bool clobbers[16];            /* which physical registers this instruction clobbers */
} InstructionConstraints;
```

**Dry run pass:**
```c
for each IR instruction:
    MachineOperand src1 = machine_op_from_ir(...)
    MachineOperand src2 = machine_op_from_ir(...)
    MachineOperand dest = machine_op_from_ir(...)

    // Instruction handler in "plan" mode:
    constraints[i] = plan_instruction(opcode, src1, src2, dest)
    // e.g., ADD with spilled src1: needs 1 scratch
    // e.g., 64-bit MUL with both spilled: needs 4 scratches
```

**Integration with allocator:**

The dry run produces per-instruction constraints. These are fed to the allocator as "clobber" intervals — the allocator avoids assigning live vregs to registers that will be clobbered at that instruction.

```
Current flow:
  liveness → allocator → fill_registers → materialize → emit

Proposed flow:
  liveness → allocator (initial) → dry run → allocator (refined) → emit
```

The second allocator pass uses clobber information from the dry run to avoid conflicts. In most cases, the initial allocation is fine and the second pass is a no-op.

**Steps:**
1. Add `plan_mode` flag to `MachineCodegenContext`
2. In plan mode, `mach_ensure_in_reg()` records what it *would* do instead of emitting
3. Collect `InstructionConstraints` array
4. Feed constraints to `tcc_ls_allocate_registers()` as additional pressure
5. Run real emission pass with final allocations

**Test:** Verify that dry run + real run produces identical code to current single-pass approach. Then progressively add constraint-aware allocation.

### Phase 4: Eliminate `ir/mat.c`

**Goal:** With all materialization in the backend, remove the IR-level materialization module entirely.

**What moves where:**
- `tcc_ir_materialize_value_ir()` → replaced by `mach_ensure_in_reg()`
- `tcc_ir_materialize_const_to_reg_ir()` → replaced by `mach_ensure_in_reg()` (IMM case)
- `tcc_ir_materialize_addr_ir()` → replaced by `mach_ensure_addr()`
- `tcc_ir_materialize_dest_ir()` → replaced by `mach_get_dest_reg()`
- `tcc_ir_storeback_materialized_dest_ir()` → replaced by `mach_writeback_dest()`
- `tcc_ir_release_materialized_*_ir()` → replaced by `mach_release_scratch()`

**What stays in IR:**
- `ir/live.c` — liveness analysis (unchanged)
- `ir/vreg.c` — virtual register tracking (unchanged)
- `ir/stack.c` — stack layout (simplified, only real locals + spill slots)
- `ir/codegen.c` — reduced to just `machine_op_from_ir()` conversion

**Files deleted:** `ir/mat.c` (entirely)

**Files reduced:** `ir/codegen.c` (from 2331 lines to ~200-300)

### Phase 5: Simplify Stack and Spill Management

**Goal:** With backend-driven materialization, simplify the stack/spill data structures.

**Changes:**
- Remove `TCCMaterializedValue`, `TCCMaterializedAddr`, `TCCMaterializedDest` structs — no longer needed
- Simplify `IROperand` — remove `pr0_spilled`, `pr1_spilled`, `is_local`, `is_llocal` flags (replaced by `MachineOperand::kind`)
- Remove `VT_LLOCAL` handling from backend — `MachineOperand::MACH_OP_SPILL` with `needs_deref=true` handles this case cleanly
- Simplify `TCCStackSlot` — remove `addressable`, `live_across_calls` fields that were only needed for materialization decisions

## Implementation Order and Milestones

### Milestone 1: SValue Elimination (Phase 0)
- **Scope:** ~500 lines removed/refactored in `ir/codegen.c` and `ir/mat.c`
- **Duration estimate:** Smallest, most mechanical change
- **Deliverable:** All codegen uses IROperand. SValue materialization functions deleted.
- **Test gate:** `make test -j16` all pass

### Milestone 2: MachineOperand + Backend Materialization (Phase 1 + Phase 2)
- **Scope:** New `MachineOperand` type, new `mach_*` helpers, convert all instruction handlers
- **Deliverable:** Backend owns all materialization. `ir/mat.c` IROperand functions unused.
- **Test gate:** `make test -j16` + `make test-gcc-torture-compile` all pass

### Milestone 3: Dry Run Pass (Phase 3)
- **Scope:** Dual-pass codegen with constraint collection
- **Deliverable:** Register allocation uses instruction-level scratch constraints
- **Test gate:** Full test suite + manual verification that scratch conflicts are eliminated

### Milestone 4: Cleanup (Phase 4 + Phase 5)
- **Scope:** Delete `ir/mat.c`, simplify data structures, remove dead code
- **Deliverable:** Cleaner, smaller codebase with single materialization path
- **Test gate:** Full test suite + code size comparison

## Risk Analysis

| Risk | Mitigation |
|---|---|
| **Breaking existing tests during migration** | Convert one instruction handler at a time; run tests after each |
| **SValue still used in parser** | SValue stays in `tccgen.c`/`tccpp.c` — we only remove it from codegen path |
| **Dry run diverges from real run** | Assert-check that dry run predictions match real emission |
| **Performance regression from two passes** | Dry run is cheap (no I/O, no encoding); total overhead is small |
| **64-bit / float edge cases** | These are already the buggiest paths; explicit MachineOperand::kind makes them clearer |

## Appendix: Current Bug Categories That This Fixes

1. **Double-dereference bugs:** VT_LVAL set when it shouldn't be (or vice versa). Root cause: `fill_registers()` guessing wrong. Fix: explicit `needs_deref` flag in `MachineOperand`.

2. **Scratch register stomping live value:** Scratch allocated at emit time conflicts with value that's about to be used. Fix: dry run knows all scratch needs upfront.

3. **Stack offset encoding bugs:** Materialization skips load when offset "should be" encodable, but backend disagrees. Fix: backend decides directly — no IR-level guessing about encoding capabilities.

4. **Parameter passing bugs:** VT_PARAM + VT_LOCAL + VT_LVAL combinations are ambiguous. Fix: `MACH_OP_PARAM_STACK` is unambiguous.

5. **64-bit materialization bugs:** Two-register values need coordinated scratch allocation. Fix: `mach_ensure_in_reg()` for 64-bit returns a register pair explicitly.

---

## Phase 5l–5p + Phase 6: Remaining Cleanup

### Current State (post-Phase 5k)

All instruction dispatch in `ir/codegen.c` (both dry-run and real-run) uses the MOP path unconditionally. The only remaining `_op` calls in production code are three control-flow handlers that read raw immediates (no regalloc fields):

| Handler | Call sites | Reads regalloc fields? |
|---|---|---|
| `tcc_gen_machine_jump_op` | 3 (dry×1, real×2) | No — `irop_get_imm32(dest)` only |
| `tcc_gen_machine_conditional_jump_op` | 2 (dry×1, real×1) | No — `src1.u.imm32` + `irop_get_imm32(dest)` |
| `tcc_gen_machine_trap_op` | 2 (dry×1, real×1) | No — takes no arguments |

`fill_registers_ir` and `ir_fill_op` are behind `#ifdef TCC_REGALLOC_DEBUG` — never called in production.

**10 dead `_op` declarations** remain in `tcc.h` (lines 2131–2195) with corresponding dead bodies in `arm-thumb-gen.c`: `load_indexed_op`, `store_indexed_op`, `load_postinc_op`, `store_postinc_op`, `indirect_jump_op`, `switch_table_op`, `setif_op`, `bool_op`, `func_parameter_op`, `vla_op`.

### Phase 5l: Remove `pr0_spilled` / `pr1_spilled` from `IROperand` — ✅ DONE

**Completed:** 2026-03-05

**What was done:**
- Replaced `pr0_spilled : 1` and `pr1_spilled : 1` with `_reserved0 : 1` and `_reserved1 : 1` in `IROperand` struct (`tccir_operand.h`) to maintain 10-byte packed layout
- Removed all `.pr0_spilled` / `.pr1_spilled` writes/reads from `IROperand` usage sites:
  - `arm-thumb-gen.c`: `load_to_dest_ir`, `load_to_reg_ir`, and dead `_op` functions — simplified conditional logic that checked spill flags (all live callers already passed 0)
  - `ir/codegen.c`: removed writes in `fill_registers_ir` (debug-only), removed `spill=%d` from debug trace format
  - `tccir_operand.c`: removed copies in `irop_copy_svalue_info`, set SValue fields to 0 in `irop_to_svalue` (SValue retains its own `pr0_spilled`/`pr1_spilled`), removed spill comparisons from validation function
  - `arm-thumb-asm.c`: removed 6 spill-flag assignments in inline asm codegen (`asm_gen_code`)
  - `tccir_operand.h`: updated `IROP_NONE` macro and `irop_init_phys_regs`

**Files modified:** `tccir_operand.h`, `tccir_operand.c`, `arm-thumb-gen.c`, `ir/codegen.c`, `arm-thumb-asm.c`

**Test result:** 3310 passed, 79 skipped, 582 xfailed — no regressions.

**Reclaimed bits:** 2 bits freed in the packed struct (currently `_reserved0`/`_reserved1`).

### Phase 5m: Delete `fill_registers_ir` Entirely — ✅ DONE

**Completed:** 2026-03-05

**What was deleted (~256 lines):**
- `tcc_ir_fill_registers_ir()` body (~157 lines) + header comment from `ir/codegen.c`
- `ir_fill_op()` wrapper (~8 lines) from `ir/codegen.c`
- `_dbg_trace_all` variable + function name matching block (~25 lines) from `ir/codegen.c`
- Main debug trace block calling `ir_fill_op` for `trc_s1/s2/d` (~60 lines, including LOAD/AND/OR/ASSIGN diagnostics) from `ir/codegen.c`
- Declaration + comment (6 lines) from `tccir.h`
- Stale comments referencing `fill_registers_ir` / `ir_fill_op` in both dry-run and real-run dispatch loops

**Files modified:** `ir/codegen.c`, `tccir.h`

**Note:** The `#ifdef TCC_REGALLOC_DEBUG` vreg statistics block and `[RA-PEEPHOLE]` trace were kept — they don't depend on `fill_registers_ir`.

**Test result:** 3310 passed, 79 skipped, 582 xfailed — no regressions. Also verified clean build with `CFLAGS+='-DTCC_REGALLOC_DEBUG'`.

### Phase 5n: Delete Dead `_op` Declarations and Bodies ✅ DONE

**Goal:** Remove the 10 dead `_op` function declarations from `tcc.h` and their corresponding bodies from `arm-thumb-gen.c`.

**Deleted functions:**

| Function | Location |
|---|---|
| `tcc_gen_machine_load_indexed_op` | tcc.h decl + arm-thumb-gen.c body |
| `tcc_gen_machine_store_indexed_op` | tcc.h decl + arm-thumb-gen.c body |
| `tcc_gen_machine_load_postinc_op` | tcc.h decl + arm-thumb-gen.c body |
| `tcc_gen_machine_store_postinc_op` | tcc.h decl + arm-thumb-gen.c body |
| `tcc_gen_machine_indirect_jump_op` | tcc.h decl + arm-thumb-gen.c body |
| `tcc_gen_machine_switch_table_op` | tcc.h decl + arm-thumb-gen.c body |
| `tcc_gen_machine_setif_op` | tcc.h decl + arm-thumb-gen.c body |
| `tcc_gen_machine_bool_op` | tcc.h decl + arm-thumb-gen.c body |
| `tcc_gen_machine_func_parameter_op` | tcc.h decl + arm-thumb-gen.c body |
| `tcc_gen_machine_vla_op` | tcc.h decl + arm-thumb-gen.c body |

Also deleted 2 now-unused static helpers: `thumb_irop_has_immediate_value`, `thumb_irop_needs_value_load`.

**Net reduction:** ~700 lines from `arm-thumb-gen.c`, 10 declarations from `tcc.h`.

**Test result:** 3310 passed, 79 skipped, 582 xfailed — no regressions.

### Phase 5o: Convert Control-Flow `_op` Handlers to `_mop` ✅ DONE

**Goal:** Convert the last 3 `_op` handlers to `_mop` so the dispatch loop is 100% MOP.

**Converted:**

| Old | New | Change |
|---|---|---|
| `tcc_gen_machine_jump_op(TccIrOp, IROperand, int)` | `tcc_gen_machine_jump_mop(TccIrOp, int32_t target_ir, int)` | Extract `irop_get_imm32(dest)` at call site |
| `tcc_gen_machine_conditional_jump_op(IROperand, TccIrOp, IROperand, int)` | `tcc_gen_machine_conditional_jump_mop(int32_t cond, TccIrOp, int32_t target_ir, int)` | Extract `src.u.imm32` and `irop_get_imm32(dest)` at call site |
| `tcc_gen_machine_trap_op(void)` | `tcc_gen_machine_trap_mop(void)` | Rename only (no IROperand args) |

**Files changed:** `tcc.h` (declarations), `arm-thumb-gen.c` (bodies), `ir/codegen.c` (5 call sites in dry-run + real-run loops).

**Result:** All backend dispatch call sites now use `_mop` variants or pass extracted scalars. No `IROperand` is passed to any backend handler.

**Test result:** 3310 passed, 79 skipped, 582 xfailed — no regressions.

### Phase 5p: Remove `pr0_reg` / `pr1_reg` from `IROperand`

**Goal:** Eliminate the physical register fields from `IROperand`. These were filled by `fill_registers_ir` and read by the old `_op` backend path. With both gone, the dispatch path no longer needs them.

**Investigation findings (2026-03-06):**

A comprehensive audit revealed **50+ live references** to `pr0_reg`/`pr1_reg` across the codebase, far more than the original estimate of 3 readers:

| Reader/Writer | File | Nature |
|---|---|---|
| `machine_op_from_ir` vreg=-1 path | `ir/machine_op.c` L167–177 | **Critical:** pinned physical register for vreg=-1 operands |
| `load_to_dest_ir` | `arm-thumb-gen.c` L3416+ | ~38 reads, 3 writes — live for inline asm + VLA |
| `store_ex_ir` | `arm-thumb-gen.c` L2622+ | ~10 reads — live for inline asm |
| `th_store_resolve_base_ir` | `arm-thumb-gen.c` L2508+ | 2 reads — live for inline asm |
| `load_to_reg_ir` | `arm-thumb-gen.c` L3745+ | 2 writes — live for inline asm |
| `asm_gen_code` | `arm-thumb-asm.c` L254+ | 6 writes — constructs IROperands with `pr0_reg` |
| `svalue_to_iroperand` Case 1/1b | `tccir_operand.c` L343/359 | Writes `pr0_reg = val_kind` from `sv->r & VT_VALMASK` |
| `iroperand_to_svalue` | `tccir_operand.c` L655 | Reads `op.pr0_reg` back to SValue |
| `irop_copy_svalue_info` | `tccir_operand.c` L298 | Copies `sv->pr0_reg` → `op->pr0_reg` |
| `tcc_ir_fill_registers` | `ir/codegen.c` L21+ | Writes `sv->pr0_reg` from interval (inline asm only) |

**Root cause discovery:** `tcc_ir_put()` clears `sv->pr0_reg = PREG_REG_NONE` before calling `svalue_to_iroperand()`, but `svalue_to_iroperand()` Case 1b **re-derives** `result.pr0_reg = val_kind` from `sv->r & VT_VALMASK`. So the clearing is ineffective for vreg=-1 operands with a physical register. Three GCC torture tests (pr41239, pr46309, pr58831) confirmed the vreg=-1 path with `pr0_reg≠PREG_REG_NONE` is live.

**Approach taken (Option 3: encode in `u.imm32`):**

Rather than plumbing interval entries for all vreg=-1 creation sites, we encode the pinned physical register in `u.imm32` for IROP_TAG_VREG operands:

- Defines: `IROP_VREG_PHYS_VALID` (0x100, validity flag) and `IROP_VREG_PHYS_MASK` (0x1F, register number) in `tccir_operand.h`
- `svalue_to_iroperand()` Case 1b (vreg=-1): sets `result.u.imm32 = IROP_VREG_PHYS_VALID | (val_kind & IROP_VREG_PHYS_MASK)`
- `machine_op_from_ir()` vreg=-1 path: reads `op->u.imm32` instead of `op->pr0_reg`

**Important:** Case 1 (vr >= 0) must **NOT** set `u.imm32` — `load_to_dest_ir()` uses `u.imm32 != 0` on VREG operands for sub-component access (complex imaginary part). Setting it caused GCC torture test 20030222-1 to fail: inline asm `"=r" (int_out) : "0" (long_long_in)` loaded the high word instead of the low word.

**Status:** ✅ Complete. The `pr0_reg`/`pr1_reg` fields have been removed from `IROperand`. The struct is now 9 bytes (down from 10). All legacy `_ir` functions use `irop_phys_r0()`/`irop_phys_r1()` helpers that read physical registers from the interval table. The `load_to_dest_ir` signature was changed to `(int dest_r0, int dest_r1, IROperand src)`. The `arm-thumb-asm.c::asm_gen_code` was updated to pass explicit register args. `tccir_operand.c` conversion functions no longer copy pr0/pr1. `irop_init_phys_regs()` was deleted. Remaining IROperand flags repacked into a single byte: `is_unsigned:1, is_static:1, is_sym:1, is_param:1, _pad:4`.

**Completed steps:**
1. ✅ Added `irop_phys_r0()`/`irop_phys_r1()` helpers in `arm-thumb-gen.c` — read interval table or IROP_VREG_PHYS encoding
2. ✅ Converted `load_to_dest_ir` signature to `(int dest_r0, int dest_r1, IROperand src)` — removed dead spilled-dest path
3. ✅ Converted `store_ex_ir`/`th_store_resolve_base_ir` to use `irop_phys_r0()`/`irop_phys_r1()`
4. ✅ Updated `arm-thumb-asm.c::asm_gen_code` to pass explicit register args
5. ✅ Updated `tccir_operand.c` — removed pr0/pr1 from `irop_copy_svalue_info`, `svalue_to_iroperand`, `iroperand_to_svalue`, `irop_compare_svalue`
6. ✅ Removed `pr0_reg:5`, `pr1_reg:5`, `_reserved0:1`, `_reserved1:1` from `IROperand` — struct shrunk to 9 bytes
7. ✅ Removed dead pr0_reg/pr1_reg init writes from `ir/core.c`
8. ✅ Updated test `bug_packed10_array` for 9-byte layout

**Dependency:** Phase 5m (delete `fill_registers_ir`) and Phase 5n (delete dead `_op` functions) — both done.

### Phase 5q: Delete Legacy `_ir` Wrappers + Rewrite `tcc_gen_mach_load_to_reg` (COMPLETED)

**What was done:**

Deleted all remaining legacy `_ir` wrapper functions from `arm-thumb-gen.c` (~560 lines) and rewrote `tcc_gen_mach_load_to_reg` for correctness.

**Functions deleted:**

| Function | ~Lines | Role |
|----------|--------|------|
| `load_to_dest_ir` | 268 | Legacy IROperand-based load (read pr0_reg/pr1_reg from interval) |
| `store_ex_ir` | 170 | Legacy IROperand-based store |
| `store_ir` | 3 | Thin wrapper around `store_ex_ir` |
| `th_store_resolve_base_ir` | 114 | Legacy base-resolution for stores |
| `irop_phys_r0` / `irop_phys_r1` | 47 | Interval-table helpers (only used by `_ir` functions) |
| `th_store32_imm_or_reg` | 5 | Became unused after `store_ex_ir` deletion |
| Forward declarations | 3 | Stale declarations for deleted functions |

Also deleted: `irop_phys_r0`/`irop_phys_r1` helper forward declarations.

**`tcc_gen_mach_load_to_reg` rewrite:**

The original 6-line implementation used `mach_ensure_in_reg` which allocates a scratch register. When inline asm loads multiple operands sequentially, the scratch for operand N could clobber operand N-1's already-loaded register (pr49390 regression).

Rewritten as a ~105-line switch covering all `MachineOperandKind` values, loading directly into `dest_reg`:

| Kind | Strategy |
|------|----------|
| `MACH_OP_REG` | `mov dest, src` (or deref via `load_from_base`) |
| `MACH_OP_SPILL` | `load_spill_slot` (with LLOCAL double-deref) |
| `MACH_OP_IMM` | `load_constant` directly into dest |
| `MACH_OP_FRAME_ADDR` | `addr_of_stack_slot` directly into dest |
| `MACH_OP_SYMBOL` | Direct load/deref; scratch via `get_scratch_reg_with_save` excluding dest |
| `MACH_OP_PARAM_STACK` | `load_from_base` from SP |
| `MACH_OP_CHAIN_REL` | `resolve_chain_base` + `load_from_base` |

Key property: **no scratch register can clobber `dest_reg`** — scratch allocation explicitly excludes `dest_reg` when needed.

**Results:**
- `arm-thumb-gen.c`: 8578 → 8055 lines (−523)
- All 3310 tests pass, 0 failed
- Inline asm operand sequential loading works correctly (pr49390 fixed)

### Phase 6: Consolidate `ir/codegen.c`

**Goal:** Reduce `ir/codegen.c` from 2362 lines to ~1400–1600 by removing structural duplication between the dry-run and real-run dispatch loops.

**Current structure (as of 2026-03-06):**

```
Lines 1–16:       Header, includes
Lines 17–190:     tcc_ir_fill_registers (SValue, used by inline asm only)
Lines 188–382:    tcc_ir_register_allocation_params
Lines 382–723:    Helper functions (branch optimization, stack layout)
Lines 723–860:    Inline asm codegen helper (tcc_ir_codegen_inline_asm_ir)
Lines 860–1059:   try_reassign_scratch_conflict, has_incoming_jump analysis
Lines 1059–1160:  tcc_ir_codegen_generate() entry, stack_size computation
Lines 1160–1693:  DRY-RUN PASS (dispatch loop L1210–L1628, ~420 lines of switch cases)
Lines 1693–1710:  Inter-pass: prologue gen, debug prolog
Lines 1710–2350:  REAL-RUN PASS (dispatch loop L1730–2320, ~590 lines of switch cases)
Lines 2350–2363:  Cleanup, backpatch, epilogue
```

The dry-run loop is ~420 lines and the real-run loop is ~590 lines. The real-run is larger because it includes:
1. `#ifdef TCC_LS_DEBUG` scratch consistency checks (~120 lines across all ops)
2. `ir_to_code_mapping[i]` updates for JUMP/JUMPIF
3. `tcc_ir_spill_cache_clear()` calls after branches, calls, and inline asm
4. SWITCH_TABLE: dry-run computes `ind += size`, real-run calls `tcc_gen_machine_switch_table_mop`
5. RETURNVOID: dry-run does nothing, real-run emits jump-to-epilogue
6. FUNCCALLVOID: real-run sets `drop_return_value = 1` via fallthrough
7. INLINE_ASM: dry-run skips via `continue`, real-run calls `tcc_ir_codegen_inline_asm_ir`
8. `before_ret` peephole: identical in both loops but duplicated (LOAD/LOAD_INDEXED/ASSIGN)

**Strategy: Unified dispatch with mode flag**

```c
for (int pass = 0; pass < 2; pass++) {
    bool is_dry_run = (pass == 0);
    if (pass == 1) {
        /* inter-pass: prologue, debug, branch optimization */
    }

    for (int i = 0; i < ir->next_instruction_index; i++) {
        IROperand src1_ir = tcc_ir_op_get_src1(ir, cq);
        // ... operand extraction ...
        // ... before_ret peephole (shared) ...

        switch (cq->op) {
        case TCCIR_OP_ADD: ... {
            MachineOperand mop_src1 = machine_op_from_ir(ir, &src1_ir);
            // ... same handler call ...
            if (is_dry_run) {
                dry_insn_scratch[i] = tcc_gen_machine_insn_scratch_count();
                dry_insn_saves[i] = tcc_gen_machine_insn_scratch_saves_mask();
            }
            break;
        }
        case TCCIR_OP_JUMP:
            tcc_gen_machine_jump_mop(cq->op, irop_get_imm32(dest_ir), i);
            if (!is_dry_run) {
                ir_to_code_mapping[i] = ind - (...);
                tcc_ir_spill_cache_clear(&ir->spill_cache);
            }
            break;
        // ...
        }
        tcc_gen_machine_end_instruction();
    }
}
```

**Detailed differences between loops (audit):**

| Op | Dry-run | Real-run | Merge strategy |
|---|---|---|---|
| Most MOP ops (DP, LOAD, STORE, ...) | call handler + record scratch | call handler + `#ifdef TCC_LS_DEBUG` check | Shared; `if (is_dry_run)` for scratch recording |
| SWITCH_TABLE | `ind += 14 + table_data_size` | `tcc_gen_machine_switch_table_mop()` | `if (is_dry_run) ind += ...; else switch_table_mop()` |
| RETURNVOID | `break` (no-op) | emit jump to epilogue | `if (!is_dry_run) { ... }` |
| FUNCCALLVOID | no fallthrough to FUNCCALLVAL | `drop_return_value = 1` + fallthrough | Use explicit flag instead of fallthrough |
| JUMP/JUMPIF | `tcc_gen_machine_jump_mop()` | same + `ir_to_code_mapping` update + `spill_cache_clear` | `if (!is_dry_run) { mapping; cache_clear; }` |
| INLINE_ASM | `continue` (skipped) | `tcc_ir_codegen_inline_asm_ir()` + `spill_cache_clear` | `if (!is_dry_run) { ... }` |
| ASM_INPUT/OUTPUT/NOP | `continue` | `break` | Normalize to `continue` or `break` |
| Loop preamble | no `ir_to_code_mapping`, no `tcc_debug_line_num`, no `codegen_materialize_scratch_flags` | all of these | `if (!is_dry_run) { ... }` |
| `before_ret` peephole | Identical to real-run | Identical to dry-run | Shared |

**Sub-steps:**

#### 6a: Normalize loop preambles

The real-run loop has extra per-iteration setup:
- `ir_to_code_mapping[i] = ind`
- `orig_ir_to_code_mapping[cq->orig_index] = ind`
- `tcc_debug_line_num(tcc_state, cq->line_num)`
- `ir->codegen_materialize_scratch_flags = 0`

Wrap these in `if (!is_dry_run)`. The dry-run loop doesn't do debug line emission or mapping updates — it only needs `ir_to_code_mapping[i] = ind` for branch offset analysis (already present).

#### 6b: Extract `before_ret` peephole into helper

The LOAD/LOAD_INDEXED/ASSIGN `before_ret` peephole is ~30 lines duplicated 3× in each loop (6× total). Extract:

```c
static bool ir_codegen_check_before_ret(TCCIRState *ir, int i, IROperand *dest_ir,
                                         const uint8_t *has_incoming_jump)
```

Returns bool and patches interval + constructs synthetic MOP dest.

#### 6c: Extract shared dispatch into function

Create `ir_codegen_dispatch_one(TCCIRState *ir, int i, bool is_dry_run, ...)` containing the switch. Both loops call it.

#### 6d: Merge into single outer loop

Replace `#if 1 /* DRY_RUN_ENABLED */ ... #endif ... /* REAL RUN */` with:

```c
for (int pass = 0; pass < 2; pass++) {
    bool is_dry_run = (pass == 0);
    if (pass == 0) { /* dry-run init */ }
    if (pass == 1) { /* inter-pass: fixup, prologue, restore */ }
    for (int i = 0; ...) {
        ir_codegen_dispatch_one(ir, i, is_dry_run, ...);
    }
    if (pass == 0) { /* dry-run end, branch analysis, scratch fixup */ }
}
```

#### 6e: Clean up `#ifdef TCC_LS_DEBUG` scratch checks

The ~120 lines of `#ifdef TCC_LS_DEBUG` scratch consistency checks only run in the real-run pass. Factor into a single helper:

```c
static inline void ir_codegen_check_scratch(int i, TccIrOp op, int *dry_scratch, uint16_t *dry_saves)
{
#ifdef TCC_LS_DEBUG
    int real_scratch = tcc_gen_machine_insn_scratch_count();
    if (real_scratch != dry_scratch[i] && dry_saves[i] == 0)
        fprintf(stderr, "[insn-scratch] i=%d op=%d dry=%d real=%d MISMATCH\n", i, (int)op, dry_scratch[i], real_scratch);
#endif
}
```

Call at the end of each op's case in the unified dispatch.

**Actual result (Phase 6 ✅ Done):**
- `ir/codegen.c`: 2106 → 1767 lines (−339 lines, ~16%)
- Single source of truth for dispatch logic
- Adding a new IR op means adding one `case`, not two
- `before_ret` peephole logic in one place instead of six
- Four extracted helpers: `ir_codegen_before_ret_peephole()`, `ir_codegen_record_scratch()`, `ir_codegen_check_scratch()`, `ir_codegen_track_scratch()`

**Risks (all resolved):**

1. **SWITCH_TABLE** — dry-run computes size arithmetically; real-run emits via handler. The handler must still produce the same `ind` advance. Can be verified with an assert.
2. **RETURNVOID jump-to-epilogue** — only needed in real-run. Simple `if (!is_dry_run)` guard.
3. **`ir_to_code_mapping` / `orig_ir_to_code_mapping`** — only meaningful in real-run. Must not be written to in dry-run (would corrupt saved state).
4. **`spill_cache_clear` after branches/calls** — no-op semantics in dry-run (cache was cleared at start). Can safely call in both passes or guard.

**Mitigation:** Do this incrementally:
1. First, extract `before_ret` peephole helper (6b) — low risk, high dedup value
2. Extract `ir_codegen_check_scratch` helper (6e) — mechanical, reduces noise
3. Extract shared dispatch function (6c) — verifiable by running both paths
4. Merge loops (6d) — final step, requires full test suite validation

**Test:** After each sub-step: `make clean && make cross && make test -j16 && make test-all`

## Updated Implementation Order

| Step | Phase | Status | Scope | Est. lines changed | Dependency |
|---|---|---|---|---|---|
| 1 | **5l** | ✅ Done | Remove `pr0_spilled`/`pr1_spilled` | ~20 lines | None |
| 2 | **5m** | ✅ Done | Delete `fill_registers_ir` (production) | ~256 lines deleted | 5l |
| 3 | **5n** | ✅ Done | Delete 10 dead `_op` declarations + bodies | ~700 lines deleted | None |
| 4 | **5o** | ✅ Done | Convert jump/conditional_jump/trap to `_mop` | ~60 lines changed | 5n |
| 5 | **5p** | ✅ Done | Decouple `machine_op_from_ir` from `pr0_reg`; add `irop_phys_r0/r1` helpers; remove fields from `IROperand` (10→9 bytes); update all callers | ~200 lines changed | 5m + 5o |
| 5 | **5q** | ✅ Done | Delete all legacy `_ir` wrappers (~560 lines); rewrite `tcc_gen_mach_load_to_reg` for direct-dest loading; fix inline asm operand clobber (pr49390) | ~560 lines deleted, ~105 lines added | 5p |
| 6 | **6a** | ✅ Done | Normalize loop preambles | ~30 lines | None |
| 7 | **6b** | ✅ Done | Extract `before_ret` peephole helper | ~120 lines deduped | None |
| 8 | **6c** | ✅ Done | Extract scratch record/check helpers | ~120 lines deduped | None |
| 9 | **6d** | ✅ Done | Merge into single `for (pass=0; pass<2)` loop | ~339 lines saved | 6a+6b+6c |

**Total expected line reduction from remaining work:** ~1000–1200 lines across all files.

### Current file sizes (2026-03-06)

| File | Lines | Notes |
|---|---|---|
| `ir/codegen.c` | 1767 | Single unified two-pass dispatch loop (`for (pass=0; pass<2)`) |
| `arm-thumb-gen.c` | 8055 | All legacy `_ir` functions deleted; `tcc_gen_mach_load_to_reg` rewritten for direct-dest loading |
| `arm-thumb-asm.c` | 3539 | Inline asm path fully on MOP via `tcc_gen_mach_load_to_reg`/`tcc_gen_mach_store_from_reg` |
| `ir/machine_op.c` | 328 | `machine_op_from_ir()` — reads interval table directly |
| `tccir_operand.h` | 560 | `IROperand` = 9 bytes; `pr0_reg`/`pr1_reg` removed |
| `tccir_operand.c` | 844 | SValue↔IROperand conversions updated (no pr0/pr1 copy) |
| `arm-thumb-callsite.c` | 322 | Callsite arg-handling fully on MOP |
| `ir/core.c` | 1951 | Removed dead `pr0_reg`/`pr1_reg` init writes |

## Updated Risk Analysis

| Risk | Mitigation |
|---|---|
| **~~`IROperand` struct size change breaks packed layout~~** | ✅ Resolved — `sizeof(IROperand)` = 9 bytes; `_Static_assert` updated; test `bug_packed10_array` updated to 9-byte layout |
| **~~vreg=-1 interval plumbing incomplete (Phase 5p)~~** | ✅ Resolved — `IROP_VREG_PHYS` encoding used by both `machine_op_from_ir` and `irop_phys_r0()` |
| **~~Dispatch loop merge (Phase 6) introduces subtle ordering bugs~~** | ✅ Resolved — merge completed successfully; all 3310 tests pass |
| **`is_local`/`is_llocal`/`is_param` still needed by IR optimizations** | These fields stay — they are IR-semantic. Only codegen-time _mutation_ is gone (`fill_registers_ir` deleted). The fields remain read-only during codegen via `machine_op_from_ir`. |
| **~~SWITCH_TABLE dry-run vs real-run divergence~~** | ✅ Resolved — unified loop handles both passes correctly |
| **Debug builds (`TCC_REGALLOC_DEBUG`) broken** | Replace deleted debug trace with MachineOperand dump; test with `make cross CFLAGS+='-DTCC_REGALLOC_DEBUG'` |
