# Phase 6: Consolidate Dispatch Loops

> **Status: ✅ Done** — All sub-steps (6a–6d) completed. `ir/codegen.c` reduced from 2106→1767 lines. All 3310 tests passing.

## Goal

Merge the dry-run and real-run dispatch loops in `ir/codegen.c` into a single parameterised loop, eliminating structural duplication.

## Result (2026-03-06)

`ir/codegen.c` is 1767 lines with a single unified two-pass dispatch loop:

| Section | Lines | Content |
|---------|-------|---------|
| Helper functions | 1–1080 | `tcc_ir_fill_registers` (SValue), `tcc_ir_register_allocation_params`, branch opt, stack layout, inline asm helper, scratch fixup |
| Extracted helpers | 1081–1146 | `ir_codegen_before_ret_peephole()`, `ir_codegen_record_scratch()`, `ir_codegen_check_scratch()`, `ir_codegen_track_scratch()` |
| `tcc_ir_codegen_generate()` | 1148–1275 | Entry, stack_size, arrays, has_incoming_jump |
| **Unified two-pass loop** | 1286–1690 | `for (pass=0; pass<2)` with single `switch (cq->op)`, `is_dry_run` guards for pass-specific logic |
| Cleanup | 1690–1767 | Gap-fill, backpatch jumps, epilogue, free arrays |

Both passes call the same `_mop` backend handlers via `machine_op_from_ir()`. No `_op` functions remain.

## Completed Implementation

### Extracted Helper Functions (lines 1081–1146)

| Helper | Lines | Purpose |
|--------|-------|---------|
| `ir_codegen_before_ret_peephole()` | ~35 | Checks LOAD/LOAD_INDEXED/ASSIGN before RETURNVALUE, patches allocation to R0 |
| `ir_codegen_record_scratch()` | ~4 | Records per-instruction scratch counts during dry-run |
| `ir_codegen_check_scratch()` | ~11 | Verifies real-run scratch counts match dry-run (under `TCC_LS_DEBUG`) |
| `ir_codegen_track_scratch()` | ~7 | Unified wrapper: dispatches to record (dry) or check (real) |

### Pass-Specific Guards (`is_dry_run` / `!is_dry_run`)

| Op/Section | Dry-run (`pass == 0`) | Real-run (`pass == 1`) |
|---|---|---|
| Loop preamble | `ir_to_code_mapping[i] = ind`, scratch flags reset, debug op tracking | Same + `orig_ir_to_code_mapping` update + `tcc_debug_line_num()` |
| Scratch tracking | `ir_codegen_record_scratch()` via `ir_codegen_track_scratch()` | `ir_codegen_check_scratch()` via `ir_codegen_track_scratch()` |
| SWITCH_TABLE | Arithmetic: `ind += 14 + num_entries*4` | `tcc_gen_machine_switch_table_mop()` handler |
| RETURNVOID | No-op (no epilogue jump) | `return_jump_addrs[n++] = ind; tcc_gen_machine_jump_mop(...)` |
| JUMP/JUMPIF | Handler call only | Handler + `ir_to_code_mapping[i]` encoding correction |
| INLINE_ASM | Skipped (assembler has side effects beyond `ot()`) | `tcc_ir_codegen_inline_asm_ir()` + `spill_cache_clear` |
| default | Silent break | Fatal error with cleanup |
| Pass init | `dry_run_init`, `branch_opt_init`, save state | Prologue emission, `tcc_debug_prolog_epilog` |
| Pass end | `dry_run_end`, branch analyze, LR check, scratch fixup, state restore | (loop simply ends) |

### Shared Logic (executed in both passes)

- Operand extraction: `tcc_ir_op_get_src1/src2/dest(ir, cq)`
- MachineOperand conversion: `machine_op_from_ir(ir, &src_ir)`
- `before_ret` peephole for LOAD/LOAD_INDEXED/ASSIGN
- `mop_fixup_subcomponent()` for LOAD/STORE
- All `_mop` handler calls (DP, MUL, LOAD, STORE, ASSIGN, FP, FUNCCALL, etc.)
- `tcc_gen_machine_end_instruction()` cleanup
- `tcc_ir_spill_cache_clear()` after branches, calls, switch tables

## Results

| Metric | Before | After |
|--------|--------|-------|
| `ir/codegen.c` lines | 2106 | 1767 |
| Dispatch switch statements | 2 | 1 |
| `before_ret` peephole copies | 6 | 1 (helper function) |
| Scratch tracking inline code | ~240 lines | ~25 lines (4 helpers) |
| Lines to add for new IR op | 2 cases | 1 case |
| Line reduction | — | −339 lines (~16%) |

## Implementation Notes

The actual implementation took a slightly different approach from the original plan:

- **Steps 6a–6c were done first** (helper extraction, preamble normalization) as preparatory refactors.
- **Step 6d merged the loops directly** rather than first extracting into a separate `ir_codegen_dispatch_one()` function. The switch body stays inline in the main function — the dispatch context struct was unnecessary since all state is already in local variables. This kept the code simpler and avoided function pointer / struct indirection overhead.
- **RETURNVALUE→RETURNVOID fallthrough was preserved** in the merged version with an `if (!is_dry_run)` guard in RETURNVOID, rather than using an explicit flag.
- **`tcc_ir_spill_cache_clear()`** calls were normalized to run in both passes (safe no-op during dry-run since cache is cleared at start).

## Test Verification

All tests passing after each sub-step and after the final merge:
```
3310 passed, 79 skipped, 582 xfailed, 0 failed
```

