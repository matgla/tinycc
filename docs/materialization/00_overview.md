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
| 1 | MachineOperand Type | New unambiguous operand representation | ✅ **Done** — type + `machine_op_from_ir()` done; `machine_op_from_ir` decoupled from `pr0_reg` via `IROP_VREG_PHYS` encoding; 8 `MachineOperand` kinds cover all cases | [02_phase1_machine_operand.md](02_phase1_machine_operand.md) |
| 2 | Backend-Driven Materialization | Move all materialization into `arm-thumb-gen.c` | ✅ **Complete** — All convertible ops have MOP handlers; `!irop_needs_pair` guards removed for DP, ASSIGN, BOOL, LOAD, FUNC_CALL (64-bit pair sources handled via `mach_resolve_deref_64`); RETURNVALUE supports 64-bit; JUMP/JUMPIF and LEA intentionally on old path | [03_phase2_backend_materialization.md](03_phase2_backend_materialization.md) |
| 3 | Dry-Run Integration | Extend existing dry-run with constraint collection | ✅ **DONE** (`c2569883`) | [04_phase3_dry_run.md](04_phase3_dry_run.md) |
| 4 | Eliminate `ir/mat.c` | Delete IR-level materialization module | ✅ **DONE** (`bc43b639`) | [05_phase4_eliminate_mat.md](05_phase4_eliminate_mat.md) |
| 5 | Simplify Stack/Spill | Clean up data structures | ✅ **Done** — Phases 5b–5q ✅; `pr0_spilled`/`pr1_spilled` removed; `fill_registers_ir` deleted (~256 lines); 10 dead `_op` declarations + bodies removed (~700 lines); JUMP/JUMPIF/TRAP converted to `_mop`; `pr0_reg`/`pr1_reg` fields removed from `IROperand` (10→9 bytes); all legacy `_ir` wrappers deleted (~560 lines); `tcc_gen_mach_load_to_reg` rewritten for direct-dest loading; inline asm path fully on MOP | [06_phase5_simplify_stack.md](06_phase5_simplify_stack.md) |
| 6 | Consolidate Dispatch | Merge dry-run/real-run loops into single parameterised pass | ✅ **Done** — merged into single `for (pass=0; pass<2)` loop; `ir/codegen.c` reduced from 2106→1767 lines (−339, ~16%); extracted `ir_codegen_before_ret_peephole()`, `ir_codegen_record_scratch()`, `ir_codegen_check_scratch()`, `ir_codegen_track_scratch()` helpers | [07_phase6_consolidate_dispatch.md](07_phase6_consolidate_dispatch.md) |

## Implementation Order and Milestones

### Milestone 1: SValue Elimination (Phase 0) — ✅ COMPLETE
- **Scope:** ~400 lines removed from `ir/codegen.c` and `ir/mat.c`
- **Deliverable:** All codegen uses IROperand. SValue materialization functions deleted.
- **Commit:** `e19755e6 new materialization plan`

### Milestone 2: MachineOperand + Backend Materialization (Phase 1 + Phase 2) — ✅ COMPLETE
- **Scope:** `MachineOperand` type, `machine_op_from_ir()`, and all convertible MOP handlers.
- **Done:** DP (ADD/SUB/CMP/SHL/SHR/SAR/AND/OR/XOR/ADC), ASSIGN (all dests), SETIF, BOOL_OR/AND, LOAD, STORE, LOAD_INDEXED, STORE_INDEXED, LOAD_POSTINC, STORE_POSTINC, IJUMP, FUNCPARAMVAL/VOID, RETURNVALUE (32-bit and 64-bit), MUL/DIV group (MUL/DIV/UDIV/IMOD/UMOD/TEST_ZERO 32-bit), MLA, UMULL, FP single-precision (FADD/FSUB/FMUL/FDIV/FNEG/FCMP/CVT_ITOF/CVT_FTOI/CVT_FTOF), VLA (VLA_ALLOC/VLA_SP_SAVE/VLA_SP_RESTORE), FUNC_CALL (32-bit and 64-bit non-complex dest), SWITCH_TABLE.
- **64-bit pair guards removed:** DP, ASSIGN, BOOL, LOAD, FUNC_CALL — `!irop_needs_pair` guards removed; 64-bit pair sources resolved by `mach_resolve_deref_64` before lo/hi splitting.
- **Intentionally on old path:** JUMP/JUMPIF (no register materialization), LEA (already single-layer), complex types, static chain, double-precision FP.
- **Key constraint resolved (Phase 5b):** `fill_registers_ir` no longer runs unconditionally at dispatch-loop top. `machine_op_from_ir` now fills its `IROperand *op` in-place (`ir_fill_op` helper at old-path `_op` sites). Double-fill is no longer possible.
- **Phase 5p complete:** `pr0_reg`/`pr1_reg` fields removed from `IROperand` (10→9 bytes). Added `irop_phys_r0()`/`irop_phys_r1()` helpers that read physical registers from interval table. `load_to_dest_ir` takes explicit `(int dest_r0, int dest_r1, IROperand src)`. All legacy `_ir` functions + `arm-thumb-asm.c` converted. `irop_init_phys_regs()` deleted.
- **Phase 5q complete:** All legacy `_ir` wrapper functions deleted (~560 lines): `load_to_dest_ir`, `store_ex_ir`, `store_ir`, `th_store_resolve_base_ir`, `irop_phys_r0`/`irop_phys_r1`, `th_store32_imm_or_reg`. `tcc_gen_mach_load_to_reg` rewritten to load directly into dest register (no scratch intermediary), fixing inline asm operand clobber regression (pr49390).
- **Test gate:** `make test -j16` — all tests passing

### Milestone 3: Dry Run Integration (Phase 3) — ✅ COMPLETE
- **Scope:** Dual arrays `dry_insn_scratch[]`/`dry_insn_saves[]`, `try_reassign_scratch_conflict()` with R_FP+static_chain exclusion.
- **Deliverable:** Scratch conflicts resolved by reassigning vregs to callee-saved registers in a fixup pass.
- **Commit:** `c2569883 phase 3: enable dry-run scratch conflict fixup`

### Milestone 4: Cleanup (Phase 4 + Phase 5 + Phase 6) — Phase 4 ✅, Phase 5 ✅, Phase 6 ✅
- **Phase 4 done:** `ir/mat.c`, `ir/operand.c`, `ir/operand.h` deleted (`bc43b639`). `ir/machine_op.c` / `ir/machine_op.h` are the replacement.
- **Phase 5 done:** Dead `TCCStackSlot` fields removed (`0e772abb`). Header deduplication moot (`ir/operand.h` already deleted; only `tccir_operand.h` remains). Lazy fill coordination (Phase 5b) complete — unconditional dispatch-loop fills removed, `machine_op_from_ir` fills in-place, explicit `ir_fill_op` calls added at all old-path `_op` sites.
- **Phase 5c done:** FP double-precision `!irop_needs_pair` guards removed — `tcc_gen_machine_fp_mop` extended with `fp_mop_load_double_arg/do_bl/writeback_result` helpers for all FADD/FSUB/FMUL/FDIV/FNEG/FCMP/CVT_* via `__aeabi_dadd` etc. All `!ir->has_static_chain` guards removed (44 occurrences) — new `MACH_OP_CHAIN_REL` operand kind handles captured variable access via static chain.
- **Phase 5d done:** 14 dead old-path `else` branches removed. `ir/codegen.c` reduced by 440 lines (3149 → 2709).
- **Phase 5e done:** `*_before_ret` peephole converted to MOP path. 6 old-path call sites removed.
- **Phase 5f–5h done:** `machine_op_from_ir` decoupled from `fill_registers_ir`; FUNCCALL func_target → MachineOperand; LOAD spilled-dest support.
- **Phase 5i done:** LOAD/STORE `MACH_OP_NONE` fallback → `tcc_error` (proves old path dead).
- **Phase 5j done:** ~2400 lines dead `_op` backend functions deleted from `arm-thumb-gen.c`.
- **Phase 5k done:** Callsite arg-handling fully on MOP. `fill_arg_from_machine_op` bridge deleted. `is_complex` guards removed from FP/FUNCCALL dispatch. `fill_registers_ir` wrapped in `#ifdef TCC_REGALLOC_DEBUG`. Bug fixes: ARM_R12 base clobber in 64-bit stack arg placement; PARAM_STACK excluded from needs_deref double-indirection.
- **Phase 5l done:** `pr0_spilled`/`pr1_spilled` fields converted to `_reserved0`/`_reserved1` (1-bit each). All 9 read sites in `ir/codegen.c` + `arm-thumb-gen.c` deleted; 3 write sites removed. IROperand remains 10 bytes.
- **Phase 5m done:** `fill_registers_ir` fully deleted (~256 lines). All 6 `#ifdef TCC_REGALLOC_DEBUG` wrappers + the 2 function implementations + 3 declarations removed. `machine_op_from_ir` is now sole materialization path.
- **Phase 5n done:** 10 dead `_op` handler declarations and bodies removed (~700 lines). Includes `tcc_gen_machine_jump_op`, `tcc_gen_machine_cond_jump_op`, `tcc_gen_machine_trap_op`, etc.
- **Phase 5o done:** JUMP, JUMPIF, and TRAP fully converted to `_mop` handlers. Dispatch loop is now 100% MOP — zero `_op` calls remain.
- **Phase 5p done:** `machine_op_from_ir` decoupled from `pr0_reg` — reads interval table directly for physreg. `IROP_VREG_PHYS_VALID`/`IROP_VREG_PHYS_MASK` encoding in `u.imm32` for vreg=-1 operands. `pr0_reg`/`pr1_reg` fields removed from `IROperand` (10→9 bytes).
- **Phase 5q done:** All legacy `_ir` wrapper functions deleted (~560 lines). `tcc_gen_mach_load_to_reg` rewritten for direct-dest loading. Inline asm operand clobber regression (pr49390) fixed.
- **Phase 6 done:** Merged dry-run + real-run dispatch loops into single `for (pass=0; pass<2)` loop. `ir/codegen.c` reduced from 2106→1767 lines (−339, ~16%). See [07_phase6_consolidate_dispatch.md](07_phase6_consolidate_dispatch.md).
- **Current file sizes:** `ir/codegen.c`=1767, `arm-thumb-gen.c`=8055, `ir/machine_op.c`=328, `tccir_operand.h`=560, `tccir_operand.c`=844, `arm-thumb-asm.c`=3539
- **Test gate:** `make test -j16` — 3310 passed, 79 skipped, 582 xfailed, 0 failed

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
