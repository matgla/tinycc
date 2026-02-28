# Materialization Refactor: Overview

## Problem Statement

The current materialization layer (`ir/mat.c`, `ir/codegen.c`) sits between the IR and the backend (`arm-thumb-gen.c`), creating a tangled intermediate abstraction:

1. **Materialization duplicates backend logic.** `ir/mat.c` decides when to load spills, how to handle constants, when addresses are encodable, etc. But the backend *also* makes these decisions (via `load_to_reg_ir`, `get_scratch_reg_with_save`, `tcc_machine_can_encode_stack_offset`). The two layers constantly second-guess each other.

2. **Register fill is fragile.** `ir/codegen.c:tcc_ir_fill_registers_ir()` translates allocation results back into `IROperand` flags (`is_local`, `is_llocal`, `is_lval`, `is_param`, `pr0_spilled`). This encoding is the source of most materialization bugs — a misset flag causes double-dereferences, missing loads, or wrong offsets.

3. **Scratch register allocation happens too late.** Materialization acquires scratch registers *during* code emission. This means the backend can't plan register usage across an instruction — it discovers conflicts as it emits.

4. **Two operand representations.** `SValue` (legacy) and `IROperand` (compact IR) both need parallel materialization paths. Every fix must be applied twice.

5. **VT_LLOCAL (double indirection) is a symptom.** The entire VT_LLOCAL mechanism exists because materialization can't express "this value is a spilled pointer that needs dereferencing" cleanly. With backend-driven materialization, the backend simply loads what it needs.

## Proposed Architecture

### Core Idea

**Operate on virtual registers throughout IR and codegen. Let the backend decide how and when to materialize physical values.**

```
Current:
  IR → fill_registers_ir() → materialize_*_ir() → tcc_gen_machine_*_op() → emit instructions
       [ir/codegen.c]         [ir/mat.c]           [arm-thumb-gen.c]

Proposed:
  IR → machine_op_from_ir() → tcc_gen_machine_*_op() → mach_ensure_in_reg() → emit
       [ir/codegen.c, thin]    [arm-thumb-gen.c]        [arm-thumb-gen.c]
```

### Key Principles

1. **IR operands stay virtual.** No `fill_registers()` pass. Operands carry vreg IDs and allocation metadata (physical reg or spill offset) but no `is_local`/`is_lval` rewriting.

2. **Backend owns materialization.** Each instruction handler in `arm-thumb-gen.c` knows exactly what it needs: "src1 in register", "src2 as immediate or register", "dest in register, store back if spilled". No generic IR-level guessing.

3. **Dry run determines scratch needs.** A first pass over instructions (without emitting) records what physical registers and scratch regs each instruction needs. This feeds register allocation constraints back to the allocator. *(Note: a dry-run pass already exists in `ir/codegen.c` — this phase extends it.)*

4. **Single operand format.** Eliminate the `SValue` path entirely from codegen. All codegen works with `IROperand` + allocation metadata via `MachineOperand`.

## Phase Summary

| Phase | Title | Scope | Status | Details |
|-------|-------|-------|--------|---------|
| 0 | SValue Elimination | Remove SValue-based materialization from codegen | ✅ **DONE** (`e19755e6`) | [01_phase0_svalue_elimination.md](01_phase0_svalue_elimination.md) |
| 1 | MachineOperand Type | New unambiguous operand representation | 🔄 **Partial** — type + `machine_op_from_ir()` done; `fill_registers_ir` still prereq | [02_phase1_machine_operand.md](02_phase1_machine_operand.md) |
| 2 | Backend-Driven Materialization | Move all materialization into `arm-thumb-gen.c` | 🔄 **Partial** — data-processing ops + ASSIGN/SETIF/BOOL | [03_phase2_backend_materialization.md](03_phase2_backend_materialization.md) |
| 3 | Dry-Run Integration | Extend existing dry-run with constraint collection | ✅ **DONE** (`c2569883`) | [04_phase3_dry_run.md](04_phase3_dry_run.md) |
| 4 | Eliminate `ir/mat.c` | Delete IR-level materialization module | ✅ **DONE** (`bc43b639`) | [05_phase4_eliminate_mat.md](05_phase4_eliminate_mat.md) |
| 5 | Simplify Stack/Spill | Clean up data structures | 🔄 **Partial** — dead `TCCStackSlot` fields removed; IROperand flags remain | [06_phase5_simplify_stack.md](06_phase5_simplify_stack.md) |

## Implementation Order and Milestones

### Milestone 1: SValue Elimination (Phase 0) — ✅ COMPLETE
- **Scope:** ~400 lines removed from `ir/codegen.c` and `ir/mat.c`
- **Deliverable:** All codegen uses IROperand. SValue materialization functions deleted.
- **Commit:** `e19755e6 new materialization plan`

### Milestone 2: MachineOperand + Backend Materialization (Phase 1 + Phase 2) — 🔄 IN PROGRESS
- **Scope:** `MachineOperand` type and `machine_op_from_ir()` complete; `tcc_gen_machine_data_processing_mop()` complete for 11 data-processing ops; `tcc_gen_machine_assign_mop()` (REG-only dest), `tcc_gen_machine_setif_mop()`, `tcc_gen_machine_bool_mop()` complete.
- **Remaining:** Extend MOP path to LOAD, STORE, MUL, LEA, JUMP, CALL, FP ops. Remove `fill_registers_ir` dependency once all semantic transforms replicated in `machine_op_from_ir`.
- **Note:** ASSIGN MOP is restricted to REG-only destinations (spill/param dest falls back to old `assign_op`) because `mach_writeback_dest` doesn't replicate `is_param`-aware `fp_adjust_local_offset` from the old path.
- **Key constraint:** `fill_registers_ir` must keep running until `machine_op_from_ir` replicates all its semantic transforms (lval propagation, VLA delta offsets, struct ctype_idx encoding, param area detection).
- **Test gate:** `make test -j16` all pass

### Milestone 3: Dry Run Integration (Phase 3) — ✅ COMPLETE
- **Scope:** Dual arrays `dry_insn_scratch[]`/`dry_insn_saves[]`, `try_reassign_scratch_conflict()` with R_FP+static_chain exclusion.
- **Deliverable:** Scratch conflicts resolved by reassigning vregs to callee-saved registers in a fixup pass.
- **Commit:** `c2569883 phase 3: enable dry-run scratch conflict fixup`

### Milestone 4: Cleanup (Phase 4 + Phase 5) — Phase 4 ✅ COMPLETE, Phase 5 🔄 Partial
- **Phase 4 done:** `ir/mat.c`, `ir/operand.c`, `ir/operand.h` deleted (`bc43b639`). `ir/machine_op.c` / `ir/machine_op.h` are the replacement.
- **Phase 5 done:** Dead `TCCStackSlot` fields removed (`0e772abb`).
- **Phase 5 remaining:** `pr0_reg`, `pr0_spilled`, `is_llocal` still in `tccir_operand.h` (blocked until `fill_registers_ir` removed); `tccir_operand.h` deduplication pending.

## Risk Analysis

| Risk | Mitigation |
|---|---|
| **Breaking existing tests during migration** | Convert one instruction handler at a time; run tests after each |
| **SValue still used in parser** | SValue stays in `tccgen.c`/`tccpp.c` — we only remove it from codegen path |
| **Dry run diverges from real run** | Assert-check that dry run predictions match real emission |
| **Performance regression from two passes** | Dry run is already implemented and cheap |
| **64-bit / float edge cases** | These are already the buggiest paths; explicit MachineOperand::kind makes them clearer |

## Review Notes

See [review.md](review.md) for a detailed review of this plan against the actual codebase state.
