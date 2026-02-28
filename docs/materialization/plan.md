# Materialization Refactor: Move from IR to Machine Backend

## Current Status (as of 2026-03-03)

| Phase | Status | Commit |
|-------|--------|--------|
| 0: SValue Elimination | ✅ Done | `e19755e6` |
| 1: MachineOperand type | 🔄 Partial — type+conversion done; `fill_registers_ir` still prereq | unstaged (`ir/machine_op.c`) |
| 2: Backend materialization | 🔄 Partial — data-processing ops + ASSIGN/SETIF/BOOL on MOP path | unstaged |
| 3: Dry-run integration | ✅ Done — scratch conflict fixup + R_FP exclusion | `c2569883` |
| 4: Eliminate `ir/mat.c` | ✅ Done — `ir/mat.c`, `ir/operand.c`, `ir/operand.h` deleted | `bc43b639` |
| 5: Simplify stack/spill | 🔄 Partial — dead `TCCStackSlot` fields removed; IROperand flags remain | `0e772abb` |

**Next:** Extend MOP path to LOAD/STORE, then MUL/LEA/JUMP/CALL.

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
